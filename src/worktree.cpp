// Worktree sessions: a session in a git worktree of its own
//
// The worktrees are made, put back and removed by git, run by a worker
// thread of their own, apart from the one that answers for the Changes
// pane (git.cpp), since a checkout, with its hooks, can take minutes.
// The worker runs one job at a time, in the order asked, and posts each
// answer to the engine's mailbox (MAIL_WORKTREE). A job asked to stop,
// by a timeout or by the engine stopping, asks git to stop first, so
// that git cleans up after itself, and the engine waits for it as it
// quits. Jobs left waiting then are asked for again by whatever needed
// them, as the session's record says what must exist.
//
// make: the worktree at PATH, on BRANCH, from that branch if it exists
// (a session's own, or one a job stopped before left), else a new one
// from TIP, else OID. A worktree already registered there whose folder
// is gone is ours to put back (add -f); BRANCH checked out elsewhere is
// an error. The worktrees' folder, MAIN/.prospero/worktrees, gets a
// .gitignore of *, if it has none, so that git status, ripgrep and the
// search tools leave them out.
//
// remove: the worktree, if git calls it clean (no --force: ignored
// files, as builds are, go with it), then BRANCH, if it is merged into
// BASE (for a detached base: still at OID) and checked out nowhere.
// What stays, and why, is in the answer.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    WT_ADDWAIT    = 600*SEC,   // longest git worktree add: the checkout, hooks
    WT_REMOVEWAIT = 120*SEC,   // longest git worktree remove
    WT_GITWAIT    = 30*SEC,    // longest of the rest: list, rev-parse, branch
    WT_PENDING    = 64,        // jobs waiting, at most
    WT_REQBOX     = 256 << 10, // the worker's mailbox of jobs
    WT_PROCBOX    = 256 << 10, // and of its children's records
    WT_REGION     = 16 << 20,  // a job's arenas, each
};

enum : i32 {
    WT_MAKE,
    WT_REMOVE,
};

static Str wtkinds[] = {"make", "remove"};

// A job, as the worker takes it.
struct WtAsk {
    i64        id;
    i32        kind;
    Str        sid;      // the session it is for, if any
    Str        main;     // the main checkout, where git runs
    Str        path;     // the worktree
    Str        branch;
    Str        base;     // the branch it was cut from, null if detached
    Str        oid;      // the commit it was cut from
    Str        tip;      // make: the branch's commit when it was removed
    Str        git;      // [tools] git
    Str        home;
    Str        errpath;  // for git's standard error
    Slice<Str> env;
    b32        win;
};

static WtAsk wtparseask(Arena *a, Str record)
{
    WtAsk   q = {};
    JsonDoc d = jsonparse(a, clone(a, record));
    q.id      = jsonint(jsonget(d.root, "id"), 0);
    q.kind    = jsonstr(jsonget(d.root, "kind"))=="remove" ? WT_REMOVE : WT_MAKE;
    q.sid     = jsonstr(jsonget(d.root, "sid"));
    q.main    = jsonstr(jsonget(d.root, "main"));
    q.path    = jsonstr(jsonget(d.root, "path"));
    q.branch  = jsonstr(jsonget(d.root, "branch"));
    q.base    = jsonstr(jsonget(d.root, "base"));
    q.oid     = jsonstr(jsonget(d.root, "oid"));
    q.tip     = jsonstr(jsonget(d.root, "tip"));
    q.git     = jsonstr(jsonget(d.root, "git"));
    q.home    = jsonstr(jsonget(d.root, "home"));
    q.errpath = jsonstr(jsonget(d.root, "errpath"));
    q.win     = jsonflag(jsonget(d.root, "win"), 0);
    for (Json *e = jsonget(d.root, "env")->child; e; e = e->next) {
        q.env = push(a, q.env, jsonstr(e));
    }
    return q;
}

// What a job did, for the engine.
struct WtDone {
    b32 ok;
    Str err;     // why not
    Str did;     // remove: removed, kept, or gone (no worktree there)
    Str why;     // kept: dirty, submodules, locked, in use, branch, or error
    Str tip;     // remove: the branch's commit then
    Str branch;  // remove: deleted, unmerged, or null if none
};

static Str wtdonejson(Arena *a, WtAsk *q, WtDone *d)
{
    Buf     b(a, 1024 + q->path.len + d->err.len);
    JsonOut w = {&b};
    putobject(&w);
    putintkey(&w, "id", q->id);
    putstrkey(&w, "kind", wtkinds[q->kind]);
    putstrkey(&w, "sid", q->sid);
    putstrkey(&w, "path", q->path);
    putintkey(&w, "ok", d->ok);
    if (d->err.len) putstrkey(&w, "err", d->err);
    if (d->did.len) putstrkey(&w, "did", d->did);
    if (d->why.len) putstrkey(&w, "why", d->why);
    if (d->tip.len) putstrkey(&w, "tip", d->tip);
    if (d->branch.len) putstrkey(&w, "branch", d->branch);
    putend(&w);
    return finish(&b);
}

// The same file or directory, as two spellings name it: alike once
// normalized, else once real (for one that is gone, its parent's).
static b32 wtsame(Os *os, Arena scratch, Str x, Str y, b32 win)
{
    x = normpath(&scratch, x, ".", win);
    y = normpath(&scratch, y, ".", win);
    if (win ? equalfold(x, y) : x==y) {
        return 1;
    }
    Str rx = os_realpath(os, &scratch, x);
    Str ry = os_realpath(os, &scratch, y);
    if (!rx.len) {
        Str up = os_realpath(os, &scratch, parentdir(x));
        rx = up.len ? join(&scratch, up, basename(x)) : Str{};
    }
    if (!ry.len) {
        Str up = os_realpath(os, &scratch, parentdir(y));
        ry = up.len ? join(&scratch, up, basename(y)) : Str{};
    }
    rx = rx.len ? normpath(&scratch, rx, ".", win) : rx;
    ry = ry.len ? normpath(&scratch, ry, ".", win) : ry;
    return rx.len && (win ? equalfold(rx, ry) : rx==ry);
}

// A run of git in the main checkout, args after the program, with
// core.longpaths on Windows, where a worktree's longer paths could
// otherwise fail its checkout.
static GitRun wtgit(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog,
                    Slice<Str> args, i64 wait)
{
    Slice<Str> argv = {};
    argv = push(&scratch, argv, prog);
    if (q->win) {
        argv = push(&scratch, argv, Str("-c"));
        argv = push(&scratch, argv, Str("core.longpaths=true"));
    }
    for (iz i = 0; i < args.len; i++) {
        argv = push(&scratch, argv, args[i]);
    }
    return gitrun(w, a, scratch, argv, q->main, q->env, q->errpath, wait, 1);
}

static Slice<Str> wtargs(Arena *a, Slice<Str> r, Str s)
{
    return push(a, r, s);
}

// Why a run failed, null if it did not, in the words of a job.
static Str wtfailed(Arena *a, GitRun *r, Str prog, Str what, i64 wait)
{
    if (r->quit && r->code) {
        return "Prospero stopped first";
    } else if (r->timeout) {
        Buf b(a, 64);
        print(&b, "git ");
        print(&b, what);
        print(&b, " took over ");
        print(&b, wait/SEC);
        print(&b, " s");
        return finish(&b);
    }
    return giterror(a, r, prog, what, wait);
}

// The commit a branch is at, by git, which reads any kind of refs, or
// null if there is no such branch.
static Str wtbranchoid(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog,
                       Str branch)
{
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "rev-parse");
    args = wtargs(&scratch, args, "--verify");
    args = wtargs(&scratch, args, "--quiet");
    args = wtargs(&scratch, args, concat(&scratch, concat(&scratch, "refs/heads/", branch),
                                         "^{commit}"));
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
    Str    o = gitclean(r.out);
    return !r.code && gitoid(o) ? o : Str{};
}

static Slice<GitTree> wtlist(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog,
                             Str *err)
{
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "worktree");
    args = wtargs(&scratch, args, "list");
    args = wtargs(&scratch, args, "--porcelain");
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
    *err = wtfailed(a, &r, prog, "worktree list", WT_GITWAIT);
    return err->len ? Slice<GitTree>{} : gitparsetrees(a, r.out);
}

// The worktrees' folder's .gitignore, unless it has one.
static void wtignore(Os *os, Arena scratch, Str path)
{
    Str dir = parentdir(path);
    Str gi  = join(&scratch, dir, ".gitignore");
    os_mkdirs(os, scratch, dir);
    if (os_stat(os, scratch, gi).type == STAT_MISSING) {
        os_write(os, scratch, gi, "*\n");
    }
}

static WtDone wtmake(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog)
{
    WtDone d   = {};
    Os    *os  = w->os;
    Str    ref = concat(&scratch, "refs/heads/", q->branch);
    wtignore(os, scratch, q->path);

    Slice<GitTree> trees = wtlist(w, a, scratch, q, prog, &d.err);
    if (d.err.len) {
        d.err = clone(a, d.err);
        return d;
    }
    GitTree *ours  = 0;
    Str      other = {};  // where else the branch is checked out
    for (iz i = 0; i < trees.len; i++) {
        GitTree *t = &trees[i];
        if (wtsame(os, scratch, t->path, q->path, q->win)) {
            ours = t;
        } else if (t->branch==ref && !t->prunable) {
            other = t->path;
        }
    }
    Str gitfile = join(&scratch, q->path, ".git");
    if (ours && !ours->prunable && ours->branch==ref &&
            os_stat(os, scratch, gitfile).type!=STAT_MISSING) {
        d.ok = 1;  // already there
        return d;
    } else if (ours && !ours->prunable) {
        d.err = concat(a, q->path, " is a worktree on another branch");
        return d;
    } else if (other.len) {
        Buf b(a, 128 + q->branch.len + other.len);
        print(&b, q->branch);
        print(&b, " is checked out at ");
        print(&b, other);
        d.err = finish(&b);
        return d;
    } else if (ours && ours->locked) {
        d.err = concat(a, q->path, " is registered and locked: git worktree unlock it");
        return d;
    }

    Str        have = wtbranchoid(w, a, scratch, q, prog, q->branch);
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "worktree");
    args = wtargs(&scratch, args, "add");
    if (ours) {
        args = wtargs(&scratch, args, "-f");  // ours, its folder gone
    }
    if (!have.len) {
        args = wtargs(&scratch, args, "-b");
        args = wtargs(&scratch, args, q->branch);
    }
    args = wtargs(&scratch, args, "--");
    args = wtargs(&scratch, args, q->path);
    args = wtargs(&scratch, args, have.len ? q->branch : q->tip.len ? q->tip : q->oid);
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_ADDWAIT);
    d.err = wtfailed(a, &r, prog, "worktree add", WT_ADDWAIT);
    d.ok  = !d.err.len;
    return d;
}

// Why git would not remove a worktree, in a word.
static Str wtwhy(Str err)
{
    Str low = err;
    if (find(low, "modified or untracked")>=0 || find(low, "contains modified")>=0) {
        return "dirty";
    } else if (find(low, "submodule") >= 0) {
        return "submodules";
    } else if (find(low, "locked") >= 0) {
        return "locked";
    } else if (find(low, "ermission denied")>=0 || find(low, "not empty")>=0 ||
               find(low, "being used")>=0 || find(low, "Invalid argument")>=0) {
        return "in use";
    }
    return "error";
}

static WtDone wtremove(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog)
{
    WtDone d   = {};
    Os    *os  = w->os;
    Str    ref = concat(&scratch, "refs/heads/", q->branch);

    Slice<GitTree> trees = wtlist(w, a, scratch, q, prog, &d.err);
    if (d.err.len) {
        d.err = clone(a, d.err);
        return d;
    }
    GitTree *ours = 0;
    for (iz i = 0; i < trees.len; i++) {
        ours = wtsame(os, scratch, trees[i].path, q->path, q->win) ? &trees[i] : ours;
    }
    d.ok  = 1;
    d.did = "gone";
    if (ours && !ours->prunable) {
        if (ours->branch != ref) {
            d.did = "kept";
            d.why = "branch";
            d.err = concat(a, "it is no longer on ", q->branch);
            return d;
        }
        Slice<Str> args = {};
        args = wtargs(&scratch, args, "worktree");
        args = wtargs(&scratch, args, "remove");
        args = wtargs(&scratch, args, "--");
        args = wtargs(&scratch, args, q->path);
        GitRun r   = wtgit(w, a, scratch, q, prog, args, WT_REMOVEWAIT);
        Str    err = wtfailed(&scratch, &r, prog, "worktree remove", WT_REMOVEWAIT);
        if (err.len) {
            d.did = "kept";
            d.why = wtwhy(err);
            d.err = clone(a, err);
            return d;
        }
        d.did = "removed";
    } else if (!ours && os_stat(os, scratch, q->path).type!=STAT_MISSING) {
        d.did = "kept";
        d.why = "error";
        d.err = concat(a, q->path, " is not a worktree of the repository");
        return d;
    }

    // The branch, once checked out nowhere, if merged
    Str tip = wtbranchoid(w, a, scratch, q, prog, q->branch);
    for (iz i = 0; tip.len && i<trees.len; i++) {
        if (&trees[i]!=ours && trees[i].branch==ref && !trees[i].prunable) {
            d.tip    = clone(a, tip);
            d.branch = "elsewhere";
            return d;
        }
    }
    if (!tip.len) {
        return d;
    }
    d.tip = clone(a, tip);
    b32 merged = 0;
    if (q->base.len) {
        Slice<Str> args = {};
        args = wtargs(&scratch, args, "merge-base");
        args = wtargs(&scratch, args, "--is-ancestor");
        args = wtargs(&scratch, args, tip);
        args = wtargs(&scratch, args, concat(&scratch, "refs/heads/", q->base));
        GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
        merged = r.spawned && !r.timeout && !r.quit && r.code==0;
    } else {
        merged = tip == q->oid;
    }
    if (!merged) {
        d.branch = "unmerged";
        return d;
    }
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "branch");
    args = wtargs(&scratch, args, "-D");
    args = wtargs(&scratch, args, "--");
    args = wtargs(&scratch, args, q->branch);
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
    d.branch = wtfailed(&scratch, &r, prog, "branch -D", WT_GITWAIT).len
               ? Str("unmerged") : Str("deleted");
    return d;
}

static Str wtjob(GitWork *w, Arena *a, Arena scratch, WtAsk *q)
{
    WtDone  d    = {};
    Str     path = envpath(q->env, HOST_WINDOWS);
    GitProg prog = gitprog(w->os, a, scratch, q->git, path, q->home,
                           HOST_WINDOWS, GIT_MACOS);
    if (!prog.path.len || prog.shim) {
        d.err = gitmissing(a, q->git, prog.shim);
    } else if (q->kind == WT_REMOVE) {
        d = wtremove(w, a, scratch, q, prog.path);
    } else {
        d = wtmake(w, a, scratch, q, prog.path);
    }
    return wtdonejson(a, q, &d);
}

// The worker: idle until a job comes, then each in turn. Those left once
// the engine stops are dropped.
static void wtthread(void *arg)
{
    GitWork *w = (GitWork *)arg;
    for (;;) {
        mailwait(w->req, -1);
        if (gitquitting(w)) {
            return;
        }
        Arena        perm = newarena(w->perm);
        Slice<WtAsk> asks = {};
        Mail         m    = mailtake(w->req);
        for (Record r; (r = mailnext(&m)).ok;) {
            asks = push(&perm, asks, wtparseask(&perm, r.data));
        }
        for (iz i = 0; i < asks.len && !gitquitting(w); i++) {
            __atomic_store_n(&w->running, 1, __ATOMIC_RELEASE);
            Arena a      = perm;
            Str   answer = wtjob(w, &a, newarena(w->scratch), &asks[i]);
            mailpost(w->out, MAIL_WORKTREE, 0, answer, 1);
            __atomic_store_n(&w->running, 0, __ATOMIC_RELEASE);
        }
    }
}


// Names and places
//
// A worktree's name is its folder's, MAIN/.prospero/worktrees/NAME, and
// its branch's after [worktree] branch_prefix: a word or two of [a-z0-9]
// joined by '-', which neither the shell nor Windows minds. MAIN is the
// main checkout of the repository holding the folder chosen, also when
// that is itself a worktree. Nothing of a worktree's place comes from a
// client but its name, so no path escapes MAIN.

enum : iz {
    WT_NAMEMAX = 24,
    WT_TRIES   = 16,  // random names tried for a free one
};

static Str const wtadjs[] = {
    "amber", "bold", "brisk", "calm", "clever", "crisp", "dapper", "deft",
    "eager", "fair", "fleet", "gentle", "glad", "golden", "hardy", "jolly",
    "keen", "kind", "lively", "lucky", "merry", "mild", "nimble", "noble",
    "plucky", "proud", "quick", "quiet", "rapid", "ready", "robust", "rosy",
    "sage", "shiny", "sleek", "snappy", "spry", "steady", "sturdy", "sunny",
    "swift", "tidy", "trusty", "vivid", "warm", "wise", "witty", "zesty",
};

static Str const wtnouns[] = {
    "badger", "beaver", "bison", "crane", "falcon", "ferret", "finch", "fox",
    "gecko", "heron", "ibex", "jay", "kestrel", "koala", "lark", "lemur",
    "lynx", "marten", "mink", "moose", "newt", "ocelot", "orca", "osprey",
    "otter", "owl", "panda", "pika", "plover", "puffin", "quail", "raven",
    "robin", "salmon", "seal", "shrike", "sparrow", "stoat", "swift", "tapir",
    "tern", "thrush", "trout", "vole", "walrus", "wombat", "wren", "yak",
};

// Whether a worktree may be named so: 1 to WT_NAMEMAX of [a-z0-9-], not
// beginning or ending with '-', nor with two in a row, and no device
// name of Windows.
static b32 wtname(Str s)
{
    if (!s.len || s.len>WT_NAMEMAX || s[0]=='-' || s[s.len-1]=='-') {
        return 0;
    }
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ||
                (c=='-' && s[i-1]=='-')) {
            return 0;
        }
    }
    static Str const devices[] = {"con", "prn", "aux", "nul"};
    for (iz i = 0; i < countof(devices); i++) {
        if (s == devices[i]) {
            return 0;
        }
    }
    b32 port = s.len==4 && (startswith(s, "com") || startswith(s, "lpt")) && digit(s[3]);
    return !port;
}

// Why a name will not do, for a client.
static Str wtnamewhy(Str s)
{
    if (!s.len) {
        return "the worktree needs a name";
    } else if (s.len > WT_NAMEMAX) {
        return "a worktree's name is at most 24 characters";
    }
    return "a worktree's name is lowercase letters, digits and single "
           "hyphens between them";
}

// A random name, as brisk-otter.
static Str wtrandom(Arena *a, u64 r)
{
    Str adj  = wtadjs[(r & 0xffffffff) % countof(wtadjs)];
    Str noun = wtnouns[(r >> 32) % countof(wtnouns)];
    return concat(a, concat(a, adj, "-"), noun);
}

// A path in the host's own form: on Windows with '\'.
static Str wtnative(Arena *a, Str path, b32 win)
{
    if (!win) {
        return path;
    }
    Str r = clone(a, path);
    for (iz i = 0; i < r.len; i++) {
        r.data[i] = r[i]=='/' ? '\\' : r[i];
    }
    return r;
}

// The folder of a worktree, by its name, in MAIN.
static Str wtpath(Arena *a, Str main, Str name, b32 win)
{
    Str dir = concat(a, main, win ? Str("\\.prospero\\worktrees\\")
                                  : Str("/.prospero/worktrees/"));
    return concat(a, dir, name);
}

// Where a worktree session can start from a folder: the repository's
// main checkout, the folder's place below its own checkout's root, and
// the commit and branch the new branch would be cut from. Else why not.
struct WtPlace {
    Str err;
    Str main;    // the main checkout, in the host's form
    Str sub;     // the folder below its checkout's root, '/' separated
    Str common;  // the git directory the worktrees share
    Str base;    // the branch HEAD is on, null if detached
    Str oid;     // HEAD's commit
    GitHead h;
};

static WtPlace wtplace(Os *os, Arena *a, Str cwd, b32 win)
{
    WtPlace p = {};
    p.h = githead(os, a, cwd, {}, win);
    GitHead *h = &p.h;
    if (!h->repo) {
        p.err = "the folder is not in a git repository";
        return p;
    } else if (h->reftable) {
        p.err = "the repository keeps its refs in a reftable, which "
                "Prospero cannot read yet";
        return p;
    } else if (h->op.len) {
        p.err = concat(a, concat(a, "a ", h->op), " is under way in the folder");
        return p;
    }

    // The main checkout: the root itself when its .git is the git
    // directory, else what its worktree's .git leads to
    Str root = normpath(a, h->root, ".", win);
    Str gd   = normpath(a, h->gitdir, ".", win);
    Str own  = normpath(a, root, ".git", win);
    Str main = (win ? equalfold(gd, own) : gd==own) ? root : memmain(os, a, root, win);
    if (!main.len) {
        p.err = "the folder is in a submodule, or a worktree of a bare "
                "repository: open its own repository instead";
        return p;
    }
    p.main   = wtnative(a, main, win);
    p.common = gitcommon(os, a, h->gitdir, win);
    Str dir  = normpath(a, cwd, ".", win);
    p.sub    = dir.len>root.len ? cuthead(dir, root.len + !endswith(root, "/")) : Str{};

    if (h->detached) {
        p.oid = h->oid;
    } else if (startswith(h->branch, "refs/")) {
        p.oid = gitrefoid(os, a, p.common, h->branch);  // HEAD on another ref
    } else {
        p.base = h->branch;
        p.oid  = gitrefoid(os, a, p.common, concat(a, "refs/heads/", h->branch));
    }
    if (!p.oid.len) {
        p.err = p.base.len ? concat(a, concat(a, "the branch ", p.base), " has no commits yet")
                           : Str("HEAD names no commit");
    }
    return p;
}

// The configured branch prefix, as a valid one.
static Str wtprefix(Engine *e)
{
    Str v = trim(cfgget(e->cfg, "worktree", "", "branch_prefix", 0));
    return v.len && cfgrefprefix(v) ? v : Str("prospero/");
}

// Whether a session, archived or not, records a worktree of MAIN by
// this name, which then stays reserved for it.
static b32 wtrecorded(Engine *e, Arena scratch, Str main, Str name, b32 win)
{
    Str dir = join(&scratch, e->statedir, "sessions");
    for (Dirent *d = os_list(e->os, &scratch, dir); d; d = d->next) {
        if (!d->isdir) continue;
        Arena t    = scratch;
        Str   path = join(&t, join(&t, dir, d->name), "events.jsonl");
        Read  rd   = os_readat(e->os, &t, path, 0, 4096);
        EvRec rec  = evparse(&t, cut(rd.data, '\n').head);
        Json *wt   = jsonget(rec.root, "worktree");
        Str   m    = jsonstr(jsonget(wt, "main"));
        if (rec.ok && rec.ev==EV_SESSION && jsonstr(jsonget(wt, "name"))==name &&
                (win ? equalfold(m, main) : m==main)) {
            return 1;
        }
    }
    return 0;
}

// Whether a worktree cannot be made by this name: its folder exists,
// its branch does, or a session has it.
enum : i32 {WT_FREE, WT_HASDIR, WT_HASBRANCH, WT_HASSESSION};

static i32 wttaken(Engine *e, Arena scratch, WtPlace *p, Str name)
{
    b32 win = HOST_WINDOWS;
    Str ref = concat(&scratch, concat(&scratch, "refs/heads/", wtprefix(e)), name);
    Str dir = wtpath(&scratch, p->main, name, win);
    if (os_stat(e->os, scratch, dir).type != STAT_MISSING) {
        return WT_HASDIR;
    } else if (gitrefoid(e->os, &scratch, p->common, ref).len) {
        return WT_HASBRANCH;
    } else if (wtrecorded(e, scratch, p->main, name, win)) {
        return WT_HASSESSION;
    }
    return WT_FREE;
}

// Why a name is taken, for a client.
static Str wttakenwhy(Engine *e, Arena *a, i32 taken, Str name)
{
    switch (taken) {
    case WT_HASDIR:
        return concat(a, name, " is taken: its folder exists");
    case WT_HASBRANCH:
        return concat(a, concat(a, concat(a, "the branch ", wtprefix(e)), name), " exists");
    case WT_HASSESSION:
        return concat(a, name, " is taken by another session");
    }
    return {};
}

// A free name, random, else null.
static Str wtsuggest(Engine *e, Arena *a, Arena scratch, WtPlace *p)
{
    for (i32 i = 0; i < WT_TRIES; i++) {
        Str name = wtrandom(a, os_random(e->os));
        if (wttaken(e, scratch, p, name) == WT_FREE) {
            return name;
        }
    }
    return {};
}

// The worktree object of a probe's reply: whether one can start there,
// else why not, and if so, a free name and what it would be cut from.
static Str wtprobejson(Engine *e, Arena *a, Arena scratch, WtPlace *p)
{
    Str name = p->err.len ? Str{} : wtsuggest(e, a, scratch, p);
    Buf b(a, 1024 + p->main.len + p->err.len);
    JsonOut w = {&b};
    putobject(&w);
    if (p->err.len) {
        putintkey(&w, "ok", 0);
        putstrkey(&w, "why", p->err);
    } else {
        putintkey(&w, "ok", 1);
        putstrkey(&w, "main", p->main);
        putstrkey(&w, "prefix", wtprefix(e));
        if (name.len) putstrkey(&w, "name", name);
        if (p->base.len) putstrkey(&w, "base", p->base);
        putstrkey(&w, "oid", p->oid);
    }
    putend(&w);
    return finish(&b);
}

// git {cwd, status?}: a folder probed for a new session, its head, its
// changes with status, and whether a worktree session can start there.
static void wtprobe(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str      cwd = jsonstr(jsonget(j, "cwd"));
    HomePath hp  = tildepath(&scratch, cwd, e->home, HOST_WINDOWS);
    Str      dir = hp.err.len ? Str{} : os_realpath(e->os, &scratch, hp.path);
    if (!dir.len || os_stat(e->os, scratch, dir).type!=STAT_DIR) {
        reply(e, scratch, id, 0, "cwd must name an existing directory", {});
        return;
    }
    Arena   keep = carve(&scratch, 1<<16);
    WtPlace p    = wtplace(e->os, &keep, dir, HOST_WINDOWS);
    Str     wt   = wtprobejson(e, &keep, scratch, &p);
    Str     head = githeadjson(&keep, &p.h);
    Str     status = {};
    if (p.h.repo && jsonflag(jsonget(j, "status"), 0)) {
        Str err = gitqueue(e, scratch, id, {}, dir, {}, p.h.root, wt);
        if (!err.len) {
            return;  // gitdone replies
        }
        status = gitfailjson(&keep, err);
    }
    Buf   b(&scratch, 256 + head.len + status.len + wt.len);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putkey(&w.json, "head");
    putraw(&w.json, head);
    if (status.len) {
        putkey(&w.json, "status");
        putraw(&w.json, status);
    }
    putkey(&w.json, "worktree");
    putraw(&w.json, wt);
    emit(e, evend(&w));
}


// The engine's side

static GitWork *wtwork(Engine *e)
{
    GitWork *w = e->wt;
    if (!w) {
        w = e->wt = alloc<GitWork>(&e->perm);
        w->os  = e->os;
        w->out = e->in;
    }
    w->req     = w->req     ? w->req     : newmailbox(&e->perm, WT_REQBOX, 0, 0);
    w->procs   = w->procs   ? w->procs   : newmailbox(&e->perm, WT_PROCBOX, 0, 0);
    w->perm    = w->perm    ? w->perm    : os_reserve(e->os, WT_REGION);
    w->scratch = w->scratch ? w->scratch : os_reserve(e->os, WT_REGION);
    if (!w->started && w->req && w->procs && w->perm && w->scratch) {
        w->started = os_thread(e->os, wtthread, w);
    }
    if (!w->started) {
        if (w->perm) os_release(e->os, w->perm);
        if (w->scratch) os_release(e->os, w->scratch);
        w->perm = w->scratch = 0;
    }
    return w->started ? w : 0;
}

// The job under way or waiting for a worktree, if any.
static WtJob *wtjobfor(Engine *e, Str path)
{
    for (iz i = 0; i < e->wtjobs.len; i++) {
        if (e->wtjobs[i].path == path) {
            return &e->wtjobs[i];
        }
    }
    return 0;
}

// What a job needs: a session's worktree, its branch and where it was
// cut, and for make, the commit to make it again from.
struct WtSpec {
    Str sid;
    Str main;
    Str path;
    Str branch;
    Str base;
    Str oid;
    Str tip;
};

// Queue a job for the worker: null if queued, else why not.
static Str wtqueue(Engine *e, Arena scratch, i32 kind, WtSpec *sp)
{
    if (wtjobfor(e, sp->path)) {
        return {};  // one is under way, which its answer settles
    }
    GitWork *w = wtwork(e);
    if (!w) {
        return "cannot start a thread for git";
    } else if (e->wtjobs.len >= WT_PENDING) {
        return "git is busy: try again";
    }
    Str tmp = join(&scratch, e->statedir, "tmp");
    os_mkdirs(e->os, scratch, tmp);
    Buf name(&scratch, 32);
    print(&name, "git-");
    print(&name, (i64)os_pid(e->os));
    print(&name, "-worktree.err");
    Str        errpath = join(&scratch, tmp, finish(&name));
    Slice<Str> env     = engineenv(e, &scratch, HOST_WINDOWS);

    i64     id = ++e->wtserial;
    Buf     b(&scratch, 4096);
    JsonOut o = {&b};
    putobject(&o);
    putintkey(&o, "id", id);
    putstrkey(&o, "kind", wtkinds[kind]);
    putstrkey(&o, "sid", sp->sid);
    putstrkey(&o, "main", sp->main);
    putstrkey(&o, "path", sp->path);
    putstrkey(&o, "branch", sp->branch);
    putstrkey(&o, "base", sp->base);
    putstrkey(&o, "oid", sp->oid);
    putstrkey(&o, "tip", sp->tip);
    putstrkey(&o, "git", trim(cfgget(e->cfg, "tools", "", "git", 0)));
    putstrkey(&o, "home", e->home);
    putstrkey(&o, "errpath", errpath);
    putintkey(&o, "win", HOST_WINDOWS);
    putkey(&o, "env");
    putarray(&o);
    for (iz i = 0; i < env.len; i++) {
        putstr(&o, env[i]);
    }
    putend(&o);
    putend(&o);
    if (!mailpost(w->req, 0, 0, finish(&b), 0)) {
        return "git is busy: try again";
    }
    WtJob job = {};
    job.id    = id;
    job.kind  = kind;
    job.path  = clone(&e->perm, sp->path);
    job.sid   = clone(&e->perm, sp->sid);
    e->wtjobs = push(&e->perm, e->wtjobs, job);
    return {};
}

// A worktree's news, for every frontend: made, failed, removed or kept,
// with the session it is for, if any.
static void wtsay(Engine *e, Arena scratch, Str sid, Str path, Str did, Str why,
                  Str err, Str branch)
{
    Buf   b(&scratch, 512 + path.len + err.len);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_WORKTREE);
    if (sid.len) putstrkey(&w.json, "session", sid);
    putstrkey(&w.json, "path", path);
    putstrkey(&w.json, "did", did);
    if (why.len) putstrkey(&w.json, "why", why);
    if (err.len) putstrkey(&w.json, "err", err);
    if (branch.len) putstrkey(&w.json, "branch", branch);
    emit(e, evend(&w));
}

// A worktree made, or not.
static void wtmade(Engine *e, Arena scratch, Str sid, Str path, b32 ok, Str err)
{
    wtsay(e, scratch, sid, path, ok ? Str("made") : Str("failed"), {}, err, {});
}

// A worktree removed, or kept, and its branch.
static void wtremoved(Engine *e, Arena scratch, Json *answer)
{
    b32 ok  = jsonint(jsonget(answer, "ok"), 0) == 1;
    Str did = ok ? jsonstr(jsonget(answer, "did")) : Str("kept");
    Str why = ok ? jsonstr(jsonget(answer, "why")) : Str("error");
    wtsay(e, scratch, jsonstr(jsonget(answer, "sid")), jsonstr(jsonget(answer, "path")),
          did, why, jsonstr(jsonget(answer, "err")), jsonstr(jsonget(answer, "branch")));
}

// The worker's answer to a job.
static void wtdone(Engine *e, Arena scratch, Str answer)
{
    JsonDoc d    = jsonparse(&scratch, answer);
    i64     id   = jsonint(jsonget(d.root, "id"), 0);
    Str     kind = jsonstr(jsonget(d.root, "kind"));
    for (iz i = 0; i < e->wtjobs.len; i++) {
        if (e->wtjobs[i].id == id) {
            e->wtjobs[i] = e->wtjobs[e->wtjobs.len-1];
            e->wtjobs.len--;
            break;
        }
    }
    if (kind == "remove") {
        wtremoved(e, scratch, d.root);
    } else {
        b32 ok = jsonint(jsonget(d.root, "ok"), 0) == 1;
        wtmade(e, scratch, jsonstr(jsonget(d.root, "sid")),
               jsonstr(jsonget(d.root, "path")), ok, jsonstr(jsonget(d.root, "err")));
    }
}

// Whether a job runs: the engine, stopping, waits for it.
static b32 wtbusy(Engine *e)
{
    GitWork *w = e->wt;
    return w && w->started && __atomic_load_n(&w->running, __ATOMIC_ACQUIRE);
}

// The engine stops: the job under way is asked to stop, and the rest
// are dropped.
static void wtquit(Engine *e)
{
    GitWork *w = e->wt;
    if (w && w->started) {
        __atomic_store_n(&w->quit, 1, __ATOMIC_RELEASE);
        mailpoke(w->req);
    }
}

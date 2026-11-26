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
// In merged mode (a subagent's worktree), a removal waits until BRANCH
// is merged, answering "waiting" until then, and touching nothing.
//
// check: what a subagent left in its worktree as it reports, reading
// only: BRANCH's commit, its commits since OID, the files not
// committed, and the branch the worktree is on.
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
    WT_CHECK,
};

static Str wtkinds[] = {"make", "remove", "check"};

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
    b32        merged;   // remove: only once BRANCH is merged
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
    Str kind  = jsonstr(jsonget(d.root, "kind"));
    q.kind    = kind=="remove" ? WT_REMOVE : kind=="check" ? WT_CHECK : WT_MAKE;
    q.sid     = jsonstr(jsonget(d.root, "sid"));
    q.main    = jsonstr(jsonget(d.root, "main"));
    q.path    = jsonstr(jsonget(d.root, "path"));
    q.branch  = jsonstr(jsonget(d.root, "branch"));
    q.base    = jsonstr(jsonget(d.root, "base"));
    q.oid     = jsonstr(jsonget(d.root, "oid"));
    q.tip     = jsonstr(jsonget(d.root, "tip"));
    q.merged  = jsonflag(jsonget(d.root, "merged"), 0);
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
    Str err;      // why not
    Str did;      // remove: removed, kept, gone (no worktree there), or
                  // waiting (merged mode); check: there, or gone
    Str why;      // kept: dirty, submodules, locked, in use, branch, or error
    Str tip;      // remove, check: the branch's commit then
    Str branch;   // remove: deleted, unmerged, or null if none
    i64 commits;  // check: the branch's since OID, -1 if not known
    i64 dirty;    // check: the files not committed, -1 if not known
    Str head;     // check: the branch the worktree is on, if not BRANCH
    Str headoid;  // and its commit
    Str baseoid;  // remove, waiting: the base's commit then
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
    if (d->baseoid.len) putstrkey(&w, "baseoid", d->baseoid);
    if (q->kind == WT_CHECK) {
        putintkey(&w, "commits", d->commits);
        putintkey(&w, "dirty", d->dirty);
        if (d->head.len) putstrkey(&w, "head", d->head);
        if (d->headoid.len) putstrkey(&w, "headoid", d->headoid);
    }
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

// Whether a folder is what a removal left, its .git gone but files that
// were in use still there.
static b32 wtleftover(Os *os, Arena scratch, Str path)
{
    Str git = join(&scratch, path, ".git");
    return os_stat(os, scratch, path).type==STAT_DIR &&
           os_stat(os, scratch, git).type==STAT_MISSING && os_list(os, &scratch, path);
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
    } else if (!ours && wtleftover(os, scratch, q->path)) {
        d.err = concat(a, q->path, " still has files left from its removal, which were "
                                   "in use then: delete the folder, then try again");
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

// Whether the branch's commit, tip, is merged into the base, or for a
// detached base, still where it was cut.
static b32 wtmerged(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog, Str tip)
{
    if (!q->base.len) {
        return tip == q->oid;
    }
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "merge-base");
    args = wtargs(&scratch, args, "--is-ancestor");
    args = wtargs(&scratch, args, tip);
    args = wtargs(&scratch, args, concat(&scratch, "refs/heads/", q->base));
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
    return r.spawned && !r.timeout && !r.quit && r.code==0;
}

static WtDone wtremove(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog)
{
    WtDone d   = {};
    Os    *os  = w->os;
    Str    ref = concat(&scratch, "refs/heads/", q->branch);

    if (q->merged) {
        Str tip = wtbranchoid(w, a, scratch, q, prog, q->branch);
        if (tip.len && !wtmerged(w, a, scratch, q, prog, tip)) {
            d.ok      = 1;
            d.did     = "waiting";
            d.tip     = clone(a, tip);
            d.baseoid = q->base.len ? wtbranchoid(w, a, scratch, q, prog, q->base) : Str{};
            return d;
        }
    }

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
        Str    git = join(&scratch, q->path, ".git");
        if (err.len && os_stat(os, scratch, git).type!=STAT_MISSING) {
            d.did = "kept";
            d.why = wtwhy(err);
            d.err = clone(a, err);
            return d;
        } else if (err.len) {
            // Part of the way: git found it clean, then deleted until a
            // file in use (Windows) stopped it, its .git first. What is
            // left is committed, so only in the way of making it again.
            d.why = "leftover";
            d.err = clone(a, err);
        }
        d.did = "removed";
    } else if (!ours && wtleftover(os, scratch, q->path)) {
        d.did = "removed";
        d.why = "leftover";
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
    if (!wtmerged(w, a, scratch, q, prog, tip)) {
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

// The number of lines in a run's output.
static i64 wtlines(Str out)
{
    i64 n = 0;
    for (iz i = 0; i < out.len; i++) {
        n += out[i]=='\n' || (i==out.len-1 && out[i]!='\n');
    }
    return n;
}

static WtDone wtcheck(GitWork *w, Arena *a, Arena scratch, WtAsk *q, Str prog)
{
    WtDone d   = {};
    Os    *os  = w->os;
    Str    ref = concat(&scratch, "refs/heads/", q->branch);
    d.commits  = d.dirty = -1;

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
    d.did = ours && !ours->prunable ? Str("there") : Str("gone");
    d.tip = wtbranchoid(w, a, scratch, q, prog, q->branch);
    if (d.tip.len) {
        Slice<Str> args = {};
        args = wtargs(&scratch, args, "rev-list");
        args = wtargs(&scratch, args, "--count");
        args = wtargs(&scratch, args, concat(&scratch, concat(&scratch, q->oid, ".."),
                                             d.tip));
        GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
        Str    n = gitclean(r.out);
        Int    c = parseint(n);
        d.commits = !r.code && c.ok ? c.val : -1;
    }
    if (d.did == "gone") {
        return d;
    } else if (ours->branch != ref) {
        Str head  = ours->branch;
        d.head    = startswith(head, "refs/heads/") ? cuthead(head, 11) : head;
        d.head    = d.head.len ? clone(a, d.head) : Str("(detached)");
        d.headoid = clone(a, ours->oid);
    }
    Slice<Str> args = {};
    args = wtargs(&scratch, args, "-C");
    args = wtargs(&scratch, args, q->path);
    args = wtargs(&scratch, args, "status");
    args = wtargs(&scratch, args, "--porcelain");
    GitRun r = wtgit(w, a, scratch, q, prog, args, WT_GITWAIT);
    d.dirty  = !r.code && r.spawned && !r.timeout ? wtlines(r.out) : -1;
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
    } else if (q->kind == WT_CHECK) {
        d = wtcheck(w, a, scratch, q, prog.path);
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

// A name from a subagent's task, as "Fix the parser" gives
// fix-the-parser: the description's letters and digits, in lowercase,
// with '-' for each run of anything else between them, cut to at most
// max characters after a whole word, should one fit. It may be empty,
// or a name no worktree may have (wtname): a device's.
static Str wtslug(Arena *a, Str desc, iz max)
{
    Buf b(a, max + 2);
    b32 gap = 0;
    for (iz i = 0; i<desc.len && b.len<=max; i++) {
        u8 c = desc[i]>='A' && desc[i]<='Z' ? (u8)(desc[i] + 32) : desc[i];
        if ((c>='a' && c<='z') || (c>='0' && c<='9')) {
            if (gap && b.len) {
                putbyte(&b, '-');
            }
            putbyte(&b, c);
            gap = 0;
        } else {
            gap = 1;
        }
    }
    Str s = finish(&b);
    if (s.len > max) {
        iz at = max;
        while (at>0 && s[at]!='-') {
            at--;
        }
        s = takehead(s, at ? at : max);  // one long word: cut in it
    }
    return s;
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

// A path inside a worktree in MAIN/.prospero/worktrees, as the same path
// in MAIN, so that what is approved for one checkout of a repository
// holds for all. Any other path as it is. It is a worktree only if git
// says it is one of MAIN's.
static Str wtmainof(Os *os, Arena *a, Str path, b32 win)
{
    Str seg = win ? Str("\\.prospero\\worktrees\\") : Str("/.prospero/worktrees/");
    Str sep = win ? Str("\\") : Str("/");
    iz  at  = find(path, seg);
    if (at <= 0) {
        return path;
    }
    Str main = takehead(path, at);
    Str rest = cuthead(path, at+seg.len);
    Cut c    = cut(rest, sep[0]);
    if (!wtname(c.head)) {
        return path;
    }
    Arena t   = *a;
    Str   top = memmain(os, &t, wtpath(&t, main, c.head, win), win);
    if (!top.len || (top!=main && os_realpath(os, &t, top)!=os_realpath(os, &t, main))) {
        return path;
    }
    return c.ok ? concat(a, concat(a, main, sep), c.tail) : main;
}

// Whether path is dir or below it, as copyremap compares them.
static b32 wtunder(Arena scratch, Str path, Str dir, b32 win)
{
    return copyremap(&scratch, path, dir, "/", win).len > 0;
}

// A path as the same place in another checkout of the repository, to: a
// path in from's checkout, or in the main checkout, main, as in to. Any
// other as it is: one outside the repository, or in another worktree.
// With win, either separator, and in any case (copyremap). So a grant
// of a subagent's parent follows it into the subagent's worktree.
static Str wtinto(Arena *a, Str path, Str from, Str main, Str to, b32 win)
{
    Arena t     = *a;
    Str   trees = concat(&t, main, win ? Str("\\.prospero\\worktrees")
                                       : Str("/.prospero/worktrees"));
    b32   tree  = wtunder(t, path, trees, win);  // in some worktree
    b32   own   = from.len && (!tree || wtunder(t, from, trees, win));
    Str   r     = own ? copyremap(a, path, from, to, win) : Str{};
    if (!r.len && !tree) {
        r = copyremap(a, path, main, to, win);
    }
    return r.len ? r : path;
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

static WtPlace wtplace(Os *os, Arena *a, Arena scratch, Str cwd, b32 win)
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
        p.oid = gitrefoid(os, a, scratch, p.common, h->branch);  // HEAD on another ref
    } else {
        Str ref = concat(&scratch, "refs/heads/", h->branch);
        p.base  = h->branch;
        p.oid   = gitrefoid(os, a, scratch, p.common, ref);
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
        EvRec rec  = evparse(&t, loghead(e->os, &t, path).head);
        Json *wt   = jsonget(rec.root, "worktree");
        Str   m    = jsonstr(jsonget(wt, "main"));
        if (rec.ok && rec.ev==EV_SESSION && jsonstr(jsonget(wt, "name"))==name &&
                (win ? equalfold(m, main) : m==main)) {
            return 1;
        }
    }
    return 0;
}

// The worktrees that session records name, as wtrecorded reads them, in
// a: read once for the names that a batch of subagents may try.
static Slice<WtName> wtrecords(Engine *e, Arena *a)
{
    Str           dir  = join(a, e->statedir, "sessions");
    Dirent       *list = os_list(e->os, a, dir);
    Arena         tmp  = carve(a, 3*LOG_HEADMAX + (1<<18));  // a record's head, read and parsed
    Slice<WtName> r    = {};
    for (Dirent *d = list; d; d = d->next) {
        if (!d->isdir) continue;
        Arena t    = tmp;
        Str   path = join(&t, join(&t, dir, d->name), "events.jsonl");
        EvRec rec  = evparse(&t, loghead(e->os, &t, path).head);
        Json *wt   = jsonget(rec.root, "worktree");
        Str   name = jsonstr(jsonget(wt, "name"));
        if (rec.ok && rec.ev==EV_SESSION && name.len) {
            WtName n = {clone(a, jsonstr(jsonget(wt, "main"))), clone(a, name)};
            r = push(a, r, n);
        }
    }
    return r;
}

static b32 wtnamed(Slice<WtName> *recs, Str main, Str name, b32 win)
{
    for (iz i = 0; i < recs->len; i++) {
        WtName *n = &recs->data[i];
        if (n->name==name && (win ? equalfold(n->main, main) : n->main==main)) {
            return 1;
        }
    }
    return 0;
}

// Whether a session loaded here has the worktree at path, as a subagent
// started a moment ago has, whatever its record holds.
static b32 wtloaded(Engine *e, Str path, b32 win)
{
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->wt.name.len && (win ? equalfold(s->wt.path, path) : s->wt.path==path)) {
            return 1;
        }
    }
    return 0;
}

// Whether a worktree cannot be made by this name: its folder exists,
// its branch does, or a session has it, by the records read (recs), if
// given, else by reading them.
enum : i32 {WT_FREE, WT_HASDIR, WT_HASBRANCH, WT_HASSESSION};

static i32 wttaken(Engine *e, Arena scratch, WtPlace *p, Str name, Slice<WtName> *recs = 0)
{
    b32 win = HOST_WINDOWS;
    Str ref = concat(&scratch, concat(&scratch, "refs/heads/", wtprefix(e)), name);
    Str dir = wtpath(&scratch, p->main, name, win);
    if (os_stat(e->os, scratch, dir).type != STAT_MISSING) {
        return WT_HASDIR;
    }
    Arena oid = carve(&scratch, 128);  // apart from the scratch it reads in
    if (gitrefoid(e->os, &oid, scratch, p->common, ref).len) {
        return WT_HASBRANCH;
    } else if (wtloaded(e, dir, win)) {
        return WT_HASSESSION;
    } else if (recs ? wtnamed(recs, p->main, name, win)
                    : wtrecorded(e, scratch, p->main, name, win)) {
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
static Str wtsuggest(Engine *e, Arena *a, Arena scratch, WtPlace *p,
                     Slice<WtName> *recs = 0)
{
    for (i32 i = 0; i < WT_TRIES; i++) {
        Str name = wtrandom(a, os_random(e->os));
        if (wttaken(e, scratch, p, name, recs) == WT_FREE) {
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
    WtPlace p    = wtplace(e->os, &keep, scratch, dir, HOST_WINDOWS);
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

// Whether a job holds a session's turns: the removal of a subagent's
// merged worktree, queued while its parent was idle, which the parent's
// next turn must not see half done.
static b32 wtholding(Engine *e, Str sid)
{
    for (iz i = 0; i < e->wtjobs.len; i++) {
        if (e->wtjobs[i].holds.len && e->wtjobs[i].holds==sid) {
            return 1;
        }
    }
    return 0;
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
    b32 merged;  // remove: only once the branch is merged
    Str holds;   // a session whose turns wait for the job
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
    putintkey(&o, "merged", sp->merged);
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
    job.holds = sp->holds.len ? clone(&e->perm, sp->holds) : Str{};
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



// Sessions
//
// A worktree session's record keeps worktree: {name, main, branch, base,
// oid}, from which its folder follows. Its worktree is made after the
// session, in the background, and each turn waits for it at its start
// (wtgate), which asks for it again should it be gone. While it is
// missing as the session opens, what reads it waits too: instructions,
// memory, and for a new session, its prefix (wtready).

static b32 wtexists(Engine *e, Arena scratch, WtSess *wt)
{
    Str git = join(&scratch, wt->path, ".git");
    return os_stat(e->os, scratch, git).type != STAT_MISSING;
}

// The rest of a worktree's plan, its name free: what the session record
// keeps, and the working directory, the same place below the
// worktree's root as below the folder's checkout's, in a.
static void wtfill(Engine *e, Arena *a, Arena scratch, WtPlace *p, Str name, WtSess *wt,
                   Str *wcwd)
{
    b32 win = HOST_WINDOWS;
    wt->name   = clone(a, name);
    wt->main   = clone(a, p->main);
    wt->branch = concat(a, wtprefix(e), name);
    wt->base   = clone(a, p->base);
    wt->oid    = clone(a, p->oid);
    wt->src    = clone(a, p->h.root);
    wt->path   = wtpath(a, wt->main, name, win);
    *wcwd      = wt->path;
    if (p->sub.len) {
        Str sub = wtnative(&scratch, p->sub, win);
        *wcwd = concat(a, concat(a, wt->path, win ? Str("\\") : Str("/")), sub);
    }
    wtignore(e->os, scratch, wt->path);
}

// A worktree as asked ({name?}) from a folder, cwd, in a's arena: what
// the session record keeps, and the working directory, the same place
// below the worktree's root, also in a. Null if it can be, else why not.
static Str wtplan(Engine *e, Arena *a, Arena scratch, Json *ask, Str cwd, WtSess *wt,
                  Str *wcwd)
{
    b32     win  = HOST_WINDOWS;
    Arena   keep = carve(&scratch, 1<<16);
    WtPlace p    = wtplace(e->os, &keep, scratch, cwd, win);
    if (p.err.len) {
        return clone(a, p.err);
    }
    Json *jn   = jsonget(ask, "name");
    Str   name = jsonstr(jn);
    if (jn->type == JSON_MISSING) {
        name = wtsuggest(e, &keep, scratch, &p);
        if (!name.len) {
            return "no free name was found for the worktree: give one";
        }
    } else if (!wtname(name)) {
        return wtnamewhy(name);
    }
    i32 taken = wttaken(e, scratch, &p, name);
    if (taken != WT_FREE) {
        return wttakenwhy(e, a, taken, name);
    }
    wtfill(e, a, scratch, &p, name, wt, wcwd);
    return {};
}

// A subagent's worktree (task {worktree}), from its parent's folder,
// cwd, as wtplan's: named after its task's description, desc, with -2
// to -9 after it should that be taken, else at random. recs is the
// names that sessions record, read once for the batch (wtrecords).
// wt->src is the parent's checkout, which its grants are mapped from.
static Str wttask(Engine *e, Arena *a, Arena scratch, Str cwd, Str desc,
                  Slice<WtName> *recs, WtSess *wt, Str *wcwd)
{
    b32     win  = HOST_WINDOWS;
    Arena   keep = carve(&scratch, 1<<16);
    WtPlace p    = wtplace(e->os, &keep, scratch, cwd, win);
    if (p.err.len && !p.h.repo) {
        // Offered before there was one (wtoffer): say how to make it
        return concat(a, p.err, ": git init and commit, then start it again");
    } else if (p.err.len && !p.oid.len && p.base.len) {
        return concat(a, p.err, ": commit, then start it again");
    } else if (p.err.len) {
        return clone(a, p.err);
    }
    Str name = wtslug(&keep, desc, WT_NAMEMAX);
    if (!wtname(name) || wttaken(e, scratch, &p, name, recs)!=WT_FREE) {
        name = {};
    }
    Str stem = wtslug(&keep, desc, WT_NAMEMAX-2);  // room for -N
    for (i32 n = 2; !name.len && stem.len && n<=9; n++) {
        Buf b(&keep, WT_NAMEMAX + 1);
        print(&b, stem);
        putbyte(&b, '-');
        print(&b, (i64)n);
        Str next = finish(&b);
        if (wtname(next) && wttaken(e, scratch, &p, next, recs)==WT_FREE) {
            name = next;
        }
    }
    name = name.len ? name : wtsuggest(e, &keep, scratch, &p, recs);
    if (!name.len) {
        return "no free name was found for the worktree";
    }
    wtfill(e, a, scratch, &p, name, wt, wcwd);
    return {};
}

// Whether task may start a subagent in a worktree from a folder: one in
// a repository with a main checkout, not a submodule's or a bare
// repository's worktree, or in none yet, which a new project's first
// turn may make before it starts subagents. What may change, as the
// repository itself, an operation under way or an unborn HEAD, is
// checked when it is called (wttask).
static b32 wtoffer(Os *os, Arena scratch, Str cwd, b32 win)
{
    GitHead h = githead(os, &scratch, cwd, {}, win);
    if (!h.repo) {
        return 1;
    } else if (h.reftable) {
        return 0;
    }
    Str root = normpath(&scratch, h.root, ".", win);
    Str gd   = normpath(&scratch, h.gitdir, ".", win);
    Str own  = normpath(&scratch, root, ".git", win);
    return (win ? equalfold(gd, own) : gd==own) || memmain(os, &scratch, root, win).len;
}

// A path of a session's, whose checkout is from (null if none), as the
// worktree subagent to has it (wtinto): to any other, as it is.
static Str wtfor(Session *to, Arena *a, Str from, Str path)
{
    if (!to->wt.name.len || !to->psid.len || to->wt.from.len || !path.len) {
        return path;
    }
    return wtinto(a, path, from, to->wt.main, to->wt.path, HOST_WINDOWS);
}

// A worktree subagent, s, started by p: listed with its parent and their
// parent, whose turns' ends may find its branch merged.
static void wtkidput(Session *s, Session *p)
{
    Engine *e = s->e;
    WtKid   k = {};
    k.sid    = clone(&e->perm, s->sid);
    k.parent = clone(&e->perm, p->sid);
    k.grand  = p->psid.len ? clone(&e->perm, p->psid) : Str{};
    e->wtkids = push(&e->perm, e->wtkids, k);
}

// Whether a session has a worktree subagent below it, as wtkids lists.
static b32 wtkidof(Engine *e, Str sid)
{
    for (iz i = 0; i < e->wtkids.len; i++) {
        if (e->wtkids[i].parent==sid || e->wtkids[i].grand==sid) {
            return 1;
        }
    }
    return 0;
}

// A new session's worktree (new {worktree: {name?}}), from the folder
// chosen: null if it can be, else why not, in the session's arena.
static Str wtnew(Session *s, Arena scratch, Json *ask, Str cwd)
{
    Str err = wtplan(s->e, &s->perm, scratch, ask, cwd, &s->wt, &s->cwd);
    s->wt.src = {};  // only a side chat's original has one
    return err;
}

// A session record's worktree.
static void wtput(JsonOut *w, WtSess *wt)
{
    putobject(w);
    putstrkey(w, "name", wt->name);
    putstrkey(w, "main", wt->main);
    putstrkey(w, "branch", wt->branch);
    if (wt->base.len) putstrkey(w, "base", wt->base);
    putstrkey(w, "oid", wt->oid);
    if (wt->src.len) putstrkey(w, "src", wt->src);
    putend(w);
}

static void wtrecord(Session *s, JsonOut *w)
{
    if (s->wt.name.len) {
        putkey(w, "worktree");
        wtput(w, &s->wt);
    }
}

// What a side chat in a worktree is told as it begins, and again after
// a compaction: what it is a copy of, where it works now, and to leave
// the original's checkout alone.
static Str wtnotice(Arena *a, WtSess *wt, Str from, Str title)
{
    Buf b(a, 1024 + 2*wt->src.len + wt->path.len + title.len);
    print(&b, "You work in a git worktree of your own, ");
    print(&b, wt->path);
    print(&b, ", on the branch ");
    print(&b, wt->branch);
    print(&b, ", cut from ");
    if (wt->base.len) {
        print(&b, wt->base);
        print(&b, " at ");
    }
    print(&b, takehead(wt->oid, 10));
    print(&b, ". This session began as a copy of the session ");
    if (title.len) {
        putbyte(&b, '"');
        print(&b, title);
        print(&b, "\" (");
        print(&b, from);
        putbyte(&b, ')');
    } else {
        print(&b, from);
    }
    print(&b, ", which goes on working in ");
    print(&b, wt->src);
    print(&b, ". Paths under ");
    print(&b, wt->src);
    print(&b, " earlier in this conversation are the same files under ");
    print(&b, wt->path);
    print(&b, " here, as committed: changes not committed there did not come "
              "along. Never change files under ");
    print(&b, wt->src);
    print(&b, ": work here, and commit on your branch.");
    return finish(&b);
}

// A side chat in a worktree (fork {side, worktree: {name?}}) of the
// session from, titled title, working in pcwd: its worktree, and what
// logcopy needs of it, in a. Null if it can be, else why not.
static Str wtfork(Engine *e, Arena *a, Arena scratch, Json *ask, Str from, Str title,
                  Str pcwd, WtSess *wt, CopyWt *cw)
{
    Str cwd = {};
    Str err = wtplan(e, a, scratch, ask, pcwd, wt, &cwd);
    if (err.len) {
        return err;
    }
    Buf     b(a, 1024);
    JsonOut w = {&b};
    wtput(&w, wt);
    cw->record = finish(&b);
    cw->cwd    = cwd;
    cw->src    = wt->src;
    cw->path   = wt->path;
    cw->notice = wtnotice(a, wt, from, title);
    return {};
}

// A side chat in a worktree, made: git makes its worktree now.
static Str wtforked(Engine *e, Arena scratch, Str sid, WtSess *wt)
{
    WtSpec sp = {};
    sp.sid    = sid;
    sp.main   = wt->main;
    sp.path   = wt->path;
    sp.branch = wt->branch;
    sp.base   = wt->base;
    sp.oid    = wt->oid;
    return wtqueue(e, scratch, WT_MAKE, &sp);
}

// After a compaction, which may have left it out: a side chat in a
// worktree is told again where it works.
static void wtheed(Session *s, Arena scratch)
{
    if (s->wt.src.len && validsid(s->wt.from)) {
        Str title = sidecarget(s->e, &scratch, s->wt.from, "title", 1024);
        notify(s, "worktree", wtnotice(&scratch, &s->wt, s->wt.from, title), 0);
    }
}

// A worktree from a session record, its folder computed again: none if
// it is not one that wtnew would have made.
static WtSess wtfrom(Arena *a, Json *j)
{
    WtSess wt   = {};
    Str    name = jsonstr(jsonget(j, "name"));
    Str    main = jsonstr(jsonget(j, "main"));
    Str    br   = jsonstr(jsonget(j, "branch"));
    Str    oid  = jsonstr(jsonget(j, "oid"));
    if (!wtname(name) || !main.len || !br.len || !gitoid(oid)) {
        return wt;
    }
    wt.name   = clone(a, name);
    wt.main   = clone(a, main);
    wt.branch = clone(a, br);
    wt.base   = clone(a, jsonstr(jsonget(j, "base")));
    wt.oid    = clone(a, oid);
    wt.src    = clone(a, jsonstr(jsonget(j, "src")));
    wt.path   = wtpath(a, wt.main, name, HOST_WINDOWS);
    return wt;
}

// A session's worktree, from its record, and the session it was started
// from, if any: a side chat's original, or a fresh one's (new {side_of}).
static void wtload(Session *s, Json *rec)
{
    s->wt = wtfrom(&s->perm, jsonget(rec, "worktree"));
    s->wt.from = s->wt.name.len ? clone(&s->perm, jsonstr(jsonget(rec, "side_of"))) : Str{};
}

// The system prompt's line for a new worktree session.
static Str wtpromptline(Session *s, Arena *a)
{
    WtSess *wt = &s->wt;
    Buf     b(a, 512 + 2*wt->main.len + wt->branch.len);
    print(&b, "\n\nThe working directory is in a git worktree made for this "
              "session, on its own branch, ");
    print(&b, wt->branch);
    print(&b, ", cut from ");
    if (wt->base.len) {
        print(&b, wt->base);
        print(&b, " at ");
    }
    print(&b, takehead(wt->oid, 10));
    print(&b, ". The repository's main checkout is ");
    print(&b, wt->main);
    print(&b, ": work and commit here, on this branch, and leave the main "
              "checkout and other worktrees alone.");
    if (s->wt.from.len && !s->wt.src.len) {
        // Started fresh from another session (new {side_of})
        print(&b, " This session was started from the session ");
        print(&b, s->wt.from);
        print(&b, ", and has none of its conversation. When your work is done, "
                  "or when asked, report it to that session with send_message: "
                  "the branch, its commits and what they change, so that it "
                  "can review and merge them.");
    } else if (s->psid.len && !s->wt.from.len) {
        // A subagent's (task {worktree})
        print(&b, " The agent that started you reviews this branch and merges "
                  "it: commit your work on it before you report, and name the "
                  "branch and its commits in your report. What you leave "
                  "uncommitted is not merged.");
    }
    return finish(&b);
}

// A side chat in a worktree read its instruction files in its original's
// checkout, and its stamps name them in the worktree (logcopy), where a
// checkout gives them other times: those as they are in the original
// are stamped anew, logged but not told, so that only a file the
// original changed since its last commit is told of, as changed.
static void wtrestamp(Session *s, Arena scratch)
{
    Os              *os  = s->e->os;
    b32              win = HOST_WINDOWS;
    Slice<InstrFile> re  = {};
    for (iz i = 0; s->wt.src.len && i<s->instrfiles.len; i++) {
        InstrStamp *k  = &s->instrfiles[i];
        Stat        st = os_stat(os, scratch, k->path);
        Str         at = copyremap(&scratch, k->path, s->wt.path, s->wt.src, win);
        if (!at.len || st.type!=STAT_FILE || (st.size==k->size && st.mtime==k->mtime)) {
            continue;
        }
        Read here  = os_readat(os, &scratch, k->path, 0, (iz)INSTR_MAX);
        Read there = os_readat(os, &scratch, at, 0, (iz)INSTR_MAX);
        if (here.err || there.err || !(here.data == there.data)) {
            continue;
        }
        k->size  = st.size;
        k->mtime = st.mtime;
        InstrFile f = {};
        f.path  = k->path;
        f.size  = st.size;
        f.mtime = st.mtime;
        re = push(&scratch, re, f);
    }
    if (re.len && !s->readonly) {
        loginstr(s, scratch, re, {}, "changed", {});
    }
}

// What waited for the worktree, now there: the working directory,
// should the commit lack it, the guard, and as the session opened,
// its instructions, memory and prefix.
static void wtready(Session *s, Arena scratch)
{
    Engine *e    = s->e;
    b32     made = !s->readonly && os_stat(e->os, scratch, s->cwd).type==STAT_MISSING;
    if (made) {
        os_mkdirs(e->os, scratch, s->cwd);
    }
    if (made || s->wtdefer || s->wtfresh) {
        s->guard   = sessionguard(s, &s->perm);  // its roots' real paths, now there
        s->wtfresh = 0;
    }
    wtrestamp(s, scratch);
    if (!s->wtdefer) {
        return;
    }
    s->wtdefer = 0;
    instrstart(s, scratch);
    memstart(s, scratch);
    b32 built = !s->system.data;
    if (built) {
        buildprefix(s, scratch);
    }
    instrlogstart(s, scratch);
    memlogstart(s, scratch);
    if (built) {
        logprefix(s, scratch);
    }
    if (built && s->shellwarn.len) {
        note(s, scratch, "warn", s->shellwarn);
    }
    if (built) {
        roleheed(s, scratch);
    }
}

// Ask git to make a session's worktree: null if asked, else why not.
static Str wtask(Session *s, Arena scratch)
{
    WtSess *wt = &s->wt;
    WtSpec  sp = {};
    sp.sid    = s->sid;
    sp.main   = wt->main;
    sp.path   = wt->path;
    sp.branch = wt->branch;
    sp.base   = wt->base;
    sp.oid    = wt->oid;
    sp.tip    = sidecarget(s->e, &scratch, s->sid, "worktree", 128);
    sp.tip    = gitoid(gitclean(sp.tip)) ? gitclean(sp.tip) : Str{};
    return wtqueue(s->e, scratch, WT_MAKE, &sp);
}

// Whether a worktree session's worktree is there, as a turn begins,
// and no job for it is under way. If not, git is asked to make it, and
// the turn waits (S_WORKTREE) for wtmade. Outside a turn, it is only
// asked for.
static b32 wtgate(Session *s, Arena scratch, b32 turn)
{
    Engine *e = s->e;
    if (turn && wtholding(e, s->sid)) {
        setstate(s, S_WORKTREE);  // until wtdone
        return 0;
    } else if (!s->wt.name.len || s->readonly) {
        return 1;
    } else if (wtexists(e, scratch, &s->wt) && !wtjobfor(e, s->wt.path)) {
        wtready(s, scratch);
        return 1;
    }
    Str err = wtask(s, scratch);
    if (err.len && turn) {
        Str msg = concat(&scratch, "the worktree cannot be made: ", err);
        endturn(s, scratch, "error", msg);
    } else if (err.len) {
        Str msg = concat(&scratch, "the worktree cannot be made: ", err);
        note(s, scratch, "warn", msg);
    } else if (turn) {
        setstate(s, S_WORKTREE);
    }
    return 0;
}

// The sessions recording a worktree, by their records.
static Slice<Str> wtrecorders(Engine *e, Arena *a, Str path)
{
    Slice<Str> r   = {};
    Str        dir = join(a, e->statedir, "sessions");
    for (Dirent *d = os_list(e->os, a, dir); d; d = d->next) {
        if (!d->isdir || !validsid(d->name)) continue;
        Arena  t   = *a;
        Str    log = join(&t, join(&t, dir, d->name), "events.jsonl");
        EvRec  rec = evparse(&t, loghead(e->os, &t, log).head);
        WtSess wt  = rec.ok && rec.ev==EV_SESSION ? wtfrom(&t, jsonget(rec.root, "worktree"))
                                                  : WtSess{};
        if (wt.path == path) {
            r = push(a, r, d->name);
        }
    }
    return r;
}

// A worktree made, or not: the sessions in it go on, and those waiting
// for it begin their turns, or end them with git's reason. Made, it is
// no longer marked as removed or kept.
static void wtforget(Engine *e, Str path)
{
    for (iz i = 0; i < e->wtwaits.len; i++) {
        if (e->wtwaits[i].path == path) {
            e->wtwaits[i] = e->wtwaits[e->wtwaits.len-1];
            e->wtwaits.len--;
            return;
        }
    }
}

static void wtmade(Engine *e, Arena scratch, Str sid, Str path, b32 ok, Str err)
{
    wtsay(e, scratch, sid, path, ok ? Str("made") : Str("failed"), {}, err, {});
    wtforget(e, path);
    Slice<Str> sids = ok ? wtrecorders(e, &scratch, path) : Slice<Str>{};
    for (iz i = 0; i < sids.len; i++) {
        sidecarput(e, scratch, sids[i], "worktree", {});
    }
    for (Session *s = e->sessions; s; s = s->next) {
        Arena a = scratch;
        if (s->wt.path != path) {
            continue;
        }
        s->wtfresh |= ok;  // its guard, built while it was missing
        if (s->state != S_WORKTREE) {
            if (ok && s->wtdefer) {
                wtready(s, a);
            }
            if (ok && wtkid(s) && !s->intask && !busy(s)) {
                wtend(e, s->sid);  // a subagent done meanwhile (cancelled)
            }
        } else if (ok) {
            setstate(s, S_IDLE);
            turnready(s, a);  // through the gate again, as it may be gone
        } else {
            setstate(s, S_IDLE);
            Str why = concat(&a, "the worktree cannot be made: ", err);
            endturn(s, a, "error", why);
        }
    }
}

static void wtkiddrop(Engine *e, Str sid)
{
    for (iz i = 0; i < e->wtkids.len; i++) {
        if (e->wtkids[i].sid == sid) {
            e->wtkids[i] = e->wtkids[e->wtkids.len-1];
            e->wtkids.len--;
            return;
        }
    }
}

// A worktree removed, or kept, and its branch: the sessions recording it
// are marked so (the worktree sidecar), with the branch's commit then, or
// "kept", which only an archive or a delete tries again. Turns waiting
// for the removal ask for the worktree again.
static void wtremoved(Engine *e, Arena scratch, Json *answer)
{
    b32 ok   = jsonint(jsonget(answer, "ok"), 0) == 1;
    Str did  = ok ? jsonstr(jsonget(answer, "did")) : Str("kept");
    Str why  = ok ? jsonstr(jsonget(answer, "why")) : Str("error");
    Str path = jsonstr(jsonget(answer, "path"));
    Str tip  = jsonstr(jsonget(answer, "tip"));
    if (did == "waiting") {
        // Merged mode, its branch not merged yet: nothing changed, so no
        // news and no mark, only what to try again once it moves
        wtforget(e, path);
        WtWait wait = {};
        wait.path = clone(&e->perm, path);
        wait.base = clone(&e->perm, jsonstr(jsonget(answer, "baseoid")));
        wait.tip  = clone(&e->perm, tip);
        e->wtwaits = push(&e->perm, e->wtwaits, wait);
        for (Session *s = e->sessions; s; s = s->next) {
            if (s->wt.path==path && s->state==S_WORKTREE) {
                Arena a = scratch;
                setstate(s, S_IDLE);
                turnready(s, a);
            }
        }
        return;
    }
    wtsay(e, scratch, jsonstr(jsonget(answer, "sid")), path, did, why,
          jsonstr(jsonget(answer, "err")), jsonstr(jsonget(answer, "branch")));
    wtforget(e, path);
    Str        mark = did=="kept" ? Str("kept") : gitoid(tip) ? tip : Str("gone");
    Slice<Str> sids = wtrecorders(e, &scratch, path);
    for (iz i = 0; i < sids.len; i++) {
        sidecarput(e, scratch, sids[i], "worktree", mark);
    }
    Str branch = jsonstr(jsonget(answer, "branch"));
    if (did!="kept" && (!branch.len || branch=="deleted")) {
        for (iz i = 0; i < sids.len; i++) {
            wtkiddrop(e, sids[i]);  // nothing of it left to merge
        }
    }
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->wt.path==path && s->state==S_WORKTREE) {
            Arena a = scratch;
            setstate(s, S_IDLE);
            turnready(s, a);
        }
    }
}

// Whether a scanned session is sid or below it.
static b32 scanunder(SessScan *sc, iz i, Str sid)
{
    for (i32 n = 0; i>=0 && n<=TASK_DEPTH+8; n++) {
        ListEntry *en = &sc->list[i];
        if (en->sid == sid) {
            return 1;
        }
        iz *p = en->parent.len ? upsert(&sc->bysid, en->parent, (Arena *)0) : 0;
        i = p ? *p : -1;
    }
    return 0;
}

// Whether a scanned session is a subagent in a worktree of its own.
static b32 scankid(ListEntry *en)
{
    return en->wt.name.len && en->parent.len && !en->wt.src.len;
}

enum : i32 {
    WT_ENDTRIES = 30,       // times a merged pass waits for a session to settle
    WT_ENDWAIT  = 2*SEC,    // between them
};

// Worktrees no session needs any longer, removed: those of the sessions
// due (wtdue), or of all sessions ("" in it, as the engine starts), and
// of those deleted (wtgone). One goes once every session recording it,
// or working in it, is archived or in the Trash (it or an ancestor:
// scanaside) or deleted, open in no other process, and idle here, which
// is then parked, so that its MCP servers let go of the folder. A due
// session names its own worktree, the one it works in, and those of
// its descendants. As the engine starts, a worktree marked removed or
// kept (the worktree sidecar) is left alone, unless it is back, made
// again by hand.
//
// Subagents' worktrees, of the sessions in wtends and below them, go
// once merged: in merged mode, once every session recording or working
// in one is a finished subagent (idle, its task done), and, should its
// branch have commits, its parent is idle too, so not while that turn
// still works in it. One that git found unmerged waits until its base
// or its branch moves (wtwaits).
static void wtsweep(Engine *e, Arena scratch)
{
    b32 win = HOST_WINDOWS;
    b32 all = 0;
    for (iz i = 0; i < e->wtdue.len; i++) {
        all |= !e->wtdue[i].len;
    }
    SessScan       sc    = scansessions(e, &scratch);
    Slice<WtSess>  cands = {};
    Slice<b32>     named = {};  // by a session due, or deleted
    Slice<iz>      ended = {};  // by a session in wtends: its index, else -1
    for (iz k = 0; k < sc.list.len + e->wtgone.len; k++) {
        WtSess *wt   = k<sc.list.len ? &sc.list[k].wt : &e->wtgone[k-sc.list.len];
        b32     gone = k >= sc.list.len;
        iz      at   = -1;
        for (iz i = 0; wt->name.len && at<0 && i<cands.len; i++) {
            at = (win ? equalfold(cands[i].path, wt->path) : cands[i].path==wt->path) ? i : -1;
        }
        if (wt->name.len && at<0) {
            cands = push(&scratch, cands, *wt);
            named = push(&scratch, named, gone);
            ended = push(&scratch, ended, (iz)-1);
        } else if (at >= 0) {
            named[at] |= gone;
        }
    }
    for (iz d = 0; d < e->wtdue.len; d++) {
        iz *at = e->wtdue[d].len ? upsert(&sc.bysid, e->wtdue[d], (Arena *)0) : 0;
        Str cwd = at ? normpath(&scratch, sc.list[*at].cwd, ".", win) : Str{};
        for (iz i = 0; at && i<cands.len; i++) {
            Str p = normpath(&scratch, cands[i].path, ".", win);
            named[i] |= sc.list[*at].wt.path==cands[i].path || within(p, cwd, win);
        }
        for (iz j = 0; at && j<sc.list.len; j++) {
            // Its descendants' too, as an archive or the Trash takes them
            for (iz i = 0; sc.list[j].wt.name.len && i<cands.len; i++) {
                named[i] |= sc.list[j].wt.path==cands[i].path &&
                            scanunder(&sc, j, e->wtdue[d]);
            }
        }
    }
    Slice<WtEnd> ends = e->wtends;
    b32          any  = 0;  // every subagent's
    for (iz d = 0; d < ends.len; d++) {
        any |= !ends[d].sid.len;
    }
    for (iz j = 0; ends.len && j<sc.list.len; j++) {
        ListEntry *en = &sc.list[j];
        if (!scankid(en)) {
            continue;
        }
        iz by = -1;
        for (iz d = 0; by<0 && d<ends.len; d++) {
            by = !ends[d].sid.len || scanunder(&sc, j, ends[d].sid) ? d : -1;
        }
        for (iz i = 0; by>=0 && i<cands.len; i++) {
            ended[i] = en->wt.path==cands[i].path && ended[i]<0 ? by : ended[i];
        }
        b32 listed = 0;
        for (iz i = 0; i < e->wtkids.len; i++) {
            listed |= e->wtkids[i].sid == en->sid;
        }
        if (any && !listed) {
            // As the engine starts: whose turns' ends may merge it
            iz *p  = upsert(&sc.bysid, en->parent, (Arena *)0);
            WtKid k = {};
            k.sid    = clone(&e->perm, en->sid);
            k.parent = clone(&e->perm, en->parent);
            k.grand  = p && sc.list[*p].parent.len ? clone(&e->perm, sc.list[*p].parent)
                                                   : Str{};
            e->wtkids = push(&e->perm, e->wtkids, k);
        }
    }
    e->wtdue.len  = 0;
    e->wtgone.len = 0;
    e->wtends     = {};
    i64 now       = os_ticks(e->os);

    for (iz c = 0; c < cands.len; c++) {
        WtSess *wt = &cands[c];
        b32     merge = ended[c]>=0 && !named[c];
        if ((!all && !named[c] && !merge) || wtjobfor(e, wt->path)) {
            continue;
        }
        Arena           t      = scratch;
        Str             p      = normpath(&t, wt->path, ".", win);
        b32             ok     = 1;
        b32             marked = 1;  // every session recording it is
        b32             settle = 0;  // a session to wait for a moment
        Str             sid    = {};
        Str             parent = {};
        Slice<Session*> parks  = {};
        for (iz i = 0; ok && i<sc.list.len; i++) {
            ListEntry *en  = &sc.list[i];
            b32        rec = en->wt.path == wt->path;
            if (!rec && !within(p, normpath(&t, en->cwd, ".", win), win)) {
                continue;
            }
            sid     = rec && !sid.len ? en->sid : sid;
            parent  = rec && !parent.len ? en->parent : parent;
            marked &= !rec || sidecarget(e, &t, en->sid, "worktree", 128).data!=0;
            Session *s    = findsession(e, en->sid);
            b32      idle = !s || (!s->readonly && parkable(s));
            if (merge) {
                // Only finished subagents, none aside (the archive rule)
                ok = !scanaside(&sc, i) && !en->locked && en->parent.len && idle &&
                     (!s || !s->wtcheck);
                settle |= !ok && s && !busy(s) && !en->locked;
            } else {
                ok = scanaside(&sc, i) && !en->locked && idle;
            }
            if (s && ok) {
                parks = push(&t, parks, s);
            }
        }
        b32 there = wtexists(e, t, wt);  // not what a removal left
        b32 kept  = sid.len && gitclean(sidecarget(e, &t, sid, "worktree", 128))=="kept";
        Str tipnow = {};  // merged mode: the branch's commit, by the refs
        if (merge && ok && !kept) {
            Arena oids = carve(&t, 256);  // apart from the scratch they are read in
            Str   git  = gitcommon(e->os, &t, join(&t, wt->main, ".git"), win);
            Str   ref  = concat(&t, "refs/heads/", wt->branch);
            Str   bref = concat(&t, "refs/heads/", wt->base);
            Str   tip  = gitrefoid(e->os, &oids, t, git, ref);
            Str   base = wt->base.len ? gitrefoid(e->os, &oids, t, git, bref) : Str{};
            tipnow = tip;
            if (!there && !tip.len) {
                wtkiddrop(e, sid);  // nothing left of it
                continue;
            }
            for (iz i = 0; i < e->wtwaits.len; i++) {
                WtWait *w = &e->wtwaits[i];
                ok &= !(w->path==wt->path && tip.len && w->tip==tip && w->base==base);
            }
            // With commits, not in its parent's turn, which may still
            // build or test in it, nor while it is open elsewhere
            iz      *at = parent.len ? upsert(&sc.bysid, parent, (Arena *)0) : 0;
            Session *ps = parent.len ? findsession(e, parent) : 0;
            b32      busyp = (ps && busy(ps)) || (at && sc.list[*at].locked);
            ok &= tip==wt->oid || !busyp;
        }
        b32 again = merge && !ok && settle && ends[ended[c]].tries<WT_ENDTRIES;
        for (iz i = 0; again && i<e->wtends.len; i++) {
            again = e->wtends[i].sid != ends[ended[c]].sid;
        }
        if (again) {
            WtEnd d = ends[ended[c]];
            d.tries++;
            d.sid = d.sid.len ? clone(&e->perm, d.sid) : Str{};
            e->wtends   = push(&e->perm, e->wtends, d);
            e->wtendsat = now + WT_ENDWAIT;  // a title or a notice, settling
        }
        if (!ok || (merge && kept)) {
            continue;
        } else if (!named[c] && !merge && marked && (!there || kept)) {
            continue;  // as the engine starts: removed, or kept, before
        }
        for (iz i = 0; i < parks.len; i++) {
            park(e, parks[i]);
        }
        WtSpec sp = {};
        sp.sid    = sid;
        sp.main   = wt->main;
        sp.path   = wt->path;
        sp.branch = wt->branch;
        sp.base   = wt->base;
        sp.oid    = wt->oid;
        sp.merged = merge;
        sp.holds  = merge && !(tipnow==wt->oid) ? parent : Str{};
        wtqueue(e, t, WT_REMOVE, &sp);  // if it cannot, the next sweep tries
    }
}

// An archive undone: its worktree made again, if gone, its branch from
// the commit it was at when removed, should it be gone too.
static void wtrestore(Engine *e, Arena scratch, Str sid)
{
    Session *s = findsession(e, sid);
    if (s) {
        wtgate(s, scratch, 0);
        return;
    }
    Str    log = sidecarpath(&scratch, e, sid, "events.jsonl");
    EvRec  rec = evparse(&scratch, loghead(e->os, &scratch, log).head);
    WtSess wt  = rec.ok && rec.ev==EV_SESSION ? wtfrom(&scratch, jsonget(rec.root, "worktree"))
                                              : WtSess{};
    if (!wt.name.len || (wtexists(e, scratch, &wt) && !wtjobfor(e, wt.path))) {
        return;
    }
    WtSpec sp = {};
    sp.sid    = sid;
    sp.main   = wt.main;
    sp.path   = wt.path;
    sp.branch = wt.branch;
    sp.base   = wt.base;
    sp.oid    = wt.oid;
    sp.tip    = gitclean(sidecarget(e, &scratch, sid, "worktree", 128));
    sp.tip    = gitoid(sp.tip) ? sp.tip : Str{};
    wtqueue(e, scratch, WT_MAKE, &sp);
}

// Subagents' worktrees
//
// A subagent started in a worktree of its own (task {worktree}) commits
// its work on its branch, which its parent reviews and merges. As it
// reports, a check job says what it left there, for a line after its
// report: its commits, and the files it did not commit. The check only
// reads, as a turn may begin in the worktree while it runs: a message
// starts one, which waits at the gate for the job, as for any.

// Whether a session is a subagent in a worktree of its own: not a side
// chat's, which is a copy of another's.
static b32 wtkid(Session *s)
{
    return s->wt.name.len && s->psid.len && !s->wt.from.len;
}

// Sessions whose worktrees, and their descendants', may go once their
// branches are merged (wtsweep).
static void wtend(Engine *e, Str sid)
{
    WtEnd d = {};
    d.sid = sid.len ? clone(&e->perm, sid) : Str{};
    e->wtends   = push(&e->perm, e->wtends, d);
    e->wtendsat = 0;  // at once
}

static Str wtshort(Str oid)
{
    return takehead(oid, 7);
}

// The start of a worktree subagent's line: where it worked.
static void wtlinehead(Buf *b, WtSess *wt)
{
    print(b, "[It worked in the worktree ");
    print(b, wt->path);
    print(b, ", on the branch ");
    print(b, wt->branch);
    print(b, ", cut from ");
    if (wt->base.len) {
        print(b, wt->base);
        print(b, " at ");
    }
    print(b, wtshort(wt->oid));
}

// A worktree subagent's line, git's check not run: its branch's commit,
// as the refs have it. Its worktree is not made yet while its prefix
// waits for it (wtdefer), though git may have begun.
static Str wtfallback(Session *s, Arena *a)
{
    Engine *e   = s->e;
    b32     win = HOST_WINDOWS;
    Arena   t   = carve(a, 1<<16);  // apart from the line, built in a
    Arena   oid = carve(&t, 128);   // apart from the scratch it is read in
    Str     git = gitcommon(e->os, &t, join(&t, s->wt.main, ".git"), win);
    Str     ref = concat(&t, "refs/heads/", s->wt.branch);
    Str     tip = gitrefoid(e->os, &oid, *a, git, ref);  // packed-refs past t
    Buf     b(a, 512 + s->wt.path.len + 2*s->wt.branch.len);
    if ((s->wtdefer || !wtexists(e, t, &s->wt)) && (!tip.len || tip==s->wt.oid)) {
        print(&b, "[Its worktree, ");
        print(&b, s->wt.path);
        print(&b, ", was not made yet, so its branch, ");
        print(&b, s->wt.branch);
        print(&b, ", has no commits.]");
        return finish(&b);
    }
    wtlinehead(&b, &s->wt);
    if (tip.len) {
        print(&b, "; the branch is at ");
        print(&b, wtshort(tip));
        print(&b, ".]");
    } else {
        print(&b, "; the branch is gone.]");
    }
    return finish(&b);
}

// A worktree subagent's line from git's check of its worktree, and
// whether nothing in it is to be merged, so that it goes at once.
static Str wtcheckline(Session *s, Arena *a, Json *ans, b32 *none)
{
    Str did     = jsonstr(jsonget(ans, "did"));
    Str tip     = jsonstr(jsonget(ans, "tip"));
    i64 commits = jsonint(jsonget(ans, "commits"), -1);
    i64 dirty   = jsonint(jsonget(ans, "dirty"), -1);
    Str head    = jsonstr(jsonget(ans, "head"));
    Str headoid = jsonstr(jsonget(ans, "headoid"));
    *none = 0;
    if (jsonint(jsonget(ans, "ok"), 0)!=1 || commits<0 || (did!="gone" && dirty<0)) {
        return wtfallback(s, a);
    }

    Buf b(a, 1024 + s->wt.path.len + 2*s->wt.branch.len + head.len);
    wtlinehead(&b, &s->wt);
    print(&b, ": ");
    if (did == "gone") {
        print(&b, commits ? Str("") : Str("no commits, "));
        if (commits) {
            print(&b, commits);
            print(&b, commits==1 ? Str(" commit, ") : Str(" commits, "));
            print(&b, "the branch now at ");
            print(&b, wtshort(tip));
            print(&b, ", ");
        }
        print(&b, "but the worktree is gone.]");
        return finish(&b);
    } else if (!commits && !dirty && !head.len) {
        *none = 1;
        print(&b, "no commits and nothing uncommitted, so the worktree and "
                  "the branch are being removed.]");
        return finish(&b);
    }
    if (!commits) {
        print(&b, "no commits");
    } else {
        print(&b, commits);
        print(&b, commits==1 ? Str(" commit, the branch now at ")
                             : Str(" commits, the branch now at "));
        print(&b, wtshort(tip));
    }
    if (!dirty) {
        print(&b, ", nothing left uncommitted");
    } else {
        print(&b, ", ");
        print(&b, dirty);
        print(&b, dirty==1 ? Str(" file changed but not committed, which stays")
                           : Str(" files changed but not committed, which stay"));
        print(&b, " in the worktree");
    }
    if (head == "(detached)") {
        print(&b, "; the worktree is no longer on the branch: its HEAD is "
                  "detached at ");
        print(&b, wtshort(headoid));
    } else if (head.len) {
        print(&b, "; the worktree is no longer on the branch but on ");
        print(&b, head);
        print(&b, ", at ");
        print(&b, wtshort(headoid));
    }
    print(&b, ".]");
    return finish(&b);
}

// Ask git to check a worktree subagent's worktree, its task done, before
// it reports: whether asked. Not when a job for the worktree is under
// way, whose answer would be taken for the check's (wtqueue).
static b32 wtcheckask(Session *s, Arena scratch)
{
    Engine *e = s->e;
    if (!wtkid(s) || e->quitting || wtjobfor(e, s->wt.path) ||
            !wtexists(e, scratch, &s->wt)) {
        return 0;
    }
    WtSpec sp = {};
    sp.sid    = s->sid;
    sp.main   = s->wt.main;
    sp.path   = s->wt.path;
    sp.branch = s->wt.branch;
    sp.base   = s->wt.base;
    sp.oid    = s->wt.oid;
    if (wtqueue(e, scratch, WT_CHECK, &sp).len) {
        return 0;
    }
    s->wtcheck = 1;
    return 1;
}

// git's check of a subagent's worktree: turns waiting at the gate for it
// go on, and unless the subagent reported already (cancelled), or one
// of its own turns began meanwhile, whose end checks again, it reports,
// with a line on what it left. With nothing to merge, the worktree and
// its branch go at once.
static void wtchecked(Engine *e, Arena scratch, Json *answer)
{
    Str      path = jsonstr(jsonget(answer, "path"));
    Session *c    = findsession(e, jsonstr(jsonget(answer, "sid")));
    b32      mine = c && c->wtcheck && c->wt.path==path;
    if (mine) {
        c->wtcheck = 0;
    }
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->wt.path==path && s->state==S_WORKTREE) {
            Arena a = scratch;
            setstate(s, S_IDLE);
            turnready(s, a);
        }
    }
    if (!mine || !c->intask || busy(c) || c->wtcheck) {
        return;
    }
    b32 none = 0;
    Str line = wtcheckline(c, &scratch, answer, &none);
    taskreport(c, scratch, "done", {}, line);
    if (none) {
        wtend(e, c->sid);
    }
}

// The worker's answer to a job.
static void wtdone(Engine *e, Arena scratch, Str answer)
{
    JsonDoc d    = jsonparse(&scratch, answer);
    i64     id   = jsonint(jsonget(d.root, "id"), 0);
    Str     kind = jsonstr(jsonget(d.root, "kind"));
    Str     held = {};
    for (iz i = 0; i < e->wtjobs.len; i++) {
        if (e->wtjobs[i].id == id) {
            held = e->wtjobs[i].holds;
            e->wtjobs[i] = e->wtjobs[e->wtjobs.len-1];
            e->wtjobs.len--;
            break;
        }
    }
    if (kind == "remove") {
        wtremoved(e, scratch, d.root);
    } else if (kind == "check") {
        wtchecked(e, scratch, d.root);
    } else {
        b32 ok = jsonint(jsonget(d.root, "ok"), 0) == 1;
        wtmade(e, scratch, jsonstr(jsonget(d.root, "sid")),
               jsonstr(jsonget(d.root, "path")), ok, jsonstr(jsonget(d.root, "err")));
    }
    Session *h = held.len ? findsession(e, held) : 0;
    if (h && h->state==S_WORKTREE && !wtholding(e, held)) {
        setstate(h, S_IDLE);
        turnready(h, scratch);  // a turn held for the removal goes on
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

// The engine stops, and waits a while for removals: subagents' trees
// whose branches a turn just merged (wtends), as `prospero run` stops
// right after that turn, and the jobs removing them. A job to make or
// check a worktree, which a hook may slow, stops it all at once
// (wtquit). Whether to wait for git still.
static b32 wtquitting(Engine *e)
{
    GitWork *w       = e->wt;
    b32      stopped = w && w->started && gitquitting(w);
    for (iz i = 0; !stopped && i<e->wtjobs.len; i++) {
        if (e->wtjobs[i].kind != WT_REMOVE) {
            wtquit(e);
            stopped = 1;
        }
    }
    return !stopped && (e->wtjobs.len || e->wtends.len);
}

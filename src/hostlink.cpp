// Subagents on other hosts
//
// The task tool's host runs a subagent on another machine, or in a WSL
// distro, as a session of the engine there (new {task}), under that
// host's settings and guard, which writes its report as a durable
// task.report in its own log. This engine is a client of that host
// then, as a window is: one link (a Remote, remote.cpp) per host name,
// made on first use, its records posted straight into the engine's
// mailbox tagged with the link's outid (farmail). The hosts are this
// configuration's own [remote] sections, never an included file's, and
// on Windows the installed WSL distros, listed by a thread at start and
// cached in remote/wsl.list, but for those whose section says
// subagents = no (farhosts). The agent may run ssh or wsl.exe in its
// shell without asking, so a subagent there asks nothing either.
//
// A call's subagent (a HostKid, matched to its TaskRun by serial) waits
// until the engine there says it takes new {task} (cap task.remote),
// since the library sends what it holds as soon as hello is answered;
// an older engine fails the call. The reply names the child, which
// task.host records in the parent's log (logopen repairs a call left
// open with it), and its task.report ends the call (taskflush), or,
// moved to the background, comes as a notice. The child is opened
// attended while its parent is attended (farattend), so that a window
// showing it can answer its asks there; nothing of its work is relayed.
// Cancelling the call ends it at once, and the child is cancelled there
// once it is named. A link that never comes up fails its calls, as does
// one down for FAR_LOST, and after a failure, calls to that host fail
// at once for FAR_COOL. A link with no subagents for FAR_IDLE is
// finished.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    FAR_IDLE = 10*60*SEC,  // a link with no subagents this long is finished
    FAR_LOST = 10*60*SEC,  // down this long, its subagents are given up
    FAR_COOL = 60*SEC,     // after a failure, calls to the host fail at once
    FAR_WHY  = 1024,       // bytes of a link's trouble kept
};

// A host a subagent may run on.
struct FarHost {
    Str name;
    Str ssh;
    Str wsl;  // a distro, if not ssh
};

// A subagent on another host, for a task call of a session here.
struct HostKid {
    HostKid *next;
    u64      serial;    // its TaskRun's far
    Str      psid;      // the parent's
    Str      call;      // the task call's id
    Str      title;
    Str      line;      // the new command, until it is sent
    Str      sid;       // the child's there, once named
    i64      newid;     // the new command's id
    i64      lastseq;   // of the child's records come so far
    b32      attended;  // as last asked of the host
    b32      bg;        // moved to the background: reports as a notice
    b32      cancel;    // cancelled: its call ended, and nothing more
};

struct HostLink {
    HostLink  *next;
    Str        name;
    Remote    *r;        // null once ended (MAIL_QUIT)
    u64        outid;
    i32        state;    // RS_*
    b32        taskok;   // its engine takes new {task}
    b32        heard;    // and it was up: its caps are known
    b32        ending;   // finished or closed: no new subagents
    Str        why;      // the last trouble, in whybuf
    u8         whybuf[FAR_WHY];
    i64        downat;   // ticks: down since, 0 if not
    i64        idleat;   // ticks: without subagents since
    i64        coolby;   // ticks: failed, calls failing at once until
    i64        nextid;   // the link's own command ids
    HostKid   *kids;
    Slice<Str> queue;    // lines remotetry could not take, in order
    Region    *mem;      // kids, lines, queue: reset when none are left
    Arena      perm;
};

static void farkeep(HostLink *l, Str why)
{
    why = utf8head(why, FAR_WHY);
    copybytes(l->whybuf, why.data, why.len);
    l->why = {l->whybuf, why.len};
}


// Hosts

// This configuration's own [remote] sections, never an included file's
// (cfgown), each as it reaches its machine.
static Slice<LinkTo> farremotes(Arena *a, Config *cfg)
{
    Slice<LinkTo> r = {};
    for (IniSection *s = cfg->ini->sections; s; s = s->next) {
        b32 seen = s->kind!="remote" || !s->name.len;
        for (iz i = 0; !seen && i<r.len; i++) {
            seen = r[i].name == s->name;
        }
        if (!seen) {
            LinkTo t = {};
            t.name = s->name;
            t.ssh  = trim(cfgown(cfg, "remote", s->name, "ssh"));
            t.wsl  = t.ssh.len ? Str{} : trim(cfgown(cfg, "remote", s->name, "wsl"));
            r = push(a, r, t);
        }
    }
    return r;
}

// The hosts a subagent may run on: the configured remotes, then the
// installed distros (wslhosts), but not WSL ones where there is no WSL
// (wsl false), nor those whose section says subagents = no.
static Slice<FarHost> farhosts(Arena *a, Config *cfg, Slice<Str> distros, b32 wsl)
{
    Slice<LinkTo> remotes = farremotes(a, cfg);
    Slice<LinkTo> autos   = wsl ? wslhosts(a, distros, remotes) : Slice<LinkTo>{};
    Slice<FarHost> r = {};
    for (iz i = 0; i < remotes.len+autos.len; i++) {
        LinkTo *t  = i<remotes.len ? &remotes[i] : &autos[i-remotes.len];
        Str     no = trim(cfgown(cfg, "remote", t->name, "subagents"));
        if ((!t->ssh.len && (!t->wsl.len || !wsl)) || cfgbool(no)==0) {
            continue;
        }
        FarHost h = {t->name, t->ssh, t->wsl};
        r = push(a, r, h);
    }
    return r;
}

// Whether the engine may reach WSL distros: on Windows, or with a
// test's stand-in for wsl.exe.
static b32 farwsl(Engine *e, Arena scratch)
{
    return HOST_WINDOWS || linkstandin(e->os, &scratch, 1).len;
}

// The host of a name, as the configuration and the distros listed have
// it now. Returns 0 if there is none.
static b32 farfind(Engine *e, Arena *a, Str name, FarHost *out)
{
    Slice<FarHost> hosts = farhosts(a, e->cfg, e->distros, farwsl(e, *a));
    for (iz i = 0; i < hosts.len; i++) {
        if (hosts[i].name == name) {
            *out = hosts[i];
            return 1;
        }
    }
    return 0;
}

// The task tool's lines for the hosts, one each, or null for none.
static Str farhostlines(Arena *a, Slice<FarHost> hosts)
{
    Buf b(a, 256);
    for (iz i = 0; i < hosts.len; i++) {
        FarHost *h = &hosts[i];
        print(&b, "\n- ");
        print(&b, h->name);
        if (h->wsl.len) {
            print(&b, ": Linux (WSL) on this machine: it sees this machine's "
                      "drives under /mnt (C:\\Users is /mnt/c/Users); its own "
                      "files are \\\\wsl.localhost\\");
            print(&b, h->wsl);
            print(&b, "\\... here");
        } else {
            print(&b, ": another machine over ssh: it shares no files with "
                      "this one");
        }
    }
    return b.len ? finish(&b) : Str{};
}

// The hosts as the task tool describes them now.
static Str farhostsdesc(Engine *e, Arena *perm, Arena scratch)
{
    Slice<FarHost> hosts = farhosts(&scratch, e->cfg, e->distros, farwsl(e, scratch));
    return farhostlines(perm, hosts);
}

// The directory of a subagent on host h: cwd if the call gave one, else
// for a distro the parent's own, pcwd, as the distro sees it when it is
// on a drive or in that distro, else the home directory there.
static Str farcwd(Arena *a, FarHost *h, Str pcwd, Str cwd)
{
    if (cwd.len) {
        return cwd;
    } else if (h->wsl.len) {
        if (pcwd.len>=2 && driveletter(pcwd[0]) && pcwd[1]==':') {
            return wslpath(a, pcwd);
        }
        Str own = wsluncpath(a, pcwd, h->wsl);
        if (own.len) {
            return own;
        }
    }
    return "~";
}

// The parent's model as another host may know it, ENDPOINT/MODEL, when
// its endpoint has a name, else null.
static Str farprefer(Arena *a, Str home, Str model)
{
    if (!home.len || !model.len || parseurl(home).ok) {
        return {};
    }
    return concat(a, concat(a, home, "/"), model);
}

// What a host is asked to make a subagent: new {task}.
struct FarAsk {
    i64 id;
    Str cwd;
    Str role;    // null if the call gave none
    Str model;
    b32 attended;
    b32 projinstr;
    b32 nofallback;
    b32 tempask;
    Str parent;
    Str call;
    i32 depth;
    i32 hop;
    b32 plan;
    Str title;
    Str prefer;
    Str via;
    Str distro;
    Str prompt;
};

static Str farline(Arena *a, FarAsk *k)
{
    Buf     b(a, 512 + k->prompt.len + k->prompt.len/8);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "cmd", "new");
    putintkey(&w, "id", k->id);
    putstrkey(&w, "cwd", k->cwd);
    putintkey(&w, "attended", k->attended);
    if (k->role.len) {
        putstrkey(&w, "role", k->role);
    }
    if (k->model.len) {
        putstrkey(&w, "model", k->model);
    }
    if (k->projinstr) {
        putstrkey(&w, "instructions", "project");
    }
    if (k->nofallback) {
        putintkey(&w, "fallback", 0);
    }
    if (k->tempask) {
        putstrkey(&w, "temp", "ask");
    }
    putkey(&w, "task");
    putobject(&w);
    putstrkey(&w, "parent", k->parent);
    putstrkey(&w, "call", k->call);
    putintkey(&w, "depth", k->depth);
    putintkey(&w, "hop", k->hop);
    putintkey(&w, "plan", k->plan);
    putstrkey(&w, "title", k->title);
    if (k->prefer.len) {
        putstrkey(&w, "prefer", k->prefer);
    }
    putstrkey(&w, "via", k->via);
    if (k->distro.len) {
        putstrkey(&w, "distro", k->distro);
    }
    putstrkey(&w, "prompt", k->prompt);
    putend(&w);
    putend(&w);
    return finish(&b);
}


// The distros, listed once at start (wsl.exe -l -q starts none)

struct FarLister {
    Os      *os;
    Mailbox *mb;       // the engine's
    Str      program;  // wsl.exe, else a test's stand-in
    Str      dir;      // remote/ of the state directory
    Region  *perm;
    Region  *scratch;
};

static void farlistthread(void *arg)
{
    FarLister *f       = (FarLister *)arg;
    Arena      perm    = newarena(f->perm);
    Arena      scratch = newarena(f->scratch);
    Slice<Str> d       = wsllist(f->os, &perm, scratch, f->program, f->dir);
    Buf        b(&perm, 1024);
    for (iz i = 0; i < d.len; i++) {
        print(&b, d[i]);
        putbyte(&b, '\n');
    }
    Str list = finish(&b);
    if (os_mkdirs(f->os, scratch, f->dir)) {
        Str path = join(&scratch, f->dir, "wsl.list");
        os_write(f->os, scratch, path, list);
    }
    mailpost(f->mb, MAIL_DISTROS, 0, list, 1);
}

static void fardistros(Engine *e, Str list)
{
    e->distros = {};
    for (Str rest = list; rest.len;) {
        Cut c = cut(rest, '\n');
        Str d = trim(c.head);
        if (d.len) {
            e->distros = push(&e->perm, e->distros, clone(&e->perm, d));
        }
        rest = c.tail;
    }
}

// At the engine's start: the distros as last listed, so that sessions
// made at once see them too, then listed anew.
static void farinit(Engine *e, Arena scratch)
{
    if (!farwsl(e, scratch)) {
        return;
    }
    Str  dir = join(&scratch, e->statedir, "remote");
    Read rd  = os_read(e->os, &scratch, join(&scratch, dir, "wsl.list"), 1<<16);
    if (rd.err == READ_OK) {
        fardistros(e, rd.data);
    }
    FarLister *f = alloc<FarLister>(&e->perm);
    f->os      = e->os;
    f->mb      = e->in;
    f->program = linkstandin(e->os, &e->perm, 1);
    f->dir     = clone(&e->perm, dir);
    f->perm    = os_reserve(e->os, (iz)1<<24);
    f->scratch = os_reserve(e->os, (iz)1<<24);
    if (f->perm && f->scratch) {
        os_thread(e->os, farlistthread, f);
    }
}


// Links

static HostLink *farbyout(Engine *e, u64 id)
{
    for (HostLink *l = e->links; id && l; l = l->next) {
        if (l->outid == id) {
            return l;
        }
    }
    return 0;
}

// Send a line, or queue it while the remote's mailbox is full.
static void farsend(HostLink *l, Str line)
{
    if (l->queue.len || !l->r || !remotetry(l->r, line)) {
        l->queue = push(&l->perm, l->queue, clone(&l->perm, line));
    }
}

static void farqueue(HostLink *l)
{
    iz n = 0;
    for (; n<l->queue.len && l->r && remotetry(l->r, l->queue[n]); n++) {}
    l->queue.data += n;
    l->queue.len  -= n;
    l->queue.cap  -= n;
}

// A command of the link's own on a child: cancel, close, open.
static void farcmd(HostLink *l, Arena scratch, Str cmd, Str sid, i64 from, i32 attended)
{
    Buf     b(&scratch, 256);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "cmd", cmd);
    putintkey(&w, "id", ++l->nextid);
    putstrkey(&w, "sid", sid);
    if (cmd == "close") {
        putintkey(&w, "forget", 1);
    } else if (cmd == "open") {
        putintkey(&w, "from", from);
        putintkey(&w, "attended", attended);
    }
    putend(&w);
    farsend(l, finish(&b));
}

// The link to a host, made if there is none, or none of use: one that
// ended, or is ending.
static HostLink *farlink(Engine *e, Arena scratch, FarHost *h)
{
    HostLink *l = 0;
    for (HostLink *t = e->links; t; t = t->next) {
        l = t->name==h->name && t->r && !t->ending ? t : l;
    }
    if (l) {
        return l;
    }
    for (HostLink *t = e->links; !l && t; t = t->next) {
        l = !t->r && !t->kids && t->name==h->name ? t : 0;  // reused
    }
    if (!l) {
        l = alloc<HostLink>(&e->perm);
        l->name = clone(&e->perm, h->name);
        l->mem  = os_reserve(e->os, (iz)1<<(sizeof(void *)==8 ? 30 : 26));
        if (!l->mem) {
            return 0;
        }
        l->next  = e->links;
        e->links = l;
    }
    i64 now = os_ticks(e->os);
    if (l->coolby > now) {
        l->state = RS_FAILED;
        return l;  // failed a moment ago, and why: failing again
    }
    HostLink *next = l->next;
    Str       name = l->name;
    Region   *mem  = l->mem;
    *l = {};
    l->next   = next;
    l->name   = name;
    l->mem    = mem;
    l->perm   = newarena(mem);
    l->state  = RS_CONNECTING;
    l->idleat = now;

    Config    *c  = e->cfg;
    RemoteConf rc = {};
    rc.name       = h->name;
    rc.ssh        = h->ssh;
    rc.wsl        = h->wsl;
    rc.path       = trim(cfgown(c, "remote", h->name, "path"));
    rc.state      = trim(cfgown(c, "remote", h->name, "state"));
    rc.rconfig    = trim(cfgown(c, "remote", h->name, "config"));
    rc.program    = linkstandin(e->os, &scratch, h->wsl.len>0);
    rc.scp        = os_getenv(e->os, &scratch, "PROSPERO_TEST_SCP");
    rc.localstate = e->statedir;
    rc.envpath    = enginepath(e, &scratch, HOST_WINDOWS);
    rc.logname    = concat(&scratch, "engine-", h->name);
    rc.outid      = l->outid = ++e->nextid;
    rc.deploy     = trim(cfgown(c, "remote", h->name, "deploy")) != "ask";
    // Its own memory, never let go: the library's thread may outlive
    // its MAIL_QUIT
    Arena   ra = newarena(os_reserve(e->os, (iz)1<<22));
    Remote *r  = ra.beg ? newremote(e->os, &ra, &rc, e->in) : 0;
    if (!r || !remotego(r)) {
        l->state = RS_FAILED;
        farkeep(l, "the connection could not start");
        return l;
    }
    l->r = r;
    return l;
}

static HostKid *farkid(HostLink *l, Str sid)
{
    for (HostKid *k = l->kids; sid.len && k; k = k->next) {
        if (k->sid == sid) {
            return k;
        }
    }
    return 0;
}

// The call of a subagent there, and its parent's session here, if both
// are still waiting for it.
static TaskRun *fartaskrun(Engine *e, HostKid *k, Session **parent)
{
    Session *p = findsession(e, k->psid);
    *parent = p;
    for (iz i = 0; p && i<p->tasks.len; i++) {
        if (p->tasks[i].far==k->serial && !p->tasks[i].done) {
            return &p->tasks[i];
        }
    }
    return 0;
}

static void fardrop(HostLink *l, HostKid *k)
{
    for (HostKid **p = &l->kids; *p; p = &(*p)->next) {
        if (*p == k) {
            *p = k->next;
            break;
        }
    }
    if (!l->kids && !l->queue.len) {
        l->perm   = newarena(l->mem);  // nothing left in it
        l->queue  = {};
    }
}

// A subagent there is done: its call ends with status and text, or a
// notice gives its parent the report once in the background. It is
// closed and forgotten there.
static void farend(Engine *e, Arena scratch, HostLink *l, HostKid *k,
                   Str status, Str text)
{
    Session *p = 0;
    TaskRun *t = fartaskrun(e, k, &p);
    if (k->cancel) {
        // Its call ended when it was cancelled
    } else if (t) {
        Str call = p->calls[t->call].id;
        text = reportcut(p, &scratch, call, text, &t->spill, &t->bytes);
        t->status = clone(&p->perm, status);
        t->text   = clone(&p->perm, text);
        t->took   = os_ticks(e->os) - t->start;
        t->done   = 1;
        arm(p, T_TASK, 0);
        live(p);  // one fewer at work
    } else if (k->bg && !e->quitting) {
        Session *q = bgparent(e, scratch, k->psid);
        if (q) {
            bgnotice(q, scratch, k->call, k->sid, k->title, l->name, status, text);
        }
    }
    if (k->sid.len && l->r) {
        farcmd(l, scratch, "close", k->sid, 0, 0);
    }
    fardrop(l, k);
    l->idleat = os_ticks(e->os);
}

static void farfail(Engine *e, Arena scratch, HostLink *l, HostKid *k, Str text)
{
    farend(e, scratch, l, k, "error", text);
}

// Every subagent of a link that is gone, or never came: each call says
// why, those named that they may be at work there still.
static void farfailall(Engine *e, Arena scratch, HostLink *l)
{
    while (l->kids) {
        HostKid *k = l->kids;
        Arena    a = scratch;
        Buf      b(&a, 512 + l->why.len);
        if (k->sid.len) {
            print(&b, "The connection to ");
            print(&b, l->name);
            print(&b, " was lost after the subagent started there (session ");
            print(&b, k->sid);
            print(&b, "), so its report never came here: it may still be at "
                      "work there, or have finished.");
        } else {
            print(&b, "No subagent was started on ");
            print(&b, l->name);
            print(&b, ": ");
            print(&b, l->why.len ? l->why : Str("the connection failed"));
        }
        Str text = finish(&b);
        k->sid   = {};  // never closed there: the link is gone
        farfail(e, a, l, k, text);
    }
}

// The engine there takes new {task}: send each subagent waiting, else
// fail them, for an engine too old.
static void farflush(Engine *e, Arena scratch, HostLink *l)
{
    if (l->state!=RS_UP || !l->heard) {
        return;
    }
    for (HostKid *k = l->kids, *next = 0; k; k = next) {
        next = k->next;
        if (!k->line.len) {
            continue;
        } else if (!l->taskok) {
            Arena a = scratch;
            Buf   b(&a, 512);
            print(&b, "No subagent was started on ");
            print(&b, l->name);
            print(&b, ": the engine there is too old to run subagents for "
                      "another host. Ask the user to update the engine there "
                      "(Deploy engine, in its menu in the window, or prospero "
                      "deploy ");
            print(&b, l->name);
            print(&b, " --force).");
            farfail(e, a, l, k, finish(&b));
            continue;
        }
        farsend(l, k->line);
        k->line = {};
    }
}

// A subagent there was named: its parent's log records where it is, and
// one cancelled meanwhile is cancelled there.
static void farnamed(Engine *e, Arena scratch, HostLink *l, HostKid *k, Str sid)
{
    k->sid = clone(&l->perm, sid);
    Session *p = 0;
    TaskRun *t = fartaskrun(e, k, &p);
    if (p && !p->readonly && !k->cancel) {
        Buf   b(&scratch, 256);
        EvOut w = sbegin(p, &b, EV_TASKHOST);
        putstrkey(&w.json, "call", k->call);
        putstrkey(&w.json, "host", l->name);
        putstrkey(&w.json, "child", sid);
        ssend(p, &w);
    }
    if (t) {
        t->child = clone(&p->perm, sid);
        t->host  = clone(&p->perm, l->name);
        live(p);
    }
    if (k->cancel) {
        farcmd(l, scratch, "cancel", k->sid, 0, 0);
        farend(e, scratch, l, k, "cancelled", {});
    }
}

// A record from a link (MAIL_OUT), or its end (MAIL_QUIT).
static void farmail(Engine *e, Arena scratch, HostLink *l, Record r)
{
    i64 now = os_ticks(e->os);
    if (r.type == MAIL_QUIT) {
        l->r     = 0;
        l->state = l->state==RS_FAILED ? RS_FAILED : RS_CLOSED;
        farfailall(e, scratch, l);
        return;
    }
    EvRec rec = evparse(&scratch, r.data);
    Json *j   = rec.root;
    switch (rec.ev) {
    case EV_REMOTE: {
        Str st  = jsonstr(jsonget(j, "state"));
        Str msg = jsonstr(jsonget(j, "msg"));
        for (i32 i = 0; i < countof(remotestates); i++) {
            l->state = remotestates[i]==st ? i : l->state;
        }
        if (msg.len) {
            farkeep(l, msg);
        }
        if (l->state == RS_UP) {
            l->downat = 0;
            l->heard  = 1;
            l->taskok = 0;
            for (Json *c = jsonget(j, "caps")->child; c; c = c->next) {
                l->taskok |= jsonstr(c) == "task.remote";
            }
            farflush(e, scratch, l);
        } else if (l->state == RS_DOWN) {
            l->downat = l->downat ? l->downat : now;
        } else if (l->state == RS_FAILED) {
            l->coolby = now + FAR_COOL;
            l->ending = 1;
            farfailall(e, scratch, l);
        } else if (l->state == RS_CLOSED) {
            l->ending = 1;
            farfailall(e, scratch, l);
        }
    } break;
    case EV_REPLY: {
        i64 id = jsonint(jsonget(j, "id"), 0);
        for (HostKid *k = l->kids; id && k; k = k->next) {
            if (k->newid!=id || k->sid.len) {
                continue;
            } else if (jsonint(jsonget(j, "ok"), 0) && validsid(rec.sid)) {
                farnamed(e, scratch, l, k, rec.sid);
            } else {
                Str err = jsonstr(jsonget(j, "err"));
                Buf b(&scratch, 256 + err.len);
                print(&b, "No subagent was started on ");
                print(&b, l->name);
                print(&b, ": ");
                print(&b, err.len ? err : Str("it refused, saying nothing"));
                farfail(e, scratch, l, k, finish(&b));
            }
            break;
        }
    } break;
    case EV_SESSION: {
        HostKid *k = farkid(l, rec.sid);
        if (k && rec.seq==1 && !jsonflag(jsonget(j, "remote"), 0)) {
            // An engine that ignored the task: no subagent at all
            Buf b(&scratch, 256);
            print(&b, "No subagent was started on ");
            print(&b, l->name);
            print(&b, ": the engine there is too old to run subagents for "
                      "another host. Ask the user to update the engine there.");
            farfail(e, scratch, l, k, finish(&b));
            return;
        }
    } break;
    case EV_TASKREPORT: {
        HostKid *k = farkid(l, rec.sid);
        if (k) {
            Str status = jsonstr(jsonget(j, "status"));
            Str text   = jsonstr(jsonget(j, "text"));
            status = status=="ok" || status=="cancelled" ? status : Str("error");
            farend(e, scratch, l, k, status, text);
            return;
        }
    } break;
    }
    HostKid *k = farkid(l, rec.sid);
    if (k && rec.seq>k->lastseq) {
        k->lastseq = rec.seq;
    }
}

// Open each child attended while its parent is, and not while it is
// not, so that its asks wait only for a frontend that can answer them.
static void farattend(Engine *e, Arena scratch)
{
    for (HostLink *l = e->links; l; l = l->next) {
        for (HostKid *k = l->kids; k; k = k->next) {
            Session *p   = findsession(e, k->psid);
            b32      att = p && attending(p);
            if (k->sid.len && !k->cancel && att!=k->attended && l->r) {
                k->attended = att;
                farcmd(l, scratch, "open", k->sid, k->lastseq, att);
            }
        }
    }
}

// Start the subagent of task call k on another host. Returns 0 if it
// cannot, with why in t->text (in the parent's arena).
static b32 farstart(Session *s, Arena scratch, iz k, Args *args, TaskRun *t)
{
    Engine *e = s->e;
    FarHost h = {};
    if (!farfind(e, &scratch, args->host, &h)) {
        Slice<FarHost> hosts = farhosts(&scratch, e->cfg, e->distros, farwsl(e, scratch));
        Buf b(&s->perm, 256);
        print(&b, "No subagent was started: there is no host named \"");
        print(&b, args->host);
        print(&b, "\". ");
        if (hosts.len) {
            print(&b, "Give one of these, or none to run it here: ");
            for (iz i = 0; i < hosts.len; i++) {
                print(&b, i ? Str(", ") : Str(""));
                print(&b, hosts[i].name);
            }
            print(&b, ".");
        } else {
            print(&b, "There are no other hosts: give none to run it here.");
        }
        t->text = finish(&b);
        return 0;
    }
    HostLink *l = farlink(e, scratch, &h);
    if (!l || l->state==RS_FAILED) {
        Buf b(&s->perm, 256);
        print(&b, "No subagent was started on ");
        print(&b, h.name);
        print(&b, ": ");
        print(&b, l && l->why.len ? l->why : Str("the connection failed"));
        t->text = finish(&b);
        return 0;
    }

    Block  *c  = &s->calls[k];
    HostKid *kid = alloc<HostKid>(&l->perm);
    kid->serial = t->far = ++e->farserial;
    kid->psid   = clone(&l->perm, s->sid);
    kid->call   = clone(&l->perm, c->id);
    kid->title  = cleantitle(&l->perm, args->desc.len ? args->desc : args->prompt,
                             TITLE_MAX);
    kid->newid  = ++l->nextid;
    kid->attended = attending(s);

    FarAsk ask = {};
    ask.id         = kid->newid;
    ask.cwd        = farcwd(&scratch, &h, s->cwd, args->cwd);
    ask.role       = args->role;
    ask.model      = args->model;
    ask.attended   = kid->attended;
    ask.projinstr  = s->projinstr;
    ask.nofallback = s->nofallback;
    ask.tempask    = s->tempask;
    ask.parent     = s->sid;
    ask.call       = c->id;
    ask.depth      = s->depth + 1;
    ask.hop        = s->hop;
    ask.plan       = planning(s);
    ask.title      = kid->title;
    ask.prefer     = farprefer(&scratch, s->home, s->homemodel);
    ask.via        = h.wsl.len ? Str("wsl") : Str("ssh");
    ask.distro     = h.wsl;
    ask.prompt     = args->prompt;
    kid->line   = farline(&l->perm, &ask);
    kid->next   = l->kids;
    l->kids     = kid;
    t->host     = clone(&s->perm, h.name);
    farflush(e, scratch, l);
    return 1;
}

// The parent's calls with subagents there end at once, cancelled; each
// child is cancelled there once it is named.
static void farcancel(Session *s, Arena scratch)
{
    Engine *e = s->e;
    for (iz i = 0; i < s->tasks.len; i++) {
        TaskRun *t = &s->tasks[i];
        if (!t->far || t->done) {
            continue;
        }
        for (HostLink *l = e->links; l; l = l->next) {
            for (HostKid *k = l->kids, *next = 0; k; k = next) {
                next = k->next;
                if (k->serial != t->far) {
                    continue;
                }
                k->cancel = 1;
                if (k->sid.len) {
                    farcmd(l, scratch, "cancel", k->sid, 0, 0);
                    farend(e, scratch, l, k, "cancelled", {});
                } else if (k->line.len) {
                    fardrop(l, k);  // never sent
                }
            }
        }
        Buf b(&s->perm, 256);
        print(&b, "The subagent on ");
        print(&b, t->host);
        print(&b, " was cancelled before it finished.");
        t->status = "cancelled";
        t->text   = finish(&b);
        t->took   = os_ticks(e->os) - t->start;
        t->done   = 1;
    }
    arm(s, T_TASK, 0);
}

// The parent's call c (all if null) with a subagent there goes to the
// background: its report comes as a notice. Returns whether one did.
static b32 farbg(Session *s, Arena *a, Str call)
{
    Engine *e     = s->e;
    b32     moved = 0;
    for (iz i = 0; s->state==S_TASK && i<s->tasks.len; i++) {
        TaskRun *t = &s->tasks[i];
        if (!t->far || t->done || (call.len && call!=s->calls[t->call].id)) {
            continue;
        }
        HostKid *kid = 0;
        for (HostLink *l = e->links; !kid && l; l = l->next) {
            for (HostKid *k = l->kids; !kid && k; k = k->next) {
                kid = k->serial==t->far ? k : 0;
            }
        }
        if (!kid) {
            continue;
        }
        kid->bg = 1;
        Buf b(a, 512);
        print(&b, "The user moved this subagent to the background. It "
                  "continues on ");
        print(&b, t->host);
        if (kid->sid.len) {
            print(&b, " in session ");
            print(&b, kid->sid);
        }
        print(&b, ", and its report comes to you as a notice when it "
                  "finishes: do not wait for it or poll it.");
        t->status = "background";
        t->text   = clone(&s->perm, finish(&b));
        t->done   = 1;
        moved     = 1;
    }
    return moved;
}

// Quitting or stopping: every subagent there is cancelled, and with
// quit, every link is finished.
static void farstop(Engine *e, Arena scratch, b32 quit)
{
    for (HostLink *l = e->links; l; l = l->next) {
        for (HostKid *k = l->kids; k; k = k->next) {
            if (k->sid.len && !k->cancel) {
                k->cancel = 1;
                farcmd(l, scratch, "cancel", k->sid, 0, 0);
            }
        }
        if (quit && l->r && !l->ending) {
            l->ending = 1;
            remotefinish(l->r);
        }
    }
}

// Subagents there at work, as turns are counted (status).
static i64 farcount(Engine *e)
{
    i64 n = 0;
    for (HostLink *l = e->links; l; l = l->next) {
        for (HostKid *k = l->kids; k; k = k->next) {
            n += !k->cancel;
        }
    }
    return n;
}

// Ticks at which fartick has work, I64MAX if none.
static i64 farnext(Engine *e)
{
    i64 next = I64MAX;
    for (HostLink *l = e->links; l; l = l->next) {
        i64 at = !l->r || l->ending ? I64MAX :
                 l->kids && l->downat ? l->downat + FAR_LOST :
                 !l->kids ? l->idleat + FAR_IDLE : I64MAX;
        at   = l->queue.len && l->r ? 0 : at;  // retried at once
        next = at<next ? at : next;
    }
    return next;
}

// Idle links finished, links down too long given up, queued lines sent.
static void fartick(Engine *e, Arena scratch, i64 now)
{
    for (HostLink *l = e->links; l; l = l->next) {
        farqueue(l);
        if (!l->r || l->ending) {
            continue;
        } else if (!l->kids && now>=l->idleat+FAR_IDLE) {
            l->ending = 1;
            remotefinish(l->r);
        } else if (l->kids && l->downat && now>=l->downat+FAR_LOST) {
            farkeep(l, "it was unreachable for 10 minutes");
            l->ending = 1;
            remoteclose(l->r);
            farfailall(e, scratch, l);
        }
    }
}

// Whether a link has yet to end: quitting waits for them (up to
// quitby), so that the cancels reach their hosts.
static b32 farbusy(Engine *e)
{
    for (HostLink *l = e->links; l; l = l->next) {
        if (l->r) {
            return 1;
        }
    }
    return 0;
}

// Remote engines: the client library, and "prospero connect"
//
//   prospero connect NAME [options]            a [remote NAME] host
//   prospero connect --ssh DEST [options]      a host over ssh
//   prospero connect --local [options]         this machine's host
//   prospero connect --endpoint EP [options]   the one at EP, if it runs
//
// A Remote speaks to an engine host (host.cpp) as the in-process engine
// would to its frontend: protocol v0 commands in (remotecmd), records
// out as MAIL_OUT records in the frontend's mailbox, then MAIL_QUIT. In
// between it keeps one framed link (frame.cpp) over whatever stream it
// has now: ssh running "PATH attach" on the remote machine, with
//
//   ssh -T -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15
//       -o ServerAliveCountMax=3 -o EscapeChar=none -- HOST 'PATH attach'
//
// where -- ends ssh's options for good: without it, ssh reads options
// again after HOST, and a PATH like -oProxyCommand=... would run here.
//
// spawned through os_spawnio, or for this machine's own host a local
// connection (os_connect), starting the host when none runs. A stream
// that breaks (ssh exiting, 255 being its own failure, or a link silent
// for 20 s) is replaced after 1, 2, 5, 10, then every 30 s, by "attach
// --reconnect", and whatever the host kept for the link comes through
// it, as does what was queued here: nothing lost, nothing twice. Status
// 90 from attach means the host is gone, and a new one starts at once.
//
// Each new host, or a host that lost records for this link, gets hello
// first, and its protocol must match (HOST_PROTO): a different major is
// a clear error, and the end. Then the sessions the frontend has open
// are opened again, from the last record it has of each, attended as
// before, so that it catches up over the link: a resync never reads
// local files, which may be on another machine. Replies to the
// library's own commands (negative ids) stay here, and replies the new
// host can never give, to commands the old one may or may not have run,
// fail. The frontend's commands wait while no host has answered hello,
// then go in order, and fail if the remote ends instead: every command
// gets one reply. Records pass to the frontend in order, waiting for
// room in its mailbox, so none is ever lost there; a session's durable
// records already passed are dropped, and the sessions list gains
// "host", the remote's name. A "remote" record tells the frontend how
// the link is: state connecting, up, down (retry_ms until the next
// try), failed or closed, with msg saying why.
//
// remoteclose ends the link with a disconnect frame: the host forgets
// this client, and its sessions run on without it. remotefinish does so
// once every command sent is answered (30 s at most), as connect does
// when its input ends. Neither waits, whatever the remote's thread is
// doing: each sets a bit the thread takes before its next take of the
// mailbox, so that commands posted before it are handled before it. A
// quit command stops the host's engine as it would a local one.
//
// A remote may be one host of this machine, at its endpoint and no
// other (a background host, as registered): none is started, and once
// it cannot be reached, it is gone and the remote ends, closed.
//
// The harness there is the one configured (path), else the engine
// "prospero deploy" put there for this protocol (deploy.cpp records it),
// else "prospero" on its PATH. When that one is missing (the shell finds
// no such program) or cannot attach (another protocol, or too old),
// deploying one helps: a remote that may deploy (deploy, as with
// "connect --deploy" or [remote NAME] deploy = auto) does so once,
// saying goodbye first to a host of another protocol, its "deploy"
// records passing to the frontend while the state is deploying, and
// then connects to the engine deployed. Otherwise it fails, and the
// failed record's "deploy" says why deploying would help, missing or
// mismatch, so that a frontend can offer it: a new remote that may
// deploy. Deploying never stops a host that runs there (deploy.cpp).
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    REMOTE_KEEPMAX   = (i64)16 << 20,  // bytes of commands kept unacknowledged
    REMOTE_KEEPCOUNT = 1 << 12,
    REMOTE_TRIES     = 3,              // failures, never up, before giving up
    REMOTE_CLOSEWAIT = 3*SEC,          // for the stream to end after a goodbye
    REMOTE_DRAINWAIT = 30*SEC,         // closing: for replies still to come
};

static i64 remotebackoff[] = {1*SEC, 2*SEC, 5*SEC, 10*SEC, 30*SEC};

enum : u32 {
    REMOTE_DEPLOYED = 300,  // a deployment's outcome: 1 and the path, or 0 and why
};

// What another thread asks of the remote's (remoteask), as bits
enum : i32 {
    REMOTE_CLOSE  = 1 << 0,  // remoteclose
    REMOTE_FINISH = 1 << 1,  // remotefinish
    REMOTE_DROP   = 1 << 2,  // remotedrop, a test hook
};

enum : i32 {
    RS_CONNECTING,  // a stream is starting, or waiting for its host
    RS_UP,          // the host answered hello
    RS_DOWN,        // waiting to try again
    RS_DEPLOYING,   // deploying an engine there, then connecting
    RS_FAILED,      // given up for good
    RS_CLOSED,      // ended by remoteclose or quit
};

static Str remotestates[] = {
    "connecting", "up", "down", "deploying", "failed", "closed",
};

// Why deploying an engine would help
enum : i32 {
    NEED_NONE,
    NEED_MISSING,   // the shell there found no such program
    NEED_MISMATCH,  // one of another protocol, or one that cannot attach
};

static Str remoteneeds[] = {"", "missing", "mismatch"};

struct RemoteConf {
    Str name;      // the remote's name, in the sessions list
    Str ssh;       // destination for ssh, empty for this machine's host
    Str program;   // the ssh program, "ssh" if empty
    Str path;      // the harness there, "prospero" if empty
    Str state;     // its state directory there (--state), if not its own
    Str rconfig;   // its configuration file there (-c), if not its own
    Str localstate;  // here: for ssh's errors, or the host of --local
    Str config;    // --local: the configuration of a host started here
    Str idle;      // --idle for a host started (tests)
    Str endpoint;  // this machine's host at this endpoint, and no other:
                   // none is started, and once it cannot be reached the
                   // remote ends (closed), as the host is gone
    Str scp;       // for deploying: scp itself, "scp" if empty
    Str engines;   // for deploying: the engines, if not next to this program
    b32 deploy;    // deploy an engine when the one there is missing or another
};

// A new command awaiting its reply, which names the session made.
struct RemoteNew {
    i64 id;
    b32 attended;  // as the command asked, for opening it again
};

// A session the frontend opened, or has records of.
struct RemoteSess {
    Str  sid;
    i64  lastseq;
    b32  open;      // opened by the frontend: open again on a new host
    b32  attended;
};

struct Remote {
    Os               *os;
    RemoteConf        conf;
    Arena             perm;
    Region           *scratch;
    Mailbox          *in;       // commands, and the stream's records
    Mailbox          *out;      // the frontend's
    Link              link;
    FrameDec          dec;
    Region           *decmem;
    Proc             *proc;     // ssh, when the stream is one
    Conn             *conn;     // a local connection, when it is one
    u64               tid;      // records of the current stream carry this
    b32               live;     // a stream is running
    i32               state;
    i32               tries;    // streams ended since the link was last up
    i32               backoffs; // waits since the link was last up
    i64               retryat;  // ticks: try again then
    i64               closeby;  // ticks: give up waiting for the end
    i64               drainby;  // ticks: closing once commands are answered
    b32               fresh;    // no host known: start one (no --reconnect)
    b32               everup;
    b32               hellook;  // this host's hello matched
    b32               wanthello;// the next hello record answers the library's
    b32               gotbye;
    b32               closing;
    b32               quitting;
    b32               done;     // MAIL_QUIT posted: the thread ends
    b32               shut;     // the stream's input was ended
    b32               gone;     // its endpoint's host cannot be reached
    Slice<RemoteSess> sess;
    Slice<i64>        pending;  // the frontend's commands awaiting replies
    Slice<RemoteNew>  news;     // of those, new commands: the sid to track
    Buf               held;     // the frontend's commands before hello
    i64               nextown;  // the library's own ids: -1, -2, ...
    Str               why;      // the last trouble, for the frontend
    Str               version;  // the host's
    i32               need;     // NEED_*: why deploying would help
    b32               deploying;
    b32               deployed; // tried once: never again
    b32               redeploy; // deploy once this stream ends
    i32               cancel;   // stops a deployment (atomic)
    i32               asked;    // REMOTE_CLOSE and others (atomic)
};

static void remoteemit(Remote *r, Str line)
{
    mailpost(r->out, MAIL_OUT, 0, line, 1);
}

static void remotestatus(Remote *r, Arena scratch, i32 state, Str msg, i64 retry)
{
    r->state = state;
    Buf   b(&scratch, 256 + msg.len);
    EvOut w = evbegin(&b, {}, 0, os_time(r->os), EV_REMOTE);
    putstrkey(&w.json, "host", r->conf.name);
    putstrkey(&w.json, "state", remotestates[state]);
    if (msg.len) {
        putstrkey(&w.json, "msg", msg);
    }
    if (retry >= 0) {
        putintkey(&w.json, "retry_ms", retry/MS);
    }
    if (state==RS_UP && r->version.len) {
        putstrkey(&w.json, "version", r->version);
    }
    if (state==RS_FAILED && r->need) {
        putstrkey(&w.json, "deploy", remoteneeds[r->need]);
    }
    remoteemit(r, evend(&w));
}

static RemoteSess *remotesess(Remote *r, Str sid, b32 add)
{
    for (iz i = 0; i < r->sess.len; i++) {
        if (r->sess[i].sid == sid) {
            return &r->sess[i];
        }
    }
    if (!add) {
        return 0;
    }
    RemoteSess s = {};
    s.sid   = clone(&r->perm, sid);
    r->sess = push(&r->perm, r->sess, s);
    return &r->sess[r->sess.len-1];
}

static void dropid(Slice<i64> *ids, i64 id)
{
    for (iz i = 0; i < ids->len; i++) {
        if ((*ids)[i] == id) {
            (*ids)[i] = (*ids)[ids->len-1];
            ids->len--;
            return;
        }
    }
}

// A reply to the frontend, made here.
static void remotereply(Remote *r, Arena scratch, i64 id, Str err)
{
    Buf   b(&scratch, 256 + err.len);
    EvOut w = evbegin(&b, {}, 0, os_time(r->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 0);
    putstrkey(&w.json, "err", err);
    remoteemit(r, evend(&w));
}

// Send a frontend's command line, newline included, keeping track of
// its reply. Returns 0 if the link cannot take it.
static b32 remotesend(Remote *r, Arena scratch, Str ln)
{
    if (!linksend(&r->link, ln)) {
        return 0;
    }
    JsonDoc d  = jsonparse(&scratch, ln);
    i64     id = jsonint(jsonget(d.root, "id"), 0);
    if (d.ok && id) {
        r->pending = push(&r->perm, r->pending, id);
        if (cmdfind(jsonstr(jsonget(d.root, "cmd"))) == CMD_NEW) {
            RemoteNew n = {};
            n.id       = id;
            n.attended = jsonflag(jsonget(d.root, "attended"), 0);
            r->news    = push(&r->perm, r->news, n);
        }
    }
    return 1;
}

// A command of the library's own.
static void remoteown(Remote *r, Arena scratch, i32 cmd, Str sid, i64 from, i32 attended)
{
    Buf   b(&scratch, 256);
    EvOut w = cmdbegin(&b, cmd, --r->nextown);
    if (cmd == CMD_HELLO) {
        putintkey(&w.json, "proto", HOST_PROTO);
        putstrkey(&w.json, "client", "prospero " PROSPERO_VERSION);
    }
    if (sid.len) {
        putstrkey(&w.json, "sid", sid);
    }
    if (from >= 0) {
        putintkey(&w.json, "from", from);
    }
    if (attended >= 0) {
        putintkey(&w.json, "attended", attended);
    }
    linksend(&r->link, evend(&w));
}


// Streams

// An argument of the remote command, for the remote machine's shell: as
// it is when plain, else in double quotes, which sh and cmd.exe both
// take, a backslash included (sh drops one unquoted, as busybox's does
// on XP), and a leading - too. Returns 0 for what they do not
// both take literally inside them: quotes, $, `, %, control characters,
// and a backslash before another or at the end (a UNC path, say).
static b32 remotearg(Buf *b, Str s)
{
    b32 plain = s.len>0 && s[0]!='-';
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c<0x20 || c==0x7f || c=='"' || c=='$' || c=='`' || c=='%') {
            return 0;
        } else if (c=='\\' && (i+1==s.len || s[i+1]=='\\')) {
            return 0;
        }
        plain &= alnum(c) || c=='/' || c=='.' || c=='-' || c=='_' ||
                 c==':' || c=='~' || c=='+' || c==',' || c=='=';
    }
    putbyte(b, ' ');
    if (!plain) putbyte(b, '"');
    print(b, s);
    if (!plain) putbyte(b, '"');
    return 1;
}

// Where ssh's errors go: <state>/remote/NAME.log, the name made safe
// for a file name anywhere.
static Str remotelog(Remote *r, Arena *a)
{
    Str name = clone(a, r->conf.name);
    for (iz i = 0; i < name.len; i++) {
        u8  c  = name[i];
        b32 ok = alnum(c) || c=='-' || c=='_' || c=='@' || (c=='.' && i);
        name[i] = ok ? c : (u8)'_';
    }
    Str dir = join(a, r->conf.localstate, "remote");
    return join(a, dir, concat(a, name, ".log"));
}

// Start a stream: ssh, or a connection to this machine's host. Returns 0
// on failure, with why set.
static b32 remotestart(Remote *r, Arena scratch)
{
    Os *os = r->os;
    r->tid++;
    r->gotbye  = 0;
    r->shut    = 0;
    r->dec.len = 0;
    r->dec.bad = 0;
    i64 now = os_ticks(os);

    if (r->conf.endpoint.len) {
        r->conn = os_connect(os, scratch, r->conf.endpoint, r->in, r->tid);
        if (!r->conn) {
            r->gone = 1;
            r->why  = clone(&r->perm, "the host is gone");
            return 0;
        }
        r->live = 1;
        linkconnect(&r->link, now);
        return 1;
    } else if (!r->conf.ssh.len) {
        Str state    = r->conf.localstate;
        Str run      = join(&scratch, state, "run");
        Str name     = hostname(&scratch, HOST_PROTO);
        Str endpoint = os_mkdirs(os, scratch, run) ?
                       os_endpoint(os, &scratch, state, name) : Str{};
        if (!endpoint.len) {
            Str err = os_lasterr(os, &scratch);
            r->why = concat(&r->perm, "cannot find the host's endpoint: ", err);
            return 0;
        }
        Arena err = {};  // for the error, apart from the scratch below
        err.beg = (byte *)allocbytes(&scratch, 1<<14);
        err.end = err.beg + (1<<14);
        HostStart hs = hoststart(os, &err, scratch, state, r->conf.config,
                                 r->conf.idle, endpoint, r->in, r->tid);
        if (!hs.c) {
            r->why = clone(&r->perm, hs.err);
            return 0;
        }
        r->conn = hs.c;
        r->live = 1;
        linkconnect(&r->link, now);
        return 1;
    }

    // The harness there: configured, or deployed for this protocol
    Str path = r->conf.path;
    if (!path.len) {
        DeployRec dr = deployrecord(os, &scratch, r->conf.localstate, r->conf.ssh);
        path = dr.ok && dr.proto==HOST_PROTO ? dr.path : Str("prospero");
    }
    Buf cmd(&scratch, 256);
    b32 ok = remotearg(&cmd, path);
    print(&cmd, " attach");
    if (!r->fresh) {
        print(&cmd, " --reconnect");
    }
    if (r->conf.state.len) {
        print(&cmd, " --state");
        ok &= remotearg(&cmd, r->conf.state);
    }
    if (r->conf.rconfig.len) {
        print(&cmd, " -c");
        ok &= remotearg(&cmd, r->conf.rconfig);
    }
    if (r->conf.idle.len) {
        print(&cmd, " --idle");
        ok &= remotearg(&cmd, r->conf.idle);
    }
    if (!ok) {
        r->why = clone(&r->perm, "a path for the remote machine has a quote, $, `, %, "
                                 "a control character, or a backslash doubled or at "
                                 "its end, which its shell cannot take");
        return 0;
    } else if (r->conf.ssh[0] == '-') {
        // ssh would take it for an option, which may run a command here
        r->why = clone(&r->perm, "an ssh destination cannot begin with -");
        return 0;
    }
    Slice<Str> argv = {};
    argv = push(&scratch, argv, r->conf.program.len ? r->conf.program : Str("ssh"));
    argv = push(&scratch, argv, Str("-T"));
    for (iz i = 0; i < countof(deployopts); i++) {
        argv = push(&scratch, argv, deployopts[i]);
    }
    argv = push(&scratch, argv, Str("-o"));
    argv = push(&scratch, argv, Str("EscapeChar=none"));
    argv = push(&scratch, argv, Str("--"));
    argv = push(&scratch, argv, r->conf.ssh);
    argv = push(&scratch, argv, cuthead(finish(&cmd), 1));

    Str dir = join(&scratch, r->conf.localstate, "remote");
    os_mkdirs(os, scratch, dir);
    Spawn sp = {};
    sp.argv    = argv;
    sp.cwd     = r->conf.localstate;
    sp.env     = os_environ(os, &scratch);
    sp.outpath = remotelog(r, &scratch);
    sp.mbox    = r->in;
    sp.id      = r->tid;
    r->proc = os_spawnio(os, &sp, scratch);
    if (!r->proc) {
        Str err = os_lasterr(os, &scratch);
        r->why = concat(&r->perm, "cannot run ssh: ", err);
        return 0;
    }
    r->live = 1;
    linkconnect(&r->link, now);
    return 1;
}

// The last line ssh wrote to its log, if any, as why it ended.
static Str remotesshsaid(Remote *r, Arena *a)
{
    Read rd   = os_read(r->os, a, remotelog(r, a), 1<<16);
    Str  last = {};
    for (Str rest = rd.data; rest.len;) {
        Cut l = cut(rest, '\n');
        if (trim(l.head).len) last = trim(l.head);
        rest = l.tail;
    }
    return last;
}

// Stop the current stream, if any; its late records are ignored.
static void remotestop(Remote *r, b32 kill)
{
    if (r->proc) {
        if (kill) {
            os_kill(r->os, r->proc, 1);
        }
        os_procfree(r->os, r->proc);
        r->proc = 0;
    }
    if (r->conn) {
        os_connclose(r->os, r->conn);
        r->conn = 0;
    }
    r->live = 0;
    r->tid++;
}

// The end: the frontend hears it, then MAIL_QUIT.
static void remoteend(Remote *r, Arena scratch, i32 state, Str why)
{
    remotestop(r, 1);
    __atomic_store_n(&r->cancel, 1, __ATOMIC_RELEASE);
    if (r->state != state) {
        remotestatus(r, scratch, state, why, -1);  // else told already
    }
    // Replies that will never come, to commands sent and held
    Str err = why.len ? why : Str("the remote is closed");
    for (iz i = 0; i < r->pending.len; i++) {
        remotereply(r, scratch, r->pending[i], err);
    }
    r->pending.len = 0;
    for (Str rest = finish(&r->held); rest.len;) {
        Arena a = scratch;
        Cut   l = cut(rest, '\n');
        rest = l.tail;
        JsonDoc d  = jsonparse(&a, l.head);
        i64     id = jsonint(jsonget(d.root, "id"), 0);
        if (d.ok && id) {
            remotereply(r, a, id, err);
        }
    }
    r->held.len = 0;
    mailpost(r->out, MAIL_QUIT, 0, {}, 1);
    r->done = 1;
}

// Deploying

// A deployment on a thread of its own, so that the remote thread goes
// on: its records come through the remote's mailbox as MAIL_OUT, then
// its outcome as REMOTE_DEPLOYED.
struct RemoteDeploy {
    Remote    *r;
    DeployConf conf;
    Region    *perm;     // kept: a child may post to its mailbox late
    Region    *scratch;
};

static void remotedeploythread(void *arg)
{
    RemoteDeploy *rd      = (RemoteDeploy *)arg;
    Remote       *r       = rd->r;
    Arena         perm    = newarena(rd->perm);
    Arena         scratch = newarena(rd->scratch);
    Deployed      d       = deploy(r->os, &perm, scratch, &rd->conf);
    Buf b(&perm, 16 + d.path.len + d.err.len);
    putbyte(&b, (u8)d.ok);
    print(&b, d.ok ? d.path : d.err);
    os_release(r->os, rd->scratch);
    mailpost(r->in, REMOTE_DEPLOYED, 0, finish(&b), 0);
}

// Deploy an engine there, once, then connect to it.
static void remotedeploy(Remote *r, Arena scratch, Str why)
{
    Os           *os = r->os;
    RemoteDeploy *rd = alloc<RemoteDeploy>(&r->perm);
    rd->r               = r;
    rd->conf.name       = r->conf.name;
    rd->conf.ssh        = r->conf.ssh;
    rd->conf.program    = r->conf.program;
    rd->conf.scp        = r->conf.scp;
    rd->conf.engines    = r->conf.engines;
    rd->conf.localstate = r->conf.localstate;
    rd->conf.notes      = r->in;
    rd->conf.cancel     = &r->cancel;
    rd->perm            = os_reserve(os, (iz)1<<24);
    rd->scratch         = os_reserve(os, 3*DEPLOY_MAXSIZE);
    r->deployed = 1;
    if (!rd->perm || !rd->scratch || !os_thread(os, remotedeploythread, rd)) {
        if (rd->perm) os_release(os, rd->perm);
        if (rd->scratch) os_release(os, rd->scratch);
        r->why = clone(&r->perm, "cannot start deploying an engine: out of memory");
        remoteend(r, scratch, RS_FAILED, r->why);
        return;
    }
    r->deploying = 1;
    Str msg = concat(&scratch, "deploying an engine: ", why);
    remotestatus(r, scratch, RS_DEPLOYING, msg, -1);
}

// What ssh's errors say when the engine there is missing (the shell
// found no such program, in sh or cmd.exe), or cannot attach (another
// platform's, or a harness too old to know attach, its usage printed).
// Not when ssh itself could not run here (the reaper's 127).
static i32 remoteneed(Str log, i32 code)
{
    if (find(log, "prospero: cannot run ") >= 0) {
        return NEED_NONE;
    } else if (code==127 ||
            find(log, "is not recognized as an internal or external command")>=0 ||
            find(log, "cannot find the path specified")>=0) {
        return NEED_MISSING;
    } else if (code==126 || ((code==1 || code==2) && find(log, "usage: prospero")>=0)) {
        return NEED_MISMATCH;
    }
    return NEED_NONE;
}

// The engine there is missing or cannot attach: deploy one when this
// remote may and has not yet, else the end, saying what would help.
static void remoteneeded(Remote *r, Arena scratch, i32 need, Str said)
{
    remotestop(r, 1);
    r->need = need;
    Buf b(&scratch, 256 + said.len);
    print(&b, need==NEED_MISSING ? Str("no engine on ") : Str("the engine on "));
    print(&b, r->conf.name);
    print(&b, need==NEED_MISSING ? Str("") : Str(" cannot attach"));
    if (said.len) {
        print(&b, " (");
        print(&b, said);
        print(&b, ")");
    }
    if (r->conf.deploy && !r->deployed) {
        remotedeploy(r, scratch, finish(&b));
        return;
    }
    print(&b, ": deploy one with \"prospero deploy ");
    print(&b, r->conf.name);
    print(&b, "\"");
    r->why = clone(&r->perm, finish(&b));
    remoteend(r, scratch, RS_FAILED, r->why);
}

// The stream ended (code: how ssh exited, else -1): try again, at once
// when the host is gone, after a while otherwise, or not at all when
// closing, or when the remote has never worked after a few tries.
static void remotedown(Remote *r, Arena scratch, Str why, i32 code)
{
    remotestop(r, 1);
    r->hellook   = 0;
    r->wanthello = 0;
    if (r->state == RS_FAILED) {
        remoteend(r, scratch, RS_FAILED, r->why);  // why it failed, told
        return;
    } else if (r->redeploy && !r->quitting) {
        // A host of another protocol: goodbye said, deploy one of ours
        r->redeploy = 0;
        r->closing  = 0;
        r->closeby  = 0;
        remotedeploy(r, scratch, r->why);
        return;
    }
    r->why = clone(&r->perm, why);
    if (r->closing) {
        remoteend(r, scratch, RS_CLOSED, {});  // as asked
        return;
    } else if (r->quitting) {
        remoteend(r, scratch, RS_CLOSED, "the host stopped");
        return;
    } else if (r->gone) {
        remoteend(r, scratch, RS_CLOSED, r->why);  // nobody to try again
        return;
    }
    b32 now = code == HOST_NOSERVER;
    if (now || r->gotbye) {
        r->fresh = 1;  // the host is gone: start another
    }
    if (!r->everup && !now && ++r->tries>=REMOTE_TRIES) {
        remoteend(r, scratch, RS_FAILED, why);
        return;
    }
    iz  last = countof(remotebackoff) - 1;
    i64 wait = now ? 0 : remotebackoff[r->backoffs<last ? r->backoffs : last];
    r->backoffs += !now;
    r->retryat   = os_ticks(r->os) + wait;
    remotestatus(r, scratch, RS_DOWN, why, wait);
}

// The host answered on a new stream: hello first, and on a new host,
// or one that lost frames, the sessions the frontend has open again.
static void remoteup(Remote *r, Arena scratch, i32 ev)
{
    b32 catchup = r->everup && ev!=LINK_READY;
    if (ev == LINK_FRESH) {
        // Replies the old host owed will never come, and the link
        // dropped what it kept for it (frame.cpp); what was held for
        // after hello goes to the new one
        for (iz i = 0; i < r->pending.len; i++) {
            Arena a = scratch;
            remotereply(r, a, r->pending[i],
                        "the remote host started over: this may not have run");
        }
        r->pending.len = 0;
        r->news.len    = 0;
    }
    if (!r->hellook || catchup || !r->everup) {
        remoteown(r, scratch, CMD_HELLO, {}, -1, -1);
        r->wanthello = 1;
    }
    if (catchup) {
        for (iz i = 0; i < r->sess.len; i++) {
            RemoteSess *s = &r->sess[i];
            if (s->open) {
                remoteown(r, scratch, CMD_OPEN, s->sid, s->lastseq, s->attended);
            }
        }
    }
    r->everup   = 1;
    r->tries    = 0;
    r->backoffs = 0;
    r->fresh    = 0;
}

// A command from the frontend, on its way to the host.
static void remotecommand(Remote *r, Arena scratch, Str line)
{
    JsonDoc d   = jsonparse(&scratch, line);
    Json   *j   = d.root;
    i64     id  = jsonint(jsonget(j, "id"), 0);
    i32     cmd = cmdfind(jsonstr(jsonget(j, "cmd")));
    Str     sid = jsonstr(jsonget(j, "sid"));
    if (r->state==RS_FAILED || r->state==RS_CLOSED) {
        remotereply(r, scratch, id, r->why.len ? r->why : Str("the remote is closed"));
        return;
    }
    if (d.ok && cmd==CMD_OPEN && sid.len) {
        RemoteSess *s = remotesess(r, sid, 1);
        i64 from = jsonint(jsonget(j, "from"), 0);
        s->lastseq  = from>0 ? from : 0;
        s->open     = 1;
        s->attended = jsonflag(jsonget(j, "attended"), 0);
    } else if (d.ok && cmd==CMD_CLOSE && sid.len) {
        RemoteSess *s = remotesess(r, sid, 0);
        if (s) {
            s->open = 0;
        }
    } else if (d.ok && cmd==CMD_QUIT) {
        r->quitting = 1;
    }

    Str ln = concat(&scratch, trimright(line), "\n");
    if (!r->hellook) {
        print(&r->held, ln);  // after hello
    } else if (!remotesend(r, scratch, ln)) {
        remotereply(r, scratch, id, "too much is waiting to go to the remote host");
    }
}

// A record from the host.
static void remoterecord(Remote *r, Arena scratch, Str line)
{
    EvRec rec = evparse(&scratch, line);
    if (!rec.ok) {
        return;
    }
    switch (rec.ev) {
    case EV_HELLO:
        if (!r->wanthello) {
            break;  // the frontend's
        }
        r->wanthello = 0;
        if (jsonint(jsonget(rec.root, "proto"), -1) != HOST_PROTO) {
            // Goodbye, then the end, or deploying one of this protocol,
            // which starts beside that host (one per protocol)
            b32 ssh = r->conf.ssh.len && !r->conf.path.len;
            Buf b(&scratch, 256);
            print(&b, "the harness on ");
            print(&b, r->conf.name);
            print(&b, " speaks protocol ");
            print(&b, jsonint(jsonget(rec.root, "proto"), -1));
            print(&b, " (version ");
            print(&b, jsonstr(jsonget(rec.root, "version")));
            print(&b, "), but this one speaks ");
            print(&b, (i64)HOST_PROTO);
            print(&b, " (version " PROSPERO_VERSION "): install a matching "
                      "version there");
            if (ssh) {
                print(&b, ", as \"prospero deploy ");
                print(&b, r->conf.name);
                print(&b, "\" does");
            }
            r->why      = clone(&r->perm, finish(&b));
            r->need     = ssh ? NEED_MISMATCH : NEED_NONE;
            r->closing  = 1;
            r->closeby  = os_ticks(r->os) + REMOTE_CLOSEWAIT;
            r->redeploy = ssh && r->conf.deploy && !r->deployed;
            linkbye(&r->link);
            if (!r->redeploy) {
                remotestatus(r, scratch, RS_FAILED, r->why, -1);
            }
            return;
        }
        if (jsonstr(jsonget(rec.root, "version")) != r->version) {
            r->version = clone(&r->perm, jsonstr(jsonget(rec.root, "version")));
        }
        r->hellook = 1;
        for (Str rest = finish(&r->held); rest.len;) {
            Arena a = scratch;
            Cut   l = cut(rest, '\n');
            rest = l.tail;
            Str ln = concat(&a, l.head, "\n");
            if (!remotesend(r, a, ln)) {
                i64 id = jsonint(jsonget(jsonparse(&a, l.head).root, "id"), 0);
                remotereply(r, a, id, "too much is waiting to go to the remote host");
            }
        }
        r->held.len = 0;
        remotestatus(r, scratch, RS_UP, {}, -1);
        return;
    case EV_REPLY: {
        i64 id = jsonint(jsonget(rec.root, "id"), 0);
        if (id < 0) {
            return;  // the library's own
        }
        dropid(&r->pending, id);
        for (iz i = 0; i < r->news.len; i++) {
            RemoteNew n = r->news[i];
            if (n.id != id) {
                continue;
            }
            r->news[i] = r->news[r->news.len-1];
            r->news.len--;
            if (jsonint(jsonget(rec.root, "ok"), 0) && rec.sid.len) {
                RemoteSess *s = remotesess(r, rec.sid, 1);
                s->open     = 1;
                s->attended = n.attended;
            }
            break;
        }
    } break;
    case EV_SESSIONS: {
        // Which remote these are on
        Str head = cuttail(trimright(line), 1);
        Buf b(&scratch, line.len + 64 + r->conf.name.len);
        print(&b, head);
        print(&b, ",\"host\":");
        printjsonstr(&b, r->conf.name);
        print(&b, "}\n");
        remoteemit(r, finish(&b));
    } return;
    case EV_OVERFLOW:
        // The host lost records: what the frontend has open, again
        for (iz i = 0; i < r->sess.len; i++) {
            RemoteSess *s = &r->sess[i];
            if (s->open) {
                remoteown(r, scratch, CMD_OPEN, s->sid, s->lastseq, s->attended);
            }
        }
        return;
    }
    if (rec.seq && rec.sid.len) {
        RemoteSess *s = remotesess(r, rec.sid, 1);
        if (rec.seq <= s->lastseq) {
            return;  // the frontend has it
        }
        s->lastseq = rec.seq;
    }
    remoteemit(r, line);
}

static void remoteframes(Remote *r, Arena scratch, Str data)
{
    i64 now = os_ticks(r->os);
    if (data.len) {
        linkheard(&r->link, now);  // a frame in pieces counts too
    }
    for (FrameIn f; (f = framenext(&r->dec, &data)).ok;) {
        Arena a  = scratch;
        i32   ev = linkframe(&r->link, &f, now);
        switch (ev) {
        case LINK_DATA:
            remoterecord(r, a, f.payload);
            break;
        case LINK_READY:
        case LINK_LOST:
        case LINK_FRESH:
            remoteup(r, a, ev);
            break;
        case LINK_BYE:
            r->gotbye = 1;
            break;
        case LINK_BROKEN:
            remotedown(r, a, "the stream broke", -1);
            return;
        }
        if (!r->live) {
            return;
        }
    }
    if (r->dec.bad) {
        remotedown(r, scratch, "garbage on the stream", -1);
    }
}

// Why ssh ended, for the frontend.
static Str remoteexit(Remote *r, Arena *a, i32 code)
{
    Str said = remotesshsaid(r, a);
    if (said.len) {
        return said;
    }
    Buf b(a, 64);
    print(&b, code==255 ? Str("ssh failed") : Str("the stream ended"));
    print(&b, ", exit status ");
    print(&b, (i64)code);
    return finish(&b);
}

// Write what the link has due to the stream.
static void remoteflush(Remote *r, Arena scratch, i64 now)
{
    if (!r->live) {
        return;
    }
    Os  *os  = r->os;
    Str  out = linkout(&r->link, &scratch, now);
    b32  ok  = !out.len || (r->proc ? os_procwrite(os, r->proc, out)
                                    : os_connwrite(os, r->conn, out));
    if (!ok) {
        remotedown(r, scratch, "cannot write to the stream", -1);
    } else if (r->closing && !r->shut) {
        // Goodbye said: end the stream's input, then wait for its end
        r->shut = 1;
        if (r->proc) os_procwrite(os, r->proc, {});
        if (r->conn) os_connwrite(os, r->conn, {});
    }
}

// End the link as remoteclose asked: at once without a stream, else
// with a goodbye, and then the stream's end.
static void remoteleave(Remote *r, Arena scratch)
{
    r->drainby = 0;
    if (!r->live) {
        remoteend(r, scratch, RS_CLOSED, {});
    } else if (!r->closing) {
        r->closing = 1;
        r->closeby = os_ticks(r->os) + REMOTE_CLOSEWAIT;
        linkbye(&r->link);
    }
}

static void remotethread(void *arg)
{
    Remote *r  = (Remote *)arg;
    Os     *os = r->os;
    r->retryat = os_ticks(os);
    while (!r->done) {
        Arena scratch = newarena(r->scratch);
        i64   now     = os_ticks(os);
        b32   nap     = 1;

        // A stream, when it is time for one
        if (!r->live && !r->deploying && now>=r->retryat) {
            remotestatus(r, scratch, RS_CONNECTING, {}, -1);
            if (!remotestart(r, scratch)) {
                remotedown(r, scratch, r->why, -1);
                continue;
            }
            remoteflush(r, scratch, now);  // the replay request
            nap = 0;  // starting it may have napped on the mailbox
        }

        i64 next = r->live      ? linkdeadline(&r->link) :
                   r->deploying ? now + 3600*SEC : r->retryat;
        next = r->closeby && r->closeby<next ? r->closeby : next;
        next = r->drainby && r->drainby<next ? r->drainby : next;
        i32 ms = 0;
        if (next>now && nap) {
            i64 wait = (next - now + MS - 1) / MS;
            ms = wait>3600000 ? 3600000 : (i32)wait;
        }
        if (ms) {
            mailwait(r->in, ms);
        }

        // What was asked before this take: the commands posted first
        // are in it, to be handled first
        i32  asked = __atomic_exchange_n(&r->asked, 0, __ATOMIC_ACQUIRE);
        Mail m     = mailtake(r->in);
        for (Record rec; !r->done && (rec = mailnext(&m)).ok;) {
            Arena a = scratch;
            switch (rec.type) {
            case MAIL_CMD:
                remotecommand(r, a, rec.data);
                break;
            case PROC_DATA:
            case CONN_DATA:
                if (rec.id==r->tid && r->live) {
                    remoteframes(r, a, rec.data);
                }
                break;
            case PROC_EXIT:
                if (rec.id==r->tid && r->live) {
                    i32  code = get32(rec.data);
                    Str  why  = code==HOST_NOSERVER ? Str("the host was gone")
                                                    : remoteexit(r, &a, code);
                    Read log  = os_read(os, &a, remotelog(r, &a), 1<<16);
                    b32  ours = !r->conf.path.len && !r->closing && !r->quitting;
                    i32  need = ours ? remoteneed(log.data, code) : NEED_NONE;
                    if (need) {
                        remoteneeded(r, a, need, remotesshsaid(r, &a));
                    } else {
                        remotedown(r, a, why, code);
                    }
                }
                break;
            case REMOTE_DEPLOYED:
                r->deploying = 0;
                if (rec.data.len && rec.data[0]) {
                    r->need     = NEED_NONE;
                    r->fresh    = 1;  // starting a host of the new engine
                    r->tries    = 0;
                    r->backoffs = 0;
                    r->retryat  = os_ticks(os);
                } else {
                    Str err = cuthead(rec.data, rec.data.len>0);
                    r->why = concat(&r->perm, "cannot deploy an engine: ", err);
                    remoteend(r, a, RS_FAILED, r->why);
                }
                break;
            case MAIL_OUT:
                remoteemit(r, rec.data);  // a deployment's progress
                break;
            case CONN_EOF:
                if (rec.id==r->tid && r->live) {
                    remotedown(r, a, "the host closed the connection", -1);
                }
                break;
            }
        }
        if (!r->done && (asked & REMOTE_DROP) && r->live && !r->closing) {
            remotedown(r, scratch, "the stream was dropped (a test)", -1);
        }
        if (r->done) {
            break;
        } else if (asked & REMOTE_CLOSE) {
            remoteleave(r, scratch);
        } else if (asked & REMOTE_FINISH) {
            if ((r->held.len || r->pending.len) && !r->drainby && !r->closing) {
                // Commands to send yet, or replies to come: first those
                r->drainby = os_ticks(os) + REMOTE_DRAINWAIT;
            } else {
                remoteleave(r, scratch);
            }
        }
        if (r->done) {
            break;
        }

        now = os_ticks(os);
        if (r->drainby && (now>=r->drainby || (!r->held.len && !r->pending.len))) {
            remoteleave(r, scratch);
            if (r->done) {
                break;
            }
        }
        if (r->live && !linktick(&r->link, now)) {
            remotedown(r, scratch, "the link went silent", -1);
        } else if (r->live && r->closeby && now>=r->closeby) {
            remotedown(r, scratch, r->why, -1);  // closing: the end
        }
        remoteflush(r, scratch, now);
    }
}

// A remote for a frontend whose mailbox takes its records. Null if out
// of memory.
static Remote *newremote(Os *os, Arena *perm, RemoteConf *conf, Mailbox *out)
{
    Remote *r = alloc<Remote>(perm);
    r->os      = os;
    r->out     = out;
    r->in      = newmailbox(perm, 1<<20, 0, 0);
    r->scratch = os_reserve(os, (iz)1<<(sizeof(void *)==8 ? 32 : 26));
    r->decmem  = os_reserve(os, FRAME_MAX + ((iz)1<<20));
    Region *pr = os_reserve(os, (iz)1<<(sizeof(void *)==8 ? 32 : 27));
    if (!r->in || !r->scratch || !r->decmem || !pr) {
        return 0;
    }
    r->perm = newarena(pr);
    Arena dm = newarena(r->decmem);
    r->dec   = newframedec(&dm, FRAME_MAX);
    r->link  = newlink(&r->perm, os_random(os), REMOTE_KEEPMAX, REMOTE_KEEPCOUNT);
    r->held  = Buf(&r->perm, 1<<12);
    r->fresh = 1;  // starting a host, if none runs
    r->conf.name       = clone(&r->perm, conf->name.len ? conf->name : conf->ssh);
    r->conf.ssh        = clone(&r->perm, conf->ssh);
    r->conf.program    = clone(&r->perm, conf->program);
    r->conf.path       = clone(&r->perm, conf->path);
    r->conf.state      = clone(&r->perm, conf->state);
    r->conf.rconfig    = clone(&r->perm, conf->rconfig);
    r->conf.localstate = clone(&r->perm, conf->localstate);
    r->conf.config     = clone(&r->perm, conf->config);
    r->conf.idle       = clone(&r->perm, conf->idle);
    r->conf.endpoint   = clone(&r->perm, conf->endpoint);
    r->conf.scp        = clone(&r->perm, conf->scp);
    r->conf.engines    = clone(&r->perm, conf->engines);
    r->conf.deploy     = conf->deploy;
    if (!r->conf.name.len) {
        r->conf.name = "local";
    }
    return r;
}

static b32 remotego(Remote *r)
{
    return os_thread(r->os, remotethread, r);
}

// Post a command line from any thread. Returns 0 if it cannot be taken.
static b32 remotecmd(Remote *r, Str line)
{
    return line.len<=mailmax(r->in) && mailpost(r->in, MAIL_CMD, 0, line, 1);
}

// The same without waiting, for a frontend whose own thread takes the
// remote's records: 0 if the remote's mailbox is full.
static b32 remotetry(Remote *r, Str line)
{
    return line.len<=mailmax(r->in) && mailpost(r->in, MAIL_CMD, 0, line, 0);
}

// Ask the remote's thread for something, from any thread, never
// waiting: the thread may itself be waiting for room in the frontend's
// mailbox, which the frontend's thread empties.
static void remoteask(Remote *r, i32 what)
{
    __atomic_fetch_or(&r->asked, what, __ATOMIC_RELEASE);
    mailpoke(r->in);
}

// End the link: the host forgets this client, MAIL_QUIT follows.
static void remoteclose(Remote *r)
{
    remoteask(r, REMOTE_CLOSE);
}

// The same once every command sent is answered, or after 30 s: the end
// of a frontend's commands, as of standard input (prospero connect).
static void remotefinish(Remote *r)
{
    remoteask(r, REMOTE_FINISH);
}

// A test hook: break the stream, killing ssh, as a network would, so
// that the link comes back over another.
static void remotedrop(Remote *r)
{
    remoteask(r, REMOTE_DROP);
}


// prospero connect

static Str connectusage =
"usage: prospero connect NAME [options]\n"
"       prospero connect --ssh DEST [options]\n"
"       prospero connect --local [options]\n"
"       prospero connect --endpoint EP [options]\n"
"Protocol v0 on standard I/O, as prospero --headless, with the engine\n"
"a host on another machine ([remote NAME], or --ssh), or on this one\n"
"(starting one if none runs, or only the one at EP, as prospero hosts\n"
"lists it). When standard input ends, the link ends: the host runs on.\n"
"options:\n"
"  --ssh DEST          ssh destination: [user@]host\n"
"  --path PATH         the harness there (default: prospero)\n"
"  --remote-state DIR  its state directory there\n"
"  --remote-config F   its configuration file there\n"
"  --ssh-program PROG  ssh itself (default: ssh)\n"
"  --deploy            deploy an engine there if it has none, or one of\n"
"                      another protocol (as prospero deploy)\n"
"  --engines DIR       the engines to deploy (default: engines next to\n"
"                      this program)\n"
"  --scp-program PROG  scp itself, for deploying (default: scp)\n"
"  -c FILE             configuration file\n"
"  --state DIR         session storage directory here\n"
"  --idle SECS         idle exit of a host started\n";

struct ConnectIn {
    Os     *os;
    Remote *r;
    Arena   a;
};

// Commands from standard input, one per line, then the end of the link,
// once they are answered.
static void connectstdin(void *arg)
{
    ConnectIn *c   = (ConnectIn *)arg;
    iz         cap = 1 << 20;
    u8        *buf = allocbytes(&c->a, cap);
    iz         len = 0;
    for (;;) {
        iz n = os_stdin(c->os, buf+len, cap-len);
        if (n <= 0) {
            break;
        }
        len += n;
        iz start = 0;
        for (iz i = len-n; i < len; i++) {
            if (buf[i] != '\n') continue;
            Str line = trim(span(buf+start, buf+i));
            if (line.len) {
                remotecmd(c->r, line);
            }
            start = i + 1;
        }
        for (iz i = start; i < len; i++) {
            buf[i-start] = buf[i];
        }
        len -= start;
        if (len == cap) {
            len = 0;  // a line too long to take: dropped
        }
    }
    remotefinish(c->r);
}

static i32 connectmain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    RemoteConf rc = {};
    Str name = {}, config = {}, state = {};
    b32 local = 0;
    for (i32 i = 2; i < nargs; i++) {
        Str a    = args[i];
        b32 more = i+1 < nargs;
        if (a=="--ssh" && more) {
            rc.ssh = args[++i];
        } else if (a=="--path" && more) {
            rc.path = args[++i];
        } else if (a=="--remote-state" && more) {
            rc.state = args[++i];
        } else if (a=="--remote-config" && more) {
            rc.rconfig = args[++i];
        } else if (a=="--ssh-program" && more) {
            rc.program = args[++i];
        } else if (a=="--scp-program" && more) {
            rc.scp = args[++i];
        } else if (a=="--engines" && more) {
            rc.engines = args[++i];
        } else if (a == "--deploy") {
            rc.deploy = 1;
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a=="--idle" && more) {
            rc.idle = args[++i];
        } else if (a == "--local") {
            local = 1;
        } else if (a=="--endpoint" && more) {
            rc.endpoint = args[++i];
        } else if (!name.len && a.len && a[0]!='-') {
            name = a;
        } else {
            os_print(os, 2, connectusage);
            return 2;
        }
    }
    if (local + (rc.ssh.len || name.len) + (rc.endpoint.len>0) != 1) {
        os_print(os, 2, connectusage);
        return 2;
    }
    // Only cached copies of URL includes: connect runs no engine here
    rc.localstate = state.len ? state : defaultstate(os, &perm);
    Config *cfg   = cliconfig(os, &perm, scratch, config, rc.localstate, CFG_NETNONE);
    if (!cfg) {
        return 2;
    }
    if (name.len) {
        Str dest = cfgget(cfg, "remote", name, "ssh", 0);
        if (!rc.ssh.len && !dest.len) {
            hostsay(os, scratch, "no such remote, or it has no ssh", name);
            return 2;
        }
        rc.ssh   = rc.ssh.len ? rc.ssh : dest;
        rc.path  = rc.path.len ? rc.path : cfgown(cfg, "remote", name, "path");
        rc.state = rc.state.len ? rc.state : cfgget(cfg, "remote", name, "state", 0);
        rc.rconfig = rc.rconfig.len ? rc.rconfig
                                    : cfgget(cfg, "remote", name, "config", 0);
        rc.name  = name;
        rc.deploy |= cfgget(cfg, "remote", name, "deploy", 0) == "auto";
    }
    rc.config = config;
    if (!os_mkdirs(os, scratch, rc.localstate)) {
        Str err = os_lasterr(os, &scratch);
        hostsay(os, scratch, "cannot create the state directory", err);
        return 2;
    }

    Mailbox *out = newmailbox(&perm, 4<<20, 0, 0);
    Remote  *r   = out ? newremote(os, &perm, &rc, out) : 0;
    if (!r || !remotego(r)) {
        hostsay(os, scratch, "cannot start");
        return 2;
    }
    ConnectIn *in = alloc<ConnectIn>(&perm);
    in->os = os;
    in->r  = r;
    in->a  = newarena(os_reserve(os, (iz)1<<22));
    if (!in->a.beg || !os_thread(os, connectstdin, in)) {
        hostsay(os, scratch, "cannot read standard input");
        return 2;
    }

    for (;;) {
        mailwait(out, -1);
        Mail m = mailtake(out);
        for (Record rec; (rec = mailnext(&m)).ok;) {
            if (rec.type == MAIL_QUIT) {
                if (r->state == RS_FAILED) {
                    hostsay(os, scratch, r->why);
                    return 2;
                }
                return 0;
            }
            os_print(os, 1, rec.data);
        }
    }
}

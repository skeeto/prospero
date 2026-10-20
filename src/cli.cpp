// Command line: harness run, and harness --headless
//
//   harness run [options] PROMPT        one turn, printing protocol records
//   harness run [options] --resume SID [PROMPT]
//   harness --headless [options]        protocol v0 on standard I/O
//
// Both drive the engine thread exactly as the GUI will: commands in,
// records out. "run" prints each record as a JSONL line, or readable
// text with --pretty, and exits when the turn ends: 0 when done, 1 on
// error, 3 at the step budget, 130 when cancelled, 2 for usage errors.
//
// This is free and unencumbered software released into the public domain.

static Str cliusage =
"usage: harness run [options] PROMPT\n"
"       harness run [options] --resume SID [PROMPT]\n"
"       harness --headless [options]\n"
"options:\n"
"  -C DIR        working directory (default: the current one)\n"
"  -e ENDPOINT   endpoint URL or configured name\n"
"  -m MODEL      model name\n"
"  -c FILE       configuration file\n"
"  --state DIR   session storage directory\n"
"  --pretty      readable output instead of JSONL (run only)\n";

struct Cli {
    Os      *os;
    Engine  *e;
    Mailbox *out;
    Str      statedir;
    Str      sid;
    b32      pretty;
    b32      headless;
    b32      quitting;
    i64      lastseq;   // the last durable record printed
    i32      exitcode;
    i64      openid;
    Str      prompt;    // for --resume
    i32      block;     // the delta block printed last, -1 if none
    b32      midline;   // pretty output does not end in a newline
    Arena    keep;      // for the session id
    Region  *spare;     // log copies for resyncs
};

static void clicmd(Cli *c, Arena scratch, i32 cmd, i64 id, Str sid, Str key, Str val)
{
    Buf   b(&scratch, 512 + val.len);
    EvOut w = cmdbegin(&b, cmd, id);
    if (sid.len) {
        putkey(&w.json, "sid");
        putstr(&w.json, sid);
    }
    if (key.len) {
        putkey(&w.json, key);
        putstr(&w.json, val);
    }
    enginecmd(c->e, cuttail(evend(&w), 1));
}

static void cliquit(Cli *c, Arena scratch)
{
    if (!c->quitting) {
        c->quitting = 1;
        clicmd(c, scratch, CMD_QUIT, 99, {}, {}, {});
    }
}

static void prettyline(Cli *c)
{
    if (c->midline) {
        os_print(c->os, 1, "\n");
        c->midline = 0;
    }
}

// Indented excerpt of a tool result.
static void prettyexcerpt(Cli *c, Arena scratch, Str text)
{
    Buf b(&scratch, 1024);
    iz  n = 0;
    for (Str rest = text; rest.len; n++) {
        Cut l = cut(rest, '\n');
        rest = l.tail;
        if (n < 12) {
            print(&b, "  ");
            print(&b, l.head.len>200 ? utf8head(l.head, 200) : l.head);
            putbyte(&b, '\n');
        }
    }
    if (n > 12) {
        print(&b, "  ... (");
        print(&b, n - 12);
        print(&b, " more lines)\n");
    }
    os_print(c->os, 1, finish(&b));
}

static void prettyrecord(Cli *c, Arena scratch, EvRec *r)
{
    Os   *os = c->os;
    Json *j  = r->root;
    Buf   b(&scratch, 1024);
    switch (r->ev) {
    case EV_USER:
        prettyline(c);
        print(&b, "> ");
        print(&b, jsonstr(jsonget(j, "text")));
        print(&b, "\n");
        break;
    case EV_DELTA: {
        Str kind  = jsonstr(jsonget(j, "kind"));
        i32 block = (i32)jsonint(jsonget(j, "block"), 0);
        if (kind == "args") {
            return;
        }
        if (block != c->block) {
            prettyline(c);
            if (kind == "think") print(&b, "[thinking] ");
            c->block = block;
        }
        print(&b, jsonstr(jsonget(j, "text")));
        c->midline = 1;
    } break;
    case EV_ASSISTANT:
        c->block = -1;
        return;
    case EV_TOOLSTART: {
        prettyline(c);
        Str     name = jsonstr(jsonget(j, "name"));
        Repair  rp   = repairargs(&scratch, jsonstr(jsonget(j, "args")));
        Str     what = name=="shell" ? jsonstr(jsonget(rp.root, "command")) :
                       jsonstr(jsonget(rp.root, "path"));
        print(&b, "$ ");
        print(&b, name);
        print(&b, ": ");
        print(&b, what.len ? what : jsonstr(jsonget(j, "args")));
        print(&b, "\n");
    } break;
    case EV_TOOLEND:
        prettyline(c);
        os_print(os, 1, concat(&scratch, concat(&scratch, "  [",
                 jsonstr(jsonget(j, "status"))), "]\n"));
        prettyexcerpt(c, scratch, jsonstr(jsonget(j, "text")));
        return;
    case EV_RETRY:
        prettyline(c);
        print(&b, "[retry in ");
        print(&b, jsonint(jsonget(j, "delay_ms"), 0));
        print(&b, " ms: ");
        print(&b, jsonstr(jsonget(j, "msg")));
        print(&b, "]\n");
        break;
    case EV_NUDGE:
        prettyline(c);
        print(&b, "[nudge: ");
        print(&b, jsonstr(jsonget(j, "kind")));
        print(&b, "]\n");
        break;
    case EV_NOTE:
        prettyline(c);
        print(&b, "[note: ");
        print(&b, jsonstr(jsonget(j, "text")));
        print(&b, "]\n");
        break;
    case EV_ANSWER:
        prettyline(c);
        print(&b, "[denied: ");
        print(&b, jsonstr(jsonget(j, "note")));
        print(&b, "]\n");
        break;
    case EV_TURNEND: {
        prettyline(c);
        Str err = jsonstr(jsonget(j, "err"));
        print(&b, "[turn ");
        print(&b, jsonint(jsonget(j, "turn"), 0));
        print(&b, " ended: ");
        print(&b, jsonstr(jsonget(j, "why")));
        if (err.len) {
            print(&b, ": ");
            print(&b, err);
        }
        print(&b, "]\n");
    } break;
    default:
        return;
    }
    os_print(os, 1, finish(&b));
}

static i32 exitcodefor(Str why)
{
    if (why == "done")   return 0;
    if (why == "cancel") return 130;
    if (why == "budget") return 3;
    return 1;
}

// Print one record and follow the run.
static void clirecord(Cli *c, Arena scratch, Str line)
{
    Arena t = scratch;
    EvRec r = evparse(&t, line);
    if (!c->headless && r.seq && r.sid==c->sid) {
        if (r.seq <= c->lastseq) {
            return;  // already printed during a resync
        }
        c->lastseq = r.seq;
    }
    if (c->pretty) {
        prettyrecord(c, t, &r);
    } else {
        os_print(c->os, 1, line);
    }
    if (c->headless) {
        return;
    }

    Json *j = r.root;
    if (r.ev == EV_REPLY) {
        i64 id = jsonint(jsonget(j, "id"), 0);
        if (!jsonint(jsonget(j, "ok"), 0) && id!=99) {
            prettyline(c);
            os_print(c->os, 2, concat(&t, concat(&t, "harness: ",
                     jsonstr(jsonget(j, "err"))), "\n"));
            c->exitcode = 2;
            cliquit(c, t);
            return;
        }
        if ((id==1 || id==c->openid) && !c->sid.len) {
            c->sid = clone(&c->keep, r.sid);
        }
        if (id == c->openid) {
            if (c->prompt.len) {
                clicmd(c, t, CMD_PROMPT, 3, r.sid, "text", c->prompt);
            } else {
                cliquit(c, t);
            }
        }
    } else if (r.ev==EV_TURNEND && r.sid==c->sid) {
        c->exitcode = exitcodefor(jsonstr(jsonget(j, "why")));
        cliquit(c, t);
    }
}

// Durable records lost to a full mailbox come back from the log.
static void cliresync(Cli *c, Arena scratch)
{
    if (c->headless) {
        Buf   b(&scratch, 128);
        EvOut w = evbegin(&b, {}, 0, os_time(c->os), EV_OVERFLOW);
        os_print(c->os, 1, evend(&w));
        return;
    }
    if (!c->sid.len) {
        return;
    }
    Str path = join(&scratch, join(&scratch, join(&scratch, c->statedir,
                    "sessions"), c->sid), "events.jsonl");
    Arena  lines = newarena(c->spare);
    Replay rp    = logreplay(c->os, &lines, scratch, path, c->sid, c->lastseq, LOG_MAX);
    for (iz i = 0; i < rp.lines.len; i++) {
        clirecord(c, scratch, rp.lines[i]);
    }
}

struct StdinReader {
    Os     *os;
    Engine *e;
    Arena   a;
};

static void stdinthread(void *arg)
{
    StdinReader *r   = (StdinReader *)arg;
    iz           cap = 1 << 20;
    u8          *buf = allocbytes(&r->a, cap);
    iz           len = 0;
    b32          skip = 0;  // inside an overlong line
    for (;;) {
        iz n = os_stdin(r->os, buf+len, cap-len);
        if (n <= 0) {
            break;
        }
        len += n;
        iz start = 0;
        for (iz i = len-n; i < len; i++) {
            if (buf[i] != '\n') continue;
            Str line = trim(span(buf+start, buf+i));
            if (!skip && line.len) {
                enginecmd(r->e, line);
            }
            skip  = 0;
            start = i + 1;
        }
        for (iz i = start; i < len; i++) {
            buf[i-start] = buf[i];  // the partial line to the front
        }
        len -= start;
        if (len == cap) {
            len  = 0;
            skip = 1;
        }
    }
    enginecmd(r->e, "{\"cmd\":\"quit\",\"id\":0}");
}

static Str defaultstate(Os *os, Arena *a)
{
    Str base = os_getenv(os, a, "XDG_STATE_HOME");
    if (base.len) {
        return join(a, base, "harness");
    }
    if (HOST_WINDOWS) {
        Str local = os_getenv(os, a, "LOCALAPPDATA");
        if (local.len) {
            return join(a, local, "harness");
        }
    }
    Str home = os_getenv(os, a, "HOME");
    if (!home.len) home = os_getenv(os, a, "USERPROFILE");
    return join(a, join(a, home, ".local/state"), "harness");
}

static Str defaultconfig(Os *os, Arena *a)
{
    Str base = os_getenv(os, a, "XDG_CONFIG_HOME");
    if (base.len) {
        return join(a, join(a, base, "harness"), "harness.ini");
    }
    Str home = os_getenv(os, a, "HOME");
    if (home.len) {
        return join(a, join(a, home, ".config/harness"), "harness.ini");
    }
    Str appdata = os_getenv(os, a, "APPDATA");
    if (appdata.len) {
        return join(a, join(a, appdata, "harness"), "harness.ini");
    }
    return {};
}

static i32 climain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs, b32 headless)
{
    Cli c = {};
    c.os       = os;
    c.headless = headless;
    c.block    = -1;

    Str dir = {}, endpoint = {}, model = {}, config = {}, state = {}, resume = {};
    Str prompt = {};
    for (i32 i = 2; i < nargs; i++) {
        Str a = args[i];
        b32 more = i+1 < nargs;
        if (a=="-C" && more) {
            dir = args[++i];
        } else if (a=="-e" && more) {
            endpoint = args[++i];
        } else if (a=="-m" && more) {
            model = args[++i];
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a=="--resume" && more && !headless) {
            resume = args[++i];
        } else if (a=="--pretty" && !headless) {
            c.pretty = 1;
        } else if (!headless && !prompt.data && (!a.len || a[0]!='-')) {
            prompt = a;
        } else {
            os_print(os, 2, cliusage);
            return 2;
        }
    }
    if (!headless && !resume.len && !prompt.len) {
        os_print(os, 2, cliusage);
        return 2;
    }

    Ini *ini = alloc<Ini>(&perm);
    Str  cfg = config.len ? config : defaultconfig(os, &perm);
    if (cfg.len && (config.len || os_stat(os, scratch, cfg).type==STAT_FILE)) {
        if (!iniload(ini, os, &perm, cfg) && config.len) {
            os_print(os, 2, "harness: cannot read the configuration file\n");
            return 2;
        }
        for (iz i = 0; i < ini->diags.len; i++) {
            Buf b(&scratch, 256);
            print(&b, "harness: ");
            print(&b, ini->diags[i]);
            putbyte(&b, '\n');
            os_print(os, 2, finish(&b));
        }
    }

    c.statedir = state.len ? state : defaultstate(os, &perm);
    Str sessions = join(&perm, c.statedir, "sessions");
    if (!os_mkdirs(os, scratch, sessions)) {
        os_print(os, 2, "harness: cannot create the state directory\n");
        return 2;
    }

    c.spare = os_reserve(os, sizeof(void *)==8 ? (iz)1<<32 : (iz)1<<27);
    c.out   = c.spare ? newmailbox(&perm, 4<<20, 0, 0) : 0;
    c.e   = c.out ? newengine(os, &perm, ini, c.statedir, c.out) : 0;
    if (!c.e || !enginestart(c.e)) {
        os_print(os, 2, "harness: cannot start the engine\n");
        return 2;
    }

    if (headless) {
        StdinReader *r = alloc<StdinReader>(&perm);
        r->os = os;
        r->e  = c.e;
        r->a  = newarena(os_reserve(os, (iz)1<<24));
        if (!r->a.beg || !os_thread(os, stdinthread, r)) {
            os_print(os, 2, "harness: cannot read standard input\n");
            return 2;
        }
    } else if (resume.len) {
        c.openid = 2;
        c.prompt = prompt;
        Arena a = scratch;
        Buf   b(&a, 512);
        EvOut w = cmdbegin(&b, CMD_OPEN, c.openid);
        putkey(&w.json, "sid");
        putstr(&w.json, resume);
        putkey(&w.json, "from");
        putint(&w.json, -1);
        if (model.len) {
            putkey(&w.json, "model");
            putstr(&w.json, model);
        }
        if (endpoint.len) {
            putkey(&w.json, "endpoint");
            putstr(&w.json, endpoint);
        }
        enginecmd(c.e, cuttail(evend(&w), 1));
    } else {
        Arena a   = scratch;
        Str   cwd = os_realpath(os, &a, dir.len ? dir : Str("."));
        Buf   b(&a, 512 + prompt.len);
        EvOut w = cmdbegin(&b, CMD_NEW, 1);
        putkey(&w.json, "cwd");
        putstr(&w.json, cwd.len ? cwd : dir);
        putkey(&w.json, "model");
        putstr(&w.json, model);
        putkey(&w.json, "endpoint");
        putstr(&w.json, endpoint);
        putkey(&w.json, "prompt");
        putstr(&w.json, prompt);
        enginecmd(c.e, cuttail(evend(&w), 1));
    }

    c.keep = perm;
    for (;;) {
        mailwait(c.out, -1);
        Mail m = mailtake(c.out);
        if (m.overflow) {
            cliresync(&c, scratch);
        }
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.type == MAIL_QUIT) {
                prettyline(&c);
                return c.exitcode;
            }
            clirecord(&c, scratch, r.data);
        }
    }
}

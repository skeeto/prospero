// Platform integration tests for processes and HTTP
// $ harness --selftest            (processes, after the M0 tests)
// $ harness --selftest URL        (processes and HTTP, against
//                                  tests/fake_provider.py at URL)
//
// Processes run through the agent's shell (findshell): /bin/sh, or
// busybox sh on Windows. With only cmd.exe, the shell-specific checks
// are skipped.
//
// This is free and unencumbered software released into the public domain.

struct ProcWatch {
    i32 code;
    i32 signal;
    i64 total;
    i64 exitat;  // ticks, 0 until it happens
    i64 eofat;
    b32 limit;
    i32 tails;
};

// Collect records for one process until it exited and its output closed,
// or until ms pass. Returns 1 if both happened.
static b32 procwatch(Os *os, Mailbox *mb, u64 id, ProcWatch *w, i32 ms)
{
    i64 deadline = os_ticks(os) + (i64)ms*1000;
    while (!w->exitat || !w->eofat) {
        i64 left = deadline - os_ticks(os);
        if (left <= 0) {
            return 0;
        }
        mailwait(mb, (i32)(left/1000 + 1));
        Mail m = mailtake(mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.id != id) continue;
            i64 now = os_ticks(os);
            switch (r.type) {
            case PROC_TAIL:  w->tails++;                                     break;
            case PROC_LIMIT: w->limit = 1;                                   break;
            case PROC_EXIT:  w->exitat = now;
                             w->code   = get32(r.data);
                             w->signal = get32(cuthead(r.data, 4));          break;
            case PROC_EOF:   w->eofat = now;
                             w->total = get64(r.data);                       break;
            }
        }
    }
    return 1;
}

struct ProcTest {
    Selftest *t;
    Os       *os;
    Mailbox  *mb;
    ToolEnv   te;
    Slice<Str> env;
    Str       dir;
    u64       id;
};

static Proc *spawnsh(ProcTest *p, Arena scratch, Str command, Str out, i64 max)
{
    ShellCmd sc = shellcmd(&scratch, &p->te, command);
    Spawn    sp = {};
    sp.argv     = sc.argv;
    sp.cmdline  = sc.cmdline;
    sp.cwd      = p->dir;
    sp.env      = p->env;
    sp.outpath  = out;
    sp.maxbytes = max;
    sp.mbox     = p->mb;
    sp.id       = ++p->id;
    return os_spawn(p->os, &sp, scratch);
}

static void testspawn(ProcTest *p, Arena scratch)
{
    Selftest *t   = p->t;
    Os       *os  = p->os;
    Str       out = join(&scratch, p->dir, "echo.log");
    Proc     *pr  = spawnsh(p, scratch, "echo hello; echo oops 1>&2; exit 3", out, 1<<20);
    if (!check(t, pr!=0, "os_spawn of the shell")) {
        say(os, os_lasterr(os, &scratch));
        return;
    }
    ProcWatch w = {};
    check(t, procwatch(os, p->mb, p->id, &w, 5000), "echo exits and closes its output");
    check(t, w.code==3 && !w.signal, "the exit code arrives");
    os_kill(os, pr, 1);
    os_procfree(os, pr);
    Read rd = os_read(os, &scratch, out, 1<<20);
    Str  got = cleanoutput(&scratch, rd.data);
    check(t, got=="hello\noops\n", "stdout and stderr merge into the file");
    check(t, w.total == rd.size, "PROC_EOF counts every byte");

    // A missing program is reported, not hung on
    Spawn sp = {};
    Str argv[] = {"harness-no-such-program-x9"};
    sp.argv     = {argv, 1, 1};
    sp.cwd      = p->dir;
    sp.env      = p->env;
    sp.outpath  = join(&scratch, p->dir, "missing.log");
    sp.maxbytes = 1<<20;
    sp.mbox     = p->mb;
    sp.id       = ++p->id;
    pr = os_spawn(os, &sp, scratch);
    if (pr) {
        ProcWatch m = {};
        check(t, procwatch(os, p->mb, p->id, &m, 5000), "a missing program ends");
        check(t, m.code==127 || m.code==9009 || m.code==1, "a missing program fails");
        os_kill(os, pr, 1);
        os_procfree(os, pr);
    } else {
        check(t, 1, "a missing program fails at once");
    }
    say(os, "spawn done");
}

static void testbulk(ProcTest *p, Arena scratch)
{
    Selftest *t   = p->t;
    Os       *os  = p->os;
    Str       out = join(&scratch, p->dir, "bulk.log");
    i64       n   = (i64)100 << 20;
    i64       t0  = os_ticks(os);
    Proc     *pr  = spawnsh(p, scratch, "head -c 104857600 /dev/zero", out, (i64)1<<30);
    if (!check(t, pr!=0, "os_spawn for bulk output")) return;
    ProcWatch w = {};
    check(t, procwatch(os, p->mb, p->id, &w, 60000), "100 MiB of output drains");
    i64 dt = os_ticks(os) - t0;
    check(t, w.code==0 && w.total==n, "100 MiB counted");
    check(t, os_stat(os, scratch, out).size == n, "100 MiB written to the file");
    os_kill(os, pr, 1);
    os_procfree(os, pr);
    report(os, scratch, "100 MiB of output in", millis(dt), 0, " ms");
    os_remove(os, scratch, out);

    // Past maxbytes the file stops growing, but the pipe keeps draining
    pr = spawnsh(p, scratch, "head -c 3000000 /dev/zero", out, 1000000);
    if (!check(t, pr!=0, "os_spawn for limited output")) return;
    ProcWatch l = {};
    check(t, procwatch(os, p->mb, p->id, &l, 30000), "limited output drains");
    check(t, l.limit, "PROC_LIMIT arrives");
    check(t, l.total==3000000 && os_stat(os, scratch, out).size==1000000,
          "the file stops at maxbytes");
    os_kill(os, pr, 1);
    os_procfree(os, pr);
    say(os, "bulk output done");
}

static void testholder(ProcTest *p, Arena scratch)
{
    Selftest *t   = p->t;
    Os       *os  = p->os;
    Str       out = join(&scratch, p->dir, "holder.log");
    i64       t0  = os_ticks(os);
    Proc     *pr  = spawnsh(p, scratch, "sleep 100 & echo started", out, 1<<20);
    if (!check(t, pr!=0, "os_spawn with a background child")) return;

    // The shell exits; the child holds the output open
    ProcWatch w = {};
    procwatch(os, p->mb, p->id, &w, 2000);
    check(t, w.exitat && !w.eofat, "the shell exits while its child holds the output");

    // The engine's bounded drain, then the kill
    while (os_ticks(os)-t0 < 2000000) {
        procwatch(os, p->mb, p->id, &w, 50);
    }
    os_kill(os, pr, 1);
    b32 done = procwatch(os, p->mb, p->id, &w, 1000);
    i64 dt   = os_ticks(os) - t0;
    check(t, done, "killing the tree closes the output");
    check(t, dt < 2500000, "the call returns within 2.5 s");
    os_procfree(os, pr);
    report(os, scratch, "background child: returned after", millis(dt), 0, " ms");
}

static void testladder(ProcTest *p, Arena scratch)
{
    Selftest *t  = p->t;
    Os       *os = p->os;
    Str       out = join(&scratch, p->dir, "ladder.log");

    // A plain process dies of TERM
    Proc *pr = spawnsh(p, scratch, "exec sleep 100", out, 1<<20);
    if (!check(t, pr!=0, "os_spawn sleep")) return;
    ProcWatch w = {};
    procwatch(os, p->mb, p->id, &w, 300);
    i64 t0 = os_ticks(os);
    os_kill(os, pr, 0);
    check(t, procwatch(os, p->mb, p->id, &w, 2000), "a soft kill ends sleep");
    check(t, os_ticks(os)-t0 < 500000, "a soft kill acts at once");
    os_kill(os, pr, 1);
    os_procfree(os, pr);

    if (HOST_WINDOWS) {
        say(os, "kill ladder done (soft is hard on Windows)");
        return;
    }

    // One that ignores TERM needs KILL
    pr = spawnsh(p, scratch, "trap '' TERM; sleep 100; echo survived", out, 1<<20);
    if (!check(t, pr!=0, "os_spawn TERM-proof")) return;
    ProcWatch v = {};
    procwatch(os, p->mb, p->id, &v, 300);
    os_kill(os, pr, 0);
    procwatch(os, p->mb, p->id, &v, 500);
    check(t, !v.exitat, "TERM is ignored");
    t0 = os_ticks(os);
    os_kill(os, pr, 1);
    check(t, procwatch(os, p->mb, p->id, &v, 2000), "KILL ends it");
    check(t, v.signal==9 || v.code==-1, "it died of KILL");
    check(t, os_ticks(os)-t0 < 500000, "KILL acts at once");
    os_procfree(os, pr);
    say(os, "kill ladder done");
}

static void testoverhead(ProcTest *p, Arena scratch)
{
    Selftest *t   = p->t;
    Os       *os  = p->os;
    Str       out = join(&scratch, p->dir, "true.log");
    i64       best = I64MAX;
    i64       sum  = 0;
    i32       n    = 20;
    for (i32 i = 0; i < n; i++) {
        Spawn    sp     = {};
        ShellCmd sc     = shellcmd(&scratch, &p->te, "exit 0");
        Str      argv[] = {"true"};
        sp.argv     = HOST_WINDOWS ? sc.argv : Slice<Str>{argv, 1, 1};
        sp.cmdline  = sc.cmdline;
        sp.cwd      = p->dir;
        sp.env      = p->env;
        sp.outpath  = out;
        sp.maxbytes = 1<<20;
        sp.mbox     = p->mb;
        sp.id       = ++p->id;
        i64   t0 = os_ticks(os);
        Proc *pr = os_spawn(os, &sp, scratch);
        if (!pr) break;
        ProcWatch w = {};
        procwatch(os, p->mb, p->id, &w, 5000);
        i64 dt = w.exitat - t0;
        best = dt<best ? dt : best;
        sum += dt;
        os_kill(os, pr, 1);
        os_procfree(os, pr);
    }
    report(os, scratch, "spawn to exit of true: best", millis(best), 2, " ms");
    report(os, scratch, "spawn to exit of true: mean", millis(sum/n), 2, " ms");
    if (!HOST_WINDOWS) {
        check(t, sum/n < 5000, "spawn overhead under 5 ms");
    }
}

static void testprocs(Selftest *t, Arena *perm, Arena scratch, Str dir)
{
    Os       *os = t->os;
    ProcTest *p  = alloc<ProcTest>(perm);
    p->t   = t;
    p->os  = os;
    p->mb  = newmailbox(perm, 1<<20, 0, 0);
    p->dir = dir;
    Str exe = os_exepath(os, perm);
    p->te  = findshell(os, perm, scratch, parentdir(exe), HOST_WINDOWS);
    p->env = shellenv(perm, os_environ(os, perm), HOST_WINDOWS);
    if (!check(t, p->mb!=0, "newmailbox")) return;
    {
        Buf b(&scratch, 256);
        print(&b, "shell ");
        print(&b, p->te.shell);
        say(os, finish(&b));
    }
    if (p->te.shellkind == SHELL_CMD) {
        say(os, "no POSIX shell: process tests skipped");
        return;
    }
    testspawn(p, scratch);
    testbulk(p, scratch);
    testholder(p, scratch);
    testladder(p, scratch);
    testoverhead(p, scratch);
}


// HTTP

struct HttpWatch {
    i32 status;
    i32 err;
    Str errtext;
    i64 bytes;
    i64 headat;
    i64 doneat;
};

static b32 httpwatch(Os *os, Mailbox *mb, u64 id, HttpWatch *w, i32 ms, Arena *a)
{
    i64 deadline = os_ticks(os) + (i64)ms*1000;
    while (!w->doneat) {
        i64 left = deadline - os_ticks(os);
        if (left <= 0) {
            return 0;
        }
        mailwait(mb, (i32)(left/1000 + 1));
        Mail m = mailtake(mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.id != id) continue;
            switch (r.type) {
            case HTTP_HEAD: w->status = get32(r.data);
                            w->headat = os_ticks(os);       break;
            case HTTP_BODY: w->bytes += r.data.len;         break;
            case HTTP_DONE: w->err     = get32(r.data);
                            w->errtext = clone(a, cuthead(r.data, 4));
                            w->doneat  = os_ticks(os);      break;
            }
        }
    }
    return 1;
}

static void testhttp(Selftest *t, Arena *perm, Arena scratch, Str base)
{
    Os      *os = t->os;
    Mailbox *mb = newmailbox(perm, 1<<20, 0, 0);
    u64      id = 1000;
    if (!check(t, mb!=0, "newmailbox")) return;

    // Throughput: 100 MiB of body
    {
        HttpReq r = {};
        r.method    = "GET";
        r.url       = concat(&scratch, base, "/fake/bytes?n=104857600");
        r.connectms = 5000;
        r.maxbytes  = (i64)200 << 20;
        r.mbox      = mb;
        r.id        = ++id;
        i64   t0 = os_ticks(os);
        Http *h  = os_http(os, &r, scratch);
        if (check(t, h!=0, "os_http")) {
            HttpWatch w = {};
            check(t, httpwatch(os, mb, id, &w, 60000, &scratch), "100 MiB arrives");
            i64 dt = os_ticks(os) - t0;
            check(t, w.status==200 && !w.err && w.bytes==(i64)100<<20,
                  "100 MiB body intact");
            os_httpfree(os, h);
            report(os, scratch, "100 MiB over HTTP in", millis(dt), 0, " ms");
        }
    }

    // Cancel while the server stalls: before the head, after a POST body
    // (the content provider path), and in the middle of a body
    Str stalls[] = {"/fake/stall", "/fake/stallbody"};
    for (iz k = 0; k < countof(stalls); k++) {
        for (i32 post = 0; post < 2; post++) {
            HttpReq r = {};
            r.method    = post ? Str("POST") : Str("GET");
            r.url       = concat(&scratch, base, stalls[k]);
            r.body      = post ? Str("{\"x\":1}") : Str{};
            r.headers   = "Content-Type: application/json\r\n";
            r.connectms = 5000;
            r.maxbytes  = 1<<20;
            r.mbox      = mb;
            r.id        = ++id;
            Http *h = os_http(os, &r, scratch);
            if (!check(t, h!=0, "os_http")) continue;
            HttpWatch w = {};
            httpwatch(os, mb, id, &w, 300, &scratch);
            b32 early = w.doneat != 0;
            i64 t0 = os_ticks(os);
            os_httpcancel(os, h);
            i64 ret = os_ticks(os) - t0;
            b32 ok = httpwatch(os, mb, id, &w, 2000, &scratch);
            i64 dt = w.doneat - t0;
            Buf what(&scratch, 128);
            print(&what, "cancel during ");
            print(&what, stalls[k]);
            print(&what, post ? Str(" (POST)") : Str(" (GET)"));
            Str label = finish(&what);
            check(t, !early, concat(&scratch, label, ": the stall holds"));
            check(t, ret < 20000, concat(&scratch, label, ": os_httpcancel returns at once"));
            check(t, ok && w.err==HTTP_ECANCEL, concat(&scratch, label, ": reports a cancel"));
            check(t, ok && dt<100000, concat(&scratch, label, ": done within 100 ms"));
            Str done = concat(&scratch, label, ": done after");
            report(os, scratch, done, millis(dt), 2, " ms");
            os_httpfree(os, h);
        }
    }

    // A refused connection fails fast
    {
        HttpReq r = {};
        r.method    = "GET";
        r.url       = "http://127.0.0.1:9/";
        r.connectms = 2000;
        r.maxbytes  = 1<<20;
        r.mbox      = mb;
        r.id        = ++id;
        Http *h = os_http(os, &r, scratch);
        if (check(t, h!=0, "os_http")) {
            HttpWatch w = {};
            check(t, httpwatch(os, mb, id, &w, 5000, &scratch) && w.err==HTTP_ECONNECT,
                  "a refused connection reports HTTP_ECONNECT");
            os_httpfree(os, h);
        }
    }
    say(os, "http done");
}

static i32 selftestio(Os *os, Arena *perm, Arena scratch, Str url)
{
    Selftest t = {};
    t.os = os;
    i64 t0 = os_ticks(os);

    Arena a = scratch;
    Buf   name(&a, 64);
    print(&name, "harness-selftest-io-");
    printhex(&name, os_random(os), 16);
    Str dir = join(&a, tempdir(os, &a), finish(&name));
    check(&t, os_mkdirs(os, a, dir), "os_mkdirs for the process tests");
    Str real = os_realpath(os, &a, dir);
    check(&t, real.len>0, "os_realpath of a directory");
    if (real.len) dir = real;

    testprocs(&t, perm, a, dir);
    if (url.len) {
        testhttp(&t, perm, a, url);
    }
    removeall(os, a, dir);

    Buf b(&scratch, 128);
    print(&b, (i64)t.checks);
    print(&b, " io checks, ");
    print(&b, (i64)t.failed);
    print(&b, " failed, ");
    printfloat(&b, millis(os_ticks(os) - t0), 0);
    print(&b, " ms");
    say(os, finish(&b));
    return t.failed ? 1 : 0;
}

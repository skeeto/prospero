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
    i64 data;    // PROC_DATA bytes, and their hash in order
    u64 hash;
    Buf keep;    // PROC_DATA bytes, if given room
};

static u64 hashmore(u64 h, Str s)
{
    for (iz i = 0; i < s.len; i++) {
        h = (h ^ s[i]) * 1111111111111111111u;
    }
    return h;
}

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
            case PROC_DATA:  w->data += r.data.len;
                             w->hash  = hashmore(w->hash, r.data);
                             if (w->keep.a && w->keep.len+r.data.len<=w->keep.cap) {
                                 print(&w->keep, r.data);
                             }                                               break;
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
        say(os, "kill ladder done (soft is CTRL_BREAK on Windows)");
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

static b32 i64less(i64 *a, i64 *b)
{
    return *a < *b;
}

struct Timing {
    i64 best;
    i64 median;
    i64 mean;
};

// Spawn n times and time each until exit, in ticks. The median shrugs
// off the outliers of a machine that is busy with other work.
static Timing timespawn(ProcTest *p, Arena scratch, Slice<Str> argv,
                        Str cmdline, i32 n)
{
    Os     *os  = p->os;
    i64     sum = 0;
    Timing  r   = {};
    i64    *dts = alloc<i64>(&scratch, n);
    for (i32 i = 0; i < n; i++) {
        Spawn sp = {};
        sp.argv     = argv;
        sp.cmdline  = cmdline;
        sp.cwd      = p->dir;
        sp.env      = p->env;
        // A fresh file each time, as tool calls get: truncating one just
        // written makes ext4 flush it, which costs milliseconds on an SD card
        Buf name(&scratch, 32);
        print(&name, "spawn");
        print(&name, (i64)i);
        print(&name, ".log");
        sp.outpath  = join(&scratch, p->dir, finish(&name));
        sp.maxbytes = 1<<20;
        sp.mbox     = p->mb;
        sp.id       = ++p->id;
        i64   t0 = os_ticks(os);
        Proc *pr = os_spawn(os, &sp, scratch);
        if (!pr) {
            return {};
        }
        ProcWatch w = {};
        procwatch(os, p->mb, p->id, &w, 5000);
        dts[i] = w.exitat - t0;
        sum   += dts[i];
        os_kill(os, pr, 1);
        os_procfree(os, pr);
    }
    sort(dts, n, i64less, scratch);
    r.best   = dts[0];
    r.median = dts[n/2];
    r.mean   = sum / n;
    return r;
}

static void reporttiming(Os *os, Arena scratch, Str what, Timing t)
{
    Buf b(&scratch, 128);
    print(&b, what);
    print(&b, ": best ");
    printfloat(&b, millis(t.best), 2);
    print(&b, " ms, median ");
    printfloat(&b, millis(t.median), 2);
    print(&b, " ms, mean ");
    printfloat(&b, millis(t.mean), 2);
    print(&b, " ms");
    say(os, finish(&b));
}

// The cost of spawning through the platform: a program that exits at
// once, and on POSIX a missing one, which times the reaper alone (its
// start, the failed exec, the status report) without any tool.
static void testoverhead(ProcTest *p, Arena scratch)
{
    Selftest *t  = p->t;
    Os       *os = p->os;

    ShellCmd   sc   = shellcmd(&scratch, &p->te, "exit 0");
    Str        tr[] = {"true"};
    Slice<Str> argv = HOST_WINDOWS ? sc.argv : Slice<Str>{tr, 1, 1};
    Timing     tt   = timespawn(p, scratch, argv, sc.cmdline, 21);
    reporttiming(os, scratch, "spawn to exit of true", tt);
    if (HOST_WINDOWS) {
        return;
    }

    Str    none[] = {"/harness-selftest/no-such-program"};
    Timing rt     = timespawn(p, scratch, {none, 1, 1}, {}, 21);
    reporttiming(os, scratch, "reaper overhead", rt);
    // The target is for the executables people run: optimized, without
    // sanitizers, which slow the reaper's start several times over
    #if !defined(__OPTIMIZE__) || defined(__SANITIZE_THREAD__) || \
            defined(__SANITIZE_ADDRESS__)
    b32 measured = 0;
    #elif defined(__has_feature)
    b32 measured = !__has_feature(thread_sanitizer) && !__has_feature(address_sanitizer);
    #else
    b32 measured = 1;
    #endif
    if (measured) {
        check(t, rt.median>0 && rt.median<5000, "spawn overhead under 5 ms");
    }
}

static Proc *spawnio(ProcTest *p, Arena scratch, Slice<Str> argv, Str cmdline,
                     Str errpath)
{
    Spawn sp = {};
    sp.argv    = argv;
    sp.cmdline = cmdline;
    sp.cwd     = p->dir;
    sp.env     = p->env;
    sp.outpath = errpath;
    sp.mbox    = p->mb;
    sp.id      = ++p->id;
    return os_spawnio(p->os, &sp, scratch);
}

// Bidirectional children: input through the writer thread, which never
// holds up the caller, output as records, errors in their own file.
static void testspawnio(ProcTest *p, Arena scratch)
{
    Selftest *t  = p->t;
    Os       *os = p->os;

    // cat: everything written comes back, in order, once input closes
    Str      errs = join(&scratch, p->dir, "io.err");
    ShellCmd sc   = shellcmd(&scratch, &p->te, "cat; echo done 1>&2");
    Proc    *pr   = spawnio(p, scratch, sc.argv, sc.cmdline, errs);
    if (!check(t, pr!=0, "os_spawnio of cat")) {
        say(os, os_lasterr(os, &scratch));
        return;
    }
    iz  block = 1<<16;
    u8 *buf   = allocbytes(&scratch, block);
    u64 want  = 0;
    i64 t0    = os_ticks(os);
    b32 ok    = 1;
    for (i32 n = 0; n < 128; n++) {
        for (iz i = 0; i < block; i++) {
            buf[i] = (u8)(n*7 + i*13 + (i>>8));
        }
        want = hashmore(want, {buf, block});
        ok &= os_procwrite(os, pr, {buf, block});
    }
    i64 dt = os_ticks(os) - t0;
    ok &= os_procwrite(os, pr, {});
    check(t, ok, "os_procwrite queues 8 MiB, then the close");
    check(t, dt < 1000000, "os_procwrite never waits on the child");
    check(t, !os_procwrite(os, pr, "late"), "os_procwrite after the close fails");
    ProcWatch w = {};
    check(t, procwatch(os, p->mb, p->id, &w, 30000), "cat sees EOF and exits");
    check(t, w.code==0 && w.data==(i64)128*block && w.total==w.data,
          "8 MiB come back as PROC_DATA");
    check(t, w.hash == want, "in order and intact");
    os_kill(os, pr, 1);
    os_procfree(os, pr);
    Read rd = os_read(os, &scratch, errs, 1<<10);
    check(t, cleanoutput(&scratch, rd.data)=="done\n", "errors go to their file");
    report(os, scratch, "8 MiB through cat in", millis(os_ticks(os)-t0), 0, " ms");

    // A child that does not read: input breaks once it exits
    sc = shellcmd(&scratch, &p->te, "exit 0");
    pr = spawnio(p, scratch, sc.argv, sc.cmdline, {});
    if (!check(t, pr!=0, "os_spawnio of a child that does not read")) return;
    ProcWatch x = {};
    check(t, procwatch(os, p->mb, p->id, &x, 5000), "it exits");
    b32 broke = 0;
    for (i32 n = 0; n<200 && !broke; n++) {
        broke = !os_procwrite(os, pr, {buf, block});
        mailwait(p->mb, 10);
    }
    check(t, broke, "writing to a child gone fails soon");
    os_kill(os, pr, 1);
    os_procfree(os, pr);

    // The engine itself, headless, as the GUI's --engine=subprocess runs
    // it, with a configuration file of its own: by default it would read
    // the user's, and seed it on first run
    Str exe = os_exepath(os, &scratch);
    Str st  = join(&scratch, p->dir, "engine-state");
    Str ini = join(&scratch, p->dir, "engine.ini");
    os_write(os, scratch, ini, "# selftest: defaults only\n");
    Str argv[] = {exe, "--headless", "--state", st, "-c", ini};
    pr = spawnio(p, scratch, {argv, countof(argv), countof(argv)}, {}, {});
    if (!check(t, pr!=0, "os_spawnio of harness --headless")) return;
    os_procwrite(os, pr, "{\"cmd\":\"hello\",\"proto\":0,\"id\":1}\n");
    os_procwrite(os, pr, "{\"cmd\":\"quit\",\"mode\":\"stop\",\"id\":2}\n");
    ProcWatch e = {};
    e.keep = Buf(&scratch, 1<<16);
    check(t, procwatch(os, p->mb, p->id, &e, 10000), "the engine answers and quits");
    Str got = finish(&e.keep);
    check(t, e.code==0, "the engine exits cleanly");
    check(t, find(got, "\"ev\":\"hello\"")>0, "the engine says hello");
    check(t, find(got, "\"ev\":\"reply\",\"id\":2,\"ok\":1")>0, "the engine replies to quit");
    os_kill(os, pr, 1);
    os_procfree(os, pr);
    say(os, "spawnio done");
}

// Real paths, and the guard on the real file system: a link inside the
// workspace that leads out is caught. The shell makes the links: ln -s,
// or on Windows junctions through cmd (symlinks need a privilege).
static void testrealpaths(ProcTest *p, Arena scratch)
{
    Selftest *t   = p->t;
    Os       *os  = p->os;
    b32       win = HOST_WINDOWS;
    Str       ws  = join(&scratch, p->dir, "ws");
    Str       sub = join(&scratch, ws, "sub");
    Str       out = join(&scratch, p->dir, "outside");
    Str       sec = join(&scratch, out, "secret");
    os_mkdirs(os, scratch, sub);
    os_mkdirs(os, scratch, out);
    os_write(os, scratch, sec, "x");
    Str cmd = win ?
        Str("cd ws && cmd /c mklink /J link '..\\outside' && cmd /c mklink /J inner sub") :
        Str("cd ws && ln -s ../outside link && ln -s sub inner && ln -s nowhere dangling");
    Str   log = join(&scratch, p->dir, "links.log");
    Proc *pr  = spawnsh(p, scratch, cmd, log, 1<<16);
    ProcWatch w = {};
    if (!check(t, pr && procwatch(os, p->mb, p->id, &w, 10000) && !w.code,
               "links made by the shell")) {
        say(os, os_read(os, &scratch, log, 1<<12).data);
        if (pr) os_procfree(os, pr);
        return;
    }
    os_kill(os, pr, 1);
    os_procfree(os, pr);

    Str wsreal  = os_realpath(os, &scratch, ws);
    Str outreal = os_realpath(os, &scratch, out);
    Str secret  = os_realpath(os, &scratch, join(&scratch, ws, "link/secret"));
    check(t, wsreal.len && outreal.len, "os_realpath of directories");
    check(t, secret.data && normpath(&scratch, outreal, secret, win)==
                            normpath(&scratch, outreal, "secret", win),
          "os_realpath follows a link");
    Str inner = os_realpath(os, &scratch, join(&scratch, ws, "inner"));
    check(t, inner.data && normpath(&scratch, wsreal, inner, win)==
                           normpath(&scratch, wsreal, "sub", win),
          "os_realpath follows a link within");
    check(t, !os_realpath(os, &scratch, join(&scratch, ws, "missing")).data,
          "os_realpath of a missing path is null");
    if (!win) {
        check(t, !os_realpath(os, &scratch, join(&scratch, ws, "dangling")).data,
              "os_realpath of a dangling link is null");
    }

    Guard     g  = newguard(os, &scratch, wsreal, {}, {}, {}, win);
    GuardPath gp = guardpath(os, &scratch, &g, "link/secret", 1);
    check(t, !gp.inside && gp.diverged, "the guard catches a link leading out");
    gp = guardpath(os, &scratch, &g, "link/new.txt", 0);
    check(t, !gp.inside && gp.diverged, "the guard catches a new file through it");
    gp = guardpath(os, &scratch, &g, "inner/new.txt", 0);
    check(t, gp.inside, "the guard allows a link within");
    gp = guardpath(os, &scratch, &g, "../outside/secret", 1);
    check(t, !gp.inside && !gp.diverged, "the guard denies a path outside");
    GuardHint h = guardhint(os, &scratch, &g, gp.path, 1, 0);
    check(t, !h.path.data, "no hint where nothing matches");

    // The directory listing skips links, so they go first
    Str links[] = {"link", "inner", "dangling"};
    for (iz i = 0; i < countof(links); i++) {
        Str link = join(&scratch, ws, links[i]);
        os_remove(os, scratch, link);
    }
    say(os, "real paths done");
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
    testspawnio(p, scratch);
    testrealpaths(p, scratch);
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

// Session locks exclude even within one process, since each lock is its
// own open file (flock) or handle (sharing).
static void testlocks(Selftest *t, Arena scratch, Str dir)
{
    Os *os   = t->os;
    Str path = join(&scratch, dir, "lock");
    check(t, !os_lockheld(os, scratch, path), "a missing lock file is not held");
    iz lock = os_lockfile(os, scratch, path);
    check(t, lock >= 0, "os_lockfile takes a lock");
    check(t, os_lockheld(os, scratch, path), "os_lockheld sees it");
    i64 t0 = os_ticks(os);
    check(t, os_lockfile(os, scratch, path) < 0, "a second lock fails");
    check(t, os_ticks(os)-t0 < 100000, "and fails at once");
    if (lock >= 0) {
        os_unlockfile(os, lock);
    }
    check(t, !os_lockheld(os, scratch, path), "os_unlockfile releases it");
    lock = os_lockfile(os, scratch, path);
    check(t, lock >= 0, "the lock is taken again");
    if (lock >= 0) {
        os_unlockfile(os, lock);
    }
    say(os, "locks done");
}

// Local endpoints: a listener, connections both ways, ends and breaks.

struct ConnWatch {
    Conn *c;       // a connection accepted, from CONN_NEW
    u64   id;
    Buf   got;     // its data, in order
    b32   eof;
};

// Collect records for one connection until the condition holds or ms
// pass. Returns 1 if it held. Another connection's records are dropped.
static b32 connwatch(Os *os, Mailbox *mb, ConnWatch *w, i32 ms, b32 (*done)(ConnWatch *))
{
    i64 deadline = os_ticks(os) + (i64)ms*1000;
    while (!done(w)) {
        i64 left = deadline - os_ticks(os);
        if (left <= 0) {
            return 0;
        }
        mailwait(mb, (i32)(left/1000 + 1));
        Mail m = mailtake(mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.type==CONN_NEW && !w->c) {
                copybytes(&w->c, r.data.data, sizeof(w->c));
                w->id = r.id;
            } else if (r.type==CONN_DATA && r.id==w->id) {
                print(&w->got, r.data);
            } else if (r.type==CONN_EOF && r.id==w->id) {
                w->eof = 1;
            }
        }
    }
    return 1;
}

static b32 hasconn(ConnWatch *w) { return w->c != 0; }
static b32 has10(ConnWatch *w)   { return w->got.len >= 10; }
static b32 haseof(ConnWatch *w)  { return w->eof; }

static void testendpoints(Selftest *t, Arena *perm, Arena scratch, Str dir)
{
    Os *os    = t->os;
    Str state = join(&scratch, dir, "state-with-a-long-name-to-go-through-a-link");
    os_mkdirs(os, scratch, state);
    Str ep = os_endpoint(os, &scratch, state, "selftest");
    if (!check(t, ep.len>0, "os_endpoint")) {
        return;
    }
    Str again = os_endpoint(os, &scratch, state, "selftest");
    check(t, ep==again, "os_endpoint is stable");
    check(t, !os_listening(os, scratch, ep), "nobody listens yet");

    Mailbox  *sm = newmailbox(perm, 1<<16, 0, 0);   // the server's
    Mailbox  *cm = newmailbox(perm, 1<<16, 0, 0);   // the client's
    check(t, !os_connect(os, scratch, ep, cm, 1), "os_connect fails with nobody there");
    Listener *l  = os_listen(os, scratch, ep, sm);
    if (!check(t, l!=0, "os_listen")) {
        return;
    }
    ConnWatch *sw = alloc<ConnWatch>(perm, 2);
    ConnWatch *cw = alloc<ConnWatch>(perm);
    for (i32 i = 0; i < 2; i++) {
        sw[i].got = Buf(perm, 1<<21);
    }
    cw->got = Buf(perm, 1<<21);

    // Data both ways, a megabyte from the server, then its end
    Conn *c = os_connect(os, scratch, ep, cm, 7);
    cw->c  = c;
    cw->id = 7;
    if (!check(t, c!=0, "os_connect")) {
        os_unlisten(os, l);
        return;
    }
    check(t, connwatch(os, sm, sw, 5000, hasconn), "the server accepts it");
    os_connwrite(os, c, "0123456789");
    check(t, connwatch(os, sm, sw, 5000, has10), "the server reads it");
    check(t, sw[0].got.len==10 && Str{sw[0].got.data, 10}=="0123456789",
          "in order and whole");
    Str mega = {allocbytes(&scratch, 1<<20), 1<<20};
    for (iz i = 0; i < mega.len; i++) {
        mega[i] = (u8)(i * 7);
    }
    os_connwrite(os, sw[0].c, takehead(mega, 1000));
    os_connwrite(os, sw[0].c, cuthead(mega, 1000));
    os_connwrite(os, sw[0].c, {});  // the end, after the queue
    check(t, connwatch(os, cm, cw, 10000, haseof), "the client reads to the end");
    check(t, finish(&cw->got)==mega, "a megabyte, intact");
    check(t, !os_connwrite(os, sw[0].c, "x"), "nothing more after the end");
    os_connclose(os, c);
    os_connclose(os, sw[0].c);

    // A client breaking its connection ends the server's
    c = os_connect(os, scratch, ep, cm, 8);
    check(t, connwatch(os, sm, sw+1, 5000, hasconn), "another is accepted");
    if (c) {
        os_connclose(os, c);
    }
    check(t, connwatch(os, sm, sw+1, 5000, haseof), "its break is seen");
    if (sw[1].c) {
        os_connclose(os, sw[1].c);
    }

    // Probes, which the listener may or may not see, then the end
    check(t, os_listening(os, scratch, ep), "os_listening sees the listener");
    check(t, !os_listen(os, scratch, ep, sm), "a second listener is refused");
    os_unlisten(os, l);
    i64 t0 = os_ticks(os);
    b32 gone = 0;
    while (!(gone = !os_listening(os, scratch, ep)) && os_ticks(os)-t0<2000000) {
        mailwait(sm, 10);
    }
    check(t, gone, "os_unlisten frees the endpoint");
    ep = os_endpoint(os, &scratch, state, "selftest");  // its link went too
    Mail m = mailtake(sm);
    for (Record r; (r = mailnext(&m)).ok;) {
        if (r.type == CONN_NEW) {
            Conn *p = 0;
            copybytes(&p, r.data.data, sizeof(p));
            os_connclose(os, p);
        }
    }
    l = os_listen(os, scratch, ep, sm);
    check(t, l!=0, "which another may take");
    if (l) {
        os_unlisten(os, l);
    }
    say(os, "endpoints done");
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

    testlocks(&t, a, dir);
    testendpoints(&t, perm, a, dir);
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

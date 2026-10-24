// Render scheduler tests: a simulated loop on a fake clock, rendering
// each run as a trace of frames, e.g. "FP@0 F@16 F@33 | wait" for three
// frames 16.667 ms apart, the first presented, then a wait for events.

enum : i64 {
    PACE60 = 16667,  // µs per frame at 60 Hz
};

// Frame hashes come from a script: frame n hashes script[n], and the
// last entry repeats forever. Busy masks likewise, when given.
struct Sim {
    Sched s;
    Buf   trace;
    u64  *hashes;
    iz    nhashes;
    u32  *busy;
    iz    nbusy;
    iz    frames;
    iz    presents;
    iz    stale;
    i64   atduring;     // every frame declares this deadline until it passes
    b32   dirtyduring;  // the next frame (once) makes new work
};

static Sim newsim(Arena *a, i64 pace)
{
    Sim m = {};
    m.s     = newsched(0, pace);
    m.trace = Buf(a, 256);
    return m;
}

static void simhashes(Sim *m, Arena *a, Str script)  // "1 2 3"
{
    m->nhashes = 0;
    m->hashes  = alloc<u64>(a, 64);
    for (Str s = script; s.len;) {
        Cut c = cut(s, ' ');
        s = c.tail;
        m->hashes[m->nhashes++] = (u64)parseint(c.head).val;
    }
}

static void simframe(Sim *m)
{
    if (!schedbegin(&m->s)) {
        print(&m->trace, "!reentered ");
        return;
    }
    if (m->atduring > m->s.now) {
        schedat(&m->s, m->atduring, DUE_APP);
    }
    if (m->dirtyduring) {
        scheddirty(&m->s);
        m->dirtyduring = 0;
    }
    iz  n    = m->frames;
    u64 hash = m->nhashes ? m->hashes[n<m->nhashes ? n : m->nhashes-1] : 1;
    u32 busy = m->nbusy ? m->busy[n<m->nbusy ? n : m->nbusy-1] : 0;
    Frame f = schedend(&m->s, hash, busy);
    m->frames++;
    m->presents += f.present;
    m->stale    += f.stale;
    putbyte(&m->trace, 'F');
    if (f.present) putbyte(&m->trace, 'P');
    if (f.stale)   putbyte(&m->trace, 'S');
    if (f.runaway) putbyte(&m->trace, 'R');
    if (f.probe)   putbyte(&m->trace, '?');
    if (f.keyup)   putbyte(&m->trace, 'K');
    putbyte(&m->trace, '@');
    print(&m->trace, m->s.now/1000);
    putbyte(&m->trace, ' ');
}

// Run the loop with no events until the given time, or until it would
// wait forever. Traces "| wait" or "| t=MS" for what it does next.
static void simrun(Sim *m, i64 until)
{
    for (i32 guard = 0; guard < 100000; guard++) {
        i64 t = schedtimeout(&m->s);
        if (t < 0) {
            print(&m->trace, "| wait");
            return;
        }
        if (m->s.now+t > until) {
            print(&m->trace, "| t=");
            print(&m->trace, (m->s.now+t)/1000);
            m->s.now = until;
            return;
        }
        m->s.now += t;
        if (scheddue(&m->s)) {
            simframe(m);
        }
    }
    print(&m->trace, "| spinning");
}

static Str simtrace(Sim *m)
{
    Str r = finish(&m->trace);
    m->trace.len = 0;
    return r;
}

static void test_sched(Test *t)
{
    Arena a = t->perm;

    // Startup: three settle frames, the first presented, then sleep
    {
        Sim m = newsim(&a, PACE60);
        simrun(&m, 1000000);
        expect(t, "sched.startup", simtrace(&m), "FP@0 F@16 F@33 | wait");
    }

    // Input owes three frames; an unchanged picture is not presented
    {
        Sim m = newsim(&a, PACE60);
        simrun(&m, 1000000);
        simtrace(&m);
        m.s.now = 2000000;
        schedinput(&m.s);
        simrun(&m, 3000000);
        expect(t, "sched.input", simtrace(&m), "F@2000 F@2016 F@2033 | wait");
    }

    // Frames continue until two in a row hash the same
    {
        Sim m = newsim(&a, PACE60);
        simhashes(&m, &a, "1 2 3 4 5");
        simrun(&m, 1000000);
        expect(t, "sched.converge", simtrace(&m),
            "FP@0 FP@16 FP@33 FP@50 FP@66 F@83 | wait");
    }

    // A wake owes two frames; with a new picture, a third to confirm it
    {
        Sim m = newsim(&a, PACE60);
        simhashes(&m, &a, "1 1 1 2");
        simrun(&m, 1000000);
        simtrace(&m);
        m.s.now = 2000000;
        scheddirty(&m.s);
        simrun(&m, 3000000);
        expect(t, "sched.wake", simtrace(&m), "FP@2000 F@2016 | wait");
    }

    // A deadline requested during a frame wakes the loop once, on time
    {
        Sim m = newsim(&a, PACE60);
        m.atduring = 300000;
        simrun(&m, 1000000);
        expect(t, "sched.deadline", simtrace(&m),
            "FP@0 F@16 F@33 F@300 | wait");
    }

    // Requests made during a frame survive it (the elfeed2 bug): work
    // arriving while the last owed frame runs gets frames of its own
    {
        Sim m = newsim(&a, PACE60);
        simrun(&m, 1000000);
        simtrace(&m);
        m.s.now = 2000000;
        schedinput(&m.s);
        m.s.owed = 1;  // as if the input's other frames already ran
        m.dirtyduring = 1;
        simrun(&m, 3000000);
        expect(t, "sched.during", simtrace(&m), "F@2000 F@2016 | wait");
    }

    // Deadlines are consumed at frame start: one not re-declared by the
    // frame is gone, and an earlier one replaces a later one
    {
        Sched s = newsched(0, PACE60);
        schedat(&s, 500000, DUE_CARET);
        schedat(&s, 300000, DUE_TOOLTIP);
        schedat(&s, 400000, DUE_SETTINGS);
        expect(t, "sched.atmin", istr(&a, s.deadline), "300000");
        expect(t, "sched.atwhy", duenames[s.duewhy], "tooltip");
        schedbegin(&s);
        schedend(&s, 1, 0);
        expect(t, "sched.atconsumed", istr(&a, s.deadline == I64MAX), "1");
    }

    // An expired deadline that pacing defers still runs a frame
    {
        Sched s = newsched(0, PACE60);
        schedbegin(&s);
        schedend(&s, 1, 0);
        s.owed = 0;
        schedat(&s, 5000, DUE_APP);  // before the next pacing slot
        s.now = 6000;
        expect(t, "sched.paceddue", istr(&a, scheddue(&s)), "0");
        expect(t, "sched.pacedwait", istr(&a, schedtimeout(&s)), "10667");
        s.now = 16667;
        expect(t, "sched.pacedrun", istr(&a, scheddue(&s)), "1");
    }

    // Busy keeps frames coming, paced, until the reason goes away
    {
        Sim m = newsim(&a, PACE60);
        u32 busy[] = {0, 0, 0, BUSY_MOUSE, BUSY_MOUSE, BUSY_MOUSE, 0};
        m.busy  = busy;
        m.nbusy = countof(busy);
        simrun(&m, 1000000);
        expect(t, "sched.busy", simtrace(&m),
            "FP@0 F@16 F@33 | wait");
        m.s.now = 2000000;
        schedinput(&m.s);
        simrun(&m, 3000000);
        expect(t, "sched.busyheld", simtrace(&m),
            "F@2000 F@2016 F@2033 F@2050 | wait");
    }

    // A key held keeps frames coming while the OS repeats it; with no
    // event for KEY_STALE its release was lost: let go of it, once, and
    // stop, though ImGui still holds it here
    {
        Sim m = newsim(&a, 500000);
        u32 busy[] = {BUSY_KEY};
        m.busy  = busy;
        m.nbusy = 1;
        simrun(&m, 10000000);
        expect(t, "sched.keylost", simtrace(&m),
            "FP@0 F@500 F@1000 F@1500 F@2000 FK@2500 F@3000 | wait");
        m.s.now = 10000000;
        schedinput(&m.s);
        schedkey(&m.s, m.s.now);
        simrun(&m, 11900000);
        expect(t, "sched.keyheld", simtrace(&m),
            "F@10000 F@10500 F@11000 F@11500 | t=12000");
        schedkey(&m.s, m.s.now);  // a repeat at 11.9 s
        simrun(&m, 20000000);
        expect(t, "sched.keyrepeat", simtrace(&m),
            "F@12000 F@12500 F@13000 F@13500 F@14000 FK@14500 F@15000 | wait");
    }

    // A forced frame presents an unchanged picture
    {
        Sim m = newsim(&a, PACE60);
        simrun(&m, 1000000);
        simtrace(&m);
        m.s.now = 2000000;
        schedforce(&m.s);
        simrun(&m, 3000000);
        expect(t, "sched.force", simtrace(&m), "FP@2000 F@2016 | wait");
    }

    // Frames are paced even when not presented
    {
        Sim m = newsim(&a, 8333);
        simhashes(&m, &a, "1 2 3 3");
        simrun(&m, 1000000);
        expect(t, "sched.pace120", simtrace(&m), "FP@0 FP@8 FP@16 F@24 | wait");
    }

    // A frame inside a frame (WM_SIZE during rendering) is refused and
    // owed instead
    {
        Sched s = newsched(0, PACE60);
        schedbegin(&s);
        s.owed = 0;
        expect(t, "sched.reenter", istr(&a, schedbegin(&s)), "0");
        expect(t, "sched.reenterowed", istr(&a, s.owed), "1");
        schedend(&s, 1, 0);
        expect(t, "sched.reenterafter", istr(&a, schedbegin(&s)), "1");
    }

    // Minimized: no frames, wait for events; restoring forces a present
    {
        Sim m = newsim(&a, PACE60);
        simrun(&m, 1000000);
        simtrace(&m);
        schedminimize(&m.s, 1);
        schedinput(&m.s);
        schedat(&m.s, 1500000, DUE_CLOCK);
        simrun(&m, 2000000);
        expect(t, "sched.minimized", simtrace(&m), "| wait");
        m.s.now = 2000000;
        schedminimize(&m.s, 0);
        simrun(&m, 3000000);
        expect(t, "sched.restored", simtrace(&m), "FP@2000 F@2016 F@2033 | wait");
    }

    // The staleness probe runs once, 250 ms after going idle; it is
    // quiet when nothing changed...
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simrun(&m, 10000000);
        expect(t, "sched.probe", simtrace(&m), "FP@0 F@16 F@33 F?@283 | wait");
        expect(t, "sched.probeclean", istr(&a, m.stale), "0");
    }

    // ...and when the picture changed with nothing scheduled, that was a
    // missed wake: presented, reported, then settled as usual
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simhashes(&m, &a, "1 1 1 7");
        simrun(&m, 10000000);
        expect(t, "sched.stale", simtrace(&m),
            "FP@0 F@16 F@33 FPS?@283 F@300 F?@550 | wait");
        expect(t, "sched.stalecount", istr(&a, m.stale), "1");
    }

    // Input cancels a pending probe, and frames after it re-arm one
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simrun(&m, 100000);
        expect(t, "sched.probearmed", simtrace(&m), "FP@0 F@16 F@33 | t=283");
        schedinput(&m.s);
        expect(t, "sched.probecancel", istr(&a, m.s.shadowat), "0");
        simrun(&m, 10000000);
        expect(t, "sched.probererun", simtrace(&m),
            "F@100 F@116 F@133 F?@383 | wait");
    }

    // Aggressive probing repeats every second while idle
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing    = 1;
        m.s.aggressive = 1;
        simrun(&m, 2500000);
        expect(t, "sched.aggressive", simtrace(&m),
            "FP@0 F@16 F@33 F?@283 F?@1283 F?@2283 | t=3283");
    }

    // A probe coinciding with a deadline is no probe: the deadline
    // declared whatever changes, so nothing is stale
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simhashes(&m, &a, "1 1 1 7");
        m.atduring = 283334;  // exactly when the probe is due
        simrun(&m, 10000000);
        expect(t, "sched.probedeadline", simtrace(&m),
            "FP@0 F@16 F@33 FP@283 F@300 F?@550 | wait");
        expect(t, "sched.probedeadlineclean", istr(&a, m.stale), "0");
    }

    // ...and none runs just before one, after the change it waits for
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simhashes(&m, &a, "1 1 1 7");
        m.atduring = 290000;  // 7 ms after the probe would run
        simrun(&m, 10000000);
        expect(t, "sched.probenear", simtrace(&m),
            "FP@0 F@16 F@33 FP@290 F@306 F?@556 | wait");
    }

    // ...nor is one coinciding with new work
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simrun(&m, 100000);
        simtrace(&m);
        m.s.now = m.s.shadowat;
        scheddirty(&m.s);
        expect(t, "sched.probework", istr(&a, scheddue(&m.s) && !m.s.probe), "1");
    }

    // A forced frame is never reported as stale
    {
        Sim m = newsim(&a, PACE60);
        m.s.probing = 1;
        simhashes(&m, &a, "1 1 1 7");
        simrun(&m, 200000);
        simtrace(&m);
        schedforce(&m.s);
        m.s.shadowat = m.s.now;  // the probe coincides with the force
        simrun(&m, 10000000);
        expect(t, "sched.forcenotstale", istr(&a, m.stale), "0");
    }

    // An undeclared animation is cut off rather than spinning forever
    {
        Sim m = newsim(&a, 1000);
        m.hashes  = alloc<u64>(&a, 1000);
        m.nhashes = 1000;
        for (iz i = 0; i < 1000; i++) m.hashes[i] = (u64)i + 1;
        simrun(&m, 10000000);
        Str tr = simtrace(&m);
        expect(t, "sched.runaway", istr(&a, m.frames), istr(&a, RUNAWAY));
        expect(t, "sched.runawaytail", taketail(tr, 14), "FPR@599 | wait");
    }

    // ...but not while the UI declares a reason to keep going
    {
        Sim m = newsim(&a, 1000);
        m.hashes  = alloc<u64>(&a, 1000);
        m.nhashes = 1000;
        for (iz i = 0; i < 1000; i++) m.hashes[i] = (u64)i + 1;
        u32 busy[] = {BUSY_APP};
        m.busy  = busy;
        m.nbusy = 1;
        simrun(&m, 699500);
        expect(t, "sched.declared", istr(&a, m.frames), "700");
    }

    // Millisecond waits round up, so a wait never ends early
    {
        expect(t, "sched.ms-1", istr(&a, timeoutms(-1)), "-1");
        expect(t, "sched.ms0", istr(&a, timeoutms(0)), "0");
        expect(t, "sched.ms1", istr(&a, timeoutms(1)), "1");
        expect(t, "sched.ms1000", istr(&a, timeoutms(1000)), "1");
        expect(t, "sched.ms1001", istr(&a, timeoutms(1001)), "2");
        expect(t, "sched.msbig", istr(&a, timeoutms(I64MAX/2)), "2147483647");
    }

    // Hashing: every byte counts, including the length of a short tail
    {
        u8 buf[24] = {};
        Hash64 h0 = {}; hashbytes(&h0, buf, 24);
        Hash64 h1 = {}; hashbytes(&h1, buf, 23);
        buf[22] = 1;
        Hash64 h2 = {}; hashbytes(&h2, buf, 24);
        buf[22] = 0;
        buf[3]  = 1;
        Hash64 h3 = {}; hashbytes(&h3, buf, 24);
        u64 a0 = hashfinal(&h0), a1 = hashfinal(&h1);
        u64 a2 = hashfinal(&h2), a3 = hashfinal(&h3);
        b32 distinct = a0!=a1 && a0!=a2 && a0!=a3 && a1!=a2 && a1!=a3 && a2!=a3;
        expect(t, "sched.hash", istr(&a, distinct), "1");
    }
}

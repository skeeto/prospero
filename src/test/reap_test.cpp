// Kill guard tests: which pids and groups a signal may reach, what
// os_kill does to a reaper, and the reaper's own decisions. Only the
// decisions: no test here, nor anywhere, sends kill(0) or kill(-1).

// A /proc stat line, parsed, as "pid ppid pgrp sid start".
static Str reapstatshow(Arena *a, Str line)
{
    ReapProc p = reapstat(line);
    Buf      b(a, 64);
    print(&b, (i64)p.pid);
    putbyte(&b, ' ');
    print(&b, (i64)p.ppid);
    putbyte(&b, ' ');
    print(&b, (i64)p.pgrp);
    putbyte(&b, ' ');
    print(&b, (i64)p.sid);
    putbyte(&b, ' ');
    print(&b, p.start);
    return finish(&b);
}

static void test_reap(Test *t)
{
    Arena *a = &t->perm;

    // The guard: one process, or one group, by a positive id above 1
    {
        static struct { i64 id; b32 ok; } cases[] = {
            {0, 0},                    // the caller's own group
            {1, 0},                    // init, or as a group every process
            {-1, 0},                   // every process
            {-2, 0},                   // as kill(2) reads it, group 2
            {-12345, 0},               // a negative group, as kill(-pgid)
            {(i64)0x80000000, 0},      // past pid_t
            {(i64)0xffffffff, 0},      // narrowed, -1
            {(i64)0x100000000, 0},     // narrowed, 0
            {(i64)0x100000001, 0},     // narrowed, 1
            {I64MAX, 0},
            {I64MIN, 0},
            {2, 1},
            {4242, 1},
            {0x7fffffff, 1},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Buf name(a, 64);
            print(&name, "signalable ");
            print(&name, cases[i].id);
            expect(t, finish(&name), istr(a, signalable(cases[i].id)),
                   istr(a, cases[i].ok));
        }
    }

    // os_kill: nothing for a Proc with no reaper's pid, nor once reaped
    {
        static i64 bad[] = {0, 1, -1, -4242, (i64)0xffffffff};
        for (iz i = 0; i < countof(bad); i++) {
            for (i32 hard = 0; hard < 2; hard++) {
                for (i32 reaped = 0; reaped < 2; reaped++) {
                    Buf name(a, 64);
                    print(&name, "os_kill pid ");
                    print(&name, bad[i]);
                    print(&name, hard ? Str(" hard") : Str(" soft"));
                    expect(t, finish(&name), istr(a, reapkill(bad[i], reaped, hard)),
                           istr(a, REAPKILL_NONE));
                }
            }
        }
        expect(t, "os_kill reaped soft", istr(a, reapkill(4242, 1, 0)),
               istr(a, REAPKILL_NONE));
        expect(t, "os_kill reaped hard", istr(a, reapkill(4242, 1, 1)),
               istr(a, REAPKILL_NONE));
        expect(t, "os_kill soft", istr(a, reapkill(4242, 0, 0)),
               istr(a, REAPKILL_TERM));
        expect(t, "os_kill hard", istr(a, reapkill(4242, 0, 1)),
               istr(a, REAPKILL_HARD));

        // Then by waitid(WNOHANG|WNOWAIT): alive, dead and unreaped, or
        // already reaped (ECHILD), when its pid may be another's
        expect(t, "os_kill hard, alive", istr(a, reaphard(1, 0)),
               istr(a, REAPKILL_SWEEP));
        expect(t, "os_kill hard, dead", istr(a, reaphard(1, 4242)),
               istr(a, REAPKILL_GROUP));
        expect(t, "os_kill hard, reaped", istr(a, reaphard(0, 0)),
               istr(a, REAPKILL_NONE));
        expect(t, "os_kill hard, reaped, a stale pid", istr(a, reaphard(0, 4242)),
               istr(a, REAPKILL_NONE));
    }

    // The reaper's own group: only one setsid gave it
    {
        expect(t, "reapgroup after setsid", istr(a, reapgroup(4242, 4242, 4242)), "4242");
        // setsid failed: still in the parent's group 4000, session 3000
        expect(t, "reapgroup, failed setsid", istr(a, reapgroup(4242, 4000, 3000)), "0");
        // setsid failed for a group leader that a shell's job control made
        expect(t, "reapgroup, a shell's job", istr(a, reapgroup(4242, 4242, 3000)), "0");
        expect(t, "reapgroup, no group", istr(a, reapgroup(4242, 4000, 4242)), "0");
        expect(t, "reapgroup as init", istr(a, reapgroup(1, 1, 1)), "0");
        expect(t, "reapgroup pid 0", istr(a, reapgroup(0, 0, 0)), "0");
        expect(t, "reapgroup pid -1", istr(a, reapgroup(-1, -1, -1)), "0");
        expect(t, "reapgroup wrapped",
               istr(a, reapgroup(0xffffffff, 0xffffffff, 0xffffffff)), "0");
    }

    // What a reaper's sweep signals of what its scan found
    {
        i32      self  = 4242;
        ReapProc tool  = {4243, 4242, 4242, 4242, 7};
        ReapProc away  = {4300, 1, 4300, 4300, 9};     // left with setsid
        ReapProc sib   = {4100, 4000, 4000, 3000, 5};  // the parent's other child
        ReapProc kern  = {2, 0, 0, 0, 1};              // kthreadd, session 0
        ReapProc init  = {1, 0, 1, 1, 1};
        ReapProc me    = {4242, 4000, 4242, 4242, 3};
        ReapProc none  = {0, 0, 0, 0, 0};
        ReapProc every = {-1, 0, 0, 0, 0};
        expect(t, "reapmine, a descendant", istr(a, reapmine(&tool, self, self, 1)), "1");
        expect(t, "reapmine, in its session", istr(a, reapmine(&tool, self, self, 0)), "1");
        expect(t, "reapmine, an escapee found", istr(a, reapmine(&away, self, self, 1)), "1");
        expect(t, "reapmine, another session", istr(a, reapmine(&away, self, self, 0)), "0");
        expect(t, "reapmine, itself", istr(a, reapmine(&me, self, self, 1)), "0");
        expect(t, "reapmine, init", istr(a, reapmine(&init, self, self, 1)), "0");
        expect(t, "reapmine, pid 0", istr(a, reapmine(&none, self, self, 1)), "0");
        expect(t, "reapmine, pid -1", istr(a, reapmine(&every, self, self, 1)), "0");
        expect(t, "reapmine, the parent's", istr(a, reapmine(&sib, self, self, 0)), "0");
        // With a failed setsid (no group of its own), sessions say nothing
        expect(t, "reapmine, no session, the parent's",
               istr(a, reapmine(&sib, self, 0, 0)), "0");
        expect(t, "reapmine, no session, session 0",
               istr(a, reapmine(&kern, self, 0, 0)), "0");
        expect(t, "reapmine, no session, a descendant",
               istr(a, reapmine(&tool, self, 0, 1)), "1");
    }

    // macOS: reapers are disclaimed only for a harness that answers for
    // itself, an app; never from Terminal, under sshd, or nested
    {
        expect(t, "reapdisclaim, an app", istr(a, reapdisclaim(4242, 4242)), "1");
        expect(t, "reapdisclaim, under Terminal", istr(a, reapdisclaim(4242, 500)), "0");
        expect(t, "reapdisclaim, under a reaper", istr(a, reapdisclaim(4242, 4100)), "0");
        expect(t, "reapdisclaim, unknown", istr(a, reapdisclaim(4242, 0)), "0");
        expect(t, "reapdisclaim, no answer", istr(a, reapdisclaim(4242, -1)), "0");
        expect(t, "reapdisclaim pid 0", istr(a, reapdisclaim(0, 0)), "0");
        expect(t, "reapdisclaim pid 1", istr(a, reapdisclaim(1, 1)), "0");
        expect(t, "reapdisclaim pid -1", istr(a, reapdisclaim(-1, -1)), "0");
    }

    // The id a reaper's sweep looks for: its own, if told and true
    {
        u64 me = 9001;
        expect(t, "reaprespid", istr(a, (i64)reaprespid(1, 4242, 4242, me)), "9001");
        expect(t, "reaprespid, not told", istr(a, (i64)reaprespid(0, 4242, 4242, me)), "0");
        // Told, but the disclaim did not take: Terminal's id is not its own
        expect(t, "reaprespid, not its own", istr(a, (i64)reaprespid(1, 4242, 500, me)), "0");
        expect(t, "reaprespid, unknown", istr(a, (i64)reaprespid(1, 4242, 0, me)), "0");
        expect(t, "reaprespid, no id", istr(a, (i64)reaprespid(1, 4242, 4242, 0)), "0");
        expect(t, "reaprespid, failed id",
               istr(a, (i64)reaprespid(1, 4242, 4242, (u64)-1)), "0");
        expect(t, "reaprespid pid 1", istr(a, (i64)reaprespid(1, 1, 1, me)), "0");
    }

    // The set of the tool's unique ids: sorted, the oldest dropped
    {
        u64 ids[4] = {};
        iz  len    = 0;
        len = reapaddid(ids, len, 4, 30);
        len = reapaddid(ids, len, 4, 10);
        len = reapaddid(ids, len, 4, 20);
        len = reapaddid(ids, len, 4, 20);
        len = reapaddid(ids, len, 4, 0);
        Buf b(a, 64);
        for (iz i = 0; i < len; i++) {
            print(&b, (i64)ids[i]);
            putbyte(&b, ' ');
        }
        expect(t, "reapaddid", finish(&b), "10 20 30 ");
        len = reapaddid(ids, len, 4, 40);
        len = reapaddid(ids, len, 4, 25);  // full: 10 goes
        len = reapaddid(ids, len, 4, 5);   // older than all kept
        b = Buf(a, 64);
        for (iz i = 0; i < len; i++) {
            print(&b, (i64)ids[i]);
            putbyte(&b, ' ');
        }
        expect(t, "reapaddid, full", finish(&b), "20 25 30 40 ");
        expect(t, "reaphasid", istr(a, reaphasid(ids, len, 25)), "1");
        expect(t, "reaphasid, dropped", istr(a, reaphasid(ids, len, 10)), "0");
        expect(t, "reaphasid, never", istr(a, reaphasid(ids, len, 26)), "0");
        expect(t, "reaphasid, zero", istr(a, reaphasid(ids, len, 0)), "0");
        expect(t, "reaphasid, empty", istr(a, reaphasid(ids, 0, 20)), "0");
        u64 none[1] = {};
        expect(t, "reapaddid, no room", istr(a, reapaddid(none, 0, 0, 7)), "0");
    }

    // What a scan finds of the tool's by origin. The reaper (unique id
    // 100) started the tool (101), which forked 102, gone, which forked
    // 103, orphaned to launchd; 103 forked 104. A chain of short-lived
    // parents never seen (105, 106, gone) left 107. 108 disclaimed itself
    // after a parent never seen. The rest are not the tool's: Terminal's
    // shell (50), with 51 under it, a process that disclaimed, and
    // launchd (1), whose parent the kernel is (0).
    {
        ReapProc procs[] = {
            //pid  ppid pg sid start uniq puniq resp
            {4243, 4242, 0, 0, 1, 101, 100, 100},  // the tool
            {4250, 1,    0, 0, 1, 103, 102, 100},  // a double fork's
            {4251, 4250, 0, 0, 1, 104, 103, 100},  // its child
            {4260, 1,    0, 0, 1, 107, 106, 100},  // after a chain
            {4270, 1,    0, 0, 1, 108, 106, 108},  // disclaimed itself
            {600,  1,    0, 0, 1, 50,  40,  50},   // Terminal's shell
            {601,  600,  0, 0, 1, 51,  50,  50},
            {602,  1,    0, 0, 1, 52,  0,   0},    // nothing known
            {1,    0,    0, 0, 1, 1,   0,   1},    // launchd
        };
        iz n = countof(procs);
        for (i32 disclaimed = 0; disclaimed < 2; disclaimed++) {
            u64 ids[16] = {};
            u8  mine[countof(procs)] = {};
            iz  len = 0;
            len = reapaddid(ids, len, countof(ids), 100);  // the reaper
            len = reapaddid(ids, len, countof(ids), 102);  // seen, gone since
            len = reaporigin(procs, n, mine, ids, len, countof(ids),
                             disclaimed ? 100 : 0);
            Buf b(a, 64);
            for (iz i = 0; i < n; i++) {
                if (mine[i]) {
                    print(&b, (i64)procs[i].pid);
                    putbyte(&b, ' ');
                }
            }
            if (disclaimed) {
                expect(t, "reaporigin, responsible", finish(&b),
                       "4243 4250 4251 4260 ");
            } else {
                expect(t, "reaporigin, by parents", finish(&b), "4243 4250 4251 ");
            }
            expect(t, "reaporigin adds what it found",
                   istr(a, reaphasid(ids, len, 104)), "1");
            expect(t, "reaporigin adds nothing else",
                   istr(a, reaphasid(ids, len, 50) || reaphasid(ids, len, 52) ||
                           reaphasid(ids, len, 1)), "0");
        }

        // What was marked already (by parent links) leads to its orphans
        u64 ids[4] = {};
        u8  mine[countof(procs)] = {};
        mine[0] = 1;  // the tool, by its parent, the reaper
        iz len = reaporigin(procs, n, mine, ids, 0, countof(ids), 0);
        expect(t, "reaporigin from a marked process", istr(a, len), "1");
        expect(t, "reaporigin, nothing else",
               istr(a, mine[1] || mine[3] || mine[5]), "0");
        // Without any id or responsibility, nothing at all
        u8 none[countof(procs)] = {};
        reaporigin(procs, n, none, ids, 0, countof(ids), 0);
        b32 any = 0;
        for (iz i = 0; i < n; i++) {
            any |= none[i];
        }
        expect(t, "reaporigin of nothing", istr(a, any), "0");
    }

    // /proc/PID/stat lines
    {
        Str tail = " 0 -1 4194304 92 0 0 0 0 0 0 0 20 0 1 0 123456 5582848 192 "
                   "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 0 0 17 2 0 0\n";
        expect(t, "reapstat",
               reapstatshow(a, concat(a, "4243 (sleep) S 4242 4242 4242", tail)),
               "4243 4242 4242 4242 123456");
        expect(t, "reapstat, odd comm",
               reapstatshow(a, concat(a, "4243 (a) R (b)) S 4242 4242 4242", tail)),
               "4243 4242 4242 4242 123456");
        expect(t, "reapstat, zombie",
               reapstatshow(a, concat(a, "4243 (sleep) Z 4242 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, dead",
               reapstatshow(a, concat(a, "4243 (sleep) X 4242 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, init",
               reapstatshow(a, concat(a, "1 (init) S 0 1 1", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, pid 0",
               reapstatshow(a, concat(a, "0 (idle) S 0 0 0", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, pid -1",
               reapstatshow(a, concat(a, "-1 (x) S 4242 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, pid wraps to -1",
               reapstatshow(a, concat(a, "4294967295 (x) S 4242 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, digits run together",
               reapstatshow(a, concat(a, "424342444245424642474248424942504251 (x) S "
                                         "4242 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, a negative ppid",
               reapstatshow(a, concat(a, "4243 (x) S -1 4242 4242", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, a session past pid_t",
               reapstatshow(a, concat(a, "4243 (x) S 4242 4242 4294967296", tail)),
               "0 0 0 0 0");
        expect(t, "reapstat, cut short",
               reapstatshow(a, "4243 (sleep) S 4242 4242 4242 0 -1 4194304 92 0"),
               "0 0 0 0 0");
        expect(t, "reapstat, no start time",
               reapstatshow(a, "4243 (x) S 4242 4242 4242 0 -1 0 0 0 0 0 0 0 0 0 "
                               "20 0 1 0 0 1 1\n"),
               "0 0 0 0 0");
        expect(t, "reapstat, no paren", reapstatshow(a, "4243 sleep S 4242"),
               "0 0 0 0 0");
        expect(t, "reapstat, empty", reapstatshow(a, ""), "0 0 0 0 0");
    }
}

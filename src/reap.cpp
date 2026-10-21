// Portable half of the POSIX process layer: the guard on every signal it
// sends, and the decisions of os_kill and of the reapers. No system
// calls, so the unit tests cover it on every host.
//
// kill(2) reads a pid of 0 as the caller's own process group, -1 as
// every process the caller may signal, and any other negative pid as a
// group, and pid 1 is init: none of those is ever meant here. On
// 2026-10-21 a shell loop sent kill(-1, SIGTERM) to a whole desktop
// session, from a pid that atoi made of 50 concatenated ones. So every
// signal goes out through one function (sendsignal, main_posix.cpp), to
// one process or to one group named by its positive id, and it refuses
// any id that signalable rejects. A pid from a scan of the system is
// signaled only if its start time is still the one the scan found.
//
// This is free and unencumbered software released into the public domain.

// Whether a pid, or a process group id, names one process or one group
// that may be signaled: above 1, and within pid_t, checked before the
// value is narrowed, so that no wider value wraps into a special one.
static b32 signalable(i64 id)
{
    return id>1 && id<=0x7fffffff;
}

// What os_kill does to a tool's reaper, by its pid.
enum : i32 {
    REAPKILL_NONE,  // nothing: no reaper, or reaped, its pid free for reuse
    REAPKILL_TERM,  // soft: TERM to its group, which the reaper ignores
    REAPKILL_HARD,  // the reaper sweeps, or its group goes if it is dead
};

static i32 reapkill(i64 pid, b32 reaped, b32 hard)
{
    if (!signalable(pid) || reaped) {
        return REAPKILL_NONE;
    }
    return hard ? REAPKILL_HARD : REAPKILL_TERM;
}

// The group a reaper may signal as its own, by its pid, group and
// session after setsid: its pid if it leads both, else 0 for none. A
// reaper whose setsid failed is still in its parent's group, perhaps
// the harness's, or a shell's with the user's other jobs in it.
static i32 reapgroup(i64 self, i64 pgrp, i64 sid)
{
    return signalable(self) && pgrp==self && sid==self ? (i32)self : 0;
}

struct ReapProc {
    i32 pid;
    i32 ppid;
    i32 pgrp;
    i32 sid;
    i64 start;  // start time: Linux clock ticks after boot, macOS µs
};

// Parse a Linux /proc/PID/stat line, "pid (comm) state ppid pgrp session
// tty_nr tpgid ...", whose 22nd field is the start time, and whose comm
// may hold anything, up to the last ')'. The pid is 0 if the line is cut
// short or malformed, or names a zombie, or a process not to signal, or
// one without a start time to tell it apart from a later one.
static ReapProc reapstat(Str s)
{
    ReapProc r     = {};
    iz       paren = -1;
    for (iz i = 0; i < s.len; i++) {
        paren = s[i]==')' ? i : paren;
    }
    Int pid = parseint(cut(s, ' ').head);
    if (paren<0 || !pid.ok || !signalable(pid.val)) {
        return r;
    }

    // The state, then fields 4 (ppid) to 22 (starttime)
    Str rest  = cuthead(s, paren+1);
    u8  state = 0;
    i64 f[19] = {};
    for (i32 k = -1; k < 19; k++) {
        Cut c = cut(trimleft(rest), ' ');
        rest = c.tail;
        if (!c.head.len) {
            return r;
        } else if (k < 0) {
            state = c.head[0];
            continue;
        }
        Int v = parseint(c.head);
        if (!v.ok) {
            return r;
        }
        f[k] = v.val;
    }
    for (i32 k = 0; k < 3; k++) {
        if (f[k]<0 || f[k]>0x7fffffff) {
            return r;  // no such ppid, pgrp or session
        }
    }
    if (state=='Z' || state=='X' || f[18]<=0) {
        return r;
    }
    r.pid   = (i32)pid.val;
    r.ppid  = (i32)f[0];
    r.pgrp  = (i32)f[1];
    r.sid   = (i32)f[2];
    r.start = f[18];
    return r;
}

// Whether a reaper may signal a process its scan found: never itself,
// nor a pid that signalable refuses; a descendant of its own; or, if it
// leads a session (group, from reapgroup), a member of that session.
static b32 reapmine(ReapProc *p, i32 self, i32 group, b32 descendant)
{
    if (!signalable(p->pid) || p->pid==self) {
        return 0;
    }
    return descendant || (group && p->sid==group);
}

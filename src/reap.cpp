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
// On macOS a reaper also finds what left the tool's tree by what each
// process keeps of its origin (reaporigin): its responsible process,
// when the reaper was spawned to be that of the tool's tree, and the
// unique id of its parent when it was forked or last exec'd.
//
// This is free and unencumbered software released into the public domain.

// Whether a pid, or a process group id, names one process or one group
// that may be signaled: above 1, and within pid_t, checked before the
// value is narrowed, so that no wider value wraps into a special one.
static b32 signalable(i64 id)
{
    return id>1 && id<=0x7fffffff;
}

// What os_kill does to a tool's reaper, by its pid, under the lock that
// its waiter reaps it under, so that its pid is held while not reaped.
enum : i32 {
    REAPKILL_NONE,   // nothing: no reaper, or reaped, its pid free for reuse
    REAPKILL_TERM,   // soft: TERM to its group, which the reaper ignores
    REAPKILL_HARD,   // hard: ask whether the reaper lives (reaphard)
    REAPKILL_SWEEP,  // it lives, and sweeps when asked
    REAPKILL_GROUP,  // it died, unreaped: KILL to the group it left
};

static i32 reapkill(i64 pid, b32 reaped, b32 hard)
{
    if (!signalable(pid) || reaped) {
        return REAPKILL_NONE;
    }
    return hard ? REAPKILL_HARD : REAPKILL_TERM;
}

// The hard kill of an unreaped reaper, by what waitid(WNOHANG|WNOWAIT)
// said of it: whether the call succeeded, and the pid it reported, 0 if
// the reaper lives. A dead reaper not yet reaped still holds its pid,
// and with it the id of its group, where what is left of the tree gets
// KILL. A failed call (ECHILD) means the pid may be another's by now:
// nothing then, rather than a group named by a stale id.
static i32 reaphard(b32 waited, i64 sipid)
{
    if (!waited) {
        return REAPKILL_NONE;
    }
    return sipid ? REAPKILL_GROUP : REAPKILL_SWEEP;
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
    i64 start;      // start time: Linux clock ticks after boot, macOS µs
    u64 uniq   = 0;  // macOS: its unique id, never reused
    u64 puniq  = 0;  // macOS: its parent's, at its fork or last exec
    u64 resp   = 0;  // macOS: the unique id of its responsible process
    b32 traced = 0;  // macOS: a debugger is attached
    i32 oppid  = 0;  // macOS: traced, its parent when that happened
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

// macOS: what a process keeps of where it came from, whatever became of
// its parents. Each has a unique id, 64 bits, that only grows, is never
// reused, and survives exec, and keeps its parent's from its fork or
// its last exec (puniq): so an orphan that execs, as a daemon often
// does once its parents are gone, keeps launchd's (1), which leads
// nowhere. (Its pid version, p_idversion, changes with each exec, so a
// parent's taken before its exec would not match what its later
// children keep.) And each has a responsible process, its parent's by
// default, which the kernel keeps through fork, exec, setsid and
// orphaning: a process spawned "disclaimed" is its own, and so its
// descendants', unless one of them disclaims in turn.
//
// A debugger that attaches to a process (PT_ATTACH, as "lldb -p" does)
// makes it its child for as long as it lasts, and should it exec
// meanwhile, gives it the debugger's id as puniq: a tool that debugs
// the user's process must not make that process the tool's. So neither
// tells where a traced process came from, unless its debugger was its
// parent when it attached (oppid), as when the debugger started it
// (reapforked, reapattached).
//
// TCC charges a process's privacy access to its responsible process. A
// reaper is spawned disclaimed only when the harness is its own already
// (reapdisclaim), as an app that LaunchServices started is, and only
// while the executable a reaper runs is still the one the harness runs:
// then the tools answer to the same code as before. Otherwise they keep
// answering to the app responsible for the harness, Terminal, or sshd
// with remote logins' full disk access, whose grants a disclaimed
// reaper would take from them, or to the harness itself after its
// executable was replaced, as a rebuild in place does, rather than to
// code that the user never granted anything.

// Whether to spawn a reaper disclaimed, by this process's pid, that of
// its responsible process (0 when unknown), and whether the executable
// the reaper would run is still the file this process started from
// (samefile): only if this process answers for itself, and the reaper
// would be the same code.
static b32 reapdisclaim(i64 self, i64 responsible, b32 samefile)
{
    return signalable(self) && responsible==self && samefile;
}

// Whether a debugger attached to a process from outside its parentage:
// traced, and not by the parent it had when that happened (oppid), as
// when the debugger started it, or was asked by it (PT_TRACE_ME). Its
// parent is then the debugger, and its puniq too if it exec'd since.
static b32 reapattached(ReapProc *p)
{
    return p->traced && p->oppid!=p->ppid;
}

// Whether a process that lists as the child of a recorded one, by its
// pid and unique id, came from it: forked by it, or exec'd while its
// child, either way keeping its id as puniq. Not a process a debugger
// attached to, unless that one is the debugger's own, nor the child of
// another process that the parent's pid named since. Zero is no id.
static b32 reapforked(ReapProc *child, i64 parent, u64 parentuniq)
{
    b32 attached = child->traced && child->oppid!=parent;
    return parentuniq && child->puniq==parentuniq && !attached;
}

// The responsible unique id a reaper's sweep looks for: its own (uniq),
// if it was spawned disclaimed (told) and is its own responsible process
// (respself, that one's pid), else 0, for none. Never anything else's:
// whatever Terminal answers for is every shell in it.
static u64 reaprespid(b32 told, i64 self, i64 respself, u64 uniq)
{
    b32 ok = told && signalable(self) && respself==self;
    return ok && uniq && uniq!=(u64)-1 ? uniq : 0;
}

// The index of the first id in ids (sorted) no less than id.
static iz reapidindex(u64 *ids, iz len, u64 id)
{
    iz lo = 0;
    iz hi = len;
    while (lo < hi) {
        iz mid = lo + (hi-lo)/2;
        if (ids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Whether a unique id is in ids (sorted). Zero is no id.
static b32 reaphasid(u64 *ids, iz len, u64 id)
{
    iz i = reapidindex(ids, len, id);
    return id && i<len && ids[i]==id;
}

// Add a unique id to a sorted set of at most cap ids, keeping the newest:
// ids only grow, so when it is full the smallest goes, the oldest
// process's, or the new id itself if it is older still. Returns the new
// length. Zero is no id.
static iz reapaddid(u64 *ids, iz len, iz cap, u64 id)
{
    iz at = reapidindex(ids, len, id);
    if (!id || (at<len && ids[at]==id)) {
        return len;
    } else if (len == cap) {
        if (!at) {
            return len;
        }
        for (iz i = 1; i < at; i++) {
            ids[i-1] = ids[i];
        }
        ids[at-1] = id;
        return len;
    }
    for (iz i = len; i > at; i--) {
        ids[i] = ids[i-1];
    }
    ids[at] = id;
    return len + 1;
}

// Mark what a scan of n processes found to have come from the tool, by
// its origin: responsible to resp (reaprespid, or 0 for none), or forked
// by a process whose unique id is in ids, the tool's processes, alive or
// gone (sorted, len of at most cap), unless a debugger attached to it
// (reapattached). Each process marked, here or before (mine), adds its
// own id, and so the closure is transitive: an orphan of an orphan is
// found too, but only through processes once seen, which a chain of
// short-lived parents may not be, and not an orphan that exec'd, whose
// puniq is launchd's. Returns the new length.
static iz reaporigin(ReapProc *procs, iz n, u8 *mine, u64 *ids, iz len,
                     iz cap, u64 resp)
{
    for (iz i = 0; i < n; i++) {
        if (mine[i]) {
            len = reapaddid(ids, len, cap, procs[i].uniq);
        }
    }
    for (b32 grew = 1; grew;) {
        grew = 0;
        for (iz i = 0; i < n; i++) {
            ReapProc *p = procs + i;
            b32 forked = !reapattached(p) && reaphasid(ids, len, p->puniq);
            if (!mine[i] && ((resp && p->resp==resp) || forked)) {
                mine[i] = 1;
                grew    = 1;
                len     = reapaddid(ids, len, cap, p->uniq);
            }
        }
    }
    return len;
}

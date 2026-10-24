// Render scheduler: when the GUI draws a frame
//
// The GUI renders on demand, and an idle window draws nothing at all.
// The loop waits, then dispatches every pending event, then decides:
//
//   for (;;) {
//       wait up to schedtimeout()           // OS input, a wake, or a deadline
//       dispatch everything pending          // schedinput, scheddirty, schedforce
//       now = os_ticks(); if (scheddue()) frame()
//   }
//
// Deciding only after the queue is empty is what kept dcmake's attempt
// one event behind: it drew after waking but before dispatching what
// woke it. A frame (schedbegin to schedend) consumes the requests made
// before it began, so requests made during the frame, by the UI or by
// a wake dispatched from inside a modal loop, survive to the next one;
// elfeed2 cleared its flag after the frame and lost them.
//
// ImGui needs several frames to settle after an event (windows appear a
// frame late, popups auto-fit over two), and drives changes from timers
// with no event at all (tooltips, the caret, fades). So input owes at
// least SETTLE_INPUT frames and a wake SETTLE_WAKE, and frames continue
// until two in a row hash the same. Meanwhile the UI declares its timers
// each frame with schedat, of which only the earliest is kept, and its
// reasons to keep going (held buttons, fades) through the busy mask.
//
// A held key is busy, as ImGui repeats it on its own timer, but only
// while the OS confirms it: a key truly held auto-repeats, each repeat
// another key event (schedkey). One with no event for KEY_STALE lost
// its release (a synthetic key-down, focus taken mid-press), and the
// frame says to let go of it rather than draw on forever. KEY_STALE
// outlasts the longest delays and intervals the systems' settings
// offer: macOS 1.8 s (InitialKeyRepeat 120), Windows 1 s, GNOME 2 s.
// With repeat turned off, ImGui's own repeat stops there too.
//
// Present is skipped when a frame hashes the same as what is on screen,
// but frames stay paced to the display refresh either way.
//
// The staleness probe checks all of this: PROBE_DELAY after going idle
// it renders one frame nobody asked for. If that frame differs from the
// screen, a wake was missed, and the caller reports what changed.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    SETTLE_INPUT = 3,    // frames owed after OS input
    SETTLE_WAKE  = 2,    // frames owed when drained events change the view
    RUNAWAY      = 600,  // changing frames in a row before giving up
};

enum : i64 {
    PROBE_DELAY   = 250000,   // µs after going idle
    PROBE_REPEAT  = 1000000,  // µs between probes in aggressive mode
    PROBE_SLACK   = 50000,    // µs: no probe this close before a deadline
    KEY_STALE     = 2500000,  // µs: a key held with no repeat was released
};

// Reasons a frame needs a successor, from the UI scan (ui.cpp)
enum : u32 {
    BUSY_MOUSE  = 1u<<0,   // a mouse button is held
    BUSY_KEY    = 1u<<1,   // a non-modifier key is held: key repeat
    BUSY_DIM    = 1u<<2,   // a modal's dimming fades in or out
    BUSY_NAVWIN = 1u<<3,   // the Ctrl+Tab window switcher is up
    BUSY_DRAG   = 1u<<4,   // drag and drop
    BUSY_HIDDEN = 1u<<5,   // a window is hidden while it measures itself
    BUSY_QUEUE  = 1u<<6,   // trickled input events are still queued
    BUSY_SCROLL = 1u<<7,   // a scroll request lands next frame
    BUSY_TABS   = 1u<<8,   // a tab bar scrolls or reorders
    BUSY_DOCK   = 1u<<9,   // docking requests are pending
    BUSY_APP    = 1u<<10,  // the app animates (smooth scrolling)
    BUSY_LAYOUT = 1u<<11,  // a window's content size changes next frame
    BUSY_TEX    = 1u<<12,  // dropped textures wait for the renderer
};

enum : i32 {
    BUSY_COUNT = 13,
};

static Str busynames[] = {
    "mouse", "key", "dim", "navwin", "drag", "hidden", "queue", "scroll",
    "tabs", "dock", "app", "layout", "textures",
};
static_assert(countof(busynames) == BUSY_COUNT);

// Deadline sources, recorded with the earliest deadline
enum : i32 {
    DUE_NONE,
    DUE_TOOLTIP,
    DUE_CARET,
    DUE_SETTINGS,
    DUE_STREAM,
    DUE_CLOCK,
    DUE_HOVER,
    DUE_APP,
    DUE_COUNT,
};

static Str duenames[] = {
    "none", "tooltip", "caret", "settings", "stream", "clock", "hover", "app",
};
static_assert(countof(duenames) == DUE_COUNT);

struct Sched {
    i64 now;        // µs, set by the loop before scheddue
    i64 pace;       // minimum frame interval: the refresh period
    i64 lastframe;  // start of the last frame
    i64 deadline;   // earliest requested wake, I64MAX if none
    i32 duewhy;     // DUE_* source of deadline
    i32 owed;       // settle frames owed
    i32 changing;   // frames in a row whose hash differed from the last
    u32 busy;       // BUSY_* reasons from the last frame
    b32 force;      // must present: resize, expose, DPI, device change
    b32 inframe;    // a frame is running (WM_SIZE can arrive inside one)
    b32 minimized;  // nothing to draw on: wait for restore or a wake
    b32 probing;    // staleness probe enabled
    b32 aggressive; // probe every PROBE_REPEAT while idle
    b32 probe;      // the running frame is a probe
    b32 keylost;    // a key went stale since keyat (keyup given)
    i64 shadowat;   // when the next probe runs, 0 if none
    i64 keyat;      // the last key event: down, up, or an auto-repeat
    u64 prevhash;   // last frame's draw hash
    u64 shownhash;  // the presented draw hash
};

// The outcome of a frame, from schedend.
struct Frame {
    b32 present;  // render and present it, else only upload textures
    b32 stale;    // a probe found the screen out of date: a missed wake
    b32 runaway;  // it kept changing with no declared reason, so stopped
    b32 probe;    // it was a probe
    b32 keyup;    // a key held with no repeat for KEY_STALE: release it
};

static Sched newsched(i64 now, i64 pace)
{
    Sched s = {};
    s.now       = now;
    s.pace      = pace;
    s.lastframe = now - pace;  // the first frame need not wait
    s.deadline  = I64MAX;
    s.owed     = SETTLE_INPUT;  // the first frames
    s.force    = 1;
    return s;
}

static i32 maxi32(i32 a, i32 b)
{
    return a>b ? a : b;
}

// OS input of any kind arrived (and went to ImGui).
static void schedinput(Sched *s)
{
    s->owed     = maxi32(s->owed, SETTLE_INPUT);
    s->shadowat = 0;
}

// A key event arrived at time t (µs): a press, a release, or the OS's
// auto-repeat of a key held.
static void schedkey(Sched *s, i64 t)
{
    s->keyat   = t;
    s->keylost = 0;
}

// Drained events changed what the view shows.
static void scheddirty(Sched *s)
{
    s->owed = maxi32(s->owed, SETTLE_WAKE);
}

// The next frame must present: resized, exposed, rescaled, restored.
static void schedforce(Sched *s)
{
    s->force = 1;
    s->owed  = maxi32(s->owed, SETTLE_WAKE);
}

// Ask for a frame at time t (µs). Only the earliest request survives,
// until a frame begins and consumes it.
static void schedat(Sched *s, i64 t, i32 why)
{
    if (t < s->deadline) {
        s->deadline = t;
        s->duewhy   = why;
    }
}

static void schedminimize(Sched *s, b32 minimized)
{
    if (s->minimized && !minimized) {
        schedforce(s);
    }
    s->minimized = minimized;
}

// How long the loop may wait, in µs, or -1 to wait for an event.
static i64 schedtimeout(Sched *s)
{
    if (s->minimized) {
        return -1;  // still woken to drain the mailbox
    }
    if (s->owed || s->busy || s->force) {
        i64 t = s->lastframe + s->pace - s->now;
        return t>0 ? t : 0;
    }
    i64 t = s->deadline;
    if (s->shadowat && s->shadowat<t) {
        t = s->shadowat;
    }
    if (t == I64MAX) {
        return -1;
    }
    t -= s->now;
    return t>0 ? t : 0;
}

// Whether to draw a frame now. Expired deadlines and probes become owed
// frames here, so pacing may still defer them to a later call. A probe
// is a frame nothing asked for: one that coincides with a deadline or
// new work is an ordinary frame, whose changes were declared. Nor does
// a probe run just before a deadline, which by design lands a little
// after the change it waits for: the probe would see the change early.
static b32 scheddue(Sched *s)
{
    if (s->deadline <= s->now) {
        s->deadline = I64MAX;
        s->owed     = maxi32(s->owed, 1);
    }
    if (s->shadowat && s->shadowat<=s->now) {
        s->shadowat = 0;
        b32 quiet = !s->owed && !s->busy && !s->force;
        if (quiet && s->deadline-s->now>PROBE_SLACK) {
            s->probe = 1;
            s->owed  = 1;
        }
    }
    return !s->minimized && (s->owed || s->busy || s->force) &&
           s->now >= s->lastframe + s->pace;
}

// Begin a frame, consuming the requests made so far. Returns 0 if a
// frame is already running, which then owes another.
static b32 schedbegin(Sched *s)
{
    if (s->inframe) {
        s->owed = maxi32(s->owed, 1);
        return 0;
    }
    s->inframe   = 1;
    s->lastframe = s->now;
    s->deadline  = I64MAX;
    s->duewhy    = DUE_NONE;
    return 1;
}

// End a frame given its draw hash and the UI's busy reasons.
static Frame schedend(Sched *s, u64 hash, u32 busy)
{
    Frame r = {};
    if (busy&BUSY_KEY && s->now-s->keyat>=KEY_STALE) {
        // Its release was lost: the caller lets go of it, once, and it
        // is busy no more whether or not that worked
        busy      &= ~(u32)BUSY_KEY;
        r.keyup    = !s->keylost;
        s->keylost = 1;
    }
    r.probe   = s->probe;
    r.present = s->force || hash!=s->shownhash;
    r.stale   = s->probe && r.present && !s->force;
    if (r.present) {
        s->shownhash = hash;
    }

    s->busy     = busy;
    s->changing = hash!=s->prevhash ? s->changing+1 : 0;
    s->prevhash = hash;
    s->owed     = maxi32(s->owed-1, 0);
    if (s->changing) {
        s->owed = maxi32(s->owed, 1);  // until two frames match
    }
    if (r.keyup) {
        s->owed = maxi32(s->owed, 1);  // to take in the release
    }
    if (s->changing>=RUNAWAY && !busy) {
        // An animation nobody declared: stop rather than spin forever.
        s->owed     = 0;
        s->changing = 0;
        r.runaway   = 1;
    }

    s->force = 0;
    s->probe = 0;
    if (!s->owed && !s->busy && s->probing) {
        // One probe per sleep, or one per second when aggressive.
        if (!r.probe) {
            s->shadowat = s->now + PROBE_DELAY;
        } else if (s->aggressive) {
            s->shadowat = s->now + PROBE_REPEAT;
        }
    } else {
        s->shadowat = 0;
    }
    s->inframe = 0;
    return r;
}

// Wait time in whole milliseconds, rounded up so a wait never returns
// before its deadline, or -1 to wait forever.
static i32 timeoutms(i64 us)
{
    if (us < 0) {
        return -1;
    }
    i64 ms = (us + 999) / 1000;
    return ms<0x7fffffff ? (i32)ms : 0x7fffffff;
}


// Draw-data hashing: 64-bit multiply-xorshift over 8-byte words.

struct Hash64 {
    u64 h;
};

static void hashword(Hash64 *h, u64 w)
{
    u64 x = h->h ^ w;
    x *= 0x9e3779b97f4a7c15u;
    x ^= x >> 31;
    h->h = x;
}

static void hashbytes(Hash64 *h, void *p, iz len)
{
    u8 *s = (u8 *)p;
    iz  i = 0;
    for (; i+8 <= len; i += 8) {
        u64 w;
        __builtin_memcpy(&w, s+i, 8);
        hashword(h, w);
    }
    u64 w = (u64)len << 56;
    for (iz k = 0; i < len; i++, k++) {
        w |= (u64)s[i] << (8*k);
    }
    hashword(h, w);
}

static u64 hashfinal(Hash64 *h)
{
    u64 x = h->h;
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93u;
    x ^= x >> 32;
    return x;
}

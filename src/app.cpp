// GUI application: the render loop, the fake agent, and the glue
// between the platform, the scheduler, the view model and the UI
//
// The loop (gui_run) waits for the scheduler's timeout, lets the
// platform dispatch everything pending, and then asks the scheduler
// whether to draw. Events from the engine arrive in a mailbox whose
// notify hook is os_wake; the platform clears its wake flag before
// calling gui_wake, which drains every record. Streamed deltas are shown
// at most every STREAM_PERIOD; anything else at once.
//
// Until the engine joins in M3, a fake agent thread plays its part: it
// speaks protocol v0 through a real mailbox and os_wake, streaming
// markdown at a configurable rate, in bursts or with stalls, and can
// load a large history.
//
// This is free and unencumbered software released into the public domain.

#include "sched.cpp"
#include "highlight.cpp"
#include "doc.cpp"
#include "layout.cpp"
#include "view.cpp"

enum : i64 {
    STREAM_PERIOD = 1000000/30,  // µs between frames showing new deltas
    CARET_STOP    = 10000000,    // µs after the last key: stop blinking
};

enum : iz {
    PROMPT_MAX = 1<<16,
    BENCH_MAX  = 4096,
    LOG_MAX    = 200,     // lines kept in the Log window
    INBOX_CAP  = 1<<21,   // bytes per inbox buffer
};

// Input kinds, recorded for staleness reports
enum : i32 {
    INPUT_MOVE,
    INPUT_BUTTON,
    INPUT_WHEEL,
    INPUT_KEY,
    INPUT_TEXT,
    INPUT_FOCUS,
    INPUT_WINDOW,
    INPUT_OTHER,
    INPUT_COUNT,
};

static Str inputnames[] = {
    "move", "button", "wheel", "key", "text", "focus", "window", "other",
};
static_assert(countof(inputnames) == INPUT_COUNT);

// Why the loop woke
enum : i32 {
    WAKE_INPUT,
    WAKE_MAIL,
    WAKE_DEADLINE,
    WAKE_PROBE,
    WAKE_FORCE,
    WAKE_PACE,   // owed frames waiting out the refresh period
    WAKE_OTHER,  // OS events that are not input: window moves, devices
    WAKE_COUNT,
};

// What happened since the last frame
enum : u32 {
    WOKE_INPUT    = 1u<<0,
    WOKE_MAIL     = 1u<<1,
    WOKE_FORCE    = 1u<<2,
    WOKE_DEADLINE = 1u<<3,  // a deadline other than the overlay's clock
    WOKE_OTHER    = 1u<<4,
};

// Mailbox record types
enum : u32 {
    MAIL_EVENT    = 1,  // a protocol v0 record, engine to GUI
    FAKE_PROMPT   = 16, // GUI to the fake agent
    FAKE_STOP,
    FAKE_TPS,
    FAKE_BURSTS,
    FAKE_PAUSES,
    FAKE_HUGE,
    FAKE_HISTORY,
    FAKE_STREAM,        // keep prompting itself (measurements)
    FAKE_QUIT,
};

// Item rectangles the scripted checklist aims at
enum : i32 {
    RECT_PROMPTBTN,
    RECT_MENU,
    RECT_HISTORY,
    RECT_MODALITEM,
    RECT_MODALOK,
    RECT_TRANSCRIPT,
    RECT_PROMPT,
    RECT_TOOLBAR,
    RECT_COPYITEM,
    RECT_COUNT,
};

struct Rect {
    f32 x0, y0, x1, y1;
};

struct BakedSlot {
    ImFontBaked *baked;
    i32          key;
    i32          gen;
};

struct Fonts {
    ImFont   *prose;
    ImFont   *bold;
    ImFont   *italic;
    ImFont   *bolditalic;
    ImFont   *mono;
    ImFont   *monobold;
    f32       size;      // base pixel size, scaled
    BakedSlot baked[32];
    i32       gen;       // bumped each frame: baked fonts may be discarded
};

struct ListHash {
    char const *owner;
    u64         hash;
};

struct Counts {
    i64 wakes[WAKE_COUNT];
    i64 frames;
    i64 presents;
};

// What the metrics overlay shows, frozen between updates so that it
// never changes a frame by itself.
struct MetricsSnap {
    Counts rate;        // the last whole second
    i64    frames;      // totals
    i64    presents;
    i64    wakes;
    i64    stale;
    i64    runaway;
    i64    frameus;
    i64    maxframeus;
    i64    layoutus;
    i64    deadline;    // relative to the snapshot, I64MAX if none
    i32    duewhy;
    i32    owed;
    i32    changing;
    u32    busy;
};

struct Fake {
    Os      *os;
    Mailbox *out;        // records to the GUI
    Mailbox *in;         // commands from the GUI
    Arena    perm;
    Str      sid;
    i64      seq;
    i32      turn;
    i32      tps;
    b32      bursts;
    b32      pauses;
    b32      stream;     // prompt itself again after each turn
    u64      rng;
    // The turn in progress
    b32      active;
    Str      reply;
    iz       sent;
    i64      next;       // µs: when the next token is due
    i64      resume;     // µs: end of a stall, 0 if none
    b32      stalled;    // this turn already stalled
    b32      huge;       // the next reply is one huge code block
    Str      queued;     // a prompt waiting for the turn to end
    Arena    turnmem;    // the reply and scratch, reset each turn
    byte    *turnbase;
};

struct DriveOp;

struct Gui {
    Os                *os;
    Arena              perm;
    Arena              frame;      // scratch, reset every frame
    byte              *framebase;
    Sched              sched;
    View              *view;
    Mailbox           *inbox;      // engine (fake agent) to GUI
    Mailbox           *outbox;     // GUI to the fake agent
    Fonts              fonts;
    Measure            measure;
    ImGuiStyle         basestyle;  // unscaled
    ImDrawListSplitter splitter;
    f32                scale;      // DPI content scale
    b32                mac;
    b32                quit;

    // Loop and metrics
    u32                woke;
    Counts             total;
    Counts             cur;        // this second
    Counts             last;       // the last whole second
    i64                second;     // start of this second
    i64                stale;
    i64                runaway;
    i64                frameus;
    i64                maxframeus;
    i64                layoutus;
    MetricsSnap        snap;
    u32                cause;      // WOKE_* since the last frame
    b32                episode;    // frames since going idle had a cause
    b32                unsnapped;  // real work not yet in the snapshot
    i64                nextsnap;   // second boundary for the next one
    b32                metricsshown;
    ListHash           shown[64];  // per-list hashes of the screen
    i32                nshown;
    ListHash           lists[64];
    i32                nlists;
    u64                header;     // display size and scale, hashed
    u64                shownheader;
    i32                inputs[8];  // recent input kinds, a ring
    i64                ninputs;
    i64                lastdeadline;
    i32                lastduewhy;

    // Timers
    i64                lastmove;
    i64                lastkey;
    i64                lastcontent;
    Arena              pendmem;    // deltas held back until a content frame
    byte              *pendbase;
    Buf                pending;    // their records, one per line
    i64                turnstart;
    b32                wasbusy;

    // Transcript
    f32                lastwidth;
    ImGuiWindow       *transcriptwin;  // the transcript's child window
    b32                stick;
    f64                scrollto;   // -1 if none
    b32                dragging;
    i32                menumsg;    // the message the context menu is for

    // Toolbar and layout
    char              *prompt;
    i32                tps;
    b32                bursts;
    b32                pauses;
    b32                showdemo;
    b32                firstlayout;
    b32                resetpending;
    b32                resetlayout;
    b32                openmodal;

    // Files and log
    Str                statedir;
    Str                inipath;
    iz                 logfd;
    Slice<Str>         logs;

    // Measurement
    i64                quitat;     // µs, 0 if none
    i64                histreq;    // when history was requested
    b32                histdone;   // the history's last record arrived
    i32                bench;      // benchmark phase, 0 if none
    i32                benchstep;
    i64                benchmax;
    i64                benchsum;
    i64               *benchtimes;   // BENCH_MAX frame times
    i64                benchlaid;
    i32                benchn;
    i64                started;
    b32                trace;      // log every frame
    i32                layoutstreak;
    f32                widthadj;   // benchmark: subtracted from the width

    // Scripted checklist (--drive)
    b32                drive;
    i32                drivepc;
    i64                driveat;
    i32                drivechecks;
    i32                drivefails;
    i64                driveframes;  // total.frames when a wait began
    f32                splitx;       // dock splitter before the drag
    Rect               rects[RECT_COUNT];
    ImGuiID            dockspace;
    i64                lastsave;     // µs
    i64                idlesaves;    // saves made with no input for 1 s
    i64                lastinput;    // µs
    f32                drivex;       // where the synthetic mouse is
    f32                drivey;
    Str                drivecode;    // expected on the clipboard
    f32                dpibase;      // scale before the DPI step
    b32                inmodal;
    i64                modalframes;  // drawn from inside modal loops
    DriveOp           *ops;          // the script
    i32                nops;
    Counts             markcounts;   // totals at the last OP_MARK
    i64                markat;
};

// Log to standard error, the log file, and the Log window, which then
// needs a frame to show it.
static void guilog(Gui *g, Str msg)
{
    scheddirty(&g->sched);
    msg = clone(&g->perm, msg);  // first: msg may be in frame scratch
    Arena a = g->frame;
    Buf   b(&a, msg.len+64);
    print(&b, "harness: ");
    print(&b, msg);
    putbyte(&b, '\n');
    os_print(g->os, 2, finish(&b));
    if (g->logfd >= 0) {
        os_logwrite(g->os, g->logfd, finish(&b));
    }
    if (g->logs.len >= LOG_MAX) {
        for (iz i = 1; i < g->logs.len; i++) {
            g->logs[i-1] = g->logs[i];
        }
        g->logs.len--;
    }
    g->logs = push(&g->perm, g->logs, msg);
}

static void fakecmd(Gui *g, u32 type, Str payload)
{
    mailpost(g->outbox, type, 0, payload, 1);
}

static void fakeint(Gui *g, u32 type, i64 v)
{
    mailpost(g->outbox, type, (u64)v, "", 1);
}

#include "ui.cpp"


// The fake agent

static u64 fakerand(Fake *f)
{
    f->rng = f->rng*0x3243f6a8885a308du + 1;
    return f->rng >> 33;
}

static Str fakepick(Fake *f, Str *list, iz n)
{
    return list[fakerand(f) % (u64)n];
}

static void fakesentence(Fake *f, Buf *b)
{
    static Str words[] = {
        "the", "harness", "agent", "renders", "on", "demand", "and", "every",
        "frame", "settles", "before", "it", "sleeps", "while", "a", "wake",
        "drains", "the", "mailbox", "so", "nothing", "goes", "stale",
        "layout", "is", "cached", "per", "block", "selection", "spans",
        "messages", "copied", "code", "stays", "byte-exact", "über", "naïve",
        "λ-calculus", "東京", "résumé",
    };
    i32 n = 6 + (i32)(fakerand(f) % 14);
    for (i32 i = 0; i < n; i++) {
        Str w = fakepick(f, words, countof(words));
        if (i) putbyte(b, ' ');
        u64 r = fakerand(f) % 40;
        if (r == 0) {
            print(b, "**"); print(b, w); print(b, "**");
        } else if (r == 1) {
            putbyte(b, '*'); print(b, w); putbyte(b, '*');
        } else if (r == 2) {
            putbyte(b, '`'); print(b, w); putbyte(b, '`');
        } else if (r == 3) {
            putbyte(b, '['); print(b, w); print(b, "](https://example.com/"); print(b, w); putbyte(b, ')');
        } else {
            print(b, w);
        }
    }
    print(b, ". ");
}

static void fakecode(Fake *f, Buf *b, i32 lines)
{
    static Str langs[] = {"c", "python", "sh", "js", "go", "make"};
    static Str c[] = {
        "static i32 sum(i32 *a, iz n)", "{", "    i32 r = 0;",
        "    for (iz i = 0; i < n; i++) {", "        r += a[i];  // accumulate",
        "    }", "    return r;", "}",
    };
    static Str py[] = {
        "def fib(n):", "    \"\"\"Fibonacci, slowly.\"\"\"", "    if n < 2:",
        "        return n", "    return fib(n-1) + fib(n-2)  # recurse",
    };
    static Str sh[] = {
        "for f in *.txt; do", "\tcount=$(wc -l < \"$f\")",
        "\techo \"$f: $count\"", "done",
    };
    static Str js[] = {
        "const xs = [1, 2, 3].map(x => x * 2);", "console.log(`doubled: ${xs}`);",
    };
    static Str go[] = {
        "func main() {", "\tvar total int", "\tfor i := 0; i < 10; i++ {",
        "\t\ttotal += i", "\t}", "\tfmt.Println(total)", "}",
    };
    static Str mk[] = {
        "all: harness", "harness: main.o", "\t$(CC) -o $@ $^ $(LDFLAGS)  ",
    };
    struct { Str *lines; iz n; } srcs[] = {
        {c, countof(c)}, {py, countof(py)}, {sh, countof(sh)},
        {js, countof(js)}, {go, countof(go)}, {mk, countof(mk)},
    };
    i32 k = (i32)(fakerand(f) % countof(langs));
    print(b, "```");
    print(b, langs[k]);
    putbyte(b, '\n');
    for (i32 i = 0; i < lines; i++) {
        print(b, srcs[k].lines[i % srcs[k].n]);
        putbyte(b, '\n');
    }
    print(b, "```\n\n");
}

// A markdown reply of about the given size.
static Str fakereply(Fake *f, Arena *a, iz size)
{
    Buf b(a, size+1024);
    i32 n = 0;
    while (b.len < size) {
        switch (fakerand(f) % (n ? 9 : 1)) {
        case 0: case 1: case 2:
            for (i32 i = (i32)(1 + fakerand(f)%3); i > 0; i--) {
                fakesentence(f, &b);
            }
            print(&b, "\n\n");
            break;
        case 3:
            print(&b, "## Section ");
            print(&b, (i64)n);
            print(&b, "\n\n");
            break;
        case 4: {
            b32 ordered = fakerand(f) % 2;
            for (i32 i = 0; i < 3; i++) {
                if (ordered) {
                    print(&b, (i64)(i+1));
                    print(&b, ". ");
                } else {
                    print(&b, i==1 ? Str("- [x] ") : Str("- "));
                }
                fakesentence(f, &b);
                putbyte(&b, '\n');
                if (i==1 && fakerand(f)%2) {
                    print(&b, "   - nested ");
                    fakesentence(f, &b);
                    putbyte(&b, '\n');
                }
            }
            putbyte(&b, '\n');
        } break;
        case 5:
            fakecode(f, &b, (i32)(3 + fakerand(f)%10));
            break;
        case 6:
            print(&b, "| name | value | note |\n|:-----|------:|------|\n");
            for (i32 i = 0; i < 3; i++) {
                print(&b, "| row");
                print(&b, (i64)i);
                print(&b, " | ");
                print(&b, (i64)(fakerand(f)%1000));
                print(&b, " | `x");
                print(&b, (i64)i);
                print(&b, "` |\n");
            }
            putbyte(&b, '\n');
            break;
        case 7:
            print(&b, "> ");
            fakesentence(f, &b);
            print(&b, "\n\n");
            break;
        case 8:
            print(&b, "Escapes: Vec<T> &amp; <thinking> \\*not emphasis\\*.\n\n");
            break;
        }
        n++;
    }
    return finish(&b);
}

static Str fakehuge(Fake *f, Arena *a)
{
    Buf b(a, 1<<18);
    print(&b, "Here is one huge code block:\n\n```c\n");
    for (i32 i = 0; i < 4000; i++) {
        print(&b, "static i32 f");
        print(&b, (i64)i);
        print(&b, "(i32 x) { return x*");
        print(&b, (i64)(fakerand(f)%997));
        print(&b, " + ");
        print(&b, (i64)i);
        print(&b, ";  }\t// line ");
        print(&b, (i64)i);
        putbyte(&b, '\n');
    }
    print(&b, "```\n\nThat was long.\n");
    return finish(&b);
}

// Post a record to the GUI: durable ones in order with seq, deltas
// dropped when the GUI falls behind (it then resyncs).
static void fakesend(Fake *f, EvOut *w, b32 block)
{
    Str line = evend(w);
    mailpost(f->out, MAIL_EVENT, 0, line, block);
}

static EvOut fakebegin(Fake *f, Buf *b, i32 ev)
{
    i64 seq = evdurable(ev) ? ++f->seq : 0;
    return evbegin(b, f->sid, seq, os_time(f->os), ev);
}

static void fakestart(Fake *f, Str prompt)
{
    f->turnmem.beg = f->turnbase;  // the last reply is done with
    Arena *a = &f->turnmem;
    Arena  t = *a;
    Buf    b(&t, 1<<12);

    EvOut w = fakebegin(f, &b, EV_USER);
    putkey(&w.json, "text");
    putstr(&w.json, prompt);
    putkey(&w.json, "via");
    putstr(&w.json, "prompt");
    fakesend(f, &w, 1);

    f->turn++;
    b.len = 0;
    w = fakebegin(f, &b, EV_TURN);
    putkey(&w.json, "n");
    putint(&w.json, f->turn);
    fakesend(f, &w, 1);

    b.len = 0;
    w = fakebegin(f, &b, EV_ATTEMPT);
    putkey(&w.json, "turn");
    putint(&w.json, f->turn);
    putkey(&w.json, "n");
    putint(&w.json, 1);
    putkey(&w.json, "endpoint");
    putstr(&w.json, "fake");
    putkey(&w.json, "model");
    putstr(&w.json, "fake:stream");
    fakesend(f, &w, 1);

    f->reply   = f->huge ? fakehuge(f, a) : fakereply(f, a, 2000 + (iz)(fakerand(f)%6000));
    f->huge    = 0;
    f->sent    = 0;
    f->active  = 1;
    f->stalled = 0;
    f->resume  = 0;
    f->next    = os_ticks(f->os);
}

static void fakefinish(Fake *f, Str why)
{
    Arena t = f->turnmem;
    Buf   b(&t, f->sent + (1<<12));
    EvOut w = fakebegin(f, &b, EV_ASSISTANT);
    putkey(&w.json, "turn");
    putint(&w.json, f->turn);
    putkey(&w.json, "attempt");
    putint(&w.json, 1);
    putkey(&w.json, "blocks");
    putarray(&w.json);
    putobject(&w.json);
    putkey(&w.json, "t");
    putstr(&w.json, "text");
    putkey(&w.json, "text");
    putstr(&w.json, takehead(f->reply, f->sent));
    putend(&w.json);
    putend(&w.json);
    putkey(&w.json, "finish");
    putstr(&w.json, why=="done" ? Str("stop") : why);
    fakesend(f, &w, 1);

    b.len = 0;
    w = fakebegin(f, &b, EV_TURNEND);
    putkey(&w.json, "turn");
    putint(&w.json, f->turn);
    putkey(&w.json, "why");
    putstr(&w.json, why);
    fakesend(f, &w, 1);
    f->active = 0;
}

// Stream the tokens now due: about four bytes each, cut on UTF-8
// boundaries, one record per token, or 25 at a time in bursts.
static void fakestep(Fake *f, i64 now)
{
    i64 interval = 1000000 / (f->tps>0 ? f->tps : 1);
    i32 per      = f->bursts ? 25 : 1;
    if (f->bursts) interval *= 25;
    while (f->active && now >= f->next) {
        if (f->pauses && !f->stalled && f->sent > f->reply.len/3) {
            f->stalled = 1;
            f->resume  = now + 5000000;
            f->next    = f->resume;
            return;
        }
        Arena t = f->turnmem;
        Buf   b(&t, 512);
        for (i32 k = 0; k < per && f->sent < f->reply.len; k++) {
            iz end = f->sent + 1 + (iz)(fakerand(f) % 6);
            end = end<f->reply.len ? end : f->reply.len;
            for (; end<f->reply.len && (f->reply[end]&0xc0)==0x80; end++) {}
            b.len = 0;
            EvOut w = fakebegin(f, &b, EV_DELTA);
            putkey(&w.json, "turn");
            putint(&w.json, f->turn);
            putkey(&w.json, "attempt");
            putint(&w.json, 1);
            putkey(&w.json, "block");
            putint(&w.json, 0);
            putkey(&w.json, "kind");
            putstr(&w.json, "text");
            putkey(&w.json, "text");
            putstr(&w.json, span(f->reply.data+f->sent, f->reply.data+end));
            fakesend(f, &w, 0);
            f->sent = end;
        }
        f->next += interval;
        if (f->sent == f->reply.len) {
            fakefinish(f, "done");
        }
    }
}

static void fakehistory(Fake *f, i64 n)
{
    for (i64 i = 0; i < n; i++) {
        Arena t = f->turnmem;
        Str   s = i%2 ? fakereply(f, &t, 300 + (iz)(fakerand(f)%1200))
                      : Str("Please explain the next part, with an example.");
        Buf   b(&t, s.len + 256);
        EvOut w = {};
        if (i % 2) {
            w = fakebegin(f, &b, EV_ASSISTANT);
            putkey(&w.json, "turn");
            putint(&w.json, ++f->turn);
            putkey(&w.json, "attempt");
            putint(&w.json, 1);
            putkey(&w.json, "blocks");
            putarray(&w.json);
            putobject(&w.json);
            putkey(&w.json, "t");
            putstr(&w.json, "text");
            putkey(&w.json, "text");
            putstr(&w.json, s);
            putend(&w.json);
            putend(&w.json);
        } else {
            w = fakebegin(f, &b, EV_USER);
            putkey(&w.json, "text");
            putstr(&w.json, s);
        }
        fakesend(f, &w, 1);
    }
    Arena t = f->turnmem;
    Buf   b(&t, 256);
    EvOut w = fakebegin(f, &b, EV_NOTE);
    putkey(&w.json, "level");
    putstr(&w.json, "info");
    putkey(&w.json, "text");
    putstr(&w.json, "history loaded");
    fakesend(f, &w, 1);
}

static void fakemain(void *arg)
{
    Fake *f = (Fake *)arg;
    for (;;) {
        i32 ms = -1;
        if (f->active) {
            i64 left = f->next - os_ticks(f->os);
            ms = left>0 ? (i32)((left + 999)/1000) : 0;
        }
        if (ms) {
            mailwait(f->in, ms);
        }
        Mail m = mailtake(f->in);
        for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
            switch (r.type) {
            case FAKE_PROMPT:
                if (f->active) {
                    f->queued = clone(&f->perm, r.data);
                } else {
                    fakestart(f, r.data);
                }
                break;
            case FAKE_STOP:
                if (f->active) fakefinish(f, "cancel");
                f->stream = 0;
                break;
            case FAKE_TPS:     f->tps    = (i32)r.id; break;
            case FAKE_BURSTS:  f->bursts = (b32)r.id; break;
            case FAKE_PAUSES:  f->pauses = (b32)r.id; break;
            case FAKE_HUGE:
                f->huge = 1;
                if (!f->active) fakestart(f, "Show me one huge code block.");
                break;
            case FAKE_HISTORY:
                fakehistory(f, (i64)r.id);
                break;
            case FAKE_STREAM:
                f->stream = 1;
                if (!f->active) fakestart(f, "Keep talking.");
                break;
            case FAKE_QUIT:
                return;
            }
        }
        fakestep(f, os_ticks(f->os));
        if (!f->active && f->queued.len) {
            Str q = f->queued;
            f->queued = {};
            fakestart(f, q);
        } else if (!f->active && f->stream) {
            fakestart(f, "Keep talking.");
        }
    }
}


// Loop callbacks from the platform

static void wakegui(void *ctx)
{
    os_wake((Os *)ctx);
}

// An OS input event went to ImGui.
static void gui_input(Gui *g, i32 kind)
{
    schedinput(&g->sched);
    g->lastinput = os_ticks(g->os);
    g->woke |= WOKE_INPUT;
    g->inputs[g->ninputs++ % countof(g->inputs)] = kind;
    if (kind==INPUT_KEY || kind==INPUT_TEXT || kind==INPUT_BUTTON) {
        // Typing or clicking into a field shows a blinking caret again.
        g->lastkey = os_ticks(g->os);
        ImGui::GetIO().ConfigInputTextCursorBlink = 1;
    }
}

// Apply the held-back deltas.
static void flushdeltas(Gui *g)
{
    for (Str rest = finish(&g->pending); rest.len;) {
        Cut c = cut(rest, '\n');
        viewapply(g->view, g->frame, c.head);
        rest = c.tail;
    }
    g->pendmem.beg = g->pendbase;
    g->pending     = Buf(&g->pendmem, 1<<12);
}

// The wake flag was cleared: drain every record from the engine. Deltas
// are held back for the next content frame, which shows them at most
// STREAM_PERIOD apart however many frames run between; anything else
// applies at once, after the deltas before it.
static void gui_wake(Gui *g)
{
    View *v   = g->view;
    i64   now = os_ticks(g->os);
    g->woke |= WOKE_MAIL;
    Mail m = mailtake(g->inbox);
    b32 other = m.overflow;
    if (m.overflow) {
        v->overflow = 1;
    }
    for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
        if (r.type != MAIL_EVENT) {
            continue;
        }
        // The event name follows sid, seq and ts, well within this.
        Str head = takehead(r.data, r.data.len<160 ? r.data.len : 160);
        if (find(head, "\"ev\":\"delta\"") >= 0) {
            print(&g->pending, r.data);
            continue;
        }
        flushdeltas(g);
        other |= viewapply(v, g->frame, r.data);
        if (g->histreq && find(head, "\"ev\":\"note\"")>=0 &&
                find(r.data, "history loaded")>=0) {
            g->histdone = 1;
        }
    }
    if (v->busy && !g->wasbusy) {
        g->turnstart = now;
    }
    g->wasbusy = v->busy;

    Sched *s = &g->sched;
    if (other) {
        scheddirty(s);
    } else if (g->pending.len) {
        if (now-g->lastcontent >= STREAM_PERIOD) {
            scheddirty(s);
        } else {
            schedat(s, g->lastcontent+STREAM_PERIOD, DUE_STREAM);
        }
    }
}

// The window was resized, exposed, or must otherwise be presented.
static void gui_force(Gui *g)
{
    g->woke |= WOKE_FORCE;
    schedforce(&g->sched);
}

static void gui_minimized(Gui *g, b32 minimized)
{
    schedminimize(&g->sched, minimized);
}

// The OS runs a modal loop (Win32 move, resize, menu), from which the
// platform draws on its own.
static void gui_modal(Gui *g, b32 inmodal)
{
    g->inmodal = inmodal;
}

// The window's content scale changed (or was first learned).
static void gui_scale(Gui *g, f32 scale)
{
    if (scale <= 0) {
        scale = 1;
    }
    g->scale = scale;
    ImGuiStyle &st = ImGui::GetStyle();
    st = g->basestyle;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
    g->fonts.size   = 15 * scale;
    g->view->topsdirty = 1;
    schedforce(&g->sched);
}


// Frames

static Str statepath(Gui *g, Str name)
{
    return join(&g->perm, g->statedir, name);
}

static void saveini(Gui *g)
{
    if (!g->inipath.len) {
        return;
    }
    uz   len = 0;
    char const *ini = ImGui::SaveIniSettingsToMemory(&len);
    Str  data = {(u8 *)ini, (iz)len};
    if (!os_write(g->os, g->frame, g->inipath, data)) {
        guilog(g, "could not save the layout");
    }
    g->lastsave = os_ticks(g->os);
    g->idlesaves += g->lastsave-g->lastinput >= 1000000;
    if (g->drive && g->lastinput) {
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: layout saved, ");
        print(&b, (g->lastsave - g->lastinput)/1000);
        print(&b, " ms after the last input");
        guilog(g, finish(&b));
    }
    ImGui::GetIO().WantSaveIniSettings = 0;
}

static void snapshot(Gui *g)
{
    Sched       *s = &g->sched;
    MetricsSnap *m = &g->snap;
    m->rate       = g->last;
    m->frames     = g->total.frames;
    m->presents   = g->total.presents;
    m->wakes      = 0;
    for (i32 i = 0; i < WAKE_COUNT; i++) m->wakes += g->total.wakes[i];
    m->stale      = g->stale;
    m->runaway    = g->runaway;
    m->frameus    = g->frameus;
    m->maxframeus = g->maxframeus;
    m->layoutus   = g->layoutus;
    m->deadline   = g->lastdeadline==I64MAX ? I64MAX : g->lastdeadline - s->now;
    m->duewhy     = g->lastduewhy;
    m->owed       = s->owed;
    m->changing   = s->changing;
    m->busy       = s->busy;
}

// Roll the per-second counters forward to now.
static void rollsecond(Gui *g, i64 now)
{
    if (now-g->second >= 1000000) {
        b32 adjacent = now-g->second < 2000000;
        g->last   = adjacent ? g->cur : Counts{};
        g->cur    = {};
        g->second = now - (now - g->second)%1000000;
    }
}

static void printowner(Buf *b, char const *owner)
{
    Str name = {(u8 *)(owner ? owner : "?"), 0};
    while (name.data[name.len]) name.len++;
    putbyte(b, ' ');
    print(b, name);
}

// A probe frame differed from the screen: log what changed, and what
// happened lately, to find the wake that was missed.
static void stalereport(Gui *g)
{
    Arena a = g->frame;
    Buf   b(&a, 1024);
    print(&b, "STALE: a probe frame differed from the screen; changed:");
    for (i32 i = 0; i < g->nlists; i++) {
        b32 same = 0;
        for (i32 k = 0; k < g->nshown; k++) {
            same |= g->shown[k].owner==g->lists[i].owner &&
                    g->shown[k].hash==g->lists[i].hash;
        }
        if (!same) printowner(&b, g->lists[i].owner);
    }
    print(&b, "; gone:");
    for (i32 k = 0; k < g->nshown; k++) {
        b32 found = 0;
        for (i32 i = 0; i < g->nlists; i++) {
            found |= g->shown[k].owner == g->lists[i].owner;
        }
        if (!found) printowner(&b, g->shown[k].owner);
    }
    if (g->header != g->shownheader) {
        print(&b, "; display size or scale changed");
    }
    // The same lists in another order, or a different number of them.
    b32 order = g->nlists != g->nshown;
    for (i32 i = 0; !order && i < g->nlists; i++) {
        order = g->lists[i].owner!=g->shown[i].owner || g->lists[i].hash!=g->shown[i].hash;
    }
    if (order) {
        print(&b, "; lists now ");
        print(&b, (i64)g->nlists);
        print(&b, ", shown ");
        print(&b, (i64)g->nshown);
        print(&b, ":");
        i32 n = g->nlists>g->nshown ? g->nlists : g->nshown;
        for (i32 i = 0; i < n; i++) {
            print(&b, " [");
            if (i < g->nlists) {
                printowner(&b, g->lists[i].owner);
                putbyte(&b, ' ');
                printhex(&b, g->lists[i].hash, 4);
            }
            print(&b, " |");
            if (i < g->nshown) {
                printowner(&b, g->shown[i].owner);
                putbyte(&b, ' ');
                printhex(&b, g->shown[i].hash, 4);
            }
            putbyte(&b, ']');
        }
    }
    print(&b, "; inputs:");
    i64 n = g->ninputs<8 ? g->ninputs : 8;
    for (i64 i = g->ninputs-n; i < g->ninputs; i++) {
        putbyte(&b, ' ');
        print(&b, inputnames[g->inputs[i%8]]);
    }
    print(&b, "; last deadline: ");
    if (g->lastdeadline == I64MAX) {
        print(&b, "none");
    } else {
        print(&b, duenames[g->lastduewhy]);
    }
    print(&b, "; busy:");
    for (i32 i = 0; i < BUSY_COUNT; i++) {
        if (g->sched.busy & (1u<<i)) {
            putbyte(&b, ' ');
            print(&b, busynames[i]);
        }
    }
    ImGuiContext &c = *GImGui;
    print(&b, "; mouse ");
    print(&b, (i64)c.IO.MousePos.x);
    putbyte(&b, ',');
    print(&b, (i64)c.IO.MousePos.y);
    print(&b, " hovered ");
    printhex(&b, c.HoveredId, 8);
    print(&b, " active ");
    printhex(&b, c.ActiveId, 8);
    guilog(g, finish(&b));
}

static void benchframe(Gui *g, i64 cpu);
static void drivestep(Gui *g);

// One line per frame (--trace): when, the deadline it declared and why,
// and what the scheduler made of it.
static void traceframe(Gui *g, Frame f, u32 busy)
{
    Sched *s = &g->sched;
    Arena  a = g->frame;
    Buf    b(&a, 160);
    print(&b, "frame ");
    print(&b, (s->now - g->started)/1000);
    print(&b, " ms: next ");
    if (g->lastdeadline == I64MAX) {
        print(&b, "none");
    } else {
        print(&b, (g->lastdeadline - s->now)/1000);
        print(&b, " ms ");
        print(&b, duenames[g->lastduewhy]);
    }
    print(&b, f.probe ? Str(", probe") : Str(""));
    print(&b, f.present ? Str(", present") : Str(""));
    print(&b, f.stale ? Str(", STALE") : Str(""));
    print(&b, ", owed ");
    print(&b, (i64)s->owed);
    print(&b, ", changing ");
    print(&b, (i64)s->changing);
    for (i32 i = 0; i < BUSY_COUNT; i++) {
        if (busy & (1u<<i)) {
            print(&b, ", ");
            print(&b, busynames[i]);
        }
    }
    putbyte(&b, '\n');
    os_print(g->os, 2, finish(&b));
}

// Draw a frame, unless one is already being drawn.
static void gui_frame(Gui *g)
{
    Sched *s = &g->sched;
    s->now = os_ticks(g->os);
    if (!schedbegin(s)) {
        return;
    }
    i64 t0 = s->now;
    g->frame.beg = g->framebase;
    g->episode  |= (g->cause & (WOKE_INPUT|WOKE_MAIL|WOKE_FORCE|WOKE_DEADLINE)) != 0;
    g->cause     = 0;
    if (g->unsnapped && s->now>=g->nextsnap) {
        snapshot(g);
        g->unsnapped = 0;
        g->nextsnap = s->now - (s->now - g->second)%1000000 + 1000000;
    }
    g->fonts.gen++;
    g->layoutus = 0;
    viewreset(g->view, 0);
    if (g->pending.len && s->now-g->lastcontent>=STREAM_PERIOD) {
        flushdeltas(g);
        g->lastcontent = s->now;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (s->now-g->lastkey >= CARET_STOP) {
        io.ConfigInputTextCursorBlink = 0;
    }

    os_guinewframe(g->os);
    ImGui::NewFrame();
    dockwindows(g);
    ImGui::Render();
    u32 busy = uiscan();
    // A window whose content size never settles would keep this busy
    // forever: after a few frames, leave it to the hash.
    g->layoutstreak = busy&BUSY_LAYOUT ? g->layoutstreak+1 : 0;
    if (g->layoutstreak > 4) {
        busy &= ~(u32)BUSY_LAYOUT;
    }
    uideadlines(g, s);
    if (g->bench) {
        busy |= BUSY_APP;
    }
    if (g->quitat) {
        schedat(s, g->quitat, DUE_APP);
    }
    if (g->pending.len) {
        schedat(s, g->lastcontent+STREAM_PERIOD, DUE_STREAM);
    }
    if (io.WantSaveIniSettings) {
        saveini(g);
    }

    ImDrawData *dd   = ImGui::GetDrawData();
    u64         hash = drawhash(dd, g->lists, &g->nlists, countof(g->lists), &g->header);
    g->lastdeadline = s->deadline;
    g->lastduewhy   = s->duewhy;
    Frame f = schedend(s, hash, busy);
    if (g->trace) {
        traceframe(g, f, busy);
    }
    i64 cpu = os_ticks(g->os) - t0;
    os_guipresent(g->os, f.present);

    // Bookkeeping, none of which changes this frame's picture.
    rollsecond(g, s->now);
    g->total.frames++;
    g->cur.frames++;
    g->modalframes += g->inmodal;
    g->total.presents += f.present;
    g->cur.presents   += f.present;
    g->frameus    = cpu;
    g->maxframeus = cpu>g->maxframeus ? cpu : g->maxframeus;
    if (f.stale) {
        g->stale++;
        stalereport(g);  // before the screen's record is replaced
    }
    if (f.present) {
        for (i32 i = 0; i < g->nlists; i++) g->shown[i] = g->lists[i];
        g->nshown      = g->nlists;
        g->shownheader = g->header;
    }
    if (f.runaway) {
        g->runaway++;
        guilog(g, "undeclared animation: frames kept changing with no reason given");
    }
    if (g->bench) {
        benchframe(g, cpu);
    }
    if (g->histdone) {
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "history: ");
        print(&b, (i64)g->view->msgs.len);
        print(&b, " messages, first frame ");
        printfloat(&b, (f64)(os_ticks(g->os) - g->histreq)/1000, 1);
        print(&b, " ms after the request");
        guilog(g, finish(&b));
        g->histdone = 0;
        g->histreq  = 0;
    }

    // The overlay's numbers change at most once a second, at the first
    // frame past a second boundary, or by a deadline then if idle. The
    // frames that only show new numbers are not work to report.
    if (!s->owed && !s->busy) {
        g->unsnapped |= g->episode;
        g->episode  = 0;
    }
    if (g->unsnapped && g->metricsshown) {
        schedat(s, g->nextsnap, DUE_CLOCK);
    }
    if (g->quitat && s->now>=g->quitat) {
        g->quit = 1;
    }
    while (g->drive && !g->quit && s->now>=g->driveat) {
        drivestep(g);
    }
    if (g->drive && g->driveat!=I64MAX) {
        schedat(s, g->driveat, DUE_APP);
    }
}

// The loop: wait, dispatch everything, then decide.
static void gui_run(Gui *g)
{
    Sched *s = &g->sched;
    while (!g->quit) {
        s->now  = os_ticks(g->os);
        i64 t   = schedtimeout(s);
        g->woke = 0;
        if (!os_guiwait(g->os, t)) {
            break;
        }
        s->now = os_ticks(g->os);
        if (s->deadline<=s->now && s->duewhy!=DUE_CLOCK) {
            g->cause |= WOKE_DEADLINE;
        }
        g->cause |= g->woke;
        i32 why = WAKE_PACE;
        if (g->woke & WOKE_INPUT) {
            why = WAKE_INPUT;
        } else if (g->woke & WOKE_MAIL) {
            why = WAKE_MAIL;
        } else if (g->woke & WOKE_FORCE) {
            why = WAKE_FORCE;
        } else if (s->shadowat && s->shadowat<=s->now) {
            why = WAKE_PROBE;
        } else if (s->deadline <= s->now) {
            why = WAKE_DEADLINE;
        } else if (g->woke & WOKE_OTHER) {
            why = WAKE_OTHER;
        }
        rollsecond(g, s->now);
        g->total.wakes[why]++;
        g->cur.wakes[why]++;
        if (scheddue(s)) {
            gui_frame(g);
        }
    }
}


// Benchmark: load history, scroll through it, relayout, report

static b32 lessi64(i64 *a, i64 *b)
{
    return *a < *b;
}

static void benchreport(Gui *g, Str what)
{
    Arena a = g->frame;
    i64  *t = alloc<i64>(&a, g->benchn+1);
    for (i32 i = 0; i < g->benchn; i++) {
        t[i] = g->benchtimes[i];
    }
    sort(t, g->benchn, lessi64, a);
    Buf b(&a, 256);
    print(&b, "bench: ");
    print(&b, what);
    print(&b, ": ");
    print(&b, g->view->laidout - g->benchlaid);
    print(&b, " messages laid out, ");
    g->benchlaid = g->view->laidout;
    print(&b, (i64)g->benchn);
    print(&b, " frames, avg ");
    printfloat(&b, g->benchn ? (f64)g->benchsum/g->benchn/1000 : 0, 2);
    static struct { Str name; i32 permille; } ps[] = {
        {"p50", 500}, {"p95", 950}, {"p99", 990},
    };
    for (iz i = 0; i < countof(ps); i++) {
        print(&b, " ms, ");
        print(&b, ps[i].name);
        putbyte(&b, ' ');
        i64 v = g->benchn ? t[(i64)(g->benchn-1)*ps[i].permille/1000] : 0;
        printfloat(&b, (f64)v/1000, 2);
    }
    print(&b, " ms, max ");
    printfloat(&b, (f64)g->benchmax/1000, 2);
    print(&b, " ms");
    guilog(g, finish(&b));
    g->benchn   = 0;
    g->benchsum = 0;
    g->benchmax = 0;
}

static void benchsample(Gui *g, i64 cpu)
{
    if (g->benchn < BENCH_MAX) {
        g->benchtimes[g->benchn++] = cpu;
    }
    g->benchsum += cpu;
    g->benchmax  = cpu>g->benchmax ? cpu : g->benchmax;
}

static void benchframe(Gui *g, i64 cpu)
{
    View *v = g->view;
    switch (g->bench) {
    case 1:  // waiting for the history
        if (v->msgs.len >= 10000 && !g->histreq) {
            g->benchlaid = v->laidout;
            g->bench     = 2;
            g->benchstep = 0;
            g->scrollto  = 0;
        }
        break;
    case 2: {  // scroll top to bottom, half a screen per frame
        if (g->benchstep++ > 2) {
            benchsample(g, cpu);
        }
        Msg *last  = &v->msgs[v->msgs.len-1];
        f64  total = last->top + last->h;
        f64  y     = (f64)g->benchstep * 400 * g->scale;
        if (y > total || g->benchstep > 3000) {
            benchreport(g, "scroll");
            g->bench     = 3;
            g->benchstep = 0;
        } else {
            g->scrollto = y;
        }
    } break;
    case 3:  // relayout: a new width every frame
        if (g->benchstep++) {
            benchsample(g, cpu);
        }
        if (g->benchstep > 30) {
            benchreport(g, "resize relayout");
            g->bench = 0;
            g->quit  = 1;
        } else {
            g->widthadj = (f32)g->benchstep;  // a new width: new layouts
        }
        break;
    }
}


// Scripted checklist (--drive): the staleness checklist from the plan,
// driven by synthetic input through the platform's own event queue.
// Each step's effect is checked against ImGui's state, the staleness
// probe watches every idle gap, and frames are counted during waits.

enum : i32 {
    OP_MOVE,      // move to a target, offset by (dx, dy)
    OP_DOWN,      // left button down
    OP_UP,
    OP_RDOWN,     // right button
    OP_RUP,
    OP_WHEEL,     // dy notches
    OP_KEYDOWN,   // key in dx, ctrl if dy
    OP_KEYUP,
    OP_TEXT,
    OP_RESIZE,    // to dx by dy
    OP_CHECK,     // target is a CHK_*
    OP_IDLE,      // log frames since the last OP_MARK
    OP_MARK,
    OP_SELCODE,   // select the first code block in the transcript
    OP_MODALSIZE, // Win32: a modal resize loop, dx steps wider
    OP_DPI,       // Win32: a DPI change to dx
    OP_STREAM,    // the fake agent streams continuously at dx tok/s
    OP_STOP,      // the fake agent stops
    OP_RATES,     // log frames, presents and wakes per second since OP_MARK
    OP_QUIT,
};

enum : i32 {
    T_NONE,
    T_PROMPTBTN,
    T_MENU,
    T_HISTORY,
    T_MODALITEM,
    T_MODALOK,
    T_TRANSCRIPT,
    T_PROMPT,
    T_EMPTY,       // an empty part of the toolbar
    T_SPLITTER,    // the dock splitter between the columns
    T_HELPTAB,
    T_TRANSCRIPTTAB,
    T_BELOW,       // below the transcript's bottom edge
    T_COPYITEM,    // "Copy" in the transcript's context menu
};

enum : i32 {
    CHK_TOOLTIP,
    CHK_NOTOOLTIP,
    CHK_POPUP,
    CHK_NOPOPUP,
    CHK_MODAL,
    CHK_NOMODAL,
    CHK_SPLIT,
    CHK_HELPTAB,
    CHK_TRANSCRIPTTAB,
    CHK_NAVWIN,
    CHK_NONAVWIN,
    CHK_RESIZED,
    CHK_TYPED,
    CHK_BLINKOFF,
    CHK_HISTORY,
    CHK_SCROLLED,
    CHK_SELECTED,
    CHK_COPIED,
    CHK_SAVED,
    CHK_NOSTALE,
    CHK_MODALSIZE,
    CHK_DPI,
    CHK_DPIBACK,
};

static Str chknames[] = {
    "tooltip shown", "tooltip gone", "popup open", "popup closed",
    "modal open", "modal closed", "splitter moved", "Help tab selected",
    "Transcript tab selected", "Ctrl+Tab switcher up", "switcher gone",
    "window resized", "prompt typed", "caret stopped blinking",
    "history loaded", "transcript scrolled", "selection spans messages",
    "code copied byte-exact", "layout saved while idle", "no stale frames",
    "frames drawn inside the modal resize loop",  // Win32; SDL: simulated
    "fonts and style rescaled for 144 DPI", "back at the original scale",
};

struct DriveOp {
    i32 op;
    i32 target;
    f32 dx, dy;
    i32 ms;      // wait before the next op
    Str text;
};

static DriveOp driveops[] = {
    {OP_MARK, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_TRANSCRIPTTAB, 0, 0, 0, ""},
    // Tooltip on a toolbar button, appearing with no event at all
    {OP_MOVE, T_PROMPTBTN, 0, 0, 1000, ""},
    {OP_CHECK, CHK_TOOLTIP, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 700, ""},
    {OP_CHECK, CHK_NOTOOLTIP, 0, 0, 0, ""},
    // A popup
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_POPUP, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_NOPOPUP, 0, 0, 0, ""},
    // A modal, whose dimming fades in and out over several frames
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_MODALITEM, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_MOVE, T_MODALOK, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    // Dragging the dock splitter
    {OP_MOVE, T_SPLITTER, 0, 0, 300, ""},
    {OP_DOWN, 0, 0, 0, 50, ""},
    {OP_MOVE, T_SPLITTER, -10, 0, 30, ""},
    {OP_MOVE, T_SPLITTER, -20, 0, 30, ""},
    {OP_MOVE, T_SPLITTER, -40, 0, 30, ""},
    {OP_MOVE, T_SPLITTER, -60, 0, 30, ""},
    {OP_MOVE, T_SPLITTER, -80, 0, 300, ""},
    {OP_UP, 0, 0, 0, 6500, ""},
    {OP_CHECK, CHK_SPLIT, 0, 0, 0, ""},
    // The drag changed the layout, which is saved 5 s later with no
    // event at all
    {OP_CHECK, CHK_SAVED, 0, 0, 0, ""},
    // Switching tabs
    {OP_MOVE, T_HELPTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_HELPTAB, 0, 0, 0, ""},
    {OP_MOVE, T_TRANSCRIPTTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_TRANSCRIPTTAB, 0, 0, 0, ""},
    // Ctrl+Tab, held
    {OP_KEYDOWN, 0, GKEY_CTRL, 1, 50, ""},
    {OP_KEYDOWN, 0, GKEY_TAB, 1, 50, ""},
    {OP_KEYUP, 0, GKEY_TAB, 1, 600, ""},
    {OP_CHECK, CHK_NAVWIN, 0, 0, 0, ""},
    {OP_KEYUP, 0, GKEY_CTRL, 0, 800, ""},
    {OP_CHECK, CHK_NONAVWIN, 0, 0, 0, ""},
    // Resizing the window
    {OP_RESIZE, 0, 1100, 700, 800, ""},
    {OP_CHECK, CHK_RESIZED, 0, 0, 0, ""},
    {OP_RESIZE, 0, 1280, 800, 800, ""},
    // Windows' own modal resize loop, during which streaming goes on
    {OP_STREAM, 0, 100, 0, 500, ""},
    {OP_MODALSIZE, 0, 12, 0, 1500, ""},
    {OP_CHECK, CHK_MODALSIZE, 0, 0, 0, ""},
    {OP_STOP, 0, 0, 0, 300, ""},
    {OP_RESIZE, 0, 1280, 800, 800, ""},
    // Moving to a monitor of another DPI
    {OP_DPI, 0, 144, 0, 800, ""},
    {OP_CHECK, CHK_DPI, 0, 0, 0, ""},
    {OP_DPI, 0, 0, 0, 800, ""},  // back to the monitor's own
    {OP_CHECK, CHK_DPIBACK, 0, 0, 0, ""},
    // Typing, then idle for 15 s: the caret blinks for 10 s, then stops
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 300, "Hello, harness"},
    {OP_CHECK, CHK_TYPED, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 5000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 10-15 s after typing"},
    {OP_CHECK, CHK_BLINKOFF, 0, 0, 0, ""},
    // History, then the mouse wheel over the transcript
    {OP_MOVE, T_HISTORY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 4000, ""},
    {OP_CHECK, CHK_HISTORY, 0, 0, 0, ""},
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_WHEEL, 0, 0, 10, 50, ""},
    {OP_WHEEL, 0, 0, 10, 800, ""},
    {OP_CHECK, CHK_SCROLLED, 0, 0, 0, ""},
    // Drag-selecting past the bottom edge, which scrolls while held
    {OP_MOVE, T_TRANSCRIPT, 0, -80, 100, ""},
    {OP_DOWN, 0, 0, 0, 100, ""},
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_MOVE, T_BELOW, 0, 30, 1200, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_SELECTED, 0, 0, 0, ""},
    // Copy a code block through the context menu and the clipboard
    {OP_SELCODE, 0, 0, 0, 300, ""},
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_COPYITEM, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_COPIED, 0, 0, 0, ""},
    // Idle: no frames at all once the probe has run
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Idle: nothing for a minute after startup settles.
static DriveOp idleops[] = {
    {OP_MARK, 0, 0, 0, 10000, ""},  // startup settles
    {OP_MARK, 0, 0, 0, 60000, ""},
    {OP_RATES, 0, 0, 0, 0, "idle 60 s"},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Streaming at 100 tokens per second for 30 s.
static DriveOp streamops[] = {
    {OP_STREAM, 0, 100, 0, 3000, ""},
    {OP_MARK, 0, 0, 0, 30000, ""},
    {OP_RATES, 0, 0, 0, 0, "streaming 100 tok/s, 30 s"},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

static ImVec2 rectcenter(Rect r)
{
    return v2((r.x0+r.x1)/2, (r.y0+r.y1)/2);
}

// A dock tab's center, from its window's tab bar.
static ImVec2 tabcenter(char const *name)
{
    ImGuiWindow *w = ImGui::FindWindowByName(name);
    if (!w || !w->DockNode || !w->DockNode->TabBar) {
        return v2(-1, -1);
    }
    ImGuiTabBar  *tb  = w->DockNode->TabBar;
    ImGuiTabItem *tab = ImGui::TabBarFindTabByID(tb, w->TabId);
    if (!tab) {
        return v2(-1, -1);
    }
    return v2(tb->BarRect.Min.x + tab->Offset + tab->Width/2,
              (tb->BarRect.Min.y + tb->BarRect.Max.y)/2);
}

static ImGuiDockNode *rootsplit(Gui *g)
{
    ImGuiDockNode *n = ImGui::DockBuilderGetNode(g->dockspace);
    return n && n->ChildNodes[0] ? n : 0;
}

static ImVec2 targetpos(Gui *g, i32 t)
{
    switch (t) {
    case T_PROMPTBTN:  return rectcenter(g->rects[RECT_PROMPTBTN]);
    case T_MENU:       return rectcenter(g->rects[RECT_MENU]);
    case T_HISTORY:    return rectcenter(g->rects[RECT_HISTORY]);
    case T_MODALITEM:  return rectcenter(g->rects[RECT_MODALITEM]);
    case T_MODALOK:    return rectcenter(g->rects[RECT_MODALOK]);
    case T_TRANSCRIPT: return rectcenter(g->rects[RECT_TRANSCRIPT]);
    case T_PROMPT:     return rectcenter(g->rects[RECT_PROMPT]);
    case T_COPYITEM:   return rectcenter(g->rects[RECT_COPYITEM]);
    case T_EMPTY: {
        Rect r = g->rects[RECT_TOOLBAR];
        return v2(r.x1 - 40*g->scale, (r.y0+r.y1)/2);
    }
    case T_SPLITTER: {
        ImGuiDockNode *n = rootsplit(g);
        if (!n) return v2(-1, -1);
        ImGuiDockNode *l = n->ChildNodes[0];
        return v2(l->Pos.x + l->Size.x + 1, l->Pos.y + l->Size.y/2);
    }
    case T_HELPTAB:       return tabcenter("Help");
    case T_TRANSCRIPTTAB: return tabcenter("Transcript");
    case T_BELOW: {
        Rect r = g->rects[RECT_TRANSCRIPT];
        return v2((r.x0+r.x1)/2, r.y1);
    }
    }
    return v2(0, 0);
}

static b32 tooltipup()
{
    ImGuiContext &c = *GImGui;
    for (i32 i = 0; i < c.Windows.Size; i++) {
        ImGuiWindow *w = c.Windows[i];
        if ((w->Flags & ImGuiWindowFlags_Tooltip) && w->Active && !w->Hidden) {
            return 1;
        }
    }
    return 0;
}

static b32 tabselected(char const *name)
{
    ImGuiWindow *w = ImGui::FindWindowByName(name);
    return w && w->DockTabIsVisible;
}

static b32 drivecheck(Gui *g, i32 chk)
{
    ImGuiContext &c = *GImGui;
    switch (chk) {
    case CHK_TOOLTIP:       return tooltipup();
    case CHK_NOTOOLTIP:     return !tooltipup();
    case CHK_POPUP:         return c.OpenPopupStack.Size > 0;
    case CHK_NOPOPUP:       return c.OpenPopupStack.Size == 0;
    case CHK_MODAL:         return ImGui::GetTopMostPopupModal() != 0;
    case CHK_NOMODAL:       return !ImGui::GetTopMostPopupModal() && c.DimBgRatio==0;
    case CHK_SPLIT: {
        ImVec2 p = targetpos(g, T_SPLITTER);
        return p.x>0 && p.x < g->splitx - 40;
    }
    case CHK_HELPTAB:       return tabselected("Help");
    case CHK_TRANSCRIPTTAB: return tabselected("Transcript");
    case CHK_NAVWIN:        return c.NavWindowingTarget != 0;
    case CHK_NONAVWIN:      return !c.NavWindowingTarget && c.NavWindowingHighlightAlpha==0;
    case CHK_RESIZED:       return (i32)c.IO.DisplaySize.x == 1100;
    case CHK_TYPED: {
        Str p = {(u8 *)g->prompt, 0};
        while (p.data[p.len]) p.len++;
        return p == "Hello, harness";
    }
    case CHK_BLINKOFF:      return !c.IO.ConfigInputTextCursorBlink;
    case CHK_HISTORY:       return g->view->msgs.len >= 10000;
    case CHK_SCROLLED:      return !g->stick;
    case CHK_SELECTED: {
        Range r = selrange(&g->view->sel);
        return g->view->sel.active && r.end.msg > r.beg.msg;
    }
    case CHK_COPIED: {
        char const *clip = ImGui::GetClipboardText();
        Str got = {(u8 *)clip, 0};
        while (clip && got.data[got.len]) got.len++;
        return clip && g->drivecode.len && got==g->drivecode;
    }
    case CHK_SAVED:         return g->idlesaves > 0;
    case CHK_NOSTALE:       return g->stale == 0;
    case CHK_DPI:
        return g->scale==1.5f && ImGui::GetStyle().FontScaleDpi==1.5f &&
               g->fonts.size==15*1.5f;
    case CHK_DPIBACK:
        return g->scale==g->dpibase && ImGui::GetStyle().FontScaleDpi==g->dpibase;
    case CHK_MODALSIZE: {
        Arena a = g->frame;
        Buf   b(&a, 128);
        #ifdef _WIN32
        print(&b, "drive: modal resize: ");
        print(&b, g->modalframes);
        print(&b, " frames inside the loop, width now ");
        print(&b, (i64)c.IO.DisplaySize.x);
        guilog(g, finish(&b));
        return g->modalframes>2 && (i32)c.IO.DisplaySize.x!=1280;
        #else
        print(&b, "drive: simulated live-resize exposes: ");
        print(&b, g->modalframes);
        print(&b, " frames drawn from the event watch");
        guilog(g, finish(&b));
        return g->modalframes > 2;
        #endif
    }
    }
    return 0;
}

// Select a code block with both tabs and trailing spaces, the bytes an
// editor is likeliest to mangle, remembering its code to compare with
// the clipboard.
static void selectcode(Gui *g)
{
    View *v = g->view;
    for (i32 i = 0; i < v->msgs.len; i++) {
        Doc *d = msgdoc(v, i);
        for (i32 k = 0; k < d->blocks.len; k++) {
            Block *b = &d->blocks[k];
            if (b->type!=BLOCK_CODE || !b->npiece) continue;
            Buf code(&g->perm, 256);
            for (i32 j = 0; j < b->npiece; j++) {
                print(&code, d->pieces[b->piece0+j].text);
            }
            Str c = finish(&code);
            if (find(c, "\t")<0 || find(c, " \n")<0) continue;
            g->drivecode   = c;
            v->sel         = {};
            v->sel.active  = 1;
            v->sel.anchor  = {i, b->off0};
            v->sel.anchor1 = v->sel.anchor;
            // Past the last piece, to take the code's final newline.
            v->sel.focus   = {i, b->off1 + 1};

            Arena a = g->frame;
            Buf   m(&a, 128);
            i64   tabs = 0, trailing = 0;
            for (iz n = 0; n < c.len; n++) {
                tabs     += c[n] == '\t';
                trailing += c[n]==' ' && n+1<c.len && c[n+1]=='\n';
            }
            print(&m, "drive: copying ");
            print(&m, c.len);
            print(&m, " bytes of code with ");
            print(&m, tabs);
            print(&m, " tabs and ");
            print(&m, trailing);
            print(&m, " lines with trailing spaces");
            guilog(g, finish(&m));
            return;
        }
    }
    guilog(g, "drive: no code block with tabs and trailing spaces");
}

static void drivestep(Gui *g)
{
    if (g->drivepc >= g->nops) {
        g->driveat = I64MAX;  // the script is done
        return;
    }
    DriveOp *op = &g->ops[g->drivepc++];
    GuiEvent e  = {};
    g->driveat  = g->sched.now + (i64)op->ms*1000;
    switch (op->op) {
    case OP_MOVE: {
        ImVec2 p = targetpos(g, op->target);
        if (op->target==T_SPLITTER && g->splitx==0) {
            g->splitx = p.x;
        }
        e.kind = GEV_MOVE;
        e.x    = p.x + op->dx*g->scale;
        e.y    = p.y + op->dy*g->scale;
        g->drivex = e.x;
        g->drivey = e.y;
        os_guiinject(g->os, &e);
    } break;
    case OP_DOWN:
    case OP_UP:
    case OP_RDOWN:
    case OP_RUP:
        e.kind   = GEV_BUTTON;
        e.x      = g->drivex;
        e.y      = g->drivey;
        e.button = op->op==OP_RDOWN || op->op==OP_RUP;
        e.down   = op->op==OP_DOWN || op->op==OP_RDOWN;
        os_guiinject(g->os, &e);
        break;
    case OP_WHEEL:
        e.kind = GEV_WHEEL;
        e.x    = g->drivex;
        e.y    = op->dy;
        os_guiinject(g->os, &e);
        break;
    case OP_KEYDOWN:
    case OP_KEYUP:
        e.kind = GEV_KEY;
        e.key  = (i32)op->dx;
        e.ctrl = op->dy != 0;
        e.down = op->op == OP_KEYDOWN;
        os_guiinject(g->os, &e);
        break;
    case OP_TEXT:
        e.kind = GEV_TEXT;
        e.text = op->text;
        os_guiinject(g->os, &e);
        break;
    case OP_MODALSIZE:
        g->modalframes = 0;
        e.kind = GEV_MODALSIZE;
        e.x    = op->dx;
        os_guiinject(g->os, &e);
        break;
    case OP_DPI:
        #ifdef _WIN32
        if (!g->dpibase) {
            g->dpibase = g->scale;
        }
        e.kind = GEV_DPI;
        e.x    = op->dx ? op->dx : g->dpibase*96;
        os_guiinject(g->os, &e);
        #endif
        break;
    case OP_RESIZE:
        e.kind = GEV_RESIZE;
        e.x    = op->dx;
        e.y    = op->dy;
        os_guiinject(g->os, &e);
        break;
    case OP_CHECK: {
        if ((op->target==CHK_DPI || op->target==CHK_DPIBACK) && g->dpibase==0) {
            guilog(g, "drive: skip DPI change (Win32 only)");
            break;
        }
        b32 ok = drivecheck(g, op->target);
        g->drivechecks++;
        g->drivefails += !ok;
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: ");
        print(&b, ok ? Str("ok   ") : Str("FAIL "));
        print(&b, chknames[op->target]);
        guilog(g, finish(&b));
    } break;
    case OP_MARK:
        g->driveframes = g->total.frames;
        g->markcounts  = g->total;
        g->markat      = g->sched.now;
        break;
    case OP_STREAM:
        fakeint(g, FAKE_TPS, (i64)op->dx);
        fakeint(g, FAKE_STREAM, 1);
        break;
    case OP_STOP:
        fakecmd(g, FAKE_STOP, "");
        break;
    case OP_RATES: {
        // This frame is the script's own, so it is left out.
        f64    secs = (f64)(g->sched.now - g->markat) / 1e6;
        Counts d    = g->total;
        d.frames   -= g->markcounts.frames + 1;
        d.presents -= g->markcounts.presents;
        i64 wakes = 0;
        for (i32 i = 0; i < WAKE_COUNT; i++) {
            d.wakes[i] -= g->markcounts.wakes[i];
            wakes += d.wakes[i];
        }
        wakes--;
        Arena a = g->frame;
        Buf   b(&a, 256);
        print(&b, "rates: ");
        print(&b, op->text);
        print(&b, ": frames ");
        print(&b, d.frames);
        print(&b, " (");
        printfloat(&b, (f64)d.frames/secs, 1);
        print(&b, "/s), presents ");
        print(&b, d.presents);
        print(&b, " (");
        printfloat(&b, (f64)d.presents/secs, 1);
        print(&b, "/s), wakes ");
        print(&b, wakes);
        print(&b, " (");
        printfloat(&b, (f64)wakes/secs, 2);
        print(&b, "/s: input ");
        print(&b, d.wakes[WAKE_INPUT]);
        print(&b, " mail ");
        print(&b, d.wakes[WAKE_MAIL]);
        print(&b, " deadline ");
        print(&b, d.wakes[WAKE_DEADLINE]-1);
        print(&b, " probe ");
        print(&b, d.wakes[WAKE_PROBE]);
        print(&b, " pace ");
        print(&b, d.wakes[WAKE_PACE]);
        print(&b, " other ");
        print(&b, d.wakes[WAKE_OTHER]);
        print(&b, "), max frame ");
        printfloat(&b, (f64)g->maxframeus/1000, 2);
        print(&b, " ms, stale ");
        print(&b, g->stale);
        guilog(g, finish(&b));
    } break;
    case OP_IDLE: {
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: frames during ");
        print(&b, op->text);
        print(&b, ": ");
        print(&b, g->total.frames - g->driveframes);
        guilog(g, finish(&b));
    } break;
    case OP_SELCODE:
        selectcode(g);
        scheddirty(&g->sched);
        break;
    case OP_QUIT: {
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: ");
        print(&b, (i64)g->drivechecks);
        print(&b, " checks, ");
        print(&b, (i64)g->drivefails);
        print(&b, " failed, ");
        print(&b, g->stale);
        print(&b, " stale");
        guilog(g, finish(&b));
        g->quit = 1;
    } break;
    }
}


// Setup

static b32 guiwanted(Str *args, i32 nargs)
{
    return nargs<2 || args[1]=="gui";
}

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const robotottf[] = {
    #embed "../assets/Roboto-Medium.ttf"
};
static u8 const monottf[] = {
    #embed "../assets/JetBrainsMono-Regular.ttf"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

static ImFont *addfont(u8 const *data, iz len, i32 flags)
{
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = 0;
    cfg.FontLoaderFlags      = (u32)flags;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        (void *)data, (i32)len, 15, &cfg
    );
}

static void loadfonts(Gui *g)
{
    i32 bold   = ImGuiFreeTypeLoaderFlags_Bold;
    i32 italic = ImGuiFreeTypeLoaderFlags_Oblique;
    Fonts *f = &g->fonts;
    f->prose      = addfont(robotottf, countof(robotottf), 0);
    f->bold       = addfont(robotottf, countof(robotottf), bold);
    f->italic     = addfont(robotottf, countof(robotottf), italic);
    f->bolditalic = addfont(robotottf, countof(robotottf), bold|italic);
    f->mono       = addfont(monottf, countof(monottf), 0);
    f->monobold   = addfont(monottf, countof(monottf), bold);
    f->size       = 15;
    g->measure.advance = fontadvance;
    g->measure.font    = fontmetrics;
    g->measure.ctx     = f;
}

static Str findstatedir(Os *os, Arena *perm)
{
    Str dir = os_getenv(os, perm, "XDG_STATE_HOME");
    if (dir.len) {
        return join(perm, dir, "harness");
    }
    #ifdef _WIN32
    dir = os_getenv(os, perm, "LOCALAPPDATA");
    if (dir.len) {
        return join(perm, dir, "harness");
    }
    #endif
    dir = os_getenv(os, perm, "HOME");
    if (dir.len) {
        return join(perm, dir, ".local/state/harness");
    }
    return {};
}

static i64 optint(Str *args, i32 nargs, i32 *i)
{
    if (*i+1 >= nargs) {
        return -1;
    }
    Int r = parseint(args[++*i]);
    return r.ok ? r.val : -1;
}

static Str guiusage =
"usage: harness [gui [options]]\n"
"  --tps N        fake agent tokens per second (default 100)\n"
"  --stream       stream continuously\n"
"  --history N    load N messages of history at startup\n"
"  --bench        load 10k messages, scroll through them, relayout, quit\n"
"  --drive        run the staleness checklist with synthetic input, quit\n"
"  --idle         measure a minute of idling, quit\n"
"  --streamtest   measure 30 s of streaming at 100 tok/s, quit\n"
"  --probe        staleness probe on (default in debug builds)\n"
"  --no-probe     staleness probe off\n"
"  --aggressive   probe every second while idle\n"
"  --trace        log every frame and why it ran\n"
"  --quit-after S quit after S seconds\n"
"  --statedir DIR layout file and log location\n";

extern "C" void harness_imassert(char const *expr, char const *file, int line)
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    Gui          *g   = ctx ? (Gui *)ImGui::GetIO().UserData : 0;
    if (g) {
        Arena a = g->frame;
        Buf   b(&a, 256);
        Str   e = {(u8 *)expr, 0};
        Str   f = {(u8 *)file, 0};
        while (e.data[e.len]) e.len++;
        while (f.data[f.len]) f.len++;
        print(&b, "imgui assertion failed: ");
        print(&b, e);
        print(&b, " (");
        print(&b, f);
        putbyte(&b, ':');
        print(&b, (i64)line);
        putbyte(&b, ')');
        guilog(g, finish(&b));
    }
    #ifndef NDEBUG
    __builtin_trap();
    #endif
}

static i32 gui_main(Os *os, Arena mem, Str *args, i32 nargs)
{
    Gui *g = alloc<Gui>(&mem);
    g->os       = os;
    g->started  = os_ticks(os);
    g->logfd    = -1;
    g->scrollto = -1;
    g->second   = g->started;
    g->tps      = 100;
    g->stick    = 1;
    g->firstlayout = 1;
    #ifdef __APPLE__
    g->mac = 1;
    #endif

    b32 probe = 1;
    #ifdef NDEBUG
    probe = 0;
    #endif
    b32 aggressive = 0, stream = 0;
    i64 history = 0;
    i32 bench   = 0;
    for (i32 i = 1 + (nargs>1 && args[1]=="gui"); i < nargs; i++) {
        Str a = args[i];
        if (a == "--tps") {
            g->tps = (i32)optint(args, nargs, &i);
        } else if (a == "--stream") {
            stream = 1;
        } else if (a == "--history") {
            history = optint(args, nargs, &i);
        } else if (a == "--bench") {
            bench    = 1;
            history  = 10000;
            g->drive = 1;  // scripted: no focus, draws when occluded
        } else if (a == "--drive") {
            g->drive = probe = 1;
            g->ops   = driveops;
            g->nops  = countof(driveops);
        } else if (a == "--idle") {
            g->drive = 1;
            g->ops   = idleops;
            g->nops  = countof(idleops);
        } else if (a == "--streamtest") {
            g->drive = 1;
            g->ops   = streamops;
            g->nops  = countof(streamops);
        } else if (a == "--trace") {
            g->trace = 1;
        } else if (a == "--probe") {
            probe = 1;
        } else if (a == "--no-probe") {
            probe = 0;
        } else if (a == "--aggressive") {
            probe = aggressive = 1;
        } else if (a == "--quit-after") {
            i64 s = optint(args, nargs, &i);
            g->quitat = s<0 ? 0 : g->started + s*1000000;
        } else if (a == "--statedir") {
            if (++i < nargs) g->statedir = args[i];
        } else {
            os_print(os, 2, guiusage);
            return 1;
        }
    }
    if (g->tps<=0 || history<0) {
        os_print(os, 2, guiusage);
        return 1;
    }

    // Memory: the frame scratch, the view's arenas, the fake agent's.
    iz mb = (iz)1 << 20;
    g->perm      = newarena(os_reserve(os, 256*mb));
    g->frame     = newarena(os_reserve(os, 1024*mb));
    g->framebase = g->frame.beg;
    g->view = newview(&g->perm, os_reserve(os, 4096*mb), os_reserve(os, 1024*mb),
                      os_reserve(os, 2048*mb), 512*mb);
    if (!g->perm.beg || !g->frame.beg || !g->view->text.beg) {
        os_print(os, 2, "harness: out of memory\n");
        return 2;
    }
    g->prompt     = alloc<char>(&g->perm, PROMPT_MAX);
    g->benchtimes = alloc<i64>(&g->perm, BENCH_MAX);
    g->pendmem  = newarena(os_reserve(os, 256*mb));
    g->pendbase = g->pendmem.beg;
    if (!g->pendmem.beg) {
        os_print(os, 2, "harness: out of memory\n");
        return 2;
    }
    g->pending = Buf(&g->pendmem, 1<<12);

    if (!g->statedir.len) {
        g->statedir = findstatedir(os, &g->perm);
    }
    if (g->statedir.len && os_mkdirs(os, g->frame, g->statedir)) {
        g->inipath = statepath(g, "imgui.ini");
        g->logfd   = os_logopen(os, g->frame, statepath(g, "gui.log"));
    }

    g->inbox  = newmailbox(&g->perm, INBOX_CAP, wakegui, os);
    g->outbox = newmailbox(&g->perm, 1<<16, 0, 0);
    Fake *f = alloc<Fake>(&g->perm);
    f->os      = os;
    f->out     = g->inbox;
    f->in      = g->outbox;
    f->perm    = newarena(os_reserve(os, 256*mb));
    f->turnmem  = newarena(os_reserve(os, 1024*mb));
    f->turnbase = f->turnmem.beg;
    f->sid     = "20261020-000000-fake";
    f->tps     = g->tps;
    f->rng     = 1;
    if (!g->inbox || !g->outbox || !f->perm.beg || !f->turnmem.beg) {
        os_print(os, 2, "harness: could not create mailboxes\n");
        return 2;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.UserData     = g;
    io.IniFilename  = 0;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigErrorRecoveryEnableAssert = 0;
    ImGui::StyleColorsDark();
    g->basestyle = ImGui::GetStyle();
    loadfonts(g);

    if (g->inipath.len) {
        Arena t  = g->frame;
        Read  rd = os_read(os, &t, g->inipath, 1<<20);
        if (rd.err == READ_OK) {
            ImGui::LoadIniSettingsFromMemory((char *)rd.data.data, (uz)rd.data.len);
        }
    }

    g->sched = newsched(os_ticks(os), 16667);
    g->sched.probing    = probe;
    g->sched.aggressive = aggressive;
    if (!os_guiopen(os, g, "harness")) {
        os_print(os, 2, "harness: could not open a window\n");
        return 1;
    }
    g->sched.pace = os_guirefresh(os);

    if (!os_thread(os, fakemain, f)) {
        os_print(os, 2, "harness: could not start the fake agent\n");
        return 2;
    }
    if (history) {
        g->histreq = os_ticks(os);
        fakeint(g, FAKE_HISTORY, history);
    }
    if (stream) {
        fakeint(g, FAKE_STREAM, 1);
    }
    g->bench = bench;

    gui_run(g);

    saveini(g);
    fakeint(g, FAKE_QUIT, 0);
    {
        Arena a = g->frame;
        Buf   b(&a, 512);
        print(&b, "exit: frames ");
        print(&b, g->total.frames);
        print(&b, ", presents ");
        print(&b, g->total.presents);
        print(&b, ", wakes");
        static Str names[] = {"input", "mail", "deadline", "probe", "force", "pace", "other"};
        for (i32 i = 0; i < WAKE_COUNT; i++) {
            putbyte(&b, ' ');
            print(&b, names[i]);
            putbyte(&b, '=');
            print(&b, g->total.wakes[i]);
        }
        print(&b, ", stale ");
        print(&b, g->stale);
        print(&b, ", runaway ");
        print(&b, g->runaway);
        print(&b, ", max frame ");
        printfloat(&b, (f64)g->maxframeus/1000, 2);
        print(&b, " ms");
        guilog(g, finish(&b));
    }
    os_guiclose(os);
    g->splitter.ClearFreeMemory();
    ImGui::DestroyContext();
    return 0;
}

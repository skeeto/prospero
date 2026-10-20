// GUI application: the render loop, the engine connection, the fake
// agent, and the glue between the platform, the scheduler, the view
// models and the UI
//
// The loop (gui_run) waits for the scheduler's timeout, lets the
// platform dispatch everything pending, and then asks the scheduler
// whether to draw. Records from the engine arrive in a mailbox (the
// inbox) whose notify hook is os_wake; the platform clears its wake flag
// before calling gui_wake, which drains every record. Streamed deltas
// and tool output are shown at most every STREAM_PERIOD; anything else
// at once.
//
// The engine runs on its own thread in this process (newengine), or as
// "harness --headless" in a child process (--engine=subprocess), whose
// standard output arrives in the inbox as PROC_DATA records and whose
// standard input takes the commands, or not at all: a fake agent thread
// (--fake) streams markdown at a configurable rate, in bursts or with
// stalls, and loads a large history, for measurements.
//
// Each session the GUI opens or creates has a view model (view.cpp),
// fed only by its records: the GUI never reads engine memory. Records
// lost to a full mailbox, or to a full mailbox in the engine process,
// come back from the session's log, which the GUI reads itself and
// applies from the last record it has (resync).
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
    GUILOG_MAX = 200,     // lines kept in the Log window
    INBOX_CAP  = 1<<21,   // bytes per inbox buffer
    FIELD_MAX  = 1<<10,   // bytes of a settings or dialog field
    REQ_MAX    = 64,      // commands awaiting replies
};

enum : i64 {
    QUIT_GRACE = 6000000,  // µs to wait for the engine to stop
    AMBER_MS   = 15000,    // no data from the model for this long: amber
};

// Where the engine runs
enum : i32 {
    ENGINE_INPROC,
    ENGINE_SUBPROCESS,
    ENGINE_FAKE,
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

// Mailbox record types: the inbox takes MAIL_OUT and MAIL_QUIT from
// the engine (and the fake agent), or PROC_* from the engine process;
// the fake agent takes these.
enum : u32 {
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
    RECT_NEWBTN,
    RECT_NEWCWD,
    RECT_NEWCREATE,
    RECT_ALLOW,
    RECT_DENY,
    RECT_SETTING,
    RECT_CLOSESTOP,
    RECT_CLOSECANCEL,
    RECT_OTHERSESS,
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

// A session the GUI knows: listed by the engine, or opened here.
struct GuiSession {
    GuiSession  *next;
    Str          sid;
    View        *view;      // null until opened or created here
    ImGuiWindow *win;       // its transcript's child window, last frame
    Rect         row;       // where the sidebar drew it last
    b32          stick;     // the transcript follows its bottom
    b32          readonly;  // another process holds it: replayed only
    b32          listed;    // in the latest sessions list
    b32          locked;    // listed as open in another process
    i32          state;     // LIVE_*, from the list until opened
    Str          title;     // from the list until opened
    Str          cwd;
    Str          model;
    i64          mtime;
    i64          openid;    // the open in flight, 0 if none
};

// A command awaiting its reply.
struct Request {
    i64         id;
    i32         cmd;
    GuiSession *s;
};

// One setting in the settings window, from config.get.
struct CfgRow {
    Str   kind;
    Str   name;
    Str   key;
    Str   type;
    Str   value;   // as in effect
    Str   dflt;
    Str   desc;
    Str   file;    // where it is set, null for its default
    i64   line;
    b32   isdflt;
    b32   valid;
    char *buf;     // the field, FIELD_MAX bytes
    b32   sent;    // an edit went out, until the next config.get
    Str   sentval;
};

struct DriveOp;

struct Gui {
    Os                *os;
    Arena              perm;
    Arena              frame;      // scratch, reset every frame
    byte              *framebase;
    Sched              sched;
    View              *view;       // the current session's, or empty
    Mailbox           *inbox;      // engine (or fake agent) to GUI
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
    u32                lasthover;  // ImGui's hovered item at the last frame
    i64                lastkey;
    i64                lastcontent;
    Arena              pendmem;    // deltas held back until a content frame
    byte              *pendbase;
    Buf                pending;    // their records, one per line

    // The engine
    i32                enginekind; // ENGINE_*
    Engine            *engine;     // in this process
    Proc              *proc;       // or in a child process
    Arena              linemem;    // its output's partial line
    byte              *linebase;
    Buf                line;
    Str                cfgpath;    // -c FILE, else null for the default
    i64                nextid;
    Request            reqs[REQ_MAX];
    b32                engineok;   // it said hello
    b32                enginegone; // it stopped
    Str                engineerr;  // why it is unusable, null if fine
    b32                quitting;   // quit was sent: exit once it stops
    i64                quitby;     // µs: exit then regardless
    b32                closereq;   // the user asked to close the window
    i64                overflows;  // times records were lost
    i64                resyncs;
    Region            *replayr;    // log copies for resyncs
    Region            *cmdr;       // commands being written
    iz                 inboxcap;

    // Sessions
    ViewCache         *vcache;
    GuiSession        *sessions;
    GuiSession        *sess;       // the session shown, or null
    GuiSession         nosession;  // the empty view and its scroll state
    Str                lasterr;    // the latest command error

    // Transcript
    f32                lastwidth;
    f64                scrollto;   // -1 if none
    b32                dragging;
    i32                menumsg;    // the message the context menu is for

    // Dialogs and settings
    char              *newcwd;     // the new session's fields
    char              *newmodel;
    Str                newerr;
    b32                opennew;
    i64                newid;      // the new command in flight
    char              *denynote;   // "Deny with note"
    Str                denyreq;
    GuiSession        *denysess;
    b32                opendeny;
    Arena              cfgmem;     // the latest config.get, reset for each
    byte              *cfgbase;
    Json              *cfg;        // its reply, null until one arrives
    Slice<CfgRow>      cfgrows;
    char              *addname;    // a new section's name
    i64                cfgget;     // config.get in flight, 0 if none
    i64                cfgset;     // config.set in flight, 0 if none
    i64                cfgsets;    // config.set replies so far
    Str                cfgstatus;  // the latest config.set outcome
    b32                cfgok;      // and whether it worked

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
    Str                shotdir;    // --shots: where screenshots go
    Str                shotpath;   // the next presented frame goes here
    i32                layoutstreak;
    f32                widthadj;   // benchmark: subtracted from the width

    // Scripted checklist (--drive)
    Str                drivecwd;     // --cwd: where its sessions work
    b32                tracedrive;   // --trace-drive: log where presses land
    GuiSession        *drivemain;    // the session it made
    b32                ambershown;   // the strip went amber
    b32                retryshown;   // the strip counted down to a retry
    b32                bannershown;  // the read-only banner was drawn
    i32                drivewait;    // W_* condition it waits for
    i64                holduntil;    // µs: the inbox is left alone until
    b32                held;         // a wake came during the hold
    Str                cfgsnap;      // the config file before an edit
    i64                tailseen;     // tool.out records for running rows
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
    b32                drivemoved;   // the synthetic mouse has a position
    f32                drivex;       // where it is
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
    if (g->logs.len >= GUILOG_MAX) {
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


// The engine connection: commands out, records in

static void wakegui(void *ctx)
{
    os_wake((Os *)ctx);
}

static GuiSession *findsess(Gui *g, Str sid)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->sid == sid) {
            return s;
        }
    }
    return 0;
}

static GuiSession *addsess(Gui *g, Str sid)
{
    GuiSession *s = alloc<GuiSession>(&g->perm);
    s->sid   = clone(&g->perm, sid);
    s->stick = 1;
    s->state = LIVE_CLOSED;
    s->next  = g->sessions;
    g->sessions = s;
    return s;
}

// The directory of a session's files.
static Str sessdir(Gui *g, Arena *a, Str sid)
{
    return join(a, join(a, g->statedir, "sessions"), sid);
}

// A session's view model, made on first use, which its records fill.
static View *sessview(Gui *g, GuiSession *s)
{
    if (!s->view) {
        iz mb = (iz)1 << 20;
        s->view = newview(&g->perm, g->vcache, os_reserve(g->os, 1024*mb),
                          os_reserve(g->os, 256*mb), os_reserve(g->os, 256*mb));
        s->view->dir = sessdir(g, &g->perm, s->sid);
    }
    return s->view;
}

// The session shown, or none.
static void setcur(Gui *g, GuiSession *s)
{
    g->sess = s;
    g->view = s && s->view ? s->view : g->nosession.view;
    scheddirty(&g->sched);
}

// Remember a command until its reply, in place of the oldest if full.
static void request(Gui *g, i64 id, i32 cmd, GuiSession *s)
{
    Request *slot = &g->reqs[0];
    for (i32 i = 1; i < REQ_MAX; i++) {
        if (g->reqs[i].id < slot->id) {
            slot = &g->reqs[i];  // a free slot (0) or an older command
        }
    }
    slot->id  = id;
    slot->cmd = cmd;
    slot->s   = s;
}

static Request takereq(Gui *g, i64 id)
{
    Request r = {};
    for (i32 i = 0; id && i < REQ_MAX; i++) {
        if (g->reqs[i].id == id) {
            r = g->reqs[i];
            g->reqs[i] = {};
            break;
        }
    }
    return r;
}

// Begin a command for a session (or none) awaiting a reply. Its id is
// g->nextid. Finish it with cmdsend.
static EvOut cmdopen(Gui *g, Buf *b, i32 cmd, GuiSession *s)
{
    i64   id = ++g->nextid;
    EvOut w  = cmdbegin(b, cmd, id);
    if (s) {
        putkey(&w.json, "sid");
        putstr(&w.json, s->sid);
    }
    request(g, id, cmd, s);
    return w;
}

static void fakecommand(Gui *g, Arena scratch, Str line);

// Whether commands can go out: the window draws (Win32: from inside
// os_guiopen) before the engine starts.
static b32 engineup(Gui *g)
{
    return g->enginekind==ENGINE_FAKE || g->engine || g->proc;
}

// Finish a command and send it to the engine. The scratch must follow
// the command's buffer.
static void cmdsend(Gui *g, EvOut *w, Arena scratch)
{
    Str line = evend(w);
    switch (g->enginekind) {
    case ENGINE_INPROC:
        if (g->engine) {
            enginecmd(g->engine, cuttail(line, 1));
        }
        break;
    case ENGINE_SUBPROCESS:
        if (g->proc && !os_procwrite(g->os, g->proc, line)) {
            g->engineerr = "the engine process stopped taking commands";
            scheddirty(&g->sched);
        }
        break;
    case ENGINE_FAKE:
        fakecommand(g, scratch, line);
        break;
    }
}

static void guihello(Gui *g)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256);
    EvOut w = cmdopen(g, &b, CMD_HELLO, 0);
    putkey(&w.json, "proto");
    putint(&w.json, 0);
    putkey(&w.json, "client");
    putstr(&w.json, "gui");
    cmdsend(g, &w, a);
}

// Ask for the sessions list, which arrives as a sessions record.
static void guilist(Gui *g)
{
    if (g->enginekind == ENGINE_FAKE) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdopen(g, &b, CMD_LIST, 0);
    cmdsend(g, &w, a);
}

static void guiconfigget(Gui *g)
{
    if (g->enginekind==ENGINE_FAKE || g->cfgget || !engineup(g)) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdopen(g, &b, CMD_CONFIGGET, 0);
    g->cfgget = g->nextid;
    cmdsend(g, &w, a);
}

// Change one setting; a null value removes it.
static void guiconfigset(Gui *g, Str kind, Str name, Str key, Str value)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 512 + value.len);
    EvOut w = cmdopen(g, &b, CMD_CONFIGSET, 0);
    putkey(&w.json, "kind");
    putstr(&w.json, kind);
    putkey(&w.json, "name");
    putstr(&w.json, name);
    putkey(&w.json, "key");
    putstr(&w.json, key);
    putkey(&w.json, "value");
    if (value.data) {
        putstr(&w.json, value);
    } else {
        putnull(&w.json);
    }
    g->cfgset = g->nextid;
    cmdsend(g, &w, a);
}

// Open a session here, attended, replaying its log from the start, or
// from where its view is after a lost reply.
static void guiopen(Gui *g, GuiSession *s)
{
    View *v = sessview(g, s);
    v->loose = 1;  // a log may skip numbers
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256);
    EvOut w = cmdopen(g, &b, CMD_OPEN, s);
    putkey(&w.json, "from");
    putint(&w.json, v->lastseq);
    putkey(&w.json, "attended");
    putint(&w.json, 1);
    s->openid = g->nextid;
    cmdsend(g, &w, a);
}

// Show a session, opening it here first if needed.
static void guiselect(Gui *g, GuiSession *s)
{
    if (!s->view && g->enginekind!=ENGINE_FAKE) {
        guiopen(g, s);
    }
    setcur(g, s);
}

// A new session in cwd, with the given model or the configured one.
static void guinew(Gui *g, Str cwd, Str model)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 512 + cwd.len + model.len);
    EvOut w = cmdopen(g, &b, CMD_NEW, 0);
    putkey(&w.json, "cwd");
    putstr(&w.json, cwd);
    if (model.len) {
        putkey(&w.json, "model");
        putstr(&w.json, model);
    }
    putkey(&w.json, "attended");
    putint(&w.json, 1);
    g->newid  = g->nextid;
    g->newerr = {};
    cmdsend(g, &w, a);
}

// Send text to the current session: a prompt, or a steer while its turn
// runs.
static void guiprompt(Gui *g, Str text)
{
    GuiSession *s = g->sess;
    if (!s || !s->view || !text.len) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + text.len);
    EvOut w = cmdopen(g, &b, s->view->busy ? CMD_STEER : CMD_PROMPT, s);
    s->stick = 1;  // to see the reply
    putkey(&w.json, "text");
    putstr(&w.json, text);
    cmdsend(g, &w, a);
}

static void guicancel(Gui *g, GuiSession *s)
{
    if (!s || !s->view || !s->view->busy) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_CANCEL, s);
    cmdsend(g, &w, a);
}

static void guianswer(Gui *g, GuiSession *s, Str req, b32 allow, Str note)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + req.len + note.len);
    EvOut w = cmdopen(g, &b, CMD_ANSWER, s);
    putkey(&w.json, "req");
    putstr(&w.json, req);
    putkey(&w.json, "decision");
    putstr(&w.json, allow ? Str("allow") : Str("deny"));
    if (note.len) {
        putkey(&w.json, "note");
        putstr(&w.json, note);
    }
    cmdsend(g, &w, a);
}

// Sessions whose turns would be stopped by quitting.
static i32 running(Gui *g)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        n += s->view && !s->readonly && (s->view->busy || s->view->asks);
    }
    return n;
}

// Stop every turn, which kills their tools, then exit once the engine
// has stopped, or after QUIT_GRACE regardless.
static void startquit(Gui *g)
{
    if (g->quitting) {
        return;
    }
    g->quitting = 1;
    g->quitby   = os_ticks(g->os) + QUIT_GRACE;
    if (g->enginegone) {
        g->quit = 1;
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_QUIT, 0);
    putkey(&w.json, "mode");
    putstr(&w.json, "stop");
    cmdsend(g, &w, a);
    scheddirty(&g->sched);
}


// Records from the engine

// The settings window's rows, from a config.get reply line.
static void loadcfg(Gui *g, Str line)
{
    g->cfgmem.beg = g->cfgbase;
    Arena *a = &g->cfgmem;
    Json  *j = jsonparse(a, clone(a, line)).root;
    g->cfg     = j;
    g->cfgrows = {};
    Json *schema = jsonget(j, "schema");
    for (Json *s = jsonget(j, "settings")->child; s; s = s->next) {
        CfgRow r = {};
        r.kind   = jsonstr(jsonget(s, "kind"));
        r.name   = jsonstr(jsonget(s, "name"));
        r.key    = jsonstr(jsonget(s, "key"));
        r.value  = jsonstr(jsonget(s, "value"));
        r.file   = jsonstr(jsonget(s, "file"));
        r.line   = jsonint(jsonget(s, "line"), 0);
        r.isdflt = jsonint(jsonget(s, "default"), 0) != 0;
        r.valid  = jsonint(jsonget(s, "valid"), 1) != 0;
        for (Json *k = schema->child; k; k = k->next) {
            if (jsonstr(jsonget(k, "kind"))==r.kind && jsonstr(jsonget(k, "key"))==r.key) {
                r.type = jsonstr(jsonget(k, "type"));
                r.dflt = jsonstr(jsonget(k, "default"));
                r.desc = jsonstr(jsonget(k, "desc"));
            }
        }
        // Defaults show as hints, not values: an edit writes a value
        r.buf = alloc<char>(a, FIELD_MAX);
        Str v = r.isdflt ? Str{} : r.value;
        copybytes(r.buf, v.data, v.len<FIELD_MAX ? v.len : FIELD_MAX-1);
        g->cfgrows = push(a, g->cfgrows, r);
    }
}

static void onsessions(Gui *g, Json *j)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        s->listed = 0;
    }
    for (Json *e = jsonget(j, "list")->child; e; e = e->next) {
        Str sid = jsonstr(jsonget(e, "sid"));
        if (!sid.len) {
            continue;
        }
        GuiSession *s = findsess(g, sid);
        s = s ? s : addsess(g, sid);
        Str title = jsonstr(jsonget(e, "title"));
        Str cwd   = jsonstr(jsonget(e, "cwd"));
        Str model = jsonstr(jsonget(e, "model"));
        s->title  = s->title==title ? s->title : clone(&g->perm, title);
        s->cwd    = s->cwd==cwd     ? s->cwd   : clone(&g->perm, cwd);
        s->model  = s->model==model ? s->model : clone(&g->perm, model);
        s->mtime  = jsonint(jsonget(e, "mtime"), 0);
        s->locked = jsonint(jsonget(e, "locked"), 0) != 0;
        s->state  = livestate(jsonstr(jsonget(e, "state")));
        s->listed = 1;
    }
}

static void onreply(Gui *g, Arena scratch, EvRec *r, Str line)
{
    Json   *j   = r->root;
    i64     id  = jsonint(jsonget(j, "id"), 0);
    b32     ok  = jsonint(jsonget(j, "ok"), 0) != 0;
    Str     err = jsonstr(jsonget(j, "err"));
    Request q   = takereq(g, id);
    switch (q.cmd) {
    case CMD_HELLO:
        g->engineok = 1;
        return;

    case CMD_NEW: {
        if (id == g->newid) {
            g->newid = 0;
        }
        if (!ok) {
            g->newerr  = clone(&g->perm, err);
            g->opennew = 1;
            return;
        }
        GuiSession *s = findsess(g, r->sid);
        s = s ? s : addsess(g, r->sid);
        sessview(g, s);
        setcur(g, s);
        guilist(g);
    } return;

    case CMD_OPEN: {
        GuiSession *s = q.s;
        if (!s || s->openid!=id) {
            return;
        }
        s->openid = 0;
        if (s->view) {
            s->view->loose = 0;
        }
        if (!ok) {
            g->lasterr = clone(&g->perm, err.len ? err : Str("cannot open the session"));
            guilog(g, g->lasterr);
            return;
        }
        s->readonly = jsonint(jsonget(j, "readonly"), 0) != 0;
        if (s->view) {
            s->view->readonly  = s->readonly;
            s->view->topsdirty = 1;  // no buttons on its asks
        }
    } return;

    case CMD_CONFIGGET:
        if (g->cfgget == id) {
            g->cfgget = 0;
        }
        if (ok) {
            loadcfg(g, line);
        }
        return;

    case CMD_CONFIGSET: {
        if (g->cfgset == id) {
            g->cfgset = 0;
        }
        g->cfgsets++;
        g->cfgok = ok;
        Buf b(&scratch, 256);
        if (ok) {
            Str path = jsonstr(jsonget(j, "path"));
            i64 at   = jsonint(jsonget(j, "line"), 0);
            print(&b, at ? Str("Saved: ") : Str("Removed from "));
            print(&b, path);
            if (at) {
                putbyte(&b, ':');
                print(&b, at);
            }
        } else {
            print(&b, "Not saved: ");
            print(&b, err);
        }
        g->cfgstatus = clone(&g->perm, finish(&b));
        guiconfigget(g);
    } return;
    }
    if (!ok && err.len) {
        g->lasterr = clone(&g->perm, err);
        guilog(g, g->lasterr);
    }
}

// The session a record line is for, read without parsing it: sid comes
// first when present.
static Str linesid(Str line)
{
    Str key = "{\"sid\":\"";
    if (!startswith(line, key)) {
        return {};
    }
    Str rest = cuthead(line, key.len);
    return cut(rest, '"').head;
}

// Apply one record from the engine. Returns 1 if what shows changed.
static b32 guirecord(Gui *g, Str line)
{
    Arena a = g->frame;
    EvRec r = evparse(&a, line);
    if (!r.ok) {
        return 0;
    }
    switch (r.ev) {
    case EV_REPLY:
        onreply(g, a, &r, line);
        return 1;
    case EV_HELLO:
        g->engineok = 1;
        return 1;
    case EV_SESSIONS:
        onsessions(g, r.root);
        return 1;
    case EV_OVERFLOW:
        if (!r.sid.len) {
            g->overflows++;  // the engine process lost records
            for (GuiSession *s = g->sessions; s; s = s->next) {
                if (s->view) s->view->lost = 1;
            }
            return 1;
        }
    }
    if (!r.sid.len) {
        return 0;
    }

    GuiSession *s = findsess(g, r.sid);
    if (!s || !s->view) {
        if (!r.seq) {
            return 0;  // not shown here
        }
        // A session made here, or records that outran its reply
        s = s ? s : addsess(g, r.sid);
        sessview(g, s);
        if (!g->sess && g->enginekind!=ENGINE_FAKE) {
            setcur(g, s);
        }
    }
    View *v     = s->view;
    i32   state = v->state;
    i32   asks  = v->asks;
    b32   busy  = v->busy;
    if (r.ev==EV_TOOLOUT) {
        i32 i = findcall(v, jsonstr(jsonget(r.root, "call")));
        g->tailseen += i>=0 && v->msgs[i].tool->status==VTOOL_RUNNING;
    }
    b32 changed = viewrecord(v, a, &r);
    if (r.ev==EV_ASK && changed) {
        s->stick = 1;  // an ask needs seeing: scroll to it
    }
    if (s != g->sess) {
        // Only the sidebar shows it
        return state!=v->state || asks!=v->asks || busy!=v->busy ||
               r.ev==EV_TITLE || r.ev==EV_SESSION;
    }
    if (g->histreq && r.ev==EV_NOTE && find(line, "history loaded")>=0) {
        g->histdone = 1;
    }
    return changed;
}

// Records shown on content frames only: model output and tool output,
// and the liveness that comes with them, for the session shown.
static b32 heldback(Gui *g, Str line)
{
    Str head = takehead(line, line.len<160 ? line.len : 160);
    b32 kind = find(head, "\"ev\":\"delta\"")>=0 ||
               find(head, "\"ev\":\"tool.out\"")>=0 ||
               find(head, "\"ev\":\"progress\"")>=0 ||
               find(head, "\"ev\":\"live\"")>=0;
    return kind && g->sess && linesid(line)==g->sess->sid;
}

// Apply the held-back records.
static void flushdeltas(Gui *g)
{
    for (Str rest = finish(&g->pending); rest.len;) {
        Cut c = cut(rest, '\n');
        guirecord(g, c.head);
        rest = c.tail;
    }
    g->pendmem.beg = g->pendbase;
    g->pending     = Buf(&g->pendmem, 1<<12);
}

// One record line from the engine: held back, or applied after those
// held back before it. Returns 1 if what shows changed at once.
static b32 takeline(Gui *g, Str line)
{
    for (; line.len && (line[line.len-1]=='\n' || line[line.len-1]=='\r'); line.len--) {}
    if (!line.len) {
        return 0;
    }
    if (heldback(g, line)) {
        print(&g->pending, line);
        putbyte(&g->pending, '\n');
        return 0;
    }
    flushdeltas(g);
    return guirecord(g, line);
}

// Output of the engine process: whole lines are records.
static b32 procdata(Gui *g, Str data)
{
    print(&g->line, data);
    Str rest  = finish(&g->line);
    b32 other = 0;
    for (;;) {
        Cut c = cut(rest, '\n');
        if (!c.ok) {
            break;
        }
        other |= takeline(g, c.head);
        rest = c.tail;
    }
    iz off = rest.data - g->line.data;
    for (iz i = 0; i < rest.len; i++) {
        g->line.data[i] = g->line.data[off+i];  // the partial line to the front
    }
    g->line.len = rest.len;
    return other;
}

// Records went missing for a session: apply the rest of its log, from
// the last record its view has. Deltas and output lost with them are
// only provisional, and their commits are in the log.
static void resync(Gui *g, GuiSession *s)
{
    View  *v     = s->view;
    Arena  a     = g->frame;
    Arena  lines = newarena(g->replayr);
    Str    path  = join(&a, sessdir(g, &a, s->sid), "events.jsonl");
    Replay rp    = logreplay(g->os, &lines, a, path, s->sid, v->lastseq, LOG_MAX);
    b32    loose = v->loose;
    v->loose = 1;
    for (iz i = 0; i < rp.lines.len; i++) {
        viewapply(v, a, rp.lines[i]);
    }
    v->loose = loose;
    v->lost  = 0;
    g->resyncs++;
}

// After lost records: the commands whose replies may be gone, sent
// again where that is harmless.
static void reissue(Gui *g)
{
    Request old[REQ_MAX];
    for (i32 i = 0; i < REQ_MAX; i++) {
        old[i] = g->reqs[i];
        g->reqs[i] = {};
    }
    for (i32 i = 0; i < REQ_MAX; i++) {
        Request q = old[i];
        if (!q.id) {
            continue;
        }
        switch (q.cmd) {
        case CMD_HELLO:     guihello(g);    break;
        case CMD_LIST:      guilist(g);     break;
        case CMD_CONFIGGET: g->cfgget = 0; guiconfigget(g); break;
        case CMD_OPEN:      if (q.s && q.s->openid==q.id) guiopen(g, q.s); break;
        case CMD_NEW:       g->newid = 0; guilist(g); break;  // it shows there
        }
    }
}

static void gui_close(Gui *);

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
    mailpost(f->out, MAIL_OUT, 0, line, block);
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
                if (f->active) fakefinish(f, "cancel");
                mailpost(f->out, MAIL_QUIT, 0, {}, 1);
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


// The fake agent speaks only some of the protocol: prompts, cancel and
// quit become its own records.
static void fakecommand(Gui *g, Arena scratch, Str line)
{
    Json *j   = jsonparse(&scratch, line).root;
    Str   cmd = jsonstr(jsonget(j, "cmd"));
    if (cmd=="prompt" || cmd=="steer") {
        fakecmd(g, FAKE_PROMPT, jsonstr(jsonget(j, "text")));
    } else if (cmd == "cancel") {
        fakecmd(g, FAKE_STOP, "");
    } else if (cmd == "quit") {
        fakeint(g, FAKE_QUIT, 0);
    }
}


// Loop callbacks from the platform

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

static b32 drivewaitdone(Gui *g);

// The wake flag was cleared: drain every record from the engine. Deltas
// and tool output for the session shown are held back for the next
// content frame, which shows them at most STREAM_PERIOD apart however
// many frames run between; anything else applies at once, after the
// records held back before it. Lost records come back from the logs.
static void gui_wake(Gui *g)
{
    i64 now = os_ticks(g->os);
    g->woke |= WOKE_MAIL;
    if (g->holduntil > now) {
        g->held = 1;  // the checklist lets the inbox fill up
        return;
    }
    g->held = 0;

    Mail m     = mailtake(g->inbox);
    b32  other = 0;
    for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
        switch (r.type) {
        case MAIL_OUT:
            other |= takeline(g, r.data);
            break;
        case PROC_DATA:
            other |= procdata(g, r.data);
            break;
        case MAIL_QUIT:
        case PROC_EXIT:
            g->enginegone = 1;
            other = 1;
            break;
        }
    }

    if (m.overflow && g->enginekind==ENGINE_FAKE) {
        g->view->overflow = 1;
        other = 1;
    } else if (m.overflow) {
        g->overflows++;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            if (s->view) s->view->lost = 1;
        }
        reissue(g);
    }
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->view && s->view->lost && g->enginekind!=ENGINE_FAKE) {
            flushdeltas(g);
            resync(g, s);
            other = 1;
        }
    }

    if (g->enginegone) {
        if (g->quitting) {
            g->quit = 1;
        } else if (!g->engineerr.len) {
            g->engineerr = "the engine stopped";
        }
    }
    if (g->drive && g->drivewait && drivewaitdone(g)) {
        other = 1;
    }

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

// The user asked to close the window, or to quit: with turns running,
// the UI asks first (ui.cpp).
static void gui_close(Gui *g)
{
    g->closereq = 1;
    scheddirty(&g->sched);
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
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->view) s->view->topsdirty = 1;
    }
    g->nosession.view->topsdirty = 1;
    cachereset(g->vcache, 1);  // layouts for the old font size
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
    cachereset(g->vcache, 0);
    if (g->held && s->now>=g->holduntil) {
        gui_wake(g);  // the hold is over: take what piled up
    }
    if (g->pending.len && s->now-g->lastcontent>=STREAM_PERIOD) {
        flushdeltas(g);
        g->lastcontent = s->now;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (s->now-g->lastkey >= CARET_STOP) {
        io.ConfigInputTextCursorBlink = 0;
    }

    os_guinewframe(g->os);
    if (g->drive && g->drivemoved) {
        // The backend reads the real mouse when the window has focus and
        // the mouse is elsewhere: the script's mouse is the one
        io.AddMousePosEvent(g->drivex, g->drivey);
    }
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
    if (g->quitting) {
        schedat(s, g->quitby, DUE_APP);
    }
    if (g->holduntil > s->now) {
        schedat(s, g->holduntil, DUE_APP);
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
    if (g->quitting && s->now>=g->quitby) {
        g->quit = 1;  // the engine did not stop in time
    }
    while (g->drive && !g->quit &&
           (s->now>=g->driveat || (g->drivewait && drivewaitdone(g)))) {
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
        ViewMsg *last  = &v->msgs[v->msgs.len-1];
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


// Screenshots, for looking at what the checklist did: PNG with stored
// (uncompressed) deflate blocks, which needs no compressor.

static u32 crc32(u32 crc, u8 *p, iz len)
{
    crc = ~crc;
    for (iz i = 0; i < len; i++) {
        crc ^= p[i];
        for (i32 k = 0; k < 8; k++) {
            crc = crc>>1 ^ (0xedb88320u & (0u - (crc&1)));
        }
    }
    return ~crc;
}

static void putbe32(Buf *b, u32 v)
{
    putbyte(b, (u8)(v>>24));
    putbyte(b, (u8)(v>>16));
    putbyte(b, (u8)(v>> 8));
    putbyte(b, (u8)(v>> 0));
}

static void pngchunk(Buf *b, Str type, Str data)
{
    putbe32(b, (u32)data.len);
    iz start = b->len;
    print(b, type);
    print(b, data);
    putbe32(b, crc32(0, b->data+start, b->len-start));
}

// A PNG of w by h RGBA pixels, top row first.
static Str pngencode(Arena *a, u8 *rgba, i32 w, i32 h)
{
    iz  row = (iz)w*4 + 1;  // with its filter byte, 0 (none)
    iz  raw = row * h;
    Buf z(a, raw + raw/65535*5 + 64);
    putbyte(&z, 0x78);
    putbyte(&z, 0x01);
    u32 s1 = 1, s2 = 0;
    iz  left = raw, y = 0, x = 0;  // x: position within the row
    while (left > 0) {
        iz n = left<65535 ? left : 65535;
        left -= n;
        putbyte(&z, (u8)!left);  // BFINAL, BTYPE 00
        putbyte(&z, (u8)n);
        putbyte(&z, (u8)(n>>8));
        putbyte(&z, (u8)~n);
        putbyte(&z, (u8)(~n>>8));
        for (iz i = 0; i < n; i++) {
            u8 c = x ? rgba[y*(iz)w*4 + x-1] : 0;
            putbyte(&z, c);
            s1 = (s1 + c) % 65521;
            s2 = (s2 + s1) % 65521;
            if (++x == row) {
                x = 0;
                y++;
            }
        }
    }
    putbe32(&z, s2<<16 | s1);

    Buf b(a, z.len + 128);
    print(&b, "\x89PNG\r\n\x1a\n");
    u8 hdr[13] = {
        (u8)(w>>24), (u8)(w>>16), (u8)(w>>8), (u8)w,
        (u8)(h>>24), (u8)(h>>16), (u8)(h>>8), (u8)h,
        8, 6, 0, 0, 0,  // 8-bit RGBA
    };
    pngchunk(&b, "IHDR", {hdr, countof(hdr)});
    pngchunk(&b, "IDAT", finish(&z));
    pngchunk(&b, "IEND", {});
    return finish(&b);
}


// Scripted checklist (--drive): the staleness checklist from the plan,
// driven by synthetic input through the platform's own event queue.
// Each step's effect is checked against ImGui's state and the view
// models, the staleness probe watches every idle gap, and frames are
// counted during waits. With the fake agent (--fake) it is M1's list;
// with an engine, M3's adds sessions, tool rows, asks, steering,
// cancelling, lost records, a settings edit and closing, against
// tests/fake_provider.py's "gui" scenario (tests/gui.py runs it).

enum : i32 {
    OP_MOVE,      // move to a target, offset by (dx, dy)
    OP_DOWN,      // left button down
    OP_UP,
    OP_RDOWN,     // right button
    OP_RUP,
    OP_WHEEL,     // dy notches
    OP_KEYDOWN,   // key in dx, modifiers (KEY_*) in dy
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
    OP_WAIT,      // until the W_* target holds, for at most ms
    OP_TEXTCWD,   // type the --cwd directory
    OP_NEWSESSION,// a new session in --cwd, without the dialog
    OP_PROMPT,    // send text to the session, without typing it
    OP_HOLD,      // leave the inbox alone for dx ms
    OP_SNAPCFG,   // remember the configuration file
    OP_CLOSE,     // ask the window to close
    OP_SUMMARY,   // log the checks so far
    OP_SHOT,      // with --shots DIR: the next frame as DIR/text.png
    OP_SHOWUSER,  // scroll to the user's message that starts with text
    OP_MAIN,      // show the session the checklist made again
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
    T_NEWBTN,      // the toolbar's "New session"
    T_NEWCWD,      // the new session's directory field
    T_NEWCREATE,
    T_ALLOW,       // the latest open ask's buttons
    T_DENY,
    T_SETTINGSTAB,
    T_SETTING,     // the first endpoint's idle_timeout field
    T_CLOSESTOP,   // the close dialog's buttons
    T_CLOSECANCEL,
    T_OTHERSESS,   // the first session in the sidebar not shown
    T_TOPTEXT,     // near the transcript's top edge
    T_SESSTITLE,   // the sidebar's session whose title starts with text
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
    CHK_ENGINE,
    CHK_SESSION,
    CHK_NEWLINE,
    CHK_SENT,
    CHK_TOOLROWS,
    CHK_DIFF,
    CHK_LIVETAIL,
    CHK_ASKCARD,
    CHK_ASKALLOWED,
    CHK_ASKTIMEOUT,
    CHK_STEERED,
    CHK_CANCELLED,
    CHK_OVERFLOWED,
    CHK_RESYNC,
    CHK_SETTING,
    CHK_CLOSEMODAL,
    CHK_STILLBUSY,
    CHK_SWITCHED,
    CHK_E2ECODE,
    CHK_E2ESTEER,
    CHK_E2EALLOWED,
    CHK_E2ETIMEOUT,
    CHK_REPLAYED,
    CHK_READONLY,
    CHK_AMBER,
    CHK_RETRYSHOWN,
    CHK_DIAGS,
    CHK_COUNT,
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
    "the engine answered", "the new session is shown",
    "Shift+Enter added a line and sent nothing", "Enter sent the prompt",
    "tool rows for read, edit and shell, each ok, with exit code and output file",
    "the edit shows a highlighted diff", "shell output showed while it ran",
    "an ask card with its buttons", "allowed by the user, and the agent went on",
    "denied at its deadline, and the agent went on",
    "a steer reached the agent mid-turn", "Esc cancelled the turn mid-tool",
    "the small inbox overflowed", "the view matches a replay of the log",
    "a settings edit changed one line of the file, comments intact",
    "closing with a turn running asks first", "Cancel kept the turn running",
    "switched back, the other session streaming on",
    "the agent wrote the files and ran the test",
    "the steer reached the agent, and the turn finished",
    "allowed by the user: the file was written outside",
    "denied at the deadline, and the agent read why",
    "a closed session opened, its log replayed",
    "a session held elsewhere opened read-only, with its banner",
    "amber after 15 s without data", "a retry's countdown shown",
    "configuration diagnostics listed with file and line",
};
static_assert(countof(chknames) == CHK_COUNT);

// Conditions OP_WAIT waits for
enum : i32 {
    W_NONE,
    W_ENGINE,     // the engine said hello
    W_SESSION,    // a session is shown and nothing is being created
    W_IDLE,       // the session shown runs no turn
    W_ASK,        // an ask is open in the session shown
    W_TOOL,       // a tool runs in the session shown
    W_CONFIG,     // the configuration is loaded, nothing in flight
    W_CONFIGSET,  // a config.set was answered since OP_SNAPCFG
    W_FOREVER,    // never: the time runs out (the app should have quit)
    W_ALLIDLE,    // no session runs a turn
    W_OPENED,     // the session shown is open, its replay applied
    W_AMBER,      // the strip went amber
    W_BACKOFF,    // the session shown backs off to retry
};

static Str waitnames[] = {
    "", "the engine", "the session", "the turn to end", "an ask",
    "a running tool", "the configuration", "the settings edit", "quitting",
    "every turn to end", "the session to open", "amber", "a retry",
};

enum : i32 {
    KEY_CTRL  = 1,
    KEY_SHIFT = 2,
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
    {OP_KEYDOWN, 0, GKEY_CTRL, KEY_CTRL, 50, ""},
    {OP_KEYDOWN, 0, GKEY_TAB, KEY_CTRL, 50, ""},
    {OP_KEYUP, 0, GKEY_TAB, KEY_CTRL, 600, ""},
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

// M3, against an engine and tests/fake_provider.py's "gui" scenario,
// whose replies the prompts choose.
static DriveOp m3ops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_ENGINE, 0, 0, 0, ""},
    {OP_CHECK, CHK_TRANSCRIPTTAB, 0, 0, 0, ""},
    // Tooltip on a toolbar button, appearing with no event at all
    {OP_MOVE, T_NEWBTN, 0, 0, 1000, ""},
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
    // A new session through its dialog, a modal: the directory typed
    // over the field's suggestion
    {OP_MOVE, T_NEWBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_TEXTCWD, 0, 0, 0, 300, ""},
    {OP_MOVE, T_NEWCREATE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_SESSION, 0, 0, 0, ""},
    // Sessions made elsewhere (tests/gui.py): one closed, opened with
    // its log replayed, and one another process holds, opened read-only
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Say hello."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_REPLAYED, 0, 0, 0, ""},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Hold me"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_READONLY, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "0-readonly"},
    {OP_MAIN, 0, 0, 0, 500, ""},
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
    {OP_KEYDOWN, 0, GKEY_CTRL, KEY_CTRL, 50, ""},
    {OP_KEYDOWN, 0, GKEY_TAB, KEY_CTRL, 50, ""},
    {OP_KEYUP, 0, GKEY_TAB, KEY_CTRL, 600, ""},
    {OP_CHECK, CHK_NAVWIN, 0, 0, 0, ""},
    {OP_KEYUP, 0, GKEY_CTRL, 0, 800, ""},
    {OP_CHECK, CHK_NONAVWIN, 0, 0, 0, ""},
    // Resizing the window
    {OP_RESIZE, 0, 1100, 700, 800, ""},
    {OP_CHECK, CHK_RESIZED, 0, 0, 0, ""},
    {OP_RESIZE, 0, 1280, 800, 800, ""},
    // The system's modal resize loop while a reply streams
    {OP_PROMPT, 0, 0, 0, 500, "Stream slowly for a while."},
    {OP_MODALSIZE, 0, 12, 0, 1500, ""},
    {OP_CHECK, CHK_MODALSIZE, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_RESIZE, 0, 1280, 800, 800, ""},
    // Moving to a monitor of another DPI
    {OP_DPI, 0, 144, 0, 800, ""},
    {OP_CHECK, CHK_DPI, 0, 0, 0, ""},
    {OP_DPI, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_DPIBACK, 0, 0, 0, ""},
    // A coding turn, typed: Shift+Enter adds a line, Enter sends. Tool
    // rows for read, edit (a diff) and shell (its output as it runs)
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "Run the tools: read, edit, then a slow shell command."},
    {OP_KEYDOWN, 0, GKEY_ENTER, KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, KEY_SHIFT, 300, ""},
    {OP_CHECK, CHK_NEWLINE, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 200, "Thank you."},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 500, ""},
    {OP_CHECK, CHK_SENT, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 30000, ""},
    {OP_CHECK, CHK_TOOLROWS, 0, 0, 0, ""},
    {OP_CHECK, CHK_DIFF, 0, 0, 0, ""},
    {OP_CHECK, CHK_LIVETAIL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "1-tools"},
    // The liveness strip: amber after 15 s without data, and a retry's
    // countdown, drawn on deadlines alone
    {OP_PROMPT, 0, 0, 0, 100, "Stall for a while."},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_WAIT, W_AMBER, 0, 0, 30000, ""},
    {OP_IDLE, 0, 0, 0, 0, "15 s of waiting for the model"},
    {OP_CHECK, CHK_AMBER, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "1-amber"},
    {OP_WAIT, W_IDLE, 0, 0, 30000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Fail once."},
    {OP_WAIT, W_BACKOFF, 0, 0, 15000, ""},
    {OP_CHECK, CHK_RETRYSHOWN, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    // The mouse wheel, drag-selecting past the edge, and copying code,
    // over what that turn made: from near the top, after scrolling up
    // past the long answer, so that the selection spans messages
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_WHEEL, 0, 0, 10, 50, ""},
    {OP_WHEEL, 0, 0, 10, 50, ""},
    {OP_WHEEL, 0, 0, 10, 50, ""},
    {OP_WHEEL, 0, 0, 10, 800, ""},
    {OP_CHECK, CHK_SCROLLED, 0, 0, 0, ""},
    {OP_SHOWUSER, 0, 0, 0, 500, "Run the tools"},
    {OP_MOVE, T_TOPTEXT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 100, ""},
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_MOVE, T_BELOW, 0, 30, 1200, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_SELECTED, 0, 0, 0, ""},
    {OP_SELCODE, 0, 0, 0, 300, ""},
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_COPYITEM, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_COPIED, 0, 0, 0, ""},
    // An ask, allowed through its card
    {OP_PROMPT, 0, 0, 0, 100, "Write outside the workspace, ask, and be allowed."},
    {OP_WAIT, W_ASK, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_ASKCARD, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "2-ask"},
    {OP_MOVE, T_ALLOW, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_ASKALLOWED, 0, 0, 0, ""},
    // An ask nobody answers, denied at its deadline, its countdown
    // drawn by deadlines alone
    {OP_PROMPT, 0, 0, 0, 100, "Write outside the workspace, ask, and wait for nobody."},
    {OP_WAIT, W_ASK, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_IDLE, 0, 0, 0, 0, "an ask counting down to its deadline"},
    {OP_CHECK, CHK_ASKTIMEOUT, 0, 0, 0, ""},
    // Sending while a turn runs steers it
    {OP_PROMPT, 0, 0, 0, 100, "Steer me: run a slow command, then answer."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "Use the left door."},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_CHECK, CHK_STEERED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "3-steer"},
    // A second session streams while the first is shown again
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 500, "Stream slowly for a while."},
    {OP_MOVE, T_OTHERSESS, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SWITCHED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "6-sessions"},
    {OP_WAIT, W_ALLIDLE, 0, 0, 20000, ""},
    // Esc cancels a turn mid-tool
    {OP_PROMPT, 0, 0, 0, 100, "Cancel me: run a very slow command."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_CANCELLED, 0, 0, 0, ""},
    // Records lost to a full inbox (--inbox-cap): held while a fast
    // stream fills it, then resynced from the log
    {OP_PROMPT, 0, 0, 0, 100, "Stream fast."},
    {OP_HOLD, 0, 3000, 0, 3200, ""},
    {OP_WAIT, W_IDLE, 0, 0, 30000, ""},
    {OP_CHECK, CHK_OVERFLOWED, 0, 0, 0, ""},
    {OP_CHECK, CHK_RESYNC, 0, 0, 0, ""},
    // A settings edit: one line of the file changes
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_SETTINGSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 5000, ""},
    {OP_CHECK, CHK_DIAGS, 0, 0, 0, ""},
    {OP_MOVE, T_SETTING, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "77"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_SETTING, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "4-settings"},
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
    // Idle: no frames at all once the probe has run
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // Closing with a turn running asks first; Cancel goes back, and
    // stopping quits
    {OP_PROMPT, 0, 0, 0, 100, "Close me: run a very slow command."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_CLOSE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_CLOSEMODAL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "5-close"},
    {OP_MOVE, T_CLOSECANCEL, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_STILLBUSY, 0, 0, 0, ""},
    {OP_CLOSE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_CLOSEMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_MOVE, T_CLOSESTOP, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 0, ""},
    {OP_WAIT, W_FOREVER, 0, 0, 10000, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// End to end against a real model (--e2e, tests/gui.py --e2e): a coding
// task, a steer, a cancel, an ask allowed and one left to time out, a
// settings edit, and closing while a command runs. The model decides
// the details, so the checks look for what any reasonable agent does.
static DriveOp e2eops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_CHECK, CHK_SESSION, 0, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 1000,
     "In this directory, write a Python module calc.py with a function "
     "add(a, b) that returns their sum, and a test file test_calc.py that "
     "imports it, checks add with assert statements and prints ok. Then "
     "run python3 test_calc.py with the shell tool and tell me its output."},
    {OP_WAIT, W_IDLE, 0, 0, 900000, ""},
    {OP_CHECK, CHK_E2ECODE, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "e2e-1-code"},
    // Steering while a command runs: read at the next step
    {OP_PROMPT, 0, 0, 0, 100,
     "Run the shell command: sleep 15. Then tell me a short joke about computers."},
    {OP_WAIT, W_TOOL, 0, 0, 300000, ""},
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "Actually, skip the joke: reply with just the word DONE."},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 300000, ""},
    {OP_CHECK, CHK_E2ESTEER, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "e2e-2-steer"},
    // Cancelling mid-tool: the process tree goes (tests/gui.py checks)
    {OP_PROMPT, 0, 0, 0, 100,
     "Run exactly this shell command and wait for it: echo $$ > cancel.pid; sleep 120"},
    {OP_WAIT, W_TOOL, 0, 0, 300000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 30000, ""},
    {OP_CHECK, CHK_CANCELLED, 0, 0, 0, ""},
    // An ask, allowed
    {OP_PROMPT, 0, 0, 0, 100,
     "Use the write tool to create the file ../e2e-allowed.txt containing "
     "the word yes. Do not use the shell."},
    {OP_WAIT, W_ASK, 0, 0, 300000, ""},
    {OP_MARK, 0, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "e2e-3-ask"},
    {OP_MOVE, T_ALLOW, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 300000, ""},
    {OP_CHECK, CHK_E2EALLOWED, 0, 0, 0, ""},
    // An ask nobody answers
    {OP_PROMPT, 0, 0, 0, 100,
     "Use the write tool to create the file ../e2e-denied.txt containing "
     "the word no. Do not use the shell, and if the write is denied, do not "
     "try again: just say DENIED."},
    {OP_WAIT, W_ASK, 0, 0, 300000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_WAIT, W_IDLE, 0, 0, 300000, ""},
    {OP_CHECK, CHK_E2ETIMEOUT, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "e2e-4-timeout"},
    // A settings edit: one line of the file
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_SETTINGSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 5000, ""},
    {OP_MOVE, T_SETTING, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "77"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_SETTING, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // Closing while a command runs
    {OP_PROMPT, 0, 0, 0, 100,
     "Run exactly this shell command and wait for it: echo $$ > close.pid; sleep 120"},
    {OP_WAIT, W_TOOL, 0, 0, 300000, ""},
    {OP_CLOSE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_CLOSEMODAL, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_MOVE, T_CLOSESTOP, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 0, ""},
    {OP_WAIT, W_FOREVER, 0, 0, 10000, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Idle: nothing for a minute after startup settles.
static DriveOp idleops[] = {
    {OP_MARK, 0, 0, 0, 10000, ""},  // startup settles
    {OP_MARK, 0, 0, 0, 60000, "idle"},
    {OP_RATES, 0, 0, 0, 3000, "idle 60 s"},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// With an engine: a session with a finished turn, then the same.
static DriveOp m3idleops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Say hello."},
    {OP_WAIT, W_IDLE, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 60000, "idle"},
    {OP_RATES, 0, 0, 0, 3000, "idle 60 s with an idle session"},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Streaming at 100 tokens per second for 30 s.
static DriveOp streamops[] = {
    {OP_STREAM, 0, 100, 0, 3000, ""},
    {OP_MARK, 0, 0, 0, 30000, "streaming"},
    {OP_RATES, 0, 0, 0, 3000, "streaming 100 tok/s, 30 s"},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

static DriveOp m3streamops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 3000, "Stream at 100 tokens per second."},
    {OP_MARK, 0, 0, 0, 30000, "streaming"},
    {OP_RATES, 0, 0, 0, 3000, "streaming 100 tok/s from the engine, 30 s"},
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
    case T_PROMPTBTN:   return rectcenter(g->rects[RECT_PROMPTBTN]);
    case T_MENU:        return rectcenter(g->rects[RECT_MENU]);
    case T_HISTORY:     return rectcenter(g->rects[RECT_HISTORY]);
    case T_MODALITEM:   return rectcenter(g->rects[RECT_MODALITEM]);
    case T_MODALOK:     return rectcenter(g->rects[RECT_MODALOK]);
    case T_TRANSCRIPT:  return rectcenter(g->rects[RECT_TRANSCRIPT]);
    case T_PROMPT:      return rectcenter(g->rects[RECT_PROMPT]);
    case T_COPYITEM:    return rectcenter(g->rects[RECT_COPYITEM]);
    case T_NEWBTN:      return rectcenter(g->rects[RECT_NEWBTN]);
    case T_NEWCWD:      return rectcenter(g->rects[RECT_NEWCWD]);
    case T_NEWCREATE:   return rectcenter(g->rects[RECT_NEWCREATE]);
    case T_ALLOW:       return rectcenter(g->rects[RECT_ALLOW]);
    case T_DENY:        return rectcenter(g->rects[RECT_DENY]);
    case T_SETTING:     return rectcenter(g->rects[RECT_SETTING]);
    case T_CLOSESTOP:   return rectcenter(g->rects[RECT_CLOSESTOP]);
    case T_CLOSECANCEL: return rectcenter(g->rects[RECT_CLOSECANCEL]);
    case T_OTHERSESS:   return rectcenter(g->rects[RECT_OTHERSESS]);
    case T_SESSTITLE:
        for (GuiSession *s = g->sessions; s; s = s->next) {
            Str t = s->view && s->view->title.len ? s->view->title : s->title;
            if (startswith(t, g->ops[g->drivepc-1].text)) {
                return rectcenter(s->row);
            }
        }
        return v2(-1, -1);
    case T_TOPTEXT: {
        Rect r = g->rects[RECT_TRANSCRIPT];
        return v2((r.x0+r.x1)/2, r.y0 + 24*g->scale);
    }
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
    case T_SETTINGSTAB:   return tabcenter("Settings");
    case T_BELOW: {
        Rect r = g->rects[RECT_TRANSCRIPT];
        return v2((r.x0+r.x1)/2, r.y1);
    }
    }
    return v2(0, 0);
}

// Window sizes in the checklist are in ImGui's units, which are pixels
// on Win32, at the display's scale, and points elsewhere.
static f32 winscale(Gui *g)
{
    #ifdef _WIN32
    return g->scale;
    #else
    return 1;
    #endif
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

static Str promptstr(Gui *g)
{
    Str p = {(u8 *)g->prompt, 0};
    while (p.data[p.len]) p.len++;
    return p;
}

// The latest message of a role in the session shown, or null.
static ViewMsg *lastmsg(Gui *g, i32 role)
{
    View *v = g->view;
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        if (v->msgs[i].role==role && !v->msgs[i].prov) {
            return &v->msgs[i];
        }
    }
    return 0;
}

// The latest tool row of a tool, or null.
static ViewTool *lasttool(Gui *g, Str name)
{
    View *v = g->view;
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_TOOL && !m->prov && m->tool->name==name) {
            return m->tool;
        }
    }
    return 0;
}

static ViewMsg *lasttoolmsg(Gui *g, Str name)
{
    View *v = g->view;
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_TOOL && !m->prov && m->tool->name==name) {
            return m;
        }
    }
    return 0;
}

static b32 drivewaitdone(Gui *g)
{
    View *v = g->view;
    switch (g->drivewait) {
    case W_ENGINE:
        return g->engineok;
    case W_SESSION:
        return g->sess && g->sess->view && g->sess->view->cwd.len && !g->newid;
    case W_IDLE:
        return g->sess && !v->busy;
    case W_ASK: {
        ViewMsg *m = lastmsg(g, VIEW_ASK);
        return m && m->tool->answer==VASK_PENDING;
    }
    case W_TOOL:
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            if (m->role==VIEW_TOOL && m->tool->status==VTOOL_RUNNING) {
                return 1;
            }
        }
        return 0;
    case W_CONFIG:
        return g->cfg && !g->cfgget;
    case W_CONFIGSET:
        return g->cfgsets>0 && !g->cfgset && !g->cfgget;
    case W_ALLIDLE:
        return !running(g);
    case W_OPENED:
        return g->sess && g->sess->view && !g->sess->openid && v->lastseq;
    case W_AMBER:
        return g->ambershown;
    case W_BACKOFF:
        return g->sess && v->state==LIVE_BACKOFF;
    }
    return 0;
}

// The current session's view against a view built from its log alone.
static b32 checkresync(Gui *g)
{
    GuiSession *s = g->sess;
    if (!s || !s->view) {
        return 0;
    }
    Arena   a   = g->frame;
    iz      mb  = (iz)1 << 20;
    Region *r[] = {os_reserve(g->os, 512*mb), os_reserve(g->os, 64*mb),
                   os_reserve(g->os, 64*mb)};
    View   *f   = newview(&a, g->vcache, r[0], r[1], r[2]);
    f->dir   = s->view->dir;
    f->loose = 1;
    Arena  lines = newarena(g->replayr);
    Str    path  = join(&a, sessdir(g, &a, s->sid), "events.jsonl");
    Replay rp    = logreplay(g->os, &lines, a, path, s->sid, 0, LOG_MAX);
    for (iz i = 0; i < rp.lines.len; i++) {
        viewapply(f, a, rp.lines[i]);
    }
    Buf x(&a, 1<<16);
    Buf y(&a, 1<<16);
    viewdump(&x, s->view);
    viewdump(&y, f);
    Str got  = finish(&x);
    Str want = finish(&y);
    b32 same = got == want;
    if (!same) {
        iz i = 0;
        for (; i<got.len && i<want.len && got[i]==want[i]; i++) {}
        Buf m(&a, 512);
        print(&m, "drive: the view differs from the log's at byte ");
        print(&m, (i64)i);
        print(&m, ": view ");
        print(&m, takehead(cuthead(got, i), got.len-i<120 ? got.len-i : 120));
        print(&m, " | log ");
        print(&m, takehead(cuthead(want, i), want.len-i<120 ? want.len-i : 120));
        guilog(g, finish(&m));
    } else {
        Buf m(&a, 128);
        print(&m, "drive: view and log agree on ");
        print(&m, (i64)rp.lines.len);
        print(&m, " records after ");
        print(&m, g->overflows);
        print(&m, " overflows and ");
        print(&m, g->resyncs);
        print(&m, " resyncs");
        guilog(g, finish(&m));
    }
    for (iz i = 0; i < countof(r); i++) {
        if (r[i]) os_release(g->os, r[i]);
    }
    return same;
}

static Str cfgfile(Gui *g, Arena *a)
{
    return g->cfgpath.len ? g->cfgpath : cfgdefault(g->os, a);
}

static Slice<Str> splitlines(Arena *a, Str s)
{
    Slice<Str> r = {};
    for (Str rest = s; rest.len;) {
        Cut c = cut(rest, '\n');
        r    = push(a, r, c.head);
        rest = c.tail;
    }
    return r;
}

// The configuration file differs from the snapshot by one line, the
// edited one, changed in place or added.
static b32 checksetting(Gui *g)
{
    Arena a  = g->frame;
    Read  rd = os_read(g->os, &a, cfgfile(g, &a), 1<<20);
    if (rd.err!=READ_OK || !g->cfgsnap.data) {
        return 0;
    }
    Slice<Str> x = splitlines(&a, g->cfgsnap);
    Slice<Str> y = splitlines(&a, rd.data);
    iz i = 0;
    for (; i<x.len && i<y.len && x[i]==y[i]; i++) {}
    b32 one = 0;
    if (i<y.len && x.len==y.len) {
        one = 1;  // changed in place: the rest the same
        for (iz k = i+1; k < x.len; k++) one &= x[k] == y[k];
    } else if (i<y.len && x.len+1==y.len) {
        one = 1;  // added: the rest moved down by one
        for (iz k = i; k < x.len; k++) one &= x[k] == y[k+1];
    }
    b32 edit = i<y.len && find(y[i], "idle_timeout")>=0 && find(y[i], "77")>=0;
    Buf m(&a, 256);
    print(&m, "drive: the settings edit ");
    print(&m, one ? Str("changed one line: ") : Str("changed more than one line, first: "));
    print(&m, i<y.len ? y[i] : Str("(none)"));
    guilog(g, finish(&m));
    return one && edit;
}

static b32 drivecheck(Gui *g, i32 chk)
{
    ImGuiContext &c = *GImGui;
    View         *v = g->view;
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
    case CHK_RESIZED:       return (i32)c.IO.DisplaySize.x == (i32)(1100*winscale(g));
    case CHK_TYPED:         return promptstr(g) == "Hello, harness";
    case CHK_BLINKOFF:      return !c.IO.ConfigInputTextCursorBlink;
    case CHK_HISTORY:       return v->msgs.len >= 10000;
    case CHK_SCROLLED:      return g->sess ? !g->sess->stick : !g->nosession.stick;
    case CHK_SELECTED: {
        Range r = selrange(&v->sel);
        return v->sel.active && r.end.msg > r.beg.msg;
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
        return g->modalframes>2 && (i32)c.IO.DisplaySize.x!=(i32)(1280*winscale(g));
        #else
        print(&b, "drive: simulated live-resize exposes: ");
        print(&b, g->modalframes);
        print(&b, " frames drawn from the event watch");
        guilog(g, finish(&b));
        return g->modalframes > 2;
        #endif
    }
    case CHK_ENGINE:   return g->engineok && !g->engineerr.len;
    case CHK_SESSION:
        g->drivemain = g->sess;
        return g->sess && g->sess->view && g->sess->view->cwd.len;
    case CHK_NEWLINE: {
        Str p = promptstr(g);
        return endswith(p, "command.\n") && !v->busy;
    }
    case CHK_SENT:
        return !promptstr(g).len && v->busy;
    case CHK_TOOLROWS: {
        ViewTool *r = lasttool(g, "read");
        ViewTool *e = lasttool(g, "edit");
        ViewTool *s = lasttool(g, "shell");
        return r && e && s && r->status==VTOOL_OK && e->status==VTOOL_OK &&
               s->status==VTOOL_OK && s->hasexit && s->exit==0 &&
               s->spill.len && s->ms>0;
    }
    case CHK_DIFF: {
        ViewMsg *m = lasttoolmsg(g, "edit");
        return m && m->md && startswith(m->text, "```diff\n") &&
               find(m->text, "\n-")>=0 && find(m->text, "\n+")>=0 && !m->collapsed;
    }
    case CHK_LIVETAIL:
        return g->tailseen > 0;
    case CHK_ASKCARD: {
        ViewMsg *m = lastmsg(g, VIEW_ASK);
        Rect     r = g->rects[RECT_ALLOW];
        return m && m->tool->answer==VASK_PENDING && m->tool->deadline &&
               r.x1>r.x0 && m->text.len;
    }
    case CHK_ASKALLOWED: {
        ViewMsg *k = lastmsg(g, VIEW_ASK);
        ViewMsg *a = lastmsg(g, VIEW_ASSISTANT);
        return k && k->tool->answer==VASK_ALLOWED && k->tool->by=="user" &&
               a && find(a->text, "allowed")>=0;
    }
    case CHK_ASKTIMEOUT: {
        ViewMsg *k = lastmsg(g, VIEW_ASK);
        ViewMsg *a = lastmsg(g, VIEW_ASSISTANT);
        return k && k->tool->answer==VASK_DENIED && k->tool->by=="timeout" &&
               a && find(a->text, "inside")>=0;
    }
    case CHK_STEERED: {
        ViewMsg *u = lastmsg(g, VIEW_USER);
        ViewMsg *a = lastmsg(g, VIEW_ASSISTANT);
        return u && u->label.len && u->text=="Use the left door." &&
               a && find(a->text, "Steered")>=0 && find(a->text, "left door")>=0;
    }
    case CHK_CANCELLED: {
        ViewMsg  *n = lastmsg(g, VIEW_NOTE);
        ViewTool *s = lasttool(g, "shell");
        return n && startswith(n->text, "Cancelled") && s &&
               s->status==VTOOL_CANCELLED && !v->busy;
    }
    case CHK_OVERFLOWED:
        return g->overflows > 0;
    case CHK_RESYNC:
        return checkresync(g);
    case CHK_SETTING:
        return checksetting(g);
    case CHK_CLOSEMODAL: {
        ImGuiWindow *w = ImGui::GetTopMostPopupModal();
        return w && find({(u8 *)w->Name, (iz)__builtin_strlen(w->Name)}, "Quit")>=0;
    }
    case CHK_STILLBUSY:
        return v->busy && !g->quitting;
    case CHK_REPLAYED: {
        b32 user = 0, said = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            user |= v->msgs[i].role==VIEW_USER && v->msgs[i].text=="Say hello.";
            said |= v->msgs[i].role==VIEW_ASSISTANT && v->msgs[i].text=="Hello!";
        }
        return user && said && g->sess && !g->sess->readonly && !v->busy;
    }
    case CHK_READONLY: {
        b32 user = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            user |= v->msgs[i].role==VIEW_USER && startswith(v->msgs[i].text, "Hold me");
        }
        return user && g->sess && g->sess->readonly && v->readonly && g->bannershown;
    }
    case CHK_AMBER:
        return g->ambershown;
    case CHK_RETRYSHOWN:
        return g->retryshown;
    case CHK_DIAGS: {
        Json *d = jsonget(g->cfg, "diags")->child;
        return d && jsonstr(jsonget(d, "file")).len && jsonint(jsonget(d, "line"), 0)>0 &&
               find(jsonstr(jsonget(d, "message")), "colour")>=0;
    }
    case CHK_E2ECODE: {
        Arena a  = g->frame;
        b32   ok = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewTool *t = v->msgs[i].tool;
            ok |= v->msgs[i].role==VIEW_TOOL && t->name=="shell" &&
                  t->status==VTOOL_OK && t->hasexit && !t->exit;
        }
        Str calc = join(&a, g->drivecwd, "calc.py");
        Str test = join(&a, g->drivecwd, "test_calc.py");
        return ok && !v->busy && os_stat(g->os, a, calc).type==STAT_FILE &&
               os_stat(g->os, a, test).type==STAT_FILE;
    }
    case CHK_E2ESTEER: {
        b32 steered = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            steered |= v->msgs[i].role==VIEW_USER && v->msgs[i].label.len &&
                       startswith(v->msgs[i].text, "Actually");
        }
        ViewMsg *n = lastmsg(g, VIEW_NOTE);
        return steered && !v->busy && (!n || !startswith(n->text, "The turn ended"));
    }
    case CHK_E2EALLOWED: {
        // The agent may ask more (to read the file back), so any ask
        Arena a       = g->frame;
        b32   allowed = 0, wrote = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewTool *t = v->msgs[i].tool;
            allowed |= v->msgs[i].role==VIEW_ASK && t->answer==VASK_ALLOWED && t->by=="user";
            wrote   |= v->msgs[i].role==VIEW_TOOL && t->name=="write" && t->status==VTOOL_OK;
        }
        Str f = join(&a, g->drivecwd, "../e2e-allowed.txt");
        return allowed && wrote && !v->busy && os_stat(g->os, a, f).type==STAT_FILE;
    }
    case CHK_E2ETIMEOUT: {
        ViewMsg *k = lastmsg(g, VIEW_ASK);
        ViewMsg *a = lastmsg(g, VIEW_ASSISTANT);
        return k && k->tool->answer==VASK_DENIED && k->tool->by=="timeout" &&
               a && a>k && !v->busy;
    }
    case CHK_SWITCHED: {
        // The first session shown again while the second streams
        b32 other = 0, first = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            other |= s!=g->sess && s->view && s->view->busy;
        }
        for (iz i = 0; i < v->msgs.len; i++) {
            first |= v->msgs[i].role==VIEW_USER && startswith(v->msgs[i].text, "Run the tools");
        }
        return other && first && g->sess && !g->sess->view->busy;
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
        if (v->msgs[i].collapsed) continue;
        Doc *d = msgdoc(v, i);
        for (i32 k = 0; k < d->blocks.len; k++) {
            DocBlock *b = &d->blocks[k];
            if (b->type!=DOC_CODE || !b->npiece) continue;
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

static void drivesummary(Gui *g)
{
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
}

static void drivestep(Gui *g)
{
    if (g->drivewait) {
        // The condition holds, or the time ran out
        if (!drivewaitdone(g)) {
            Arena a = g->frame;
            Buf   b(&a, 128);
            print(&b, "drive: FAIL gave up waiting for ");
            print(&b, waitnames[g->drivewait]);
            guilog(g, finish(&b));
            g->drivechecks++;
            g->drivefails++;
        }
        g->drivewait = W_NONE;
        g->driveat   = g->sched.now;
        return;
    }
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
        g->drivemoved = 1;
        g->drivex     = e.x;
        g->drivey     = e.y;
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
        if (e.down && g->tracedrive) {
            // What the press lands on, as of the last frame
            ImGuiContext &c = *GImGui;
            Arena a = g->frame;
            Buf   b(&a, 256);
            print(&b, "drive: press at ");
            print(&b, (i64)e.x);
            putbyte(&b, ',');
            print(&b, (i64)e.y);
            print(&b, ", ImGui's mouse at ");
            print(&b, (i64)c.IO.MousePos.x);
            putbyte(&b, ',');
            print(&b, (i64)c.IO.MousePos.y);
            print(&b, " over ");
            char const *w = c.HoveredWindow ? c.HoveredWindow->Name : "nothing";
            print(&b, Str{(u8 *)w, (iz)__builtin_strlen(w)});
            print(&b, ", item ");
            printhex(&b, c.HoveredId, 8);
            guilog(g, finish(&b));
        }
        break;
    case OP_WHEEL:
        e.kind = GEV_WHEEL;
        e.x    = g->drivex;
        e.y    = op->dy;
        os_guiinject(g->os, &e);
        break;
    case OP_KEYDOWN:
    case OP_KEYUP:
        e.kind  = GEV_KEY;
        e.key   = (i32)op->dx;
        e.ctrl  = ((i32)op->dy & KEY_CTRL) != 0;
        e.shift = ((i32)op->dy & KEY_SHIFT) != 0;
        e.down  = op->op == OP_KEYDOWN;
        os_guiinject(g->os, &e);
        break;
    case OP_TEXT:
        e.kind = GEV_TEXT;
        e.text = op->text;
        os_guiinject(g->os, &e);
        break;
    case OP_TEXTCWD:
        e.kind = GEV_TEXT;
        e.text = g->drivecwd;  // null-terminated, from the command line
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
        e.x    = op->dx * winscale(g);
        e.y    = op->dy * winscale(g);
        os_guiinject(g->os, &e);
        break;
    case OP_CLOSE:
        e.kind = GEV_CLOSE;
        os_guiinject(g->os, &e);
        break;
    case OP_CHECK: {
        if ((op->target==CHK_DPI || op->target==CHK_DPIBACK) && g->dpibase==0) {
            guilog(g, "drive: skip DPI change (Win32 only)");
            break;
        }
        if (op->target==CHK_OVERFLOWED && g->inboxcap>=INBOX_CAP) {
            guilog(g, "drive: skip the overflow check (no --inbox-cap)");
            break;
        } else if (op->target==CHK_OVERFLOWED && g->enginekind==ENGINE_SUBPROCESS) {
            // Its output waits in the pipe, never dropped on this side
            guilog(g, "drive: skip the overflow check (the engine is a process)");
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
    case OP_WAIT:
        g->drivewait = op->target;
        if (drivewaitdone(g)) {
            g->drivewait = W_NONE;
            g->driveat   = g->sched.now;
        }
        break;
    case OP_MARK:
        g->driveframes = g->total.frames;
        g->markcounts  = g->total;
        g->markat      = g->sched.now;
        if (op->text.len) {
            // For a measurement outside, by the operating system
            Arena a = g->frame;
            // Not in the Log window, which would make frames to show it
            os_print(g->os, 2, concat(&a, concat(&a, "harness: drive: mark: ", op->text), "\n"));
        }
        break;
    case OP_STREAM:
        fakeint(g, FAKE_TPS, (i64)op->dx);
        fakeint(g, FAKE_STREAM, 1);
        break;
    case OP_STOP:
        fakecmd(g, FAKE_STOP, "");
        break;
    case OP_NEWSESSION:
        guinew(g, g->drivecwd, {});
        break;
    case OP_PROMPT:
        guiprompt(g, op->text);
        break;
    case OP_HOLD:
        g->holduntil = g->sched.now + (i64)op->dx*1000;
        break;
    case OP_SNAPCFG: {
        Arena a = g->perm;
        Read  rd = os_read(g->os, &a, cfgfile(g, &a), 1<<20);
        g->cfgsnap = rd.err==READ_OK ? clone(&g->perm, rd.data) : Str{};
        g->cfgsets = 0;
    } break;
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
        print(&b, " in ");
        printfloat(&b, (f64)(g->sched.now - g->markat)/1e6, 1);
        print(&b, " s");
        guilog(g, finish(&b));
    } break;
    case OP_SELCODE:
        selectcode(g);
        scheddirty(&g->sched);
        break;
    case OP_SUMMARY:
        drivesummary(g);
        break;
    case OP_MAIN:
        if (g->drivemain) {
            guiselect(g, g->drivemain);
        }
        break;
    case OP_SHOWUSER: {
        View *v = g->view;
        for (iz i = v->msgs.len-1; i >= 0; i--) {
            if (v->msgs[i].role==VIEW_USER && startswith(v->msgs[i].text, op->text)) {
                g->scrollto = v->msgs[i].top;
                if (g->sess) g->sess->stick = 0;
                scheddirty(&g->sched);
                break;
            }
        }
    } break;
    case OP_SHOT:
        if (g->shotdir.len) {
            Str name = concat(&g->perm, op->text, ".png");
            g->shotpath = join(&g->perm, g->shotdir, name);
            schedforce(&g->sched);  // presented, so captured
        }
        break;
    case OP_QUIT:
        if (!g->quitting) {
            drivesummary(g);
        }
        startquit(g);
        break;
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
"  -c FILE        configuration file (default: harness.ini, as for run)\n"
"  --statedir DIR sessions, layout file and log (default: the state dir)\n"
"  --engine=MODE  inproc (default) or subprocess: \"harness --headless\"\n"
"  --fake         a fake agent instead of an engine, for measurements:\n"
"    --tps N        its tokens per second (default 100)\n"
"    --stream       stream continuously\n"
"    --history N    load N messages of history at startup\n"
"    --bench        load 10k messages, scroll through them, relayout, quit\n"
"  --drive        run the checklist with synthetic input, quit: M1's with\n"
"                 --fake, else M3's against tests/fake_provider.py\n"
"  --cwd DIR      the checklist's working directory\n"
"  --inbox-cap N  bytes per inbox buffer, small to force lost records\n"
"  --shots DIR    the checklist saves screenshots there (SDL only)\n"
"  --idle         measure a minute of idling, quit\n"
"  --streamtest   measure 30 s of streaming at 100 tok/s, quit\n"
"  --e2e          the checklist's end-to-end part against a real model\n"
"  --probe        staleness probe on (default in debug builds)\n"
"  --no-probe     staleness probe off\n"
"  --aggressive   probe every second while idle\n"
"  --trace        log every frame and why it ran\n"
"  --trace-drive  log where the checklist's presses land\n"
"  --quit-after S quit after S seconds\n";

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

// A null-terminated copy, for synthetic text input.
static Str cstrclone(Arena *a, Str s)
{
    Str r = clone(a, s);
    *alloc<u8>(a) = 0;
    if (!r.data) r.data = (u8 *)"";
    return r;
}

// The engine on its own thread, with the configuration it owns.
static b32 startinproc(Gui *g, Arena scratch)
{
    Os     *os   = g->os;
    Str     path = g->cfgpath.len ? g->cfgpath : cfgdefault(os, &g->perm);
    Config *cfg  = newconfig(os, &g->perm, scratch, path, !g->cfgpath.len);
    if (!cfg) {
        g->engineerr = "out of memory";
        return 0;
    } else if (g->cfgpath.len && !cfg->user) {
        g->engineerr = "cannot read the configuration file";
        return 0;
    }
    for (iz i = 0; i < cfg->ini->diags.len; i++) {
        Buf b(&scratch, 256);
        print(&b, cfg->ini->diags[i]);
        guilog(g, finish(&b));
    }
    g->engine = newengine(os, &g->perm, cfg, g->statedir, g->inbox);
    if (!g->engine || !enginestart(g->engine)) {
        g->engineerr = "cannot start the engine";
        return 0;
    }
    return 1;
}

// The engine as "harness --headless" in a child process: its output
// arrives in the inbox, its errors go to engine.log.
static b32 startsubprocess(Gui *g, Arena scratch)
{
    Os        *os   = g->os;
    Str        exe  = os_exepath(os, &scratch);
    Slice<Str> argv = {};
    argv = push(&scratch, argv, exe);
    argv = push(&scratch, argv, Str("--headless"));
    argv = push(&scratch, argv, Str("--state"));
    argv = push(&scratch, argv, g->statedir);
    if (g->cfgpath.len) {
        argv = push(&scratch, argv, Str("-c"));
        argv = push(&scratch, argv, g->cfgpath);
    }
    Spawn sp = {};
    sp.argv    = argv;
    sp.cwd     = g->statedir;
    sp.env     = os_environ(os, &scratch);
    sp.outpath = join(&scratch, g->statedir, "engine.log");
    sp.mbox    = g->inbox;
    sp.id      = 1;
    g->proc = os_spawnio(os, &sp, scratch);
    if (!g->proc) {
        g->engineerr = concat(&g->perm, "cannot start the engine process: ",
                              os_lasterr(os, &scratch));
        return 0;
    }
    g->linemem  = newarena(os_reserve(os, (iz)1<<30));
    g->linebase = g->linemem.beg;
    g->line     = Buf(&g->linemem, 1<<16);
    return 1;
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
    g->inboxcap = INBOX_CAP;
    g->firstlayout = 1;
    #ifdef __APPLE__
    g->mac = 1;
    #endif

    b32 probe = 1;
    #ifdef NDEBUG
    probe = 0;
    #endif
    b32 aggressive = 0, stream = 0, idle = 0, streamtest = 0, drive = 0;
    b32 subprocess = 0, fake = 0, e2e = 0;
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
            fake     = 1;
            history  = 10000;
            g->drive = 1;  // scripted: no focus, draws when occluded
        } else if (a == "--drive") {
            drive = probe = 1;
        } else if (a == "--idle") {
            idle = 1;
        } else if (a == "--streamtest") {
            streamtest = 1;
        } else if (a == "--e2e") {
            e2e = probe = 1;
        } else if (a == "--trace") {
            g->trace = 1;
        } else if (a == "--trace-drive") {
            g->tracedrive = 1;
        } else if (a == "--probe") {
            probe = 1;
        } else if (a == "--no-probe") {
            probe = 0;
        } else if (a == "--aggressive") {
            probe = aggressive = 1;
        } else if (a == "--quit-after") {
            i64 s = optint(args, nargs, &i);
            g->quitat = s<0 ? 0 : g->started + s*1000000;
        } else if (a=="--statedir" && i+1<nargs) {
            g->statedir = args[++i];
        } else if (a=="-c" && i+1<nargs) {
            g->cfgpath = args[++i];
        } else if (a=="--cwd" && i+1<nargs) {
            g->drivecwd = cstrclone(&mem, args[++i]);
        } else if (a == "--inbox-cap") {
            g->inboxcap = optint(args, nargs, &i);
        } else if (a=="--shots" && i+1<nargs) {
            g->shotdir = args[++i];
        } else if (a == "--fake") {
            fake = 1;
        } else if (a == "--engine=subprocess") {
            subprocess = 1;
        } else if (a == "--engine=inproc") {
            subprocess = 0;
        } else {
            os_print(os, 2, guiusage);
            return 1;
        }
    }
    if (g->tps<=0 || history<0 || g->inboxcap<(1<<12)) {
        os_print(os, 2, guiusage);
        return 1;
    }
    g->enginekind = fake ? ENGINE_FAKE : subprocess ? ENGINE_SUBPROCESS : ENGINE_INPROC;
    if (drive || idle || streamtest || e2e) {
        g->drive = 1;
        if (e2e) {
            g->ops  = e2eops;
            g->nops = countof(e2eops);
        } else if (drive) {
            g->ops  = fake ? driveops : m3ops;
            g->nops = fake ? countof(driveops) : countof(m3ops);
        } else if (idle) {
            g->ops  = fake ? idleops : m3idleops;
            g->nops = fake ? countof(idleops) : countof(m3idleops);
        } else {
            g->ops  = fake ? streamops : m3streamops;
            g->nops = fake ? countof(streamops) : countof(m3streamops);
        }
        if (!fake && !g->drivecwd.len) {
            os_print(os, 2, "harness: the checklist needs --cwd DIR\n");
            return 1;
        }
    }

    // Memory: the frame scratch, the views' arenas, the fake agent's.
    iz mb = (iz)1 << 20;
    g->perm      = newarena(os_reserve(os, 256*mb));
    g->frame     = newarena(os_reserve(os, 1024*mb));
    g->framebase = g->frame.beg;
    g->vcache    = newviewcache(&g->perm, os_reserve(os, 2048*mb), 512*mb);
    g->replayr   = os_reserve(os, (iz)1<<32);
    g->cmdr      = os_reserve(os, 256*mb);
    g->cfgmem    = newarena(os_reserve(os, 256*mb));
    g->cfgbase   = g->cfgmem.beg;
    g->pendmem   = newarena(os_reserve(os, 256*mb));
    g->pendbase  = g->pendmem.beg;
    if (!g->perm.beg || !g->frame.beg || !g->vcache->arena.beg || !g->replayr ||
            !g->cmdr || !g->cfgmem.beg || !g->pendmem.beg) {
        os_print(os, 2, "harness: out of memory\n");
        return 2;
    }
    g->nosession.stick = 1;
    g->nosession.view  = newview(&g->perm, g->vcache, os_reserve(os, 16*mb),
                                 os_reserve(os, 16*mb), os_reserve(os, 16*mb));
    g->view       = g->nosession.view;
    g->prompt     = alloc<char>(&g->perm, PROMPT_MAX);
    g->newcwd     = alloc<char>(&g->perm, FIELD_MAX);
    g->newmodel   = alloc<char>(&g->perm, FIELD_MAX);
    g->denynote   = alloc<char>(&g->perm, FIELD_MAX);
    g->addname    = alloc<char>(&g->perm, FIELD_MAX);
    g->benchtimes = alloc<i64>(&g->perm, BENCH_MAX);
    g->pending    = Buf(&g->pendmem, 1<<12);

    if (!g->statedir.len) {
        g->statedir = defaultstate(os, &g->perm);
    }
    Str sessions = join(&g->perm, g->statedir, "sessions");
    if (g->statedir.len && os_mkdirs(os, g->frame, sessions)) {
        g->inipath = statepath(g, "imgui.ini");
        g->logfd   = os_logopen(os, g->frame, statepath(g, "gui.log"));
    }
    if (g->drivecwd.len) {
        Str cwd = g->drivecwd;
        copybytes(g->newcwd, cwd.data, cwd.len<FIELD_MAX ? cwd.len : FIELD_MAX-1);
    } else {
        Str cwd = os_realpath(os, &g->frame, ".");
        copybytes(g->newcwd, cwd.data, cwd.len<FIELD_MAX ? cwd.len : FIELD_MAX-1);
    }

    g->inbox  = newmailbox(&g->perm, g->inboxcap, wakegui, os);
    g->outbox = newmailbox(&g->perm, 1<<16, 0, 0);
    if (!g->inbox || !g->outbox) {
        os_print(os, 2, "harness: could not create mailboxes\n");
        return 2;
    }
    Fake *f = 0;
    if (fake) {
        f = alloc<Fake>(&g->perm);
        f->os       = os;
        f->out      = g->inbox;
        f->in       = g->outbox;
        f->perm     = newarena(os_reserve(os, 256*mb));
        f->turnmem  = newarena(os_reserve(os, 1024*mb));
        f->turnbase = f->turnmem.beg;
        f->sid      = "20261020-000000-fake";
        f->tps      = g->tps;
        f->rng      = 1;
        if (!f->perm.beg || !f->turnmem.beg) {
            os_print(os, 2, "harness: out of memory\n");
            return 2;
        }
        GuiSession *s = addsess(g, f->sid);
        View       *v = sessview(g, s);
        v->title = "Fake agent";
        v->cwd   = "(none)";
        v->state = LIVE_IDLE;
        setcur(g, s);
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

    switch (g->enginekind) {
    case ENGINE_FAKE:
        if (!os_thread(os, fakemain, f)) {
            os_print(os, 2, "harness: could not start the fake agent\n");
            return 2;
        }
        g->engineok = 1;
        break;
    case ENGINE_INPROC:
        startinproc(g, g->frame);
        break;
    case ENGINE_SUBPROCESS:
        startsubprocess(g, g->frame);
        break;
    }
    if (g->engineerr.len) {
        guilog(g, g->engineerr);
        g->enginegone = 1;
    } else if (!fake) {
        guihello(g);
        guilist(g);
        guiconfigget(g);
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
    if (!g->quitting && !g->enginegone) {
        startquit(g);  // the window went without asking: stop anyway
    }
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
        print(&b, ", overflows ");
        print(&b, g->overflows);
        print(&b, ", resyncs ");
        print(&b, g->resyncs);
        print(&b, ", max frame ");
        printfloat(&b, (f64)g->maxframeus/1000, 2);
        print(&b, " ms");
        guilog(g, finish(&b));
    }
    os_guiclose(os);
    if (g->proc) {
        // It stopped, or had its chance: the rest of its tree goes now
        // (POSIX: its reaper), as a tool's would
        os_kill(os, g->proc, 1);
        os_procfree(os, g->proc);
    }
    g->splitter.ClearFreeMemory();
    ImGui::DestroyContext();
    return 0;
}

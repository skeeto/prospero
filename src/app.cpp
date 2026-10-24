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
// "prospero --headless" in a child process (--engine=subprocess), whose
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
// Besides its engine, the GUI reaches hosts (host.cpp) through the
// client library (remote.cpp), each with an inbox of its own: the
// background hosts its state directory registers, found at startup,
// and remote ones over ssh, as [remote NAME] sets them up, connected
// from the Hosts panel. Their sessions are listed under them, and work
// as the engine's do, but their files are never read here: a link that
// breaks catches up through the library, which opens them again from
// where their views are, and a spill file comes through the file
// command. A background host shares the state directory, so the engine
// lists its sessions too, as held elsewhere: they are the host's while
// it holds them, and the engine's again once it lets them go.
//
// Each session has its own draft in the compose box, which its engine
// keeps (draft) once typing pauses, as the session is left, and as the
// GUI quits, and which open's reply gives back (Drafts, below); the box
// opens a draft in an external editor, run as a tool is (The editor).
//
// Closing the window with turns running may keep them running instead
// of stopping them: this process becomes a host of its engine, in
// place, with no window and a tray icon (Background hosting, below).
//
// The theme is [ui] theme: read from the configuration before the
// window opens, so that it opens in it, then from each config.get
// reply, and picked in the settings window, where it applies at once.
// "system" follows what the platform reports (gui_systheme). [ui]
// thinking, how reasoning shows, and [ui] tools, how tool rows fold,
// come the same ways and apply to every session's view at once
// (setthinking, settools). [ui] font_size sets the
// text's size, which the View menu's zoom scales for this run only
// (setzoom), both applied between frames (fontsize). The layout
// file is written by a thread of its own (Saver), since a write that is
// to survive a crash waits on the disk for longer than a frame. It also
// keeps which panes show and the window's geometry (iniwrite), read
// before the window opens.
//
// This is free and unencumbered software released into the public domain.

#include "sched.cpp"
#include "highlight.cpp"
#include "doc.cpp"
#include "layout.cpp"
#include "view.cpp"
#include "theme.cpp"
#include "png.cpp"
#include "fonts.cpp"
#include "place.cpp"
#include "compose.cpp"
#include "sidebar.cpp"

enum : i64 {
    STREAM_PERIOD = 1000000/30,  // µs between frames showing new deltas
    JOBS_PERIOD   = 250000,      // µs between frames showing job output
    CARET_STOP    = 10000000,    // µs after the last key: stop blinking
    COPIED_US     = 1500000,     // µs a code block's button says Copied
};

enum : iz {
    BENCH_MAX  = 4096,
    GUILOG_MAX = 200,     // lines kept in the Log window
    INBOX_CAP  = 1<<21,   // bytes per inbox buffer
    FIELD_MAX  = 1<<10,   // bytes of a settings or dialog field
    REQ_MAX    = 64,      // commands awaiting replies
    FIND_BUDGET = 4<<20,  // bytes of text a frame searches at most
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

// And the tasks mailbox takes these, besides a deployment's MAIL_OUT
// progress records: outcomes from other threads, which wake the GUI.
enum : u32 {
    TASK_SAVED = 32,    // a save dialog's path (id its ask), empty if cancelled
    TASK_DEPLOYED,      // Update engine: 1 or 0, then the path or why (id the host's uid)
    TASK_RAISE,         // a launch handed off to this window: its token (door.cpp)
    TASK_OPENED,        // an open dialog's path (id its ask), empty if cancelled
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
    RECT_ALWAYS,
    RECT_SETTING,
    RECT_CLOSESTOP,
    RECT_CLOSECANCEL,
    RECT_OTHERSESS,
    RECT_ADDNAME,
    RECT_ADDBTN,
    RECT_THEMECOMBO,
    RECT_LONGFIELD,  // a setting's field holding more than its slack
    RECT_TODO,       // the todo list's items
    RECT_JOBKILL,    // the newest running job's Kill
    RECT_TASKLINK,   // the latest task row's subagent link
    RECT_PARENTLINK, // a subagent's link to its parent
    RECT_MSGLINK,    // the latest message's link to its other session
    RECT_UNDO,       // the latest Undo on a tool row
    RECT_REWINDTURN, // the latest Rewind to here
    RECT_REWINDOK,   // the rewind confirmation's buttons
    RECT_REWINDCANCEL,
    RECT_FORCE,      // the latest rewind card's Rewind anyway
    RECT_INFOBTN,    // the session's Info
    RECT_COMPACTHDR, // the latest compaction card's header
    RECT_THINKHDR,   // the latest reasoning's header drawn
    RECT_USERMSG,    // the latest user message drawn
    RECT_CLOSEBG,    // the close dialog's Keep running in background
    RECT_HOSTCOMBO,  // the new session's host, and its first choices
    RECT_HOST0,
    RECT_HOST1,
    RECT_HOST2,
    RECT_HOST3,
    RECT_CONNECT,    // the Hosts panel's first remote: Connect or Disconnect
    RECT_CONNECT2,   // and its second's
    RECT_REMNAME,    // its new remote's fields and Add
    RECT_REMSSH,
    RECT_REMADD,
    RECT_OUTPUT,     // the latest tool row's Output
    RECT_THUMB,      // the latest screenshot's thumbnail drawn
    RECT_SHOTCLOSE,  // the Screenshot window's Close
    RECT_REFRESHINC, // the settings window's Refresh includes
    RECT_CODEBLOCK,  // the code block aimed at (selectcode), else the latest
    RECT_CODECOPY,   // and its Copy button, while shown
    RECT_TOOLSBTN,   // the session bar's toggles
    RECT_THINKBTN,
    RECT_FINDFIELD,  // the find bar's field
    RECT_GAUGE,      // the context gauge
    RECT_PAGECOMBO,  // its pages as a combo, when narrow
    RECT_DIAGPAGE,   // its Diagnostics page in the list
    RECT_ADDOPEN,    // its Add for each named kind, in order: endpoint,
    RECT_ADDOPEN1,   // model, role, remote
    RECT_ADDOPEN2,
    RECT_ADDOPEN3,
    RECT_DELETE,     // the page's Delete, and its confirmation's buttons
    RECT_DELOK,
    RECT_DELCANCEL,
    RECT_PAGECONNECT,// a remote's page's Connect or Disconnect
    RECT_ROOTS,      // the guard page's roots field
    RECT_SESSMENU,   // the menu bar's Session menu
    RECT_ZOOMRESET,  // View's Actual size
    RECT_EDITCANCEL, // the compose box's Cancel of the editor
    RECT_CMUNDO,     // its context menu's Undo, Copy, Paste, Select all,
    RECT_CMCOPY,     // and Open in editor
    RECT_CMPASTE,
    RECT_CMSELALL,
    RECT_CMEDITOR,
    RECT_CMREDO,
    RECT_CMCUT,
    RECT_MODEL0,     // /model's first model
    RECT_THEME0,     // the theme's choices in its open list
    RECT_PANE0 = RECT_THEME0 + THEME_COUNT,  // View's toggle of each pane
    RECT_COUNT = RECT_PANE0 + 11,
};

// The panes, which the View menu shows and hides
enum : i32 {
    PANE_SESSIONS,
    PANE_TRANSCRIPT,
    PANE_PROMPT,
    PANE_TODO,
    PANE_PLAN,
    PANE_JOBS,
    PANE_LOG,
    PANE_HOSTS,
    PANE_SETTINGS,
    PANE_HELP,
    PANE_METRICS,
    PANE_COUNT,
};

static char const *panenames[] = {
    "Sessions", "Transcript", "Prompt", "Todo", "Plan", "Jobs", "Log", "Hosts",
    "Settings", "Help", "Metrics",
};
static_assert(countof(panenames) == PANE_COUNT);
static_assert((i32)RECT_COUNT == (i32)RECT_PANE0 + (i32)PANE_COUNT);

// Zoom steps, as browsers have them: the View menu's, and Ctrl+wheel's
static f32 zoomsteps[] = {
    0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3,
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
    i64    maxwakeus;   // records applied outside frames
    i64    layoutus;
    i64    deadline;    // relative to the snapshot, I64MAX if none
    i32    duewhy;
    i32    owed;
    i32    changing;
    u32    busy;
    i64    at;          // when it was taken
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

// Update engine: deploy the engine for this protocol and version on a
// remote host, on a thread of its own, its progress and outcome coming
// to the tasks mailbox (TASK_DEPLOYED). Then, if nothing runs there,
// the host is stopped and reached again, which starts the new engine
// (deployrecord); else the user is told how it will take over.
enum : i32 {
    UPD_NONE,
    UPD_DEPLOYING,
    UPD_CHECKING,  // deployed: asking the host whether it is idle
    UPD_RESTART,   // stopped, to be reached again
    UPD_DONE,
    UPD_FAILED,
};

// Where sessions run: this process's engine, always first, and hosts
// reached through the client library (remote.cpp): background hosts on
// this machine, at the endpoints their registrations name, and remote
// ones over ssh, as [remote NAME] sections set them up.
enum : i32 {
    GH_ENGINE,  // this process's engine, or its process (--engine)
    GH_LOCAL,   // a host on this machine
    GH_REMOTE,  // a [remote NAME] over ssh
};

struct GuiHost {
    GuiHost *next;
    i32      kind;
    Str      name;      // a remote's name, a local host's pid; "" for the engine
    Remote  *r;         // GH_LOCAL and GH_REMOTE while connected, else null
    Mailbox *inbox;     // its records, which wake the GUI
    i32      state;     // RS_*, from its "remote" records
    Str      msg;       // why it is down, or failed
    i64      retryat;   // epoch ms: it tries again then, while down
    b32      up;        // it answered hello
    b32      closing;   // asked to close: its records are let go
    b32      reconnect; // asked to connect while closing: once it has ended
    b32      own;       // this process's own host, after the window came back
    i64      pid;       // GH_LOCAL: the host's process
    i64      started;   // GH_LOCAL: epoch ms
    Str      endpoint;  // GH_LOCAL
    Str      regpath;   // GH_LOCAL: its registration, removed once it is gone
    i64      opens;     // times its link was made, for the checklist
    i64      downs;     // times it went down
    // Its engine, as its hello says (hostcan)
    Str        version;
    Slice<Str> cmds;    // the commands it takes
    Slice<Str> caps;    // and its features (putcaps)
    b32        relist;  // a command changed its list: ask again (drainmail)
    u64        uid;     // unique to this host while it is one (addhost)
    // Update engine: deploying there, then the host stopped and reached
    // again (UPD_*)
    i32        updating;
    Str        updmsg;  // how it goes, or went
    // The models its own configuration names, for its sessions' model
    // picker where it is not the main host (hostmodels)
    Slice<Str> models;
    i64        modelsid;   // its config.get in flight, 0 if none
    b32        modelsgot;  // models is its answer since it came up
    Str        modelserr;  // why there is no answer
};

// A session the GUI knows: listed by the engine, or opened here.
struct GuiSession {
    GuiSession  *next;
    GuiHost     *host;      // where it runs
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
    Str          parent;    // a subagent's, from the list until opened
    i64          mtime;
    i64          jobs;      // running background jobs, from the list
    i64          openid;    // the open in flight, 0 if none
    i32          depth;     // under its parent in the sidebar, this frame
    b32          kidask;    // a subagent below it needs an answer, this frame
    // Its compose box (compose.cpp): the draft, in a reservation of its
    // own made as it is first needed, untouched pages costing nothing
    Region      *dregion;
    char        *draft;     // DRAFT_MAX+1 bytes, null until made
    DraftClock   dclock;
    ImGuiID      draftid;   // the box's item, as last drawn
    i32          hist;      // recalling history entry hist (1 the newest), else 0
    i32          histmsg;   // its message in the view
    Str          note;      // what the latest command from the box came to
    b32          noteerr;
    Str          cmdtext;   // that command, put back in the box if it fails
    i64          unqueue;   // the queued message being withdrawn, else 0
    i64          cmdid;
    Str          group;     // its group, from the list (only a root's counts)
    b32          archived;  // from the list (only a root's counts)
    b32          gone;      // deleted: shown nowhere
    Slice<SkillItem> skills;  // what /name invokes, as its engine lists them
    i64          skillsid;  // that list's skills in flight, 0 if none
};

// A command awaiting its reply.
struct Request {
    i64         id;
    i32         cmd;
    GuiSession *s;
    GuiHost    *h;   // where it went
};

// A file of a session's directory, as the file command brings it in
// parts: a spill file shown in the Output window.
struct OutFile {
    GuiSession *s;
    Str         path;   // relative to the session's directory
    Buf         text;
    i64         off;    // where the next part begins
    i64         size;
    b32         more;
    i64         id;     // the command in flight, 0 if none
    Str         err;
    Arena       mem;
    byte       *base;
};

// The Plan window's plan file, of the session shown, fetched again as
// its view's planver changes.
struct PlanFile {
    GuiSession *s;
    View       *v;
    i32         ver;    // the planver fetched, or being fetched
    Buf         text;
    i64         off;
    i64         id;     // the command in flight, 0 if none
    Str         err;
    Arena       mem;
    byte       *base;
};

// One setting in the settings window, from config.get.
struct CfgRow {
    Str        kind;
    Str        name;
    Str        key;
    Str        type;
    Str        value;    // as in effect
    Str        dflt;
    Str        desc;
    Str        file;     // where it is set, null for its default
    Str        like;     // a model it is inherited from, null if none
    i64        line;
    i64        fetched;  // a URL file's copy, Unix epoch ms, else 0
    b32        isdflt;
    b32        valid;
    Slice<Str> choices;  // an enum's, to pick from a list
    char      *buf;      // the field: the value, then FIELD_MAX bytes to spare
    iz         cap;
    b32        sent;     // an edit went out, until the next config.get
    Str        sentval;
    Rect       page;     // the first of a section's: its page in the list
};

// Where a section is defined, from config.get's headers.
struct CfgHead {
    Str kind;
    Str name;
    Str file;
    i64 line;
    b32 own;  // in the user's own file, which edits go to
};

// The layout file's writer, on a thread of its own: a write that is to
// survive a crash waits for the disk, tens of milliseconds on an SD card
// or a busy one, which no frame can spare. Layouts arrive in a mailbox,
// and the latest in each take is written. The last, as the GUI closes,
// is waited for.
enum : u32 {
    SAVE_LAYOUT = 1,
    SAVE_LAST,
};

struct Saver {
    Os      *os;
    Mailbox *in;
    Str      path;
    Arena    scratch;
    Event    done;     // the last layout is written
};

// Screenshots: a desktop call's PNG, out/CALL.png in its session's
// directory, which the file command brings in base64 through the
// session's host, one fetch at a time, decoded once whole, shown as a
// thumbnail on its tool row and whole in the Screenshot window.
// Thumbnails are kept in IMG_SLOTS, fetched as their rows are first
// drawn, the slot drawn longest ago making room. Each texture lives in
// a reservation of its own with its pixels, which goes once the
// renderer has let the texture go, a frame or two after it is dropped.
enum : i32 {
    IMG_SLOTS  = 24,
    IMG_THUMBW = 320,   // the thumbnail's box, in units at scale 1
    IMG_THUMBH = 200,
    IMG_TEXMAX = 8192,  // pixels along a texture's edge, else shrunk
    IMG_STRMAX = 256,   // bytes of a path or an error kept
};

enum : iz {
    IMG_FILEMAX   = sizeof(void *)==8 ? (iz)32<<20 : (iz)8<<20,  // bytes
    IMG_MAXPIXELS = sizeof(void *)==8 ? (iz)1<<24  : (iz)1<<22,
    // A fetch's: the PNG, then its pixels and the decoder's temporaries
    IMG_REGION    = 2*IMG_FILEMAX + 25*IMG_MAXPIXELS + ((iz)1<<20),
};

enum : i32 {
    IMG_EMPTY,
    IMG_WANT,    // waiting for its fetch
    IMG_FETCH,
    IMG_READY,
    IMG_FAILED,
};

struct GuiTex {
    GuiTex        *next;   // in the list of the dying
    Region        *r;
    ImTextureData  td;
};

// A string in a buffer of its own, cut to fit.
struct ImgStr {
    u8 buf[IMG_STRMAX];
    iz len;
};

struct ImgSlot {
    GuiSession *s;
    ImgStr      path;
    i32         state;   // IMG_*
    GuiTex     *tex;
    ImgStr      err;
    i64         used;    // the frame that last drew it
    i64         opens;   // its host's links when it failed
};

struct ImgView {
    GuiSession *s;       // null if none
    ImgStr      path;
    i32         w;       // the image's size, as its record gave it
    i32         h;
    i32         state;   // IMG_*
    GuiTex     *tex;
    ImgStr      err;
    b32         open;
    b32         focus;   // brought to the front next frame
    b32         fit;     // scaled down to fit the window
};

struct ImgFetch {
    GuiSession *s;       // null if none
    ImgStr      path;
    i64         id;      // the file command in flight
    i64         off;
    Region     *r;
    Arena       mem;
    u8         *png;     // size bytes, once the first part came
    i64         size;
};

struct Images {
    ImgSlot   slots[IMG_SLOTS];
    ImgView   view;
    ImgFetch  fetch;
    GuiTex   *dying;
    i64       decodes;   // PNGs decoded, and how long that took
    i64       decodeus;
};

// An export of a session: Markdown made from its view, or its log as
// it is, events.jsonl, read here or through its host (file, in base64
// parts). A session not viewed here is opened first. Saved where the
// platform's dialog says (os_guisave), or the Markdown copied.
enum : i32 {
    EXPORT_MD,
    EXPORT_RAW,
};

enum : i32 {
    EXP_IDLE,
    EXP_OPENING,  // the session opens, its replay to come
    EXP_FETCH,    // the log comes through its host
    EXP_ASKING,   // the dialog is up
};

struct Export {
    GuiSession *s;
    i32         kind;     // EXPORT_*
    i32         state;    // EXP_*
    b32         clip;     // copied instead of saved
    b32         think;    // the Markdown's: reasoning, tool calls, their output
    b32         tools;
    b32         output;
    i64         id;       // the file command in flight
    i64         off;
    u64         ask;      // the dialog's, to know its answer
    Region     *r;        // what is saved, until it is
    Arena       mem;
    Buf         data;
    Str         status;   // how the last went
    b32         ok;
    i64         saves;    // exports written, for the checklist
    Str         saved;    // and where the last went
    b32         want;     // asked for from a menu: begun before the next frame
};

// Import session: a raw log picked in the platform's dialog
// (os_guiopenfile) and read here, then the new session dialog, filled
// from the log's records for the host it shows, then the log uploaded to
// the host picked, a part at each reply, and imported there (IMP_*). A
// failure opens the dialog again with why, and trying again uploads the
// log anew, as the engine removes what was staged whatever comes of it.
enum : i32 {
    IMP_IDLE,
    IMP_ASKING,     // the open dialog is up
    IMP_DIALOG,     // the new session dialog is up for it
    IMP_UPLOADING,  // its parts go to the host
    IMP_IMPORTING,  // and then the import
};

struct Import {
    i32      state;      // IMP_*
    u64      ask;        // the open dialog's, to know its answer
    Region  *r;          // what follows, until the next import
    Arena    mem;
    Str      data;       // the log
    Str      path;       // where it came from
    Str      title;      // and from its records: the latest title, the
    Str      cwd;        // session record's cwd, and the model as the
    Str      model;      // latest model record left it
    GuiHost *filled;     // the host the dialog was filled for
    b32      modelwait;  // its models were yet to come
    GuiHost *to;         // where it goes, as what the dialog said
    Str      newcwd;
    Str      newmodel;
    Str      token;      // the upload's, once the first part is in
    i64      off;        // bytes uploaded
    i64      id;         // the command in flight
    Str      status;     // how the last went
    i64      imports;    // imports made, for the checklist
};

// The dialog that names something: a session's title, a new group for
// it, or a group's new name.
enum : i32 {
    NAME_RENAME,
    NAME_NEWGROUP,
    NAME_GROUP,
};

// Item rectangles by name, for the checklist: menu items by label,
// headings and fold arrows by what they are for.
enum : i32 {
    NAMED_MAX    = 96,
    SIDEDUMP_MAX = 1<<14,  // bytes of the sidebar's tree as text
};

struct Named {
    u8   name[64];
    iz   len;
    Rect r;
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
    Palette           *pal;        // the theme in effect
    i32                themeset;   // THEME_*, from [ui] theme
    i32                thinkset;   // THINK_*, from [ui] thinking
    i32                toolset;    // TOOLS_*, from [ui] tools
    b32                sysdark;    // the system prefers dark
    b32                syshas;     // the platform said so once
    b32                titleok;    // the window's frame follows the theme
    b32                clickthrough; // an activating click reaches the UI

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
    i64                maxwakeus;  // the longest gui_wake
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
    b32                jobsvisible;  // the Jobs window drew last frame
    b32                jobsdirty;    // job output it has not shown
    i64                lastjobs;     // when it last showed new output
    Arena              pendmem;    // deltas held back until a content frame
    byte              *pendbase;
    Buf                pending;    // their records, one per line

    // The engine
    i32                enginekind; // ENGINE_*
    Engine            *engine;     // in this process
    Config            *config;     // its configuration, read before it starts
    Proc              *proc;       // or in a child process
    Arena              linemem;    // its output's partial line
    byte              *linebase;
    Buf                line;
    Str                cfgpath;    // -c FILE, else null for the default
    i64                nextid;
    GuiHost           *cmdhost;    // where the command being written goes
    Request            reqs[REQ_MAX];
    b32                engineok;   // it said hello
    b32                enginegone; // it stopped
    Str                engineerr;  // why it is unusable, null if fine
    b32                quitting;   // quit was sent: exit once it stops
    i64                quitby;     // µs: exit then regardless
    b32                closereq;   // the user asked to close the window
    i64                overflows;  // times records were lost
    b32                reissuing;  // replies may be lost: send again
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

    // The sidebar's tree, and the session operations
    Map<b32>          *folds;      // the user's fold choices (sidebar.cpp)
    Slice<Str>         groups;     // the groups' order, empty ones included
    b32                sidedirty;  // folds or groups changed: kept with the layout
    GuiSession        *menusess;   // the session the context menu is for
    char              *namebuf;    // the naming dialog's field
    i32                nameask;    // NAME_*
    GuiSession        *namesess;
    Str                namegroup;  // NAME_GROUP: the group renamed
    b32                opennamer;  // open it next frame
    Slice<GuiSession *> delroots;  // to delete once confirmed, with their subagents
    i32                delcount;   // sessions in all
    Str                delgroup;   // the group going with them, if any
    b32                opendelsess;
    i64                deletes;    // delete replies, for the checklist
    Mailbox           *tasks;      // the dialogs' and deployments' outcomes
    Export             exp;
    Import             imp;
    b32                openexport; // the Markdown export's options
    GuiSession        *exportsess;
    char              *modelbuf;   // the model picker's own name
    u64                hostuids;

    // Hosts
    GuiHost           *hosts;      // the engine's first, then the others
    GuiHost           *spare;      // hosts gone, to make over
    GuiHost           *main;       // where the configuration is, and new
                                   // sessions by default: the engine's, or
                                   // this process's own host once its
                                   // window comes back
    GuiHost           *pendhost;   // whose records are held back
    Host              *host;       // this process as a host, else null
    i32                hostdone;   // atomic: that host has stopped
    b32                bg;         // the window is gone, the host runs on
    i64                shedat;     // total.frames as it went
    i64                shedframes; // frames drawn while shed: none, ever
    i32                shedreq;    // between frames: 1 the window goes,
                                   // 2 to the background
    i64                hostidle;   // ticks: --host-idle, else [host] idle_exit
    b32                trayup;     // a tray icon brings the window back
    iz                 guilock;    // the state directory's (door.cpp), or -1
    i32                raises;     // launches that handed off to this window
    b32                raisereq;   // between frames: come forward for one
    Str                raisetoken; // its activation token (Wayland)
    Str                hosterr;    // why this process could not host
    Str                whynobg;    // why it cannot, as the close dialog opened
    i32                hostfound;  // background hosts found at startup
    char              *remname;    // the Hosts panel's new remote
    char              *remssh;
    char              *rempath;
    Str                remstatus;  // and how adding it went
    b32                remok;
    GuiHost           *newhost;    // the new session's host
    OutFile            out;        // the Output window's file
    PlanFile           plan;       // the Plan window's
    Images             img;        // screenshots
    b32                outopen;
    b32                outfocus;   // brought to the front next frame

    // Transcript
    f64                scrollto;   // -1 if none
    b32                dragging;
    i32                menumsg;    // the message the context menu is for
    View              *copiedview; // a code block's Copy, clicked then
    i32                copiedmsg;
    i32                copiedblk;
    i64                copiedat;   // µs, 0 if never

    // The find bar
    ViewSearch        *search;
    char              *findbuf;
    b32                findopen;
    b32                findfocus;  // focus its field next frame
    b32                findactive; // its field had the keyboard
    b32                findjump;   // scroll to the current hit
    b32                findwant;   // the current hit is the first found
    i32                findfrom;   //   from this message on

    // Dialogs and settings
    char              *newcwd;     // the new session's fields
    char              *newmodel;
    Str                firstcwd;   // newcwd's first, a directory here
    Str                newerr;
    b32                opennew;
    b32                newplan;    // in plan mode
    i64                newid;      // the new command in flight
    char              *denynote;   // "Deny with note"
    Str                denyreq;
    GuiSession        *denysess;
    i32                opendeny;   // 2 to keep planning
    b32                denyplan;
    b32                openrewind; // the rewind confirmation, for
    GuiSession        *rewsess;
    Str                rewcall;    // a call's change and those after, or
    i32                rewturn;    // a turn's on
    b32                rewforce;   // conflicts overwritten
    i64                rewindid;   // rewind in flight, 0 if none
    Arena              cfgmem;     // the latest config.get, reset for each
    byte              *cfgbase;
    Json              *cfg;        // its reply, null until one arrives
    Slice<CfgRow>      cfgrows;
    Slice<CfgHead>     cfgheads;
    Str                cfgpkind;   // the settings' page: a section's kind
    Str                cfgpname;   // and name, in cfgpbuf ("" and "" for
    u8                *cfgpbuf;    // the top level), FIELD_MAX bytes
    b32                cfgdiag;    // the page is Diagnostics instead
    b32                cfgshowsel; // scroll the list to it
    b32                cfgtwopane; // the page list was drawn beside the page
    b32                cfgcombo;   // or as a combo, which is open
    f32                cfghscroll; // room the settings had to scroll sideways
    i64                cfgframe;   // the frame they were last drawn in
    f32                hostshscroll; // and the Hosts panel, likewise
    i64                hostsframe;
    Str                addkind;    // the kind of a new section, and its name
    char              *addname;
    Str                adderr;     // why it was not added
    b32                openadd;    // open the new section's dialog
    b32                opendel;    // open the deletion's confirmation
    i64                cfgdelid;   // config.set {delete} in flight, 0 if none
    Str                cfgdelwhat; // what it deletes, "[kind name]"
    char              *tplname;    // a template argument to set: its name
    char              *tplval;     // and value
    i64                cfgget;     // config.get in flight, 0 if none
    i64                cfgrefresh; // and its id if it refreshes includes
    i64                cfgrefreshed; // config.get {refresh} replies so far
    i64                cfgset;     // config.set in flight, 0 if none
    i64                cfgsets;    // config.set replies so far
    Str                cfgstatus;  // the latest config.set outcome
    b32                cfgok;      // and whether it worked

    // Compose box: the editor a draft is out in (one at a time), and
    // what the box's popups have open
    Str                editor;     // [ui] editor
    Mailbox           *edbox;      // its process's records
    GuiSession        *edsess;     // whose draft it edits, or null
    Proc              *edproc;
    u64                edid;
    Str                edpath;     // the draft's file
    Str                edlog;      // the editor's output
    Str                edname;     // as the box names it
    i32                slashsel;   // the slash popup's pick, among its matches
    GuiSession        *pickmodel;  // /model's list is open for it
    i32                menuact;    // a context menu pick waiting for the box
    i32                menuwait;   // frames it has waited

    // Menu bar, toolbar and layout
    i32                tps;
    b32                bursts;
    b32                pauses;
    b32                showdemo;
    b32                panes[PANE_COUNT];  // shown, as the View menu says
    i32                panefocus;  // PANE_*+1 to bring to the front, else 0
    b32                openabout;
    f32                fontbase;   // [ui] font_size
    f32                zoom;       // the View menu's, for this run only
    f32                zoomwheel;  // Ctrl+wheel notches short of a step
    b32                fontdirty;  // the size changed: fontsize next frame
    WinGeom            geom;       // the window's, as last placed
    b32                firstlayout;
    i32                jobsfocus;  // frames until the Jobs tab is selected
    b32                resetpending;
    b32                resetlayout;
    b32                openmodal;

    // Files and log
    Str                statedir;
    Str                inipath;
    Saver             *saver;      // writes inipath, or null: written here
    iz                 logfd;
    Slice<Str>         logs;

    // Measurement
    i64                quitat;     // µs, 0 if none
    i64                histreq;    // when history was requested
    b32                histdone;   // the history's last record arrived
    i64                histwakeus; // the GUI's own work on it: applying
    i64                histwakes;  // its records in this many wakes,
    i64                histparseus;  // parsing them, of that
    i64                histframeus;  // and drawing frames meanwhile
    i64                histframes;
    i32                bench;      // benchmark phase, 0 if none
    i32                benchstep;
    i64                benchmax;
    i64                benchsum;
    i64               *benchtimes;   // BENCH_MAX frame times
    i64                benchlaid;
    i64                benchseg[3];  // this frame: new frame, UI, render (µs)
    i64                benchlaid1;   // messages laid out in this frame
    i32                benchglyphs;  // glyphs packed into the atlas in it
    i32                benchtex;     // its texture's width then, and before
    i32                benchtex0;
    i64                benchsave;    // µs saving the layout in it
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
    b32                roclock;      // a read-only view's turn had a clock
    b32                queuedshown;  // the strip showed the line's position
    i32                taskshown;    // and how many subagents work
    b32                compactingshown;
    b32                fallbackshown;  // a retry on a fallback model
    b32                kidaskshown;  // the sidebar: a subagent needs an answer
    i32                todoshown;    // items the todo list drew
    i32                planshown;    // bytes of the plan the Plan window drew
    i32                jobsshown;    // jobs the jobs panel drew
    i32                infoshown;    // instruction files the info listed
    i32                infoagents;   // of them AGENTS.md
    i32                bgbadge;      // the session shown was badged background
    b32                outshown;     // the Output window showed text
    i32                thumbsdrawn;  // thumbnails drawn with their image
    i32                thinkrows;    // reasoning headers the transcript drew
    i64                thinkchars;   // the latest one's count, -1 if none
    i64                thinksecs;    // and its clock while it streams, else -1
    i64                thinkchars0;  // as a check last saw them
    i64                thinksecs0;
    b32                thinkhidshown; // the strip said reasoning is hidden
    b32                copiedshown;  // a code block's button said Copied
    i32                gaugeshown;   // the gauge drew: 1, 2 if amber
    i32                drivecodemsg; // the code block selectcode chose, or
    i32                drivecodeblk; //   -1 for any
    Anchor             driveanchor;  // the top edge before a resize
    i32                drivefound;   // the find hit first current
    b32                shotshown;    // the Screenshot window drew its image
    b32                downshown;    // a host's countdown to its next try
    b32                elseshown;    // closing said what runs on elsewhere
    b32                failshown;    // the Hosts panel said why one failed
    GuiHost           *hostitems[4]; // the new session's host list, as drawn
    i64                deltamark;    // the shown session's deltas, as marked
    Str                driverwd;     // --rcwd: the remote one
    b32                kidlinkshown; // a task row asked to answer its subagent
    i64                rewsent;      // rewinds sent
    i32                drivewait;    // W_* condition it waits for
    i64                holduntil;    // µs: the inbox is left alone until
    b32                held;         // a wake came during the hold
    Str                cfgsnap;      // the config file before an edit
    GuiSession        *drivesess;    // the session OP_MARKSESS remembered
    b32                modelsshown;  // /model's list drew
    i64                tailseen;     // tool.out records for running rows
    b32                drive;
    i32                drivepc;
    i64                driveat;
    i32                drivechecks;
    i32                drivefails;
    i64                driveframes;  // total.frames when a wait began
    f32                splitx;       // dock splitter before the drag
    Rect               rects[RECT_COUNT];
    Named              named[NAMED_MAX];
    i32                nnamed;
    u8                 sidedump[SIDEDUMP_MAX];  // the sidebar's tree, drawn last
    iz                 sidelen;
    ImGuiID            dockspace;
    i64                lastsave;     // µs
    i64                idlesaves;    // saves made with no input for 1 s
    i64                saves;        // all saves, and as of the last OP_MARK
    i64                marksaves;
    i64                lastinput;    // µs
    b32                drivemoved;   // the synthetic mouse has a position
    f32                drivex;       // where it is
    f32                drivey;
    Str                drivecode;    // expected on the clipboard
    Str                driveclip;    // the latest text set on it (driveclipset)
    f32                dpibase;      // scale before the DPI step
    b32                inmodal;
    i64                modalframes;  // drawn from inside modal loops
    DriveOp           *ops;          // the script
    i32                nops;
    b32                minclose;     // ask to close once minimized
    Counts             markcounts;   // totals at the last OP_MARK
    i64                markat;
    i32                reveal;       // RECT_* the settings scroll to, or -1
    Str                showpage;     // the settings page the list scrolls to
    Str                longval;      // the long setting before its edit
    WinGeom            geommark;     // the window's, as OP_GEOMMARK found it
    b32                slashshown;   // the slash popup drew
    b32                cmenushown;   // the compose box's context menu drew
    u32                cmenustate;   // and what it enabled: 1 undo, 2 redo,
                                     // 4 cut and copy, 8 paste, 16 editor
    i32                histshown;    // the box showed history entry N/M
    b32                editshown;    // it said an editor was at work
    i32                queueshown;   // queued messages drawn above the box
    b32                promptfocus;  // activate the box when next drawn
};

// Log to standard error, the log file, and the Log window, which then
// needs a frame to show it.
static void guilog(Gui *g, Str msg)
{
    scheddirty(&g->sched);
    msg = clone(&g->perm, msg);  // first: msg may be in frame scratch
    Arena a = g->frame;
    Buf   b(&a, msg.len+64);
    print(&b, "prospero: ");
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

// A session of a host.
static GuiSession *findsess(Gui *g, GuiHost *h, Str sid)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host==h && s->sid==sid) {
            return s;
        }
    }
    return 0;
}

// A session of any host on this machine, which share its state
// directory: the engine's, or a background host's.
static GuiSession *findlocal(Gui *g, Str sid)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host->kind!=GH_REMOTE && s->sid==sid) {
            return s;
        }
    }
    return 0;
}

static GuiSession *addsess(Gui *g, GuiHost *h, Str sid)
{
    GuiSession *s = alloc<GuiSession>(&g->perm);
    s->host  = h;
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

// A session's view model, made on first use, which its records fill. A
// remote session's files are elsewhere, named relative to its directory.
static View *sessview(Gui *g, GuiSession *s)
{
    if (!s->view) {
        iz mb = (iz)1 << 20;
        s->view = newview(&g->perm, g->vcache, os_reserve(g->os, 1024*mb),
                          os_reserve(g->os, 256*mb), os_reserve(g->os, 256*mb));
        if (s->host->kind != GH_REMOTE) {
            s->view->dir = sessdir(g, &g->perm, s->sid);
        }
        s->view->loose = s->host->kind != GH_ENGINE;  // as an open's reply says
        viewthink(s->view, g->thinkset);
        viewtools(s->view, g->toolset);
    }
    return s->view;
}

// The session shown, or none.
static void draftsend(Gui *, GuiSession *);

static void setcur(Gui *g, GuiSession *s)
{
    if (g->sess && g->sess!=s) {
        draftsend(g, g->sess);  // left: its draft is kept now
    }
    g->sess = s;
    g->view = s && s->view ? s->view : g->nosession.view;
    scheddirty(&g->sched);
}

// Remember a command until its reply, in place of the oldest if full.
static void request(Gui *g, i64 id, i32 cmd, GuiSession *s, GuiHost *h)
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
    slot->h   = h;
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

// Begin a command for a session, else for a host (the main one if
// null), awaiting a reply. Its id is g->nextid. Finish it with cmdsend,
// before beginning another.
static EvOut cmdopen(Gui *g, Buf *b, i32 cmd, GuiSession *s, GuiHost *h = 0)
{
    i64   id = ++g->nextid;
    EvOut w  = cmdbegin(b, cmd, id);
    if (s) {
        putkey(&w.json, "sid");
        putstr(&w.json, s->sid);
    }
    g->cmdhost = s ? s->host : h ? h : g->main;
    request(g, id, cmd, s, g->cmdhost);
    return w;
}

static void fakecommand(Gui *g, Arena scratch, Str line);

// Whether commands can go out: the window draws (Win32: from inside
// os_guiopen) before the engine starts.
static b32 engineup(Gui *g)
{
    return g->enginekind==ENGINE_FAKE || g->engine || g->proc || g->main->kind!=GH_ENGINE;
}

// A reply to a command that could not go out, as its host would give
// one, taken with its host's records.
static void cmdfail(Gui *g, GuiHost *h, Arena scratch, i64 id, Str err)
{
    Buf   b(&scratch, 256 + err.len);
    EvOut w = evbegin(&b, {}, 0, os_time(g->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 0);
    putstrkey(&w.json, "err", err);
    mailpost(h->inbox, MAIL_OUT, 0, evend(&w), 0);
}

// Finish a command and send it to its host (cmdopen). The scratch must
// follow the command's buffer.
static void cmdsend(Gui *g, EvOut *w, Arena scratch)
{
    Str      line = evend(w);
    GuiHost *h    = g->cmdhost;
    if (h->kind != GH_ENGINE) {
        // The library holds it while the host has not answered hello
        if (!h->r || h->closing) {
            cmdfail(g, h, scratch, g->nextid, "not connected to the host");
        } else if (!remotetry(h->r, cuttail(line, 1))) {
            cmdfail(g, h, scratch, g->nextid, "too much is waiting to go to the host");
        }
        return;
    }
    switch (g->enginekind) {
    case ENGINE_INPROC:
        if (g->engine && !g->host) {
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

// Ask a host (the main one if null) for its sessions, which arrive as a
// sessions record.
static void guilist(Gui *g, GuiHost *h = 0)
{
    h = h ? h : g->main;
    if (g->enginekind==ENGINE_FAKE || (h->kind!=GH_ENGINE && !h->up)) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdopen(g, &b, CMD_LIST, 0, h);
    cmdsend(g, &w, a);
}

// Every host's sessions.
static void guilistall(Gui *g)
{
    for (GuiHost *h = g->hosts; h; h = h->next) {
        guilist(g, h);
    }
}

static void guiconfigget(Gui *g, b32 refresh = 0)
{
    if (g->enginekind==ENGINE_FAKE || g->cfgget || !engineup(g)) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdopen(g, &b, CMD_CONFIGGET, 0);
    if (refresh) {
        putkey(&w.json, "refresh");
        putint(&w.json, 1);
        g->cfgrefresh = g->nextid;
    }
    g->cfgget = g->nextid;
    cmdsend(g, &w, a);
}

// The models a session of a host other than the main one can switch to
// are those its engine's own configuration names, which may not be this
// one's: ask it (config.get) once each time it comes up (onremote).
static void hostmodels(Gui *g, GuiHost *h)
{
    if (h==g->main || h->modelsgot || h->modelsid || !h->up || !h->r) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdopen(g, &b, CMD_CONFIGGET, 0, h);
    h->modelsid = g->nextid;
    cmdsend(g, &w, a);
}

// The models in a config.get reply: the default, then each [model NAME]
// section's, as cfgmodels lists them.
static Slice<Str> replymodels(Arena *a, Json *j)
{
    Slice<Str> r = {};
    for (Json *s = jsonget(j, "settings")->child; s; s = s->next) {
        Str kind = jsonstr(jsonget(s, "kind"));
        Str m    = !kind.len && jsonstr(jsonget(s, "key"))=="model" ?
                   jsonstr(jsonget(s, "value")) :
                   kind=="model" ? jsonstr(jsonget(s, "name")) : Str{};
        b32 dup  = !m.len;
        for (iz k = 0; !dup && k < r.len; k++) {
            dup = r[k] == m;
        }
        if (!dup) {
            r = push(a, r, m);
        }
    }
    return r;
}

// A host's answer to hostmodels, kept anew only when it changed.
static void onhostmodels(Gui *g, GuiHost *h, Arena scratch, Json *j, b32 ok, Str err)
{
    h->modelsid  = 0;
    h->modelsgot = 1;
    h->modelserr = ok ? Str{} : clone(&g->perm, err);
    Slice<Str> m    = ok ? replymodels(&scratch, j) : Slice<Str>{};
    b32        same = m.len == h->models.len;
    for (iz i = 0; same && i < m.len; i++) {
        same = m[i] == h->models[i];
    }
    if (!same) {
        h->models = {};
        for (iz i = 0; i < m.len; i++) {
            h->models = push(&g->perm, h->models, clone(&g->perm, m[i]));
        }
    }
    scheddirty(&g->sched);
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

// Delete a section of the user's file.
static void guiconfigdelete(Gui *g, Str kind, Str name)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + kind.len + name.len);
    EvOut w = cmdopen(g, &b, CMD_CONFIGSET, 0);
    putkey(&w.json, "kind");
    putstr(&w.json, kind);
    putkey(&w.json, "name");
    putstr(&w.json, name);
    putkey(&w.json, "delete");
    putint(&w.json, 1);
    g->cfgset   = g->nextid;
    g->cfgdelid = g->nextid;
    Buf d(&g->perm, 16 + kind.len + name.len);
    putbyte(&d, '[');
    print(&d, kind);
    if (name.len) {
        putbyte(&d, ' ');
        print(&d, name);
    }
    putbyte(&d, ']');
    g->cfgdelwhat = finish(&d);
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

// No more records of a session: its host may close it, unless another
// of its clients has it open.
static void imgdrop(Gui *, GuiSession *);

static void guiclose(Gui *g, GuiSession *s)
{
    imgdrop(g, s);
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_CLOSE, s);
    cmdsend(g, &w, a);
}

// Ask for the next part of the Output window's file, if any.
static void guifile(Gui *g)
{
    OutFile *o = &g->out;
    if (!o->s || o->id || (o->off && !o->more)) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + o->path.len);
    EvOut w = cmdopen(g, &b, CMD_FILE, o->s);
    putkey(&w.json, "path");
    putstr(&w.json, o->path);
    putkey(&w.json, "off");
    putint(&w.json, o->off);
    o->id = g->nextid;
    cmdsend(g, &w, a);
}

// Ask for the plan file's next part.
static void planpart(Gui *g)
{
    PlanFile *f = &g->plan;
    Arena     a = newarena(g->cmdr);
    Buf       b(&a, 256);
    EvOut     w = cmdopen(g, &b, CMD_FILE, f->s);
    putkey(&w.json, "path");
    putstr(&w.json, "plan.md");  // in the session's directory
    putkey(&w.json, "off");
    putint(&w.json, f->off);
    f->id = g->nextid;
    cmdsend(g, &w, a);
}

static b32 hostlive(Gui *, GuiSession *);

// Fetch the shown session's plan file, from its start, unless what the
// Plan window has is the latest or on its way.
static void guiplan(Gui *g)
{
    PlanFile   *f = &g->plan;
    GuiSession *s = g->sess;
    View       *v = g->view;
    if (!s || !v || !v->planpath.len || s->gone || !hostlive(g, s)) {
        return;
    }
    if (f->id || (f->s==s && f->v==v && f->ver==v->planver)) {
        return;
    }
    if (!f->base) {
        f->mem  = newarena(os_reserve(g->os, (iz)1<<20));
        f->base = f->mem.beg;
    }
    f->mem.beg = f->base;
    f->s    = s;
    f->v    = v;
    f->ver  = v->planver;
    f->text = Buf(&f->mem, 1<<16);
    f->off  = 0;
    f->err  = {};
    planpart(g);
}

// Show a file of a session's directory in the Output window, taken
// through its host, which may be on another machine.
static void showfile(Gui *g, GuiSession *s, Str path)
{
    OutFile *o = &g->out;
    if (!o->base) {
        o->mem  = newarena(os_reserve(g->os, (iz)1<<28));
        o->base = o->mem.beg;
    }
    o->mem.beg = o->base;
    o->s       = s;
    o->path    = clone(&o->mem, path);
    o->text    = Buf(&o->mem, 1<<16);
    o->off     = 0;
    o->size    = 0;
    o->more    = 0;
    o->id      = 0;
    o->err      = {};
    g->outopen  = 1;
    g->outfocus = 1;
    guifile(g);
    scheddirty(&g->sched);
}

// A part of the plan arrived, and then the next, if more, as long as
// the plan is no longer than the engine takes (PLAN_MAX).
static void onplanfile(Gui *g, Json *j, b32 ok, Str err)
{
    PlanFile *f = &g->plan;
    f->id = 0;
    if (!ok) {
        f->text.len = 0;
        f->err = err=="no such file" ? Str{} : clone(&f->mem, err);
        return;
    }
    Str text = jsonstr(jsonget(j, "text"));
    if (f->text.len+text.len > (iz)64<<10) {
        f->err = "too long to show";
        return;
    }
    print(&f->text, text);
    f->off = jsonint(jsonget(j, "off"), f->off);
    if (jsonint(jsonget(j, "more"), 0) && f->s) {
        planpart(g);
    }
    scheddirty(&g->sched);
}

// A part of the Output window's file arrived. The window keeps 64 MiB at
// most, and asks for more only when told to.
static void onfile(Gui *g, Json *j, b32 ok, Str err)
{
    OutFile *o = &g->out;
    o->id = 0;
    if (!ok) {
        o->err = clone(&o->mem, err.len ? err : Str("cannot read the file"));
        return;
    }
    Str text = jsonstr(jsonget(j, "text"));
    if (o->text.len+text.len > (iz)64<<20) {
        o->more = 0;
        o->err  = "too long to show more";
        return;
    }
    print(&o->text, text);
    o->off  = jsonint(jsonget(j, "off"), o->off);
    o->size = jsonint(jsonget(j, "size"), 0);
    o->more = jsonint(jsonget(j, "more"), 0) != 0;
}


// Screenshots

static Str imgstr(ImgStr *b)
{
    return {b->buf, b->len};
}

static void imgset(ImgStr *b, Str s)
{
    b->len = s.len<(iz)IMG_STRMAX ? s.len : (iz)IMG_STRMAX;
    copybytes(b->buf, s.data, b->len);
}

static b32 imgis(GuiSession *s, ImgStr *path, GuiSession *s2, Str path2)
{
    return s && s==s2 && imgstr(path)==path2;
}

// A texture of a copy of the pixels, which the renderer creates when it
// next draws. Null if there is no memory for it.
static GuiTex *newtex(Gui *g, u8 *rgba, i32 w, i32 h)
{
    iz      n = (iz)w * h * 4;
    Region *r = os_reserve(g->os, n + (iz)sizeof(GuiTex) + 4096);
    if (!r) {
        return 0;
    }
    Arena   a = newarena(r);
    GuiTex *t = alloc<GuiTex>(&a);
    u8     *p = allocbytes(&a, n);
    copybytes(p, rgba, n);
    t->r = r;
    ImTextureData *td = &t->td;
    td->Format        = ImTextureFormat_RGBA32;
    td->Width         = w;
    td->Height        = h;
    td->BytesPerPixel = 4;
    td->Pixels        = p;
    td->UseColors     = 1;
    td->UsedRect.w    = (unsigned short)w;
    td->UsedRect.h    = (unsigned short)h;
    td->UpdateRect.x  = (unsigned short)~0;
    td->UpdateRect.y  = (unsigned short)~0;
    td->Status        = ImTextureStatus_WantCreate;
    ImGui::RegisterUserTexture(td);
    return t;
}

// Drop a texture: the renderer destroys it at the next frame, and the
// frame after that releases it (imgreap). Frames go on until then.
static void killtex(Gui *g, GuiTex *t)
{
    if (t) {
        t->td.WantDestroyNextFrame = 1;
        t->next = g->img.dying;
        g->img.dying = t;
        scheddirty(&g->sched);
    }
}

// Between frames: release the textures the renderer has destroyed.
static void imgreap(Gui *g)
{
    for (GuiTex **p = &g->img.dying; *p;) {
        GuiTex *t = *p;
        if (t->td.Status == ImTextureStatus_Destroyed) {
            *p = t->next;
            ImGui::UnregisterUserTexture(&t->td);
            t->td.Pixels = 0;
            os_release(g->os, t->r);
        } else {
            p = &t->next;
        }
    }
}

// Ask for the fetch's next part.
static void imgrequest(Gui *g)
{
    ImgFetch *f = &g->img.fetch;
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + f->path.len);
    EvOut w = cmdopen(g, &b, CMD_FILE, f->s);
    putkey(&w.json, "path");
    putstr(&w.json, imgstr(&f->path));
    putkey(&w.json, "off");
    putint(&w.json, f->off);
    putkey(&w.json, "encoding");
    putstr(&w.json, "base64");
    f->id = g->nextid;
    cmdsend(g, &w, a);
}

static void imgdone(Gui *g, Png *p, Str err);

// Start the next fetch, if none runs: the Screenshot window's first,
// then the thumbnail drawn latest.
static void imgnext(Gui *g)
{
    Images   *m = &g->img;
    ImgFetch *f = &m->fetch;
    if (f->s) {
        return;
    }
    if (m->view.s && m->view.state==IMG_WANT) {
        f->s = m->view.s;
        imgset(&f->path, imgstr(&m->view.path));
    } else {
        ImgSlot *best = 0;
        for (i32 i = 0; i < IMG_SLOTS; i++) {
            ImgSlot *sl = m->slots + i;
            if (sl->state==IMG_WANT && (!best || sl->used>best->used)) {
                best = sl;
            }
        }
        if (!best) {
            return;
        }
        f->s = best->s;
        imgset(&f->path, imgstr(&best->path));
    }

    Str path = imgstr(&f->path);
    for (i32 i = 0; i < IMG_SLOTS; i++) {
        ImgSlot *sl = m->slots + i;
        if (imgis(sl->s, &sl->path, f->s, path)) {
            sl->state = IMG_FETCH;
        }
    }
    if (imgis(m->view.s, &m->view.path, f->s, path)) {
        m->view.state = IMG_FETCH;
    }
    f->r = os_reserve(g->os, IMG_REGION);
    if (!f->r) {
        imgdone(g, 0, "out of memory");
        return;
    }
    f->mem = newarena(f->r);
    imgrequest(g);
}

// The thumbnail's size in pixels: the image in the box at the display's
// scale, aspect kept, never larger than it is.
static Thumb thumbpixels(Gui *g, i32 w, i32 h)
{
    f32 fb = ImGui::GetIO().DisplayFramebufferScale.x;
    fb = fb>0 ? fb : 1;
    f32 bw = (f32)IMG_THUMBW * g->scale * fb;
    f32 bh = (f32)IMG_THUMBH * g->scale * fb;
    f32 s  = bw/(f32)w < bh/(f32)h ? bw/(f32)w : bh/(f32)h;
    s = s<1 ? s : 1;
    Thumb r = {};
    r.w = (f32)(i32)((f32)w*s + 0.5f);
    r.h = (f32)(i32)((f32)h*s + 0.5f);
    r.w = r.w<1 ? 1 : r.w;
    r.h = r.h<1 ? 1 : r.h;
    return r;
}

// The fetch ended, with the decoded image or an error: textures for
// whatever waits for it, and on to the next.
static void imgdone(Gui *g, Png *p, Str err)
{
    Images   *m    = &g->img;
    ImgFetch *f    = &m->fetch;
    Str       path = imgstr(&f->path);
    for (i32 i = 0; i < IMG_SLOTS; i++) {
        ImgSlot *sl = m->slots + i;
        if (!imgis(sl->s, &sl->path, f->s, path) || sl->state!=IMG_FETCH) {
            continue;
        }
        if (err.data) {
            sl->state = IMG_FAILED;
            sl->opens = sl->s->host->opens;
            imgset(&sl->err, err);
            continue;
        }
        Thumb  tp = thumbpixels(g, p->width, p->height);
        i32    tw = (i32)tp.w;
        i32    th = (i32)tp.h;
        Arena  a  = f->mem;
        u8    *px = tw==p->width && th==p->height ? p->rgba :
                    shrink(&a, p->rgba, p->width, p->height, tw, th);
        sl->tex   = newtex(g, px, tw, th);
        sl->state = sl->tex ? IMG_READY : IMG_FAILED;
        imgset(&sl->err, sl->tex ? Str{} : Str("out of memory"));
    }
    ImgView *v = &m->view;
    if (imgis(v->s, &v->path, f->s, path) && v->state==IMG_FETCH) {
        if (err.data) {
            v->state = IMG_FAILED;
            imgset(&v->err, err);
        } else {
            // Too big for a texture: shrunk to fit one
            i32   w  = p->width;
            i32   h  = p->height;
            u8   *px = p->rgba;
            if (w>IMG_TEXMAX || h>IMG_TEXMAX) {
                f32   s = (f32)IMG_TEXMAX / (f32)(w>h ? w : h);
                i32   tw = (i32)((f32)w*s);
                i32   th = (i32)((f32)h*s);
                Arena a  = f->mem;
                tw = tw<1 ? 1 : tw;
                th = th<1 ? 1 : th;
                px = shrink(&a, px, w, h, tw, th);
                w  = tw;
                h  = th;
            }
            v->tex   = newtex(g, px, w, h);
            v->state = v->tex ? IMG_READY : IMG_FAILED;
            imgset(&v->err, v->tex ? Str{} : Str("out of memory"));
        }
    }
    if (f->r) {
        os_release(g->os, f->r);
    }
    *f = {};
    imgnext(g);
    scheddirty(&g->sched);
}

// A part of the fetch's file arrived: the next asked for, or once all
// is here, decoded.
static void onimgfile(Gui *g, Arena scratch, Json *j, b32 ok, Str err)
{
    ImgFetch *f = &g->img.fetch;
    f->id = 0;
    if (!ok) {
        imgdone(g, 0, err.len ? err : Str("cannot read the file"));
        return;
    }
    Json *d = jsonget(j, "data");
    if (d->type != JSON_STRING) {
        imgdone(g, 0, "its host is too old to send images");
        return;
    }
    Unbase64 u    = unbase64(&scratch, jsonstr(d));
    i64      size = jsonint(jsonget(j, "size"), 0);
    if (!u.ok) {
        imgdone(g, 0, "a part of it came garbled");
        return;
    }
    if (!f->png) {
        if (size < 1) {
            imgdone(g, 0, "the file is empty");
            return;
        } else if (size > IMG_FILEMAX) {
            imgdone(g, 0, "the file is too big to show");
            return;
        }
        f->size = size;
        f->png  = allocbytes(&f->mem, (iz)size);
    }
    if (u.data.len > f->size-f->off || jsonint(jsonget(j, "off"), -1)!=f->off+u.data.len) {
        imgdone(g, 0, "the file changed while it was read");
        return;
    }
    copybytes(f->png+f->off, u.data.data, u.data.len);
    f->off += u.data.len;
    if (jsonint(jsonget(j, "more"), 0)) {
        if (!u.data.len) {
            imgdone(g, 0, "the file changed while it was read");
        } else {
            imgrequest(g);
        }
        return;
    } else if (f->off != f->size) {
        imgdone(g, 0, "the file changed while it was read");
        return;
    }

    i64 t0 = os_ticks(g->os);
    Png p  = pngdecode(&f->mem, {f->png, (iz)f->size}, IMG_MAXPIXELS);
    g->img.decodes++;
    g->img.decodeus += os_ticks(g->os) - t0;
    if (p.err.data) {
        imgdone(g, 0, concat(&scratch, "not an image it can show: ", p.err));
        return;
    }
    imgdone(g, &p, {});
}

// The thumbnail slot of a session's screenshot, as its row draws in
// this frame: fetched once, kept while there is room. Null if every
// slot was drawn in this frame.
static ImgSlot *imgslot(Gui *g, GuiSession *s, Str path)
{
    Images  *m   = &g->img;
    i64      now = g->total.frames;
    ImgSlot *old = 0;
    for (i32 i = 0; i < IMG_SLOTS; i++) {
        ImgSlot *sl = m->slots + i;
        if (imgis(sl->s, &sl->path, s, path)) {
            sl->used = now;
            if (sl->state==IMG_FAILED && sl->opens!=s->host->opens) {
                sl->state = IMG_WANT;  // its host is back: once more
                imgnext(g);
            }
            return sl;
        } else if (sl->state==IMG_FETCH || sl->used==now) {
            continue;
        } else if (!old || sl->state==IMG_EMPTY ||
                   (old->state!=IMG_EMPTY && sl->used<old->used)) {
            old = sl;
        }
    }
    if (!old || path.len>IMG_STRMAX) {
        return 0;
    }
    killtex(g, old->tex);
    *old = {};
    old->s     = s;
    old->state = imgis(m->fetch.s, &m->fetch.path, s, path) ? IMG_FETCH : IMG_WANT;
    old->used  = now;
    imgset(&old->path, path);
    imgnext(g);
    return old;
}

// Show a screenshot whole in the Screenshot window.
static void imgopen(Gui *g, GuiSession *s, Str path, i32 w, i32 h)
{
    ImgView *v = &g->img.view;
    v->open  = 1;
    v->focus = 1;
    scheddirty(&g->sched);
    if (imgis(v->s, &v->path, s, path) && v->state!=IMG_FAILED) {
        return;
    }
    killtex(g, v->tex);
    v->tex   = 0;
    v->s     = s;
    v->w     = w;
    v->h     = h;
    v->err   = {};
    v->state = path.len>IMG_STRMAX ? IMG_FAILED :
               imgis(g->img.fetch.s, &g->img.fetch.path, s, path) ? IMG_FETCH : IMG_WANT;
    imgset(&v->path, path);
    if (v->state == IMG_FAILED) {
        imgset(&v->err, "its path is too long");
    }
    imgnext(g);
}

// The Screenshot window closed: its image goes.
static void imgclose(Gui *g)
{
    ImgView *v = &g->img.view;
    killtex(g, v->tex);
    v->tex   = 0;
    v->s     = 0;
    v->open  = 0;
    v->state = IMG_EMPTY;
}

// A session's images go, as it closes here, or its host goes: its
// thumbnails, the Screenshot window's if it is one of them, and its
// fetch, whose reply, if it comes, is then ignored.
static void imgdrop(Gui *g, GuiSession *s)
{
    Images *m = &g->img;
    for (i32 i = 0; i < IMG_SLOTS; i++) {
        if (m->slots[i].s == s) {
            killtex(g, m->slots[i].tex);
            m->slots[i] = {};
        }
    }
    if (m->view.s == s) {
        imgclose(g);
    }
    if (m->fetch.s == s) {
        if (m->fetch.r) {
            os_release(g->os, m->fetch.r);
        }
        m->fetch = {};
        imgnext(g);
    }
}

// A new session in cwd, with the given model or the configured one,
// and a role ([role NAME]), if named, in plan mode if asked.
static void guinew(Gui *g, GuiHost *h, Str cwd, Str model, Str role = {}, b32 plan = 0)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 512 + cwd.len + model.len + role.len);
    EvOut w = cmdopen(g, &b, CMD_NEW, 0, h);
    putkey(&w.json, "cwd");
    putstr(&w.json, cwd);
    if (model.len) {
        putkey(&w.json, "model");
        putstr(&w.json, model);
    }
    if (role.len) {
        putkey(&w.json, "role");
        putstr(&w.json, role);
    }
    if (plan) {
        putkey(&w.json, "plan");
        putint(&w.json, 1);
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

static b32 hostcap(GuiHost *, Str);

// Ask a session's engine for its skills, what /name in the compose box
// may invoke: as it opens, and as each turn ends (onskills).
static void guiskills(Gui *g, GuiSession *s)
{
    if (!s || s->skillsid || !hostcap(s->host, "skills")) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_SKILLS, s);
    s->skillsid = g->nextid;
    cmdsend(g, &w, a);
}

// Invoke a skill in the current session, with what followed /name: as
// a prompt, or a steer while its turn runs. A failure puts the text
// back in the box (onsesscmd).
static void guiskill(Gui *g, GuiSession *s, Str skill, Str args, Str text)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + skill.len + args.len);
    EvOut w = cmdopen(g, &b, s->view->busy ? CMD_STEER : CMD_PROMPT, s);
    putkey(&w.json, "text");
    putstr(&w.json, args);
    putkey(&w.json, "skill");
    putstr(&w.json, skill);
    s->stick   = 1;
    s->cmdid   = g->nextid;
    s->cmdtext = clone(&g->perm, text);
    s->note    = {};
    cmdsend(g, &w, a);
}

// A skills reply: the list kept anew only when it changed.
static void onskills(Gui *g, GuiSession *s, Json *j, b32 ok)
{
    Json *list = jsonget(j, "skills");
    b32   same = jsonlen(list) == s->skills.len;
    iz    i    = 0;
    for (Json *k = list->child; same && k; k = k->next, i++) {
        SkillItem *t = &s->skills[i];
        same = t->name==jsonstr(jsonget(k, "name")) &&
               t->desc==jsonstr(jsonget(k, "description")) &&
               t->path==jsonstr(jsonget(k, "path")) &&
               t->hidden==(jsonget(k, "hidden")->boolean != 0);
    }
    if (!ok || same) {
        return;
    }
    s->skills = {};
    for (Json *k = list->child; k; k = k->next) {
        SkillItem t = {};
        t.name   = clone(&g->perm, jsonstr(jsonget(k, "name")));
        t.desc   = clone(&g->perm, jsonstr(jsonget(k, "description")));
        t.path   = clone(&g->perm, jsonstr(jsonget(k, "path")));
        t.hidden = jsonget(k, "hidden")->boolean != 0;
        s->skills = push(&g->perm, s->skills, t);
    }
    scheddirty(&g->sched);
}

// Withdraw a message queued for the session's turn (queue's id): its
// text comes back in the reply, for the draft (onreply).
static void guiunqueue(Gui *g, GuiSession *s, i64 item)
{
    if (!s || s->unqueue) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_UNQUEUE, s);
    putkey(&w.json, "item");
    putint(&w.json, item);
    cmdsend(g, &w, a);
    s->unqueue = item;
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

// The session operations the compose box's slash commands and the
// session menus run alike: the reply's outcome becomes the session's
// note above the box, and a failed command's text, kept in cmdtext by
// the box (a menu's has none), goes back in the box (onsesscmd).
static void guicompact(Gui *g, GuiSession *s, Str focus)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + focus.len);
    EvOut w = cmdopen(g, &b, CMD_COMPACT, s);
    if (focus.len) {
        putkey(&w.json, "focus");
        putstr(&w.json, focus);
    }
    s->cmdid   = g->nextid;
    s->cmdtext = {};
    s->note    = {};
    s->stick   = 1;  // to see it run
    cmdsend(g, &w, a);
}

static void guirename(Gui *g, GuiSession *s, Str title)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + title.len);
    EvOut w = cmdopen(g, &b, CMD_RENAME, s);
    putkey(&w.json, "title");
    putstr(&w.json, title);
    s->cmdid   = g->nextid;
    s->cmdtext = {};
    s->note    = {};
    cmdsend(g, &w, a);
    s->title = clone(&g->perm, title);  // until the list says
    scheddirty(&g->sched);
}

// Another model for the rest of the session: a name as the session's
// endpoint lists it, or ENDPOINT/MODEL.
static void guimodel(Gui *g, GuiSession *s, Str model)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + model.len);
    EvOut w = cmdopen(g, &b, CMD_MODEL, s);
    putkey(&w.json, "model");
    putstr(&w.json, model);
    s->cmdid   = g->nextid;
    s->cmdtext = {};
    s->note    = {};
    cmdsend(g, &w, a);
}

// Plan mode on or off: the agent hears of it at its next request.
static void guimode(Gui *g, GuiSession *s, b32 plan)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_MODE, s);
    putkey(&w.json, "mode");
    putstr(&w.json, plan ? Str("plan") : Str("default"));
    s->cmdid   = g->nextid;
    s->cmdtext = {};
    s->note    = {};
    cmdsend(g, &w, a);
}

// A new session from a copy of this one's log, whole (turn < 0) or cut
// after a turn, shown once made, with the prompt that came next, if
// any, as its draft (onsesscmd).
static void guifork(Gui *g, GuiSession *s, i64 turn)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_FORK, s);
    if (turn >= 0) {
        putkey(&w.json, "turn");
        putint(&w.json, turn);
    }
    s->cmdid   = g->nextid;
    s->cmdtext = {};
    s->note    = {};
    cmdsend(g, &w, a);
}

static void guianswer(Gui *g, GuiSession *s, Str req, Str decision, Str note)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + req.len + note.len);
    EvOut w = cmdopen(g, &b, CMD_ANSWER, s);
    putkey(&w.json, "req");
    putstr(&w.json, req);
    putkey(&w.json, "decision");
    putstr(&w.json, decision);
    if (note.len) {
        putkey(&w.json, "note");
        putstr(&w.json, note);
    }
    cmdsend(g, &w, a);
}

// Put back the files write and edit changed: from a call's on (call),
// or from a turn's on, overwriting conflicts if forced. A rewind record
// tells how each went.
static void guirewind(Gui *g, GuiSession *s, Str call, i32 turn, b32 force)
{
    if (!s || !s->view) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256 + call.len);
    EvOut w = cmdopen(g, &b, CMD_REWIND, s);
    putkey(&w.json, "to");
    if (call.len) {
        putstr(&w.json, call);
    } else {
        putint(&w.json, turn);
    }
    if (force) {
        putkey(&w.json, "force");
        putint(&w.json, 1);
    }
    g->rewindid = g->nextid;
    g->rewsent++;
    cmdsend(g, &w, a);
}

// Stop a background job; its end tells the agent.
static void guijobkill(Gui *g, GuiSession *s, i64 job)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_JOBKILL, s);
    putkey(&w.json, "job");
    putint(&w.json, job);
    cmdsend(g, &w, a);
}

// Whether a session is at work: a turn, or an ask.
static b32 sessbusy(GuiSession *s)
{
    return s->view && !s->readonly && (s->view->busy || s->view->asks);
}

// Sessions whose turns would be stopped by quitting: this process's
// engine's, while it is not a host.
static i32 running(Gui *g)
{
    if (g->enginegone || g->host) {
        return 0;  // nothing can run, or it runs on regardless
    }
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        n += s->host->kind==GH_ENGINE && sessbusy(s);
    }
    return n;
}

// Sessions at work elsewhere, which closing the window leaves running:
// on background hosts, this process's own included, and remote ones.
static i32 elsewhere(Gui *g)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        n += s->host->kind!=GH_ENGINE && s->host->up && sessbusy(s);
    }
    return n;
}

// Say goodbye to every host reached through the library: each forgets
// this client, and its sessions run on without it. Their records are
// let go until each confirms (MAIL_QUIT).
static void closehosts(Gui *g)
{
    for (GuiHost *h = g->hosts; h; h = h->next) {
        h->reconnect = 0;  // asked for before, and wanted no more
        if (h->r && !h->closing) {
            h->closing = 1;
            remoteclose(h->r);
        }
    }
}

// Whether quitting may end: this process's engine stopped, and every
// host said goodbye to.
static b32 quitdone(Gui *g)
{
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->r) {
            return 0;
        }
    }
    return g->host ? __atomic_load_n(&g->hostdone, __ATOMIC_ACQUIRE) : g->enginegone;
}

// Stop every turn of this process's engine, which kills their tools,
// leave the other hosts, then exit once all that is done, or after
// QUIT_GRACE regardless. Sessions on the other hosts run on.
static void draftsendall(Gui *);
static void editcancel(Gui *);

static void startquit(Gui *g)
{
    if (g->quitting) {
        return;
    }
    g->quitting = 1;
    g->quitby   = os_ticks(g->os) + QUIT_GRACE;
    draftsendall(g);  // before the hosts are left
    editcancel(g);
    closehosts(g);
    if (!g->enginegone && !g->host) {
        Arena a = newarena(g->cmdr);
        Buf   b(&a, 128);
        EvOut w = cmdopen(g, &b, CMD_QUIT, 0, g->hosts);
        putkey(&w.json, "mode");
        putstr(&w.json, "stop");
        cmdsend(g, &w, a);
    }
    g->quit = quitdone(g);
    scheddirty(&g->sched);
}


// Records from the engine

static void settheme(Gui *, i32);
static void setthinking(Gui *, i32);
static void settools(Gui *, i32);
static void setfontbase(Gui *, Str);

// Words of s separated by sep, trimmed, the empty ones dropped, joined
// by with: a paths setting as the settings window edits it, one a line.
static Str rejoin(Arena *a, Str s, u8 sep, Str with)
{
    Buf b(a, s.len + 16);
    for (Str rest = s; rest.len;) {
        Cut c    = cut(rest, sep);
        Str word = trim(c.head);
        rest = c.tail;
        if (word.len) {
            print(&b, b.len ? with : Str(""));
            print(&b, word);
        }
    }
    return finish(&b);
}

// The settings window's rows, from a config.get reply line, and the
// theme they set.
static void loadcfg(Gui *g, Str line)
{
    g->cfgmem.beg = g->cfgbase;
    Arena *a = &g->cfgmem;
    Json  *j = jsonparse(a, clone(a, line)).root;
    g->cfg      = j;
    g->cfgrows  = {};
    g->cfgheads = {};
    for (Json *h = jsonget(j, "headers")->child; h; h = h->next) {
        CfgHead d = {};
        d.kind = jsonstr(jsonget(h, "kind"));
        d.name = jsonstr(jsonget(h, "name"));
        d.file = jsonstr(jsonget(h, "file"));
        d.line = jsonint(jsonget(h, "line"), 0);
        d.own  = jsonint(jsonget(h, "own"), 0) != 0;
        g->cfgheads = push(a, g->cfgheads, d);
    }
    Json *schema = jsonget(j, "schema");
    for (Json *s = jsonget(j, "settings")->child; s; s = s->next) {
        CfgRow r = {};
        r.kind    = jsonstr(jsonget(s, "kind"));
        r.name    = jsonstr(jsonget(s, "name"));
        r.key     = jsonstr(jsonget(s, "key"));
        r.value   = jsonstr(jsonget(s, "value"));
        r.file    = jsonstr(jsonget(s, "file"));
        r.like    = jsonstr(jsonget(s, "like"));
        r.line    = jsonint(jsonget(s, "line"), 0);
        r.fetched = jsonint(jsonget(s, "fetched"), 0);
        r.isdflt  = jsonint(jsonget(s, "default"), 0) != 0;
        r.valid   = jsonint(jsonget(s, "valid"), 1) != 0;
        for (Json *k = schema->child; k; k = k->next) {
            // A key ending in a dot is a family, as template.NAME
            Str key    = jsonstr(jsonget(k, "key"));
            b32 family = key.len && key[key.len-1]=='.' && startswith(r.key, key);
            if (jsonstr(jsonget(k, "kind"))==r.kind && (key==r.key || family)) {
                r.type = jsonstr(jsonget(k, "type"));
                r.dflt = jsonstr(jsonget(k, "default"));
                r.desc = jsonstr(jsonget(k, "desc"));
                if (r.type == "enum") {
                    for (Json *c = jsonget(k, "choices")->child; c; c = c->next) {
                        r.choices = push(a, r.choices, jsonstr(c));
                    }
                }
            }
        }
        if (r.kind=="ui" && r.key=="theme") {
            settheme(g, themeparse(r.value));
        } else if (r.kind=="ui" && r.key=="thinking") {
            setthinking(g, thinkparse(r.value));
        } else if (r.kind=="ui" && r.key=="tools") {
            settools(g, toolsparse(r.value));
        } else if (r.kind=="ui" && r.key=="font_size") {
            setfontbase(g, r.value);
        } else if (r.kind=="ui" && r.key=="editor" && r.value!=g->editor) {
            g->editor = clone(&g->perm, r.value);
        }
        // Defaults, and what a model inherits by like, show as hints, not
        // values: an edit writes a value of its own. The field holds the
        // whole value, which an edit sends back whole, and paths one a
        // line.
        Str v = r.isdflt || r.like.len ? Str{} : r.value;
        if (r.type == "paths") {
            v = rejoin(a, v, ';', "\n");
        }
        r.cap = v.len + FIELD_MAX;
        r.buf = alloc<char>(a, r.cap);
        copybytes(r.buf, v.data, v.len);
        g->cfgrows = push(a, g->cfgrows, r);
    }
}

static void guiopen(Gui *, GuiSession *);
static void guiclose(Gui *, GuiSession *);

static void addgroup(Gui *, Str);

// A session moves to another host on this machine, which shares the
// state directory: one a background host holds, or one it let go. A
// view goes on from where it is, opened there. Not one open here for
// writing, which no other process can hold.
static void movesess(Gui *g, GuiSession *s, GuiHost *to)
{
    if (s->host==to || (s->view && !s->readonly && s->host->up)) {
        return;
    }
    if (s->view && s->host->up) {
        guiclose(g, s);  // read-only here: no more of it
    }
    s->host     = to;
    s->readonly = 0;
    s->locked   = 0;
    s->openid   = 0;
    if (s->view) {
        s->view->readonly = 0;
        guiopen(g, s);
    }
    scheddirty(&g->sched);
}

// The sessions a host lists. A background host of another process lists
// every session of the state directory it shares with the engine here,
// but only those it holds (not closed there) are its own, and they leave
// the engine's list, where they are held elsewhere; those it lets go
// come back to the engine.
static void onsessions(Gui *g, GuiHost *h, Json *j)
{
    b32 shared = h->kind==GH_LOCAL && !h->own && g->enginekind!=ENGINE_FAKE;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        s->listed &= s->host != h;
    }
    for (Json *e = jsonget(j, "list")->child; e; e = e->next) {
        Str sid   = jsonstr(jsonget(e, "sid"));
        Str state = jsonstr(jsonget(e, "state"));
        if (!sid.len || (shared && state=="closed")) {
            continue;
        }
        GuiSession *s = findsess(g, h, sid);
        if (!s && h->kind==GH_ENGINE) {
            GuiSession *o = findlocal(g, sid);
            if (o && o->host->kind==GH_LOCAL && !o->host->own) {
                continue;  // a background host's
            }
        } else if (!s && shared) {
            s = findsess(g, g->hosts, sid);
            if (s) {
                movesess(g, s, h);
                s = s->host==h ? s : 0;
            }
            if (!s && findlocal(g, sid)) {
                continue;  // open here for writing: not really held there
            }
        }
        s = s ? s : addsess(g, h, sid);
        Str title  = jsonstr(jsonget(e, "title"));
        Str cwd    = jsonstr(jsonget(e, "cwd"));
        Str model  = jsonstr(jsonget(e, "model"));
        Str parent = jsonstr(jsonget(e, "parent"));
        s->title  = s->title==title   ? s->title  : clone(&g->perm, title);
        s->cwd    = s->cwd==cwd       ? s->cwd    : clone(&g->perm, cwd);
        s->model  = s->model==model   ? s->model  : clone(&g->perm, model);
        s->parent = s->parent==parent ? s->parent : clone(&g->perm, parent);
        s->mtime  = jsonint(jsonget(e, "mtime"), 0);
        s->jobs   = jsonint(jsonget(e, "jobs"), 0);
        s->locked = jsonint(jsonget(e, "locked"), 0) != 0;
        s->state  = livestate(state);
        s->listed = 1;
        s->gone   = 0;  // listed, so there after all
        Str group = jsonstr(jsonget(e, "group"));
        s->group    = s->group==group ? s->group : clone(&g->perm, group);
        s->archived = jsonint(jsonget(e, "archived"), 0) != 0;
        if (!s->parent.len && !s->archived) {
            addgroup(g, group);
        }
    }
    if (shared) {
        // What it no longer holds is the engine's again
        b32 back = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            if (s->host==h && !s->listed && !s->openid) {
                movesess(g, s, g->hosts);
                back = 1;
            }
        }
        if (back) {
            guilist(g, g->hosts);
        }
    }
}

static void onfile(Gui *, Json *, b32 ok, Str err);
static Str  draftstr(GuiSession *);
static void draftput(Gui *, GuiSession *, Str);
static void setdraft(Gui *, GuiSession *, Str);
static void setnote(Gui *, GuiSession *, Str, b32);

// The reply to a session operation from the box (guicompact and the
// rest): its note, or its error with the command put back in the box,
// if it is still empty, to try again; a fork's new session is shown.
static void onsesscmd(Gui *g, GuiHost *h, Arena scratch, Request *q, EvRec *r)
{
    Json       *j    = r->root;
    b32         ok   = jsonint(jsonget(j, "ok"), 0) != 0;
    Str         err  = jsonstr(jsonget(j, "err"));
    Str         note = jsonstr(jsonget(j, "note"));
    GuiSession *s    = q->s;
    if (!s) {
        return;
    }
    b32 mine = s->cmdid == q->id;
    s->cmdid = mine ? 0 : s->cmdid;
    if (!ok) {
        setnote(g, s, err.len ? err : Str("the command failed"), 1);
        if (mine && s->cmdtext.len && !draftstr(s).len) {
            setdraft(g, s, s->cmdtext);
        }
        s->cmdtext = mine ? Str{} : s->cmdtext;
        return;
    }
    s->cmdtext = mine ? Str{} : s->cmdtext;
    switch (q->cmd) {
    case CMD_COMPACT:
        setnote(g, s, "compacting: summarizing the older messages", 0);
        break;
    case CMD_RENAME:
        setnote(g, s, "renamed", 0);
        guilist(g, h);
        break;
    case CMD_MODEL: {
        Str  model = jsonstr(jsonget(j, "model"));
        Buf  b(&scratch, 256 + model.len + note.len);
        print(&b, "switched model");
        if (model.len) {
            print(&b, " to ");
            print(&b, model);
        }
        if (note.len) {
            print(&b, ": ");
            print(&b, note);
        }
        setnote(g, s, finish(&b), 0);
        guilist(g, h);
    } break;
    case CMD_MODE:
        setnote(g, s, jsonstr(jsonget(j, "mode"))=="plan"
                      ? Str("plan mode: the agent changes nothing but its plan")
                      : Str("plan mode is off"), 0);
        break;
    case CMD_FORK: {
        // {ok, sid:<new>, from, turn, next?}
        Str sid = jsonstr(jsonget(j, "sid"));
        if (!validsid(sid) || sid==s->sid) {
            setnote(g, s, "forked", 0);
            guilist(g, h);
            break;
        }
        GuiSession *f = findsess(g, h, sid);
        f = f ? f : addsess(g, h, sid);
        guiselect(g, f);
        Str next = jsonstr(jsonget(j, "next"));
        if (next.len) {
            setdraft(g, f, next);
        }
        setnote(g, f, concat(&scratch, "forked from ", s->sid), 0);
        guilist(g, h);
    } break;
    }
}
static void onexportfile(Gui *, Arena, Json *, b32 ok, Str err);
static void onimport(Gui *, GuiHost *, Arena, Request *, EvRec *);
static void sessgone(Gui *, GuiSession *);
static void updatestatus(Gui *, GuiHost *, b32 idle);

static void onreply(Gui *g, GuiHost *h, Arena scratch, EvRec *r, Str line)
{
    Json   *j   = r->root;
    i64     id  = jsonint(jsonget(j, "id"), 0);
    b32     ok  = jsonint(jsonget(j, "ok"), 0) != 0;
    Str     err = jsonstr(jsonget(j, "err"));
    Request q   = takereq(g, id);  // ids are unique across hosts
    switch (q.cmd) {
    case CMD_HELLO:
        g->engineok = 1;
        h->up       = 1;
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
        GuiSession *s = findsess(g, h, r->sid);
        s = s ? s : addsess(g, h, r->sid);
        sessview(g, s);
        setcur(g, s);
        guilist(g, h);
        guiskills(g, s);
    } return;

    case CMD_SKILLS:
        if (q.s && q.s->skillsid==id) {
            q.s->skillsid = 0;
            onskills(g, q.s, j, ok);
        }
        return;

    case CMD_PROMPT:
    case CMD_STEER:
        if (q.s && q.s->cmdid==id) {
            onsesscmd(g, h, scratch, &q, r);  // a skill invoked (guiskill)
            return;
        }
        break;

    case CMD_FILE:
        if (id == g->out.id) {
            onfile(g, j, ok, err);
        } else if (id && id==g->plan.id) {
            onplanfile(g, j, ok, err);
        } else if (id == g->img.fetch.id) {
            onimgfile(g, scratch, j, ok, err);
        } else if (id && id==g->exp.id) {
            onexportfile(g, scratch, j, ok, err);
        }
        return;

    case CMD_ARCHIVE:
    case CMD_GROUP:
        // Listed again: as it now is, or back as it was
        h->relist = 1;
        break;

    case CMD_DELETE:
        // Every session it took, subagents included, even should it
        // fail part of the way
        g->deletes++;
        h->relist = 1;
        for (Json *k = jsonget(j, "deleted")->child; k; k = k->next) {
            GuiSession *s = findsess(g, h, jsonstr(k));
            if (s) {
                sessgone(g, s);
            }
        }
        if (!ok) {
            break;
        } else if (q.s && !jsonlen(jsonget(j, "deleted"))) {
            sessgone(g, q.s);  // a reply that lists none
        }
        return;

    case CMD_STATUS:
        if (q.h && q.h->updating==UPD_CHECKING) {
            updatestatus(g, q.h, ok && !jsonint(jsonget(j, "turns"), 0) &&
                                 !jsonint(jsonget(j, "jobs"), 0));
        }
        return;

    case CMD_OPEN: {
        GuiSession *s = q.s;
        if (!s || s->openid!=id) {
            return;
        }
        s->openid = 0;
        if (s->view) {
            // The engine here replays before it replies. A host replays
            // after, from the log, which may skip numbers (an import
            // leaves records out), and it brings whatever a client
            // misses itself, so that a gap through it is no loss
            s->view->loose = s->host->kind != GH_ENGINE;
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
        // The draft its engine kept, unless something was typed since
        Str draft = jsonstr(jsonget(j, "draft"));
        if (draft.len && draftloads(&s->dclock) && g->edsess!=s) {
            draftput(g, s, draft);
        }
        guiskills(g, s);
    } return;

    case CMD_COMPACT:
    case CMD_RENAME:
    case CMD_MODEL:
    case CMD_MODE:
    case CMD_FORK:
        onsesscmd(g, h, scratch, &q, r);
        return;

    case CMD_UPLOAD:
    case CMD_IMPORT:
        onimport(g, h, scratch, &q, r);
        return;

    case CMD_DRAFT:
        if (!ok && q.s) {
            setnote(g, q.s, concat(&scratch, "the draft was not kept: ", err), 1);
        }
        return;

    case CMD_UNQUEUE:
        // Withdrawn: back in the box, after what it holds, to edit
        if (q.s && ok) {
            Str text  = jsonstr(jsonget(j, "text"));
            Str draft = draftstr(q.s);
            setdraft(g, q.s, draft.len ? concat(&scratch, concat(&scratch, draft, "\n"), text)
                                       : text);
            g->promptfocus = q.s == g->sess;
        } else if (q.s) {
            setnote(g, q.s, concat(&scratch, "not withdrawn: ", err), 1);
        }
        if (q.s) {
            q.s->unqueue = 0;
        }
        return;

    case CMD_REWIND: {
        if (g->rewindid == id) {
            g->rewindid = 0;
        }
        // A rewind that changed nothing has no record: its note says so
        Str note = jsonstr(jsonget(j, "note"));
        if (!ok || !jsonlen(jsonget(j, "files"))) {
            g->lasterr = clone(&g->perm, ok ? note : err);
            guilog(g, g->lasterr);
        }
    } return;

    case CMD_JOBKILL:
        if (!ok) {
            GuiSession *s = q.s;
            for (iz i = 0; s && s->view && i<s->view->jobs.len; i++) {
                s->view->jobs[i].killing = 0;
            }
            g->lasterr = clone(&g->perm, err);
            guilog(g, g->lasterr);
        }
        return;

    case CMD_CONFIGGET:
        if (q.h && q.h->modelsid==id) {
            onhostmodels(g, q.h, scratch, j, ok, err);
            return;
        }
        if (g->cfgget == id) {
            g->cfgget = 0;
        }
        if (g->cfgrefresh == id) {
            g->cfgrefresh = 0;
            g->cfgrefreshed++;
            g->cfgstatus = ok ? Str("Includes fetched again") : clone(&g->perm, err);
            g->cfgok     = ok;
        }
        if (ok) {
            loadcfg(g, line);
        }
        return;

    case CMD_CONFIGSET: {
        if (g->cfgset == id) {
            g->cfgset = 0;
        }
        b32 del = g->cfgdelid == id;
        if (del) {
            g->cfgdelid = 0;
        }
        g->cfgsets++;
        g->cfgok = ok;
        Buf b(&scratch, 256);
        if (ok && del) {
            print(&b, "Deleted ");
            print(&b, g->cfgdelwhat);
            print(&b, " from ");
            print(&b, jsonstr(jsonget(j, "path")));
        } else if (ok) {
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

// The engine stopped by itself: no view of its sessions will hear of its
// turn ending, or of its asks, so none runs any more, and nothing waits
// for them.
static void enginestopped(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host->kind == GH_ENGINE) {
            s->state = LIVE_CLOSED;
            if (s->view) {
                viewstopped(s->view);
            }
        }
    }
    scheddirty(&g->sched);
}

static void onremote(Gui *, GuiHost *, Arena, EvRec *);

// Names from a hello (cmds or caps), kept anew only when they changed,
// as a host says hello again on every connection.
static void hostnames(Gui *g, Slice<Str> *names, Json *list)
{
    b32 same = jsonlen(list) == names->len;
    iz  i    = 0;
    for (Json *k = list->child; same && k; k = k->next, i++) {
        same = jsonstr(k) == (*names)[i];
    }
    if (!same) {
        *names = {};
        for (Json *k = list->child; k; k = k->next) {
            *names = push(&g->perm, *names, clone(&g->perm, jsonstr(k)));
        }
    }
}

// What a host's engine is, from its hello, or from a remote's "up",
// which passes that on.
static void hostcaps(Gui *g, GuiHost *h, Json *j)
{
    Str version = jsonstr(jsonget(j, "version"));
    h->version = h->version==version ? h->version : clone(&g->perm, version);
    hostnames(g, &h->cmds, jsonget(j, "cmds"));
    hostnames(g, &h->caps, jsonget(j, "caps"));
}

// Whether a host's engine takes a command, or has a feature, by name:
// an older engine may lack it.
static b32 hostcan(GuiHost *h, Str name)
{
    for (iz i = 0; i < h->cmds.len; i++) {
        if (h->cmds[i] == name) {
            return 1;
        }
    }
    for (iz i = 0; i < h->caps.len; i++) {
        if (h->caps[i] == name) {
            return 1;
        }
    }
    return 0;
}


// Drafts: each session's compose box, which its engine keeps (draft)
// once typing pauses, as the session is left, and as the GUI quits or
// goes to the background, and gives back in open's reply. A host whose
// engine is too old for drafts, or is down, keeps none: the box keeps
// its text for this run only.

// A session's draft, its buffer made on first use: reserved, and
// untouched until typed in, it costs no memory.
static char *draftbuf(Gui *g, GuiSession *s)
{
    if (!s->draft) {
        s->dregion = os_reserve(g->os, DRAFT_MAX + ((iz)1<<16));
        if (!s->dregion) {
            os_oom();
        }
        Arena a = newarena(s->dregion);
        s->draft = (char *)allocbytes(&a, DRAFT_MAX+1);
        s->draft[0] = 0;
    }
    return s->draft;
}

static Str draftstr(GuiSession *s)
{
    return s->draft ? Str{(u8 *)s->draft, (iz)__builtin_strlen(s->draft)} : Str{};
}

// Whether a session's host keeps its draft now: up, with an engine new
// enough.
static b32 draftkept(Gui *g, GuiSession *s)
{
    GuiHost *h = s->host;
    if (!h) {
        return 0;  // the empty view's
    } else if (h->kind == GH_ENGINE) {
        return !g->enginegone && !g->host && g->enginekind!=ENGINE_FAKE &&
               engineup(g) && hostcan(h, "draft");
    }
    return h->r && !h->closing && hostcan(h, "draft");
}

// Give the engine the draft now, if it changed since it last did. A
// host that cannot keep it leaves it here, waiting for no deadline.
static void draftsend(Gui *g, GuiSession *s)
{
    if (!s->dclock.dirty) {
        return;
    } else if (!draftkept(g, s)) {
        s->dclock.due = 0;
        return;
    }
    Str   text = draftstr(s);
    Arena a    = newarena(g->cmdr);
    Buf   b(&a, 256 + text.len + text.len/8);
    EvOut w = cmdopen(g, &b, CMD_DRAFT, s);
    putkey(&w.json, "text");
    putstr(&w.json, text);
    cmdsend(g, &w, a);
    draftdone(&s->dclock);
}

static void draftsendall(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        draftsend(g, s);
    }
}

// Between frames: drafts whose typing paused go out, and the next to
// come due asks for a frame then, which draws nothing new.
static void draftpoll(Gui *g, Sched *sc)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (draftdue(&s->dclock, sc->now)) {
            draftsend(g, s);
        }
        if (s->dclock.due) {
            schedat(sc, s->dclock.due, DUE_APP);
        }
    }
}

// Put text in a session's box, at most a draft's worth, cut at a code
// point. ImGui's state of the box, if it has it, takes it up, the caret
// at its end. Not sent: setdraft sends.
static void draftput(Gui *g, GuiSession *s, Str text)
{
    char *buf = draftbuf(g, s);
    iz    n   = text.len<DRAFT_MAX ? text.len : (iz)DRAFT_MAX;
    while (n>0 && n<text.len && (text[n]&0xc0)==0x80) n--;
    copybytes(buf, text.data, n);
    buf[n]  = 0;
    s->hist = 0;
    ImGuiInputTextState *st = ImGui::GetCurrentContext() ? ImGui::GetInputTextState(s->draftid) : 0;
    if (st) {
        st->ReloadUserBufAndMoveToEnd();
    }
    scheddirty(&g->sched);
}

// The text becomes a session's draft, as if typed, and its engine keeps
// it at once: for a fork, its next prompt; for the editor, the file.
static void setdraft(Gui *g, GuiSession *s, Str text)
{
    draftput(g, s, text);
    drafttyped(&s->dclock, os_ticks(g->os));
    draftsend(g, s);
}

// What the latest command from the box came to, shown above it until
// the next is typed.
static void setnote(Gui *g, GuiSession *s, Str note, b32 err)
{
    s->note    = clone(&g->perm, note);
    s->noteerr = err;
    scheddirty(&g->sched);
}


// The editor: the draft written to a file under the state directory,
// which is this user's own, then opened by [ui] editor, else $VISUAL,
// else $EDITOR, else macOS's default text editor, through the shell, as
// a tool runs: under a reaper (POSIX) or in a job (Windows), so that it
// goes if this process does. Its exit comes as a record, which wakes
// the GUI: with status 0 the file is the new draft; with any other, or
// cancelled, the draft stays as it was. One draft at a time is out,
// and its box is read-only meanwhile.

static Str editorcmd(Gui *g, Arena *a)
{
    return editorpick(g->editor, os_getenv(g->os, a, "VISUAL"),
                      os_getenv(g->os, a, "EDITOR"), g->mac);
}

static void editstart(Gui *g, GuiSession *s)
{
    if (g->edsess) {
        setnote(g, s, g->edsess==s ? Str("the draft is open in the editor already")
                                   : Str("the editor has another session's draft open"), 1);
        return;
    }
    Arena a   = g->frame;
    Str   cmd = editorcmd(g, &a);
    if (!cmd.len) {
        setnote(g, s, "no editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)", 1);
        return;
    }
    Str dir  = join(&a, g->statedir, "edit");
    Str base = concat(&a, "draft-", s->sid.len ? s->sid : Str("new"));
    Str path = join(&a, dir, concat(&a, base, ".md"));
    Str log  = join(&a, dir, concat(&a, base, ".log"));
    if (!os_mkdirs(g->os, a, dir) || !os_write(g->os, a, path, draftstr(s))) {
        setnote(g, s, concat(&a, "cannot write the draft for the editor: ", os_lasterr(g->os, &a)), 1);
        return;
    }
    Spawn sp = {};
    #ifdef _WIN32
    Str comspec = os_getenv(g->os, &a, "COMSPEC");
    comspec = comspec.len ? comspec : Str("cmd.exe");
    Str line = editorline(&a, cmd, path, 1);
    Buf c(&a, 64 + comspec.len + line.len);
    putbyte(&c, '"');
    print(&c, comspec);
    print(&c, "\" /d /s /c \"");
    print(&c, line);
    putbyte(&c, '"');
    sp.argv    = push(&a, sp.argv, comspec);
    sp.cmdline = finish(&c);
    #else
    sp.argv = push(&a, sp.argv, Str("/bin/sh"));
    sp.argv = push(&a, sp.argv, Str("-c"));
    sp.argv = push(&a, sp.argv, editorline(&a, cmd, path, 0));
    #endif
    sp.cwd      = dir;
    sp.env      = os_environ(g->os, &a);
    sp.outpath  = log;
    sp.maxbytes = 1<<16;
    sp.mbox     = g->edbox;
    sp.id       = ++g->edid;
    Proc *p = os_spawn(g->os, &sp, a);
    if (!p) {
        setnote(g, s, concat(&a, "cannot start the editor: ", os_lasterr(g->os, &a)), 1);
        os_remove(g->os, a, path);
        return;
    }
    g->edsess = s;
    g->edproc = p;
    g->edpath = clone(&g->perm, path);
    g->edlog  = clone(&g->perm, log);
    g->edname = clone(&g->perm, editorname(cmd));
    guilog(g, concat(&a, "editing a draft: ", cmd));
    s->hist   = 0;
    s->note   = {};
    scheddirty(&g->sched);
}

// The editor exited with a status, or was cancelled (status -1).
static void editend(Gui *g, i32 status)
{
    GuiSession *s = g->edsess;
    Arena       a = g->frame;
    if (status == 0) {
        Read rd = os_read(g->os, &a, g->edpath, DRAFT_MAX+2);
        Str  t  = editortext(rd.data);
        if (rd.err==READ_TOOBIG || t.len>DRAFT_MAX) {
            setnote(g, s, "the edited draft is too long, over 1 MiB: the draft is as it was", 1);
        } else if (rd.err != READ_OK) {
            setnote(g, s, "cannot read the edited draft: the draft is as it was", 1);
        } else {
            setdraft(g, s, t);
            s->note = {};
        }
    } else if (status > 0) {
        // Its output's last line says why, as a shell's does when it
        // finds no such command
        Read rd   = os_read(g->os, &a, g->edlog, 1<<16);
        Str  out  = trim(rd.data);
        iz   from = out.len;
        while (from>0 && out[from-1]!='\n') from--;
        Str  last = cuthead(out, from);
        last = takehead(last, last.len<200 ? last.len : 200);
        Buf  b(&a, 256 + last.len);
        print(&b, "the editor exited with status ");
        print(&b, (i64)status);
        print(&b, ": the draft is as it was");
        if (last.len) {
            print(&b, " (");
            print(&b, last);
            putbyte(&b, ')');
        }
        setnote(g, s, finish(&b), 1);
    }
    Buf lb(&a, 64);
    print(&lb, "the draft's editor ");
    print(&lb, status<0 ? Str("was cancelled") : Str("exited with status "));
    if (status >= 0) {
        print(&lb, (i64)status);
    }
    guilog(g, finish(&lb));
    os_remove(g->os, a, g->edpath);
    os_remove(g->os, a, g->edlog);
    os_procfree(g->os, g->edproc);
    g->edsess = 0;
    g->edproc = 0;
    scheddirty(&g->sched);
}

// Cancel the edit: the editor is killed, and its exit, when it comes,
// is ignored.
static void editcancel(Gui *g)
{
    if (g->edproc) {
        os_kill(g->os, g->edproc, 1);
        editend(g, -1);
    }
}

// The editor's records: only its exit matters.
static b32 drainedit(Gui *g)
{
    if (!g->edbox) {
        return 0;
    }
    b32  any = 0;
    Mail m   = mailtake(g->edbox);
    for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
        if (r.type==PROC_EXIT && g->edproc && r.id==g->edid) {
            i32 code = get32(r.data);
            i32 sig  = get32(cuthead(r.data, 4));
            editend(g, sig ? 128+sig : code<0 ? 255 : code);
            any = 1;
        }
    }
    return any;
}

// Whether a host's engine has a feature by name in its caps. Unlike
// hostcan, not by its cmds, which name every command of the protocol,
// even those an engine only refuses: an M8 operation works where caps
// name it (capnames).
static b32 hostcap(GuiHost *h, Str name)
{
    for (iz i = 0; i < h->caps.len; i++) {
        if (h->caps[i] == name) {
            return 1;
        }
    }
    return 0;
}

// Apply one record from a host. Returns 1 if what shows changed.
static b32 guirecord(Gui *g, GuiHost *h, Str line)
{
    Arena a  = g->frame;
    i64   t0 = g->histreq ? os_ticks(g->os) : 0;
    EvRec r  = evparse(&a, line);
    if (g->histreq) {
        g->histparseus += os_ticks(g->os) - t0;
    }
    if (!r.ok) {
        return 0;
    }
    switch (r.ev) {
    case EV_REPLY:
        onreply(g, h, a, &r, line);
        return 1;
    case EV_HELLO:
        hostcaps(g, h, r.root);
        if (h->kind == GH_ENGINE) {
            g->engineok = 1;
            h->up       = 1;
        }
        return 1;
    case EV_SESSIONS:
        onsessions(g, h, r.root);
        return 1;
    case EV_REMOTE:
        onremote(g, h, a, &r);
        return 1;
    case EV_OVERFLOW:
        if (!r.sid.len && h->kind==GH_ENGINE) {
            // The engine process lost records, replies among them
            g->overflows++;
            g->reissuing = 1;
            for (GuiSession *s = g->sessions; s; s = s->next) {
                if (s->view && s->host==h) s->view->lost = 1;
            }
            return 1;
        }
    }
    if (!r.sid.len) {
        return 0;
    }

    GuiSession *s = findsess(g, h, r.sid);
    if (!s || !s->view) {
        // A view begins with its session's first record: one made here,
        // or a subagent. Any record will do for a listed subagent of a
        // session viewed here, as its host would send it, should its
        // beginning have been lost (it resyncs). Other records, as of a
        // command on a session not open here, are not shown: an open
        // makes its view before it asks for any.
        GuiSession *p = s && s->parent.len ? findsess(g, h, s->parent) : 0;
        b32 first = r.ev==EV_SESSION && r.seq==1;
        if (!first && !(r.seq && p && p->view)) {
            return 0;
        }
        s = s ? s : addsess(g, h, r.sid);
        sessview(g, s);
        if (!g->sess && g->enginekind!=ENGINE_FAKE) {
            setcur(g, s);
        }
    }
    View *v     = s->view;
    i32   state = v->state;
    i32   asks  = v->asks;
    b32   busy  = v->busy;
    i32   jobs  = v->jobsrun;
    i32   qpos  = v->queuepos;
    if (r.seq && v->lost) {
        // The resync brings it, in order: applied now, in a replay's
        // loose order, it would move lastseq past the records lost
        return 0;
    }
    if (r.seq && s->openid && !v->lastseq && r.seq!=1) {
        // Written as its host opened it (instructions changed, as for an
        // imported session), ahead of the replay from its start, which
        // brings it too: applied now, in the replay's loose order, the
        // replay's records would seem applied already
        return 0;
    }
    if (r.ev==EV_TOOLOUT) {
        i32 i = findcall(v, jsonstr(jsonget(r.root, "call")));
        g->tailseen += i>=0 && v->msgs[i].tool->status==VTOOL_RUNNING;
    }
    b32 changed = viewrecord(v, a, &r);
    if (r.ev==EV_ASK && changed) {
        s->stick = 1;  // an ask needs seeing: scroll to it
    }
    if (r.ev==EV_TURNEND && s==g->sess) {
        guiskills(g, s);  // skills may have come or gone meanwhile
    }
    if (r.ev == EV_JOBOUT) {
        // Applied, so that the Jobs window is current once shown, but
        // drawn only while it shows, and a few times a second at most:
        // a job may print for hours with nobody watching (drainmail)
        g->jobsdirty |= changed && s==g->sess && g->jobsvisible;
        return 0;
    }
    if (s != g->sess) {
        // Only the sidebar shows it, and the rows of its parent's tasks
        return state!=v->state || asks!=v->asks || busy!=v->busy ||
               jobs!=v->jobsrun || qpos!=v->queuepos ||
               r.ev==EV_TITLE || r.ev==EV_SESSION || r.ev==EV_TOOLEND;
    }
    if (g->histreq && r.ev==EV_NOTE && find(line, "history loaded")>=0) {
        g->histdone = 1;
    }
    return changed;
}

// Records shown on content frames only: model output and tool output,
// and the liveness that comes with them, for the session shown.
static b32 heldback(Gui *g, GuiHost *h, Str line)
{
    if (!g->sess || g->sess->host!=h) {
        return 0;
    }
    // The event's name follows sid, seq and ts, near the start
    Str head = takehead(line, line.len<160 ? line.len : 160);
    iz  at   = find(head, "\"ev\":\"");
    if (at < 0) {
        return 0;
    }
    Str ev   = cuthead(head, at+6);
    b32 kind = startswith(ev, "delta\"") || startswith(ev, "tool.out\"") ||
               startswith(ev, "progress\"") || startswith(ev, "live\"");
    return kind && linesid(line)==g->sess->sid;
}

// Apply the held-back records, all of one host's.
static void flushdeltas(Gui *g)
{
    for (Str rest = finish(&g->pending); rest.len;) {
        Cut c = cut(rest, '\n');
        guirecord(g, g->pendhost, c.head);
        rest = c.tail;
    }
    g->pendmem.beg = g->pendbase;
    g->pending     = Buf(&g->pendmem, 1<<12);
}

// One record line from a host: held back, or applied after those held
// back before it. Returns 1 if what shows changed at once.
static b32 takeline(Gui *g, GuiHost *h, Str line)
{
    for (; line.len && (line[line.len-1]=='\n' || line[line.len-1]=='\r'); line.len--) {}
    if (!line.len) {
        return 0;
    }
    if (heldback(g, h, line)) {
        if (g->pendhost != h) {
            flushdeltas(g);  // another session's, shown before
            g->pendhost = h;
        }
        print(&g->pending, line);
        putbyte(&g->pending, '\n');
        return 0;
    }
    flushdeltas(g);
    return guirecord(g, h, line);
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
        other |= takeline(g, g->hosts, c.head);
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
// only provisional, and their commits are in the log. Reading starts
// where the last resync ended, as everything lost since lies beyond
// it, so that one with nothing to bring costs nothing.
static void resync(Gui *g, GuiSession *s)
{
    View  *v     = s->view;
    Arena  a     = g->frame;
    Arena  lines = newarena(g->replayr);
    Str    path  = join(&a, sessdir(g, &a, s->sid), "events.jsonl");
    Replay rp    = logreplayat(g->os, &lines, a, path, s->sid, v->lastseq,
                               v->logoff, LOG_MAX);
    if (rp.ok) {
        v->logoff = rp.end;
    }
    b32    loose = v->loose;
    v->loose = 1;
    for (iz i = 0; i < rp.lines.len; i++) {
        viewapply(v, a, rp.lines[i]);
    }
    v->loose = loose;
    v->lost  = 0;
    g->resyncs++;
}

// After lost records: the engine's commands whose replies may be gone,
// sent again where that is harmless. The library never loses records
// of the other hosts.
static void reissue(Gui *g)
{
    GuiHost *e = g->hosts;
    Request  old[REQ_MAX];
    for (i32 i = 0; i < REQ_MAX; i++) {
        old[i] = g->reqs[i];
        if (old[i].h == e) {
            g->reqs[i] = {};
        }
    }
    for (i32 i = 0; i < REQ_MAX; i++) {
        Request q = old[i];
        if (!q.id || q.h!=e) {
            continue;
        }
        switch (q.cmd) {
        case CMD_HELLO:     guihello(g);    break;
        case CMD_CONFIGGET: g->cfgget = 0; guiconfigget(g); break;
        case CMD_OPEN:      if (q.s && q.s->openid==q.id) guiopen(g, q.s); break;
        case CMD_NEW:       g->newid = 0; break;  // it shows in the list
        case CMD_FILE:
            if (g->out.id == q.id) {
                g->out.id = 0;
            } else if (g->img.fetch.id == q.id) {
                imgrequest(g);  // the same part again
            }
            break;
        }
    }
    // The list again in any case, with sessions begun in what was lost,
    // so that their subagents' records make views (guirecord)
    guilist(g, e);
}



// Hosts besides the engine: found, connected, lost
//
// Each has its own inbox, which the library fills without ever losing a
// record (it waits for room), and which is big enough for any record a
// link can carry: so these views never resync from a log, which may be
// on another machine anyway. A host gone or closed takes its sessions
// with it, except that a background host's go back to the engine, as
// they are in its state directory.

enum : iz {
    HOST_MAILCAP = FRAME_MAX + ((iz)1<<16),
};

// A host, made anew, or one gone before made over (its inbox kept: the
// library posts nothing more to it after MAIL_QUIT, and the inboxes
// are large).
static GuiHost *addhost(Gui *g, i32 kind, Str name)
{
    GuiHost *h = 0;
    if (kind!=GH_ENGINE && g->spare) {
        h = g->spare;
        g->spare = h->next;
        Mailbox *inbox = h->inbox;
        *h = {};
        h->inbox = inbox;
    } else {
        h = alloc<GuiHost>(&g->perm);
    }
    h->kind  = kind;
    h->name  = clone(&g->perm, name);
    h->state = RS_CLOSED;
    h->uid   = ++g->hostuids;
    if (kind!=GH_ENGINE && !h->inbox) {
        h->inbox = newmailbox(&g->perm, HOST_MAILCAP, wakegui, g->os);
    }
    GuiHost **p = &g->hosts;
    for (; *p; p = &(*p)->next) {}
    *p = h;
    return h;
}

static GuiHost *findhost(Gui *g, i32 kind, Str name)
{
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->kind==kind && h->name==name) {
            return h;
        }
    }
    return 0;
}

// Reach a host through the library. Sessions viewed here open again,
// from where their views are, once it answers hello. A host still
// closing is reached again once that ends (onhostend, hostagain).
static b32 hostconnect(Gui *g, GuiHost *h, RemoteConf *rc)
{
    if (h->r && h->closing) {
        h->reconnect = 1;
        scheddirty(&g->sched);
        return 1;
    } else if (h->r || !h->inbox) {
        return h->r != 0;
    }
    // Its own memory, as connections come and go (its reservations are
    // never let go: the library's thread may outlive its MAIL_QUIT)
    Arena   ra = newarena(os_reserve(g->os, (iz)1<<22));
    Remote *r  = ra.beg ? newremote(g->os, &ra, rc, h->inbox) : 0;
    if (!r || !remotego(r)) {
        h->state = RS_FAILED;
        h->msg   = "cannot start the connection";
        return 0;
    }
    h->r       = r;
    h->state   = RS_CONNECTING;
    h->msg     = {};
    h->up      = 0;
    h->closing = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host==h && s->view) {
            guiopen(g, s);
        }
    }
    scheddirty(&g->sched);
    return 1;
}

// A setting of a [remote NAME] section, from the latest config.get: not
// one ignored, as any is in an included file (config.cpp).
static Str remoteset(Gui *g, Str name, Str key)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind=="remote" && r->name==name && r->key==key && !r->isdflt &&
                r->valid) {
            return r->value;
        }
    }
    return {};
}

// Whether a [remote NAME] setting is written, but ignored.
static b32 remoteignored(Gui *g, Str name, Str key)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind=="remote" && r->name==name && r->key==key && !r->isdflt &&
                !r->valid) {
            return 1;
        }
    }
    return 0;
}

// Connect to a [remote NAME] over ssh, as its section says. The ssh
// program is "ssh", or PROSPERO_TEST_SSH (a test hook).
static GuiHost *remoteconnect(Gui *g, Arena scratch, Str name)
{
    GuiHost *h = findhost(g, GH_REMOTE, name);
    h = h ? h : addhost(g, GH_REMOTE, name);
    if (h->r && !h->closing) {
        return h;
    }
    RemoteConf rc = {};
    rc.name       = name;
    rc.ssh        = remoteset(g, name, "ssh");
    rc.path       = remoteset(g, name, "path");
    rc.state      = remoteset(g, name, "state");
    rc.rconfig    = remoteset(g, name, "config");
    rc.program    = os_getenv(g->os, &scratch, "PROSPERO_TEST_SSH");
    rc.localstate = g->statedir;
    rc.deploy     = remoteset(g, name, "deploy") == "auto";
    if (!rc.ssh.len && remoteignored(g, name, "ssh")) {
        h->state = RS_FAILED;
        h->msg   = "its ssh is set in an included file, which cannot choose "
                   "what runs where: set it in your own file";
        return h;
    } else if (!rc.ssh.len) {
        h->state = RS_FAILED;
        h->msg   = "no ssh destination: set ssh in its section";
        return h;
    }
    hostconnect(g, h, &rc);
    return h;
}

// Leave a host: it forgets this client, and its sessions run on.
static void hostclose(Gui *g, GuiHost *h)
{
    if (h->r && !h->closing) {
        h->closing = 1;
        remoteclose(h->r);
        scheddirty(&g->sched);
    }
}

// The background hosts of the state directory, as registered, but not
// this process's: each one reached at its endpoint. One that cannot be
// reached is gone (onhostgone), so that a stale registration costs a
// moment on the library's thread and never holds up the window.
static void findhosts(Gui *g, Arena a)
{
    i64 pid = os_pid(g->os);
    for (HostEntry *e = hostentries(g->os, &a, g->statedir); e; e = e->next) {
        if (e->pid==pid || !e->endpoint.len || e->proto!=HOST_PROTO) {
            continue;
        }
        Buf b(&a, 32);
        print(&b, e->pid);
        Str      name = finish(&b);
        GuiHost *h    = findhost(g, GH_LOCAL, name);
        if (h) {
            continue;
        }
        h = addhost(g, GH_LOCAL, name);
        h->pid      = e->pid;
        h->started  = e->started;
        h->endpoint = clone(&g->perm, e->endpoint);
        h->regpath  = clone(&g->perm, e->path);
        RemoteConf rc = {};
        rc.name       = "background";
        rc.endpoint   = e->endpoint;
        rc.localstate = g->statedir;
        hostconnect(g, h, &rc);
        g->hostfound++;
    }
}

// A host's connection changed: connecting, up, down until a retry,
// failed or closed, as the library says.
static void onremote(Gui *g, GuiHost *h, Arena a, EvRec *r)
{
    Str state = jsonstr(jsonget(r->root, "state"));
    Str msg   = jsonstr(jsonget(r->root, "msg"));
    i32 was   = h->state;
    for (i32 i = 0; i < countof(remotestates); i++) {
        h->state = remotestates[i]==state ? i : h->state;
    }
    h->msg     = h->msg==msg ? h->msg : clone(&g->perm, msg);
    h->retryat = r->ts + jsonint(jsonget(r->root, "retry_ms"), 0);
    h->up      = h->state == RS_UP;
    h->downs  += h->state==RS_DOWN && was!=RS_DOWN;
    if (h->up) {
        hostcaps(g, h, r->root);
    }
    if (h->up && was!=RS_UP) {
        h->opens++;
        h->modelsid  = 0;  // its configuration may differ now
        h->modelsgot = 0;
        if (h->updating == UPD_RESTART) {
            h->updating = UPD_DONE;
            h->updmsg   = clone(&g->perm, concat(&a, "Updated: the engine there is ", h->version));
        }
        guilist(g, h);
        ImgFetch *f = &g->img.fetch;
        if (f->s && f->s->host==h && f->id) {
            imgrequest(g);  // its reply may have gone with the link
        }
    }
    if (msg.len && h->state!=RS_UP) {
        Buf b(&a, 128 + msg.len);
        print(&b, h->kind==GH_REMOTE ? Str("remote ") : Str("background host "));
        print(&b, h->name);
        print(&b, ": ");
        print(&b, state);
        print(&b, ": ");
        print(&b, msg);
        guilog(g, finish(&b));
    }
}

// Connect again to a host reached before: a remote as its section says,
// this process's own host at its endpoint.
static void hostagain(Gui *g, GuiHost *h)
{
    if (h->kind == GH_REMOTE) {
        remoteconnect(g, g->frame, h->name);
        return;
    }
    RemoteConf rc = {};
    rc.name       = "background";
    rc.endpoint   = h->endpoint;
    rc.localstate = g->statedir;
    hostconnect(g, h, &rc);
}

// A host's connection ended (MAIL_QUIT). A background host's sessions
// go back to the engine, and the host goes; a remote one stays listed,
// to connect again, at once if asked meanwhile.
static void onhostend(Gui *g, GuiHost *h)
{
    b32 again = h->reconnect;
    h->r         = 0;
    h->up        = 0;
    h->closing   = 0;
    h->reconnect = 0;
    h->state     = h->state==RS_FAILED ? RS_FAILED : RS_CLOSED;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host == h) {
            imgdrop(g, s);  // closed with its host
        }
    }
    if (h->kind!=GH_LOCAL || h->own) {
        if (again) {
            hostagain(g, h);  // asked for while it was closing
        }
        scheddirty(&g->sched);
        return;
    }
    if (!h->opens && h->regpath.len) {
        os_remove(g->os, g->frame, h->regpath);  // stale: nobody there
    }
    b32 any = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host == h) {
            s->listed = 0;
            movesess(g, s, g->hosts);
            any = 1;
        }
    }
    for (GuiHost **p = &g->hosts; *p; p = &(*p)->next) {
        if (*p == h) {
            *p = h->next;
            break;
        }
    }
    h->next  = g->spare;
    g->spare = h;
    if (g->newhost == h) {
        g->newhost = 0;
    }
    for (i32 i = 0; i < countof(g->hostitems); i++) {
        g->hostitems[i] = g->hostitems[i]==h ? 0 : g->hostitems[i];
    }
    if (any) {
        guilist(g, g->hosts);
    }
    scheddirty(&g->sched);
}

// A host's name, as the window shows it.
static Str hostlabel(Gui *g, Arena *a, GuiHost *h)
{
    Buf b(a, 64 + h->name.len);
    switch (h->kind) {
    case GH_ENGINE:
        print(&b, "This computer");
        break;
    case GH_LOCAL:
        print(&b, h->own ? Str("This process, in the background") : Str("In the background, process "));
        if (!h->own) {
            print(&b, h->name);
        }
        break;
    case GH_REMOTE: {
        Str ssh = remoteset(g, h->name, "ssh");
        print(&b, h->name);
        if (ssh.len && ssh!=h->name) {
            print(&b, " (");
            print(&b, ssh);
            putbyte(&b, ')');
        }
    } break;
    }
    return finish(&b);
}

// Whether new sessions can go to a host now.
static b32 hostready(Gui *g, GuiHost *h)
{
    if (h->kind == GH_ENGINE) {
        return !g->host && g->engineok && !g->enginegone;
    }
    return h->up && !h->closing;
}

// How a host is, in a few words, with left the seconds until its next
// try when it is down.
static Str hoststatus(Gui *g, Arena *a, GuiHost *h, i64 left)
{
    if (h->kind!=GH_ENGINE && h->r && h->closing) {
        return h->reconnect ? Str("disconnecting, then connecting again")
                            : Str("disconnecting");
    }
    switch (h->kind==GH_ENGINE ? -1 : h->state) {
    case -1:            return g->enginegone ? Str("stopped") : Str("running");
    case RS_CONNECTING: return "connecting";
    case RS_UP:         return "connected";
    case RS_DOWN: {
        Buf b(a, 64);
        print(&b, "reconnecting in ");
        print(&b, left);
        print(&b, " s");
        return finish(&b);
    }
    case RS_DEPLOYING:  return "deploying an engine";
    case RS_FAILED:     return "failed";
    }
    return "not connected";
}



// Session operations (M8): what the sidebar's context menus and the
// Session menu do, each a command to the session's host. Each works only
// where the host's engine names it in its hello's caps (hostcap), which
// an older one lacks. What changes a session's place in the list (its
// title, group, archiving) lists that host again once answered (relist),
// after updating the session here at once, so that the sidebar moves it
// before the reply; a refusal's list puts it back.

static b32 hostcap(GuiHost *, Str);

// The session a subagent belongs to, at the top of its parents here.
static GuiSession *sessroot(Gui *g, GuiSession *s)
{
    for (i32 n = 0; n < 16; n++) {
        Str         p  = s->view && s->view->parent.len ? s->view->parent : s->parent;
        GuiSession *up = p.len ? findsess(g, s->host, p) : 0;
        if (!up || up==s) break;
        s = up;
    }
    return s;
}

// Sessions shown under a root, the root included.
static i32 sesscount(Gui *g, GuiSession *root)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        n += !s->gone && (s->view || s->listed) && s->host==root->host &&
             sessroot(g, s)==root;
    }
    return n;
}

static void guiarchive(Gui *g, GuiSession *s, b32 on)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_ARCHIVE, s);
    putkey(&w.json, "on");
    putint(&w.json, on ? 1 : 0);
    cmdsend(g, &w, a);
    s->archived = on;
    scheddirty(&g->sched);
}

// A group joins the order kept with the layout, last, unless known.
static void addgroup(Gui *g, Str name)
{
    b32 known = !name.len;
    for (iz i = 0; !known && i < g->groups.len; i++) {
        known = g->groups[i] == name;
    }
    if (!known) {
        g->groups    = push(&g->perm, g->groups, clone(&g->perm, name));
        g->sidedirty = 1;
    }
}

// Into a group by name, or out of any (empty name).
static void guigroup(Gui *g, GuiSession *s, Str name)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128 + name.len);
    EvOut w = cmdopen(g, &b, CMD_GROUP, s);
    putkey(&w.json, "name");
    if (name.len) {
        putstr(&w.json, name);
    } else {
        putnull(&w.json);
    }
    cmdsend(g, &w, a);
    s->group = name.len ? clone(&g->perm, name) : Str{};
    addgroup(g, name);
    scheddirty(&g->sched);
}

// A session and its subagents, for good.
static void guidelete(Gui *g, GuiSession *s)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_DELETE, s);
    cmdsend(g, &w, a);
}

// A session is gone from its host: from the sidebar, the transcript
// and whatever shows its files.
static void sessgone(Gui *g, GuiSession *s)
{
    s->gone   = 1;
    s->listed = 0;
    imgdrop(g, s);
    if (g->out.s == s) {
        g->out.s   = 0;
        g->outopen = 0;
    }
    if (g->plan.s == s) {
        g->plan.s = 0;
    }
    if (g->exp.s == s) {
        g->exp.s     = 0;
        g->exp.state = EXP_IDLE;
    }
    if (g->sess == s) {
        setcur(g, 0);
    }
    if (g->menusess == s) {
        g->menusess = 0;
    }
    scheddirty(&g->sched);
}

// What an export keeps until it is saved
enum : iz {
    EXPORT_MAX = sizeof(void *)==8 ? (iz)1<<30 : (iz)1<<27,
};

static void exportfail(Gui *g, Str why)
{
    Export *x = &g->exp;
    x->state   = EXP_IDLE;
    x->ok      = 0;
    x->status  = clone(&g->perm, why);
    g->lasterr = x->status;
    guilog(g, x->status);
}

// A file name from a session's title: letters, digits and a few
// others, the rest as dashes.
static Str exportname(Arena *a, GuiSession *s, Str ext)
{
    Str t = s->view && s->view->title.len ? s->view->title : s->title;
    Buf b(a, 64 + s->sid.len + ext.len);
    b32 dash = 0;
    for (iz i = 0; i<t.len && b.len<60; i++) {
        u8  c  = t[i];
        b32 ok = (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
                 c=='_' || c>=0x80;
        if (ok) {
            putbyte(&b, c);
            dash = 0;
        } else if (!dash && b.len) {
            putbyte(&b, '-');
            dash = 1;
        }
    }
    // No character cut short at the end, nor a dash
    iz end = b.len;
    for (; end>0 && (b.data[end-1]&0xc0)==0x80; end--) {}
    if (end>0 && b.data[end-1]>=0xc0) {
        i32 need = b.data[end-1]>=0xf0 ? 4 : b.data[end-1]>=0xe0 ? 3 : 2;
        b.len = b.len-(end-1)==need ? b.len : end-1;
    }
    for (; b.len && b.data[b.len-1]=='-'; b.len--) {}
    if (!b.len) {
        print(&b, s->sid);
    }
    print(&b, ext);
    return finish(&b);
}

static void exportwrite(Gui *g, Arena scratch, Str path);
static void exportrequest(Gui *g);

// What the export saves is ready: copied, or a place to save it asked
// for. The checklist saves in its directory without asking.
static void exportready(Gui *g)
{
    Export *x = &g->exp;
    Arena   a = g->frame;
    if (x->clip) {
        putbyte(&x->data, 0);  // for ImGui
        ImGui::SetClipboardText((char *)x->data.data);
        x->data.len--;
        x->state  = EXP_IDLE;
        x->ok     = 1;
        x->status = "Copied the Markdown";
        x->saves++;
        scheddirty(&g->sched);
        return;
    }
    Str name = exportname(&a, x->s, x->kind==EXPORT_MD ? Str(".md") : Str(".jsonl"));
    if (g->drive) {
        Str path = join(&a, g->drivecwd, name);
        exportwrite(g, a, path);
        return;
    }
    x->state = EXP_ASKING;
    x->ask++;
    Str title = x->kind==EXPORT_MD ? Str("Export as Markdown") : Str("Export the raw log");
    if (!os_guisave(g->os, a, title, name, g->tasks, TASK_SAVED, x->ask)) {
        exportfail(g, "cannot ask where to save it here");
    }
}

// Build what the export saves, once its session's view is loaded.
static void exportbuild(Gui *g)
{
    Export     *x = &g->exp;
    GuiSession *s = x->s;
    x->mem  = newarena(x->r);
    x->data = Buf(&x->mem, 1<<16);
    if (x->kind == EXPORT_MD) {
        viewexport(&x->data, s->view, s->sid, x->think, x->tools, x->output);
        exportready(g);
    } else if (s->host->kind != GH_REMOTE) {
        Arena a  = g->frame;
        Str   p  = join(&a, sessdir(g, &a, s->sid), "events.jsonl");
        Read  rd = os_read(g->os, &x->mem, p, EXPORT_MAX);
        if (rd.err != READ_OK) {
            exportfail(g, rd.err==READ_TOOBIG ? Str("the log is too big to export")
                                              : Str("cannot read the session's log"));
            return;
        }
        x->data      = Buf();
        x->data.data = rd.data.data;
        x->data.len  = rd.data.len;
        x->data.cap  = rd.data.len;
        x->data.a    = &x->mem;
        exportready(g);
    } else {
        x->state = EXP_FETCH;
        x->off   = 0;
        exportrequest(g);
    }
}

// Ask the host for the raw log's next part.
static void exportrequest(Gui *g)
{
    Export *x = &g->exp;
    Arena   a = newarena(g->cmdr);
    Buf     b(&a, 256);
    EvOut   w = cmdopen(g, &b, CMD_FILE, x->s);
    putkey(&w.json, "path");
    putstr(&w.json, "events.jsonl");
    putkey(&w.json, "off");
    putint(&w.json, x->off);
    putkey(&w.json, "encoding");
    putstr(&w.json, "base64");
    x->id = g->nextid;
    cmdsend(g, &w, a);
}

// A part of a remote session's raw log arrived.
static void onexportfile(Gui *g, Arena scratch, Json *j, b32 ok, Str err)
{
    Export *x = &g->exp;
    x->id = 0;
    if (x->state != EXP_FETCH) {
        return;
    } else if (!ok) {
        exportfail(g, err.len ? err : Str("cannot read the session's log"));
        return;
    }
    Unbase64 u = unbase64(&scratch, jsonstr(jsonget(j, "data")));
    if (!u.ok || jsonint(jsonget(j, "off"), -1)!=x->off+u.data.len) {
        exportfail(g, "a part of the log came garbled");
        return;
    } else if (x->data.len+u.data.len > EXPORT_MAX) {
        exportfail(g, "the log is too big to export");
        return;
    }
    print(&x->data, u.data);
    x->off += u.data.len;
    if (jsonint(jsonget(j, "more"), 0) && u.data.len) {
        exportrequest(g);
    } else {
        exportready(g);
    }
}

// Export a session: Markdown with the options set, or its raw log, saved
// or copied (clip). A session with no view here opens first. Begun
// before the next frame (exportstart), whose scratch it has to itself,
// rather than inside the menu that asked.
static void guiexport(Gui *g, GuiSession *s, i32 kind, b32 clip)
{
    Export *x = &g->exp;
    x->s    = s;
    x->kind = kind;
    x->clip = clip;
    x->want = 1;
    scheddirty(&g->sched);
}

static void exportstart(Gui *g)
{
    Export     *x    = &g->exp;
    GuiSession *s    = x->s;
    x->want = 0;
    if (!s || s->gone) {
        return;
    }
    if (!x->r) {
        x->r = os_reserve(g->os, 2*EXPORT_MAX);
        if (!x->r) {
            exportfail(g, "out of memory to export");
            return;
        }
    }
    x->id     = 0;
    x->status = {};
    if (!s->view || s->openid) {
        x->state = EXP_OPENING;
        if (!s->view) {
            guiopen(g, s);
        }
        return;
    }
    exportbuild(g);
}

// Between records: an export waiting for its session to open goes on
// once its replay is in.
static void exportpoll(Gui *g)
{
    Export *x = &g->exp;
    if (x->state==EXP_OPENING && x->s && x->s->view && !x->s->openid) {
        x->state = EXP_IDLE;
        exportbuild(g);
    }
}

// Write the export where the dialog said.
static void exportwrite(Gui *g, Arena scratch, Str path)
{
    Export *x = &g->exp;
    x->state = EXP_IDLE;
    if (!os_write(g->os, scratch, path, finish(&x->data))) {
        exportfail(g, concat(&scratch, "cannot write ", path));
        return;
    }
    x->ok     = 1;
    x->saves++;
    x->saved  = clone(&g->perm, path);
    x->status = clone(&g->perm, concat(&scratch, "Exported to ", path));
    scheddirty(&g->sched);
}

// Import session (IMP_*, above Gui)

static void importfail(Gui *g, Str why)
{
    Import *x = &g->imp;
    x->state   = IMP_IDLE;
    x->status  = clone(&g->perm, why);
    g->lasterr = x->status;
    guilog(g, x->status);
}

// Ask for a raw log to import to a host (the dialog's default if null).
// The checklist imports its latest export without asking.
static void importstart(Gui *g, GuiHost *h)
{
    Import *x = &g->imp;
    if (x->state != IMP_IDLE) {
        return;
    }
    g->newhost = h;
    x->status  = {};
    x->state   = IMP_ASKING;
    x->ask++;
    if (g->drive) {
        mailpost(g->tasks, TASK_OPENED, x->ask, g->exp.saved, 1);
    } else if (!os_guiopenfile(g->os, g->frame, "Import a session's raw log", g->tasks,
                               TASK_OPENED, x->ask)) {
        importfail(g, "cannot ask for a file here");
    }
}

// The log picked: read, whole, and the new session dialog opened for it.
static void importread(Gui *g, Str path)
{
    Import *x = &g->imp;
    x->state = IMP_IDLE;
    if (!x->r) {
        x->r = os_reserve(g->os, EXPORT_MAX + ((iz)1<<20));
        if (!x->r) {
            importfail(g, "out of memory to import");
            return;
        }
    }
    x->mem   = newarena(x->r);
    Read rd  = os_read(g->os, &x->mem, path, EXPORT_MAX);
    if (rd.err != READ_OK) {
        importfail(g, rd.err==READ_TOOBIG ? Str("the log is too big to import")
                                          : concat(&g->frame, "cannot read ", path));
        return;
    }
    x->data = rd.data;
    x->path = clone(&x->mem, path);

    // What its records say, of those the dialog shows
    Arena a = g->frame;
    Cut   c = cut(x->data, '\n');
    EvRec e = evparse(&a, c.head);
    if (!c.ok || e.ev!=EV_SESSION) {
        importfail(g, "not a session's raw log: it must begin with its session record");
        return;
    }
    x->cwd   = clone(&x->mem, jsonstr(jsonget(e.root, "cwd")));
    x->model = clone(&x->mem, jsonstr(jsonget(e.root, "model")));
    x->title = {};
    for (Str rest = c.tail; rest.len;) {
        Cut l = cut(rest, '\n');
        rest  = l.tail;
        if (find(l.head, "\"ev\":\"title\"")<0 && find(l.head, "\"ev\":\"model\"")<0) {
            continue;
        }
        Arena t = a;
        EvRec r = evparse(&t, l.head);
        if (r.ev == EV_TITLE) {
            x->title = clone(&x->mem, jsonstr(jsonget(r.root, "text")));
        } else if (r.ev == EV_MODEL) {
            x->model = clone(&x->mem, jsonstr(jsonget(r.root, "model")));
        }
    }
    x->state   = IMP_DIALOG;
    x->filled  = 0;
    g->newerr  = {};
    g->opennew = 1;
}

// Send the log's next part, or once all are in, the import.
static void importnext(Gui *g)
{
    Import *x = &g->imp;
    Arena   a = newarena(g->cmdr);
    if (x->off < x->data.len) {
        iz    n    = x->data.len - (iz)x->off;
        n          = n<(iz)UPLOAD_PART ? n : (iz)UPLOAD_PART;
        Str   data = base64(&a, Str{x->data.data+x->off, n});
        Buf   b(&a, 256 + data.len);
        EvOut w = cmdopen(g, &b, CMD_UPLOAD, 0, x->to);
        if (x->token.len) {
            putstrkey(&w.json, "token", x->token);
        }
        putstrkey(&w.json, "data", data);
        putintkey(&w.json, "off", x->off);
        if (x->off+n == x->data.len) {
            putintkey(&w.json, "done", 1);
        }
        x->state = IMP_UPLOADING;
        x->id    = g->nextid;
        cmdsend(g, &w, a);
        return;
    }
    Buf   b(&a, 256 + x->newcwd.len + x->newmodel.len);
    EvOut w = cmdopen(g, &b, CMD_IMPORT, 0, x->to);
    putstrkey(&w.json, "token", x->token);
    putstrkey(&w.json, "cwd", x->newcwd);
    if (x->newmodel.len) {
        putstrkey(&w.json, "model", x->newmodel);
    }
    x->state = IMP_IMPORTING;
    x->id    = g->nextid;
    cmdsend(g, &w, a);
}

// The dialog's Import: the log goes to the host, from its start.
static void importbegin(Gui *g, GuiHost *h, Str cwd, Str model)
{
    Import *x = &g->imp;
    x->to       = h;
    x->newcwd   = clone(&x->mem, cwd);
    x->newmodel = clone(&x->mem, model);
    x->token    = {};
    x->off      = 0;
    g->newerr   = {};
    importnext(g);
}

// A reply to the upload or the import in flight.
static void onimport(Gui *g, GuiHost *h, Arena scratch, Request *q, EvRec *r)
{
    Import *x   = &g->imp;
    Json   *j   = r->root;
    b32     ok  = jsonint(jsonget(j, "ok"), 0) != 0;
    Str     err = jsonstr(jsonget(j, "err"));
    if (q->id!=x->id || (x->state!=IMP_UPLOADING && x->state!=IMP_IMPORTING)) {
        return;
    }
    x->id = 0;
    if (!ok) {
        // As a new session's failure: the dialog again, with why
        x->state   = IMP_DIALOG;
        g->newhost = x->to;
        g->newerr  = clone(&g->perm, err.len ? err : Str("the import failed"));
        g->opennew = 1;
        return;
    }
    if (q->cmd == CMD_UPLOAD) {
        Str token = jsonstr(jsonget(j, "token"));
        i64 off   = jsonint(jsonget(j, "off"), -1);
        if (!token.len || off<=x->off || off>x->data.len) {
            importfail(g, "the host's answer to an upload came garbled");
            return;
        }
        x->token = x->token.len ? x->token : clone(&x->mem, token);
        x->off   = off;
        importnext(g);
        return;
    }
    x->state = IMP_IDLE;
    x->imports++;
    x->status = clone(&g->perm, concat(&scratch, "Imported ", x->path));
    if (!validsid(r->sid)) {
        guilist(g, h);
        return;
    }
    GuiSession *s = findsess(g, h, r->sid);
    s = s ? s : addsess(g, h, r->sid);
    guiselect(g, s);
    setnote(g, s, concat(&scratch, "imported from ", x->path), 0);
    guilist(g, h);
}

// Update engine's thread (UPD_*, above GuiHost)

struct GuiDeploy {
    Os         *os;
    Mailbox    *done;
    u64         uid;
    DeployConf  conf;
    Region     *perm;
    Region     *scratch;
};

static void guideploythread(void *arg)
{
    GuiDeploy *d       = (GuiDeploy *)arg;
    Arena      perm    = newarena(d->perm);
    Arena      scratch = newarena(d->scratch);
    Deployed   r       = deploy(d->os, &perm, scratch, &d->conf);
    Buf b(&perm, 16 + r.shown.len + r.err.len);
    putbyte(&b, (u8)r.ok);
    print(&b, r.ok ? r.shown : r.err);
    mailpost(d->done, TASK_DEPLOYED, d->uid, finish(&b), 1);
    os_release(d->os, d->scratch);
    os_release(d->os, d->perm);
}

static b32 updating(GuiHost *h)
{
    return h->updating==UPD_DEPLOYING || h->updating==UPD_CHECKING ||
           h->updating==UPD_RESTART;
}

// Deployed, and the host there asked whether anything runs: if not,
// it is stopped, to be reached again (onhostend), which starts the new
// engine; else it is left to finish, and the new engine waits.
static void updatestatus(Gui *g, GuiHost *h, b32 idle)
{
    if (!idle || !h->up || h->closing) {
        h->updating = UPD_DONE;
        h->updmsg   = "Deployed. Its host is at work, so it runs the old engine until it "
                      "exits: disconnect, then connect after its idle exit.";
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = cmdopen(g, &b, CMD_QUIT, 0, h);
    putkey(&w.json, "mode");
    putstr(&w.json, "stop");
    cmdsend(g, &w, a);
    h->updating  = UPD_RESTART;
    h->updmsg    = "deployed: starting the new engine there";
    h->reconnect = 1;  // once it ends
    scheddirty(&g->sched);
}

// What another thread finished: a save or open dialog answered, a
// deployment's progress and outcome, a launch that handed off to this
// window.
static void ontask(Gui *g, Record r)
{
    Export *x = &g->exp;
    switch (r.type) {
    case TASK_SAVED:
        if (x->state==EXP_ASKING && r.id==x->ask) {
            if (r.data.len) {
                exportwrite(g, g->frame, r.data);
            } else {
                x->state = EXP_IDLE;  // cancelled
            }
        }
        return;
    case TASK_OPENED:
        if (g->imp.state==IMP_ASKING && r.id==g->imp.ask) {
            if (r.data.len) {
                importread(g, r.data);
            } else {
                g->imp.state = IMP_IDLE;  // cancelled
            }
        }
        return;
    case TASK_DEPLOYED: {
        GuiHost *h = 0;
        for (GuiHost *k = g->hosts; k; k = k->next) {
            h = k->uid==r.id ? k : h;
        }
        if (!h || h->updating!=UPD_DEPLOYING || !r.data.len) {
            return;
        }
        Str msg = cuthead(r.data, 1);
        if (!r.data[0]) {
            h->updating = UPD_FAILED;
            h->updmsg   = clone(&g->perm, concat(&g->frame, "Not deployed: ", msg));
            guilog(g, concat(&g->frame, concat(&g->frame, h->name, ": "), h->updmsg));
            return;
        } else if (!h->up) {
            h->updating = UPD_DONE;
            h->updmsg   = clone(&g->perm, concat(&g->frame, msg, " deployed: connect to use it"));
            return;
        }
        Arena a = newarena(g->cmdr);
        Buf   b(&a, 64);
        EvOut w = cmdopen(g, &b, CMD_STATUS, 0, h);
        cmdsend(g, &w, a);
        h->updating = UPD_CHECKING;
        h->updmsg   = "deployed: is its host idle?";
    } return;
    case TASK_RAISE:
        g->raises++;
        g->raisereq   = 1;
        g->raisetoken = clone(&g->perm, r.data);
        scheddirty(&g->sched);
        return;
    case MAIL_OUT: {
        // A deployment's progress, by the remote's name
        Arena a = g->frame;
        EvRec e = evparse(&a, r.data);
        Str   n = jsonstr(jsonget(e.root, "host"));
        Str   m = jsonstr(jsonget(e.root, "msg"));
        GuiHost *h = findhost(g, GH_REMOTE, n);
        if (e.ev==EV_DEPLOY && h && h->updating==UPD_DEPLOYING && m.len) {
            h->updmsg = clone(&g->perm, m);
        }
    } return;
    }
}

static void guiupdate(Gui *g, GuiHost *h)
{
    if (h->kind!=GH_REMOTE || updating(h)) {
        return;
    }
    Str ssh = remoteset(g, h->name, "ssh");
    if (!ssh.len) {
        h->updating = UPD_FAILED;
        h->updmsg   = "no ssh destination: set ssh in its section";
        return;
    }
    GuiDeploy *d = alloc<GuiDeploy>(&g->perm);  // kept: the thread may outlive a frame
    d->os              = g->os;
    d->done            = g->tasks;
    d->uid             = h->uid;
    d->perm            = os_reserve(g->os, (iz)1<<24);
    d->scratch         = os_reserve(g->os, 3*DEPLOY_MAXSIZE);
    d->conf.name       = clone(&g->perm, h->name);
    d->conf.ssh        = clone(&g->perm, ssh);
    d->conf.program    = os_getenv(g->os, &g->perm, "PROSPERO_TEST_SSH");
    d->conf.localstate = g->statedir;
    d->conf.force      = 1;  // another build of this version, as a developer's
    d->conf.notes      = g->tasks;
    if (!d->perm || !d->scratch || !os_thread(g->os, guideploythread, d)) {
        if (d->perm) os_release(g->os, d->perm);
        if (d->scratch) os_release(g->os, d->scratch);
        h->updating = UPD_FAILED;
        h->updmsg   = "cannot start deploying: out of memory";
        return;
    }
    h->updating = UPD_DEPLOYING;
    h->updmsg   = "deploying the engine";
    scheddirty(&g->sched);
}


// Background hosting
//
// "Keep running in background" makes this process a host of its own
// engine (host.cpp), in place: the engine's records, which came to the
// GUI's inbox, go to a host thread from then on, which listens at the
// state directory's endpoint and is registered there, so that a window
// that comes later finds its sessions: this process's own, as a launch
// on the same state directory hands off to it (door.cpp), or a client
// from elsewhere (prospero attach, a remote's window).
// Nothing moves, as the engine stays where it was. The window and its
// renderer go, and a tray icon, where the system has one, offers the
// window again, a stop of every session's work, and quitting. Nobody
// attends the sessions from then on, so an ask is denied by policy at
// once, until a window opens them attended again. The host ends as
// "prospero serve" does, after [host] idle_exit with no client and
// nothing running, and this process with it. A window that comes back
// here is a client of the host like any other, through its endpoint, so
// that closing it leaves the host as it was.

static void saveini(Gui *, b32 last);
static void gui_close(Gui *);

static Str hostlockpath(Arena *a, Str statedir)
{
    Str run = join(a, statedir, "run");
    return concat(a, join(a, run, hostname(a, HOST_PROTO)), ".lock");
}

// Why this process cannot keep its sessions running in the background,
// or null if it can.
static Str cannothost(Gui *g)
{
    Arena a    = g->frame;
    Str   lock = hostlockpath(&a, g->statedir);  // before a goes as scratch
    if (g->host) {
        return {};
    } else if (g->enginekind != ENGINE_INPROC) {
        return "the engine runs as a process of its own (--engine)";
    } else if (!g->engine || g->enginegone) {
        return "the engine has stopped";
    } else if (os_lockheld(g->os, a, lock)) {
        return "another process already runs a background host for this "
               "state directory";
    }
    return {};
}

static void hostthread(void *arg)
{
    Gui *g = (Gui *)arg;
    hostrun(g->host);
    __atomic_store_n(&g->hostdone, 1, __ATOMIC_RELEASE);
    os_wake(g->os);
}

// The window goes, and the tray icon stays, if there is one. Never from
// inside a frame (shedlater).
static void shed(Gui *g)
{
    if (g->bg) {
        return;
    }
    saveini(g, 0);
    flushdeltas(g);
    g->bg     = 1;
    g->shedat = g->total.frames;
    schedminimize(&g->sched, 1);  // no frames, whatever is owed or busy
    // The tray first, for the window's going to hide the Dock's icon
    // too (macOS): the tray's icon is the way back
    if (!g->trayup) {
        g->trayup = os_trayopen(g->os, "Prospero: sessions running in the background");
    }
    os_guished(g->os);
    guilog(g, g->trayup ? Str("the window is gone; the tray icon brings it back")
                        : Str("the window is gone; there is no tray here to bring it back"));
}

// Keep this process's sessions running with no window: become a host,
// then go. Returns 0 if that cannot be, saying why in the log.
static b32 gobackground(Gui *g)
{
    Str why = cannothost(g);
    if (!why.len) {
        HostConf hc = {};
        hc.cfg      = g->config;
        hc.statedir = g->statedir;
        hc.e        = g->engine;
        hc.inbox    = g->inbox;
        hc.idle     = g->hostidle ? g->hostidle : hostticks(g->config, "idle_exit");
        hc.firstid  = g->nextid + (1<<20);
        hc.held     = -1;
        Arena hp = newarena(os_reserve(g->os, (iz)1<<30));
        g->host  = hp.beg ? newhost(g->os, &hp, g->frame, &hc, &why) : 0;
        why      = g->host ? Str{} : why.len ? why : Str("out of memory");
    }
    if (why.len) {
        g->hosterr = clone(&g->perm, concat(&g->frame, "cannot keep running: ", why));
        guilog(g, g->hosterr);
        return 0;
    }

    // What the host needs to know, and nobody attends the sessions now
    Host *h = g->host;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        View *v = s->view;
        if (s->host==g->hosts && v && !s->readonly) {
            hostseed(h, s->sid, v->parent, v->busy || v->asks);
            hostpoke(h, g->frame, s->sid, 0);
        }
    }
    mailnotify(g->inbox, 0, 0);  // the host's from now on
    if (!os_thread(g->os, hostthread, g)) {
        guilog(g, "cannot start the host's thread: quitting");
        g->quit = 1;
        return 0;
    }
    closehosts(g);  // the others forget this window
    Arena a = g->frame;
    Buf   b(&a, 128);
    print(&b, "running in the background as host ");
    print(&b, (i64)os_pid(g->os));
    print(&b, ", at ");
    print(&b, h->endpoint);
    guilog(g, finish(&b));
    shed(g);
    return 1;
}

// Bring the window back, in a process that hosts: a client of its own
// host, as another process's window would be, its views taking up from
// where they were.
static void reopen(Gui *g)
{
    if (!g->bg) {
        os_guiraise(g->os);
        return;
    }
    i64 frames = g->total.frames;  // before the opening draws any
    if (!os_guiopen(g->os, g, "Prospero")) {
        guilog(g, "cannot open a window");
        return;
    }
    g->bg          = 0;
    g->shedframes += frames - g->shedat;
    g->sched.pace  = os_guirefresh(g->os);
    schedminimize(&g->sched, 0);
    schedforce(&g->sched);

    Arena    a   = g->frame;
    Buf      b(&a, 32);
    print(&b, (i64)os_pid(g->os));
    GuiHost *own = findhost(g, GH_LOCAL, finish(&b));
    if (!own) {
        own = addhost(g, GH_LOCAL, finish(&b));
        own->own      = 1;
        own->pid      = os_pid(g->os);
        own->endpoint = clone(&g->perm, g->host->endpoint);
    }
    g->main = own;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->host == g->hosts) {
            s->host = own;  // the engine's, through the host now
        }
    }
    RemoteConf rc = {};
    rc.name       = "background";
    rc.endpoint   = own->endpoint;
    rc.localstate = g->statedir;
    hostconnect(g, own, &rc);
    guiconfigget(g);
    os_guiraise(g->os);  // in front, as asked for from the tray
    guilog(g, "the window is back");
}

// Ask for the window to go once the frame drawing now is done: to the
// background (bg set), or, in a process hosting already, just away.
static void shedlater(Gui *g, b32 bg)
{
    g->shedreq = bg ? 2 : 1;
    scheddirty(&g->sched);
}

// What shedlater asked for, between frames.
static void shednow(Gui *g)
{
    i32 req = g->shedreq;
    g->shedreq = 0;
    draftsendall(g);
    if (req == 2) {
        gobackground(g);
    } else if (req==1 && g->host) {
        closehosts(g);  // its own included: they forget this window
        shed(g);
    }
}

// A pick from the tray icon's menu (or the checklist's, as if).
static void gui_tray(Gui *g, i32 item)
{
    switch (item) {
    case TRAY_OPEN:
        if (g->host) {
            reopen(g);
        } else {
            os_guiraise(g->os);
        }
        break;
    case TRAY_STOPALL:
        if (g->host) {
            guilog(g, "stopping every session's work, as the tray asked");
            hoststop(g->host, 0);
        }
        break;
    case TRAY_QUIT:
        if (g->host && !g->quitting) {
            // The host stops its engine, as quit does (5 s for turns to
            // end), says goodbye (2 s), then this process ends
            guilog(g, "quitting, as the tray asked");
            g->quitting = 1;
            g->quitby   = os_ticks(g->os) + 2*QUIT_GRACE;
            hoststop(g->host, 1);
            closehosts(g);
        } else if (!g->host) {
            gui_close(g);
        }
        break;
    }
}

// Another launch on this state directory handed off to this window
// (door.cpp), between frames: it comes forward as from the tray's Open,
// a shed window opening with the launcher's activation token. A window
// up already gets only what os_guiraise can do, which under Wayland is
// an attention hint, as the token is good only for a window's showing.
static void raisenow(Gui *g)
{
    g->raisereq = 0;
    guilog(g, g->bg ? Str("another launch asked for the window: it comes back")
                    : Str("another launch asked for the window: raised"));
    if (g->bg) {
        os_guitoken(g->os, g->raisetoken);
    }
    gui_tray(g, TRAY_OPEN);
}

// This launch found the state directory's window lock held: ask its
// window to come forward instead, given half a second to be found (it
// may be starting), and exit. Returns the exit status.
static i32 handoff(Gui *g)
{
    Os   *os    = g->os;
    Arena a     = g->frame;
    Str   token = os_getenv(os, &a, "XDG_ACTIVATION_TOKEN");
    Knock k     = knock(os, &g->perm, a, g->statedir, token, 500, os_guiletfront);
    Buf   b(&a, 256 + g->statedir.len);
    if (k.ok) {
        print(&b, "prospero: a window has this state directory already, process ");
        print(&b, k.pid);
        print(&b, ": brought it forward\n");
        os_print(os, 2, finish(&b));
        return 0;
    }
    print(&b, "prospero: another Prospero window holds this state directory, "
              "and does not answer: ");
    print(&b, g->statedir);
    putbyte(&b, '\n');
    os_print(os, 2, finish(&b));
    return 1;
}

static void gui_close(Gui *);
static void setzoom(Gui *, f32);
static void zoomby(Gui *, i32);
static void setpane(Gui *, i32, b32);

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
        "all: prospero", "prospero: main.o", "\t$(CC) -o $@ $^ $(LDFLAGS)  ",
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

// Post n messages of history, and report the time spent making them,
// apart from the time spent waiting for room in the GUI's inbox.
static void fakehistory(Fake *f, i64 n)
{
    i64 t0   = os_ticks(f->os);
    i64 wait = 0;
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
        i64 t1 = os_ticks(f->os);
        fakesend(f, &w, 1);
        wait += os_ticks(f->os) - t1;
    }
    {
        Arena t = f->turnmem;
        Buf   b(&t, 128);
        print(&b, "prospero: fake: ");
        print(&b, n);
        print(&b, " messages made in ");
        printfloat(&b, (f64)(os_ticks(f->os) - t0 - wait)/1000, 1);
        print(&b, " ms, and ");
        printfloat(&b, (f64)wait/1000, 1);
        print(&b, " ms waiting for the GUI's inbox\n");
        os_print(f->os, 2, finish(&b));
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
    if (kind == INPUT_KEY) {
        schedkey(&g->sched, g->lastinput);  // a held key's repeats too
    }
    g->woke |= WOKE_INPUT;
    g->inputs[g->ninputs++ % countof(g->inputs)] = kind;
    if (kind==INPUT_KEY || kind==INPUT_TEXT || kind==INPUT_BUTTON) {
        // Typing or clicking into a field shows a blinking caret again.
        g->lastkey = os_ticks(g->os);
        ImGui::GetIO().ConfigInputTextCursorBlink = 1;
    }
}

static b32 drivewaitdone(Gui *g);

// Drain every record from the engine. Deltas and tool output for the
// session shown are held back for the next content frame, which shows
// them at most STREAM_PERIOD apart however many frames run between;
// anything else applies at once, after the records held back before it.
// Lost records come back from the logs.
static void drainmail(Gui *g, i64 now)
{
    g->woke |= WOKE_MAIL;
    if (g->holduntil > now) {
        g->held = 1;  // the checklist lets the inbox fill up
        return;
    }
    g->held = 0;

    // The engine's, while this process has it to itself
    b32 other   = 0;
    b32 stopped = 0;
    if (!g->host) {
        Mail m = mailtake(g->inbox);
        for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
            switch (r.type) {
            case MAIL_OUT:
                other |= takeline(g, g->hosts, r.data);
                break;
            case PROC_DATA:
                other |= procdata(g, r.data);
                break;
            case MAIL_QUIT:
            case PROC_EXIT:
                stopped |= !g->enginegone;
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
            g->reissuing = 1;
            for (GuiSession *s = g->sessions; s; s = s->next) {
                if (s->view && s->host==g->hosts) s->view->lost = 1;
            }
        }
    }

    other |= drainedit(g);

    // The other hosts': let go while closing, or in the background
    for (GuiHost *h = g->hosts, *next; h; h = next) {
        next = h->next;
        if (!h->inbox) {
            continue;
        }
        Mail m   = mailtake(h->inbox);
        b32  end = 0;
        for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
            if (r.type == MAIL_QUIT) {
                end = 1;
            } else if (r.type==MAIL_OUT && !h->closing && !g->bg) {
                other |= takeline(g, h, r.data);
            }
        }
        if (end) {
            flushdeltas(g);
            onhostend(g, h);
            other = 1;
        }
    }

    // Other threads' outcomes, while hosting too
    {
        Mail m = mailtake(g->tasks);
        for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
            ontask(g, r);
            other = 1;
        }
    }

    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->view && s->view->lost && s->host==g->hosts && g->enginekind!=ENGINE_FAKE) {
            flushdeltas(g);
            resync(g, s);
            other = 1;
        }
    }
    if (g->reissuing) {
        // After the resyncs, so that an open asks only for what is new
        g->reissuing = 0;
        reissue(g);
    }
    // Session operations answered: their hosts' lists, once each
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->relist) {
            h->relist = 0;
            guilist(g, h);
            other = 1;
        }
    }
    exportpoll(g);

    if (g->enginegone && !g->quitting && !g->engineerr.len) {
        g->engineerr = "the engine stopped";
    }
    if (g->quitting && quitdone(g)) {
        g->quit = 1;
    }
    if (g->enginegone && !g->quitting && (stopped || other)) {
        // Its last output may come after its exit, since Windows reads
        // the pipe and waits for the process on threads of their own,
        // and records held back for a content frame may be older still:
        // whatever they bring, nothing runs any more
        flushdeltas(g);
        enginestopped(g);
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
    if (g->jobsdirty) {
        if (now-g->lastjobs >= JOBS_PERIOD) {
            scheddirty(s);
        } else {
            schedat(s, g->lastjobs+JOBS_PERIOD, DUE_STREAM);
        }
    }
}

// The wake flag was cleared: drain the mail, timed, since its work
// happens outside any frame.
static void gui_wake(Gui *g)
{
    i64 now = os_ticks(g->os);
    drainmail(g, now);
    i64 us = os_ticks(g->os) - now;
    if (us > g->maxwakeus) {
        g->maxwakeus = us;
        g->unsnapped = 1;
    }
    if (g->histreq) {
        g->histwakeus += us;
        g->histwakes++;
    }
}

// The user asked to close the window, or to quit: at once with nothing
// running, which needs no frame (the window may be minimized, or quit
// from the Dock while covered). Otherwise the UI asks first (ui.cpp),
// in a window brought back for it.
//
// In a process that hosts, the window just goes, and the host runs on;
// with no window, as from a signal or a logout, everything stops as the
// tray's Quit stops it. Sessions at work on other hosts (remote, or in
// the background) run on whatever happens, and the dialog says so.
static void gui_close(Gui *g)
{
    if (g->bg) {
        gui_tray(g, TRAY_QUIT);
        return;
    } else if (g->host) {
        shedlater(g, 0);
        return;
    }
    if (!g->quitting && !running(g) && !elsewhere(g)) {
        startquit(g);
        return;
    }
    g->closereq = 1;
    if (!g->quitting) {
        os_guiraise(g->os);
        schedminimize(&g->sched, 0);  // to draw the dialog regardless
    }
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
    if (minimized && g->minclose) {
        g->minclose = 0;
        guilog(g, "drive: minimized, asking to close");
        GuiEvent e = {};
        e.kind = GEV_CLOSE;
        os_guiinject(g->os, &e);
    }
}

// The OS runs a modal loop (Win32 move, resize, menu), from which the
// platform draws on its own.
static void gui_modal(Gui *g, b32 inmodal)
{
    g->inmodal = inmodal;
}

// Text at its size: [ui] font_size, zoomed, at the display's scale, in
// ImGui's widgets and the transcript alike, whose layouts for the old
// size go. Never inside a frame, which would mix sizes (setzoom).
static void fontsize(Gui *g)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st.FontSizeBase  = g->fontbase;
    st.FontScaleMain = g->zoom;
    st.FontScaleDpi  = g->scale;
    g->fonts.size    = g->fontbase * g->zoom * g->scale;
    g->fontdirty     = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->view) s->view->topsdirty = 1;
    }
    g->nosession.view->topsdirty = 1;
    cachereset(g->vcache, 1);  // layouts for the old font size
    schedforce(&g->sched);
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
    fontsize(g);
}

// Zoom to a step (zoomsteps) for this run, clamped to the steps, from
// the next frame on.
static void setzoom(Gui *g, f32 zoom)
{
    f32 lo = zoomsteps[0], hi = zoomsteps[countof(zoomsteps)-1];
    zoom = zoom<lo ? lo : zoom>hi ? hi : zoom;
    if (zoom != g->zoom) {
        g->zoom      = zoom;
        g->fontdirty = 1;
        scheddirty(&g->sched);
    }
}

// Zoom in (steps > 0) or out by steps from the nearest step.
static void zoomby(Gui *g, i32 steps)
{
    i32 at = 0;
    for (i32 i = 1; i < countof(zoomsteps); i++) {
        f32 d0 = zoomsteps[at] - g->zoom, d1 = zoomsteps[i] - g->zoom;
        at = d1*d1 < d0*d0 ? i : at;
    }
    at += steps;
    at  = at<0 ? 0 : at>=countof(zoomsteps) ? countof(zoomsteps)-1 : at;
    setzoom(g, zoomsteps[at]);
}

// The permanent text size, [ui] font_size, in pixels at scale 1: from
// the configuration at startup and from each config.get. Out of range
// or unreadable is the default, 15.
static void setfontbase(Gui *g, Str value)
{
    Float f    = parsefloat(trim(value));
    f32   base = f.ok && f.val>=8 && f.val<=40 ? (f32)f.val : 15;
    if (base != g->fontbase) {
        g->fontbase  = base;
        g->fontdirty = 1;
        scheddirty(&g->sched);
    }
}

// The window was moved, resized, maximized or restored: its geometry is
// saved with the layout, a little later (ImGui's ini saving rate), by
// a frame of its own if nothing else draws one.
static void gui_placed(Gui *g, WinGeom geom)
{
    WinGeom *o = &g->geom;
    if (!geom.ok || (o->ok && o->max==geom.max && o->full==geom.full &&
                     o->box.x==geom.box.x && o->box.y==geom.box.y &&
                     o->box.w==geom.box.w && o->box.h==geom.box.h)) {
        return;
    }
    *o = geom;
    ImGuiContext &c = *GImGui;
    if (c.SettingsDirtyTimer <= 0) {
        ImGui::MarkIniSettingsDirty();
        Sched *s = &g->sched;
        schedat(s, os_ticks(g->os) + (i64)(c.IO.IniSavingRate*1e6f) + 1000, DUE_SETTINGS);
    }
}

// The system's theme preference, as the window opens and whenever it
// changes. The frame is set each time, so that a new window's is too.
static void gui_systheme(Gui *g, b32 dark)
{
    if (g->syshas && g->sysdark!=dark) {
        guilog(g, dark ? Str("the system's theme changed to dark")
                       : Str("the system's theme changed to light"));
    }
    g->syshas  = 1;
    g->sysdark = dark;
    applytheme(g);
    g->titleok = os_guitheme(g->os, g->pal->dark);
}

// Frames

static Str statepath(Gui *g, Str name)
{
    return join(&g->perm, g->statedir, name);
}

static void savermain(void *arg)
{
    Saver *s = (Saver *)arg;
    for (;;) {
        mailwait(s->in, -1);
        Mail   m    = mailtake(s->in);
        Record save = {};
        b32    last = 0;
        for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
            save  = r;
            last |= r.type == SAVE_LAST;
        }
        if (save.ok && !os_write(s->os, s->scratch, s->path, save.data)) {
            os_print(s->os, 2, "prospero: could not save the layout\n");
        }
        if (last) {
            os_signal(&s->done);
            return;
        }
    }
}

// A writer for the layout file, or null if it cannot start.
static Saver *newsaver(Os *os, Arena *perm, Str path)
{
    Saver *s = alloc<Saver>(perm);
    s->os      = os;
    s->path    = path;
    s->in      = newmailbox(perm, 1<<20, 0, 0);
    s->scratch = newarena(os_reserve(os, 64<<20));
    if (!s->in || !s->scratch.beg || !os_eventinit(&s->done) ||
            !os_thread(os, savermain, s)) {
        return 0;
    }
    return s;
}

// Save the layout: through the saver, or here without one, or when it
// is too big for the saver's mailbox. As the GUI closes (last), wait
// for it to be written.
static void saveini(Gui *g, b32 last)
{
    if (!g->inipath.len) {
        return;
    }
    uz   len = 0;
    char const *ini = ImGui::SaveIniSettingsToMemory(&len);
    Str  data = {(u8 *)ini, (iz)len};
    Saver *s  = g->saver;
    if (s && mailpost(s->in, last ? SAVE_LAST : SAVE_LAYOUT, 0, data, 1)) {
        if (last && !os_wait(&s->done, 5000)) {
            guilog(g, "the layout is not saved yet");
        }
    } else if (!os_write(g->os, g->frame, g->inipath, data)) {
        guilog(g, "could not save the layout");
    }
    g->lastsave = os_ticks(g->os);
    g->idlesaves += g->lastsave-g->lastinput >= 1000000;
    g->saves++;
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

// The window's own settings in the layout file, beside ImGui's, read
// before the window opens: which panes show, and where the window is.
//
//   [Prospero][Panes]
//   Metrics=0
//   [Prospero][Window]
//   Pos=120,80
//   Size=1280,800
//   Maximized=0
//   Fullscreen=0
//   [Prospero][Sidebar]
//   Group=Work
//   Folded=g:Work
//   Opened=a
//
// A pane missing (a layout older than it) keeps its default, and the
// window's place is fitted to the displays as they are (place.cpp). The
// sidebar keeps its groups' order, empty ones included, and each fold
// the user chose by its key (sidebar.cpp), folded or opened.

static Str inistr(char const *s)
{
    return {(u8 *)s, s ? (iz)__builtin_strlen(s) : 0};
}

static void *iniopen(ImGuiContext *, ImGuiSettingsHandler *, char const *name)
{
    Str n = inistr(name);
    return n=="Panes" ? (void *)1 : n=="Window" ? (void *)2 : n=="Sidebar" ? (void *)3 : 0;
}

static void iniline(ImGuiContext *, ImGuiSettingsHandler *h, void *entry, char const *line)
{
    Gui *g  = (Gui *)h->UserData;
    Cut  kv = cut(inistr(line), '=');
    if (!kv.ok) {
        return;
    }
    Int v = parseint(kv.tail);
    if (entry == (void *)3) {
        if (kv.head=="Group" && kv.tail.len) {
            addgroup(g, kv.tail);
            g->sidedirty = 0;  // as the file has it
        } else if ((kv.head=="Folded" || kv.head=="Opened") && kv.tail.len) {
            *upsert(&g->folds, clone(&g->perm, kv.tail), &g->perm) = kv.head == "Folded";
        }
        return;
    } else if (entry == (void *)1) {
        for (i32 i = 0; i < PANE_COUNT; i++) {
            if (v.ok && kv.head==inistr(panenames[i])) {
                g->panes[i] = v.val != 0;
            }
        }
        return;
    }
    Cut xy = cut(kv.tail, ',');
    Int x  = parseint(xy.head);
    Int y  = parseint(xy.tail);
    b32 ok = xy.ok && x.ok && y.ok && x.val>-(1<<20) && x.val<1<<20 &&
             y.val>-(1<<20) && y.val<1<<20;
    WinGeom *w = &g->geom;
    if (kv.head=="Pos" && ok) {
        w->box.x = (i32)x.val;
        w->box.y = (i32)y.val;
    } else if (kv.head=="Size" && ok && x.val>0 && y.val>0) {
        w->box.w = (i32)x.val;
        w->box.h = (i32)y.val;
        w->ok    = 1;
    } else if (kv.head=="Maximized" && v.ok) {
        w->max = v.val != 0;
    } else if (kv.head=="Fullscreen" && v.ok) {
        w->full = v.val != 0;
    }
}

static void inifolds(ImGuiTextBuffer *out, Map<b32> *m)
{
    if (!m) {
        return;
    }
    out->append(m->val ? "Folded=" : "Opened=");
    out->append((char *)m->key.data, (char *)m->key.data + m->key.len);
    out->append("\n");
    for (i32 i = 0; i < 4; i++) {
        inifolds(out, m->child[i]);
    }
}

static void iniwrite(ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out)
{
    Gui *g = (Gui *)h->UserData;
    out->append("[Prospero][Panes]\n");
    for (i32 i = 0; i < PANE_COUNT; i++) {
        out->appendf("%s=%d\n", panenames[i], g->panes[i] ? 1 : 0);
    }
    out->append("\n");
    WinGeom *w = &g->geom;
    if (w->ok) {
        out->append("[Prospero][Window]\n");
        out->appendf("Pos=%d,%d\n", (int)w->box.x, (int)w->box.y);
        out->appendf("Size=%d,%d\n", (int)w->box.w, (int)w->box.h);
        out->appendf("Maximized=%d\n", w->max ? 1 : 0);
        out->appendf("Fullscreen=%d\n\n", w->full ? 1 : 0);
    }
    if (g->groups.len || g->folds) {
        out->append("[Prospero][Sidebar]\n");
        for (iz i = 0; i < g->groups.len; i++) {
            Str n = g->groups[i];
            out->append("Group=");
            out->append((char *)n.data, (char *)n.data + n.len);
            out->append("\n");
        }
        inifolds(out, g->folds);
        out->append("\n");
    }
}

// Read and write the window's settings with the layout file's.
static void addinihandler(Gui *g)
{
    ImGuiSettingsHandler h;
    h.TypeName   = "Prospero";
    h.TypeHash   = ImHashStr("Prospero");
    h.ReadOpenFn = iniopen;
    h.ReadLineFn = iniline;
    h.WriteAllFn = iniwrite;
    h.UserData   = g;
    ImGui::AddSettingsHandler(&h);
}

// Show or hide a pane, kept with the layout. One shown is brought to
// the front, its tab selected.
static void setpane(Gui *g, i32 pane, b32 show)
{
    if (g->panes[pane] != show) {
        g->panes[pane] = show;
        ImGui::MarkIniSettingsDirty();
        scheddirty(&g->sched);
    }
    if (show) {
        g->panefocus = pane + 1;
    }
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
    m->maxwakeus  = g->maxwakeus;
    m->layoutus   = g->layoutus;
    m->deadline   = g->lastdeadline==I64MAX ? I64MAX : g->lastdeadline - s->now;
    m->duewhy     = g->lastduewhy;
    m->owed       = s->owed;
    m->changing   = s->changing;
    m->busy       = s->busy;
    m->at         = s->now;
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
    if (g->exp.want) {
        exportstart(g);  // asked for in the last frame
    }
    g->episode  |= (g->cause & (WOKE_INPUT|WOKE_MAIL|WOKE_FORCE|WOKE_DEADLINE)) != 0;
    g->cause     = 0;
    if (g->unsnapped && s->now>=g->nextsnap) {
        snapshot(g);
        g->unsnapped = 0;
        g->nextsnap = s->now - (s->now - g->second)%1000000 + 1000000;
    }
    g->fonts.gen++;
    g->layoutus = 0;
    g->thumbsdrawn = 0;
    if (g->fontdirty) {
        fontsize(g);  // zoomed, or [ui] font_size changed
    }
    cachereset(g->vcache, 0);
    if (g->held && s->now>=g->holduntil) {
        gui_wake(g);  // the hold is over: take what piled up
    }
    if (g->pending.len && s->now-g->lastcontent>=STREAM_PERIOD) {
        flushdeltas(g);
        g->lastcontent = s->now;
    }
    if (g->jobsdirty) {
        g->jobsdirty = 0;  // this frame shows it
        g->lastjobs  = s->now;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (s->now-g->lastkey >= CARET_STOP) {
        io.ConfigInputTextCursorBlink = 0;
    }

    // Where the time goes, for the benchmark's slow frames: glyphs new to
    // the atlas are rasterized as the UI asks for them, and a full atlas
    // grows, repacked
    ImFontAtlas *atlas  = io.Fonts;
    i32          packed = atlas->Builder ? atlas->Builder->RectsPackedCount : 0;
    i64          laid   = g->view->laidout;
    g->benchtex0 = atlas->TexData ? atlas->TexData->Width : 0;

    imgreap(g);  // before this frame lists the textures
    os_guinewframe(g->os);
    if (g->drive && g->drivemoved) {
        // The backend reads the real mouse when the window has focus and
        // the mouse is elsewhere: the script's mouse is the one
        io.AddMousePosEvent(g->drivex, g->drivey);
    }
    ImGui::NewFrame();
    i64 t1 = os_ticks(g->os);
    dockwindows(g);
    i64 t2 = os_ticks(g->os);
    ImGui::Render();
    i64 t3 = os_ticks(g->os);
    g->benchseg[0]  = t1 - t0;
    g->benchseg[1]  = t2 - t1;
    g->benchseg[2]  = t3 - t2;
    g->benchlaid1   = g->view->laidout - laid;
    g->benchglyphs  = (atlas->Builder ? atlas->Builder->RectsPackedCount : 0) - packed;
    g->benchtex     = atlas->TexData ? atlas->TexData->Width : 0;
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
    if (g->img.dying) {
        busy |= BUSY_TEX;
    }
    draftpoll(g, s);
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
    g->benchsave = 0;
    if (io.WantSaveIniSettings) {
        i64 t = os_ticks(g->os);
        saveini(g, 0);
        g->benchsave = os_ticks(g->os) - t;
    }

    ImDrawData *dd   = ImGui::GetDrawData();
    u64         hash = drawhash(dd, g->lists, &g->nlists, countof(g->lists), &g->header);
    g->lastdeadline = s->deadline;
    g->lastduewhy   = s->duewhy;
    Frame f = schedend(s, hash, busy);
    if (f.keyup) {
        Arena a = g->frame;
        Buf   b(&a, 96);
        print(&b, "no key event for ");
        print(&b, (s->now - s->keyat)/1000);
        print(&b, " ms with a key held: its release was lost, let go of ");
        print(&b, (i64)uikeysup());
        guilog(g, finish(&b));
    }
    if (g->trace) {
        traceframe(g, f, busy);
    }
    i64 cpu = os_ticks(g->os) - t0;
    os_guipresent(g->os, f.present, g->pal->clear);

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
    if (g->histreq) {
        g->histframeus += cpu;
        g->histframes++;
    }
    if (g->histdone) {
        // The fake agent makes the history on its own thread meanwhile,
        // and reports that time itself: the GUI's share is its own
        Arena a = g->frame;
        Buf   b(&a, 256);
        print(&b, "history: ");
        print(&b, (i64)g->view->msgs.len);
        print(&b, " messages, first frame ");
        printfloat(&b, (f64)(os_ticks(g->os) - g->histreq)/1000, 1);
        print(&b, " ms after the request; the GUI's own work ");
        printfloat(&b, (f64)(g->histwakeus + g->histframeus)/1000, 1);
        print(&b, " ms: applying the records ");
        printfloat(&b, (f64)g->histwakeus/1000, 1);
        print(&b, " ms (parsing ");
        printfloat(&b, (f64)g->histparseus/1000, 1);
        print(&b, " ms) in ");
        print(&b, g->histwakes);
        print(&b, " wakes, ");
        print(&b, g->histframes);
        print(&b, " frames ");
        printfloat(&b, (f64)g->histframeus/1000, 1);
        print(&b, " ms, this one ");
        printfloat(&b, (f64)cpu/1000, 1);
        print(&b, " ms");
        guilog(g, finish(&b));
        g->histdone = 0;
        g->histreq  = 0;
    }

    // The overlay's numbers change at most once a second, at the first
    // frame past a second boundary, or by a deadline then if idle. The
    // frames that only show new numbers are not work to report. Frames
    // that run on are, each second as they go, lest the overlay stand
    // still exactly while they run (a reason to go on that never ends).
    g->unsnapped |= g->episode;
    if (!s->owed && !s->busy) {
        g->episode = 0;
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

// With no window: wait for the tray, a wake, or the checklist's next
// step, which runs without frames.
static void bgwait(Gui *g)
{
    Sched *s = &g->sched;
    s->now = os_ticks(g->os);
    i64 t = -1;
    if (g->drive && g->driveat!=I64MAX) {
        t = g->driveat>s->now ? g->driveat-s->now : 0;
        if (g->drivewait && t>200000) {
            t = 200000;  // nothing wakes a wait without frames: look again
        }
    }
    if (g->quitting) {
        i64 left = g->quitby>s->now ? g->quitby-s->now : 0;
        t = t<0 || t>left ? left : t;
    }
    if (!os_guiwait(g->os, t)) {
        g->quit = 1;
        return;
    }
    s->now = os_ticks(g->os);
    if (g->quitting && s->now>=g->quitby) {
        g->quit = 1;  // the host did not stop in time
        return;
    }
    while (g->bg && g->drive && !g->quit &&
           (s->now>=g->driveat || (g->drivewait && drivewaitdone(g)))) {
        drivestep(g);
    }
}

// The loop: wait, dispatch everything, then decide.
static void gui_run(Gui *g)
{
    Sched *s = &g->sched;
    while (!g->quit) {
        if (__atomic_load_n(&g->hostdone, __ATOMIC_ACQUIRE)) {
            g->quit = 1;  // the host stopped, and this process with it
            break;
        } else if (g->shedreq) {
            shednow(g);
            continue;
        } else if (g->raisereq) {
            raisenow(g);
            continue;
        } else if (g->bg) {
            bgwait(g);
            continue;
        }
        s->now  = os_ticks(g->os);
        i64 t   = schedtimeout(s);
        if (g->quitting) {
            // Frames may not come (minimized), but the grace still ends
            i64 left = g->quitby - s->now;
            left = left>0 ? left : 0;
            t    = t<0 || t>left ? left : t;
        }
        g->woke = 0;
        if (!os_guiwait(g->os, t)) {
            break;
        }
        if (g->quitting && os_ticks(g->os)>=g->quitby) {
            g->quit = 1;  // the engine did not stop in time
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

    // The anatomy of a frame over the scroll budget
    #ifdef NDEBUG
    i64 slow = 4000;
    #else
    i64 slow = 12000;
    #endif
    if (cpu >= slow) {
        Arena a = g->frame;
        Buf   b(&a, 256);
        print(&b, "bench: slow frame ");
        print(&b, (i64)g->benchn);
        print(&b, ": ");
        printfloat(&b, (f64)cpu/1000, 2);
        print(&b, " ms: new frame ");
        printfloat(&b, (f64)g->benchseg[0]/1000, 2);
        print(&b, ", UI ");
        printfloat(&b, (f64)g->benchseg[1]/1000, 2);
        print(&b, " (layout ");
        printfloat(&b, (f64)g->layoutus/1000, 2);
        print(&b, " for ");
        print(&b, g->benchlaid1);
        print(&b, " messages), render ");
        printfloat(&b, (f64)g->benchseg[2]/1000, 2);
        if (g->benchsave) {
            print(&b, ", saving the layout ");
            printfloat(&b, (f64)g->benchsave/1000, 2);
        }
        print(&b, "; ");
        print(&b, (i64)g->benchglyphs);
        print(&b, " glyphs new to the atlas");
        if (g->benchtex != g->benchtex0) {
            print(&b, ", which grew to ");
            print(&b, (i64)g->benchtex);
            print(&b, " wide");
        }
        guilog(g, finish(&b));
    }
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
// tests/fake_provider.py's "gui" scenario (tests/gui.py runs it), and
// M4's: subagents and their asks, the todo list, background jobs, undo
// and rewind with a conflict, the session's info, compactions, a queue
// and a fallback, half in the light theme and half in the dark.

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
    OP_MINCLOSE,  // minimize the window, then ask it to close once it is
    OP_MINCLOSED, // ask to close now if the window never minimized
    OP_SYSTHEME,  // the system's theme: dx 1 dark, 0 light, -1 its own
    OP_KILLENGINE,// the engine process dies (--engine=subprocess)
    OP_REVEAL,    // scroll the settings window to the RECT_* target
    OP_TOUCH,     // write over a file in --cwd, as something else would
    OP_TRAY,      // pick the tray icon's menu item dx (TRAY_*)
    OP_DROPLINK,  // break the shown session's host's stream (remotedrop)
    OP_TEXTRCWD,  // type the --rcwd directory, the remote one
    OP_MARKDELTAS,// remember the deltas the shown session has had
    OP_IFSESS,    // unless a session's title starts with text, skip dx ops
    OP_THINKING,  // show reasoning as dx (THINK_*), as a setting would
    OP_TOOLS,     // fold tool rows as dx (TOOLS_*), as a setting would
    OP_SHOWCODE,  // scroll to the code block selectcode chose
    OP_ANCHOR,    // remember what is at the transcript's top edge
    OP_GAUGE,     // a live record with ctx dx, compact_at dy, tokens text
    OP_SHOWPAGE,  // scroll the settings' pages to the one text names
    OP_GEOMMARK,  // remember the window's geometry
    OP_PLACE,     // move the window as if saved: dx 1 on a display to the
                  // right of all, gone since; 0 back as remembered
    OP_MARKSESS,  // remember the session shown, for CHK_DRAFTFILE
    OP_IFROLE,    // unless the configuration has role text, skip dx ops
    OP_MAXIMIZE,  // maximize the window (macOS: zoom), as its button would;
                  // dx 1: fill the primary display unmaximized (macOS's
                  // Fill), else skip the next op where that is no maximum
};

enum : i32 {
    T_NONE,
    T_PROMPTBTN,
    T_MENU,        // the menu bar's View menu
    T_HISTORY,
    T_MODALITEM,
    T_MODALOK,
    T_TRANSCRIPT,
    T_PROMPT,
    T_EMPTY,       // an empty part of the menu bar
    T_SPLITTER,    // the dock splitter between the columns
    T_HELPTAB,
    T_TRANSCRIPTTAB,
    T_BELOW,       // below the transcript's bottom edge
    T_COPYITEM,    // "Copy" in the transcript's context menu
    T_NEWBTN,      // the Session menu's "New session..."
    T_NEWCWD,      // the new session's directory field
    T_NEWCREATE,
    T_ALLOW,       // the latest open ask's buttons
    T_DENY,
    T_ALWAYS,
    T_SETTINGSTAB,
    T_SETTING,     // the first endpoint's idle_timeout field
    T_CLOSESTOP,   // the close dialog's buttons
    T_CLOSECANCEL,
    T_OTHERSESS,   // the first session in the sidebar not shown
    T_TOPTEXT,     // near the transcript's top edge
    T_SESSTITLE,   // the sidebar's session whose title starts with text
    T_ADDNAME,     // the new endpoint's name field
    T_ADDBTN,      // "Add endpoint"
    T_THEMECOMBO,  // the theme's list in the settings window
    T_THEMEITEM,   // the theme named by text, in that list once open
    T_LONGFIELD,   // a setting's field holding more than its slack
    T_TASKLINK,    // the latest task row's subagent link
    T_PARENTLINK,  // a subagent's link back to its parent
    T_MSGLINK,     // the latest message's link to its other session
    T_UNDO,        // the latest Undo on a tool row
    T_REWINDTURN,  // the latest Rewind to here
    T_REWINDOK,    // the rewind confirmation's Rewind
    T_FORCE,       // the latest rewind card's Rewind anyway
    T_JOBKILL,     // the newest running job's Kill
    T_INFOBTN,     // the session's Info
    T_COMPACTHDR,  // the latest compaction card's header
    T_JOBSTAB,
    T_LOGTAB,
    T_CLOSEBG,     // the close dialog's Keep running in background
    T_HOSTSTAB,
    T_CONNECT,     // the Hosts panel's first remote's Connect or Disconnect
    T_CONNECT2,    // and its second's
    T_HOSTCOMBO,   // the new session's host
    T_HOSTITEM,    // the host named by text, in that list once open
    T_OUTPUT,      // the latest tool row's Output
    T_REFRESHINC,  // the settings' Refresh includes
    T_THUMB,       // the latest screenshot thumbnail drawn
    T_SHOTCLOSE,   // the Screenshot window's Close
    T_THINKHDR,    // the latest reasoning's header drawn
    T_CODEBLOCK,   // the code block selectcode chose, as drawn
    T_CODECOPY,    // and its Copy button
    T_TOOLSBTN,    // the session bar's toggles
    T_THINKBTN,
    T_GAUGE,       // the context gauge
    T_PAGECOMBO,   // the settings' pages as a combo, when narrow
    T_PAGEITEM,    // the settings page text names ("endpoint fake", "ui",
                   // "" for the top level, "diagnostics"), but the combo
                   // while it is closed: so twice to pick either way
    T_ADDOPEN,     // the Add of the kind text names, likewise
    T_DELETE,      // the settings page's Delete
    T_DELOK,       // its confirmation's Delete and Cancel
    T_DELCANCEL,
    T_ROOTS,       // the guard page's roots field
    T_SPLITFROM,   // where the dock splitter was before the first drag
    T_SESSMENU,    // the menu bar's Session menu
    T_ZOOMRESET,   // View's Actual size
    T_PANEITEM,    // View's toggle of the pane named by text
    T_CMUNDO,      // the compose box's context menu's items
    T_CMREDO,
    T_CMCUT,
    T_CMCOPY,
    T_CMPASTE,
    T_CMSELALL,
    T_CMEDITOR,
    T_EDITCANCEL,  // its Cancel while the editor runs
    T_MODEL0,      // the first model /model lists
    T_NAMED,       // the item whose name (itemnamed) starts with text
    T_USERMSG,     // the latest user message drawn in the transcript
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
    CHK_ADDEXISTS,
    CHK_ROQUIET,
    CHK_LIGHT,
    CHK_DARK,
    CHK_THEMEQUIET,
    CHK_TITLEBAR,
    CHK_LONGFIELD,
    CHK_LONGEDIT,
    CHK_ENGINESTOPPED,
    CHK_CLICKTHROUGH,
    CHK_NESTED,
    CHK_TASKSTATE,
    CHK_CHILDSHOWN,
    CHK_PARENTSHOWN,
    CHK_KIDASK,
    CHK_KIDALLOWED,
    CHK_TODOACTIVE,
    CHK_TODODONE,
    CHK_JOBS,
    CHK_JOBKILLED,
    CHK_REWINDASK,
    CHK_UNDONE,
    CHK_CONFLICT,
    CHK_FORCED,
    CHK_COMPACTING,
    CHK_SUMMARY,
    CHK_COMPACTOPEN,
    CHK_PRUNE,
    CHK_INFO,
    CHK_QUEUED,
    CHK_FALLBACK,
    CHK_JOBSTAB,
    CHK_JOBSDRAWN,
    CHK_JOBSQUIET,
    CHK_CLOSEBG,
    CHK_SHED,
    CHK_REOPENED,
    CHK_BGDONE,
    CHK_BGLISTED,
    CHK_BGSESSION,
    CHK_BGPROMPT,
    CHK_REMOTEUP,
    CHK_REMOTESESSION,
    CHK_REMOTEDOWN,
    CHK_REMOTEOK,
    CHK_OUTPUT,
    CHK_REMOTECLOSED,
    CHK_REFRESHED,
    CHK_OWNNEW,
    CHK_CLOSEREMOTE,
    CHK_REMOTEFAILED,
    CHK_THUMBS,
    CHK_SHOTOPEN,
    CHK_SHOTCLOSED,
    CHK_IMGQUIET,
    CHK_THINKFOLD,
    CHK_THINKGREW,
    CHK_THINKQUIET,
    CHK_THINKOPENED,
    CHK_THINKKEPT,
    CHK_THINKHIDDEN,
    CHK_THINKNONE,
    CHK_THINKOPEN,
    CHK_THINKSTAYS,
    CHK_THINKREFOLD,
    CHK_COPYBTN,
    CHK_CODECOPIED,
    CHK_COPYQUIET,
    CHK_REFLOW,
    CHK_TOOLSOPEN,
    CHK_TOOLSFOLD,
    CHK_TOOLSAUTO,
    CHK_SHOWTOOLS,
    CHK_SHOWTHINK,
    CHK_THINKBACK,
    CHK_FINDOPEN,
    CHK_FOUND,
    CHK_FOUNDNEXT,
    CHK_FOUNDPREV,
    CHK_FINDQUIET,
    CHK_FINDHIDDEN,
    CHK_FINDCLOSED,
    CHK_GAUGEREAL,
    CHK_GAUGE,
    CHK_GAUGEAMBER,
    CHK_ONEPANE,
    CHK_TWOPANE,
    CHK_NOHSCROLL,
    CHK_PAGE,
    CHK_ROOTS,
    CHK_DELINCLUDE,
    CHK_DELMODAL,
    CHK_DELKEPT,
    CHK_DELETED,
    CHK_REMOTEPAGE,
    CHK_HOSTSNOHSCROLL,
    CHK_MENU,
    CHK_PANECLOSED,
    CHK_PANEOPEN,
    CHK_ZOOM,
    CHK_ZOOMQUIET,
    CHK_PLACED,
    CHK_GEOMKEPT,
    CHK_PROMPT,       // the compose box holds text
    CHK_HIST,         // it recalls history entry text ("N/M"), "" if none
    CHK_DRAFTFILE,    // the marked session's draft sidecar holds text
    CHK_CARET,        // the caret: "0", "end", "first" or "second" line
    CHK_COMPOSEQUIET,
    CHK_CMENU,        // its context menu, enabling dy of the items dx
    CHK_SELALL,
    CHK_CLIPBOARD,    // the clipboard holds text
    CHK_EDITING,
    CHK_NOTEDITING,
    CHK_NOTE,         // the session's note has text in it
    CHK_SLASH,        // the slash popup lists dx commands, skills aside (0: none)
    CHK_SLASHSEL,     // its pick is text
    CHK_TITLE,        // the session's title is text
    CHK_LASTUSER,     // its latest user message is text
    CHK_MODELS,
    CHK_COMPACTED,
    CHK_SIDE,
    CHK_NOSIDE,
    CHK_SIDEINI,
    CHK_MODEL,
    CHK_EXPORTED,
    CHK_LASTERR,
    CHK_HOSTTRIED,
    CHK_FORKED,
    CHK_DELCOUNT,     // the delete confirmation counts dx sessions
    CHK_GUILOCK,      // this process holds the state directory's window lock
    CHK_ALWAYSBTN,
    CHK_ALWAYSPATH,
    CHK_DESKALWAYSBTN,
    CHK_KIDALWAYS,
    CHK_IMPORTCWD,
    CHK_IMPORTREMOTE,
    CHK_SHEDQUIET,
    CHK_KEYLOST,
    CHK_SNAPLIVE,
    CHK_PLANMODE,     // dx 1: the session shown is in plan mode, 0 not
    CHK_PLANASK,      // the open ask is the plan's, in Markdown, its buttons drawn
    CHK_PLANTEXT,     // the Plan window shows the plan, with text in it
    CHK_PLANMODEL,    // the model picker is enabled while the plan waits
    CHK_MAXIMIZED,    // maximized, filling its display, and saved so;
                      // dx 1: filling it, saved as maximized
    CHK_QUEUESHOWN,   // dx queued messages drawn above the box, the first text
    CHK_MSGNOTE,      // a message's notice names its sender, with a link
    CHK_MSGSENDER,    // the link showed the sender, whose row links back
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
    "adding a section that exists changed nothing",
    "a read-only session's turn runs elsewhere, with no clock here",
    "the light theme is in effect", "the dark theme is in effect",
    "the theme changed in a few frames, then none",
    "the window's frame follows the theme",
    "a setting longer than a field's slack is in its field whole",
    "the long setting, edited, was saved whole",
    "the engine died: nothing runs or waits, no ask stays open",
    "a click that activates the window also reaches what it lands on",
    "two subagents reported, listed under their parent",
    "the parent's strip counted its subagents at work",
    "the task row's link showed the subagent's transcript",
    "the subagent's link went back to its parent",
    "a subagent's ask marked its parent's row and task",
    "the subagent's ask, answered from its card, let it write",
    "the todo list showed an item in progress, the rest pending",
    "the todo list showed every item done",
    "the jobs panel: one exited, one running with its output",
    "killed from the panel, the job's end brought the agent back",
    "the rewind confirmation listed the file",
    "undone: the file is gone, its row says so, a card lists it",
    "rewinding a turn left a changed file alone, as a conflict",
    "forced, the rewind put the file back",
    "the strip showed a compaction running",
    "a summary's card with its token counts",
    "the compaction card opened to its summary",
    "a pruning's card listed the elided outputs, their rows marked",
    "the session's info listed its instruction file",
    "a session queued behind another model, with its place in line",
    "a retry's countdown named the fallback model",
    "a new layout shows the Jobs tab",
    "a job's output drawn a few times a second, the Jobs tab shown",
    "no frames for a job's output, the Jobs tab hidden",
    "closing offers to keep the turn running in the background",
    "the window is gone, and this process hosts its engine, registered",
    "the window came back, a client of its own host",
    "the turn finished in the background, its whole answer shown",
    "a new window found the background host and listed its session",
    "the background session opened, badged, its finished turn shown",
    "a prompt to the background session was answered through its host",
    "the remote host is connected",
    "a new session on the remote host, in its directory there",
    "the dropped link showed reconnecting, counting down",
    "reconnected: the answer whole, each delta once",
    "a call's whole output, read through its host, in the Output window",
    "disconnected: the remote's sessions say so",
    "the URL includes were fetched again",
    "a new session went to this process's own host",
    "closing says the remote turn keeps running there",
    "a remote that cannot work failed, saying why",
    "screenshots show as thumbnails on their tool rows",
    "a thumbnail's click opened the screenshot whole, at its size",
    "closed, the Screenshot window let its texture go",
    "no frames while screenshots show, once loaded",
    "reasoning streams folded to one header with its count and clock",
    "the folded header's count grew as it streamed",
    "a pause in reasoning drew its clock each second, and little more",
    "a click opened the streaming reasoning",
    "the reasoning opened stayed open through its commit",
    "hide: no reasoning drawn while it streams, the strip says why",
    "hide: no reasoning drawn once committed either",
    "open: the reasoning streams open",
    "open: the committed reasoning stays open",
    "fold again: reasoning folded, but for what the user opened",
    "a code block shows its Copy button while the mouse is over it",
    "its Copy put its code on the clipboard byte for byte, and said so",
    "Copied went back to Copy on its deadline, and little else drew",
    "a new width kept the line at the top edge there",
    "tools = open: every tool row open but those the user chose",
    "tools = fold: every tool row folded but those the user chose",
    "tools = auto: tool rows folded by kind and outcome again",
    "the session's toggle opened every tool row, the setting unchanged",
    "the session's toggle opened all reasoning",
    "toggled off, reasoning folded again but for what the user opened",
    "the find bar opened with its field focused",
    "found in folded reasoning, opened as chosen and scrolled to",
    "Enter went to the next match, opening its row",
    "Shift+Enter went back to the first",
    "no frames while the find bar shows its matches",
    "hidden reasoning is not searched",
    "Esc closed the find bar, and left the session alone",
    "the context gauge drew from the engine's own numbers",
    "the context gauge drew from a record's numbers",
    "the gauge turned amber near where compaction begins",
    "narrow, the settings' pages are a combo above the page",
    "wide, the settings' pages are a list beside the page",
    "the settings have nothing to scroll to sideways",
    "the settings page picked is shown",
    "the roots, one a line, were saved joined by \"; \"",
    "a section only an include defines cannot be deleted, which says where it is",
    "Delete asks first",
    "cancelled, the section and the file are as they were",
    "deleted: the section and the comment above it are gone, its page too",
    "a remote's page has Connect, and its list setting",
    "the Hosts panel has nothing to scroll to sideways",
    "a menu of the menu bar opened",
    "the pane is hidden, and the layout file will say so",
    "the pane is shown again, its tab in front",
    "zoomed: the text's size in ImGui and the transcript",
    "no frames once the zoom settled",
    "a window saved on a display gone since is centered on the primary",
    "the window is where it was",
    "the compose box holds what it should",
    "history mode as it should be",
    "the engine keeps the draft it should",
    "the caret is where it should be",
    "no frames once typing is done, the draft kept",
    "the box's context menu, its items enabled as they should be",
    "the box's text all selected",
    "the clipboard holds what was cut",
    "the box is read-only, saying an editor is at work",
    "no editor at work, and the box is writable",
    "the session's note says how the command went",
    "the slash popup lists the commands it should",
    "the slash popup's pick moved",
    "the session's title is as renamed",
    "the text went out as a message, not a command",
    "/model lists the configured models",
    "/compact compacted the conversation",
    "the sidebar shows it",
    "the sidebar does not show it",
    "the layout file keeps the sidebar's state",
    "the session switched model",
    "the export was written with what it should hold",
    "the error says why",
    "the remote's heading connected, or tried",
    "the fork is shown, its draft the prompt that came next",
    "the delete confirmation counts the sessions it takes",
    "the window lock is held, and by this process",
    "the ask offers Always for its directory, apart from Allow",
    "allowed always: the second write was not asked about",
    "the subagent's desktop ask offers Always for input",
    "the subagent typed for good, asked once",
    "the import dialog holds the directory there",
    "the imported session is shown on the remote host, no records lost",
    "no frames while the window was gone",
    "a key whose release was lost was let go of, and frames stopped",
    "the metrics overlay's numbers went on as frames ran on",
    "plan mode is on or off as it should be",
    "the plan's ask shows the plan, with Approve plan and Keep planning",
    "the Plan window shows the plan as the agent wrote it",
    "the model may change while the plan waits",
    "the window is maximized, filling its display, and will open so",
    "the box shows the messages queued, as it should",
    "the message from the subagent is shown, with a link to it",
    "the link showed the subagent, its message row linking back",
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
    W_GONE,       // the engine stopped
    W_TASKS,      // the session shown waits for two subagents
    W_KIDASK,     // a subagent of the session shown asks
    W_TODO,       // its todo list's first item is in progress
    W_SAID,       // its turn ended, its last reply starting with text
    W_JOBS,       // it is idle, its first job ended, its second runs
                  // with its output at "tick 3"
    W_REWOUND,    // every rewind sent has its card
    W_COMPACTING, // it runs a summary request
    W_QUEUED,     // it waits in the endpoint's line
    W_STREAMING,  // it streams a response
    W_SHED,       // the window is gone
    W_REOPENED,   // the window is back, its own host connected, the
                  // session shown open again
    W_BGHOST,     // a background host is connected, with sessions
    W_REMOTEUP,   // the first remote host is connected
    W_REMOTEDOWN, // and down, waiting to try again
    W_REMOTECLOSED, // and closed
    W_OUTPUT,     // the Output window's file came
    W_FILE,       // the file named by text exists in --cwd
    W_REFRESHED,  // a config.get {refresh} was answered
    W_THUMBS,     // the session shown's thumbnails came, and drew
    W_SHOT,       // the Screenshot window's image came, or failed
    W_THINKING,   // the session shown streams reasoning
    W_THINKSTALL, // and has had none for a while
    W_TURNS,      // the session shown is idle after dx turns
    W_EDITED,     // no editor runs
    W_NOTE,       // the session shown has a note
    W_COMPACTED,  // it has a compaction card, and is idle
    W_NAMED,      // an item named starting with text drew
    W_RAISED,     // dx launches handed off to this window, and it is up
    W_IMPORTED,   // an import ended, and none is under way
    W_EXPORTED,   // an export was saved, and none is under way
    W_PLAN,       // the Plan window has the plan, with text in it
};

static Str waitnames[] = {
    "", "the engine", "the session", "the turn to end", "an ask",
    "a running tool", "the configuration", "the settings edit", "quitting",
    "every turn to end", "the session to open", "amber", "a retry",
    "the engine to stop", "two subagents at work", "a subagent's ask",
    "a todo item in progress", "a reply", "the jobs", "a rewind",
    "a compaction", "the endpoint's line", "streaming", "the window to go",
    "the window to come back", "a background host", "the remote host",
    "the link to drop", "the remote to close", "the output", "a file",
    "the includes", "the thumbnails", "the screenshot", "reasoning",
    "a pause in reasoning", "turns", "the editor", "a note", "a compaction card",
    "a named item", "a launch to hand off", "the import", "the export",
    "the plan",
};
static_assert(countof(waitnames) == W_PLAN+1);

enum : i32 {
    KEY_CTRL  = 1,
    KEY_SHIFT = 2,
    KEY_CMD   = 4,  // Cmd on macOS, else Ctrl
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
    {OP_CHECK, CHK_CLICKTHROUGH, 0, 0, 0, ""},
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
    // The system's theme turns light, dark and light again, while
    // nothing else is pending, so that it changes at least twice either
    // way: the theme follows in a few frames, then none, and so does the
    // window's frame. The checks' log lines settle before the next.
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 1, 0, 2000, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 1000, ""},  // the system's own again
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
    // The menu bar: a pane hidden from the View menu, the layout saying
    // so, then shown again, in front
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MENU, 0, 0, 0, ""},
    {OP_MOVE, T_PANEITEM, 0, 0, 100, "Todo"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PANECLOSED, 0, 0, 0, "Todo"},
    {OP_CHECK, CHK_NOPOPUP, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "18-notodo"},
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_PANEITEM, 0, 0, 100, "Todo"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PANEOPEN, 0, 0, 0, "Todo"},
    // Zoom, by keys (Cmd on macOS, else Ctrl), by the wheel with the
    // key held, and back from the View menu, each in a few frames
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "125"},
    {OP_SHOT, 0, 0, 0, 300, "19-zoom125"},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "90"},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_WHEEL, 0, 0, 1, 50, ""},
    {OP_WHEEL, 0, 0, 1, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "110"},
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_ZOOMRESET, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 5000, ""},  // hover delays and the layout's save
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "100"},
    {OP_CHECK, CHK_ZOOMQUIET, 0, 0, 0, ""},
    // A window saved on a display unplugged since comes back centered on
    // the primary display, then goes back where it was
    {OP_GEOMMARK, 0, 0, 0, 0, ""},
    {OP_PLACE, 0, 1, 0, 1000, ""},
    {OP_CHECK, CHK_PLACED, 0, 0, 0, ""},
    {OP_PLACE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_GEOMKEPT, 0, 0, 0, ""},
    // Typing, then idle for 15 s: the caret blinks for 10 s, then stops
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 300, "Hello, Prospero"},
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
    // A key pressed whose release never comes (synthetic typing, focus
    // taken mid-press) is busy as if held, the metrics overlay's numbers
    // following the frames as they run, until no auto-repeat came for
    // KEY_STALE: then it is let go of, and frames stop. Away from the
    // prompt first, whose caret would blink again after a key
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_KEYDOWN, 0, GKEY_G, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_SNAPLIVE, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_KEYLOST, 0, 0, 0, ""},
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
    {OP_CHECK, CHK_JOBSTAB, 0, 0, 0, ""},
    {OP_CHECK, CHK_CLICKTHROUGH, 0, 0, 0, ""},
    // Tooltip on a menu item, appearing with no event at all
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MENU, 0, 0, 0, ""},
    {OP_MOVE, T_NEWBTN, 0, 0, 1000, ""},
    {OP_CHECK, CHK_TOOLTIP, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 700, ""},
    {OP_CHECK, CHK_NOTOOLTIP, 0, 0, 0, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    // A popup
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_POPUP, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 700, ""},
    {OP_CHECK, CHK_NOPOPUP, 0, 0, 0, ""},
    // A new session through its dialog, a modal, from the Session menu:
    // Esc cancels it; then the directory typed over the field's
    // suggestion, and Enter creates it
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NEWBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 500, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NEWBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_TEXTCWD, 0, 0, 0, 300, ""},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
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
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_ROQUIET, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "0-readonly"},
    // A session with two screenshots (tests/gui.py, on POSIX): their
    // thumbnails, through the file command, then one whole in the
    // Screenshot window, and closed; no frames meanwhile but for input
    {OP_IFSESS, 0, 23, 0, 0, "Look at the desktop."},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Look at the desktop."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_WAIT, W_THUMBS, 0, 0, 10000, ""},
    {OP_CHECK, CHK_THUMBS, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_IMGQUIET, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "0-thumbs"},
    {OP_MOVE, T_THUMB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SHOT, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 800, ""},
    {OP_CHECK, CHK_SHOTOPEN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "0-screenshot"},
    {OP_MOVE, T_SHOTCLOSE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_SHOTCLOSED, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_IMGQUIET, 0, 0, 0, ""},
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
    // The system's theme turns light, dark and light again, while
    // nothing else is pending, so that it changes at least twice either
    // way: the theme follows in a few frames, then none, and so does the
    // window's frame. The checks' log lines settle before the next.
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 1, 0, 2000, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 1000, ""},  // the system's own again
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
    // The menu bar: a pane hidden from the View menu, the layout saying
    // so, then shown again, in front
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MENU, 0, 0, 0, ""},
    {OP_MOVE, T_PANEITEM, 0, 0, 100, "Todo"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PANECLOSED, 0, 0, 0, "Todo"},
    {OP_CHECK, CHK_NOPOPUP, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "18-notodo"},
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_PANEITEM, 0, 0, 100, "Todo"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PANEOPEN, 0, 0, 0, "Todo"},
    // Zoom, by keys (Cmd on macOS, else Ctrl), by the wheel with the
    // key held, and back from the View menu, each in a few frames
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_EQUAL, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "125"},
    {OP_SHOT, 0, 0, 0, 300, "19-zoom125"},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_MINUS, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "90"},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_WHEEL, 0, 0, 1, 50, ""},
    {OP_WHEEL, 0, 0, 1, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 500, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "110"},
    {OP_MOVE, T_MENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_ZOOMRESET, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 5000, ""},  // hover delays and the layout's save
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_ZOOM, 0, 0, 0, "100"},
    {OP_CHECK, CHK_ZOOMQUIET, 0, 0, 0, ""},
    // A window saved on a display unplugged since comes back centered on
    // the primary display, then goes back where it was
    {OP_GEOMMARK, 0, 0, 0, 0, ""},
    {OP_PLACE, 0, 1, 0, 1000, ""},
    {OP_CHECK, CHK_PLACED, 0, 0, 0, ""},
    {OP_PLACE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_GEOMKEPT, 0, 0, 0, ""},
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
    // Reasoning as [ui] thinking says, changed as a setting would be,
    // for the session already open. Folded, one line whose count goes
    // up as it streams, its clock drawn once a second through a pause
    // and little else; opened by a click while it streams, it stays
    // open through its commit
    {OP_THINKING, 0, THINK_FOLD, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Think slowly."},
    {OP_WAIT, W_THINKING, 0, 0, 10000, ""},
    {OP_CHECK, CHK_THINKFOLD, 0, 0, 800, ""},
    {OP_CHECK, CHK_THINKGREW, 0, 0, 0, ""},
    {OP_WAIT, W_THINKSTALL, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_THINKQUIET, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "17-thinking"},
    {OP_MOVE, T_THINKHDR, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_THINKOPENED, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_THINKKEPT, 0, 0, 0, ""},
    // Hidden: none drawn, this turn's or the earlier, the strip saying
    // why nothing shows
    {OP_THINKING, 0, THINK_HIDE, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Think briefly."},
    {OP_WAIT, W_THINKING, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_THINKHIDDEN, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_THINKNONE, 0, 0, 0, ""},
    // Open: streamed open, and left so; folded again, all but the one
    // the user opened
    {OP_THINKING, 0, THINK_OPEN, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Think briefly."},
    {OP_WAIT, W_THINKING, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_THINKOPEN, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_THINKSTAYS, 0, 0, 0, ""},
    {OP_THINKING, 0, THINK_FOLD, 0, 500, ""},
    {OP_CHECK, CHK_THINKREFOLD, 0, 0, 0, ""},
    // The transcript's M8 parts, over what the turns so far made. A code
    // block's Copy, shown while the mouse is over it: the code byte for
    // byte, then "Copied" until its deadline, and little else drawn
    {OP_SELCODE, 0, 0, 0, 0, ""},
    {OP_SHOWCODE, 0, 0, 0, 500, ""},
    {OP_MOVE, T_CODEBLOCK, 0, 0, 400, ""},
    {OP_CHECK, CHK_COPYBTN, 0, 0, 0, ""},
    {OP_MOVE, T_CODECOPY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_CHECK, CHK_CODECOPIED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "20-codecopy"},
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_COPYQUIET, 0, 0, 0, ""},
    // A narrower window and back: the line at the transcript's top edge
    // stays there
    {OP_MOVE, T_TRANSCRIPT, 0, 0, 100, ""},
    {OP_WHEEL, 0, 0, -2, 800, ""},
    {OP_ANCHOR, 0, 0, 0, 0, ""},
    {OP_RESIZE, 0, 1050, 760, 800, ""},
    {OP_CHECK, CHK_REFLOW, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "21-reflow"},
    {OP_ANCHOR, 0, 0, 0, 0, ""},
    {OP_RESIZE, 0, 1280, 800, 800, ""},
    {OP_CHECK, CHK_REFLOW, 0, 0, 0, ""},
    // tools = open, fold and auto again, as the settings would set it
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_TOOLS, 0, TOOLS_OPEN, 0, 300, ""},
    {OP_CHECK, CHK_TOOLSOPEN, 0, 0, 0, ""},
    {OP_TOOLS, 0, TOOLS_FOLD, 0, 300, ""},
    {OP_CHECK, CHK_TOOLSFOLD, 0, 0, 0, ""},
    {OP_TOOLS, 0, TOOLS_AUTO, 0, 300, ""},
    {OP_CHECK, CHK_TOOLSAUTO, 0, 0, 0, ""},
    // The session's toggles, from the session bar, then off by their
    // shortcuts, Cmd+Shift+T and R (Ctrl elsewhere)
    {OP_MOVE, T_TOOLSBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 400, ""},
    {OP_CHECK, CHK_SHOWTOOLS, 0, 0, 0, ""},
    {OP_MOVE, T_THINKBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 400, ""},
    {OP_CHECK, CHK_SHOWTHINK, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "22-showall"},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_SHIFT, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYDOWN, 0, GKEY_T, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_T, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_SHIFT, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 300, ""},
    {OP_CHECK, CHK_TOOLSAUTO, 0, 0, 0, ""},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_SHIFT, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYDOWN, 0, GKEY_R, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_R, KEY_CMD|KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_SHIFT, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 300, ""},
    {OP_CHECK, CHK_THINKBACK, 0, 0, 0, ""},
    // Find, opened by Cmd+F (Ctrl elsewhere): in folded reasoning, whose row
    // opens; Enter to the next match and Shift+Enter back; nothing
    // drawn while the matches show; hidden reasoning not searched; Esc
    // closes the bar
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_F, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_F, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 300, ""},
    {OP_CHECK, CHK_FINDOPEN, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 800, "briefly 7."},
    {OP_CHECK, CHK_FOUND, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "23-find"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 600, ""},
    {OP_CHECK, CHK_FOUNDNEXT, 0, 0, 0, ""},
    {OP_KEYDOWN, 0, GKEY_ENTER, KEY_SHIFT, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, KEY_SHIFT, 600, ""},
    {OP_CHECK, CHK_FOUNDPREV, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_MARK, 0, 0, 0, 2500, ""},
    {OP_CHECK, CHK_FINDQUIET, 0, 0, 0, ""},
    {OP_THINKING, 0, THINK_HIDE, 0, 500, ""},
    {OP_CHECK, CHK_FINDHIDDEN, 0, 0, 0, ""},
    {OP_THINKING, 0, THINK_FOLD, 0, 300, ""},
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""},
    {OP_KEYDOWN, 0, GKEY_F, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_F, KEY_CMD, 50, ""},
    {OP_KEYUP, 0, GKEY_CMD, 0, 300, ""},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 300, ""},
    {OP_CHECK, CHK_FINDCLOSED, 0, 0, 0, ""},
    // The context gauge, from the engine's records, then from a live
    // record's numbers near where compaction begins, and its tooltip
    {OP_CHECK, CHK_GAUGEREAL, 0, 0, 0, ""},
    {OP_GAUGE, 0, 131072, 104857, 300, "42000"},
    {OP_CHECK, CHK_GAUGE, 0, 0, 0, ""},
    {OP_GAUGE, 0, 131072, 104857, 300, "98000"},
    {OP_CHECK, CHK_GAUGEAMBER, 0, 0, 0, ""},
    {OP_MOVE, T_GAUGE, 0, 0, 1500, ""},
    {OP_SHOT, 0, 0, 0, 300, "24-gauge"},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
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
    // An ask allowed for good through its Always button, past a rule
    // from Allow: a directory of its own (tests/gui.py), so that later
    // asks still ask, written twice with one ask
    {OP_PROMPT, 0, 0, 0, 100, "Write outside for good: ask once."},
    {OP_WAIT, W_ASK, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_ALWAYSBTN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "2-always"},
    {OP_MOVE, T_ALWAYS, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_ALWAYSPATH, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "2-allowedalways"},
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
    // M4, in the light theme first. Two subagents, counted at work in
    // their parent's strip, then listed under it; the parent's task row
    // shows one, and the subagent's own link goes back
    {OP_SYSTHEME, 0, 0, 0, 1000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Start subagents."},
    {OP_WAIT, W_TASKS, 0, 0, 10000, ""},
    {OP_CHECK, CHK_TASKSTATE, 0, 0, 0, ""},
    {OP_WAIT, W_ALLIDLE, 0, 0, 20000, ""},
    {OP_CHECK, CHK_NESTED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "7-subagents"},
    {OP_MOVE, T_TASKLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_CHILDSHOWN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "8-subagent"},
    {OP_MOVE, T_PARENTLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PARENTSHOWN, 0, 0, 0, ""},
    // A subagent messages its parent, read after it reports; the notice
    // leads to the sender, and its row back
    {OP_PROMPT, 0, 0, 0, 0, "Start a subagent that messages."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_WAIT, W_ALLIDLE, 0, 0, 20000, ""},
    {OP_CHECK, CHK_MSGNOTE, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "8b-message"},
    {OP_MOVE, T_MSGLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MSGSENDER, 0, 0, 0, ""},
    {OP_MOVE, T_MSGLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PARENTSHOWN, 0, 0, 0, ""},
    // A subagent asks: its parent's row and task row say so, and the
    // task row leads to its card, answered before the deadline
    {OP_PROMPT, 0, 0, 0, 0, "Start a subagent that asks."},
    {OP_WAIT, W_KIDASK, 0, 0, 10000, ""},
    {OP_CHECK, CHK_KIDASK, 0, 0, 0, ""},
    {OP_MOVE, T_TASKLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 400, ""},
    {OP_WAIT, W_ASK, 0, 0, 2000, ""},
    {OP_MOVE, T_ALLOW, 0, 0, 150, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_MAIN, 0, 0, 0, 300, ""},
    {OP_WAIT, W_ALLIDLE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_KIDALLOWED, 0, 0, 0, ""},
    // The todo list as the agent works through it
    {OP_PROMPT, 0, 0, 0, 0, "Plan it with a todo list."},
    {OP_WAIT, W_TODO, 0, 0, 10000, ""},
    {OP_CHECK, CHK_TODOACTIVE, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "9-todo"},
    {OP_WAIT, W_SAID, 0, 0, 10000, "Planned and done."},
    {OP_CHECK, CHK_TODODONE, 0, 0, 0, ""},
    // Background jobs: one done, one printing, its output drawn only
    // while the Jobs tab shows it, and a few times a second at most,
    // then killed from the panel, whose end brings the agent back by
    // itself
    {OP_PROMPT, 0, 0, 0, 0, "Start background jobs."},
    {OP_WAIT, W_JOBS, 0, 0, 10000, ""},
    {OP_CHECK, CHK_JOBS, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "10-jobs"},
    {OP_MARK, 0, 0, 0, 5000, ""},
    {OP_CHECK, CHK_JOBSDRAWN, 0, 0, 0, ""},
    {OP_MOVE, T_LOGTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1500, ""},
    {OP_MARK, 0, 0, 0, 5000, ""},
    {OP_CHECK, CHK_JOBSQUIET, 0, 0, 0, ""},
    {OP_MOVE, T_JOBSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_JOBKILL, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SAID, 0, 0, 10000, "The second job was killed."},
    {OP_CHECK, CHK_JOBKILLED, 0, 0, 0, ""},
    // Then dark. The latest write undone from its row, the turn rewound
    // into a conflict with a change made outside, then forced
    {OP_SYSTHEME, 0, 1, 0, 1000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Make undoable changes."},
    {OP_WAIT, W_SAID, 0, 0, 10000, "Made three changes."},
    {OP_MOVE, T_UNDO, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_REWINDASK, 0, 0, 0, "undo-b.txt"},
    {OP_SHOT, 0, 0, 0, 300, "11-undo"},
    {OP_MOVE, T_REWINDOK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REWOUND, 0, 0, 5000, ""},
    {OP_CHECK, CHK_UNDONE, 0, 0, 0, ""},
    {OP_TOUCH, 0, 0, 0, 300, "undo-a.txt"},
    {OP_MOVE, T_REWINDTURN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_REWINDASK, 0, 0, 0, "undo-a.txt"},
    {OP_MOVE, T_REWINDOK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REWOUND, 0, 0, 5000, ""},
    {OP_CHECK, CHK_CONFLICT, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "12-conflict"},
    {OP_MOVE, T_FORCE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_REWINDASK, 0, 0, 0, "undo-a.txt"},
    {OP_MOVE, T_REWINDOK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REWOUND, 0, 0, 5000, ""},
    {OP_CHECK, CHK_FORCED, 0, 0, 0, ""},
    // The session's info: its instruction file
    {OP_MOVE, T_INFOBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_INFO, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "13-info"},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_NOPOPUP, 0, 0, 0, ""},
    // Compaction: a summary in a small window, the strip saying so as it
    // runs, its card opened; then old outputs elided in a big one
    {OP_NEWSESSION, 0, 0, 0, 0, "small"},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Read the big files."},
    {OP_WAIT, W_COMPACTING, 0, 0, 15000, ""},
    {OP_CHECK, CHK_COMPACTING, 0, 0, 0, ""},
    {OP_WAIT, W_SAID, 0, 0, 15000, "All files read."},
    {OP_CHECK, CHK_SUMMARY, 0, 0, 0, ""},
    {OP_MOVE, T_COMPACTHDR, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 600, ""},
    {OP_CHECK, CHK_COMPACTOPEN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "14-compact"},
    {OP_NEWSESSION, 0, 0, 0, 0, "prune"},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Read the big files."},
    {OP_WAIT, W_SAID, 0, 0, 15000, "Pruned and done."},
    {OP_CHECK, CHK_PRUNE, 0, 0, 0, ""},
    // Liveness: queued behind another model at a router of one, then a
    // limit's fallback counting down to its retry
    {OP_NEWSESSION, 0, 0, 0, 0, "lead"},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 300, "Stream slowly for a while."},
    {OP_NEWSESSION, 0, 0, 0, 0, "rival"},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Say hello."},
    {OP_WAIT, W_QUEUED, 0, 0, 5000, ""},
    {OP_CHECK, CHK_QUEUED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "15-queued"},
    {OP_WAIT, W_ALLIDLE, 0, 0, 20000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, "failover"},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Fail over."},
    {OP_WAIT, W_BACKOFF, 0, 0, 10000, ""},
    {OP_CHECK, CHK_FALLBACK, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "16-fallback"},
    {OP_WAIT, W_SAID, 0, 0, 15000, "The fallback answered."},
    {OP_SYSTHEME, 0, -1, 0, 500, ""},
    {OP_MAIN, 0, 0, 0, 500, ""},
    // Esc abandons an edit in a settings field, leaving the turn alone,
    // and from the session's own windows cancels the turn mid-tool
    {OP_PROMPT, 0, 0, 0, 100, "Cancel me: run a very slow command."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_MOVE, T_SETTINGSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 5000, ""},
    {OP_SHOWPAGE, 0, 0, 0, 300, "endpoint fake"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "endpoint fake"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "endpoint fake"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "endpoint fake"},
    {OP_MOVE, T_SETTING, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "7"},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 500, ""},
    {OP_CHECK, CHK_STILLBUSY, 0, 0, 0, ""},
    {OP_MOVE, T_TRANSCRIPTTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
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
    // Adding a section that exists leaves it alone, and shows it (the
    // file keeps its one-line diff, which tests/gui.py checks)
    {OP_SHOWPAGE, 0, 0, 0, 300, "add endpoint"},
    {OP_MOVE, T_ADDOPEN, 0, 0, 100, "endpoint"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_ADDOPEN, 0, 0, 100, "endpoint"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_ADDNAME, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "fake"},
    {OP_MOVE, T_ADDBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_ADDEXISTS, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "endpoint fake"},
    // Typing, then idle for 15 s: the caret blinks for 10 s, then stops
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 300, "Hello, Prospero"},
    {OP_CHECK, CHK_TYPED, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 5000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 10-15 s after typing"},
    {OP_CHECK, CHK_BLINKOFF, 0, 0, 0, ""},
    // Idle: no frames at all once the probe has run
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // A key pressed whose release never comes (synthetic typing, focus
    // taken mid-press) is busy as if held, the metrics overlay's numbers
    // following the frames as they run, until no auto-repeat came for
    // KEY_STALE: then it is let go of, and frames stop. Away from the
    // prompt first, whose caret would blink again after a key
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_KEYDOWN, 0, GKEY_G, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_SNAPLIVE, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_KEYLOST, 0, 0, 0, ""},
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
    // Again, minimized: the window comes back for the dialog (no frame
    // runs while minimized, and the checklist would wait forever)
    {OP_MINCLOSE, 0, 0, 0, 3000, ""},
    {OP_MINCLOSED, 0, 0, 0, 1000, ""},
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

// The settings window (--settings, tests/gui.py --mode settings), over a
// transcript of most of what it can show: reasoning, markdown and code,
// a diff, an open ask, the strip waiting for an answer. Docked as it is
// by default, it is narrow: its pages are a combo above the page. Wider,
// they are a list beside it, from which the checklist edits a value
// longer than a field's slack whole, picks the theme from its list, dark
// and then light, edits the guard's roots one a line, finds a section of
// an included file that cannot be deleted, deletes one of the user's own
// after cancelling once, adds a model, and shows a remote's page.
// Narrower again, nothing scrolls sideways, in the Hosts panel either.
// Screenshots of each with --shots.
static DriveOp settingsops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Show the themes."},
    {OP_WAIT, W_ASK, 0, 0, 10000, ""},
    {OP_MOVE, T_SETTINGSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 5000, ""},
    // Narrow: the pages in a combo
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_ONEPANE, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    {OP_SHOWPAGE, 0, 0, 0, 300, "endpoint fake"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "endpoint fake"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "settings-combo"},
    {OP_SHOWPAGE, 0, 0, 0, 300, "endpoint fake"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "endpoint fake"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "endpoint fake"},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "settings-narrow"},
    // Wider: the list beside the page, which stays the one picked
    {OP_MOVE, T_SPLITTER, 0, 0, 300, ""},
    {OP_DOWN, 0, 0, 0, 50, ""},
    {OP_MOVE, T_SPLITFROM, -60, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, -120, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, -180, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, -240, 0, 300, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_TWOPANE, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "endpoint fake"},
    {OP_SHOT, 0, 0, 0, 300, "settings-wide"},
    // The long value: whole in its field, and whole once edited there
    {OP_SHOWPAGE, 0, 0, 0, 300, "role long"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "role long"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "role long"},
    {OP_REVEAL, RECT_LONGFIELD, 0, 0, 500, ""},
    {OP_CHECK, CHK_LONGFIELD, 0, 0, 0, ""},
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_LONGFIELD, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_KEYDOWN, 0, GKEY_END, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_END, 0, 100, ""},
    {OP_TEXT, 0, 0, 0, 200, "!"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_LONGEDIT, 0, 0, 0, ""},
    // The theme, dark, then light
    {OP_SHOWPAGE, 0, 0, 0, 300, "ui"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "ui"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_REVEAL, RECT_THEMECOMBO, 0, 0, 500, ""},
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_THEMECOMBO, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_THEMEITEM, 0, 0, 100, "dark"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_SHOT, 0, 0, 0, 500, "dark"},
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_THEMECOMBO, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_THEMEITEM, 0, 0, 100, "light"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLEBAR, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_SHOT, 0, 0, 0, 500, "light"},
    // A configured theme stays when the system's changes: no frames
    {OP_MARK, 0, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 1, 0, 1000, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_CHECK, CHK_THEMEQUIET, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 300, ""},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // The guard's roots, one a line, saved as the field is left
    {OP_SHOWPAGE, 0, 0, 0, 300, "guard"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "guard"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_ROOTS, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 100, "~/a"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 100, ""},
    {OP_TEXT, 0, 0, 0, 300, "~/b"},
    {OP_SHOT, 0, 0, 0, 300, "guard"},
    {OP_MOVE, T_EMPTY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_ROOTS, 0, 0, 0, ""},
    // An included file's section cannot be deleted here
    {OP_SHOWPAGE, 0, 0, 0, 300, "model shared"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "model shared"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_DELINCLUDE, 0, 0, 0, ""},
    // The user's own: asks first, and cancelled, stays
    {OP_SHOWPAGE, 0, 0, 0, 300, "endpoint gone"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "endpoint gone"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_MOVE, T_DELETE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_DELMODAL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "delete"},
    {OP_MOVE, T_DELCANCEL, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_DELKEPT, 0, 0, 0, ""},
    {OP_MOVE, T_DELETE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MOVE, T_DELOK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_CHECK, CHK_DELETED, 0, 0, 0, ""},
    // A new model from its list, its page then shown
    {OP_SNAPCFG, 0, 0, 0, 100, ""},
    {OP_SHOWPAGE, 0, 0, 0, 300, "add model"},
    {OP_MOVE, T_ADDOPEN, 0, 0, 100, "model"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MOVE, T_ADDNAME, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 200, ""},
    {OP_TEXT, 0, 0, 0, 200, "fresh"},
    {OP_SHOT, 0, 0, 0, 300, "add"},
    {OP_MOVE, T_ADDBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_CONFIGSET, 0, 0, 5000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "model fresh"},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    // A remote's page
    {OP_SHOWPAGE, 0, 0, 0, 300, "remote far"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "remote far"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_REMOTEPAGE, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote"},
    // Narrower than docked: still nothing to scroll sideways
    {OP_MOVE, T_SPLITTER, 0, 0, 300, ""},
    {OP_DOWN, 0, 0, 0, 50, ""},
    {OP_MOVE, T_SPLITFROM, -60, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, 60, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, 120, 0, 30, ""},
    {OP_MOVE, T_SPLITFROM, 150, 0, 300, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_ONEPANE, 0, 0, 0, ""},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "settings-narrowest"},
    {OP_SHOWPAGE, 0, 0, 0, 300, "model shared"},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "model shared"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_PAGEITEM, 0, 0, 100, "model shared"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_PAGE, 0, 0, 0, "model shared"},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOHSCROLL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "settings-narrowest-model"},
    // The Hosts panel there too, its new remote's fields one a line
    {OP_MOVE, T_HOSTSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_HOSTSNOHSCROLL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "hosts-narrowest"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // Deny the ask, and quit
    {OP_MOVE, T_DENY, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_IDLE, 0, 0, 10000, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// The sessions sidebar (--sessions, tests/gui.py --mode sessions): two
// sessions, one with two subagents, under this computer's heading and
// a configured remote's, never connected, while a remote listed only
// when connected is left out. A session with subagents folds and opens
// again; one is renamed, moved into a new group, which folds, kept in
// the layout file, archived and back, its model switched from the
// session bar's picker, exported as Markdown and as its raw log, and
// forked before its prompt from the transcript's menu, the prompt then
// the fork's draft. The other is forked whole, then deleted with its
// subagents after cancelling once. The remote's heading connects. The
// Session menu compacts, which is refused with its reason. Screenshots
// of each with --shots.
static DriveOp sessionsops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Say hello."},
    {OP_WAIT, W_SAID, 0, 0, 10000, "Hello"},
    {OP_MOVE, T_EMPTY, 0, 0, 1100, ""},  // the next session's id later
    {OP_NEWSESSION, 0, 0, 0, 500, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Start subagents."},
    {OP_WAIT, W_SAID, 0, 0, 20000, "Both"},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    // The tree: this computer and the remote, the hidden one left out,
    // the subagents under their parent
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "H this\n S Start subagents.\n  S "},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "\n S Say hello.\n"},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "H remote:far\n"},
    {OP_CHECK, CHK_NOSIDE, 0, 0, 0, "remote:hidden"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-tree"},
    // Folded, and open again
    {OP_MOVE, T_NAMED, 0, 0, 100, "fold Start subagents."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "H this\n S Start subagents.+\n S Say hello.\n"},
    {OP_CHECK, CHK_SIDEINI, 0, 0, 0, "Folded=s:this:"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-folded"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "fold Start subagents."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Start subagents.\n  S "},
    // Renamed from its context menu
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Say hello."},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_POPUP, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-menu"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Rename..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 200, "Greeting"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-rename"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 1500, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Greeting\n"},
    // Into a new group, which folds, kept with the layout
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 500, "Move to group"},
    {OP_MOVE, T_NAMED, 0, 0, 300, "New group..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_TEXT, 0, 0, 0, 200, "Work"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 1500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "G Work\n H this\n  S Greeting\n"},
    {OP_CHECK, CHK_SIDEINI, 0, 0, 0, "Group=Work\n"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-group"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "head g:Work"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "G Work+\nH this\n"},
    {OP_CHECK, CHK_SIDEINI, 0, 0, 0, "Folded=g:Work\n"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "head g:Work"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SIDEINI, 0, 0, 0, "Opened=g:Work\n"},
    // Archived: under the Archived heading, folded, opened; then back
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Archive"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "A+\n"},
    {OP_CHECK, CHK_NOSIDE, 0, 0, 0, "S Greeting"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "head a"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "A\n H this\n  S Greeting\n"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-archived"},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Unarchive"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "G Work\n H this\n  S Greeting\n"},
    {OP_CHECK, CHK_NOSIDE, 0, 0, 0, "\nA"},
    // Its model, from the session bar's picker
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_OPENED, 0, 0, 5000, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "model picker"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-picker"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "model fake:gui_b:"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_MODEL, 0, 0, 0, "fake:gui_b:"},
    // Exported: Markdown, then the raw log
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 500, "Export"},
    {OP_MOVE, T_NAMED, 0, 0, 300, "Markdown..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-export"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},  // Save...
    {OP_KEYUP, 0, GKEY_ENTER, 0, 500, ""},
    {OP_WAIT, W_FILE, 0, 0, 5000, "Greeting.md"},
    {OP_CHECK, CHK_EXPORTED, 0, 0, 0, "Greeting.md|# Greeting\n"},
    {OP_CHECK, CHK_EXPORTED, 0, 0, 0, "Greeting.md|## Agent\n\nHello!"},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 500, "Export"},
    {OP_MOVE, T_NAMED, 0, 0, 300, "Raw log..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_WAIT, W_FILE, 0, 0, 5000, "Greeting.jsonl"},
    {OP_CHECK, CHK_EXPORTED, 0, 0, 0, "Greeting.jsonl|\"ev\":\"session\""},
    {OP_CHECK, CHK_EXPORTED, 0, 0, 0, "Greeting.jsonl|\"ev\":\"model\""},
    // The Session menu: compacting so short a session is refused, with
    // its reason in the note above the box
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-sessmenu"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Compact"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "too little"},
    // The remote's heading connects (there is no such host)
    {OP_MOVE, T_NAMED, 0, 0, 100, "connect h:remote:far"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_HOSTTRIED, 0, 0, 0, "far"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-connect"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "connect h:remote:far"},  // Disconnect
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_REMOTECLOSED, 0, 0, 10000, ""},
    // A fork of a whole session, from the sidebar: shown, with no draft,
    // then renamed from the Session menu
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Start subagents."},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Fork"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "forked from"},
    {OP_CHECK, CHK_FORKED, 0, 0, 0, ""},
    {OP_CHECK, CHK_TITLE, 0, 0, 0, "Start subagents. (fork)"},
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Rename..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 200, "Whole fork"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 1500, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Whole fork\n"},
    // A fork before a prompt, from the transcript's menu: the turns
    // before it, none here, the prompt left in the new session's box
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_OPENED, 0, 0, 5000, ""},
    {OP_MOVE, T_USERMSG, 0, 0, 100, ""},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-forkhere"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Fork before this prompt"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_FORKED, 0, 0, 0, "Say hello."},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello."},
    {OP_CHECK, CHK_TITLE, 0, 0, 0, "Greeting (fork)"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-forked"},
    // The forked session with its subagents, deleted once confirmed,
    // after a cancel, the confirmation counting all three
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Start subagents."},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Delete..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_DELCOUNT, 3, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-delete"},
    {OP_MOVE, T_NAMED, 0, 0, 100, "delete cancel"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Start subagents.\n"},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Start subagents."},
    {OP_RDOWN, 0, 0, 0, 60, ""},
    {OP_RUP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Delete..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "delete ok"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 2000, ""},
    {OP_CHECK, CHK_NOSIDE, 0, 0, 0, "S Start subagents.\n"},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Whole fork\n"},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Greeting\n"},
    // Imported from the raw log exported above, from the Session menu
    // (the open dialog's answer that export): the dialog filled from the
    // log, its model the one switched to, then the imported session
    // shown with its transcript, and going on
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Import session..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-import"},
    {OP_MOVE, T_NEWCREATE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_IMPORTED, 0, 0, 10000, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "imported from"},
    {OP_CHECK, CHK_TITLE, 0, 0, 0, "Greeting (imported)"},
    {OP_CHECK, CHK_LASTUSER, 0, 0, 0, "Say hello."},
    {OP_CHECK, CHK_MODEL, 0, 0, 0, "fake:gui_b:"},
    {OP_SHOT, 0, 0, 0, 300, "sessions-imported"},
    {OP_PROMPT, 0, 0, 0, 100, "Say hello again."},
    {OP_WAIT, W_SAID, 0, 0, 10000, "Hello"},
    {OP_CHECK, CHK_LASTUSER, 0, 0, 0, "Say hello again."},
    {OP_CHECK, CHK_SIDE, 0, 0, 0, "S Greeting (imported)\n"},
    // A subagent's desktop ask (the fake helper, which tests/gui.py
    // configures here only), allowed for its class through Always,
    // apart from Allow: its next input is not asked about
    {OP_IFROLE, 0, 17, 0, 0, "desk"},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Greeting"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 0, "Start a desktop subagent."},
    {OP_WAIT, W_KIDASK, 0, 0, 10000, ""},
    {OP_MOVE, T_TASKLINK, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 400, ""},
    {OP_WAIT, W_ASK, 0, 0, 2000, ""},
    {OP_CHECK, CHK_DESKALWAYSBTN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-deskalways"},
    {OP_MOVE, T_ALWAYS, 0, 0, 150, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_ALLIDLE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_KIDALWAYS, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "sessions-deskallowed"},
    // Idle: no frames
    {OP_MOVE, T_EMPTY, 0, 0, 6000, ""},  // hover delays and the layout's save
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// The engine process dies (--enginestop, tests/gui.py --mode enginestop)
// with a command running in one session and an ask open in another: no
// record ends either turn, and the views must stop counting them, so
// that closing quits at once.
static DriveOp stopops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Close me: run a very slow command."},
    {OP_WAIT, W_TOOL, 0, 0, 10000, ""},
    {OP_NEWSESSION, 0, 0, 0, 500, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Write outside the workspace, ask, and wait for nobody."},
    {OP_WAIT, W_ASK, 0, 0, 10000, ""},
    {OP_KILLENGINE, 0, 0, 0, 0, ""},
    {OP_WAIT, W_GONE, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_ENGINESTOPPED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "stopped"},
    {OP_MARK, 0, 0, 0, 2000, ""},
    {OP_IDLE, 0, 0, 0, 0, "2 s after the engine stopped"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_CLOSE, 0, 0, 0, 0, ""},  // nothing runs: it quits at once
    {OP_WAIT, W_FOREVER, 0, 0, 5000, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// M5, keeping a turn running in the background (--background,
// tests/gui.py --mode background): the window goes partway through a
// streaming turn, and this process hosts its engine, keeping the state
// directory's window lock (M9), a key held as it goes (its release
// lost). The checklist goes on without frames, none drawn for that
// key, until tests/gui.py launches the GUI again on that state directory,
// which hands off to this process and exits: the window comes back, a
// client of its own host, showing the turn finished. Another launch,
// and the tray's Open, then raise it. A new session there starts a
// turn, and closing the window leaves the host and the turn running;
// the tray's Stop all stops the turn, and the host, with nothing to do,
// ends this process after its idle time (--host-idle). Light first,
// then dark.
static DriveOp bgops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_SYSTHEME, 0, 0, 0, 1000, ""},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_CHECK, CHK_SESSION, 0, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Stream in the background."},
    {OP_WAIT, W_STREAMING, 0, 0, 10000, ""},
    {OP_GEOMMARK, 0, 0, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 800, ""},
    {OP_CLOSE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_CLOSEMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_CLOSEBG, 0, 0, 0, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "bg-1-close"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // A key's release lost as the window goes: busy, and no frames
    // anyway while shed
    {OP_KEYDOWN, 0, GKEY_G, 0, 100, ""},
    {OP_MOVE, T_CLOSEBG, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SHED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_SHED, 0, 0, 0, ""},
    {OP_CHECK, CHK_GUILOCK, 0, 0, 0, ""},
    // No window: tests/gui.py looks on, then launches the GUI again
    {OP_TOUCH, 0, 0, 0, 0, "bg-ready"},
    {OP_WAIT, W_RAISED, 1, 0, 180000, ""},
    {OP_WAIT, W_REOPENED, 0, 0, 15000, ""},
    {OP_SYSTHEME, 0, 1, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 15000, ""},
    {OP_MARK, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_REOPENED, 0, 0, 0, ""},
    {OP_CHECK, CHK_SHEDQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_GEOMKEPT, 0, 0, 0, ""},
    {OP_CHECK, CHK_BGDONE, 0, 0, 0, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "bg-3-reopened"},
    {OP_TRAY, 0, TRAY_OPEN, 0, 1000, ""},  // shown already: raised
    // Launched again, shown: raised, and that launch exits
    {OP_TOUCH, 0, 0, 0, 0, "bg-back"},
    {OP_WAIT, W_RAISED, 2, 0, 60000, ""},
    {OP_CHECK, CHK_GUILOCK, 0, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s, the window back"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 500, ""},
    // A new session goes to the host, this window being its client
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_CHECK, CHK_OWNNEW, 0, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Stream slowly for a while."},
    {OP_WAIT, W_STREAMING, 0, 0, 10000, ""},
    // Closing the window of a host: it goes, the host and the turn stay,
    // until the tray stops it all; then the host has nothing to do, and
    // ends after its idle time (tests/gui.py gives it 20 s), as does this
    // process
    {OP_CLOSE, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SHED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_SHED, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_TRAY, 0, TRAY_STOPALL, 0, 0, ""},
    {OP_WAIT, W_FOREVER, 0, 0, 60000, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// M5, a window while another process hosts its state directory
// (--attach, tests/gui.py --mode background: since M9 "prospero serve",
// once the first window's process has ended, as two windows never
// share a state directory): it finds the host at startup, lists the
// session that ran in the background, badged, opens it with the turn
// finished, and prompts it through the host. Dark first, then light.
static DriveOp attachops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_SYSTHEME, 0, 1, 0, 0, ""},
    {OP_WAIT, W_BGHOST, 0, 0, 15000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_BGLISTED, 0, 0, 0, ""},
    // The Hosts tab, whose background host's age counts on by itself
    {OP_MOVE, T_HOSTSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s, the hosts shown"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_MOVE, T_TRANSCRIPTTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_MOVE, T_SESSTITLE, 0, 0, 100, "Stream in the background"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_BGSESSION, 0, 0, 0, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "bg-2-attached"},
    {OP_SYSTHEME, 0, 0, 0, 1000, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Say hello."},
    {OP_WAIT, W_SAID, 0, 0, 15000, "Hello!"},
    {OP_CHECK, CHK_BGPROMPT, 0, 0, 0, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s, attached"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 300, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// M5, a remote host (--remote, tests/gui.py --mode remote): connected
// from the Hosts panel, a session made there through the dialog in its
// directory there, its stream's link dropped and taken up again, a
// call's output read through it, its raw log exported and imported
// there, URL includes fetched again, then disconnected. Its model picker lists the models of the host's own
// configuration. Light, then dark.
static DriveOp remoteops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_SYSTHEME, 0, 0, 0, 500, ""},
    {OP_MOVE, T_HOSTSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MOVE, T_CONNECT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REMOTEUP, 0, 0, 90000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_REMOTEUP, 0, 0, 0, ""},
    // One whose destination ssh would take for an option fails, saying why
    {OP_MOVE, T_CONNECT2, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 5000, ""},
    {OP_CHECK, CHK_REMOTEFAILED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-1-hosts"},
    // A session there, through the dialog
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NEWBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_MOVE, T_HOSTCOMBO, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_HOSTITEM, 0, 0, 100, "r"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NEWCWD, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},  // selects it all, to be typed over
    {OP_TEXTRCWD, 0, 0, 0, 300, ""},
    {OP_MOVE, T_NEWCREATE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SESSION, 0, 0, 30000, ""},
    {OP_CHECK, CHK_REMOTESESSION, 0, 0, 0, ""},
    // Its model picker lists the models of the host's own configuration
    {OP_MOVE, T_NAMED, 0, 0, 100, "model picker"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_NAMED, 0, 0, 20000, "model fake:far:"},
    {OP_SHOT, 0, 0, 0, 300, "remote-models"},
    {OP_KEYDOWN, 0, GKEY_ESCAPE, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ESCAPE, 0, 300, ""},
    // Its stream's link dropped in the middle, and taken up again
    {OP_MARKDELTAS, 0, 0, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 100, "Stream for the link."},
    {OP_WAIT, W_STREAMING, 0, 0, 30000, ""},
    {OP_MARK, 0, 0, 0, 1500, ""},
    {OP_DROPLINK, 0, 0, 0, 0, ""},
    {OP_WAIT, W_REMOTEDOWN, 0, 0, 10000, ""},
    {OP_MARK, 0, 0, 0, 300, ""},
    {OP_CHECK, CHK_REMOTEDOWN, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 0, "remote-2-down"},
    {OP_WAIT, W_REMOTEUP, 0, 0, 60000, ""},
    {OP_WAIT, W_IDLE, 0, 0, 60000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_REMOTEOK, 0, 0, 0, ""},
    {OP_CHECK, CHK_LIGHT, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, 1, 0, 1000, ""},
    // Tools there, and a call's whole output through the host
    {OP_PROMPT, 0, 0, 0, 100, "Run the tools: read, edit, then a slow shell command."},
    {OP_WAIT, W_IDLE, 0, 0, 60000, ""},
    {OP_CHECK, CHK_TOOLROWS, 0, 0, 0, ""},
    {OP_SHOWUSER, 0, 0, 0, 500, "Run the tools"},
    {OP_MOVE, T_OUTPUT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_OUTPUT, 0, 0, 30000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_OUTPUT, 0, 0, 0, ""},
    {OP_CHECK, CHK_DARK, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-3-output"},
    // Its raw log exported, and imported there again from the Session
    // menu, the dialog filled with its directory there: the copy shown
    // with its transcript, though its log skips the numbers of the
    // records an import leaves out, and its replay follows the reply
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 500, "Export"},
    {OP_MOVE, T_NAMED, 0, 0, 300, "Raw log..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_WAIT, W_EXPORTED, 0, 0, 30000, ""},
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "Import session..."},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_MOVE, T_HOSTCOMBO, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_HOSTITEM, 0, 0, 100, "r"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_CHECK, CHK_IMPORTCWD, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-import"},
    {OP_MOVE, T_NEWCREATE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 100, ""},
    {OP_WAIT, W_IMPORTED, 0, 0, 60000, ""},
    {OP_WAIT, W_OPENED, 0, 0, 30000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "imported from"},
    {OP_CHECK, CHK_LASTUSER, 0, 0, 0, "Run the tools: read, edit, then a slow shell command."},
    {OP_CHECK, CHK_IMPORTREMOTE, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-imported"},
    // URL includes, fetched again from the settings window
    {OP_MOVE, T_SETTINGSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 5000, ""},
    {OP_MOVE, T_REFRESHINC, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REFRESHED, 0, 0, 40000, ""},
    {OP_CHECK, CHK_REFRESHED, 0, 0, 0, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_IDLE, 0, 0, 0, 0, "idle 3 s, connected"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    // Closing while a turn runs there says that it runs on; Cancel stays
    {OP_PROMPT, 0, 0, 0, 100, "Stream slowly for a while."},
    {OP_WAIT, W_STREAMING, 0, 0, 30000, ""},
    {OP_CLOSE, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_CLOSEMODAL, 0, 0, 0, ""},
    {OP_CHECK, CHK_CLOSEREMOTE, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-4-close"},
    {OP_MOVE, T_CLOSECANCEL, 0, 0, 200, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_NOMODAL, 0, 0, 0, ""},
    {OP_WAIT, W_IDLE, 0, 0, 30000, ""},
    // Left: its sessions run on there
    {OP_MOVE, T_HOSTSTAB, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 800, ""},
    {OP_MOVE, T_CONNECT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_REMOTECLOSED, 0, 0, 15000, ""},
    {OP_MARK, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_REMOTECLOSED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "remote-5-closed"},
    {OP_CHECK, CHK_NOSTALE, 0, 0, 0, ""},
    {OP_SYSTHEME, 0, -1, 0, 300, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Plan mode (--plan): a new session in plan mode from its dialog, the
// chip in its bar; the agent refused a change, asking to approve its
// plan, which the Plan window shows; kept planning with a note, the
// plan written again and shown again, the model free to change while
// it waits; approved, plan mode off and the plan carried out; then the
// toggle by Send, and /plan off.
static DriveOp planops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_MOVE, T_SESSMENU, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_MOVE, T_NEWBTN, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 1000, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_TEXTCWD, 0, 0, 0, 300, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "new plan"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_SHOT, 0, 0, 0, 300, "plan-1-new"},
    {OP_MOVE, T_NEWCREATE, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_WAIT, W_NAMED, 0, 0, 10000, "plan chip"},
    {OP_CHECK, CHK_PLANMODE, 1, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Make inside.txt."},
    {OP_WAIT, W_ASK, 0, 0, 30000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_PLANASK, 0, 0, 0, ""},
    // The chip shows the Plan window, beside the todo list
    {OP_MOVE, T_NAMED, 0, 0, 100, "plan chip"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_PLAN, 0, 0, 10000, "# Plan"},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_PLANTEXT, 0, 0, 0, "Write **inside.txt**"},
    {OP_SHOT, 0, 0, 0, 300, "plan-2-ask"},
    // Keep planning, with a note: written again, asked again
    {OP_MOVE, T_DENY, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_CHECK, CHK_POPUP, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 300, "Test it too."},
    {OP_SHOT, 0, 0, 0, 300, "plan-3-note"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 500, ""},
    {OP_WAIT, W_PLAN, 0, 0, 15000, "Plan v2"},
    {OP_WAIT, W_ASK, 0, 0, 15000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_PLANASK, 0, 0, 0, ""},
    {OP_CHECK, CHK_PLANTEXT, 0, 0, 0, "Test it too."},
    {OP_CHECK, CHK_PLANMODE, 1, 0, 0, ""},
    {OP_CHECK, CHK_PLANMODEL, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "plan-4-again"},
    // Approved: plan mode ends, and the agent carries out the plan
    {OP_MOVE, T_ALLOW, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_SAID, 0, 0, 15000, "Done"},
    {OP_WAIT, W_FILE, 0, 0, 5000, "inside.txt"},
    {OP_CHECK, CHK_PLANMODE, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_SHOT, 0, 0, 0, 300, "plan-5-approved"},
    // The toggle by Send, while idle, then /plan off
    {OP_MOVE, T_NAMED, 0, 0, 100, "plan toggle"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_WAIT, W_NOTE, 0, 0, 5000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "plan mode:"},
    {OP_CHECK, CHK_PLANMODE, 1, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "plan-6-toggle"},
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 300, ""},
    {OP_TEXT, 0, 0, 0, 300, "/plan off"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 300, ""},
    {OP_WAIT, W_NOTE, 0, 0, 5000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "plan mode is off"},
    {OP_CHECK, CHK_PLANMODE, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// Idle: nothing for a minute after startup settles.
// The compose box (--compose, then --compose-again, as tests/gui.py
// --mode compose runs them): history, the caret's edges, the context
// menu, the editor ([ui] editor a test script), slash commands and their
// popup, and one draft per session, kept by the engine once typing
// pauses and at once on leaving it or quitting, with no frames for it;
// then, after a restart, the drafts back from the engine.
#define KEY(k, mods, ms) \
    {OP_KEYDOWN, 0, k, mods, 50, ""}, {OP_KEYUP, 0, k, mods, ms, ""}
#define CLICK(t, text, ms) \
    {OP_MOVE, t, 0, 0, 100, text}, {OP_DOWN, 0, 0, 0, 60, ""}, {OP_UP, 0, 0, 0, ms, ""}
#define RCLICK(ms) \
    {OP_MOVE, T_PROMPT, 0, 0, 100, ""}, {OP_RDOWN, 0, 0, 0, 60, ""}, \
    {OP_RUP, 0, 0, 0, ms, ""}
#define SELALL \
    {OP_KEYDOWN, 0, GKEY_CMD, KEY_CMD, 50, ""}, KEY(GKEY_A, KEY_CMD, 50), \
    {OP_KEYUP, 0, GKEY_CMD, 0, 200, ""}
#define CTRLG(ms) \
    {OP_KEYDOWN, 0, GKEY_CTRL, KEY_CTRL, 50, ""}, KEY(GKEY_G, KEY_CTRL, 50), \
    {OP_KEYUP, 0, GKEY_CTRL, 0, ms, ""}

static DriveOp composeops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    // A, with two prompts to recall
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_MARKSESS, 0, 0, 0, 0, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Say hello."},
    {OP_WAIT, W_TURNS, 1, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Say hello again, please."},
    {OP_WAIT, W_TURNS, 2, 0, 10000, ""},
    // History: Up on the empty box recalls the newest, then older, no
    // further than the oldest; Down comes back to the empty draft, as
    // Esc does, which leaves the turn alone
    CLICK(T_PROMPT, "", 300),
    KEY(GKEY_UP, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello again, please."},
    {OP_CHECK, CHK_HIST, 0, 0, 0, "2/2"},
    KEY(GKEY_UP, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello."},
    {OP_CHECK, CHK_HIST, 0, 0, 0, "1/2"},
    {OP_SHOT, 0, 0, 0, 300, "compose-history"},
    KEY(GKEY_UP, 0, 300),
    {OP_CHECK, CHK_HIST, 0, 0, 0, "1/2"},
    KEY(GKEY_DOWN, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello again, please."},
    KEY(GKEY_DOWN, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_CHECK, CHK_HIST, 0, 0, 0, ""},
    KEY(GKEY_UP, 0, 300),
    KEY(GKEY_ESCAPE, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_CHECK, CHK_HIST, 0, 0, 0, ""},
    // Editing a recalled message makes it the draft, which the engine
    // keeps once typing has paused for a second
    KEY(GKEY_UP, 0, 300),
    {OP_TEXT, 0, 0, 0, 300, " Thanks."},
    {OP_CHECK, CHK_HIST, 0, 0, 0, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello again, please. Thanks."},
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1500, ""},
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0, "Say hello again, please. Thanks."},
    // The caret: between lines as ever, but Up on the first goes to the
    // start, and Down on the last to the end
    KEY(GKEY_ENTER, KEY_SHIFT, 100),
    {OP_TEXT, 0, 0, 0, 300, "line two"},
    {OP_CHECK, CHK_CARET, 0, 0, 0, "end"},
    KEY(GKEY_UP, 0, 300),
    {OP_CHECK, CHK_CARET, 0, 0, 0, "first"},
    KEY(GKEY_UP, 0, 300),
    {OP_CHECK, CHK_CARET, 0, 0, 0, "0"},
    KEY(GKEY_DOWN, 0, 300),
    {OP_CHECK, CHK_CARET, 0, 0, 0, "second"},
    KEY(GKEY_DOWN, 0, 300),
    {OP_CHECK, CHK_CARET, 0, 0, 0, "end"},
    // Once the caret stops blinking, nothing: the draft's deadline went
    // by in a frame of its own long ago
    {OP_MOVE, T_EMPTY, 0, 0, 11000, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_CHECK, CHK_COMPOSEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0, "Say hello again, please. Thanks.\nline two"},
    // The context menu, each pick done by the box itself: Undo there,
    // nothing selected; then Select all, Cut, Paste, Undo and Redo
    RCLICK(500),
    {OP_CHECK, CHK_CMENU, 1|4|16, 1|16, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-menu"},
    CLICK(T_CMSELALL, "", 500),
    {OP_CHECK, CHK_SELALL, 0, 0, 0, ""},
    RCLICK(500),
    {OP_CHECK, CHK_CMENU, 4, 4, 0, ""},
    CLICK(T_CMCUT, "", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_CHECK, CHK_CLIPBOARD, 0, 0, 0, "Say hello again, please. Thanks.\nline two"},
    RCLICK(500),
    CLICK(T_CMPASTE, "", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello again, please. Thanks.\nline two"},
    RCLICK(500),
    CLICK(T_CMUNDO, "", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    RCLICK(500),
    {OP_CHECK, CHK_CMENU, 2, 2, 0, ""},
    CLICK(T_CMREDO, "", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Say hello again, please. Thanks.\nline two"},
    // The editor, by Ctrl+G: read-only meanwhile, saying so, then the
    // draft as it saved it, kept at once
    CTRLG(300),
    {OP_CHECK, CHK_EDITING, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-editing"},
    {OP_WAIT, W_EDITED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0,
     "Say hello again, please. Thanks.\nline two\nEdited by the test editor."},
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0,
     "Say hello again, please. Thanks.\nline two\nEdited by the test editor."},
    // An editor that fails, from the menu, leaves the draft alone, and
    // so does one cancelled
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 300, "exit 1 please"},
    RCLICK(500),
    CLICK(T_CMEDITOR, "", 300),
    {OP_WAIT, W_EDITED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "exit 1 please"},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "status 1"},
    {OP_SHOT, 0, 0, 0, 300, "compose-editfailed"},
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 300, "sleep please"},
    CTRLG(800),
    {OP_CHECK, CHK_EDITING, 0, 0, 0, ""},
    CLICK(T_EDITCANCEL, "", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "sleep please"},
    {OP_CHECK, CHK_NOTEDITING, 0, 0, 0, ""},
    // Slash commands: the popup while the first word is typed, Down to
    // pick, Tab to complete; /rename end to end
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 300, "/"},
    {OP_CHECK, CHK_SLASH, SLASH_COUNT-1, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-slash"},
    KEY(GKEY_DOWN, 0, 300),
    {OP_CHECK, CHK_SLASHSEL, 0, 0, 0, "/model"},
    {OP_TEXT, 0, 0, 0, 300, "re"},
    {OP_CHECK, CHK_SLASH, 1, 0, 0, ""},
    KEY(GKEY_TAB, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "/rename "},
    {OP_CHECK, CHK_SLASH, 0, 0, 0, ""},
    {OP_TEXT, 0, 0, 0, 200, "Session A"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "renamed"},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 500, ""},
    {OP_CHECK, CHK_TITLE, 0, 0, 0, "Session A"},
    // A skill of the workspace's (tests/gui.py) completes as a command
    // does
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 300, "/zzc"},
    {OP_CHECK, CHK_SLASH, 0, 0, 0, ""},
    {OP_CHECK, CHK_SLASHSEL, 0, 0, 0, "/zzcheck"},
    KEY(GKEY_TAB, 0, 300),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "/zzcheck "},
    // Not a command: a path is sent as text
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 200, "/usr/bin/foo is broken"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_TURNS, 3, 0, 10000, ""},
    {OP_CHECK, CHK_LASTUSER, 0, 0, 0, "/usr/bin/foo is broken"},
    // /compact with too little to compact: the reason, and the command
    // back in the box
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "/compact the parser"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "too little"},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "/compact the parser"},
    // /model alone lists the configured models; one picked switches
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 200, "/model"},
    KEY(GKEY_ENTER, 0, 600),
    {OP_CHECK, CHK_MODELS, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-models"},
    CLICK(T_MODEL0, "", 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "switched"},
    // /fork shows the new session, renamed here so that "Session A"
    // still names one; /export saves it as Markdown, as the menus do
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "/fork"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "forked from"},
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 300, ""},
    {OP_CHECK, CHK_TITLE, 0, 0, 0, "Session A (fork)"},
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "/rename Forked"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_CHECK, CHK_NOTE, 0, 0, 0, "renamed"},
    CLICK(T_SESSTITLE, "Session A", 500),
    CLICK(T_PROMPT, "", 200),
    SELALL,
    {OP_TEXT, 0, 0, 0, 200, "/export"},
    {OP_KEYDOWN, 0, GKEY_ENTER, 0, 50, ""},
    {OP_KEYUP, 0, GKEY_ENTER, 0, 500, ""},
    {OP_CHECK, CHK_MODAL, 0, 0, 0, ""},
    {OP_MOVE, T_NAMED, 0, 0, 100, "export save"},
    {OP_DOWN, 0, 0, 0, 60, ""},
    {OP_UP, 0, 0, 0, 500, ""},
    {OP_WAIT, W_FILE, 0, 0, 5000, "Session-A.md"},
    {OP_CHECK, CHK_EXPORTED, 0, 0, 0, "Session-A.md|# Session A\n"},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    CLICK(T_PROMPT, "", 200),
    // B: a draft of its own. Leaving each keeps its draft at once
    SELALL,
    {OP_TEXT, 0, 0, 0, 200, "Kept across a restart"},
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0, "Kept across a restart"},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_MARKSESS, 0, 0, 0, 0, ""},
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "/rename Session B"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_NOTE, 0, 0, 10000, ""},
    {OP_TEXT, 0, 0, 0, 200, "Draft for B"},
    CLICK(T_SESSTITLE, "Session A", 500),
    {OP_CHECK, CHK_DRAFTFILE, 0, 0, 0, "Draft for B"},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Kept across a restart"},
    {OP_SHOT, 0, 0, 0, 300, "compose-drafts"},
    CLICK(T_SESSTITLE, "Session B", 500),
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Draft for B"},
    // D: a steer sent while the response streams waits above the box
    // until withdrawn, back into the box, and never reaches the agent
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Stream in the background."},
    {OP_WAIT, W_STREAMING, 0, 0, 10000, ""},
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "Withdraw me"},
    KEY(GKEY_ENTER, 0, 600),
    {OP_CHECK, CHK_QUEUESHOWN, 1, 0, 0, "Withdraw me"},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-queued"},
    {OP_TEXT, 0, 0, 0, 200, "Draft for D"},
    CLICK(T_NAMED, "withdraw", 600),
    {OP_CHECK, CHK_QUEUESHOWN, 0, 0, 0, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Draft for D\nWithdraw me"},
    {OP_WAIT, W_IDLE, 0, 0, 20000, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    {OP_CHECK, CHK_LASTUSER, 0, 0, 0, "Stream in the background."},
    // C: enough of a conversation for /compact, with a focus
    {OP_NEWSESSION, 0, 0, 0, 0, ""},
    {OP_WAIT, W_SESSION, 0, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Answer at length."},
    {OP_WAIT, W_TURNS, 1, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Answer at length, again."},
    {OP_WAIT, W_TURNS, 2, 0, 10000, ""},
    {OP_PROMPT, 0, 0, 0, 200, "Answer at length, once more."},
    {OP_WAIT, W_TURNS, 3, 0, 10000, ""},
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 200, "/compact keep the API"},
    KEY(GKEY_ENTER, 0, 300),
    {OP_WAIT, W_COMPACTED, 0, 0, 20000, ""},
    {OP_CHECK, CHK_COMPACTED, 0, 0, 0, ""},
    {OP_SHOT, 0, 0, 0, 300, "compose-compacted"},
    // Quitting keeps a draft typed just before
    CLICK(T_PROMPT, "", 200),
    {OP_TEXT, 0, 0, 0, 100, "Draft for C"},
    // Filling the display counts as maximized. Maximized (zoomed) from
    // there, it does not go smaller, and it opens maximized again.
    {OP_MAXIMIZE, 0, 1, 0, 1500, ""},
    {OP_CHECK, CHK_MAXIMIZED, 1, 0, 0, ""},
    {OP_MAXIMIZE, 0, 0, 0, 1500, ""},
    {OP_CHECK, CHK_MAXIMIZED, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};

// After a restart: each draft back from its engine, as typed.
static DriveOp compose2ops[] = {
    {OP_WAIT, W_ENGINE, 0, 0, 10000, ""},
    {OP_WAIT, W_CONFIG, 0, 0, 10000, ""},
    {OP_CHECK, CHK_MAXIMIZED, 0, 0, 0, ""},
    {OP_MOVE, T_EMPTY, 0, 0, 1000, ""},
    CLICK(T_SESSTITLE, "Session A", 300),
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Kept across a restart"},
    CLICK(T_SESSTITLE, "Session B", 300),
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Draft for B"},
    CLICK(T_SESSTITLE, "Answer at length.", 300),
    {OP_WAIT, W_OPENED, 0, 0, 10000, ""},
    {OP_CHECK, CHK_PROMPT, 0, 0, 0, "Draft for C"},
    {OP_SHOT, 0, 0, 0, 300, "compose-restored"},
    {OP_MOVE, T_EMPTY, 0, 0, 6000, ""},
    {OP_MARK, 0, 0, 0, 3000, ""},
    {OP_CHECK, CHK_COMPOSEQUIET, 0, 0, 0, ""},
    {OP_CHECK, CHK_MAXIMIZED, 0, 0, 0, ""},
    {OP_SUMMARY, 0, 0, 0, 0, ""},
    {OP_QUIT, 0, 0, 0, 0, ""},
};
#undef KEY
#undef CLICK
#undef RCLICK
#undef SELALL
#undef CTRLG

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
    case T_ALWAYS:      return rectcenter(g->rects[RECT_ALWAYS]);
    case T_SETTING:     return rectcenter(g->rects[RECT_SETTING]);
    case T_ADDNAME:     return rectcenter(g->rects[RECT_ADDNAME]);
    case T_ADDBTN:      return rectcenter(g->rects[RECT_ADDBTN]);
    case T_CLOSESTOP:   return rectcenter(g->rects[RECT_CLOSESTOP]);
    case T_CLOSECANCEL: return rectcenter(g->rects[RECT_CLOSECANCEL]);
    case T_OTHERSESS:   return rectcenter(g->rects[RECT_OTHERSESS]);
    case T_THEMECOMBO:  return rectcenter(g->rects[RECT_THEMECOMBO]);
    case T_LONGFIELD:   return rectcenter(g->rects[RECT_LONGFIELD]);
    case T_TASKLINK:    return rectcenter(g->rects[RECT_TASKLINK]);
    case T_PARENTLINK:  return rectcenter(g->rects[RECT_PARENTLINK]);
    case T_MSGLINK:     return rectcenter(g->rects[RECT_MSGLINK]);
    case T_UNDO:        return rectcenter(g->rects[RECT_UNDO]);
    case T_REWINDTURN:  return rectcenter(g->rects[RECT_REWINDTURN]);
    case T_REWINDOK:    return rectcenter(g->rects[RECT_REWINDOK]);
    case T_FORCE:       return rectcenter(g->rects[RECT_FORCE]);
    case T_JOBKILL:     return rectcenter(g->rects[RECT_JOBKILL]);
    case T_INFOBTN:     return rectcenter(g->rects[RECT_INFOBTN]);
    case T_COMPACTHDR:  return rectcenter(g->rects[RECT_COMPACTHDR]);
    case T_THEMEITEM: {
        i32 i = themeparse(g->ops[g->drivepc-1].text);
        return i<0 ? v2(-1, -1) : rectcenter(g->rects[RECT_THEME0+i]);
    }
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
    case T_CLOSEBG:       return rectcenter(g->rects[RECT_CLOSEBG]);
    case T_CONNECT:       return rectcenter(g->rects[RECT_CONNECT]);
    case T_CONNECT2:      return rectcenter(g->rects[RECT_CONNECT2]);
    case T_HOSTCOMBO:     return rectcenter(g->rects[RECT_HOSTCOMBO]);
    case T_OUTPUT:        return rectcenter(g->rects[RECT_OUTPUT]);
    case T_REFRESHINC:    return rectcenter(g->rects[RECT_REFRESHINC]);
    case T_THUMB:         return rectcenter(g->rects[RECT_THUMB]);
    case T_SHOTCLOSE:     return rectcenter(g->rects[RECT_SHOTCLOSE]);
    case T_THINKHDR:      return rectcenter(g->rects[RECT_THINKHDR]);
    case T_USERMSG:       return rectcenter(g->rects[RECT_USERMSG]);
    case T_CODEBLOCK:     return rectcenter(g->rects[RECT_CODEBLOCK]);
    case T_CODECOPY:      return rectcenter(g->rects[RECT_CODECOPY]);
    case T_TOOLSBTN:      return rectcenter(g->rects[RECT_TOOLSBTN]);
    case T_THINKBTN:      return rectcenter(g->rects[RECT_THINKBTN]);
    case T_GAUGE:         return rectcenter(g->rects[RECT_GAUGE]);
    case T_PAGECOMBO:     return rectcenter(g->rects[RECT_PAGECOMBO]);
    case T_DELETE:        return rectcenter(g->rects[RECT_DELETE]);
    case T_DELOK:         return rectcenter(g->rects[RECT_DELOK]);
    case T_DELCANCEL:     return rectcenter(g->rects[RECT_DELCANCEL]);
    case T_ROOTS:         return rectcenter(g->rects[RECT_ROOTS]);
    case T_SPLITFROM:     return v2(g->splitx, targetpos(g, T_SPLITTER).y);
    case T_PAGEITEM:
    case T_ADDOPEN: {
        Str text = g->ops[g->drivepc-1].text;
        if (!g->cfgtwopane && !g->cfgcombo) {
            return rectcenter(g->rects[RECT_PAGECOMBO]);
        } else if (t==T_PAGEITEM && text=="diagnostics") {
            return rectcenter(g->rects[RECT_DIAGPAGE]);
        } else if (t == T_ADDOPEN) {
            i32 n = 0;
            for (Json *k = jsonget(g->cfg, "kinds")->child; k; k = k->next) {
                if (!jsonint(jsonget(k, "named"), 0)) {
                    continue;
                } else if (jsonstr(jsonget(k, "kind")) == text) {
                    return n<4 ? rectcenter(g->rects[RECT_ADDOPEN+n]) : v2(-1, -1);
                }
                n++;
            }
            return v2(-1, -1);
        }
        Arena a = g->frame;
        for (iz i = 0; i < g->cfgrows.len; i++) {
            CfgRow *r = &g->cfgrows[i];
            if (cfgpagekey(&a, r->kind, r->name) == text) {
                return rectcenter(r->page);
            }
        }
        return v2(-1, -1);
    }
    case T_SESSMENU:      return rectcenter(g->rects[RECT_SESSMENU]);
    case T_ZOOMRESET:     return rectcenter(g->rects[RECT_ZOOMRESET]);
    case T_CMUNDO:        return rectcenter(g->rects[RECT_CMUNDO]);
    case T_CMREDO:        return rectcenter(g->rects[RECT_CMREDO]);
    case T_CMCUT:         return rectcenter(g->rects[RECT_CMCUT]);
    case T_CMCOPY:        return rectcenter(g->rects[RECT_CMCOPY]);
    case T_CMPASTE:       return rectcenter(g->rects[RECT_CMPASTE]);
    case T_CMSELALL:      return rectcenter(g->rects[RECT_CMSELALL]);
    case T_CMEDITOR:      return rectcenter(g->rects[RECT_CMEDITOR]);
    case T_EDITCANCEL:    return rectcenter(g->rects[RECT_EDITCANCEL]);
    case T_MODEL0:        return rectcenter(g->rects[RECT_MODEL0]);
    case T_NAMED: {
        Str text = g->ops[g->drivepc-1].text;
        for (i32 i = 0; i < g->nnamed; i++) {
            Named *n = &g->named[i];
            if (startswith(Str{n->name, n->len}, text)) {
                return rectcenter(n->r);
            }
        }
        return v2(-1, -1);
    }
    case T_PANEITEM:
        for (i32 i = 0; i < PANE_COUNT; i++) {
            if (g->ops[g->drivepc-1].text == inistr(panenames[i])) {
                return rectcenter(g->rects[RECT_PANE0+i]);
            }
        }
        return v2(-1, -1);
    case T_HOSTITEM:
        for (i32 i = 0; i < countof(g->hostitems); i++) {
            GuiHost *h = g->hostitems[i];
            if (h && h->name==g->ops[g->drivepc-1].text) {
                return rectcenter(g->rects[RECT_HOST0+i]);
            }
        }
        return v2(-1, -1);
    case T_HOSTSTAB:      return tabcenter("Hosts");
    case T_HELPTAB:       return tabcenter("Help");
    case T_TRANSCRIPTTAB: return tabcenter("Transcript");
    case T_SETTINGSTAB:   return tabcenter("Settings");
    case T_JOBSTAB:       return tabcenter("Jobs");
    case T_LOGTAB:        return tabcenter("Log");
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

// The compose box's text, the session shown's draft.
static Str promptstr(Gui *g)
{
    return draftstr(g->sess ? g->sess : &g->nosession);
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

// A subagent of the session shown, the latest made, that asks if any
// does; null if none.
static GuiSession *drivekid(Gui *g, b32 asking)
{
    GuiSession *r = 0;
    for (GuiSession *s = g->sessions; g->sess && s; s = s->next) {
        View *k = s->view;
        if (k && k->parent==g->sess->sid && (!asking || k->asks) &&
                (!r || compare(s->sid, r->sid)>0)) {
            r = s;
        }
    }
    return r;
}

// The latest message of a role in a view, or null.
static ViewMsg *lastof(View *v, i32 role)
{
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        if (v->msgs[i].role==role && !v->msgs[i].prov) {
            return &v->msgs[i];
        }
    }
    return 0;
}

// Whether every tool row the user did not choose is folded (want 1),
// open (0), or as tools = auto says (-1).
static b32 toolsas(View *v, i32 want)
{
    i32 rows = 0;
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role!=VIEW_TOOL || m->chosen || m->prov) {
            continue;
        }
        rows++;
        b32 f = want<0 ? toolauto(m) : want;
        if (m->collapsed != f) {
            return 0;
        }
    }
    return rows > 0;
}

// Whether every reasoning the user did not choose is folded (want 1) or
// open (0), with none hidden.
static b32 thinkas(View *v, b32 want)
{
    i32 rows = 0;
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role!=VIEW_THINK || m->prov) {
            continue;
        }
        rows++;
        if (msghidden(v, m) || (!m->chosen && m->collapsed!=want)) {
            return 0;
        }
    }
    return rows > 0;
}

// Whether the current find hit's row is open, as chosen, with its line
// in the transcript's window.
static b32 hitshown(Gui *g)
{
    ViewSearch *s  = g->search;
    View       *v  = g->view;
    GuiSession *gs = g->sess;
    if (!gs || !gs->win || s->v!=v || s->cur<0 || s->cur>=s->hits.len) {
        return 0;
    }
    FindHit *h = &s->hits[s->cur];
    ViewMsg *m = &v->msgs[h->msg];
    if (m->collapsed || !m->chosen) {
        return 0;
    }
    MsgLayout *ml = msglayout(v, h->msg, &g->measure, v->lastw, g->scale);
    f64        y  = m->top + v->head + msgy(msgdoc(v, h->msg), ml, h->off);
    f32        sy = gs->win->Scroll.y;
    return y>=sy && y<sy+gs->win->Size.y;
}

// Frames since the mark: the probe's, a deadline's frame and its
// settling, and the overlay's numbers if shown.
static b32 drivequiet(Gui *g, Str what, i64 allow)
{
    i64   n     = g->total.frames - g->driveframes;
    i64   us    = g->sched.now - g->markat;
    i64   other = (g->metricsshown ? 4*us/1000000 : 0) + allow;
    Arena a     = g->frame;
    Buf   b(&a, 128);
    print(&b, "drive: frames ");
    print(&b, what);
    print(&b, ": ");
    print(&b, n);
    print(&b, " in ");
    printfloat(&b, (f64)us/1e6, 1);
    print(&b, " s");
    guilog(g, finish(&b));
    return n <= other;
}

// The latest reasoning in a view, provisional or not, or null.
static ViewMsg *lastthink(View *v)
{
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        if (v->msgs[i].role == VIEW_THINK) {
            return &v->msgs[i];
        }
    }
    return 0;
}

// Whether the view streams reasoning now.
static b32 thinking(View *v)
{
    return v->busy && v->stream>=0 && v->streamkind==VIEW_THINK &&
           v->msgs[v->stream].chars>0;
}

static i32 countrole(View *v, i32 role)
{
    i32 n = 0;
    for (iz i = 0; i < v->msgs.len; i++) {
        n += v->msgs[i].role == role;
    }
    return n;
}

// The first host of a kind, another process's when local.
static GuiHost *drivehost(Gui *g, i32 kind)
{
    for (GuiHost *h = g->hosts; h; h = h->next) {
        if (h->kind==kind && !h->own) {
            return h;
        }
    }
    return 0;
}

// The thumbnail slots of the session shown: how many are ready, and
// how many are still to come.
struct ImgCount {
    i32 ready;
    i32 waiting;
    i32 failed;
};

static ImgCount imgcount(Gui *g)
{
    ImgCount r = {};
    for (i32 i = 0; i < IMG_SLOTS; i++) {
        ImgSlot *sl = g->img.slots + i;
        if (sl->s && sl->s==g->sess) {
            r.ready   += sl->state == IMG_READY;
            r.waiting += sl->state==IMG_WANT || sl->state==IMG_FETCH;
            r.failed  += sl->state == IMG_FAILED;
        }
    }
    return r;
}

static b32 drivewaitdone(Gui *g)
{
    View *v = g->view;
    switch (g->drivewait) {
    case W_THUMBS: {
        ImgCount n = imgcount(g);
        return g->sess && !n.waiting && n.ready+n.failed>0 && g->thumbsdrawn>0;
    }
    case W_SHOT:
        return g->img.view.state==IMG_READY || g->img.view.state==IMG_FAILED;
    case W_STREAMING:
        return g->sess && v->state==LIVE_STREAMING && v->deltas>0;
    case W_THINKING:
        return g->sess && thinking(v) && (v->thinking==THINK_HIDE || g->thinkchars>0);
    case W_THINKSTALL:
        return g->sess && thinking(v) && os_time(g->os)-v->lastdata>=700;
    case W_TURNS:
        return g->sess && !v->busy && v->turn>=(i32)g->ops[g->drivepc-1].dx;
    case W_EDITED:
        return !g->edsess;
    case W_NOTE:
        return g->sess && g->sess->note.len;
    case W_COMPACTED:
        return g->sess && !v->busy && lastof(v, VIEW_COMPACT);
    case W_IMPORTED:
        return g->imp.imports && g->imp.state==IMP_IDLE;
    case W_EXPORTED:
        return g->exp.saves && g->exp.state==EXP_IDLE && !g->exp.want;
    case W_NAMED:
        for (i32 i = 0; i < g->nnamed; i++) {
            Named *n = &g->named[i];
            if (startswith(Str{n->name, n->len}, g->ops[g->drivepc-1].text)) {
                return 1;
            }
        }
        return 0;
    case W_RAISED:
        return !g->bg && !g->raisereq && g->raises>=(i32)g->ops[g->drivepc-1].dx;
    case W_SHED:
        return g->bg;
    case W_REOPENED:
        return !g->bg && g->main->own && g->main->up && g->sess &&
               g->sess->host==g->main && !g->sess->openid;
    case W_BGHOST: {
        GuiHost *h = drivehost(g, GH_LOCAL);
        b32      any = 0;
        for (GuiSession *s = g->sessions; h && s; s = s->next) {
            any |= s->host==h && s->listed;
        }
        return h && h->up && any;
    }
    case W_REMOTEUP: {
        GuiHost *h = drivehost(g, GH_REMOTE);
        return h && h->up;
    }
    case W_REMOTEDOWN: {
        GuiHost *h = drivehost(g, GH_REMOTE);
        return h && h->state==RS_DOWN;
    }
    case W_REMOTECLOSED: {
        GuiHost *h = drivehost(g, GH_REMOTE);
        return h && !h->r && h->state==RS_CLOSED;
    }
    case W_OUTPUT:
        return g->out.s && !g->out.id && (g->out.text.len || g->out.err.len);
    case W_PLAN: {
        PlanFile *f = &g->plan;
        return g->sess && f->s==g->sess && !f->id && f->ver==v->planver &&
               find(finish(&f->text), g->ops[g->drivepc-1].text)>=0;
    }
    case W_FILE: {
        Arena a = g->frame;
        Str   f = join(&a, g->drivecwd, g->ops[g->drivepc-1].text);
        return os_stat(g->os, a, f).type == STAT_FILE;
    }
    case W_REFRESHED:
        return g->cfgrefreshed>0 && !g->cfgrefresh;
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
    case W_GONE:
        return g->enginegone;
    case W_TASKS:
        return g->sess && v->state==LIVE_TASK && v->tasks==2;
    case W_KIDASK:
        return drivekid(g, 1) != 0;
    case W_TODO:
        return v->todos.len==3 && v->todos[0].status==VTODO_ACTIVE;
    case W_SAID: {
        ViewMsg *m = lastof(v, VIEW_ASSISTANT);
        return g->sess && !v->busy && m && startswith(m->text, g->ops[g->drivepc-1].text);
    }
    case W_JOBS: {
        // The first may end after the turn, which then resumes for it
        ViewJob *a = findjob(v, 1), *b = findjob(v, 2);
        return g->sess && !v->busy && a && a->status!=VJOB_RUNNING && b &&
               b->status==VJOB_RUNNING && find({b->tail, b->taillen}, "tick 3")>=0;
    }
    case W_REWOUND:
        return !g->rewindid && countrole(v, VIEW_REWIND)>=g->rewsent;
    case W_COMPACTING:
        return g->sess && v->state==LIVE_COMPACTING;
    case W_QUEUED:
        return g->sess && v->state==LIVE_QUEUED;
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

// Whether the theme in effect is the palette's, ImGui's colors too.
static b32 themeshown(Gui *g, Palette *p)
{
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    return g->pal==p && ImGui::ColorConvertFloat4ToU32(bg)==p->panel;
}

// The settings row whose value is longer than a field's slack, or null.
static CfgRow *longrow(Gui *g)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        if (g->cfgrows[i].value.len >= FIELD_MAX) {
            return &g->cfgrows[i];
        }
    }
    return 0;
}

// Whether a path ends with the last two components of another, a slash
// matching a backslash: what the host spells above them may differ (a
// real path, Windows backslashes, XP's long names for 8.3 ones).
static b32 pathendswith(Str path, Str tail)
{
    for (iz i = tail.len-1, n = 0; i >= 0; i--) {
        if ((tail[i]=='/' || tail[i]=='\\') && ++n==2) {
            tail = cuthead(tail, i);
            break;
        }
    }
    if (tail.len > path.len) {
        return 0;
    }
    Str end = taketail(path, tail.len);
    for (iz i = 0; i < tail.len; i++) {
        u8 a = end[i]=='\\' ? (u8)'/' : end[i];
        u8 b = tail[i]=='\\' ? (u8)'/' : tail[i];
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

// The checklist's record of what the GUI sets on the clipboard, for
// window systems that may refuse a selection made by synthetic input
// (GUI_NOSERIAL): the backend's own setter, wrapped (drivestep).
static Gui  *clipgui;
static void (*clipset)(ImGuiContext *, char const *);

static void driveclipset(ImGuiContext *c, char const *text)
{
    Gui *g = clipgui;
    if (g) {
        g->driveclip = clone(&g->perm, Str{(u8 *)text, (iz)__builtin_strlen(text)});
    }
    if (clipset) {
        clipset(c, text);
    }
}

// The clipboard's text for a check. Where the window system takes a
// selection only with a real input's serial, newer than the last one's
// (Wayland), the checklist's synthetic clicks and keys have none, so it
// may refuse the GUI's: then what the GUI set there, which a real
// user's input would have put on it, as said in the log.
static Str driveclipboard(Gui *g, Str want)
{
    char const *c   = ImGui::GetClipboardText();
    Str         got = c ? Str{(u8 *)c, (iz)__builtin_strlen(c)} : Str{};
    if (!(got == want) && (os_guilimits(g->os) & GUI_NOSERIAL) && g->driveclip==want) {
        guilog(g, "drive: the window system refused the selection made by synthetic "
                  "input (no real input's serial): checking what the GUI set");
        return g->driveclip;
    }
    return got;
}

// The compose box's checks: what each found, logged, with what it
// should have been when not.
static b32 composecheck(Gui *g, i32 chk, DriveOp *op)
{
    Arena       a    = g->frame;
    GuiSession *s    = g->sess ? g->sess : &g->nosession;
    View       *v    = g->view;
    Str         want = op->text;
    Str         got  = {};
    b32         ok   = 0;
    switch (chk) {
    case CHK_PROMPT:
        got = promptstr(g);
        ok  = got == want;
        break;
    case CHK_HIST: {
        Buf b(&a, 32);
        if (s->hist) {
            i32 m = histcount(v);
            print(&b, (i64)(m - s->hist + 1));
            putbyte(&b, '/');
            print(&b, (i64)m);
        }
        got = finish(&b);
        ok  = got==want && (!s->hist || g->histshown==s->hist);
    } break;
    case CHK_DRAFTFILE: {
        GuiSession *m = g->drivesess;
        if (m) {
            Str path = join(&a, sessdir(g, &a, m->sid), "draft");
            got = os_read(g->os, &a, path, DRAFT_MAX).data;
        }
        ok = m && got==want;
    } break;
    case CHK_CARET: {
        ImGuiInputTextState *st = ImGui::GetInputTextState(s->draftid);
        Str text = promptstr(g);
        iz  nl   = 0;
        while (nl<text.len && text[nl]!='\n') nl++;
        i64 at = st ? st->GetCursorPos() : -1;
        Buf b(&a, 24);
        print(&b, at);
        got = finish(&b);
        ok  = st && (want=="0"      ? at==0 :
                     want=="end"    ? at==text.len :
                     want=="first"  ? at>0 && at<=nl :
                     want=="second" ? at>nl : 0);
    } break;
    case CHK_CMENU: {
        u32 mask = (u32)op->dx;
        Buf b(&a, 24);
        print(&b, (i64)(g->cmenustate & mask));
        got = finish(&b);
        ok  = g->cmenushown && (g->cmenustate & mask)==(u32)op->dy;
    } break;
    case CHK_SELALL: {
        ImGuiInputTextState *st = ImGui::GetInputTextState(s->draftid);
        iz n = promptstr(g).len;
        ok = st && n>0 && st->GetSelectionStart()==0 && st->GetSelectionEnd()==n;
    } break;
    case CHK_CLIPBOARD:
        got = driveclipboard(g, want);
        ok  = got == want;
        break;
    case CHK_NOTE:
        got = s->note;
        ok  = got.len && find(got, want)>=0;
        break;
    case CHK_SLASH: {
        // Commands only: the user's own skills may be listed too
        i32 n = slashcount(slashmatches(promptstr(g)));
        n = g->slashshown ? n : 0;
        Buf b(&a, 24);
        print(&b, (i64)n);
        got = finish(&b);
        ok  = n == (i32)op->dx;
    } break;
    case CHK_SLASHSEL: {
        Slice<SkillItem> sk = g->sess ? g->sess->skills : Slice<SkillItem>{};
        Slice<i32>       p  = slashpicks(&a, promptstr(g), sk);
        got = g->slashsel<p.len ? slashpickinfo(&a, p[g->slashsel], sk).name : Str{};
        ok  = g->slashshown && got==want;
    } break;
    case CHK_TITLE:
        got = v->title;
        ok  = got == want;
        break;
    case CHK_LASTUSER: {
        ViewMsg *m = lastmsg(g, VIEW_USER);
        got = m ? m->text : Str{};
        ok  = got == want;
    } break;
    }
    Buf b(&a, 256 + got.len);
    print(&b, "prospero: drive: ");
    print(&b, chknames[chk]);
    print(&b, ": [");
    print(&b, takehead(got, got.len<200 ? got.len : 200));
    putbyte(&b, ']');
    if (!ok) {
        print(&b, ", not [");
        print(&b, want);
        putbyte(&b, ']');
    }
    putbyte(&b, '\n');
    os_print(g->os, 2, finish(&b));
    return ok;
}

static b32 drivecheck(Gui *g, i32 chk)
{
    ImGuiContext &c = *GImGui;
    View         *v = g->view;
    switch (chk) {
    case CHK_THUMBS: {
        // Both of the session's screenshots, as their records say
        ImgCount n     = imgcount(g);
        i32      shots = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            shots += m->role==VIEW_TOOL && m->tool->image.len && m->tool->imgw>0;
        }
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: screenshots ");
        print(&b, (i64)shots);
        print(&b, ", thumbnails ready ");
        print(&b, (i64)n.ready);
        print(&b, ", failed ");
        print(&b, (i64)n.failed);
        print(&b, ", drawn ");
        print(&b, (i64)g->thumbsdrawn);
        print(&b, "; PNGs decoded ");
        print(&b, g->img.decodes);
        print(&b, " in ");
        printfloat(&b, (f64)g->img.decodeus/1000, 1);
        print(&b, " ms");
        guilog(g, finish(&b));
        return shots==2 && n.ready==2 && !n.failed && g->thumbsdrawn==2;
    }
    case CHK_SHOTOPEN: {
        ImgView *iv = &g->img.view;
        return iv->open && iv->state==IMG_READY && g->shotshown && iv->tex &&
               iv->tex->td.Width==iv->w && iv->tex->td.Height==iv->h &&
               ImGui::FindWindowByName("Screenshot") &&
               ImGui::FindWindowByName("Screenshot")->Active;
    }
    case CHK_SHOTCLOSED: {
        ImGuiWindow *w = ImGui::FindWindowByName("Screenshot");
        return !g->img.view.open && !g->img.view.tex && !g->img.dying && (!w || !w->Active);
    }
    case CHK_THINKFOLD:
    case CHK_THINKGREW: {
        // One header line, its count and clock as last drawn
        ViewMsg *m   = lastthink(v);
        b32      ok  = m && m->prov && m->collapsed && thinking(v) && g->thinkrows>0 &&
                       g->thinkchars==m->chars && g->thinksecs>=0;
        b32      grew = g->thinkchars > g->thinkchars0;
        Arena    a   = g->frame;
        Buf      b(&a, 96);
        print(&b, "drive: reasoning folded: ");
        print(&b, g->thinkchars);
        print(&b, " characters, ");
        print(&b, g->thinksecs);
        print(&b, " s");
        guilog(g, finish(&b));
        g->thinkchars0 = g->thinkchars;
        g->thinksecs0  = g->thinksecs;
        return ok && (chk==CHK_THINKFOLD || grew);
    }
    case CHK_THINKQUIET: {
        // Its clock and the turn's tick on one deadline a second, a
        // frame and its settling, plus the probe's: never 30 a second
        // for a count that does not change. Windows draws about five a
        // second (the 15 s amber wait shows the same), which the metrics
        // overlay, shown by default before, used to hide in its slack
        i64   n     = g->total.frames - g->driveframes;
        i64   us    = g->sched.now - g->markat;
        i64   other = (g->metricsshown ? 4*us/1000000 : 0) + 3 +
                      3*(g->saves - g->marksaves);  // a save's, and a probe after
        ViewMsg *m  = lastthink(v);
        Arena a     = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: frames while reasoning paused: ");
        print(&b, n);
        print(&b, " in ");
        printfloat(&b, (f64)us/1e6, 1);
        print(&b, " s, its clock from ");
        print(&b, g->thinksecs0);
        print(&b, " to ");
        print(&b, g->thinksecs);
        print(&b, " s");
        guilog(g, finish(&b));
        return m && m->prov && m->collapsed && g->thinksecs>g->thinksecs0 &&
               n <= 6*us/1000000 + other;
    }
    case CHK_THINKOPENED: {
        ViewMsg *m = lastthink(v);
        return m && m->prov && !m->collapsed && m->chosen && m->measured && m->h>0;
    }
    case CHK_THINKKEPT: {
        ViewMsg *m = lastthink(v);
        return m && !m->prov && m->seq && !m->collapsed && m->chosen && m->measured &&
               m->h>0 && !v->busy;
    }
    case CHK_THINKHIDDEN:
        return thinking(v) && v->thinking==THINK_HIDE && g->thinkhidshown && !g->thinkrows;
    case CHK_THINKNONE: {
        ViewMsg *m = lastthink(v);
        return m && !m->prov && !v->busy && countrole(v, VIEW_THINK)>=2 && !g->thinkrows;
    }
    case CHK_THINKOPEN: {
        ViewMsg *m = lastthink(v);
        return m && m->prov && thinking(v) && !m->collapsed && !m->chosen && m->measured &&
               m->h>0 && g->thinkrows>0;
    }
    case CHK_THINKSTAYS: {
        ViewMsg *m = lastthink(v);
        return m && !m->prov && !v->busy && !m->collapsed && !m->chosen && g->thinkrows>0;
    }
    case CHK_THINKREFOLD: {
        // The one opened by a click stays open, the rest fold
        i32 opened = 0, wrong = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            if (m->role == VIEW_THINK) {
                opened += m->chosen && !m->collapsed;
                wrong  += !m->chosen && !m->collapsed;
            }
        }
        return opened==1 && !wrong && g->thinkrows>0;
    }
    case CHK_COPYBTN:
        return g->rects[RECT_CODECOPY].x1 > g->rects[RECT_CODECOPY].x0;
    case CHK_CODECOPIED: {
        // The code without its last newline, as a triple click copies it
        Str want = g->drivecode;
        want = want.len && want[want.len-1]=='\n' ? cuttail(want, 1) : want;
        return want.len && driveclipboard(g, want)==want && g->copiedshown;
    }
    case CHK_COPYQUIET:
        // Copied back to Copy at its deadline: that frame and its settling,
        // the probe's, and the checklist's own at the mark and the check
        return !g->copiedshown && drivequiet(g, "after Copied", 5);
    case CHK_REFLOW: {
        GuiSession *gs  = g->sess;
        Anchor      was = g->driveanchor;
        if (!gs || !gs->win || was.msg<0) {
            return 0;
        }
        Anchor now = viewanchor(v, &g->measure, v->lastw, g->scale, v->head, gs->win->Scroll.y);
        b32    ok  = now.msg==was.msg && now.dy-was.dy<1.5 && was.dy-now.dy<1.5;
        if (ok && was.off>=0) {
            Doc       *d  = msgdoc(v, now.msg);
            MsgLayout *ml = msglayout(v, now.msg, &g->measure, v->lastw, g->scale);
            ok = now.off>=0 && msgy(d, ml, now.off)==msgy(d, ml, was.off);
        }
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: the top edge at width ");
        print(&b, (i64)v->lastw);
        print(&b, ": message ");
        print(&b, (i64)now.msg);
        print(&b, ", offset ");
        print(&b, (i64)now.off);
        print(&b, " for ");
        print(&b, (i64)was.off);
        print(&b, ", ");
        printfloat(&b, now.dy, 1);
        print(&b, " px into its line for ");
        printfloat(&b, was.dy, 1);
        guilog(g, finish(&b));
        return ok;
    }
    case CHK_TOOLSOPEN:  return g->toolset==TOOLS_OPEN && toolsas(v, 0);
    case CHK_TOOLSFOLD:  return g->toolset==TOOLS_FOLD && toolsas(v, 1);
    case CHK_TOOLSAUTO:  return g->toolset==TOOLS_AUTO && !v->showtools && toolsas(v, -1);
    case CHK_SHOWTOOLS:  return g->toolset==TOOLS_AUTO && v->showtools && toolsas(v, 0);
    case CHK_SHOWTHINK:  return g->thinkset==THINK_FOLD && v->showthink && thinkas(v, 0);
    case CHK_THINKBACK:  return !v->showthink && thinkas(v, 1);
    case CHK_FINDOPEN:   return g->findopen && g->findactive;
    case CHK_FOUND:
        g->drivefound = g->search->cur;
        return g->search->hits.len==2 && v->msgs[g->search->hits[0].msg].role==VIEW_THINK &&
               hitshown(g);
    case CHK_FOUNDNEXT: {
        ViewSearch *s = g->search;
        b32 both = s->hits.len==2 && v->msgs[s->hits[0].msg].chosen &&
                   v->msgs[s->hits[1].msg].chosen;
        return both && s->cur==(g->drivefound+1)%2 && hitshown(g);
    }
    case CHK_FOUNDPREV:
        return g->search->cur==g->drivefound && hitshown(g);
    case CHK_FINDQUIET:
        return g->findopen && !g->findactive && g->search->hits.len==2 &&
               drivequiet(g, "with the find bar's matches shown", 3);
    case CHK_FINDHIDDEN:
        return g->findopen && !g->search->hits.len;
    case CHK_FINDCLOSED:
        return !g->findopen && !g->findactive && !v->busy;
    case CHK_GAUGEREAL: {
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: the gauge: ");
        print(&b, v->tokens);
        print(&b, " tokens, compacting at ");
        print(&b, v->compactat);
        print(&b, ", window ");
        print(&b, v->ctx);
        guilog(g, finish(&b));
        return g->gaugeshown==1 && v->ctx>0 && v->compactat>0 && v->tokens>0;
    }
    case CHK_GAUGE:      return g->gaugeshown == 1;
    case CHK_GAUGEAMBER: return g->gaugeshown == 2;
    case CHK_IMGQUIET: {
        // The probe's, and the overlay's numbers if shown
        i64   n     = g->total.frames - g->driveframes;
        i64   us    = g->sched.now - g->markat;
        i64   other = (g->metricsshown ? 4*us/1000000 : 0) + 2 +
                      3*(g->saves - g->marksaves);  // a save's, and a probe after
        Arena a     = g->frame;
        Buf   b(&a, 96);
        print(&b, "drive: frames with screenshots shown: ");
        print(&b, n);
        print(&b, " in ");
        printfloat(&b, (f64)us/1e6, 1);
        print(&b, " s");
        guilog(g, finish(&b));
        return n <= other;
    }
    case CHK_MENU: {
        // A popup of the menu bar's, which a click opened
        ImGuiWindow *w = c.OpenPopupStack.Size ? c.OpenPopupStack[0].Window : 0;
        return w && (w->Flags & ImGuiWindowFlags_ChildMenu) && w->Active;
    }
    case CHK_PANECLOSED:
    case CHK_PANEOPEN: {
        Str name = g->ops[g->drivepc-1].text;
        i32 pane = -1;
        for (i32 i = 0; i < PANE_COUNT; i++) {
            pane = name==inistr(panenames[i]) ? i : pane;
        }
        if (pane < 0) {
            return 0;
        }
        ImGuiWindow *w = ImGui::FindWindowByName(panenames[pane]);
        if (chk == CHK_PANEOPEN) {
            return g->panes[pane] && w && w->Active && (!w->DockNode || w->DockTabIsVisible);
        }
        uz   len = 0;
        char const *ini = ImGui::SaveIniSettingsToMemory(&len);
        Str  want = concat(&g->frame, concat(&g->frame, "\n", name), "=0\n");
        return !g->panes[pane] && (!w || !w->Active) &&
               find({(u8 *)ini, (iz)len}, want) >= 0;
    }
    case CHK_ZOOM: {
        Int   pct  = parseint(g->ops[g->drivepc-1].text);
        f32   want = (f32)pct.val / 100;
        Arena a    = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: zoom ");
        print(&b, (i64)(g->zoom*100 + 0.5f));
        print(&b, "%, text ");
        printfloat(&b, g->fonts.size, 2);
        print(&b, " px, ImGui's ");
        printfloat(&b, ImGui::GetStyle().FontSizeBase*ImGui::GetStyle().FontScaleMain*
                       ImGui::GetStyle().FontScaleDpi, 2);
        guilog(g, finish(&b));
        ImGuiStyle &st = ImGui::GetStyle();
        return g->zoom==want && st.FontScaleMain==want &&
               g->fonts.size==g->fontbase*want*g->scale && !g->fontdirty;
    }
    case CHK_ZOOMQUIET: {
        i64 n = g->total.frames - g->driveframes;
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: ");
        print(&b, n);
        print(&b, " frames in 2 s after the zoom");
        guilog(g, finish(&b));
        return n <= 2;  // the probe's, and the script's own
    }
    case CHK_PLACED: {
        // Wholly on the primary display, centered across it
        WinBox  disp[16];
        i32     n = os_guidisplays(g->os, disp, countof(disp));
        WinGeom w = os_guiwhere(g->os);
        Arena   a = g->frame;
        Buf     b(&a, 128);
        print(&b, "drive: the window at ");
        print(&b, (i64)w.box.x);
        putbyte(&b, ',');
        print(&b, (i64)w.box.y);
        print(&b, ", ");
        print(&b, (i64)w.box.w);
        putbyte(&b, 'x');
        print(&b, (i64)w.box.h);
        print(&b, "; the primary display ");
        if (n) {
            print(&b, (i64)disp[0].x);
            putbyte(&b, ',');
            print(&b, (i64)disp[0].y);
            print(&b, ", ");
            print(&b, (i64)disp[0].w);
            putbyte(&b, 'x');
            print(&b, (i64)disp[0].h);
        }
        guilog(g, finish(&b));
        if (!n || !w.ok) {
            return 0;
        } else if (os_guilimits(g->os) & GUI_NOPLACE) {
            // The compositor's to place (Wayland): the size fits
            guilog(g, "drive: the window system places windows: checking the size alone");
            return w.box.w<=disp[0].w && w.box.h<=disp[0].h;
        }
        i64 cx = 2*(i64)w.box.x + w.box.w, dx = 2*(i64)disp[0].x + disp[0].w;
        return boxoverlap(w.box, disp[0]) == (i64)w.box.w*w.box.h &&
               cx-dx<=4 && dx-cx<=4 && g->geom.box.x==w.box.x && g->geom.box.y==w.box.y;
    }
    case CHK_PROMPT:
    case CHK_HIST:
    case CHK_DRAFTFILE:
    case CHK_CARET:
    case CHK_CMENU:
    case CHK_SELALL:
    case CHK_CLIPBOARD:
    case CHK_NOTE:
    case CHK_SLASH:
    case CHK_SLASHSEL:
    case CHK_TITLE:
    case CHK_LASTUSER:
        return composecheck(g, chk, &g->ops[g->drivepc-1]);
    case CHK_COMPOSEQUIET: {
        i64 n = g->total.frames - g->driveframes;
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: ");
        print(&b, n);
        print(&b, " frames in 3 s, the box idle");
        guilog(g, finish(&b));
        return n <= 2 + 3*(g->saves - g->marksaves);  // the probe's, the script's, a save's
    }
    case CHK_EDITING:
        return g->sess && g->edsess==g->sess && g->editshown;
    case CHK_NOTEDITING:
        return !g->edsess && !g->edproc;
    case CHK_MODELS:
        return g->modelsshown;
    case CHK_COMPACTED:
        return lastof(v, VIEW_COMPACT) != 0;
    case CHK_SIDE:
    case CHK_NOSIDE:
    case CHK_SIDEINI:
    case CHK_LASTERR: {
        Str text = g->ops[g->drivepc-1].text;
        Str in   = {g->sidedump, g->sidelen};
        if (chk == CHK_SIDEINI) {
            uz len = 0;
            char const *ini = ImGui::SaveIniSettingsToMemory(&len);
            in = {(u8 *)ini, (iz)len};
        } else if (chk == CHK_LASTERR) {
            in = g->lasterr;
        }
        b32 found = find(in, text) >= 0;
        if ((chk==CHK_NOSIDE) == found) {
            Arena a = g->frame;
            guilog(g, concat(&a, "drive: the sidebar:\n", Str{g->sidedump, g->sidelen}));
            guilog(g, concat(&a, "drive: the latest error: ", g->lasterr));
        }
        return (chk==CHK_NOSIDE) != found;
    }
    case CHK_MODEL:
        return g->sess && startswith(g->view->model, g->ops[g->drivepc-1].text);
    case CHK_EXPORTED: {
        // "FILE|NEEDLE": the file in --cwd holds the needle
        Arena a    = g->frame;
        Cut   c    = cut(g->ops[g->drivepc-1].text, '|');
        Read  rd   = os_read(g->os, &a, join(&a, g->drivecwd, c.head), (iz)1<<24);
        b32   ok   = rd.err==READ_OK && find(rd.data, c.tail)>=0;
        if (!ok) {
            guilog(g, concat(&a, "drive: export status: ", g->exp.status));
        }
        return ok;
    }
    case CHK_HOSTTRIED: {
        GuiHost *h = findhost(g, GH_REMOTE, g->ops[g->drivepc-1].text);
        return h && (h->r || h->state==RS_FAILED || h->opens || h->downs);
    }
    case CHK_DELCOUNT:
        return (f32)g->delcount == g->ops[g->drivepc-1].dx;
    case CHK_FORKED:
        return g->sess && endswith(sesstitle(g->sess), "(fork)") &&
               draftstr(g->sess)==g->ops[g->drivepc-1].text;
    case CHK_GEOMKEPT: {
        WinGeom w = os_guiwhere(g->os);
        WinGeom m = g->geommark;
        return w.ok && m.ok && w.box.x==m.box.x && w.box.y==m.box.y &&
               w.box.w==m.box.w && w.box.h==m.box.h && w.max==m.max;
    }
    case CHK_MAXIMIZED: {
        // Most of the display it is on, and the layout says maximized
        WinBox  disp[16];
        i32     n = os_guidisplays(g->os, disp, countof(disp));
        WinGeom w = os_guiwhere(g->os);
        i64     most = 0;
        WinBox  on   = {};
        for (i32 i = 0; i < n; i++) {
            i64 o = boxoverlap(w.box, disp[i]);
            on    = o>most ? disp[i] : on;
            most  = o>most ? o : most;
        }
        Arena a = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: the window ");
        print(&b, (i64)w.box.w);
        putbyte(&b, 'x');
        print(&b, (i64)w.box.h);
        print(&b, w.max ? Str(", maximized") : Str(", not maximized"));
        print(&b, g->geom.max ? Str(", saved so") : Str(", saved not so"));
        print(&b, "; its display ");
        print(&b, (i64)on.w);
        putbyte(&b, 'x');
        print(&b, (i64)on.h);
        guilog(g, finish(&b));
        return w.ok && (w.max || g->ops[g->drivepc-1].dx!=0) && g->geom.max &&
               10*(i64)w.box.w>=9*(i64)on.w && 10*(i64)w.box.h>=8*(i64)on.h;
    }
    case CHK_QUEUESHOWN: {
        DriveOp *op = &g->ops[g->drivepc-1];
        return g->sess && v->queue.len==(iz)op->dx && g->queueshown==(i32)op->dx &&
               (!v->queue.len || v->queue[0].text==op->text);
    }
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
    case CHK_JOBSTAB:       return tabselected("Jobs") && g->jobsvisible;
    case CHK_NAVWIN:        return c.NavWindowingTarget != 0;
    case CHK_NONAVWIN:      return !c.NavWindowingTarget && c.NavWindowingHighlightAlpha==0;
    case CHK_RESIZED:       return (i32)c.IO.DisplaySize.x == (i32)(1100*winscale(g));
    case CHK_TYPED:         return promptstr(g) == "Hello, Prospero";
    case CHK_BLINKOFF:      return !c.IO.ConfigInputTextCursorBlink;
    case CHK_HISTORY:       return v->msgs.len >= 10000;
    case CHK_SCROLLED:      return g->sess ? !g->sess->stick : !g->nosession.stick;
    case CHK_SELECTED: {
        Range r = selrange(&v->sel);
        return v->sel.active && r.end.msg > r.beg.msg;
    }
    case CHK_COPIED:
        return g->drivecode.len && driveclipboard(g, g->drivecode)==g->drivecode;
    case CHK_SAVED:         return g->idlesaves > 0;
    case CHK_NOSTALE:       return g->stale == 0;
    case CHK_DPI:
        return g->scale==1.5f && ImGui::GetStyle().FontScaleDpi==1.5f &&
               g->fonts.size==g->fontbase*g->zoom*1.5f;
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
    case CHK_ENGINE:
        // Its hello said what it takes and has (hostcan), and it is no host
        return g->engineok && !g->engineerr.len && g->hosts->version.len &&
               hostcan(g->hosts, "list") && hostcan(g->hosts, "file.base64") &&
               !hostcan(g->hosts, "host");
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
    case CHK_ADDEXISTS:
        return find(g->cfgstatus, "already exists")>=0 && !g->cfgset;
    case CHK_ROQUIET:
        return g->sess && g->sess->readonly && v->busy && !g->roclock;
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
    case CHK_LIGHT: return themeshown(g, &themelight);
    case CHK_DARK:  return themeshown(g, &themedark);
    case CHK_THEMEQUIET: {
        // Two frames recolor (docked windows take their background a
        // frame late), one settles; with the probe's, the overlay's new
        // numbers, the script's own and those after an input (Windows
        // counts a settings broadcast as one), under ten in a second or
        // two, where rendering on would be sixty a second
        Arena a = g->frame;
        Buf   b(&a, 64);
        i64   n = g->total.frames - g->driveframes;
        print(&b, "drive: ");
        print(&b, n);
        print(&b, " frames since the theme changed");
        guilog(g, finish(&b));
        return n <= 20;
    }
    case CHK_TITLEBAR:
        return g->titleok;
    case CHK_CLICKTHROUGH:
        return g->clickthrough;
    case CHK_LONGFIELD: {
        CfgRow *r = longrow(g);
        if (!r) {
            return 0;
        }
        Str field = {(u8 *)r->buf, (iz)__builtin_strlen(r->buf)};
        g->longval = clone(&g->perm, r->value);
        return field==r->value && r->cap>=r->value.len+FIELD_MAX;
    }
    case CHK_ONEPANE:
        return !g->cfgtwopane && g->cfgframe+3 >= g->total.frames;
    case CHK_TWOPANE:
        return g->cfgtwopane && g->cfgframe+3 >= g->total.frames;
    case CHK_NOHSCROLL: {
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: the settings could scroll sideways by ");
        print(&b, (i64)g->cfghscroll);
        guilog(g, finish(&b));
        return g->cfghscroll<=0 && g->cfgframe+3>=g->total.frames;
    }
    case CHK_PAGE: {
        Arena a    = g->frame;
        Str   want = g->ops[g->drivepc-1].text;
        Str   page = g->cfgdiag ? Str("diagnostics") : cfgpagekey(&a, g->cfgpkind, g->cfgpname);
        return page == want;
    }
    case CHK_ROOTS:
        for (iz i = 0; i < g->cfgrows.len; i++) {
            CfgRow *r = &g->cfgrows[i];
            if (r->kind=="guard" && r->key=="roots") {
                Str field = {(u8 *)r->buf, (iz)__builtin_strlen(r->buf)};
                return r->value=="~/a; ~/b" && field=="~/a\n~/b" && r->file.len;
            }
        }
        return 0;
    case CHK_DELINCLUDE: {
        CfgHead *any = cfghead(g, "model", "shared", 0);
        return g->cfgpkind=="model" && g->cfgpname=="shared" &&
               any && !any->own && endswith(any->file, "team.ini") &&
               !cfghead(g, "model", "shared", 1) && g->rects[RECT_DELETE].x1>0;
    }
    case CHK_DELMODAL: {
        ImGuiWindow *w = ImGui::GetTopMostPopupModal();
        return w && find({(u8 *)w->Name, (iz)__builtin_strlen(w->Name)}, "Delete section")>=0;
    }
    case CHK_DELKEPT: {
        Arena a  = g->frame;
        Read  rd = os_read(g->os, &a, cfgfile(g, &a), 1<<20);
        return cfghas(g, "endpoint", "gone") && g->cfgsnap.data && rd.data==g->cfgsnap &&
               g->cfgpkind=="endpoint" && g->cfgpname=="gone";
    }
    case CHK_DELETED: {
        Arena a  = g->frame;
        Read  rd = os_read(g->os, &a, cfgfile(g, &a), 1<<20);
        return !cfghas(g, "endpoint", "gone") && startswith(g->cfgstatus, "Deleted [endpoint gone]") &&
               rd.err==READ_OK && find(rd.data, "[endpoint gone]")<0 &&
               find(rd.data, "An endpoint to delete")<0 && !g->cfgpkind.len;
    }
    case CHK_REMOTEPAGE: {
        b32 list = 0;
        for (iz i = 0; i < g->cfgrows.len; i++) {
            CfgRow *r = &g->cfgrows[i];
            list |= r->kind=="remote" && r->name=="far" && r->key=="list" &&
                    r->value=="connected" && r->choices.len==2;
        }
        return list && g->cfgpkind=="remote" && g->cfgpname=="far" &&
               g->rects[RECT_PAGECONNECT].x1 > 0;
    }
    case CHK_HOSTSNOHSCROLL: {
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: the Hosts panel could scroll sideways by ");
        print(&b, (i64)g->hostshscroll);
        guilog(g, finish(&b));
        return g->hostshscroll<=0 && g->hostsframe+3>=g->total.frames;
    }
    case CHK_LONGEDIT: {
        CfgRow *r = longrow(g);
        Arena   a = g->frame;
        return r && g->longval.len && r->file.len &&
               r->value==concat(&a, g->longval, "!");
    }
    case CHK_ENGINESTOPPED: {
        b32 ok = g->enginegone && !running(g), unknown = 0, denied = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            View *sv = s->view;
            if (!sv) continue;
            ok &= !sv->busy && !sv->asks && sv->state==LIVE_CLOSED;
            for (iz i = 0; i < sv->msgs.len; i++) {
                ViewTool *t = sv->msgs[i].tool;
                i32       r = sv->msgs[i].role;
                unknown |= r==VIEW_TOOL && t->name=="shell" && t->status==VTOOL_UNKNOWN;
                denied  |= r==VIEW_ASK && t->answer==VASK_DENIED && t->by=="stopped";
            }
        }
        return ok && unknown && denied;
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
    case CHK_NESTED: {
        // The sidebar: the session shown, then its two subagents
        Arena               a    = g->frame;
        Slice<GuiSession *> list = sesslist(g, &a);
        iz                  at   = -1;
        for (iz i = 0; i < list.len; i++) {
            at = list[i]==g->sess ? i : at;
        }
        if (at<0 || at+2>=list.len) {
            return 0;
        }
        i32 reported = 0;
        for (iz i = at+1; i <= at+2; i++) {
            View *k = list[i]->view;
            reported += list[i]->depth==1 && k && k->parent==g->sess->sid &&
                        !k->busy && k->msgs.len>=2;
        }
        return reported==2 && v->msgs.len &&
               endswith(v->msgs[v->msgs.len-1].text, "Both subagents reported.");
    }
    case CHK_TASKSTATE:
        return g->taskshown == 2;
    case CHK_CHILDSHOWN: {
        b32 task = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            task |= v->msgs[i].role==VIEW_USER &&
                    startswith(v->msgs[i].text, "Report as a subagent");
        }
        return g->drivemain && g->sess!=g->drivemain && v->parent==g->drivemain->sid &&
               task && g->rects[RECT_PARENTLINK].x1>0;
    }
    case CHK_PARENTSHOWN:
        return g->sess && g->sess==g->drivemain;
    case CHK_MSGNOTE:
    case CHK_MSGSENDER: {
        b32 want = chk==CHK_MSGNOTE;
        b32 row  = 0;
        if (!g->drivemain || !v) return 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            row |= want ? m->role==VIEW_NOTE && m->from.len &&
                          m->label=="Message from Messenger"
                        : m->role==VIEW_TOOL && m->tool->name=="send_message" &&
                          m->tool->child==g->drivemain->sid;
        }
        b32 where = want ? g->sess==g->drivemain
                         : g->sess!=g->drivemain && v->parent==g->drivemain->sid;
        return where && row && g->rects[RECT_MSGLINK].x1>0;
    }
    case CHK_KIDASK:
        return g->kidaskshown && g->kidlinkshown && g->rects[RECT_TASKLINK].x1>0;
    case CHK_KIDALLOWED: {
        Arena       a = g->frame;
        GuiSession *k = drivekid(g, 0);
        ViewMsg    *q = k ? lastof(k->view, VIEW_ASK) : 0;
        ViewMsg    *m = lastof(v, VIEW_ASSISTANT);
        Str         f = join(&a, g->drivecwd, "../gui-child.txt");
        return q && q->tool->answer==VASK_ALLOWED && q->tool->by=="user" &&
               m && find(m->text, "Child wrote it")>=0 &&
               os_stat(g->os, a, f).type==STAT_FILE;
    }
    case CHK_ALWAYSBTN: {
        ViewMsg *m = lastmsg(g, VIEW_ASK);
        Rect     a = g->rects[RECT_ALLOW];
        Rect     w = g->rects[RECT_ALWAYS];
        Rect     d = g->rects[RECT_DENY];
        return m && m->tool->answer==VASK_PENDING && endswith(m->tool->grant, "/gui-always") &&
               m->tool->access=="write" && !m->tool->scope.len &&
               w.x1>w.x0 && w.x0>a.x1 && d.x0>w.x1;
    }
    case CHK_ALWAYSPATH: {
        ViewMsg *k = lastmsg(g, VIEW_ASK);
        ViewMsg *m = lastmsg(g, VIEW_ASSISTANT);
        Arena    a = g->frame;
        Str      f = join(&a, g->drivecwd, "../gui-always/b.txt");
        i32      n = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewTool *t = v->msgs[i].tool;
            n += v->msgs[i].role==VIEW_ASK && endswith(t->grant, "/gui-always");
        }
        return k && k->tool->answer==VASK_ALLOWED && k->tool->always &&
               k->tool->by=="user" && n==1 && g->rects[RECT_ALWAYS].x1<=0 &&
               m && find(m->text, "Written twice")>=0 &&
               os_stat(g->os, a, f).type==STAT_FILE;
    }
    case CHK_DESKALWAYSBTN: {
        ViewMsg *m = lastmsg(g, VIEW_ASK);
        Rect     a = g->rects[RECT_ALLOW];
        Rect     w = g->rects[RECT_ALWAYS];
        return v->parent.len && m && m->tool->answer==VASK_PENDING &&
               m->tool->scope=="desktop" && m->tool->grant=="input" &&
               w.x1>w.x0 && w.x0>a.x1;
    }
    case CHK_KIDALWAYS: {
        // Shown: the subagent, whose parent's reply follows
        GuiSession *k = g->sess;
        GuiSession *p = k ? findsess(g, k->host, v->parent) : 0;
        ViewMsg    *q = lastof(v, VIEW_ASK);
        ViewMsg    *m = p && p->view ? lastof(p->view, VIEW_ASSISTANT) : 0;
        i32         n = 0;
        for (iz i = 0; k && i < k->view->msgs.len; i++) {
            n += k->view->msgs[i].role == VIEW_ASK;
        }
        return q && q->tool->answer==VASK_ALLOWED && q->tool->always &&
               q->tool->scope=="desktop" && n==1 &&
               m && find(m->text, "Typed for good")>=0;
    }
    case CHK_TODOACTIVE:
        return g->todoshown==3 && v->todos[0].status==VTODO_ACTIVE &&
               v->todos[1].status==VTODO_PENDING && v->todos[2].status==VTODO_PENDING;
    case CHK_TODODONE: {
        b32 done = v->todos.len == 3;
        for (iz i = 0; done && i < v->todos.len; i++) {
            done = v->todos[i].status == VTODO_DONE;
        }
        return done && g->todoshown==3;
    }
    case CHK_JOBSDRAWN:
    case CHK_JOBSQUIET: {
        // Shown, a frame and its settling (three at most) for each
        // JOBS_PERIOD, rather than 30 a second; hidden, none. The
        // metrics, if shown, count each second in a few more.
        i64   n     = g->total.frames - g->driveframes;
        i64   us    = g->sched.now - g->markat;
        i64   other = (g->metricsshown ? 4*us/1000000 : 0) + 2 +
                      3*(g->saves - g->marksaves);  // a save's, and a probe after
        b32   show  = chk == CHK_JOBSDRAWN;
        Arena a     = g->frame;
        Buf   b(&a, 128);
        print(&b, "drive: frames while a job prints, the Jobs tab ");
        print(&b, show ? Str("shown: ") : Str("hidden: "));
        print(&b, n);
        print(&b, " in ");
        printfloat(&b, (f64)us/1e6, 1);
        print(&b, " s");
        guilog(g, finish(&b));
        if (show) {
            return tabselected("Jobs") && g->jobsvisible && n>0 &&
                   n<=3*us/JOBS_PERIOD + other;
        }
        return !g->jobsvisible && n<=other;
    }
    case CHK_JOBS: {
        ViewJob *a = findjob(v, 1), *b = findjob(v, 2);
        return v->jobs.len==2 && a && b && a->status==VJOB_EXITED && a->hasexit &&
               !a->exit && b->status==VJOB_RUNNING && v->jobsrun==1 &&
               g->jobsshown==2 && g->rects[RECT_JOBKILL].x1>0;
    }
    case CHK_JOBKILLED: {
        ViewJob *b = findjob(v, 2);
        ViewMsg *n = lastof(v, VIEW_NOTE);
        b32 resumed = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            resumed |= v->msgs[i].role==VIEW_NOTE && v->msgs[i].startsturn;
        }
        return b && b->status==VJOB_KILLED && !v->jobsrun && resumed &&
               n && startswith(n->label, "To the agent: job");
    }
    case CHK_REWINDASK: {
        Arena           a   = g->frame;
        ImGuiWindow    *w   = ImGui::GetTopMostPopupModal();
        GuiSession     *s   = g->rewsess;
        Slice<ViewSnap> set = s && s->view ? rewindset(s->view, &a, g->rewcall, g->rewturn) : Slice<ViewSnap>{};
        b32 open = w && find({(u8 *)w->Name, (iz)__builtin_strlen(w->Name)}, "Rewind")>=0;
        return open && set.len==1 && endswith(set[0].path, g->ops[g->drivepc-1].text) &&
               g->rects[RECT_REWINDOK].x1>0;
    }
    case CHK_UNDONE: {
        Arena     a = g->frame;
        ViewMsg  *c = lastof(v, VIEW_REWIND);
        ViewTool *t = lasttool(g, "write");
        ViewSnap *n = rowsnap(v, t);
        Str       f = join(&a, g->drivecwd, "undo-b.txt");
        return c && c->card->count==1 && !c->card->conflicts && find(c->text, "removed")>=0 &&
               n && n->undone && endswith(n->path, "undo-b.txt") &&
               os_stat(g->os, a, f).type==STAT_MISSING;
    }
    case CHK_CONFLICT: {
        Arena    a  = g->frame;
        ViewMsg *c  = lastof(v, VIEW_REWIND);
        Read     rd = os_read(g->os, &a, join(&a, g->drivecwd, "undo-a.txt"), 1<<10);
        return c && c->card->conflicts==1 && c->card->turn && c->card->latest &&
               rd.err==READ_OK && rd.data=="changed elsewhere\n" &&
               g->rects[RECT_FORCE].x1>0;
    }
    case CHK_FORCED: {
        Arena    a = g->frame;
        ViewMsg *c = lastof(v, VIEW_REWIND);
        Str      f = join(&a, g->drivecwd, "undo-a.txt");
        return c && c->card->force && !c->card->conflicts && find(c->text, "removed")>=0 &&
               os_stat(g->os, a, f).type==STAT_MISSING;
    }
    case CHK_COMPACTING:
        return g->compactingshown;
    case CHK_SUMMARY: {
        ViewMsg *m = lastof(v, VIEW_COMPACT);
        return m && m->card->kind=="summary" && m->card->before>m->card->after &&
               m->card->count>0 && find(m->text, "SUMMARY-")>=0 &&
               g->rects[RECT_COMPACTHDR].x1>0;
    }
    case CHK_COMPACTOPEN: {
        ViewMsg *m = lastof(v, VIEW_COMPACT);
        return m && !m->collapsed && m->measured && m->h>0;
    }
    case CHK_PRUNE: {
        ViewMsg *m = lastof(v, VIEW_COMPACT);
        i32 elided = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            elided += v->msgs[i].role==VIEW_TOOL && v->msgs[i].tool->elided;
        }
        return m && m->card->kind=="prune" && m->card->count>=3 && elided==m->card->count;
    }
    case CHK_INFO:
        return c.OpenPopupStack.Size>0 && g->infoshown>=1 && g->infoagents>=1;
    case CHK_QUEUED:
        return g->queuedshown && v->state==LIVE_QUEUED && v->queuepos==1;
    case CHK_FALLBACK:
        return g->fallbackshown && find(v->retrymodel, "guifail_b")>=0;
    case CHK_CLOSEBG: {
        Rect r = g->rects[RECT_CLOSEBG];
        return r.x1>r.x0 && !g->whynobg.len && running(g)==1;
    }
    case CHK_SHED: {
        // Registered, listening, and nothing drawn any more
        Arena a   = g->frame;
        i64   pid = os_pid(g->os);
        b32   reg = 0;
        for (HostEntry *e = g->host ? hostentries(g->os, &a, g->statedir) : 0; e; e = e->next) {
            reg |= e->pid==pid && e->endpoint==g->host->endpoint;
        }
        b32 listening = g->host && os_listening(g->os, a, g->host->endpoint);
        Buf b(&a, 128);
        print(&b, "drive: shed, host ");
        print(&b, reg ? Str("registered") : Str("not registered"));
        print(&b, listening ? Str(" and listening") : Str(", not listening"));
        print(&b, g->trayup ? Str(", with a tray icon") : Str(", with no tray icon"));
        guilog(g, finish(&b));
        return g->bg && reg && listening;
    }
    case CHK_SHEDQUIET: {
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: frames while shed: ");
        print(&b, g->shedframes);
        guilog(g, finish(&b));
        return !g->shedframes;
    }
    case CHK_KEYLOST:
        // Frames since the mark: the probe's and the script's own
        return !ImGui::IsKeyDown(ImGuiKey_G) &&
               drivequiet(g, "2 s after a key's release was lost", 2);
    case CHK_PLANMODE:
        return g->sess && v->plan==(b32)g->ops[g->drivepc-1].dx;
    case CHK_PLANASK: {
        ViewMsg *m = lastmsg(g, VIEW_ASK);
        return m && m->tool->answer==VASK_PENDING && m->tool->kind=="plan" &&
               m->md && find(m->text, "# Plan")==0 && g->rects[RECT_ALLOW].x1>0 &&
               g->rects[RECT_DENY].x1>0;
    }
    case CHK_PLANTEXT:
        return tabselected("Plan") && g->planshown>0 &&
               find(finish(&g->plan.text), g->ops[g->drivepc-1].text)>=0;
    case CHK_PLANMODEL:
        return g->sess && v->busy && planwaits(v) &&
               !sesswhy(g, g->sess, "model", !planwaits(v), 0).len;
    case CHK_SNAPLIVE: {
        // Taken again a second or more into the run, the key busy: one
        // a second, not only when frames stop
        i64   ms = (g->snap.at - g->markat)/1000;
        Arena a  = g->frame;
        Buf   b(&a, 96);
        print(&b, "drive: metrics snapshot ");
        print(&b, ms);
        print(&b, " ms after the mark");
        print(&b, (g->snap.busy & BUSY_KEY) ? Str(", key busy") : Str(""));
        guilog(g, finish(&b));
        return g->snap.busy&BUSY_KEY && ms>=1000;
    }
    case CHK_REOPENED:
        return !g->bg && g->main->own && g->main->up && g->sess &&
               g->sess->host==g->main && !g->sess->readonly;
    case CHK_GUILOCK: {
        // Taken again from here, it fails as for any other process
        Arena a     = g->frame;
        Str   path  = guilockpath(&a, g->statedir);
        b32   held  = os_lockheld(g->os, a, path);
        iz    again = guilock(g->os, a, g->statedir);
        if (again >= 0) {
            os_unlockfile(g->os, again);
        }
        return g->guilock>=0 && held && again<0;
    }
    case CHK_BGDONE:
    case CHK_BGSESSION: {
        // The whole answer, streamed partly to nobody, in order, once
        ViewMsg *m = 0;
        for (iz i = v->msgs.len-1; i >= 0; i--) {
            m = v->msgs[i].role==VIEW_ASSISTANT && !v->msgs[i].prov ? &v->msgs[i] : m;
        }
        b32 whole = m && startswith(m->text, "bg0 bg1 ") && endswith(m->text, "bg399 ") &&
                    countrole(v, VIEW_ASSISTANT)==1;
        Arena a = g->frame;
        Buf   b(&a, 256);
        print(&b, "drive: ");
        print(&b, (i64)countrole(v, VIEW_ASSISTANT));
        print(&b, " answers, the last of ");
        print(&b, m ? m->text.len : (iz)0);
        print(&b, " bytes, ending ");
        print(&b, m ? taketail(m->text, m->text.len<16 ? m->text.len : 16) : Str("-"));
        print(&b, v->busy ? Str("; busy, on ") : Str("; idle, on "));
        print(&b, g->sess ? hostlabel(g, &a, g->sess->host) : Str("no session"));
        guilog(g, finish(&b));
        if (chk == CHK_BGDONE) {
            return whole && !v->busy && g->sess && g->sess->host==g->main;
        }
        return whole && !v->busy && g->sess && g->sess->host->kind==GH_LOCAL &&
               !g->sess->readonly && g->bgbadge>0;
    }
    case CHK_BGLISTED: {
        GuiHost *h = drivehost(g, GH_LOCAL);
        b32      listed = 0, engine = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            b32 bg = s->listed && startswith(s->title, "Stream in the background");
            listed |= bg && s->host==h;
            engine |= bg && s->host->kind==GH_ENGINE;
        }
        return h && listed && !engine;
    }
    case CHK_BGPROMPT:
        return g->sess && g->sess->host->kind==GH_LOCAL && !v->busy &&
               countrole(v, VIEW_ASSISTANT)==2;
    case CHK_REMOTEUP: {
        // Up, with what its hello said passed on by the library
        GuiHost *h = drivehost(g, GH_REMOTE);
        return h && h->up && h->opens==1 && h->version.len &&
               hostcan(h, "host") && hostcan(h, "open");
    }
    case CHK_REMOTESESSION:
        // (the directory as given, as far as the host spells it alike)
        return g->sess && g->sess->host->kind==GH_REMOTE && g->driverwd.len &&
               pathendswith(v->cwd, g->driverwd) && !g->sess->readonly;
    case CHK_IMPORTCWD: {
        Str cwd = {(u8 *)g->newcwd, (iz)__builtin_strlen(g->newcwd)};
        return g->driverwd.len && pathendswith(cwd, g->driverwd);
    }
    case CHK_IMPORTREMOTE:
        return g->sess && g->sess->host->kind==GH_REMOTE && !v->lost &&
               g->sess->note.len && !g->sess->readonly;
    case CHK_REMOTEDOWN:
        return g->downshown;
    case CHK_REMOTEOK: {
        // The answer whole, and the deltas once each: as many bytes as
        // it has, though the link dropped in the middle of them
        GuiHost *h = drivehost(g, GH_REMOTE);
        ViewMsg *m = lastmsg(g, VIEW_ASSISTANT);
        i64      d = v->deltabytes - g->deltamark;
        Arena    a = g->frame;
        Buf      b(&a, 128);
        print(&b, "drive: ");
        print(&b, d);
        print(&b, " bytes of deltas for an answer of ");
        print(&b, m ? m->text.len : (iz)0);
        print(&b, "; the link went down ");
        print(&b, h ? h->downs : 0);
        print(&b, " times, up ");
        print(&b, h ? h->opens : 0);
        guilog(g, finish(&b));
        return h && h->up && h->downs>=1 && h->opens>=2 && m && !v->lost &&
               startswith(m->text, "link0 link1 ") && endswith(m->text, "link299 ") &&
               d==m->text.len && !v->busy;
    }
    case CHK_OUTPUT:
        return g->outshown && g->out.s==g->sess && g->sess->host->kind!=GH_ENGINE &&
               find(finish(&g->out.text), "line 6")>=0 && !g->out.err.len;
    case CHK_REMOTECLOSED: {
        GuiHost *h = drivehost(g, GH_REMOTE);
        b32      any = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            any |= s->host==h && s->view;
        }
        return h && !h->r && !h->up && any;
    }
    case CHK_OWNNEW:
        return g->sess && g->main->own && g->sess->host==g->main && v->cwd.len &&
               !g->sess->readonly;
    case CHK_REMOTEFAILED: {
        GuiHost *h = findhost(g, GH_REMOTE, "bad");
        return h && h->state==RS_FAILED && !h->r && g->failshown &&
               find(h->msg, "cannot begin with -")>=0;
    }
    case CHK_CLOSEREMOTE: {
        ImGuiWindow *w = ImGui::GetTopMostPopupModal();
        return w && !running(g) && elsewhere(g)==1 && g->elseshown;
    }
    case CHK_REFRESHED: {
        Json *inc = jsonget(g->cfg, "includes")->child;
        return g->cfgrefreshed>0 && g->cfgok && inc && jsonint(jsonget(inc, "fetched"), 0)>0 &&
               !jsonstr(jsonget(inc, "error")).len;
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
            g->drivecode    = c;
            g->drivecodemsg = i;
            g->drivecodeblk = k;
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
    ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    if (pio.Platform_SetClipboardTextFn != driveclipset) {
        // Again after the window comes back, with a new backend
        clipgui = g;
        clipset = pio.Platform_SetClipboardTextFn;
        pio.Platform_SetClipboardTextFn = driveclipset;
    }
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
    case OP_KEYUP: {
        // The command key is Cmd on macOS, Ctrl elsewhere
        b32 cmd = ((i32)op->dy & KEY_CMD) != 0;
        e.kind  = GEV_KEY;
        e.key   = (i32)op->dx==GKEY_CMD && !g->mac ? GKEY_CTRL : (i32)op->dx;
        e.ctrl  = ((i32)op->dy & KEY_CTRL)!=0 || (cmd && !g->mac);
        e.cmd   = cmd && g->mac;
        e.shift = ((i32)op->dy & KEY_SHIFT) != 0;
        e.down  = op->op == OP_KEYDOWN;
        os_guiinject(g->os, &e);
    } break;
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
        #if !defined(_WIN32) && !defined(__APPLE__)
        if (op->target == CHK_TITLEBAR) {
            guilog(g, "drive: skip the window frame check (the window manager draws it)");
            break;
        }
        #endif
        #ifdef _WIN32
        if (op->target == CHK_CLICKTHROUGH) {
            guilog(g, "drive: skip the click-through check (Windows passes the click on)");
            break;
        }
        #endif
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
        g->marksaves   = g->saves;
        if (op->text.len) {
            // For a measurement outside, by the operating system
            Arena a = g->frame;
            // Not in the Log window, which would make frames to show it
            os_print(g->os, 2, concat(&a, concat(&a, "prospero: drive: mark: ", op->text), "\n"));
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
        guinew(g, g->main, g->drivecwd, {}, op->text);  // in a role, if named
        break;
    case OP_TOUCH: {
        Arena a = g->frame;
        if (!os_write(g->os, a, join(&a, g->drivecwd, op->text), "changed elsewhere\n")) {
            guilog(g, "drive: FAIL could not change the file");
            g->drivechecks++;
            g->drivefails++;
        }
    } break;
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
    case OP_MINCLOSE:
        if (os_guilimits(g->os) & GUI_NOPLACE) {
            // Wayland: a window cannot leave being minimized by itself,
            // so the dialog would wait there until the user brought it
            // back, and the checklist forever
            guilog(g, "drive: a minimized window cannot come back by itself here: "
                      "closing it as it is");
            e.kind = GEV_CLOSE;
            os_guiinject(g->os, &e);
            break;
        }
        g->minclose = 1;  // gui_minimized asks to close
        e.kind = GEV_MINIMIZE;
        os_guiinject(g->os, &e);
        break;
    case OP_MINCLOSED:
        if (g->minclose) {
            // No window manager to minimize it (Xvfb)
            guilog(g, "drive: the window did not minimize: closing anyway");
            g->minclose = 0;
            e.kind = GEV_CLOSE;
            os_guiinject(g->os, &e);
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
    case OP_SYSTHEME:
        e.kind = GEV_SYSTHEME;
        e.x    = op->dx;
        os_guiinject(g->os, &e);
        break;
    case OP_KILLENGINE:
        if (g->proc) {
            guilog(g, "drive: killing the engine process");
            os_kill(g->os, g->proc, 1);
        } else {
            guilog(g, "drive: FAIL no engine process to kill");
            g->drivechecks++;
            g->drivefails++;
        }
        break;
    case OP_REVEAL:
        g->reveal = op->target;
        scheddirty(&g->sched);
        break;
    case OP_TOOLS:
        settools(g, (i32)op->dx);
        break;
    case OP_SHOWCODE: {
        View *v = g->view;
        if (g->drivecodemsg>=0 && g->drivecodemsg<v->msgs.len && v->lastw>0) {
            i32        i  = g->drivecodemsg;
            MsgLayout *ml = msglayout(v, i, &g->measure, v->lastw, g->scale);
            f64        y  = v->msgs[i].top + v->head;
            for (iz k = 0; k < ml->blocks.len; k++) {
                if (ml->blocks[k].block == g->drivecodeblk) {
                    y += ml->blocks[k].y;
                }
            }
            g->scrollto = y - 120*g->scale;
            g->scrollto = g->scrollto<0 ? 0 : g->scrollto;
            if (g->sess) g->sess->stick = 0;
            scheddirty(&g->sched);
        }
    } break;
    case OP_ANCHOR: {
        View       *v  = g->view;
        GuiSession *gs = g->sess;
        g->driveanchor = {-1, -1, 0};
        if (gs && gs->win) {
            g->driveanchor = viewanchor(v, &g->measure, v->lastw, g->scale, v->head,
                                        gs->win->Scroll.y);
        }
    } break;
    case OP_GAUGE: {
        // As track A1's engine sends them
        Arena a = g->frame;
        Buf   b(&a, 256);
        print(&b, "{\"ts\":");
        print(&b, os_time(g->os));
        print(&b, ",\"ev\":\"live\",\"state\":\"idle\",\"ctx\":");
        print(&b, (i64)op->dx);
        print(&b, ",\"compact_at\":");
        print(&b, (i64)op->dy);
        print(&b, ",\"tokens\":");
        print(&b, op->text);
        putbyte(&b, '}');
        viewapply(g->view, a, finish(&b));
        scheddirty(&g->sched);
    } break;
    case OP_SHOWPAGE:
        g->showpage = op->text;  // static
        scheddirty(&g->sched);
        break;
    case OP_THINKING:
        setthinking(g, (i32)op->dx);
        g->thinkchars0 = g->thinksecs0 = -1;
        break;
    case OP_TRAY:
        e.kind = GEV_TRAY;
        e.x    = op->dx;
        os_guiinject(g->os, &e);
        break;
    case OP_DROPLINK:
        if (g->sess && g->sess->host->r) {
            guilog(g, "drive: dropping the link to the session's host");
            remotedrop(g->sess->host->r);
        } else {
            guilog(g, "drive: FAIL no link to drop");
            g->drivechecks++;
            g->drivefails++;
        }
        break;
    case OP_TEXTRCWD:
        e.kind = GEV_TEXT;
        e.text = g->driverwd;  // null-terminated, from the command line
        os_guiinject(g->os, &e);
        break;
    case OP_IFSESS: {
        b32 found = 0;
        for (GuiSession *s = g->sessions; s; s = s->next) {
            Str t = s->view && s->view->title.len ? s->view->title : s->title;
            found |= startswith(t, op->text);
        }
        if (!found) {
            Arena a = g->frame;
            guilog(g, concat(&a, "drive: skip, with no session titled ", op->text));
            g->drivepc += (i32)op->dx;
        }
    } break;
    case OP_IFROLE: {
        b32 found = 0;
        for (iz i = 0; i < g->cfgheads.len; i++) {
            CfgHead *h = &g->cfgheads[i];
            found |= h->kind=="role" && h->name==op->text;
        }
        if (!found) {
            Arena a = g->frame;
            guilog(g, concat(&a, "drive: skip, with no role ", op->text));
            g->drivepc += (i32)op->dx;
        }
    } break;
    case OP_MARKDELTAS:
        g->deltamark = g->view->deltabytes;
        break;
    case OP_GEOMMARK:
        g->geommark = os_guiwhere(g->os);
        break;
    case OP_MAXIMIZE: {
        WinGeom w = os_guiwhere(g->os);
        WinBox  d = {};
        w.max = op->dx == 0;
        if (op->dx != 0) {
            #ifdef _WIN32
            b32 nofill = 1;
            #else
            b32 nofill = (os_guilimits(g->os) & GUI_NOPLACE) != 0;
            #endif
            if (nofill || !os_guidisplays(g->os, &d, 1)) {
                guilog(g, "drive: skip filling the display (no maximum here)");
                g->drivepc++;
                break;
            }
            w.box = {d.x, d.y + PLACE_TITLE, d.w, d.h - PLACE_TITLE};
        }
        os_guiplace(g->os, w);
    } break;
    case OP_MARKSESS:
        g->drivesess = g->sess;
        break;
    case OP_PLACE:
        if (op->dx != 0) {
            // As if saved on a display right of every display there is
            WinBox disp[16];
            i32    n    = os_guidisplays(g->os, disp, countof(disp));
            WinGeom far = g->geommark;
            for (i32 i = 0; i < n; i++) {
                i32 right = disp[i].x + disp[i].w;
                far.box.x = right>far.box.x ? right : far.box.x;
            }
            far.box.x += 200;
            os_guiplace(g->os, far);
        } else {
            os_guiplace(g->os, g->geommark);
        }
        break;
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

// A font for each style, from fonts.cpp: prose falls back on code's font
// and then the symbol subsets, code on the subsets, each fallback merged
// into the face before it with the face's own flags (synthetic bold and
// oblique), so that a symbol in bold prose is bold. Glyphs share the
// first font's baseline, which the loader aligns them on.
static ImFont *addfont(Str data, i32 flags, b32 merge)
{
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = 0;
    cfg.FontLoaderFlags      = (u32)flags;
    cfg.MergeMode            = merge != 0;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        (void *)data.data, (i32)data.len, 15, &cfg
    );
}

static ImFont *addface(Str data, i32 flags)
{
    ImFont *f = addfont(data, flags, 0);
    if (data.data != fontmono().data) {
        addfont(fontmono(), flags, 1);
    }
    addfont(fontsymbols(), flags, 1);
    addfont(fontmath(), flags, 1);
    return f;
}

static void loadfonts(Gui *g)
{
    i32 bold   = ImGuiFreeTypeLoaderFlags_Bold;
    i32 italic = ImGuiFreeTypeLoaderFlags_Oblique;
    Fonts *f = &g->fonts;
    f->prose      = addface(fontroboto(), 0);
    f->bold       = addface(fontroboto(), bold);
    f->italic     = addface(fontroboto(), italic);
    f->bolditalic = addface(fontroboto(), bold|italic);
    f->mono       = addface(fontmono(), 0);
    f->monobold   = addface(fontmono(), bold);
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
"usage: prospero [gui [options]]\n"
"  -c FILE        configuration file (default: prospero.ini, as for run)\n"
"  --statedir DIR sessions, layout file and log (default: the state dir)\n"
"  --engine=MODE  inproc (default) or subprocess: \"prospero --headless\"\n"
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
"  --settings     the settings window's checklist, screenshots of each theme\n"
"  --sessions     the sessions sidebar's checklist: tree, menus, export\n"
"  --enginestop   the engine process dies mid-turn (implies subprocess)\n"
"  --background   M5's checklist: keep a turn running in the background,\n"
"                 then come back from the tray (tests/gui.py drives it)\n"
"  --attach       a second window while one runs in the background\n"
"  --remote       a remote host, [remote r], its link dropped mid-turn\n"
"  --rcwd DIR     the remote checklist's working directory there\n"
"  --compose      the compose box's checklist: drafts, history, the menu,\n"
"                 the editor, slash commands ([ui] editor a test script)\n"
"  --compose-again  its drafts after a restart, in the same directories\n"
"  --plan         plan mode's checklist: the dialog, the plan's ask, the\n"
"                 Plan window, the toggle and /plan\n"
"  --host-idle S  in the background, exit S seconds after the last\n"
"                 session ends with no window (default: [host] idle_exit)\n"
"  --probe        staleness probe on (default in debug builds)\n"
"  --no-probe     staleness probe off\n"
"  --aggressive   probe every second while idle\n"
"  --trace        log every frame and why it ran\n"
"  --trace-drive  log where the checklist's presses land\n"
"  --quit-after S quit after S seconds\n";

extern "C" void prospero_imassert(char const *expr, char const *file, int line)
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

// The configuration, read before the window opens so that it opens in
// the theme set there: the in-process engine's own, which it takes over
// when it starts, or a reading of the same file for an engine process.
// The former fetches URL includes not yet cached (30 s at most), the
// latter leaves that to the engine. The fake agent has none.
static void readconfig(Gui *g, Arena scratch)
{
    if (g->enginekind == ENGINE_FAKE) {
        return;
    }
    Os     *os   = g->os;
    b32     own  = g->enginekind == ENGINE_INPROC;
    Str     path = g->cfgpath.len ? g->cfgpath : cfgdefault(os, &g->perm);
    Config *cfg  = newconfig(os, &g->perm, scratch, path, own && !g->cfgpath.len,
                             g->statedir, own ? CFG_NETWAIT : CFG_NETNONE);
    if (!cfg) {
        return;
    }
    i32 theme = themeparse(cfgget(cfg, "ui", "", "theme", 0));
    g->themeset = theme<0 ? THEME_SYSTEM : theme;
    i32 think = thinkparse(cfgget(cfg, "ui", "", "thinking", 0));
    g->thinkset = think<0 ? THINK_FOLD : think;
    i32 tools = toolsparse(cfgget(cfg, "ui", "", "tools", 0));
    g->toolset = tools<0 ? TOOLS_AUTO : tools;
    setfontbase(g, cfgget(cfg, "ui", "", "font_size", 0));
    g->editor = clone(&g->perm, cfgget(cfg, "ui", "", "editor", 0));
    if (own) {
        g->config = cfg;
    } else {
        cfgfree(cfg);
    }
}

// The engine on its own thread, with the configuration it owns.
static b32 startinproc(Gui *g, Arena scratch)
{
    Os     *os  = g->os;
    Config *cfg = g->config;
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

// The engine as "prospero --headless" in a child process: its output
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
    g->guilock  = -1;
    g->scrollto = -1;
    g->second   = g->started;
    g->tps      = 100;
    g->inboxcap = INBOX_CAP;
    g->reveal   = -1;
    g->firstlayout = 1;
    g->zoom     = 1;
    g->fontbase = 15;
    for (i32 i = 0; i < PANE_COUNT; i++) {
        g->panes[i] = i != PANE_METRICS;  // the metrics only when asked for
    }
    #ifdef __APPLE__
    g->mac = 1;
    #endif

    b32 probe = 1;
    #ifdef NDEBUG
    probe = 0;
    #endif
    b32 aggressive = 0, stream = 0, idle = 0, streamtest = 0, drive = 0;
    b32 subprocess = 0, fake = 0, e2e = 0, settings = 0, enginestop = 0;
    b32 background = 0, attach = 0, remote = 0, compose = 0, sidebar = 0, plan = 0;
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
        } else if (a == "--settings") {
            settings = probe = 1;
        } else if (a == "--sessions") {
            sidebar = probe = 1;
        } else if (a == "--enginestop") {
            enginestop = subprocess = probe = 1;
        } else if (a == "--background") {
            background = probe = 1;
        } else if (a == "--attach") {
            attach = probe = 1;
        } else if (a == "--remote") {
            remote = probe = 1;
        } else if (a == "--compose") {
            compose = probe = 1;
        } else if (a == "--plan") {
            plan = probe = 1;
        } else if (a == "--compose-again") {
            compose = 2;
            probe   = 1;
        } else if (a=="--rcwd" && i+1<nargs) {
            g->driverwd = cstrclone(&mem, args[++i]);
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
        } else if (a == "--host-idle") {
            i64 s = optint(args, nargs, &i);
            g->hostidle = s>0 ? s*1000000 : 0;
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
    if (drive || idle || streamtest || e2e || settings || enginestop ||
            background || attach || remote || compose || sidebar || plan) {
        g->drive = 1;
        if (plan) {
            g->ops  = planops;
            g->nops = countof(planops);
        } else if (compose) {
            g->ops  = compose==1 ? composeops : compose2ops;
            g->nops = compose==1 ? countof(composeops) : countof(compose2ops);
        } else if (e2e) {
            g->ops  = e2eops;
            g->nops = countof(e2eops);
        } else if (background) {
            g->ops  = bgops;
            g->nops = countof(bgops);
        } else if (attach) {
            g->ops  = attachops;
            g->nops = countof(attachops);
        } else if (remote) {
            g->ops  = remoteops;
            g->nops = countof(remoteops);
        } else if (settings) {
            g->ops  = settingsops;
            g->nops = countof(settingsops);
        } else if (sidebar) {
            g->ops  = sessionsops;
            g->nops = countof(sessionsops);
        } else if (enginestop) {
            g->ops  = stopops;
            g->nops = countof(stopops);
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
            os_print(os, 2, "prospero: the checklist needs --cwd DIR\n");
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
        os_print(os, 2, "prospero: out of memory\n");
        return 2;
    }
    g->nosession.stick = 1;
    g->nosession.view  = newview(&g->perm, g->vcache, os_reserve(os, 16*mb),
                                 os_reserve(os, 16*mb), os_reserve(os, 16*mb));
    g->view       = g->nosession.view;
    g->edbox      = newmailbox(&g->perm, 1<<14, wakegui, os);
    g->newcwd     = alloc<char>(&g->perm, FIELD_MAX);
    g->namebuf    = alloc<char>(&g->perm, FIELD_MAX);
    g->modelbuf   = alloc<char>(&g->perm, FIELD_MAX);
    g->exp.think  = 0;
    g->exp.tools  = 1;
    g->exp.output = 0;
    g->newmodel   = alloc<char>(&g->perm, FIELD_MAX);
    g->denynote   = alloc<char>(&g->perm, FIELD_MAX);
    g->addname    = alloc<char>(&g->perm, FIELD_MAX);
    g->cfgpbuf    = alloc<u8>(&g->perm, FIELD_MAX);
    g->tplname    = alloc<char>(&g->perm, FIELD_MAX);
    g->tplval     = alloc<char>(&g->perm, FIELD_MAX);
    g->remname    = alloc<char>(&g->perm, FIELD_MAX);
    g->findbuf    = alloc<char>(&g->perm, FIELD_MAX);
    g->search     = newsearch(&g->perm, os_reserve(os, 256*mb), os_reserve(os, 256*mb),
                              FIELD_MAX);
    g->drivecodemsg = -1;
    g->remssh     = alloc<char>(&g->perm, FIELD_MAX);
    g->rempath    = alloc<char>(&g->perm, FIELD_MAX);
    g->hosts      = addhost(g, GH_ENGINE, "");
    g->main       = g->hosts;
    g->pendhost   = g->hosts;
    g->benchtimes = alloc<i64>(&g->perm, BENCH_MAX);
    g->pending    = Buf(&g->pendmem, 1<<12);

    // Paths as given, made absolute here: the engine process runs in the
    // state directory, where relative ones would name other places
    Str here = os_realpath(os, &g->perm, ".");
    if (here.len) {
        if (g->cfgpath.len) {
            g->cfgpath = normpath(&g->perm, here, g->cfgpath, HOST_WINDOWS);
        }
        if (g->statedir.len) {
            g->statedir = normpath(&g->perm, here, g->statedir, HOST_WINDOWS);
        }
        if (g->drivecwd.len) {
            Str cwd = normpath(&g->perm, here, g->drivecwd, HOST_WINDOWS);
            g->drivecwd = cstrclone(&g->perm, cwd);
        }
    }
    if (!g->statedir.len) {
        g->statedir = defaultstate(os, &g->perm);
    }
    // One window per state directory (door.cpp), its lock held for this
    // process's life, the window gone to the background or not: a second
    // launch hands off to the first, and goes
    if (g->statedir.len) {
        Arena a    = g->frame;
        Str   path = guilockpath(&a, g->statedir);
        g->guilock = guilock(os, a, g->statedir);
        if (g->guilock<0 && os_lockheld(os, a, path)) {
            return handoff(g);
        }
    }
    Str sessions = join(&g->perm, g->statedir, "sessions");
    if (g->statedir.len && os_mkdirs(os, g->frame, sessions)) {
        g->inipath = statepath(g, "imgui.ini");
        g->logfd   = os_logopen(os, g->frame, statepath(g, "gui.log"));
        g->saver   = newsaver(os, &g->perm, g->inipath);
    }
    {
        Str cwd = g->drivecwd.len ? g->drivecwd : os_realpath(os, &g->perm, ".");
        copybytes(g->newcwd, cwd.data, cwd.len<FIELD_MAX ? cwd.len : FIELD_MAX-1);
        g->firstcwd = cwd;
    }

    g->inbox  = newmailbox(&g->perm, g->inboxcap, wakegui, os);
    g->outbox = newmailbox(&g->perm, 1<<16, 0, 0);
    g->tasks  = newmailbox(&g->perm, 1<<16, wakegui, os);
    if (!g->inbox || !g->outbox || !g->tasks) {
        os_print(os, 2, "prospero: could not create mailboxes\n");
        return 2;
    }
    if (g->guilock >= 0) {
        Str err = opendoor(os, &g->perm, g->frame, g->statedir, g->tasks, TASK_RAISE);
        if (err.len) {
            guilog(g, concat(&g->frame, "other launches cannot hand off: ", err));
        }
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
            os_print(os, 2, "prospero: out of memory\n");
            return 2;
        }
        GuiSession *s = addsess(g, g->hosts, f->sid);
        View       *v = sessview(g, s);
        v->title = "Fake agent";
        v->cwd   = "(none)";
        v->state = LIVE_IDLE;
        setcur(g, s);
    }

    readconfig(g, g->frame);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.UserData     = g;
    io.IniFilename  = 0;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigErrorRecoveryEnableAssert = 0;
    // Panes close from their tabs alone, not each dock node as well
    ImGui::GetStyle().DockingNodeHasCloseButton = 0;
    g->basestyle = ImGui::GetStyle();
    g->sysdark   = 1;  // until the window opens and the platform says
    applytheme(g);
    loadfonts(g);
    addinihandler(g);

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
    if (!os_guiopen(os, g, "Prospero")) {
        os_print(os, 2, "prospero: could not open a window\n");
        return 1;
    }
    g->sched.pace = os_guirefresh(os);

    switch (g->enginekind) {
    case ENGINE_FAKE:
        if (!os_thread(os, fakemain, f)) {
            os_print(os, 2, "prospero: could not start the fake agent\n");
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
    if (!fake) {
        findhosts(g, g->frame);  // background hosts of the state directory
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

    saveini(g, 1);
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
        print(&b, " ms, max wake ");
        printfloat(&b, (f64)g->maxwakeus/1000, 2);
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

// Configuration: schema, layering, validation, edits and hot reload
//
// The user's file (cfgdefault: ~/.config/harness/harness.ini) is read
// with its includes (ini.cpp) over built-in defaults: every setting has
// a default in the schema below, so an empty file, or none, is a whole
// configuration. An include's settings land where its line is, and
// includes come before the first section, so the user's own sections
// override a shared file's. On first run, when the user has no file,
// the embedded seed (assets/harness.ini), which documents every key,
// is written there byte for byte.
//
// The schema is one table: each key's section kind, type, default,
// settings-window group and description. Loading checks every line
// against it and adds what it finds to the reader's diagnostics, with
// file, line and column. Nothing stops the harness from starting: a bad
// value is an error and falls back to its default, while an unknown
// key, section kind or enum value is a warning. Secrets are references,
// key = env:NAME, resolved when used. A plaintext secret in the user's
// own file is used, with a warning; in an included (shared) file it is
// an error and ignored.
//
// Edits (cfgset) go to the user's file only, never into an include,
// through iniedit: one changed line, with comments, blank lines and
// order kept. The file is replaced atomically (os_write) and reloaded.
// The engine thread owns the Config. It polls the fingerprint of the
// files (type, size and mtime of the user's file and of every include
// target) at most once a second while it has work, and reloads when
// that changes. A load builds a new Ini in the other of two regions,
// so whatever outlives a load must be copied before the next one.
//
// URL includes ("include = https://host/team.ini", or with a pin,
// "include = URL sha256=HEX") load from a cache, <state>/includes/<the
// URL's SHA-256 in hex>, which holds the URL, the fetch time, the ETag
// and Last-Modified validators, then the text. A load never touches the
// network; fetches fill the cache, and a fetch that changes what a load
// would see reloads:
//
// - At startup (newconfig with fetch), a URL with no usable cached copy
//   is fetched while the caller waits, 30 s at most for all of them,
//   nested includes too. One with a copy loads from it, and the engine
//   refreshes it in the background once it starts.
// - While running, a URL is fetched again at most every 10 minutes,
//   when the engine has work, and at once on config.get {refresh:1},
//   whose reply waits for the fetches. Refreshes are conditional GETs.
// - A fetch connects within 10 s, ends within 30 s, follows at most 5
//   redirects (never from https to http), and takes at most 1 MiB. Up
//   to 4 run at once. A failure keeps the cached copy, with a warning at
//   the include line saying why and how old the copy is; with no copy,
//   the include is skipped with a warning.
// - A pin is the SHA-256 of the text. A fetched text that does not match
//   is an error and is ignored, never cached, so a cached copy that does
//   match stays in use; a cached copy that does not match is never used.
//
// Nothing a configuration says runs a command, and an included file's
// plaintext secrets are ignored, so a shared file, local or from the
// web, can at most point the harness at endpoints and models.
//
// Protocol, as engine.cpp answers it ("..." elides values):
//
//   {"cmd":"config.get","id":N}
//   {"ts":T,"ev":"reply","id":N,"ok":1,
//    "path":"/home/u/.config/harness/harness.ini",
//    "exists":1,
//    "rev":3,
//    "files":["/home/u/.config/harness/harness.ini", ...],
//    "settings":[{"kind":"endpoint","name":"local","key":"url",
//                 "value":"http://localhost/","file":"...","line":61,
//                 "default":0,"valid":1}, ...],
//    "schema":[{"kind":"endpoint","key":"url","type":"url","default":"",
//               "local":"","choices":[],"group":"endpoint",
//               "desc":"..."}, ...],
//    "kinds":[{"kind":"endpoint","named":1,"desc":"..."}, ...],
//    "includes":[{"url":"https://team.example/h.ini","pin":"",
//                 "sha256":"...","fetched":T,"fetching":0,
//                 "error":""}, ...],
//    "diags":[{"file":"...","line":7,"col":1,"severity":"warning",
//              "message":"..."}, ...]}
//
//   {"cmd":"config.get","id":N,"refresh":1}
//   (the same reply, once every URL include is fetched afresh)
//
//   {"cmd":"config.set","id":N,"kind":"endpoint","name":"local",
//    "key":"idle_timeout","value":"120"}
//   {"ts":T,"ev":"reply","id":N,"ok":1,"path":"...","line":63,"rev":4,
//    "diags":[...]}
//   {"ts":T,"ev":"reply","id":N,"ok":0,"err":"...","path":"...","line":0,
//    "rev":3,"diags":[...]}
//
// In config.get, path is where edits go ("" if there is no location),
// exists says whether that file was read, rev counts loads, and files
// lists every file read, in load order. Settings list every schema key
// of the top level and of the unnamed kinds ([guard], [tools],
// [instructions], [ui] and [host]), and of each named [endpoint],
// [model], [role] and [remote] section that exists, in schema order. A
// value is the one in effect as written, or its default ("default":1,
// with file "" and line 0, or where the key is set empty); "valid":0
// marks one ignored for its default. A file may be a URL, and then the
// setting also has "fetched", when its cached copy was fetched (Unix
// epoch ms). Includes list the URL includes of the load: the pin
// written, the SHA-256 of the copy in use, when it was fetched (0 for no
// copy), whether a fetch is under way or due, and why the latest one
// failed ("" if it did not). An endpoint's timeouts default by whether
// its URL is local (the schema's "local" defaults). A plaintext secret
// is reported as "(hidden)", so no reply ever carries one. The schema's
// types are string, int, number, seconds, bool, enum, url, secret
// (env:NAME), list (words of choices) and paths (separated by ;).
//
// In config.set, kind and name "" (or absent) are the top level, and a
// null value removes the key. The reply's line is where the key now is
// in the user's file (0 after a removal). A value invalid for its type,
// or a plaintext secret, is refused before anything is written, and an
// edit whose value is not the one in effect afterward fails, saying
// which file decides. Both replies carry the diagnostics of the
// configuration as now loaded.
//
// This is free and unencumbered software released into the public domain.

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const cfgseedbytes[] = {
    #embed "../assets/harness.ini"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

// The embedded seed file, as written on first run.
static Str cfgseed()
{
    return {(u8 *)cfgseedbytes, countof(cfgseedbytes)};
}

enum : i32 {
    CFG_STRING,
    CFG_INT,      // a whole number
    CFG_NUMBER,
    CFG_SECONDS,  // a number above 0, fractions allowed
    CFG_BOOL,     // true/false, yes/no, on/off, 1/0
    CFG_ENUM,     // one of the choices
    CFG_URL,      // http:// or https://
    CFG_SECRET,   // env:NAME
    CFG_LIST,     // words from the choices, separated by blanks or commas
    CFG_PATHS,    // absolute or ~ paths, separated by ;
    CFG_TIMES,    // seconds, each above 0, separated by blanks
    CFG_NTYPES,
};

static Str cfgtypenames[] = {
    "string", "int", "number", "seconds", "bool", "enum", "url", "secret",
    "list", "paths", "times",
};
static_assert(countof(cfgtypenames) == CFG_NTYPES);

// Section kinds. The top level is the kind "", without a name.
struct CfgKind {
    Str kind;
    b32 named;  // [kind NAME], else [kind]
    Str desc;
};

static CfgKind cfgkinds[] = {
    {"",         0, "Defaults for new sessions"},
    {"endpoint", 1, "An OpenAI-compatible server"},
    {"model",    1, "Settings for one model, named as its endpoint lists it"},
    {"role",     1, "Instructions, a model and tools for one kind of work"},
    {"guard",    0, "Where the file tools may go without asking"},
    {"tools",    0, "Background jobs, stopping commands, and search"},
    {"instructions", 0, "Instruction files for the agent: AGENTS.md and the like"},
    {"ui",       0, "How the GUI looks"},
    {"host",     0, "The engine as a background host (harness serve)"},
    {"remote",   1, "An engine on another machine, reached over ssh"},
};

struct CfgKey {
    Str kind;
    Str key;
    i32 type;
    Str dflt;     // as written in a file; empty: unset (not sent, learned)
    Str local;    // the default for a local endpoint, null if the same
    Str choices;  // CFG_ENUM and CFG_LIST: words separated by blanks
    Str group;    // for a settings window
    Str desc;
};

static CfgKey cfgschema[] = {
    {"", "endpoint", CFG_STRING, "", {}, {}, "general",
     "Endpoint for new sessions: an [endpoint] name or a URL"},
    {"", "model", CFG_STRING, "", {}, {}, "general",
     "Model for new sessions, as the endpoint names it"},

    {"endpoint", "url", CFG_URL, "", {}, {}, "endpoint",
     "Base URL: chat requests go to URL/v1/chat/completions"},
    {"endpoint", "key", CFG_SECRET, "", {}, {}, "endpoint",
     "API key sent as a bearer token, as env:NAME"},
    {"endpoint", "connect_timeout", CFG_SECONDS, "15", "5", {}, "timeouts",
     "Seconds to connect"},
    {"endpoint", "first_byte_timeout", CFG_SECONDS, "300", "1800", {},
     "timeouts", "Seconds until a response starts, model loading and "
     "prompt reading included"},
    {"endpoint", "idle_timeout", CFG_SECONDS, "90", {}, {}, "timeouts",
     "Seconds of silence once a response started, keep-alive pings "
     "counting as data"},
    {"endpoint", "progress_timeout", CFG_SECONDS, "600", {}, {}, "timeouts",
     "Seconds without new output or prompt progress"},
    {"endpoint", "slow_retry", CFG_TIMES, "60 120 300 600 900", {}, {},
     "retries", "Seconds between slow retries once quick ones fail, the "
     "last repeating"},
    {"endpoint", "slow_retry_budget", CFG_SECONDS, "7200", {}, {}, "retries",
     "Seconds of slow retries before a turn gives up"},
    {"endpoint", "max_concurrent", CFG_INT, "", {}, {}, "capacity",
     "Most requests in flight at once, by default no limit"},
    {"endpoint", "max_models", CFG_INT, "", {}, {}, "capacity",
     "Most different models in flight at once, by default a llama.cpp "
     "router's max_instances"},

    {"model", "reasoning", CFG_ENUM, "reasoning_content", {},
     "reasoning_content reasoning reasoning_text none", "model",
     "Field that returns the model's reasoning to it in later requests, "
     "or none"},
    {"model", "context", CFG_INT, "", {}, {}, "model",
     "Context limit in tokens, for a server that does not report it"},
    {"model", "compactor", CFG_STRING, "", {}, {}, "model",
     "Model that summarizes long conversations, MODEL or ENDPOINT/MODEL, "
     "by default this one"},
    {"model", "fallback", CFG_STRING, "", {}, {}, "model",
     "Models to use in turn when this one fails or hits a usage limit, "
     "each MODEL or ENDPOINT/MODEL"},
    {"model", "fallback_cooldown", CFG_SECONDS, "600", {}, {}, "model",
     "Seconds on a fallback before this model is tried again"},
    {"model", "reasoning_budget", CFG_INT, "", {}, {}, "model",
     "Most reasoning tokens in a response before the model is told to "
     "answer"},
    {"model", "edit_format", CFG_ENUM, "replace", {}, "replace patch hashline",
     "model", "How the model's edit tool changes files: replace (old and "
     "new text), patch (an apply_patch envelope) or hashline (line "
     "numbers checked against a tag that read shows)"},
    {"model", "max_concurrent", CFG_INT, "", {}, {}, "capacity",
     "Most requests for this model at once, by default the server's "
     "slots, else 4"},
    {"model", "temperature", CFG_NUMBER, "", {}, {}, "sampling",
     "Sampling temperature, sent only when set"},
    {"model", "top_p", CFG_NUMBER, "", {}, {}, "sampling",
     "Nucleus sampling threshold, sent only when set"},
    {"model", "top_k", CFG_INT, "", {}, {}, "sampling",
     "Top-k sampling, sent only when set"},
    {"model", "min_p", CFG_NUMBER, "", {}, {}, "sampling",
     "Min-p sampling threshold, sent only when set"},
    {"model", "presence_penalty", CFG_NUMBER, "", {}, {}, "sampling",
     "Presence penalty, sent only when set"},
    {"model", "frequency_penalty", CFG_NUMBER, "", {}, {}, "sampling",
     "Frequency penalty, sent only when set"},
    {"model", "repeat_penalty", CFG_NUMBER, "", {}, {}, "sampling",
     "Repetition penalty (llama.cpp), sent only when set"},
    {"model", "seed", CFG_INT, "", {}, {}, "sampling",
     "Sampling seed, sent only when set"},
    {"model", "max_tokens", CFG_INT, "", {}, {}, "sampling",
     "Most tokens in one response, sent only when set"},

    {"role", "description", CFG_STRING, "", {}, {}, "role",
     "What the role is for, for an agent choosing one for a subagent"},
    {"role", "prompt", CFG_STRING, "", {}, {}, "role",
     "Instructions added to the system prompt"},
    {"role", "endpoint", CFG_STRING, "", {}, {}, "role",
     "Endpoint for the role, by default the session's, or a subagent's parent's"},
    {"role", "model", CFG_STRING, "", {}, {}, "role",
     "Model for the role, by default the session's, or a subagent's parent's"},
    {"role", "tools", CFG_LIST,
     "read write edit shell grep glob todo_write jobs job_output job_kill task", {},
     "read write edit shell grep glob todo_write jobs job_output job_kill task",
     "role", "Tools the role may use"},

    {"guard", "roots", CFG_PATHS, "", {}, {}, "guard",
     "More directories the file tools may use, separated by ;"},
    {"guard", "ask_timeout", CFG_SECONDS, "30", {}, {}, "guard",
     "Seconds to wait for an answer when a tool asks to leave the roots, "
     "then deny"},

    {"tools", "job_timeout", CFG_SECONDS, "1800", {}, {}, "tools",
     "Seconds a background job may run, unless the agent asks for less "
     "or more"},
    {"tools", "job_timeout_max", CFG_SECONDS, "7200", {}, {}, "tools",
     "Most seconds the agent may ask for a background job to run"},
    {"tools", "auto_resume", CFG_INT, "5", {}, {}, "tools",
     "Turns a session may start by itself in an hour, to report background "
     "jobs that ended while it was idle; 0 for none"},
    {"tools", "stop_grace", CFG_SECONDS, "", {}, {}, "tools",
     "Seconds between asking a command to stop (SIGTERM, or CTRL_BREAK on "
     "Windows) and killing it (default: 2, or 3 on Windows)"},
    {"tools", "use_rg", CFG_BOOL, "true", {}, {}, "tools",
     "Search with ripgrep (rg on PATH) when found, else the built-in search"},
    {"tools", "task_budget", CFG_INT, "200", {}, {}, "tools",
     "Model requests a subagent may make before it is told to wrap up and "
     "report; it is stopped at half as many again"},

    {"instructions", "files", CFG_STRING, "AGENTS.md CLAUDE.md", {}, {},
     "instructions", "File names to load from each directory, the first "
     "found in each, separated by blanks"},

    {"ui", "theme", CFG_ENUM, "system", {}, "system dark light", "ui",
     "Colors: dark, light, or system to follow the operating system"},

    {"host", "idle_exit", CFG_SECONDS, "600", {}, {}, "host",
     "Seconds a host waits with no client connected and nothing running, "
     "then exits"},
    {"host", "grace", CFG_SECONDS, "10800", {}, {}, "host",
     "Seconds a host keeps the place of a client whose connection broke, "
     "its records kept for when it comes back"},

    {"remote", "ssh", CFG_STRING, "", {}, {}, "remote",
     "Where ssh reaches the machine: [user@]host, or a Host of ~/.ssh/config"},
    {"remote", "path", CFG_STRING, "harness", {}, {}, "remote",
     "The harness executable there, a path or a name on its PATH"},
    {"remote", "state", CFG_STRING, "", {}, {}, "remote",
     "The state directory there, by default its own"},
    {"remote", "config", CFG_STRING, "", {}, {}, "remote",
     "The configuration file there, by default its own"},
};

// Value judgments
enum : i32 {
    CFG_OK,
    CFG_ODD,  // used, with a warning
    CFG_BAD,  // ignored for the default, with an error
};

enum : i64 {
    CFG_POLL      = 1000000,      // os_ticks between checks for changed files
    CFG_FETCHTIME = 30*1000000,   // os_ticks a fetch may take, redirects too
    CFG_REFRESH   = 600*1000000,  // os_ticks between refreshes, by default
    CFG_GRACE     = 2*1000000,    // os_ticks for a cancelled fetch to end
};

enum {
    CFG_FETCHMAX  = 1 << 20,  // bytes of a fetched include
    CFG_CONNECTMS = 10000,
    CFG_REDIRECTS = 5,
    CFG_FETCHES   = 4,        // fetches at once
};

// How a Config may use the network for URL includes.
enum : i32 {
    CFG_NETNONE,  // never: cached copies only
    CFG_NETWAIT,  // at startup: fetch what has no copy, and wait for it
    CFG_NETLIVE,  // in the engine: fetch in the background, and refresh
};

struct CfgSlot;

// A URL included in some load, kept for the Config's lifetime.
struct CfgUrl {
    CfgUrl  *next;
    Str      url;        // canonical (iniurlclean)
    CfgSlot *slot;       // its fetch under way, or null
    i64      seen;       // the rev of the latest load that included it
    i64      attempted;  // os_ticks when its latest fetch started
    i64      failedat;   // os_time of its latest fetch, if that failed
    i64      fetched;    // os_time of the copy in use, 0 if none
    Str      want;       // the pin in the latest load, or null
    Str      pin;        // the pin its latest fetch checked, or null
    Str      sha;        // SHA-256 of its cached copy, or null
    b32      tried;      // a fetch started since the Config was made
    b32      due;        // to fetch when a slot is free
    b32      cached;     // the latest load used a cached copy
    b32      pinbad;     // its latest fetch failed its pin
    iz       errlen;
    u8       err[240];   // why its latest fetch failed, if it did
};

// A fetch under way, which may follow redirects.
struct CfgSlot {
    CfgUrl *u;           // null when free
    Http   *http;        // null if the request could not start
    u64     id;
    i64     began;       // os_ticks
    i64     deadline;    // os_ticks: cancelled then
    i64     cutoff;      // os_ticks it ran until cancelled, 0 if not
    i32     redirects;
    i32     status;
    Str     pin;         // to check, in pinbuf
    u8      pinbuf[64];
    Str     at;          // the URL requested, after redirects, in atbuf
    u8      atbuf[2048];
    Str     oldetag;     // validators of the cached copy, if usable
    u8      oldetagbuf[256];
    Str     oldmod;
    u8      oldmodbuf[64];
    Str     etag;        // the response's validators
    u8      etagbuf[256];
    Str     mod;
    u8      modbuf[64];
    Str     location;    // a redirect's, in locbuf; null if missing or long
    u8      locbuf[2048];
    Str     err;         // why the request could not start, in errbuf
    u8      errbuf[256];
    u8     *body;        // CFG_FETCHMAX bytes
    iz      len;
};

struct Config {
    Os         *os;
    Str         path;        // the user's file, normalized; null if none
    b32         seed;        // write the seed where the file is missing
    Str         note;        // a warning about the file for every load
    Region     *regions[2];  // loads alternate between these
    i32         cur;
    Ini        *ini;         // the current load, never null
    IniFile    *user;        // the user's file in ini, null if unread
    Slice<Str>  watch;       // the files whose stamps make the fingerprint
    u64         stamp;       // fingerprint at the last load, 0 to reload
    i64         polled;      // os_ticks of the last check
    i64         rev;         // loads so far

    // URL includes
    Str         cache;       // <state>/includes, null for none
    i32         net;         // CFG_NETNONE, CFG_NETWAIT or CFG_NETLIVE
    Mailbox    *mbox;        // for the fetches' records
    Region     *keepregion;  // backs keep
    Arena       keep;        // URLs and slots, for the Config's lifetime
    CfgUrl     *urls;        // in order of first inclusion
    CfgUrl     *lasturl;
    CfgSlot    *slots;       // CFG_FETCHES, made with the first fetch
    u64         nextid;
    i64         every;       // os_ticks between refreshes while live
    i64         waitby;      // os_ticks: no fetch at startup goes past it
    i64         loading;     // the rev of the load under way
};


// Schema

static CfgKind *cfgkind(Str kind)
{
    for (iz i = 0; i < countof(cfgkinds); i++) {
        if (cfgkinds[i].kind == kind) {
            return &cfgkinds[i];
        }
    }
    return 0;
}

static CfgKey *cfgkey(Str kind, Str key)
{
    for (iz i = 0; i < countof(cfgschema); i++) {
        if (cfgschema[i].kind==kind && cfgschema[i].key==key) {
            return &cfgschema[i];
        }
    }
    return 0;
}

// The next word of a list, separated by blanks or commas. Advances *s.
static Str cfgnextword(Str *s)
{
    iz i = 0;
    for (; i<s->len && (whitespace((*s)[i]) || (*s)[i]==','); i++) {}
    iz j = i;
    for (; j<s->len && !whitespace((*s)[j]) && (*s)[j]!=','; j++) {}
    Str w = span(s->data+i, s->data+j);
    *s = cuthead(*s, j);
    return w;
}

static b32 cfghasword(Str words, Str w)
{
    for (Str rest = words; rest.len;) {
        if (cfgnextword(&rest) == w) {
            return 1;
        }
    }
    return 0;
}

// 1 for true, 0 for false, -1 for neither.
static i32 cfgbool(Str v)
{
    static Str const yes[] = {"true", "yes", "on", "1"};
    static Str const no[]  = {"false", "no", "off", "0"};
    for (iz i = 0; i < countof(yes); i++) {
        if (equalfold(v, yes[i])) return 1;
        if (equalfold(v, no[i]))  return 0;
    }
    return -1;
}

static b32 cfgenvname(Str s)
{
    if (!s.len || digit(s[0])) {
        return 0;
    }
    for (iz i = 0; i < s.len; i++) {
        if (!alnum(s[i]) && s[i]!='_') {
            return 0;
        }
    }
    return 1;
}

static b32 cfgtilde(Str p)
{
    return p=="~" || startswith(p, "~/") || startswith(p, "~\\");
}

// How a value fares as a setting of its key. A shared value comes from
// an included file. An empty value means the default, and is fine.
static i32 cfgjudge(CfgKey *k, Str v, b32 shared)
{
    v = trim(v);
    if (!v.len) {
        return CFG_OK;
    }
    switch (k->type) {
    case CFG_STRING:
        return CFG_OK;
    case CFG_INT:
        return parseint(v).ok ? CFG_OK : CFG_BAD;
    case CFG_NUMBER:
        return parsefloat(v).ok ? CFG_OK : CFG_BAD;
    case CFG_SECONDS: {
        Float f = parsefloat(v);
        return f.ok && f.val>0 && f.val<1e7 ? CFG_OK : CFG_BAD;
    }
    case CFG_BOOL:
        return cfgbool(v)>=0 ? CFG_OK : CFG_BAD;
    case CFG_ENUM:
        return cfghasword(k->choices, v) ? CFG_OK : CFG_ODD;
    case CFG_URL:
        return parseurl(v).ok ? CFG_OK : CFG_BAD;
    case CFG_SECRET:
        if (startswith(v, "env:")) {
            return cfgenvname(trim(cuthead(v, 4))) ? CFG_OK : CFG_BAD;
        }
        return shared ? CFG_BAD : CFG_ODD;
    case CFG_LIST:
        for (Str rest = v; rest.len;) {
            Str w = cfgnextword(&rest);
            if (w.len && !cfghasword(k->choices, w)) {
                return CFG_ODD;
            }
        }
        return CFG_OK;
    case CFG_PATHS:
        for (Str rest = v; rest.len;) {
            Cut c = cut(rest, ';');
            Str p = trim(c.head);
            rest = c.tail;
            if (p.len && !iniroot(p) && !cfgtilde(p)) {
                return CFG_ODD;
            }
        }
        return CFG_OK;
    case CFG_TIMES:
        for (Str rest = v; rest.len;) {
            Str   w = cfgnextword(&rest);
            Float f = parsefloat(w);
            if (w.len && (!f.ok || f.val<=0 || f.val>=1e7)) {
                return CFG_BAD;
            }
        }
        return CFG_OK;
    }
    return CFG_OK;
}

// Why a value is odd or bad for its key, for a diagnostic.
static Str cfgproblem(Arena *a, CfgKey *k, Str v, b32 shared)
{
    v = trim(v);
    Buf b(a, 128);
    switch (k->type) {
    case CFG_INT:     print(&b, "expected a whole number");             break;
    case CFG_NUMBER:  print(&b, "expected a number");                   break;
    case CFG_SECONDS: print(&b, "expected seconds, a number above 0");  break;
    case CFG_TIMES:   print(&b, "expected seconds, numbers above 0");   break;
    case CFG_BOOL:    print(&b, "expected true or false");              break;
    case CFG_URL:
        print(&b, "expected a URL starting with http:// or https://");
        break;
    case CFG_ENUM:
    case CFG_LIST: {
        Str bad = v;
        for (Str rest = v; k->type==CFG_LIST && rest.len;) {
            Str w = cfgnextword(&rest);
            if (w.len && !cfghasword(k->choices, w)) {
                bad = w;
                break;
            }
        }
        print(&b, "unknown value \"");
        print(&b, bad);
        print(&b, k->type==CFG_ENUM ? Str("\", expected one of: ")
                                    : Str("\", expected any of: "));
        iz n = 0;
        for (Str rest = k->choices; rest.len;) {
            Str w = cfgnextword(&rest);
            print(&b, n++ ? Str(", ") : Str(""));
            print(&b, w);
        }
    } break;
    case CFG_SECRET:
        if (startswith(v, "env:")) {
            print(&b, "expected env:NAME, with NAME an environment variable");
        } else if (shared) {
            print(&b, "a plaintext secret in an included file is ignored: "
                      "write env:NAME and set the variable");
        } else {
            print(&b, "a plaintext secret: write env:NAME and set the "
                      "variable, so that this file can be shared");
        }
        return finish(&b);
    case CFG_PATHS:
        for (Str rest = v; rest.len;) {
            Cut c = cut(rest, ';');
            Str p = trim(c.head);
            rest = c.tail;
            if (p.len && !iniroot(p) && !cfgtilde(p)) {
                print(&b, "relative path \"");
                print(&b, p);
                print(&b, "\": use an absolute path or one starting with ~");
                break;
            }
        }
        return finish(&b);
    }
    if (cfgjudge(k, v, shared) == CFG_BAD) {
        print(&b, k->dflt.len ? Str(", so the default applies")
                              : Str(", so this is ignored"));
    }
    return finish(&b);
}

// Edit distance between short words, or a large number for long ones.
static iz cfgdistance(Str a, Str b)
{
    if (a.len>32 || b.len>32) {
        return 99;
    }
    iz row[33];
    for (iz j = 0; j <= b.len; j++) {
        row[j] = j;
    }
    for (iz i = 1; i <= a.len; i++) {
        iz diag = row[0];
        row[0] = i;
        for (iz j = 1; j <= b.len; j++) {
            iz up   = row[j];
            iz best = diag + (a[i-1]!=b[j-1]);
            best    = up+1<best ? up+1 : best;
            best    = row[j-1]+1<best ? row[j-1]+1 : best;
            diag    = up;
            row[j]  = best;
        }
    }
    return row[b.len];
}


// Values in effect

// The value of a setting as the harness uses it: as written, or its
// default when unset, empty, or bad for its type, or when a kind that
// takes names has none (as for an endpoint given by URL alone). For a
// key outside the schema, the raw value or null. With local, a local
// endpoint's default.
static Str cfgget(Config *c, Str kind, Str name, Str key, b32 local)
{
    CfgKey   *k = cfgkey(kind, key);
    CfgKind  *n = cfgkind(kind);
    IniEntry *e = n && n->named && !name.len ? 0 :
                  inilookup(c->ini, kind, name, key);
    if (!k) {
        return e ? e->value : Str{};
    }
    Str dflt = local && k->local.data ? k->local : k->dflt;
    if (!e || !trim(e->value).len ||
            cfgjudge(k, e->value, e->file!=c->user)==CFG_BAD) {
        return dflt;
    }
    return e->value;
}

// A named section's setting only where a file sets it, and validly for
// a schema key: null when unset, empty or bad, for a caller with
// defaults of its own (a built-in role's).
static Str cfgown(Config *c, Str kind, Str name, Str key)
{
    CfgKey   *k = cfgkey(kind, key);
    IniEntry *e = name.len ? inilookup(c->ini, kind, name, key) : 0;
    if (!e || !trim(e->value).len ||
            (k && cfgjudge(k, e->value, e->file!=c->user)==CFG_BAD)) {
        return {};
    }
    return e->value;
}

// A seconds setting (CFG_SECONDS) in milliseconds, at least 1.
static i64 cfgms(Config *c, Str kind, Str name, Str key, b32 local)
{
    Float f  = parsefloat(trim(cfgget(c, kind, name, key, local)));
    i64   ms = f.ok ? (i64)(f.val * 1000) : 0;
    return ms>0 ? ms : 1;
}

// A secret setting, resolved: env:NAME reads the environment. Null when
// unset, when the variable is unset, or when ignored (cfgjudge).
static Str cfgsecret(Config *c, Arena *perm, Str kind, Str name, Str key)
{
    Str v = trim(cfgget(c, kind, name, key, 0));
    if (startswith(v, "env:")) {
        return os_getenv(c->os, perm, trim(cuthead(v, 4)));
    }
    return v.len ? clone(perm, v) : Str{};
}

// The paths of a paths setting (CFG_PATHS), with a leading ~ replaced by
// the home directory when one is known, in perm.
static Slice<Str> cfgpaths(Config *c, Arena *perm, Str kind, Str name,
                           Str key, Str home)
{
    Slice<Str> r = {};
    for (Str rest = cfgget(c, kind, name, key, 0); rest.len;) {
        Cut cp = cut(rest, ';');
        Str p  = trim(cp.head);
        rest = cp.tail;
        if (!p.len) {
            continue;
        } else if (cfgtilde(p) && home.len) {
            p = concat(perm, home, cuthead(p, 1));
        } else {
            p = clone(perm, p);
        }
        r = push(perm, r, p);
    }
    return r;
}


// Validation

// Report at a byte offset of a line, counting against the file's limit
// as the reader does (inireport): the report after INI_MAXDIAGS becomes
// a warning that the rest were suppressed, and later ones are dropped,
// so msg may be null when the file is muted.
static void cfgreport(Ini *ini, Arena *perm, IniFile *f, IniLine *l,
                      i32 severity, iz off, Str msg)
{
    if (f->ndiags > INI_MAXDIAGS) {
        return;
    } else if (f->ndiags++ == INI_MAXDIAGS) {
        severity = INI_WARNING;
        off      = -1;
        msg      = "too many diagnostics, rest suppressed";
    }
    inidiag(ini, perm, f->path, l->num, off+1, severity, msg);
}

// Whether a file has used up its diagnostics, so that a caller can skip
// formatting a message cfgreport would not keep.
static b32 cfgmuted(IniFile *f)
{
    return f->ndiags >= INI_MAXDIAGS;
}

static void cfgsectionline(Ini *ini, Arena *perm, IniFile *f, IniLine *l)
{
    CfgKind *k       = cfgkind(l->key);
    b32      unnamed = k && k->named && !l->value.len;
    b32      named   = k && !k->named && l->value.len;
    b32      nourl   = k && l->key=="endpoint" && l->value.len &&
                       !inientry(l->section, "url");
    if (k && !unnamed && !named && !nourl) {
        return;
    } else if (cfgmuted(f)) {
        cfgreport(ini, perm, f, l, INI_WARNING, 0, {});
        return;
    }

    Buf b(perm, 128);
    if (!k) {
        print(&b, "unknown section kind \"");
        print(&b, l->key);
        print(&b, "\", expected endpoint, model, role, guard, tools, "
                  "instructions or ui");
    } else if (unnamed) {
        print(&b, "a section needs a name, as in [");
        print(&b, l->key);
        print(&b, " NAME], or its settings are unused");
    } else if (named) {
        print(&b, "[");
        print(&b, l->key);
        print(&b, "] takes no name, or its settings are unused");
    } else {
        print(&b, "endpoint ");
        print(&b, l->value);
        print(&b, " has no url");
    }
    iz off = named ? l->valbeg : l->keybeg;
    cfgreport(ini, perm, f, l, INI_WARNING, off, finish(&b));
}

static void cfgkeyline(Ini *ini, Arena *perm, IniFile *f, IniFile *user,
                       IniLine *l)
{
    IniSection *s = l->section;
    CfgKind    *k = cfgkind(s->kind);
    if (!k || k->named!=(s->name.len>0)) {
        return;  // reported at the header
    }

    CfgKey *key = cfgkey(s->kind, l->key);
    if (!key && cfgmuted(f)) {
        cfgreport(ini, perm, f, l, INI_WARNING, 0, {});
        return;
    } else if (!key) {
        Buf b(perm, 128);
        print(&b, "unknown key \"");
        print(&b, l->key);
        if (s->kind.len) {
            print(&b, "\" in [");
            print(&b, s->kind);
            print(&b, "]");
        } else {
            print(&b, "\" at the top level");
        }
        CfgKey *near = 0;
        iz      best = l->key.len<=4 ? 1 : 2;
        for (iz i = 0; i < countof(cfgschema); i++) {
            CfgKey *o = &cfgschema[i];
            iz      d = cfgdistance(l->key, o->key);
            if (o->kind==s->kind && d<=best) {
                near = o;
                best = d - 1;
            }
        }
        if (near) {
            print(&b, "; did you mean ");
            print(&b, near->key);
            putbyte(&b, '?');
        } else {
            for (iz i = 0; i < countof(cfgschema); i++) {
                CfgKey *o = &cfgschema[i];
                if (o->key==l->key && o->kind.len) {
                    print(&b, "; it belongs in [");
                    print(&b, o->kind);
                    print(&b, cfgkind(o->kind)->named ? Str(" NAME]")
                                                      : Str("]"));
                    break;
                }
            }
        }
        cfgreport(ini, perm, f, l, INI_WARNING, l->keybeg, finish(&b));
        return;
    }

    b32 shared = f != user;
    i32 j      = cfgjudge(key, l->value, shared);
    if (j != CFG_OK) {
        Str msg = {};
        i32 sev = j==CFG_BAD ? INI_ERROR : INI_WARNING;
        if (!cfgmuted(f)) {
            msg = cfgproblem(perm, key, l->value, shared);
        }
        cfgreport(ini, perm, f, l, sev, l->valbeg, msg);
    }
}

// Check a load against the schema, adding diagnostics to it. The user's
// file may be null.
static void cfgvalidate(Config *c, Ini *ini, IniFile *user, Arena *perm)
{
    for (IniFile *f = ini->files; f; f = f->next) {
        for (iz i = 0; i<f->lines.len && f->ndiags<=INI_MAXDIAGS; i++) {
            IniLine *l = &f->lines[i];
            if (l->type == INI_SECTION) {
                cfgsectionline(ini, perm, f, l);
            } else if (l->type==INI_KEY && l->section) {
                cfgkeyline(ini, perm, f, user, l);
            }
        }
    }

    // Settings in effect: endpoints that do not exist, unset variables,
    // and the user's settings that a later include overrides
    for (IniSection *s = ini->sections; s; s = s->next) {
        for (IniEntry *e = s->keys; e; e = e->next) {
            CfgKey *k = cfgkey(s->kind, e->key);
            Str     v = trim(e->value);
            if (k && k->key=="endpoint" && v.len && !parseurl(v).ok &&
                    !inientry(inisection(ini, "endpoint", v), "url")) {
                Str msg = {};
                if (!cfgmuted(e->file)) {
                    msg = concat(perm, "no [endpoint ", v);
                    msg = concat(perm, msg, "] with a url, and not a URL");
                }
                cfgreport(ini, perm, e->file, e->line, INI_WARNING,
                          e->line->valbeg, msg);
            }
            if (k && k->type==CFG_SECRET && startswith(v, "env:") &&
                    cfgjudge(k, v, 0)==CFG_OK) {
                Str name = trim(cuthead(v, 4));
                if (!os_getenv(c->os, perm, name).data) {
                    Str msg = {};
                    if (!cfgmuted(e->file)) {
                        msg = concat(perm, "environment variable ", name);
                        msg = concat(perm, msg, " is not set");
                    }
                    cfgreport(ini, perm, e->file, e->line, INI_WARNING,
                              e->line->valbeg, msg);
                }
            }
            for (IniEntry *o = e->older; user && e->file!=user && o;
                 o = o->older) {
                if (o->file == user) {
                    Str msg = {};
                    if (!cfgmuted(user)) {
                        Buf b(perm, 128);
                        print(&b, "overridden by ");
                        print(&b, e->file->path);
                        putbyte(&b, ':');
                        print(&b, (i64)e->line->num);
                        print(&b, ", included after this line");
                        msg = finish(&b);
                    }
                    cfgreport(ini, perm, user, o->line, INI_WARNING,
                              o->line->keybeg, msg);
                    break;
                }
            }
        }
    }
}


// SHA-256, for pins and cache file names

static u32 cfgrotr(u32 x, i32 n)
{
    return x>>n | x<<(32 - n);
}

static void cfgshablock(u32 *h, u8 *p)
{
    static u32 const k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    u32 w[64];
    for (i32 i = 0; i < 16; i++) {
        w[i] = (u32)p[4*i]<<24 | (u32)p[4*i+1]<<16 | (u32)p[4*i+2]<<8 |
               (u32)p[4*i+3];
    }
    for (i32 i = 16; i < 64; i++) {
        u32 s0 = cfgrotr(w[i-15], 7) ^ cfgrotr(w[i-15], 18) ^ w[i-15]>>3;
        u32 s1 = cfgrotr(w[i-2], 17) ^ cfgrotr(w[i-2], 19) ^ w[i-2]>>10;
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 v[8];  // a through h
    for (i32 i = 0; i < 8; i++) {
        v[i] = h[i];
    }
    for (i32 i = 0; i < 64; i++) {
        u32 a  = v[0];
        u32 e  = v[4];
        u32 s1 = cfgrotr(e, 6) ^ cfgrotr(e, 11) ^ cfgrotr(e, 25);
        u32 ch = (e & v[5]) ^ (~e & v[6]);
        u32 t1 = v[7] + s1 + ch + k[i] + w[i];
        u32 s0 = cfgrotr(a, 2) ^ cfgrotr(a, 13) ^ cfgrotr(a, 22);
        u32 mj = (a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]);
        for (i32 j = 7; j > 0; j--) {
            v[j] = v[j-1];
        }
        v[4] += t1;
        v[0]  = t1 + s0 + mj;
    }
    for (i32 i = 0; i < 8; i++) {
        h[i] += v[i];
    }
}

// SHA-256 of s, as 64 lowercase hex digits.
static Str cfgsha256(Arena *perm, Str s)
{
    u32 h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    iz full = s.len - s.len%64;
    for (iz i = 0; i < full; i += 64) {
        cfgshablock(h, s.data+i);
    }
    u8 tail[128] = {};
    iz rest = s.len - full;
    iz n    = rest<56 ? 64 : 128;
    copybytes(tail, s.data+full, rest);
    tail[rest] = 0x80;
    u64 bits = (u64)s.len * 8;
    for (i32 i = 0; i < 8; i++) {
        tail[n-1-i] = (u8)(bits >> (8*i));
    }
    for (iz i = 0; i < n; i += 64) {
        cfgshablock(h, tail+i);
    }

    Buf b(perm, 64);
    for (i32 i = 0; i < 8; i++) {
        printhex(&b, h[i], 8);
    }
    return finish(&b);
}


// URL includes
//
// The reader asks for an include's text through cfgsource, which reads
// the cache only. Fetches run in slots, each request on a platform
// thread posting to the Config's mailbox: its own at startup, where
// newconfig waits on it (cfgwait), then the engine's (cfgattach), whose
// thread hands over the records (cfgrecord) and wakes for deadlines
// (cfgnext, cfgtimeout). A fetch that ends with something new for a
// load reloads, and every load marks what is due and starts it, so
// nested includes follow their parents.

static i32  get32(Str);
static void cfgload(Config *, Arena);

// An age in milliseconds as "5 seconds", "1 minute", "3 hours" or "2
// days": whole units, the largest that fits.
static void printcfgage(Buf *b, i64 ms)
{
    static Str const names[] = {"second", "minute", "hour", "day"};
    static i64 const sizes[] = {1000, 60*1000, 3600*1000, 86400*1000};
    i32 u = 3;
    for (; u>0 && ms<sizes[u]; u--) {}
    i64 n = (ms<0 ? 0 : ms) / sizes[u];
    print(b, n);
    putbyte(b, ' ');
    print(b, names[u]);
    if (n != 1) {
        putbyte(b, 's');
    }
}

// A cached copy, as stored: "harness include 1", then "key value" lines
// for url, fetched (Unix epoch ms), and the etag and modified validators
// when the server gave them, a blank line, and the text.
struct CfgCache {
    Str url;
    Str etag;
    Str mod;
    Str body;
    i64 fetched;
    b32 ok;
};

static Str cfgcachepath(Config *c, Arena *a, Str url)
{
    return join(a, c->cache, cfgsha256(a, url));
}

// The cached copy of a URL, if the cache has a well-formed one.
static CfgCache cfgreadcache(Config *c, Arena *perm, Str url)
{
    CfgCache r = {};
    if (!c->cache.len) {
        return r;
    }
    Str  path = cfgcachepath(c, perm, url);
    Read rd   = os_read(c->os, perm, path, CFG_FETCHMAX + (1<<14));
    if (rd.err) {
        return r;
    }
    Cut line = cut(rd.data, '\n');
    if (!(line.head == "harness include 1")) {
        return r;
    }
    for (;;) {
        line = cut(line.tail, '\n');
        if (!line.ok) {
            return r;  // no blank line: cut short
        } else if (!line.head.len) {
            break;
        }
        Cut kv = cut(line.head, ' ');
        if (kv.head == "url") {
            r.url = kv.tail;
        } else if (kv.head == "fetched") {
            r.fetched = parseint(kv.tail).val;
        } else if (kv.head == "etag") {
            r.etag = kv.tail;
        } else if (kv.head == "modified") {
            r.mod = kv.tail;
        }
    }
    r.body = line.tail.data ? line.tail : Str("");
    r.ok   = r.url==url && r.body.len<=CFG_FETCHMAX;
    return r;
}

// Replace a URL's cached copy atomically.
static b32 cfgwritecache(Config *c, Arena scratch, CfgCache *k)
{
    iz  cap = 256 + k->url.len + k->etag.len + k->mod.len + k->body.len;
    Buf b(&scratch, cap);
    print(&b, "harness include 1\nurl ");
    print(&b, k->url);
    print(&b, "\nfetched ");
    print(&b, k->fetched);
    if (k->etag.len) {
        print(&b, "\netag ");
        print(&b, k->etag);
    }
    if (k->mod.len) {
        print(&b, "\nmodified ");
        print(&b, k->mod);
    }
    print(&b, "\n\n");
    print(&b, k->body);
    Str text = finish(&b);
    Str path = cfgcachepath(c, &scratch, k->url);
    return os_mkdirs(c->os, scratch, c->cache) &&
           os_write(c->os, scratch, path, text);
}

// A URL's record, or with add a new one at the end. Null if there is
// none, or no room.
static CfgUrl *cfgurl(Config *c, Str url, b32 add)
{
    for (CfgUrl *u = c->urls; u; u = u->next) {
        if (u->url == url) {
            return u;
        }
    }
    if (!add || !fits(&c->keep, (iz)sizeof(CfgUrl) + url.len + 64)) {
        return 0;
    }
    CfgUrl *u = alloc<CfgUrl>(&c->keep);
    u->url = clone(&c->keep, url);
    if (c->lasturl) {
        c->lasturl->next = u;
    } else {
        c->urls = u;
    }
    c->lasturl = u;
    return u;
}

// Keep a copy of s, a pin or a hash, in *dst unless it is there already.
static void cfgkeep(Config *c, Str *dst, Str s)
{
    if (*dst == s && !dst->data==!s.data) {
        return;
    } else if (!s.data) {
        *dst = {};
    } else if (fits(&c->keep, s.len)) {
        *dst = clone(&c->keep, s);
    }
}

// Copy s into a slot's buffer of cap bytes as *dst, or set *dst null
// and return 0 if it is empty or too long.
static b32 cfghold(Str *dst, u8 *buf, iz cap, Str s)
{
    *dst = {};
    if (!s.len || s.len>cap) {
        return 0;
    }
    copybytes(buf, s.data, s.len);
    *dst = {buf, s.len};
    return 1;
}

// The text of a URL include for the reader (Ini.fetch): its cached copy
// if there is one, and it matches the pin, with a diagnostic if the
// latest fetch failed; otherwise none, saying why.
static IniSource cfgsource(void *ctx, Arena *perm, Str url, Str pin)
{
    Config   *c = (Config *)ctx;
    IniSource r = {};
    r.severity  = INI_WARNING;
    CfgUrl   *u = c->cache.len ? cfgurl(c, url, 1) : 0;
    if (!u) {
        Buf b(perm, 64 + url.len);
        print(&b, "cannot include ");
        print(&b, url);
        print(&b, c->cache.len ? Str(" (too many URLs)")
                               : Str(" (no state directory for a cache)"));
        r.msg = finish(&b);
        return r;
    }
    u->seen = c->loading;
    cfgkeep(c, &u->want, pin);

    CfgCache k   = cfgreadcache(c, perm, url);
    Str      sha = k.ok ? cfgsha256(perm, k.body) : Str{};
    b32      use = k.ok && (!pin.data || sha==pin);
    u->cached  = use;
    u->fetched = use ? k.fetched : 0;
    cfgkeep(c, &u->sha, use ? sha : Str{});

    // A failure since the copy was fetched (by this process or another),
    // unless against another pin
    b32 failed = u->errlen && (!use || u->failedat>=k.fetched) &&
                 (!u->pinbad || u->pin==pin);
    Buf b(perm, 256 + url.len);
    if (failed && u->pinbad) {
        r.severity = INI_ERROR;
        print(&b, url);
        print(&b, " does not match its pin: ");
        print(&b, Str{u->err, u->errlen});
    } else if (failed) {
        print(&b, "cannot fetch ");
        print(&b, url);
        print(&b, " (");
        print(&b, Str{u->err, u->errlen});
        putbyte(&b, ')');
    } else if (!use && k.ok) {
        print(&b, "the cached copy of ");
        print(&b, url);
        print(&b, " does not match its pin");
    } else if (!use) {
        print(&b, url);
        print(&b, " is not fetched yet");
    }
    if (b.len && use) {
        print(&b, "; using the copy fetched ");
        printcfgage(&b, os_time(c->os) - k.fetched);
        print(&b, " ago");
    } else if (b.len) {
        print(&b, ", include skipped");
    }
    r.msg  = b.len ? finish(&b) : Str{};
    r.data = use ? k.body : Str{};
    return r;
}

// A free slot, made with the first fetch, or null.
static CfgSlot *cfgfreeslot(Config *c)
{
    if (!c->slots) {
        iz need = CFG_FETCHES*((iz)sizeof(CfgSlot) + CFG_FETCHMAX + 64);
        if (!fits(&c->keep, need)) {
            return 0;
        }
        c->slots = alloc<CfgSlot>(&c->keep, CFG_FETCHES);
        for (i32 i = 0; i < CFG_FETCHES; i++) {
            c->slots[i].body = allocbytes(&c->keep, CFG_FETCHMAX);
        }
    }
    for (i32 i = 0; i < CFG_FETCHES; i++) {
        if (!c->slots[i].u) {
            return &c->slots[i];
        }
    }
    return 0;
}

// Request a slot's URL (at), or should the request not start, arrange
// for it to fail at the next cfgtimeout.
static void cfgsend(Config *c, CfgSlot *s, Arena scratch)
{
    i64 now  = os_ticks(c->os);
    i64 left = (s->deadline - now) / 1000;
    i64 most = CFG_CONNECTMS;
    Buf h(&scratch, 1024);
    print(&h, "User-Agent: harness/" HARNESS_VERSION "\r\nAccept: */*\r\n");
    if (s->oldetag.len) {
        print(&h, "If-None-Match: ");
        print(&h, s->oldetag);
        print(&h, "\r\n");
    }
    if (s->oldmod.len) {
        print(&h, "If-Modified-Since: ");
        print(&h, s->oldmod);
        print(&h, "\r\n");
    }

    HttpReq r = {};
    r.method    = "GET";
    r.url       = s->at;
    r.headers   = finish(&h);
    r.connectms = (i32)(left<1 ? 1 : left<most ? left : most);
    r.readms    = 0;  // the deadline covers it
    r.maxbytes  = CFG_FETCHMAX;
    r.mbox      = c->mbox;
    r.id        = s->id = (u64)1<<63 | ++c->nextid;
    s->status   = 0;
    s->len      = 0;
    s->etag     = {};
    s->mod      = {};
    s->location = {};
    s->http     = os_http(c->os, &r, scratch);
    if (!s->http) {
        Str why = os_lasterr(c->os, &scratch);
        why = takehead(why, why.len<countof(s->errbuf) ? why.len : countof(s->errbuf));
        cfghold(&s->err, s->errbuf, countof(s->errbuf), why);
        s->deadline = now;
    }
}

// Start fetching a URL in a free slot. The validators of a cached copy
// make it conditional, if the copy would be used.
static void cfgstart(Config *c, CfgSlot *s, CfgUrl *u, Arena scratch)
{
    i64 now = os_ticks(c->os);
    s->u         = u;
    s->http      = 0;
    s->began     = now;
    s->deadline  = now + CFG_FETCHTIME;
    s->cutoff    = 0;
    s->redirects = 0;
    s->oldetag   = {};
    s->oldmod    = {};
    s->err       = {};
    if (c->net==CFG_NETWAIT && c->waitby<s->deadline) {
        s->deadline = c->waitby;
    }
    u->slot      = s;
    u->due       = 0;
    u->tried     = 1;
    u->attempted = now;
    cfgkeep(c, &u->pin, u->want);
    cfghold(&s->pin, s->pinbuf, countof(s->pinbuf), u->want);

    CfgCache k = cfgreadcache(c, &scratch, u->url);
    if (k.ok && (!s->pin.len || cfgsha256(&scratch, k.body)==s->pin)) {
        cfghold(&s->oldetag, s->oldetagbuf, countof(s->oldetagbuf), k.etag);
        cfghold(&s->oldmod, s->oldmodbuf, countof(s->oldmodbuf), k.mod);
    }
    if (!cfghold(&s->at, s->atbuf, countof(s->atbuf), u->url)) {
        cfghold(&s->err, s->errbuf, countof(s->errbuf), "URL too long");
        s->deadline = now;
        return;
    }
    cfgsend(c, s, scratch);
}

// Start due fetches while slots are free, and at startup only until
// the wait is over.
static void cfgpump(Config *c, Arena scratch)
{
    for (CfgUrl *u = c->urls; u; u = u->next) {
        if (!u->due || u->slot) {
            continue;
        } else if (u->seen != c->rev) {
            u->due = 0;  // no longer included
            continue;
        } else if (c->net==CFG_NETWAIT && os_ticks(c->os)>=c->waitby) {
            return;
        }
        CfgSlot *s = cfgfreeslot(c);
        if (!s && !c->slots) {
            u->due = 0;  // no memory for fetches
            continue;
        } else if (!s) {
            return;
        }
        cfgstart(c, s, u, scratch);
    }
}

// Mark the current load's URL includes due for a fetch, then start what
// slots allow. Due: one not fetched since the Config was made, or one
// without a usable copy whose pin changed since its last fetch; while
// live, also every one with force, and each once its interval is over;
// at startup, only those without a usable copy. Without the network or
// a cache, nothing is.
static void cfgrefresh(Config *c, Arena scratch, b32 force)
{
    if (c->net==CFG_NETNONE || !c->cache.len) {
        return;
    }
    i64 now = os_ticks(c->os);
    for (CfgUrl *u = c->urls; u; u = u->next) {
        if (u->seen!=c->rev || u->slot) {
            continue;
        }
        b32 due = !u->tried || (!u->cached && !(u->want==u->pin));
        if (c->net == CFG_NETLIVE) {
            due |= force || now-u->attempted>=c->every;
        } else {
            due &= !u->cached;
        }
        u->due |= due;
    }
    cfgpump(c, scratch);
}

// End a slot's request: follow a redirect, or record the outcome, free
// the slot, start what is due, and reload if a load would see anything
// new: other text, a failure that began or ended, or a pin newly met.
static void cfgdone(Config *c, CfgSlot *s, Arena scratch, i32 err, Str text)
{
    CfgUrl *u       = s->u;
    i64     now     = os_time(c->os);
    i32     st      = s->status;
    b32     moved   = st==301 || st==302 || st==303 || st==307 || st==308;
    Str     why     = {};  // null on success
    Str     sha     = {};  // of new text
    b32     pinbad  = 0;
    if (s->http) {
        os_httpfree(c->os, s->http);
        s->http = 0;
    }

    if (s->cutoff) {
        Buf b(&scratch, 64);
        print(&b, "timed out after ");
        printcfgage(&b, (s->cutoff + 500000)/1000000*1000);
        why = finish(&b);
    } else if (err == HTTP_ETOOBIG) {
        why = "larger than 1 MiB";
    } else if (err != HTTP_OK) {
        why = trim(text).len ? trim(text) : Str("the connection failed");
    } else if (moved) {
        Str next = {};
        if (s->location.len) {
            next = iniurlresolve(&scratch, s->at, s->location);
        }
        if (s->redirects == CFG_REDIRECTS) {
            why = "more than 5 redirects";
        } else if (!next.data) {
            why = "redirected without an http or https URL";
        } else if (startswith(s->at, "https:") && startswith(next, "http:")) {
            why = "redirected from https to http";
        } else if (!cfghold(&s->at, s->atbuf, countof(s->atbuf), next)) {
            why = "redirected to a URL too long";
        } else {
            s->redirects++;
            cfgsend(c, s, scratch);
            return;
        }
    } else if (st==304 && (s->oldetag.len || s->oldmod.len)) {
        CfgCache k = cfgreadcache(c, &scratch, u->url);
        k.fetched = now;
        k.etag    = s->etag.len ? s->etag : k.etag;
        k.mod     = s->mod.len ? s->mod : k.mod;
        if (!k.ok) {
            why = "not modified, but its cached copy is gone";
        } else if (!cfgwritecache(c, scratch, &k)) {
            why = "cannot write its cached copy";
        }
    } else if (st == 200) {
        Str body = {s->body, s->len};
        sha = cfgsha256(&scratch, body);
        if (s->pin.len && !(sha == s->pin)) {
            why    = concat(&scratch, "its sha256 is ", sha);
            pinbad = 1;
        } else {
            CfgCache k = {};
            k.url     = u->url;
            k.etag    = s->etag;
            k.mod     = s->mod;
            k.body    = body;
            k.fetched = now;
            if (!cfgwritecache(c, scratch, &k)) {
                why = "cannot write its cached copy";
            }
        }
    } else {
        Buf b(&scratch, 32);
        print(&b, "HTTP ");
        print(&b, (i64)st);
        why = finish(&b);
    }

    Str olderr = clone(&scratch, {u->err, u->errlen});
    b32 oldpin = u->pinbad;
    b32 reload = 0;
    if (why.data) {
        why = takehead(why, why.len<countof(u->err) ? why.len : countof(u->err));
        copybytes(u->err, why.data, why.len);
        u->errlen   = why.len;
        u->failedat = now;
        u->pinbad   = pinbad;
    } else {
        reload      = !u->cached || (sha.data && !(sha == u->sha));
        u->errlen   = 0;
        u->failedat = 0;
        u->pinbad   = 0;
        u->fetched  = now;
        if (sha.data) {
            cfgkeep(c, &u->sha, sha);
        }
    }
    reload |= !(olderr == Str{u->err, u->errlen}) || oldpin!=u->pinbad;
    s->u    = 0;
    u->slot = 0;
    cfgpump(c, scratch);
    if (reload) {
        cfgload(c, scratch);
    }
}

// Take a record of a fetch from the Config's mailbox. Returns 0 if it
// is not one, by its id. Ending a fetch may reload.
static b32 cfgrecord(Config *c, Arena scratch, Record r)
{
    if (!(r.id >> 63)) {
        return 0;
    }
    CfgSlot *s = 0;
    for (i32 i = 0; c->slots && i < CFG_FETCHES; i++) {
        CfgSlot *t = &c->slots[i];
        s = t->u && t->http && t->id==r.id ? t : s;
    }
    if (!s) {
        return 1;  // of a request given up on
    }

    switch (r.type) {
    case HTTP_HEAD: {
        Str head = cuthead(r.data, 4);
        s->status = get32(r.data);
        for (Str rest = head; rest.len;) {
            Cut line = cut(rest, '\n');
            Cut kv   = cut(line.head, ':');
            Str name = trim(kv.head);
            Str v    = trim(kv.tail);
            rest = line.tail;
            if (!kv.ok) {
                continue;  // WinHTTP's status line
            } else if (equalfold(name, "location")) {
                cfghold(&s->location, s->locbuf, countof(s->locbuf), v);
            } else if (equalfold(name, "etag")) {
                cfghold(&s->etag, s->etagbuf, countof(s->etagbuf), v);
            } else if (equalfold(name, "last-modified")) {
                cfghold(&s->mod, s->modbuf, countof(s->modbuf), v);
            }
        }
    } break;
    case HTTP_BODY:
        if (s->status==200 && r.data.len<=CFG_FETCHMAX-s->len) {
            copybytes(s->body+s->len, r.data.data, r.data.len);
            s->len += r.data.len;
        }
        break;
    case HTTP_DONE:
        cfgdone(c, s, scratch, get32(r.data), cuthead(r.data, 4));
        break;
    }
    return 1;
}

// The earliest deadline of the fetches under way in os_ticks, or I64MAX
// if none is.
static i64 cfgnext(Config *c)
{
    i64 r = I64MAX;
    for (i32 i = 0; c->slots && i < CFG_FETCHES; i++) {
        CfgSlot *s = &c->slots[i];
        r = s->u && s->deadline<r ? s->deadline : r;
    }
    return r;
}

// Cancel fetches at their deadlines, and end those that could not
// start, or whose cancel went unanswered.
static void cfgtimeout(Config *c, Arena scratch)
{
    for (i32 i = 0; c->slots && i < CFG_FETCHES; i++) {
        CfgSlot *s   = &c->slots[i];
        i64      now = os_ticks(c->os);
        if (!s->u || s->deadline>now) {
            continue;
        } else if (!s->http) {
            cfgdone(c, s, scratch, HTTP_ECONNECT, s->err);
        } else if (!s->cutoff) {
            s->cutoff   = now>s->began ? now-s->began : 1;
            s->deadline = now + CFG_GRACE;
            os_httpcancel(c->os, s->http);
        } else {
            cfgdone(c, s, scratch, HTTP_ECANCEL, {});
        }
    }
}

// Whether fetches are under way or due.
static b32 cfgbusy(Config *c)
{
    if (cfgnext(c) != I64MAX) {
        return 1;
    }
    for (CfgUrl *u = c->urls; u; u = u->next) {
        if (u->due && u->seen==c->rev) {
            return 1;
        }
    }
    return 0;
}

// At startup: wait for the fetches the first load started, and those
// the loads they cause start, none past waitby.
static void cfgwait(Config *c, Arena scratch)
{
    for (;;) {
        i64 next = cfgnext(c);
        if (next == I64MAX) {
            return;
        }
        i64 wait = (next - os_ticks(c->os) + 999) / 1000;
        if (wait > 0) {
            mailwait(c->mbox, (i32)(wait<60000 ? wait : 60000));
        }
        Mail m = mailtake(c->mbox);
        for (Record r; (r = mailnext(&m)).ok;) {
            cfgrecord(c, scratch, r);
        }
        cfgtimeout(c, scratch);
    }
}

// Hand fetching to the engine: records go to its mailbox, which it
// gives to cfgrecord, and refreshes run in the background, each URL's
// at most once every interval but on demand. A Config made without
// fetching stays without.
static void cfgattach(Config *c, Mailbox *mbox, i64 every)
{
    if (c->net != CFG_NETNONE) {
        c->net   = CFG_NETLIVE;
        c->mbox  = mbox;
        c->every = every>0 ? every : CFG_REFRESH;
    }
}


// Loading

// The default location of the user's file, or null if there is none:
// $XDG_CONFIG_HOME/harness, else $HOME/.config/harness, else (Windows)
// %APPDATA%\harness, with harness.ini in it.
static Str cfgdefault(Os *os, Arena *a)
{
    Str base = os_getenv(os, a, "XDG_CONFIG_HOME");
    if (base.len) {
        return join(a, join(a, base, "harness"), "harness.ini");
    }
    Str home = os_getenv(os, a, "HOME");
    if (home.len) {
        return join(a, join(a, home, ".config/harness"), "harness.ini");
    }
    Str appdata = os_getenv(os, a, "APPDATA");
    if (appdata.len) {
        return join(a, join(a, appdata, "harness"), "harness.ini");
    }
    return {};
}

// A hash over the type, size and mtime of each path, never 0.
static u64 cfgfingerprint(Os *os, Slice<Str> paths, Arena scratch)
{
    u64 h = 0x100;
    for (iz i = 0; i < paths.len; i++) {
        Stat s    = os_stat(os, scratch, paths[i]);
        u64  v[3] = {(u64)s.type, (u64)s.size, (u64)s.mtime};
        for (i32 k = 0; k < 3; k++) {
            h ^= v[k];
            h *= 1111111111111111111u;
        }
    }
    return h | 1;
}

// Load afresh into the idle region and make it current.
static void cfgload(Config *c, Arena scratch)
{
    i32      next = c->cur ^ 1;
    Arena    a    = newarena(c->regions[next]);
    Ini     *ini  = alloc<Ini>(&a);
    IniFile *user = 0;
    Stat     pre  = {};
    ini->fetch    = cfgsource;
    ini->fetchctx = c;
    c->loading    = c->rev + 1;
    if (c->path.len) {
        pre  = os_stat(c->os, scratch, c->path);
        user = iniload(ini, c->os, &a, c->path);
    } else {
        Str msg = "no configuration file: none of XDG_CONFIG_HOME, HOME "
                  "or APPDATA is set";
        inidiag(ini, &a, {}, 0, 0, INI_WARNING, msg);
    }
    if (c->note.len) {
        inidiag(ini, &a, c->path, 0, 0, INI_WARNING, c->note);
    }
    cfgvalidate(c, ini, user, &a);

    // The files to watch: the user's, and every include target, read
    // or not, so that creating a missing one counts as a change. URLs
    // are watched by refreshing them.
    Slice<Str> watch = {};
    if (c->path.len) {
        watch = push(&a, watch, c->path);
    }
    for (IniFile *f = ini->files; f; f = f->next) {
        for (iz i = 0; i<f->lines.len && !iniurl(f->path); i++) {
            IniLine *l = &f->lines[i];
            if (l->type==INI_INCLUDE && !iniurl(l->value) &&
                    !inidevice(l->value)) {
                Str p = inipath(&a, inidir(f->path), l->value);
                watch = push(&a, watch, p);
            }
        }
    }

    c->cur    = next;
    c->ini    = ini;
    c->user   = user;
    c->watch  = watch;
    c->rev++;
    c->polled = os_ticks(c->os);
    c->stamp  = cfgfingerprint(c->os, watch, scratch);
    if (c->path.len) {
        // Changed while loading: load again at the next poll
        Stat post = os_stat(c->os, scratch, c->path);
        if (post.type!=pre.type || post.size!=pre.size ||
                post.mtime!=pre.mtime) {
            c->stamp = 0;
        }
    }
    cfgrefresh(c, scratch, 0);
}

// Where writes to the user's file go: through a symbolic link to its
// target (dotfiles managers link it), since replacing the file would
// replace the link instead.
static Str cfgdest(Config *c, Arena *a)
{
    Str real = os_realpath(c->os, a, c->path);
    return real.len ? real : c->path;
}

// Write the seed file where the user's file is missing. Returns 0 on
// failure, with c->note explaining.
static b32 cfgwriteseed(Config *c, Arena scratch)
{
    Str dst = cfgdest(c, &scratch);
    Str dir = inidir(dst);
    if ((dir.len && !os_mkdirs(c->os, scratch, dir)) ||
            !os_write(c->os, scratch, dst, cfgseed())) {
        c->note = "cannot write the initial configuration file here";
        return 0;
    }
    return 1;
}

// Load the user's file at path (null for none) and its includes. With
// seed, a missing file is first created from the seed. Bad or missing
// files are diagnostics, never failures: null means only that memory
// could not be reserved. The Config belongs to one thread at a time.
//
// URL includes are cached under statedir, and without one are skipped.
// With fetch, those with no cached copy are fetched before this returns,
// which takes 30 s at most, and cfgattach later hands fetching over to
// the engine. Without, only cached copies are used.
static Config *newconfig(Os *os, Arena *perm, Arena scratch, Str path,
                         b32 seed, Str statedir = {}, b32 fetch = 0)
{
    b32     big = sizeof(void *) == 8;
    iz      cap = big ? (iz)1<<28 : (iz)1<<26;
    Config *c   = alloc<Config>(perm);
    c->os         = os;
    c->seed       = seed;
    c->regions[0] = os_reserve(os, cap);
    c->regions[1] = os_reserve(os, cap);
    c->keepregion = os_reserve(os, (iz)1<<24);
    if (!c->regions[0] || !c->regions[1] || !c->keepregion) {
        if (c->regions[0]) os_release(os, c->regions[0]);
        if (c->regions[1]) os_release(os, c->regions[1]);
        if (c->keepregion) os_release(os, c->keepregion);
        return 0;
    }
    c->keep = newarena(c->keepregion);
    if (path.len) {
        c->path = inipath(perm, {}, path);
    }
    if (statedir.len) {
        c->cache = join(perm, statedir, "includes");
    }
    if (fetch && c->cache.len) {
        c->mbox   = newmailbox(perm, 1<<17, 0, 0);
        c->net    = c->mbox ? CFG_NETWAIT : CFG_NETNONE;
        c->waitby = os_ticks(os) + CFG_FETCHTIME;
    }
    if (seed && c->path.len &&
            os_stat(os, scratch, c->path).type==STAT_MISSING) {
        cfgwriteseed(c, scratch);
    }
    cfgload(c, scratch);
    if (c->net == CFG_NETWAIT) {
        cfgwait(c, scratch);
    }
    return c;
}

static void cfgfree(Config *c)
{
    for (i32 i = 0; c->slots && i < CFG_FETCHES; i++) {
        if (c->slots[i].http) {
            os_httpcancel(c->os, c->slots[i].http);
            os_httpfree(c->os, c->slots[i].http);
        }
    }
    os_release(c->os, c->regions[0]);
    os_release(c->os, c->regions[1]);
    os_release(c->os, c->keepregion);
}

// Reload if any watched file changed since the last load, checking at
// most once a second unless forced, and start any URL refreshes due.
// Returns 1 after a reload.
static b32 cfgpoll(Config *c, Arena scratch, b32 force)
{
    i64 now = os_ticks(c->os);
    if (!force && now-c->polled<CFG_POLL) {
        return 0;
    }
    c->polled = now;
    if (c->stamp && cfgfingerprint(c->os, c->watch, scratch)==c->stamp) {
        cfgrefresh(c, scratch, 0);
        return 0;
    }
    cfgload(c, scratch);
    return 1;
}


// Editing

struct CfgSet {
    Str err;
    iz  line;  // of the key in the user's file, 0 if removed
    b32 ok;
};

// Set a key in the user's file, or remove it given a null value, then
// replace the file atomically and reload. Kind and name "" are the top
// level. Unknown keys, values bad for their type, and plaintext secrets
// are refused. A top-level key that an include after it overrides is
// moved below the includes, so that it wins. It fails if the value in
// effect afterward is not the edit: a removal that leaves a value from
// an include, when the user's file did not set it. Results and
// temporaries go in perm.
static CfgSet cfgset(Config *c, Arena *perm, Str kind, Str name, Str key,
                     Str value)
{
    CfgSet   r = {};
    CfgKind *k = cfgkind(kind);
    CfgKey  *s = k ? cfgkey(kind, key) : 0;
    if (!k) {
        r.err = "unknown section kind";
        return r;
    } else if (k->named != (name.len>0)) {
        r.err = k->named ? Str("this kind of section needs a name")
                         : Str("this kind of section has no name");
        return r;
    } else if (!s) {
        r.err = "unknown key";
        return r;
    } else if (value.data && cfgjudge(s, value, 0)==CFG_BAD) {
        r.err = cfgproblem(perm, s, value, 0);
        return r;
    } else if (value.data && s->type==CFG_SECRET && trim(value).len &&
               !startswith(trim(value), "env:")) {
        r.err = "a secret must be a reference such as env:NAME";
        return r;
    } else if (!c->path.len) {
        r.err = "no configuration file location";
        return r;
    }

    // Edit the file as it is now. The fingerprint misses a change of the
    // same size within one mtime tick, so compare the text itself.
    cfgpoll(c, *perm, 1);
    if (c->user) {
        Arena t  = *perm;
        Read  rd = os_read(c->os, &t, c->path, INI_MAXBYTES);
        Buf   b(&t, rd.data.len + 1);
        printinifile(&b, c->user);
        if (rd.err || !(rd.data == finish(&b))) {
            cfgload(c, *perm);
        }
    }
    IniFile *f = c->user;
    if (!f) {
        if (os_stat(c->os, *perm, c->path).type != STAT_MISSING) {
            r.err = concat(perm, "cannot read ", c->path);
            return r;
        }
        Ini *fresh = alloc<Ini>(perm);
        f = iniparse(fresh, perm, c->path, c->seed ? cfgseed() : Str(""));
    }
    // The user's own definition, overridden by an include
    IniEntry *was  = c->user ? inilookup(c->ini, kind, name, key) : 0;
    b32       move = 0;
    for (IniEntry *o = was ? was->older : 0; o && was->file!=f; o = o->older) {
        move |= o->file == f;
    }
    IniEdit e = iniedit(perm, f, kind, name, key, value, move);
    if (!e.text.data) {
        r.err = e.err;
        return r;
    }
    if (e.changed || !c->user) {
        Str dst = cfgdest(c, perm);
        Str dir = inidir(dst);
        if ((dir.len && !os_mkdirs(c->os, *perm, dir)) ||
                !os_write(c->os, *perm, dst, e.text)) {
            r.err = concat(perm, "cannot write ", c->path);
            return r;
        }
        c->note = {};
    }
    cfgload(c, *perm);
    r.line = e.line;

    // What is in effect now: the edit, unless another file decides. A
    // removal of the user's own setting may reveal an include's, as it
    // should, but one of a setting only an include makes did nothing.
    IniEntry *now = inilookup(c->ini, kind, name, key);
    b32       won = !now || e.changed;
    if (value.data) {
        won = now && now->file==c->user && now->line->num==e.line;
    }
    if (!won) {
        Buf b(perm, 128);
        if (!now) {
            print(&b, "saved, but the file did not read back");
        } else {
            print(&b, value.data ? Str("saved, but ") : Str("nothing to remove: "));
            print(&b, now->file->path);
            putbyte(&b, ':');
            print(&b, now->line->num);
            print(&b, value.data ? Str(" sets it too, and wins")
                                 : Str(" sets it, not this file"));
        }
        r.err = finish(&b);
        return r;
    }
    r.ok = 1;
    return r;
}


// Protocol replies

static void putcfgdiags(JsonOut *w, Config *c)
{
    putkey(w, "diags");
    putarray(w);
    for (iz i = 0; i < c->ini->diags.len; i++) {
        IniDiag d = c->ini->diags[i];
        putobject(w);
        putstrkey(w, "file", d.file);
        putintkey(w, "line", (i64)d.line);
        putintkey(w, "col", (i64)d.col);
        putstrkey(w, "severity", d.severity==INI_ERROR ? Str("error")
                                                        : Str("warning"));
        putstrkey(w, "message", d.msg);
        putend(w);
    }
    putend(w);
}

// Every schema key of one section, as in effect.
static void putcfgsection(JsonOut *w, Config *c, Str kind, Str name)
{
    b32 local = 0;
    if (kind == "endpoint") {
        Url u = parseurl(trim(cfgget(c, kind, name, "url", 0)));
        local = u.ok && localhost(u.host);
    }
    for (iz i = 0; i < countof(cfgschema); i++) {
        CfgKey *k = &cfgschema[i];
        if (!(k->kind == kind)) {
            continue;
        }
        IniEntry *e      = inilookup(c->ini, kind, name, k->key);
        Str       value  = local && k->local.data ? k->local : k->dflt;
        b32       valid  = 1;
        b32       isdflt = 1;  // unset, or set empty
        if (e && trim(e->value).len) {
            Str v = trim(e->value);
            value  = e->value;
            valid  = cfgjudge(k, v, e->file!=c->user) != CFG_BAD;
            isdflt = 0;
            if (k->type==CFG_SECRET && !startswith(v, "env:")) {
                value = "(hidden)";
            }
        }
        putobject(w);
        putstrkey(w, "kind", kind);
        putstrkey(w, "name", name);
        putstrkey(w, "key", k->key);
        putstrkey(w, "value", value);
        putstrkey(w, "file", e ? e->file->path : Str(""));
        putintkey(w, "line", e ? (i64)e->line->num : 0);
        putintkey(w, "default", (i64)isdflt);
        putintkey(w, "valid", (i64)valid);
        CfgUrl *u = e && iniurl(e->file->path) ? cfgurl(c, e->file->path, 0) : 0;
        if (u) {
            putintkey(w, "fetched", u->fetched);
        }
        putend(w);
    }
}

// The URL includes of the current load.
static void putcfgincludes(JsonOut *w, Config *c)
{
    putkey(w, "includes");
    putarray(w);
    for (CfgUrl *u = c->urls; u; u = u->next) {
        if (u->seen != c->rev) {
            continue;
        }
        putobject(w);
        putstrkey(w, "url", u->url);
        putstrkey(w, "pin", u->want.data ? u->want : Str(""));
        putstrkey(w, "sha256", u->sha.data ? u->sha : Str(""));
        putintkey(w, "fetched", u->fetched);
        putintkey(w, "fetching", (i64)(u->slot || u->due));
        putstrkey(w, "error", Str{u->err, u->errlen});
        putend(w);
    }
    putend(w);
}

// The payload of a config.get reply, after its id and ok.
static void putcfg(JsonOut *w, Config *c)
{
    putstrkey(w, "path", c->path.len ? c->path : Str(""));
    putintkey(w, "exists", (i64)(c->user != 0));
    putintkey(w, "rev", c->rev);

    putkey(w, "files");
    putarray(w);
    for (IniFile *f = c->ini->files; f; f = f->next) {
        putstr(w, f->path);
    }
    putend(w);

    putkey(w, "settings");
    putarray(w);
    for (iz i = 0; i < countof(cfgkinds); i++) {
        CfgKind *k = &cfgkinds[i];
        if (!k->named) {
            putcfgsection(w, c, k->kind, "");
            continue;
        }
        for (IniSection *s = inifirst(c->ini, k->kind); s; s = ininext(s)) {
            if (s->name.len) {
                putcfgsection(w, c, k->kind, s->name);
            }
        }
    }
    putend(w);

    putkey(w, "schema");
    putarray(w);
    for (iz i = 0; i < countof(cfgschema); i++) {
        CfgKey *k = &cfgschema[i];
        putobject(w);
        putstrkey(w, "kind", k->kind);
        putstrkey(w, "key", k->key);
        putstrkey(w, "type", cfgtypenames[k->type]);
        putstrkey(w, "default", k->dflt);
        putstrkey(w, "local", k->local.data ? k->local : k->dflt);
        putkey(w, "choices");
        putarray(w);
        for (Str rest = k->choices; rest.len;) {
            Str word = cfgnextword(&rest);
            if (word.len) {
                putstr(w, word);
            }
        }
        putend(w);
        putstrkey(w, "group", k->group);
        putstrkey(w, "desc", k->desc);
        putend(w);
    }
    putend(w);

    putkey(w, "kinds");
    putarray(w);
    for (iz i = 0; i < countof(cfgkinds); i++) {
        putobject(w);
        putstrkey(w, "kind", cfgkinds[i].kind);
        putintkey(w, "named", (i64)cfgkinds[i].named);
        putstrkey(w, "desc", cfgkinds[i].desc);
        putend(w);
    }
    putend(w);

    putcfgincludes(w, c);
    putcfgdiags(w, c);
}

// The payload of a config.set reply, after its id, ok and err.
static void putcfgset(JsonOut *w, Config *c, CfgSet *r)
{
    putstrkey(w, "path", c->path.len ? c->path : Str(""));
    putintkey(w, "line", (i64)r->line);
    putintkey(w, "rev", c->rev);
    putcfgdiags(w, c);
}

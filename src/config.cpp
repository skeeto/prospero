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
//    "diags":[{"file":"...","line":7,"col":1,"severity":"warning",
//              "message":"..."}, ...]}
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
// marks one ignored for its default. An endpoint's timeouts default by
// whether its URL is local (the schema's "local" defaults). A plaintext
// secret is reported as "(hidden)", so no reply ever carries one. The
// schema's types are string, int, number, seconds, bool, enum, url,
// secret (env:NAME), list (words of choices) and paths (separated by ;).
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
    CFG_POLL = 1000000,  // os_ticks between checks for changed files
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
    // or not, so that creating a missing one counts as a change
    Slice<Str> watch = {};
    if (c->path.len) {
        watch = push(&a, watch, c->path);
    }
    for (IniFile *f = ini->files; f; f = f->next) {
        for (iz i = 0; i < f->lines.len; i++) {
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
static Config *newconfig(Os *os, Arena *perm, Arena scratch, Str path,
                         b32 seed)
{
    b32     big = sizeof(void *) == 8;
    iz      cap = big ? (iz)1<<28 : (iz)1<<26;
    Config *c   = alloc<Config>(perm);
    c->os         = os;
    c->seed       = seed;
    c->regions[0] = os_reserve(os, cap);
    c->regions[1] = os_reserve(os, cap);
    if (!c->regions[0] || !c->regions[1]) {
        if (c->regions[0]) os_release(os, c->regions[0]);
        if (c->regions[1]) os_release(os, c->regions[1]);
        return 0;
    }
    if (path.len) {
        c->path = inipath(perm, {}, path);
    }
    if (seed && c->path.len &&
            os_stat(os, scratch, c->path).type==STAT_MISSING) {
        cfgwriteseed(c, scratch);
    }
    cfgload(c, scratch);
    return c;
}

static void cfgfree(Config *c)
{
    os_release(c->os, c->regions[0]);
    os_release(c->os, c->regions[1]);
}

// Reload if any watched file changed since the last load, checking at
// most once a second unless forced. Returns 1 after a reload.
static b32 cfgpoll(Config *c, Arena scratch, b32 force)
{
    i64 now = os_ticks(c->os);
    if (!force && now-c->polled<CFG_POLL) {
        return 0;
    }
    c->polled = now;
    if (c->stamp && cfgfingerprint(c->os, c->watch, scratch)==c->stamp) {
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
        putend(w);
    }
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

// "bash (builtin)": what `prospero --bash` takes on its command line,
// and the state one run leaves for the next, in a file beside the
// spill. The hosts (main_posix.cpp, main_win32.cpp) run libunabashed;
// this part is portable and needs no library, so the tests cover it.
//
//   prospero --bash [--root DIR] [--carry IN] [--state OUT] ARGS...
//   prospero --bash --term ARGS...
//
// ARGS are bash's own (-c CMD, -l), with $0 "bash". --root is the
// Windows tools' directory (ub_config.root). --carry names the state
// file whose exports this run starts with; --state, where it leaves its
// own once it ends on its own (not a signal, not an abort). --term runs
// the shell on the terminal it leads, a Terminal tab's (UB_TERMINAL,
// POSIX): it carries nothing, so it takes none of the others.
//
// The state file, NUL-separated: a version line, then key and value
// pairs, pwd and cwd (logical and native, either empty), then, only
// when the run's changes are known, "exports" with a count and that
// many entries, NAME=value for one set or exported, a bare NAME for
// one unset or unexported, then "changed" (0 or 1) and "end". A file
// that is missing, cut short or of another version reads as none.

struct BashArgs {
    Str root;
    Str carry;
    Str state;
    Str err;   // why not ok
    i32 rest;  // index of bash's arguments, after the options
    b32 term;
    b32 ok;
};

// args[at] is "--bash".
static BashArgs bashargs(Str *args, i32 argc, i32 at)
{
    BashArgs r = {};
    i32 i = at + 1;
    for (; i < argc; i++) {
        if (args[i] == "--term") {
            r.term = 1;
            continue;
        }
        Str *dst = args[i]=="--root"  ? &r.root  :
                   args[i]=="--carry" ? &r.carry :
                   args[i]=="--state" ? &r.state : 0;
        if (!dst) break;
        if (i+1 >= argc) {
            r.err = "an option needs a value";
            return r;
        }
        *dst = args[++i];
    }
    if (r.term && (r.root.len || r.carry.len || r.state.len)) {
        r.err = "--term takes no --root, --carry or --state";
        return r;
    }
    r.rest = i;
    r.ok   = 1;
    return r;
}

struct BashState {
    Str        pwd;      // logical, as pwd shows it; may be empty
    Str        cwd;      // native; empty: none
    Slice<Str> exports;  // with known: NAME=value, or NAME to unset
    b32        known;    // exports holds the run's changes
    b32        changed;  // the run changed its exports
    b32        ok;
};

static Str const bashstatemagic = "prospero-bash-state 1";

// The variable an export entry names, or empty for one that never
// carries: PWD and OLDPWD (the directory carries apart), SHLVL and _
// (each run's own), Windows' per-drive "=C:" entries, and the malformed.
static Str bashexportname(Str entry)
{
    Cut c = cut(entry, '=');
    Str n = c.head;
    if (!n.len || n=="PWD" || n=="OLDPWD" || n=="SHLVL" || n=="_") {
        return {};
    }
    for (iz i = 0; i < n.len; i++) {
        if (!n[i]) return {};
    }
    return n;
}

static b32 bashnameeq(Str a, Str b, b32 windows)
{
    return windows ? equalfold(a, b) : a==b;
}

static void bashfield(Buf *b, Str v)
{
    print(b, v);
    putbyte(b, 0);
}

static Str bashstateencode(Arena *a, BashState *s)
{
    Buf b(a, 256);
    bashfield(&b, bashstatemagic);
    bashfield(&b, "pwd");
    bashfield(&b, s->pwd);
    bashfield(&b, "cwd");
    bashfield(&b, s->cwd);
    if (s->known) {
        bashfield(&b, "exports");
        print(&b, (i64)s->exports.len);
        putbyte(&b, 0);
        for (iz i = 0; i < s->exports.len; i++) {
            bashfield(&b, s->exports[i]);
        }
    }
    bashfield(&b, "changed");
    bashfield(&b, s->changed ? Str("1") : Str("0"));
    bashfield(&b, "end");
    return finish(&b);
}

// The next NUL-terminated field, failing past the end.
static Str bashnext(Str *in, b32 *bad)
{
    Cut c = cut(*in, 0);
    *bad |= !c.ok;
    *in   = c.tail;
    return c.ok ? c.head : Str{};
}

static BashState bashstatedecode(Arena *a, Str data)
{
    BashState r   = {};
    Str       in  = data;
    b32       bad = 0;
    if (bashnext(&in, &bad) != bashstatemagic) return r;
    if (bashnext(&in, &bad) != "pwd")          return r;
    Str pwd = bashnext(&in, &bad);
    if (bashnext(&in, &bad) != "cwd")          return r;
    Str cwd = bashnext(&in, &bad);
    Str key = bashnext(&in, &bad);
    Slice<Str> exports = {};
    b32 known = 0;
    if (key == "exports") {
        Int n = parseint(bashnext(&in, &bad));
        if (bad || !n.ok || n.val<0 || n.val>in.len) return r;
        for (i64 i = 0; i < n.val; i++) {
            Str e = bashnext(&in, &bad);
            if (bad || !bashexportname(e).len) return r;
            exports = push(a, exports, e);
        }
        known = 1;
        key   = bashnext(&in, &bad);
    }
    if (key != "changed") return r;
    Str changed = bashnext(&in, &bad);
    if (changed!="0" && changed!="1")      return r;
    if (bashnext(&in, &bad)!="end" || bad || in.len) return r;

    r.pwd     = pwd;
    r.cwd     = cwd;
    r.exports = exports;
    r.known   = known;
    r.changed = changed == "1";
    r.ok      = 1;
    return r;
}

// The carried exports with a run's changes over them, one entry per
// name, in first-seen order: what the next run starts with.
static Slice<Str> bashmerge(Arena *a, Slice<Str> carried, Slice<Str> delta, b32 windows)
{
    Slice<Str> r = {};
    for (i32 pass = 0; pass < 2; pass++) {
        Slice<Str> src = pass ? delta : carried;
        for (iz i = 0; i < src.len; i++) {
            Str name = bashexportname(src[i]);
            if (!name.len) continue;
            iz at = -1;
            for (iz j = 0; at<0 && j<r.len; j++) {
                at = bashnameeq(bashexportname(r[j]), name, windows) ? j : -1;
            }
            if (at >= 0) {
                r[at] = src[i];
            } else {
                r = push(a, r, src[i]);
            }
        }
    }
    return r;
}

// An environment with exports applied: each NAME=value set, each bare
// NAME removed.
static Slice<Str> bashapply(Arena *a, Slice<Str> env, Slice<Str> exports, b32 windows)
{
    for (iz i = 0; i < exports.len; i++) {
        Str name = bashexportname(exports[i]);
        if (!name.len) continue;
        Cut c = cut(exports[i], '=');
        env = c.ok ? envset(a, env, name, c.tail, windows)
                   : envdel(a, env, name, windows);
    }
    return env;
}

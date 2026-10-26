// Engine deployment: "prospero deploy", and deploying for a Remote
//
//   prospero deploy NAME|DEST [--force] [options]
//
// Puts an engine built for the remote machine's platform in ~/.prospero
// there, over ssh and scp, where "prospero connect" (remote.cpp) then
// runs it. A WSL distro is reached through wsl.exe instead (linkargv),
// which runs the same commands in its /bin/sh, and takes uploads on
// standard input, as it has no scp:
//
// 1. Probe, with one command that sh and cmd.exe both take:
//
//      uname -sm && echo %OS% || (echo %OS% %PROCESSOR_ARCHITECTURE%
//                                 %PROCESSOR_ARCHITEW6432% && ver)
//
//    A POSIX shell, busybox-w32's on Windows included, prints uname's
//    "SYSNAME MACHINE" and then "%OS%" as it is. cmd.exe expands the
//    variables instead, after a uname of its own (Git's) if it has one,
//    else with the processor architecture, then ver's version line.
// 2. Choose prospero-engine-VERSION-OS-ARCH[.exe] from the engines
//    directory (the first with one of "engines" next to this
//    executable and beside the default prospero.ini, unless given;
//    built by tools/engines.py),
//    checked against its .sha256 file. With none
//    there for the platform, this executable serves when it is built
//    for that platform.
// 3. Prepare ~/.prospero there (mode 0700), learning what is already at
//    the engine's place, .prospero/engine-PROTO-VERSION-OS-ARCH[.exe]:
//    the same engine means nothing to upload. With no tool there to
//    compute a SHA-256 (below), deploying fails at once, for good.
// 4. Upload with scp (into WSL: cat) to that name plus ".part", again after a dropped
//    link (an XP laptop's Wi-Fi stalls), and verify its SHA-256 there
//    (sha256sum, shasum -a 256 or openssl; certutil -hashfile in cmd).
//    A damaged copy is uploaded again.
// 5. Rename it into place, and run it once (--version): an engine that
//    cannot run there (too old a C library, say) is removed again.
// 6. The desktop helper the same way (steps 3 to 5), except for an
//    engine that is its own helper (Windows', or this program's where it
//    is one), when the engines directory has one for the platform, prospero-desktop-VERSION-OS-
//    ARCH[.exe] (or, when this program serves, its own helper next to
//    it), to .prospero/desktop-PROTO-VERSION-OS-ARCH[.exe], beside the
//    engine, where the engine looks for it (deskhelpers, engine.cpp).
//    The engine works without it, so a helper that cannot be put there
//    or does not run there (--version) is only a warning.
// 7. Record where it went in <state>/deploy/DEST.ini (for WSL, DEST is
//    wsl:DISTRO, linkdest), for connecting:
//    host, platform, shell, path (for the remote shell), sha256,
//    desktop (the helper's path, if deployed), version, proto, and
//    deployed (Unix epoch ms).
//
// A new version gets a new name, so a running engine is never replaced
// or stopped: a host of another protocol starts beside the old one (a
// host listens per protocol, host.cpp), and old sessions keep their
// engine until they end. Another build of the same version is refused
// unless --force, which replaces it: in place on POSIX, where a running
// program keeps its file, and on Windows, whose running programs cannot
// be replaced, after renaming the old one aside.
//
// Progress goes to standard error, or as "deploy" records to a mailbox:
//   {"ts":T,"ev":"deploy","host":"pi","step":"upload","msg":"..."}
// step being probe, upload, verify, install, then done (with path, the
// engine there from the home directory, and platform) or failed.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    DEPLOY_STEPTIME = 90*SEC,   // an ssh command, at most
    DEPLOY_UPTIME   = 120*SEC,  // an upload, at most, plus a second per...
    DEPLOY_UPRATE   = 16<<10,   // ...this many bytes
    DEPLOY_MAXSIZE  = sizeof(void *)==8 ? (i64)64<<20 : (i64)32<<20,
    DEPLOY_OUTMAX   = 1<<16,    // standard output kept from a command
};

// Tries of an ssh command whose ssh failed (255), or of an upload, with
// waits between them: together about as long as an XP laptop's Wi-Fi
// stalls, a minute or two.
enum : i32 {
    DEPLOY_TRIES = 6,
};

static i64 deploywaits[] = {2*SEC, 5*SEC, 10*SEC, 20*SEC, 30*SEC};
static_assert(countof(deploywaits) == DEPLOY_TRIES-1);

// The shell that runs commands over ssh there.
enum : i32 {
    DEPLOY_SH,   // POSIX sh, busybox-w32's included
    DEPLOY_CMD,  // cmd.exe
};

static Str deployopts[] = {
    "-o", "BatchMode=yes", "-o", "ConnectTimeout=15",
    "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
};

static Str deployprobe =
    "uname -sm && echo %OS% || (echo %OS% %PROCESSOR_ARCHITECTURE% "
    "%PROCESSOR_ARCHITEW6432% && ver)";

struct DeployConf {
    Str       name;        // for messages: the remote's name, or ssh
    Str       ssh;         // ssh destination
    Str       wsl;         // else a WSL distro, reached through wsl.exe
    Str       program;     // ssh or wsl.exe itself, by name if empty
    Str       scp;         // scp itself, "scp" if empty
    Str       engines;     // directory of engines, if not next to this program
    Str       envpath;     // PATH for ssh and scp, if not this process's
    Str       localstate;  // the record goes under deploy/ there
    b32       force;       // replace another build of this version
    b32       verbose;     // progress on standard error
    Mailbox  *notes;       // deploy records (MAIL_OUT), if any
    i32      *cancel;      // another thread sets it to stop (may be null)
};

struct Deployed {
    Str path;      // the engine there, for its shell, from the home directory
    Str shown;     // the same as ~/.prospero/NAME
    Str platform;
    Str err;       // why not, unless ok
    b32 ok;
};


// Platforms

struct Platform {
    Str os;     // linux, macos, windows
    Str arch;   // x86_64, aarch64 (arm64 on macOS), i686
    Str name;   // OS-ARCH
    Str about;  // what the probe said, for messages
    i32 shell;
    b32 ok;
};

// The word at index i of a line, or a null string.
static Str deployword(Str s, i32 i)
{
    for (i32 n = 0;; n++) {
        s = trimleft(s);
        if (!s.len) {
            return {};
        }
        iz e = 0;
        for (; e<s.len && !whitespace(s[e]); e++) {}
        if (n == i) {
            return takehead(s, e);
        }
        s = cuthead(s, e);
    }
}

// The engine's platform from uname -s and -m.
static Platform deployuname(Arena *a, Str sys, Str mach)
{
    Platform r = {};
    if (sys == "Linux") {
        r.os = "linux";
    } else if (sys == "Darwin") {
        r.os = "macos";
    } else if (startswith(sys, "Windows_NT") || startswith(sys, "MINGW") ||
               startswith(sys, "MSYS") || startswith(sys, "CYGWIN")) {
        r.os = "windows";
    } else {
        Buf b(a, 64);
        for (iz i = 0; i < sys.len; i++) {
            putbyte(&b, lowercase(sys[i]));
        }
        r.os = finish(&b);
    }

    b32 x64 = equalfold(mach, "x86_64") || equalfold(mach, "amd64") ||
              equalfold(mach, "x64");
    b32 a64 = equalfold(mach, "aarch64") || equalfold(mach, "arm64");
    b32 x86 = equalfold(mach, "x86") || equalfold(mach, "i386") ||
              equalfold(mach, "i486") || equalfold(mach, "i586") ||
              equalfold(mach, "i686");
    if (x64 || (a64 && r.os=="windows")) {
        r.arch = "x86_64";  // Windows on Arm runs x86-64 programs
    } else if (a64) {
        r.arch = r.os=="macos" ? Str("arm64") : Str("aarch64");
    } else if (x86) {
        r.arch = "i686";
    } else {
        Buf b(a, 32);
        for (iz i = 0; i < mach.len; i++) {
            putbyte(&b, lowercase(mach[i]));
        }
        r.arch = finish(&b);
    }
    r.ok = r.os.len && r.arch.len;
    if (r.ok) {
        Str name = concat(a, r.os, "-");
        r.name = concat(a, name, r.arch);
    }
    return r;
}

// The platform and shell a probe's output tells (see the top).
static Platform deployplatform(Arena *a, Str out)
{
    Str lines[16] = {};
    i32 n = 0;
    for (Str rest = out; rest.len && n<countof(lines);) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str ln = trim(c.head);
        if (ln.len) {
            lines[n++] = ln;
        }
    }

    for (i32 i = 1; i < n; i++) {
        if (lines[i]=="%OS%" || lines[i]=="Windows_NT") {
            // uname then the marker: sh's "%OS%", or cmd.exe's own
            Str sys  = deployword(lines[i-1], 0);
            Str mach = deployword(lines[i-1], 1);
            if (!sys.len || !mach.len) {
                break;
            }
            Platform r = deployuname(a, sys, mach);
            r.shell = lines[i]=="%OS%" ? DEPLOY_SH : DEPLOY_CMD;
            r.about = lines[i-1];
            return r;
        }
    }

    for (i32 i = 0; i < n; i++) {
        // cmd.exe without uname: "Windows_NT ARCH ARCHW6432", then ver
        if (deployword(lines[i], 0)=="Windows_NT" && deployword(lines[i], 1).len) {
            Str arch = deployword(lines[i], 1);
            Str w64  = deployword(lines[i], 2);
            if (arch[0] == '%') {
                break;  // nothing expanded: no cmd.exe
            }
            if (w64.len && w64[0]!='%') {
                arch = w64;  // a 32-bit cmd.exe on 64-bit Windows
            }
            Platform r = deployuname(a, "Windows_NT", arch);
            r.shell = DEPLOY_CMD;
            r.about = i+1<n ? lines[i+1] : lines[i];
            return r;
        }
    }
    return {};
}

// This program's own platform, as deployuname names it, or null.
static Str deployself()
{
    #if defined(_WIN32) && defined(__x86_64__)
    return "windows-x86_64";
    #elif defined(_WIN32) && defined(__i386__)
    return "windows-i686";
    #elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
    #elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x86_64";
    #elif defined(__linux__) && defined(__aarch64__)
    return "linux-aarch64";
    #elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
    #else
    return {};
    #endif
}

static b32 hexdigit(u8 c)
{
    return digit(c) || (c>='a' && c<='f') || (c>='A' && c<='F');
}

// The SHA-256 a hashing tool printed, in lowercase: sha256sum's and
// shasum's "HEX  FILE" (with a backslash first for an odd file name),
// openssl's "HEX *FILE", or certutil's line of hex, which older Windows
// spaces out. Null if none.
static Str deployhash(Arena *a, Str out)
{
    for (Str rest = out; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str ln = trim(c.head);
        if (ln.len && ln[0]=='\\') {
            ln = cuthead(ln, 1);
        }
        Buf b(a, 64);
        Str w = deployword(ln, 0);
        if (w.len != 64) {
            for (iz i = 0; i < ln.len; i++) {  // certutil's, spaced out
                if (ln[i] != ' ') putbyte(&b, ln[i]);
            }
            w = finish(&b);
            b = Buf(a, 64);
        }
        b32 hex = w.len == 64;
        for (iz i = 0; hex && i<w.len; i++) {
            hex = hexdigit(w[i]);
        }
        if (hex) {
            for (iz i = 0; i < w.len; i++) {
                putbyte(&b, lowercase(w[i]));
            }
            return finish(&b);
        }
    }
    return {};
}


// Commands there

enum : i32 {
    DEPLOY_PREPARE,  // make ~/.prospero, find a hash tool, hash what is there
    DEPLOY_VERIFY,   // hash the upload, or say nohash
    DEPLOY_INSTALL,  // rename it into place, then run it
    DEPLOY_RUN,      // run what is there
    DEPLOY_REMOVE,   // remove what does not run
};

// A command for a step, in the shell's dialect, about the engine at
// path (.prospero/NAME, from the home directory). The stamp names an
// old engine renamed aside, on Windows. A POSIX one runs in sh -c,
// whatever the login shell (fish, csh), and so has no single quotes.
static Str deploycmd(Arena *a, i32 shell, i32 step, Str path, i64 stamp)
{
    Buf b(a, 512);
    b32 sh = shell == DEPLOY_SH;
    Str p  = path;
    if (sh) {
        print(&b, "sh -c '");
    }
    switch (step) {
    case DEPLOY_PREPARE:
        if (sh) {
            print(&b, "mkdir -p .prospero && chmod 700 .prospero && "
                      "echo ready; { command -v sha256sum || "
                      "command -v shasum || command -v openssl; } "
                      ">/dev/null 2>&1 || echo nohash; if [ -f ");
            print(&b, p);
            print(&b, " ]; then echo have; { sha256sum ");
            print(&b, p);
            print(&b, " || shasum -a 256 ");
            print(&b, p);
            print(&b, " || openssl dgst -sha256 -r ");
            print(&b, p);
            print(&b, "; } 2>/dev/null; fi");
        } else {
            print(&b, "(if not exist .prospero mkdir .prospero) & "
                      "(if exist .prospero echo ready) & "
                      "(where certutil >nul 2>&1 || echo nohash) & (if exist ");
            print(&b, p);
            print(&b, " (echo have & certutil -hashfile ");
            print(&b, p);
            print(&b, " SHA256))");
        }
        break;
    case DEPLOY_VERIFY:
        if (sh) {
            print(&b, "chmod 755 ");
            print(&b, p);
            print(&b, ".part && { sha256sum ");
            print(&b, p);
            print(&b, ".part || shasum -a 256 ");
            print(&b, p);
            print(&b, ".part || openssl dgst -sha256 -r ");
            print(&b, p);
            print(&b, ".part || echo nohash; } 2>/dev/null");
        } else {
            print(&b, "certutil -hashfile ");
            print(&b, p);
            print(&b, ".part SHA256");
        }
        break;
    case DEPLOY_INSTALL:
        // Replace in place, or else (a running Windows program) rename
        // the old one aside first, which Windows allows
        if (sh) {
            print(&b, "{ mv -f ");
            print(&b, p);
            print(&b, ".part ");
            print(&b, p);
            print(&b, " 2>/dev/null || { mv -f ");
            print(&b, p);
            print(&b, " ");
            print(&b, p);
            print(&b, ".old-");
            print(&b, stamp);
            print(&b, " && mv -f ");
            print(&b, p);
            print(&b, ".part ");
            print(&b, p);
            print(&b, "; }; } && ");
            print(&b, p);
            print(&b, " --version");
        } else {
            print(&b, "(move /y ");
            print(&b, p);
            print(&b, ".part ");
            print(&b, p);
            print(&b, " >nul 2>&1 || (move /y ");
            print(&b, p);
            print(&b, " ");
            print(&b, p);
            print(&b, ".old-");
            print(&b, stamp);
            print(&b, " >nul && move /y ");
            print(&b, p);
            print(&b, ".part ");
            print(&b, p);
            print(&b, " >nul)) && ");
            print(&b, p);
            print(&b, " --version");
        }
        break;
    case DEPLOY_RUN:
        if (sh) {
            print(&b, "chmod 755 ");
            print(&b, p);
            print(&b, "; ");
        }
        print(&b, p);
        print(&b, " --version");
        break;
    case DEPLOY_REMOVE:
        print(&b, sh ? Str("rm -f ") : Str("del /f /q "));
        print(&b, p);
        break;
    }
    if (sh) {
        putbyte(&b, '\'');
    }
    return finish(&b);
}


// Records of deployments: <state>/deploy/DEST.ini

struct DeployRec {
    Str path;      // for the remote shell, from the home directory
    Str platform;
    Str sha256;
    Str version;
    i64 proto;
    b32 ok;        // a record for this destination
};

// The destination made safe for a file name anywhere.
static Str deployfile(Arena *a, Str localstate, Str dest, Str ext)
{
    Str name = clone(a, dest);
    for (iz i = 0; i < name.len; i++) {
        u8  c  = name[i];
        b32 ok = alnum(c) || c=='-' || c=='_' || c=='@' || (c=='.' && i);
        name[i] = ok ? c : (u8)'_';
    }
    Str dir = join(a, localstate, "deploy");
    name = concat(a, name, ext);
    return join(a, dir, name);
}

// What was deployed to a destination, if anything.
static DeployRec deployrecord(Os *os, Arena *a, Str localstate, Str dest)
{
    DeployRec r    = {};
    Str       path = deployfile(a, localstate, dest, ".ini");
    Read      rd   = os_read(os, a, path, 1<<12);
    if (rd.err != READ_OK) {
        return r;
    }
    Str host = {};
    for (Str rest = rd.data; rest.len;) {
        Cut line = cut(rest, '\n');
        Cut kv   = cut(line.head, '=');
        rest = line.tail;
        Str k = trim(kv.head);
        Str v = trim(kv.tail);
        if (k == "host") {
            host = v;
        } else if (k == "path") {
            r.path = v;
        } else if (k == "platform") {
            r.platform = v;
        } else if (k == "sha256") {
            r.sha256 = v;
        } else if (k == "version") {
            r.version = v;
        } else if (k == "proto") {
            r.proto = parseint(v).val;
        }
    }
    r.ok = host==dest && r.path.len;
    return r;
}


// Running ssh and scp, or wsl.exe

// A remote machine's key for its deploy record and errors: the ssh
// destination, or wsl:DISTRO, which no ssh destination looks like.
static Str linkdest(Arena *a, Str ssh, Str wsl)
{
    return wsl.len ? concat(a, "wsl:", wsl) : ssh;
}

// Why a link cannot start, or null: a destination or distro that ssh or
// wsl.exe would take for an option, which may run a command here.
static Str linkbad(Str ssh, Str wsl)
{
    if (wsl.len) {
        return startswith(wsl, "-") ? Str("a WSL distro name cannot begin with -")
                                    : Str{};
    }
    return startswith(ssh, "-") ? Str("an ssh destination cannot begin with -") : Str{};
}

// The command line that runs a shell command there: ssh, whose login
// shell takes it; or for a WSL distro, wsl.exe straight into /bin/sh,
// in the home directory as an ssh login starts (else it starts in this
// process's directory, under /mnt).
static Slice<Str> linkargv(Arena *a, Str program, Str ssh, Str wsl, Str command)
{
    Slice<Str> r = {};
    if (wsl.len) {
        r = push(a, r, program.len ? program : Str("wsl.exe"));
        r = push(a, r, Str("-d"));
        r = push(a, r, wsl);
        r = push(a, r, Str("--cd"));
        r = push(a, r, Str("~"));
        r = push(a, r, Str("--exec"));
        r = push(a, r, Str("/bin/sh"));
        r = push(a, r, Str("-c"));
        r = push(a, r, command);
        return r;
    }
    r = push(a, r, program.len ? program : Str("ssh"));
    r = push(a, r, Str("-T"));
    for (iz i = 0; i < countof(deployopts); i++) {
        r = push(a, r, deployopts[i]);
    }
    r = push(a, r, Str("-o"));
    r = push(a, r, Str("EscapeChar=none"));
    r = push(a, r, Str("--"));  // no options after it
    r = push(a, r, ssh);
    r = push(a, r, command);
    return r;
}

// The environment for a link's program: for wsl.exe, which otherwise
// writes its own messages in UTF-16, asking for UTF-8.
static Slice<Str> linkenv(Arena *a, Slice<Str> env, b32 wsl)
{
    if (!wsl) {
        return env;
    }
    Slice<Str> r = {};
    for (iz i = 0; i < env.len; i++) {
        if (!envnameis(env[i], "WSL_UTF8", HOST_WINDOWS)) {
            r = push(a, r, env[i]);
        }
    }
    return push(a, r, Str("WSL_UTF8=1"));
}

// wsl.exe's own messages as UTF-8: an inbox wsl.exe (WSL not installed)
// writes UTF-16 (little endian) whatever WSL_UTF8 says, as older ones
// do. Text that does not look like UTF-16, a NUL second, is as it was.
static Str wslutf8(Arena *a, Str s)
{
    b32 bom  = s.len>=2 && s[0]==0xff && s[1]==0xfe;
    b32 wide = bom || (s.len>=2 && s[0] && !s[1]);
    if (!wide) {
        return s;
    }
    Buf b(a, s.len/2*3);
    for (iz i = bom ? 2 : 0; i+1 < s.len; i += 2) {
        i32 c = s[i] | s[i+1]<<8;
        c = c>=0xd800 && c<0xe000 ? 0xfffd : c;  // a pair's halves: rare
        u8  e[4];
        iz  n = utf8encode(e, c);
        for (iz k = 0; k < n; k++) {
            putbyte(&b, e[k]);
        }
    }
    return finish(&b);
}

struct DeployRun {
    Str out;      // standard output
    Str err;      // standard error
    i32 code;     // exit status, -1 if it never finished
    b32 timeout;  // stopped for taking too long
};

struct Deploy {
    Os         *os;
    DeployConf *conf;
    Mailbox    *mb;       // the children's records
    Str         errpath;  // a child's standard error
    Slice<Str>  env;
    u64         id;       // of the latest child
};

static b32 deploycancelled(Deploy *d)
{
    return d->conf->cancel && __atomic_load_n(d->conf->cancel, __ATOMIC_ACQUIRE);
}

// Progress, for standard error or the mailbox; done and failed close it.
static void deploynote(Deploy *d, Arena scratch, Str step, Str msg,
                       Str path = {}, Str platform = {})
{
    DeployConf *c = d->conf;
    if (c->verbose) {
        Buf b(&scratch, 64 + msg.len);
        print(&b, "prospero: deploy: ");
        print(&b, msg);
        putbyte(&b, '\n');
        os_print(d->os, 2, finish(&b));
    }
    if (c->notes) {
        Buf   b(&scratch, 256 + msg.len + path.len);
        EvOut w = evbegin(&b, {}, 0, os_time(d->os), EV_DEPLOY);
        putstrkey(&w.json, "host", c->name);
        putstrkey(&w.json, "step", step);
        putstrkey(&w.json, "msg", msg);
        if (path.len) {
            putstrkey(&w.json, "path", path);
        }
        if (platform.len) {
            putstrkey(&w.json, "platform", platform);
        }
        mailpost(c->notes, MAIL_OUT, 0, evend(&w), 0);
    }
}

// Wait, unless cancelled. Returns 0 if cancelled.
static b32 deploynap(Deploy *d, i64 wait)
{
    Os *os    = d->os;
    i64 until = os_ticks(os) + wait;
    for (i64 now; (now = os_ticks(os)) < until;) {
        if (deploycancelled(d)) {
            return 0;
        }
        i64 left = until - now;
        mailwait(d->mb, left>200*MS ? 200 : (i32)(left/MS + 1));
        mailtake(d->mb);  // late records of a child given up on
    }
    return !deploycancelled(d);
}

// Run a child to its end, given input and then its input closed,
// keeping its output.
static DeployRun deployrun(Deploy *d, Arena *a, Slice<Str> argv, i64 timeout,
                           Str input = {})
{
    DeployRun r  = {};
    Os       *os = d->os;
    r.code = -1;

    Spawn sp = {};
    sp.argv    = argv;
    sp.cwd     = d->conf->localstate;
    sp.env     = d->env;
    sp.outpath = d->errpath;
    sp.mbox    = d->mb;
    sp.id      = ++d->id;
    Proc *p = os_spawnio(os, &sp, *a);
    if (!p) {
        Str err = os_lasterr(os, a);
        Str msg = concat(a, "cannot run ", argv[0]);
        msg   = concat(a, msg, ": ");
        r.err = concat(a, msg, err);
        return r;
    }
    if (input.len) {
        os_procwrite(os, p, input);
    }
    os_procwrite(os, p, {});

    Buf out(a, DEPLOY_OUTMAX);
    i32 code     = -1;
    b32 exited   = 0;
    b32 eof      = 0;
    b32 killed   = 0;
    i64 deadline = os_ticks(os) + timeout;
    while (!exited || !eof) {
        i64 now = os_ticks(os);
        if (!killed && (now>=deadline || deploycancelled(d))) {
            os_kill(os, p, 1);
            killed    = 1;
            r.timeout = now >= deadline;
            deadline  = now + 5*SEC;  // for the kill to show
        } else if (now >= deadline) {
            break;  // given up on: its late records are skipped
        }
        i64 left = deadline - now;
        mailwait(d->mb, left>200*MS ? 200 : (i32)(left/MS + 1));
        Mail m = mailtake(d->mb);
        for (Record rec; (rec = mailnext(&m)).ok;) {
            if (rec.id != d->id) {
                continue;  // an earlier child's
            }
            switch (rec.type) {
            case PROC_DATA:
                if (out.len+rec.data.len <= out.cap) {
                    print(&out, rec.data);
                }
                break;
            case PROC_EXIT:
                exited = 1;
                code   = get32(rec.data);
                if (!killed && deadline-os_ticks(os) > 2*SEC) {
                    deadline = os_ticks(os) + 2*SEC;  // for its output
                }
                break;
            case PROC_EOF:
                eof = 1;
                break;
            }
        }
    }
    os_procfree(os, p);
    r.code = exited && !killed ? code : -1;
    r.out  = wslutf8(a, finish(&out));
    Read rd = os_read(os, a, d->errpath, 1<<16);
    r.err = rd.err==READ_OK ? wslutf8(a, rd.data) : Str{};
    return r;
}

// Whether a line of a child's output is just this word.
static b32 hasline(Str out, Str word)
{
    for (Str rest = out; rest.len;) {
        Cut l = cut(rest, '\n');
        rest = l.tail;
        if (trim(l.head) == word) {
            return 1;
        }
    }
    return 0;
}

// The last line of some output, for messages, but for what wsl.exe
// writes after its message: "Error code: Wsl/...", or where to read
// more.
static Str lastsaid(Str out)
{
    Str last = {};
    for (Str rest = out; rest.len;) {
        Cut l = cut(rest, '\n');
        Str t = trim(l.head);
        if (t.len && !startswith(t, "Error code: Wsl/") &&
                !startswith(t, "For more information")) last = t;
        rest = l.tail;
    }
    return last;
}

// The last line of a child's errors, else of its output (where wsl.exe
// writes its own), for messages.
static Str deploysaid(DeployRun *r)
{
    return lastsaid(trim(r->err).len ? r->err : r->out);
}

// Failures that trying again never fixes: ssh's, in its errors, and
// wsl.exe's, in its output.
static b32 deploypermanent(DeployRun *r)
{
    Str err = r->err;
    Str out = r->out;
    return find(err, "Permission denied")>=0 ||
           find(err, "Could not resolve hostname")>=0 ||
           find(err, "Host key verification failed")>=0 ||
           find(err, "Name or service not known")>=0 ||
           find(err, "No such file or directory")>=0 ||
           find(out, "Error code: Wsl/")>=0 ||
           find(err, "Windows Subsystem for Linux is not installed")>=0 ||
           find(err, "cannot run ")>=0;
}

// Run a command there, again while ssh itself fails (status 255), with
// a note for the step on each failure. Into WSL, once: its failures are
// not a dropped network.
static DeployRun deployssh(Deploy *d, Arena *a, Str step, Str command)
{
    DeployConf *c = d->conf;
    for (i32 n = 1;; n++) {
        Slice<Str> argv = linkargv(a, c->program, c->ssh, c->wsl, command);
        DeployRun  r    = deployrun(d, a, argv, DEPLOY_STEPTIME);
        if (c->wsl.len || r.code!=255 || n>=DEPLOY_TRIES || deploypermanent(&r)) {
            return r;
        }
        i64 wait = deploywaits[n-1];
        Buf b(a, 256);
        print(&b, "ssh failed (");
        print(&b, deploysaid(&r));
        print(&b, "), trying again in ");
        print(&b, wait/SEC);
        print(&b, " s");
        deploynote(d, *a, step, finish(&b));
        if (!deploynap(d, wait)) {
            return r;
        }
    }
}

static void printsize(Buf *b, i64 n)
{
    if (n < 1<<20) {
        print(b, (n + 1023) >> 10);
        print(b, " KiB");
    } else {
        print(b, n >> 20);
        putbyte(b, '.');
        print(b, ((n & ((1<<20) - 1)) * 10) >> 20);
        print(b, " MiB");
    }
}


// Names

// A platform's programs by name: here, in the engines directory, as a
// release names them, and there, in ~/.prospero, where the engine looks
// for its helper beside it (deskhelpers, engine.cpp).
struct DeployNames {
    Str engine;    // prospero-engine-VERSION-OS-ARCH[.exe]
    Str desktop;   // prospero-desktop-VERSION-OS-ARCH[.exe]
    Str rengine;   // engine-PROTO-VERSION-OS-ARCH[.exe]
    Str rdesktop;  // desktop-PROTO-VERSION-OS-ARCH[.exe]
};

static DeployNames deploynames(Arena *a, Str platform, Str ext)
{
    DeployNames r = {};
    Buf b(a, 64);
    print(&b, "-" PROSPERO_VERSION "-");
    print(&b, platform);
    print(&b, ext);
    Str tail = finish(&b);
    r.engine  = concat(a, "prospero-engine", tail);
    r.desktop = concat(a, "prospero-desktop", tail);
    b = Buf(a, 32);
    print(&b, "-");
    print(&b, (i64)HOST_PROTO);
    Str proto = finish(&b);
    r.rengine  = concat(a, concat(a, "engine", proto), tail);
    r.rdesktop = concat(a, concat(a, "desktop", proto), tail);
    return r;
}


// Deploying

// A failure: noted, and returned.
static Deployed deployfail(Deploy *d, Arena *perm, Arena scratch, Str why)
{
    Deployed r = {};
    r.err = clone(perm, why);
    deploynote(d, scratch, "failed", why);
    return r;
}

// A program to put there: the engine, or the desktop helper beside it.
struct DeployFile {
    Str what;   // for messages: "engine", "desktop helper"
    Str prog;   // its --version's first word: prospero, prospero-desktop
    Str local;  // here
    Str data;
    Str sha;    // of data, in lowercase hex
    Str rpath;  // there, for its shell, from the home directory
    Str upath;  // its upload there, for scp
    Str shown;  // ~/.prospero/NAME
    i32 err;    // reading it: READ_OK, READ_NOTFOUND...
    b32 self;   // it is self, not from dir
};

// The engines directories, unless given, in the order they are looked
// in: "engines" next to this program, then "engines" in the
// configuration directory, beside prospero.ini, where the release's
// engines zip unpacks wherever the program lives: a macOS app in
// /Applications, say, whose bundle stays as signed. Each platform's
// engine comes from the first that has one.
static Slice<Str> enginesdirs(Os *os, Arena *a, Str exe)
{
    Slice<Str> r   = {};
    Str        cfg = cfgdefault(os, a);
    r = push(a, r, join(a, parentdir(exe), "engines"));
    if (cfg.len) {
        Str there = join(a, parentdir(cfg), "engines");
        if (there!=r[0] && os_stat(os, *a, there).type==STAT_DIR) {
            r = push(a, r, there);
        }
    }
    return r;
}

// Read a program to upload, from dir/file, or else from self when it
// is not there and self is not empty. Its place there is .prospero/
// rname. Null if ok, else why not (unless it is just missing: err).
static Str deployread(Os *os, Arena *a, DeployFile *f, Str dir, Str file,
                      Str self, Str rname, i32 shell)
{
    f->local = join(a, dir, file);
    Read rd  = os_read(os, a, f->local, DEPLOY_MAXSIZE);
    if (rd.err==READ_NOTFOUND && self.len) {
        f->local = self;
        f->self  = 1;
        rd       = os_read(os, a, f->local, DEPLOY_MAXSIZE);
    }
    f->err = rd.err;
    if (rd.err == READ_NOTFOUND) {
        return {};
    } else if (rd.err != READ_OK) {
        return concat(a, "cannot read ", f->local);
    }
    Str full = os_realpath(os, a, f->local);  // for scp, which runs elsewhere
    f->local = full.len ? full : f->local;
    f->data  = rd.data;
    f->sha   = cfgsha256(a, rd.data);
    Str  sums = concat(a, f->local, ".sha256");
    Read want = os_read(os, a, sums, 1<<12);
    if (want.err==READ_OK && deployhash(a, want.data)!=f->sha) {
        Buf b(a, 256);
        print(&b, f->local);
        print(&b, " does not match ");
        print(&b, sums);
        return finish(&b);
    }
    Str rdir = shell==DEPLOY_CMD ? Str(".prospero\\") : Str(".prospero/");
    f->rpath = concat(a, rdir, rname);
    f->upath = concat(a, ".prospero/", rname);
    f->upath = concat(a, f->upath, ".part");
    f->shown = concat(a, "~/.prospero/", rname);
    return {};
}

struct DeployPut {
    Str err;   // why not, unless ok
    b32 ok;
    b32 same;  // it was there already
};

static DeployPut deployputfail(Arena *perm, Str why)
{
    DeployPut r = {};
    r.err = clone(perm, why);
    return r;
}

// Put a program in its place there, unless it is there already, and run
// it once (--version). Notes progress, but not the failure it returns.
static DeployPut deployput(Deploy *d, Arena *perm, Arena scratch, Platform *pf,
                           DeployFile *f, i64 stamp)
{
    DeployConf *conf = d->conf;
    Str         name = conf->name.len ? conf->name : linkdest(&scratch, conf->ssh,
                                                              conf->wsl);

    Str       cmd    = deploycmd(&scratch, pf->shell, DEPLOY_PREPARE, f->rpath, stamp);
    DeployRun prep   = deployssh(d, &scratch, "probe", cmd);
    b32       ready  = hasline(prep.out, "ready");
    b32       have   = hasline(prep.out, "have");
    b32       nohash = hasline(prep.out, "nohash");
    Str there = have ? deployhash(&scratch, prep.out) : Str{};
    Buf b(&scratch, 256);
    if (deploycancelled(d)) {
        return deployputfail(perm, "cancelled");
    } else if (!ready) {
        print(&b, "cannot create ~/.prospero on ");
        print(&b, name);
        print(&b, ": ");
        print(&b, deploysaid(&prep));
        return deployputfail(perm, finish(&b));
    } else if (nohash || (have && !there.len)) {
        print(&b, "cannot compute a SHA-256 on ");
        print(&b, name);
        print(&b, pf->shell==DEPLOY_CMD ? Str(": no certutil")
                                        : Str(": no sha256sum, shasum or openssl"));
        return deployputfail(perm, finish(&b));
    } else if (have && there!=f->sha && !conf->force) {
        print(&b, name);
        print(&b, " has another build of ");
        print(&b, f->prog);
        print(&b, " " PROSPERO_VERSION " at ");
        print(&b, f->shown);
        print(&b, ": --force replaces it (what runs from it keeps running)");
        return deployputfail(perm, finish(&b));
    }

    // Upload and verify, unless it is there already
    b32 same = have && there==f->sha && !conf->force;
    for (i32 n = 1; !same; n++) {
        b = Buf(&scratch, 256);
        print(&b, "uploading ");
        print(&b, basename(f->local));
        print(&b, " (");
        printsize(&b, f->data.len);
        print(&b, ") to ");
        print(&b, f->shown);
        print(&b, ".part");
        if (n > 1) {
            print(&b, ", try ");
            print(&b, (i64)n);
        }
        deploynote(d, scratch, "upload", finish(&b));

        Arena      a     = scratch;
        Slice<Str> argv  = {};
        Str        input = {};
        if (conf->wsl.len) {
            // No scp there: the data on cat's input
            Str cat = concat(&a, "cat > ", f->upath);
            argv  = linkargv(&a, conf->program, {}, conf->wsl, cat);
            input = f->data;
        } else {
            argv = push(&a, argv, conf->scp.len ? conf->scp : Str("scp"));
            argv = push(&a, argv, Str("-q"));
            for (iz i = 0; i < countof(deployopts); i++) {
                argv = push(&a, argv, deployopts[i]);
            }
            argv = push(&a, argv, Str("--"));
            argv = push(&a, argv, f->local);
            Str dest = concat(&a, conf->ssh, ":");
            argv = push(&a, argv, concat(&a, dest, f->upath));
        }
        i64       limit = DEPLOY_UPTIME + f->data.len/DEPLOY_UPRATE*SEC;
        DeployRun up    = deployrun(d, &a, argv, limit, input);
        Str       why   = {};
        if (deploycancelled(d)) {
            return deployputfail(perm, "cancelled");
        } else if (up.code != 0) {
            b = Buf(&a, 256);
            print(&b, up.timeout ? Str("the upload took too long")
                                 : Str("the upload failed"));
            Str said = deploysaid(&up);
            if (said.len) {
                print(&b, " (");
                print(&b, said);
                print(&b, ")");
            }
            why = finish(&b);
            if (deploypermanent(&up)) {
                return deployputfail(perm, why);
            }
        } else {
            deploynote(d, a, "verify", "verifying its SHA-256 there");
            Str       vc  = deploycmd(&a, pf->shell, DEPLOY_VERIFY, f->rpath, stamp);
            DeployRun v   = deployssh(d, &a, "verify", vc);
            Str       got = deployhash(&a, v.out);
            if (got == f->sha) {
                break;
            } else if (!got.len && hasline(v.out, "nohash")) {
                b = Buf(&a, 256);
                print(&b, "cannot compute a SHA-256 on ");
                print(&b, name);
                print(&b, ": no sha256sum, shasum or openssl");
                return deployputfail(perm, finish(&b));
            } else if (got.len) {
                why = concat(&a, "the upload arrived damaged, its SHA-256 ", got);
            } else if (v.code == 0) {
                b = Buf(&a, 256);
                print(&b, "cannot compute a SHA-256 on ");
                print(&b, name);
                print(&b, ": ");
                print(&b, deploysaid(&v));
                return deployputfail(perm, finish(&b));
            } else {
                why = concat(&a, "cannot verify the upload: ", deploysaid(&v));
            }
        }
        if (n >= DEPLOY_TRIES) {
            return deployputfail(perm, why);
        }
        i64 wait = deploywaits[n-1];
        b = Buf(&a, 256);
        print(&b, why);
        print(&b, ", trying again in ");
        print(&b, wait/SEC);
        print(&b, " s");
        deploynote(d, a, "upload", finish(&b));
        if (!deploynap(d, wait)) {
            return deployputfail(perm, "cancelled");
        }
    }

    // Into place, and run once
    b = Buf(&scratch, 128);
    print(&b, same ? Str("already there") : Str("renaming"));
    print(&b, " the ");
    print(&b, f->what);
    print(&b, same ? Str(", running it once") : Str(" into place, running it once"));
    deploynote(d, scratch, "install", finish(&b));
    cmd = deploycmd(&scratch, pf->shell, same ? DEPLOY_RUN : DEPLOY_INSTALL, f->rpath, stamp);
    DeployRun run  = deployssh(d, &scratch, "install", cmd);
    Str       want = concat(&scratch, f->prog, " " PROSPERO_VERSION);
    if (deploycancelled(d)) {
        return deployputfail(perm, "cancelled");
    } else if (find(run.out, want) < 0) {
        b = Buf(&scratch, 256);
        print(&b, "the ");
        print(&b, f->what);
        print(&b, " does not run on ");
        print(&b, name);
        Str said = deploysaid(&run);
        if (said.len) {
            print(&b, ": ");
            print(&b, said);
        }
        Str why = finish(&b);
        if (!same && run.code!=255) {
            Str rm = deploycmd(&scratch, pf->shell, DEPLOY_REMOVE, f->rpath, stamp);
            deployssh(d, &scratch, "install", rm);  // put there now, and useless
        }
        return deployputfail(perm, why);
    }
    DeployPut r = {};
    r.ok   = 1;
    r.same = same;
    return r;
}

// Deploy an engine as described at the top, and its desktop helper if
// there is one. Blocks until done; another thread may cancel through
// conf->cancel.
static Deployed deploy(Os *os, Arena *perm, Arena scratch, DeployConf *conf)
{
    Deploy d = {};
    d.os   = os;
    d.conf = conf;
    d.mb   = newmailbox(perm, 1<<16, 0, 0);  // children may outlive a call
    d.env  = linkenv(perm, os_environ(os, perm), conf->wsl.len>0);
    if (conf->envpath.len) {
        d.env = envsetpath(perm, d.env, conf->envpath, HOST_WINDOWS);
    }
    Str dest = linkdest(perm, conf->ssh, conf->wsl);
    Str name = conf->name.len ? conf->name : dest;
    Str dir  = join(&scratch, conf->localstate, "deploy");
    Str bad  = linkbad(conf->ssh, conf->wsl);
    if (bad.len) {
        return deployfail(&d, perm, scratch, bad);
    } else if (!d.mb || !os_mkdirs(os, scratch, dir)) {
        Str err = os_lasterr(os, &scratch);
        Str why = concat(&scratch, "cannot create the state directory: ", err);
        return deployfail(&d, perm, scratch, why);
    }
    d.errpath = deployfile(perm, conf->localstate, dest, ".err");

    // The platform
    Buf b(&scratch, 256);
    print(&b, "probing ");
    print(&b, name);
    deploynote(&d, scratch, "probe", finish(&b));
    DeployRun pr = deployssh(&d, &scratch, "probe", deployprobe);
    Platform  pf = deployplatform(&scratch, pr.out);
    if (deploycancelled(&d)) {
        return deployfail(&d, perm, scratch, "cancelled");
    } else if (!pf.ok) {
        b = Buf(&scratch, 256);
        print(&b, pr.code==255 || pr.code<0 ? Str("cannot reach ")
                                            : Str("cannot tell the platform of "));
        print(&b, name);
        Str said = deploysaid(&pr);
        if (said.len) {
            print(&b, ": ");
            print(&b, said);
        }
        return deployfail(&d, perm, scratch, finish(&b));
    }

    // The engine for it, and its place there (deploynames)
    b32 win  = pf.os == "windows";
    Str ext  = win ? Str(".exe") : Str("");
    Str exe  = os_exepath(os, &scratch);
    Str self = pf.name==deployself() ? exe : Str{};  // this program serves
    Slice<Str> dirs = {};
    dirs = conf->engines.len ? push(&scratch, dirs, conf->engines)
                             : enginesdirs(os, &scratch, exe);
    DeployNames names = deploynames(&scratch, pf.name, ext);

    // The engine from the first directory with one, else this program
    // when it serves; its helper from the same place
    DeployFile eng  = {};
    Str        from = {};
    Str        err  = {};
    eng.err = READ_NOTFOUND;
    for (iz i = 0; i<dirs.len && eng.err==READ_NOTFOUND; i++) {
        eng      = {};
        eng.what = "engine";
        eng.prog = "prospero";
        from     = dirs[i];
        err      = deployread(os, &scratch, &eng, from, names.engine,
                              i+1==dirs.len ? self : Str{}, names.rengine, pf.shell);
    }
    if (eng.err == READ_NOTFOUND) {
        b = Buf(&scratch, 512);
        print(&b, "no engine for ");
        print(&b, pf.name);
        print(&b, " (");
        print(&b, pf.about);
        print(&b, "): ");
        print(&b, names.engine);
        print(&b, " is in neither ");
        for (iz i = 0; i < dirs.len; i++) {
            print(&b, i ? Str(" nor ") : Str(""));
            print(&b, dirs[i]);
        }
        print(&b, " (tools/engines.py builds it)");
        return deployfail(&d, perm, scratch, finish(&b));
    } else if (err.len) {
        return deployfail(&d, perm, scratch, err);
    }

    // The desktop helper beside it, if any: in the engines directory, or
    // else, when this program serves, its own helper next to it. None for
    // an engine that is its own helper ("prospero desktop"): Windows', and
    // this program serving when it is one (PROSPERO_DESKTOP).
    DeployFile desk = {};
    desk.what = "desktop helper";
    desk.prog = "prospero-desktop";
    Str deskerr = {};
    b32 hasdesk = 0;
    if (!win && !(eng.self && PROSPERO_DESKTOP)) {
        Str selfdesk = eng.self ? join(&scratch, parentdir(exe), "prospero-desktop") : Str{};
        deskerr = deployread(os, &scratch, &desk, from, names.desktop, selfdesk,
                             names.rdesktop, pf.shell);
        hasdesk = desk.err == READ_OK;
    }

    b = Buf(&scratch, 256);
    print(&b, name);
    print(&b, " is ");
    print(&b, pf.name);
    print(&b, " (");
    print(&b, pf.about);
    print(&b, pf.shell==DEPLOY_CMD ? Str("; cmd.exe") : Str(""));
    print(&b, ")");
    deploynote(&d, scratch, "probe", finish(&b));

    i64       stamp = os_time(os);
    DeployPut put   = deployput(&d, perm, scratch, &pf, &eng, stamp);
    if (!put.ok) {
        return deployfail(&d, perm, scratch, put.err);
    }

    // The helper is optional: the engine works without it, so a helper
    // that fails is a warning, and the desktop tool is unavailable there
    DeployPut dput = {};
    if (deskerr.len) {
        dput.err = deskerr;
    } else if (hasdesk) {
        dput = deployput(&d, perm, scratch, &pf, &desk, stamp);
    }
    if (deploycancelled(&d)) {
        return deployfail(&d, perm, scratch, "cancelled");
    } else if (dput.err.len) {
        b = Buf(&scratch, 256 + dput.err.len);
        print(&b, "warning: the desktop helper is not deployed: ");
        print(&b, dput.err);
        deploynote(&d, scratch, "install", finish(&b));
    }

    // Recorded, for connecting
    b = Buf(&scratch, 512);
    print(&b, "host = ");
    print(&b, dest);
    print(&b, "\nplatform = ");
    print(&b, pf.name);
    print(&b, "\nshell = ");
    print(&b, pf.shell==DEPLOY_CMD ? Str("cmd") : Str("sh"));
    print(&b, "\npath = ");
    print(&b, eng.rpath);
    print(&b, "\nsha256 = ");
    print(&b, eng.sha);
    if (dput.ok) {
        print(&b, "\ndesktop = ");
        print(&b, desk.rpath);
    }
    print(&b, "\nversion = " PROSPERO_VERSION "\nproto = ");
    print(&b, (i64)HOST_PROTO);
    print(&b, "\ndeployed = ");
    print(&b, os_time(os));
    putbyte(&b, '\n');
    Str rec = deployfile(&scratch, conf->localstate, dest, ".ini");
    if (!os_write(os, scratch, rec, finish(&b))) {
        Str err = os_lasterr(os, &scratch);
        Str why = concat(&scratch, "cannot record the deployment: ", err);
        return deployfail(&d, perm, scratch, why);
    }

    Deployed r = {};
    r.ok       = 1;
    r.path     = clone(perm, eng.rpath);
    r.shown    = clone(perm, eng.shown);
    r.platform = clone(perm, pf.name);
    b32 same   = put.same && (!dput.ok || dput.same);
    b = Buf(&scratch, 256);
    print(&b, same ? Str("already deployed: ") : Str("deployed: "));
    print(&b, eng.shown);
    if (dput.ok) {
        print(&b, ", with the desktop helper");
    }
    deploynote(&d, scratch, "done", finish(&b), eng.shown, pf.name);
    return r;
}


// WSL distros as remotes

// The distros in wsl.exe -l -q's output, which is UTF-16 (little
// endian) unless it honors WSL_UTF8, as older ones do not, else UTF-8.
// Docker Desktop's own (docker-desktop, docker-desktop-data) are left
// out, as is anything but a plain name: letters, digits, ., - and _,
// as WSL allows.
static Slice<Str> wsldistros(Arena *a, Str out)
{
    Slice<Str> r = {};
    for (Str rest = wslutf8(a, out); rest.len;) {
        Cut l    = cut(rest, '\n');
        Str name = trim(l.head);
        rest = l.tail;
        if (startswith(name, "\xef\xbb\xbf")) {
            name = cuthead(name, 3);  // a BOM, in UTF-8
        }
        b32 ok = name.len > 0;
        for (iz i = 0; ok && i<name.len; i++) {
            u8 c = name[i];
            ok = alnum(c) || c=='.' || c=='-' || c=='_';
        }
        if (ok && !startswith(name, "docker-desktop")) {
            r = push(a, r, name);
        }
    }
    return r;
}

// The installed WSL distros, by wsl.exe (or program) -l -q, which
// starts none of them. None where it cannot run or fails: no WSL, no
// distros, or not Windows (unless program, a test's stand-in). It runs
// in dir. Since a child given up on may write late, the records go to
// perm.
static Slice<Str> wsllist(Os *os, Arena *perm, Arena scratch, Str program, Str dir)
{
    if (!program.len && !HOST_WINDOWS) {
        return {};
    }
    DeployConf c = {};
    c.localstate = dir;
    Deploy d = {};
    d.os   = os;
    d.conf = &c;
    d.mb   = newmailbox(perm, 1<<16, 0, 0);
    d.env  = linkenv(&scratch, os_environ(os, &scratch), 1);
    if (!d.mb) {
        return {};
    }
    Slice<Str> argv = {};
    argv = push(&scratch, argv, program.len ? program : Str("wsl.exe"));
    argv = push(&scratch, argv, Str("-l"));
    argv = push(&scratch, argv, Str("-q"));
    DeployRun r = deployrun(&d, &scratch, argv, 15*SEC);
    return r.code==0 ? wsldistros(perm, r.out) : Slice<Str>{};
}

static b32 driveletter(u8 c)
{
    return (c>='a' && c<='z') || (c>='A' && c<='Z');
}

// A Windows path typed for a WSL distro, as the distro has it under the
// default automount root: a drive's "C:\" or "C:/" becomes "/mnt/c/",
// "C:" alone "/mnt/c", and under a drive's /mnt/X a backslash becomes a
// slash, as no Windows name holds one. Any other text as it is, such as
// a drive-relative "C:x". The length may change only at its head.
static Str wslpath(Arena *a, Str t)
{
    Str rest = {};
    u8  d    = 0;
    if (t.len>=2 && driveletter(t[0]) && t[1]==':' && (t.len==2 || t[2]=='\\' || t[2]=='/')) {
        d    = t[0] | 0x20;
        rest = cuthead(t, 2);
    } else if (t.len>=6 && takehead(t, 5)=="/mnt/" && driveletter(t[5]) &&
               (t.len==6 || t[6]=='\\' || t[6]=='/')) {
        d    = t[5];
        rest = cuthead(t, 6);
    }
    if (!d || (t[0]=='/' && find(rest, "\\")<0)) {
        return t;
    }
    Buf b(a, 6 + rest.len);
    print(&b, "/mnt/");
    putbyte(&b, d);
    for (iz i = 0; i < rest.len; i++) {
        putbyte(&b, rest[i]=='\\' ? '/' : rest[i]);
    }
    return finish(&b);
}

// The head of a Windows path up to its first separator.
static Str winpart(Str s)
{
    iz i = 0;
    for (; i<s.len && !issep(s[i], 1); i++) {}
    return takehead(s, i);
}

// A Windows path into a distro's own files, \\wsl.localhost\D\x or the
// older \\wsl$\D\x, with either slash and without case, as that distro
// has it: "/x", or "/" for its root. Null for any other path, one into
// another distro included.
static Str wsluncpath(Arena *a, Str t, Str distro)
{
    if (t.len<2 || !issep(t[0], 1) || !issep(t[1], 1)) {
        return {};
    }
    Str rest   = cuthead(t, 2);
    Str server = winpart(rest);
    if (!equalfold(server, "wsl.localhost") && !equalfold(server, "wsl$")) {
        return {};
    }
    rest = cuthead(rest, server.len);
    rest = rest.len ? cuthead(rest, 1) : rest;
    Str share = winpart(rest);
    if (!distro.len || !equalfold(share, distro)) {
        return {};
    }
    rest = cuthead(rest, share.len);
    Buf b(a, 1 + rest.len);
    putbyte(&b, '/');
    for (iz i = rest.len && issep(rest[0], 1); i < rest.len; i++) {
        putbyte(&b, rest[i]=='\\' ? '/' : rest[i]);
    }
    return finish(&b);
}

// The program that reaches a remote, ssh or wsl.exe, by name if empty,
// unless a test's stand-in: PROSPERO_TEST_SSH, PROSPERO_TEST_WSL.
static Str linkstandin(Os *os, Arena *a, b32 wsl)
{
    return os_getenv(os, a, wsl ? Str("PROSPERO_TEST_WSL") : Str("PROSPERO_TEST_SSH"));
}

// How a remote reaches its machine.
struct LinkTo {
    Str name;  // the remote's
    Str ssh;
    Str wsl;
};

// The installed distros as remotes, but for those a [remote] claims
// (wsl = DISTRO, without ssh, which wins), each named after its distro,
// or DISTRO-wsl where a remote of that name reaches something else.
// remotes are the configured ones.
static Slice<LinkTo> wslhosts(Arena *a, Slice<Str> distros, Slice<LinkTo> remotes)
{
    Slice<LinkTo> r = {};
    for (iz i = 0; i < distros.len; i++) {
        Str  d       = distros[i];
        b32  claimed = 0;
        b32  taken   = 0;
        for (iz n = 0; n < remotes.len; n++) {
            LinkTo *o = &remotes[n];
            claimed |= !o->ssh.len && o->wsl==d;
            taken   |= o->name==d && (o->ssh.len || o->wsl.len);
        }
        if (!claimed) {
            LinkTo h = {};
            h.name = taken ? concat(a, d, "-wsl") : d;
            h.wsl  = d;
            r = push(a, r, h);
        }
    }
    return r;
}

// The configured remotes, each as its settings reach it: ssh, else wsl.
static Slice<LinkTo> cfgremotes(Arena *a, Config *cfg)
{
    Slice<LinkTo> r = {};
    for (IniSection *s = cfg->ini->sections; s; s = s->next) {
        if (s->kind=="remote" && s->name.len) {
            LinkTo t = {};
            t.name = s->name;
            t.ssh  = trim(cfgget(cfg, "remote", s->name, "ssh", 0));
            t.wsl  = t.ssh.len ? Str{} : trim(cfgget(cfg, "remote", s->name, "wsl", 0));
            r = push(a, r, t);
        }
    }
    return r;
}

// How the remote of a name reaches its machine: as configured, or else
// an installed distro (wsllist, with program) of that name (wslhosts).
// Neither ssh nor wsl when there is no such remote.
static LinkTo linkto(Os *os, Arena *perm, Arena scratch, Config *cfg, Str name,
                     Str program, Str dir)
{
    Slice<LinkTo> remotes = cfgremotes(&scratch, cfg);
    for (iz i = 0; i < remotes.len; i++) {
        if (remotes[i].name==name && (remotes[i].ssh.len || remotes[i].wsl.len)) {
            LinkTo r = remotes[i];
            r.ssh = clone(perm, r.ssh);
            r.wsl = clone(perm, r.wsl);
            return r;
        }
    }
    Slice<Str>    distros = wsllist(os, perm, scratch, program, dir);
    Slice<LinkTo> hosts   = wslhosts(&scratch, distros, remotes);
    LinkTo        r       = {};
    r.name = name;
    for (iz i = 0; i < hosts.len; i++) {
        if (hosts[i].name == name) {
            r.wsl = clone(perm, hosts[i].wsl);
        }
    }
    return r;
}


// prospero deploy

static Str deployusage =
"usage: prospero deploy NAME|DEST [--force] [options]\n"
"       prospero deploy --wsl DISTRO [--force] [options]\n"
"Put an engine for the platform of a remote machine, a [remote NAME],\n"
"an ssh destination, or a WSL distro, in ~/.prospero there, over ssh and\n"
"scp (or wsl.exe), where \"prospero connect\" then runs it, and its\n"
"desktop helper beside it if the engines directory has one. Prints\n"
"where the engine went.\n"
"options:\n"
"  --force             replace another build of this version there\n"
"  --engines DIR       the engines to choose from (default: engines\n"
"                      next to this program, else engines beside the\n"
"                      default prospero.ini)\n"
"  --wsl DISTRO        a WSL distro, through wsl.exe\n"
"  --ssh-program PROG  ssh itself (default: ssh)\n"
"  --scp-program PROG  scp itself (default: scp)\n"
"  --wsl-program PROG  wsl.exe itself (default: wsl.exe)\n"
"  -c FILE             configuration file\n"
"  --state DIR         session storage directory here\n";

static i32 deploymain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    DeployConf dc = {};
    Str target = {}, config = {}, state = {}, sshprog = {}, wslprog = {};
    for (i32 i = 2; i < nargs; i++) {
        Str a    = args[i];
        b32 more = i+1 < nargs;
        if (a == "--force") {
            dc.force = 1;
        } else if (a=="--engines" && more) {
            dc.engines = args[++i];
        } else if (a=="--wsl" && more) {
            dc.wsl = args[++i];
        } else if (a=="--ssh-program" && more) {
            sshprog = args[++i];
        } else if (a=="--wsl-program" && more) {
            wslprog = args[++i];
        } else if (a=="--scp-program" && more) {
            dc.scp = args[++i];
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (!target.len && a.len && a[0]!='-') {
            target = a;
        } else {
            os_print(os, 2, deployusage);
            return 2;
        }
    }
    if (!target.len == !dc.wsl.len) {
        os_print(os, 2, deployusage);
        return 2;
    }
    // Only cached copies of URL includes: deploy runs no engine here
    dc.localstate = state.len ? state : defaultstate(os, &perm);
    Config *cfg   = cliconfig(os, &perm, scratch, config, dc.localstate, CFG_NETNONE);
    if (!cfg) {
        return 2;
    }
    // A remote's name (a distro's, unconfigured), or else a destination
    if (!dc.wsl.len) {
        LinkTo to = {};
        if (os_mkdirs(os, scratch, dc.localstate)) {
            to = linkto(os, &perm, scratch, cfg, target, wslprog, dc.localstate);
        }
        dc.ssh = to.ssh.len || to.wsl.len ? to.ssh : target;
        dc.wsl = to.wsl;
    }
    dc.name    = target.len ? target : dc.wsl;
    dc.program = dc.wsl.len ? wslprog : sshprog;
    dc.envpath = cfgownpath(cfg, &perm);
    dc.verbose = 1;

    Deployed r = deploy(os, &perm, scratch, &dc);
    if (!r.ok) {
        return 1;
    }
    os_print(os, 1, concat(&scratch, r.shown, "\n"));
    return 0;
}

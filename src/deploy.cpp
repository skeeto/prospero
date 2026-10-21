// Engine deployment: "harness deploy", and deploying for a Remote
//
//   harness deploy NAME|DEST [--force] [options]
//
// Puts an engine built for the remote machine's platform in ~/.harness
// there, over ssh and scp, where "harness connect" (remote.cpp) then
// runs it:
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
// 2. Choose harness-engine-VERSION-OS-ARCH[.exe] from the engines
//    directory ("engines" next to this executable unless given; built
//    by tools/engines.py), checked against its .sha256 file. With none
//    there for the platform, this executable serves when it is built
//    for that platform.
// 3. Prepare ~/.harness there (mode 0700), learning what is already at
//    the engine's place, .harness/engine-PROTO-VERSION-OS-ARCH[.exe]:
//    the same engine means nothing to upload.
// 4. Upload with scp to that name plus ".part", again after a dropped
//    link (the XP laptop's Wi-Fi stalls), and verify its SHA-256 there
//    (sha256sum, shasum -a 256 or openssl; certutil -hashfile in cmd).
//    A damaged copy is uploaded again.
// 5. Rename it into place, and run it once (--version): an engine that
//    cannot run there (too old a C library, say) is removed again.
// 6. Record where it went in <state>/deploy/DEST.ini, for connecting:
//    host, platform, shell, path (for the remote shell), sha256,
//    version, proto, and deployed (Unix epoch ms).
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
// waits between them: together about as long as the XP laptop's Wi-Fi
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
    Str       program;     // ssh itself, "ssh" if empty
    Str       scp;         // scp itself, "scp" if empty
    Str       engines;     // directory of engines, if not next to this program
    Str       localstate;  // the record goes under deploy/ there
    b32       force;       // replace another build of this version
    b32       verbose;     // progress on standard error
    Mailbox  *notes;       // deploy records (MAIL_OUT), if any
    i32      *cancel;      // another thread sets it to stop (may be null)
};

struct Deployed {
    Str path;      // the engine there, for its shell, from the home directory
    Str shown;     // the same as ~/.harness/NAME
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
    DEPLOY_PREPARE,  // make ~/.harness, and hash what is at the place
    DEPLOY_VERIFY,   // hash the upload
    DEPLOY_INSTALL,  // rename it into place, then run it
    DEPLOY_RUN,      // run what is there
    DEPLOY_REMOVE,   // remove what does not run
};

// A command for a step, in the shell's dialect, about the engine at
// path (.harness/NAME, from the home directory). The stamp names an
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
            print(&b, "mkdir -p .harness && chmod 700 .harness && echo ready; "
                      "if [ -f ");
            print(&b, p);
            print(&b, " ]; then echo have; { sha256sum ");
            print(&b, p);
            print(&b, " || shasum -a 256 ");
            print(&b, p);
            print(&b, " || openssl dgst -sha256 -r ");
            print(&b, p);
            print(&b, "; } 2>/dev/null; fi");
        } else {
            print(&b, "(if not exist .harness mkdir .harness) & "
                      "(if exist .harness echo ready) & (if exist ");
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
            print(&b, ".part; } 2>/dev/null");
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


// Running ssh and scp

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
        print(&b, "harness: deploy: ");
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

// Run a child to its end, its input closed, keeping its output.
static DeployRun deployrun(Deploy *d, Arena *a, Slice<Str> argv, i64 timeout)
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
    r.out  = finish(&out);
    Read rd = os_read(os, a, d->errpath, 1<<16);
    r.err = rd.err==READ_OK ? rd.data : Str{};
    return r;
}

// The last line of a child's errors, else of its output, for messages.
static Str deploysaid(DeployRun *r)
{
    Str last = {};
    Str from = trim(r->err).len ? r->err : r->out;
    for (Str rest = from; rest.len;) {
        Cut l = cut(rest, '\n');
        if (trim(l.head).len) last = trim(l.head);
        rest = l.tail;
    }
    return last;
}

// Failures that trying again never fixes.
static b32 deploypermanent(Str err)
{
    return find(err, "Permission denied")>=0 ||
           find(err, "Could not resolve hostname")>=0 ||
           find(err, "Host key verification failed")>=0 ||
           find(err, "Name or service not known")>=0 ||
           find(err, "No such file or directory")>=0;
}

// Run a command there, again while ssh itself fails (status 255), with
// a note for the step on each failure.
static DeployRun deployssh(Deploy *d, Arena *a, Str step, Str command)
{
    DeployConf *c = d->conf;
    for (i32 n = 1;; n++) {
        Slice<Str> argv = {};
        argv = push(a, argv, c->program.len ? c->program : Str("ssh"));
        argv = push(a, argv, Str("-T"));
        for (iz i = 0; i < countof(deployopts); i++) {
            argv = push(a, argv, deployopts[i]);
        }
        argv = push(a, argv, c->ssh);
        argv = push(a, argv, command);
        DeployRun r = deployrun(d, a, argv, DEPLOY_STEPTIME);
        if (r.code!=255 || n>=DEPLOY_TRIES || deploypermanent(r.err)) {
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


// Deploying

// A failure: noted, and returned.
static Deployed deployfail(Deploy *d, Arena *perm, Arena scratch, Str why)
{
    Deployed r = {};
    r.err = clone(perm, why);
    deploynote(d, scratch, "failed", why);
    return r;
}

// Deploy an engine as described at the top. Blocks until done; another
// thread may cancel through conf->cancel.
static Deployed deploy(Os *os, Arena *perm, Arena scratch, DeployConf *conf)
{
    Deploy d = {};
    d.os   = os;
    d.conf = conf;
    d.mb   = newmailbox(perm, 1<<16, 0, 0);  // children may outlive a call
    d.env  = os_environ(os, perm);
    Str name = conf->name.len ? conf->name : conf->ssh;
    Str dir  = join(&scratch, conf->localstate, "deploy");
    if (!d.mb || !os_mkdirs(os, scratch, dir)) {
        Str err = os_lasterr(os, &scratch);
        Str why = concat(&scratch, "cannot create the state directory: ", err);
        return deployfail(&d, perm, scratch, why);
    }
    d.errpath = deployfile(perm, conf->localstate, conf->ssh, ".err");

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

    // The engine for it
    b32 win  = pf.os == "windows";
    Str exe  = os_exepath(os, &scratch);
    Str file = concat(&scratch, "harness-engine-" HARNESS_VERSION "-", pf.name);
    file = win ? concat(&scratch, file, ".exe") : file;
    Str from = conf->engines.len ? conf->engines : join(&scratch, parentdir(exe), "engines");
    Str local = join(&scratch, from, file);
    Read eng  = os_read(os, &scratch, local, DEPLOY_MAXSIZE);
    if (eng.err==READ_NOTFOUND && pf.name==deployself() && exe.len) {
        local = exe;  // this program serves
        eng   = os_read(os, &scratch, local, DEPLOY_MAXSIZE);
    }
    if (eng.err != READ_OK) {
        b = Buf(&scratch, 256);
        if (eng.err == READ_NOTFOUND) {
            print(&b, "no engine for ");
            print(&b, pf.name);
            print(&b, " (");
            print(&b, pf.about);
            print(&b, "): ");
            print(&b, local);
            print(&b, " is missing (tools/engines.py builds it)");
        } else {
            print(&b, "cannot read ");
            print(&b, local);
        }
        return deployfail(&d, perm, scratch, finish(&b));
    }
    Str  sha  = cfgsha256(&scratch, eng.data);
    Str  sums = concat(&scratch, local, ".sha256");
    Read want = os_read(os, &scratch, sums, 1<<12);
    if (want.err==READ_OK && deployhash(&scratch, want.data)!=sha) {
        b = Buf(&scratch, 256);
        print(&b, local);
        print(&b, " does not match ");
        print(&b, sums);
        return deployfail(&d, perm, scratch, finish(&b));
    }

    // Its place there
    b = Buf(&scratch, 128);
    print(&b, "engine-");
    print(&b, (i64)HOST_PROTO);
    print(&b, "-" HARNESS_VERSION "-");
    print(&b, pf.name);
    print(&b, win ? Str(".exe") : Str(""));
    Str rname = finish(&b);
    Str rpath = concat(&scratch, pf.shell==DEPLOY_CMD ? Str(".harness\\")
                                                      : Str(".harness/"), rname);
    Str upath = concat(&scratch, ".harness/", rname);
    upath = concat(&scratch, upath, ".part");
    Str shown = concat(&scratch, "~/.harness/", rname);
    i64 stamp = os_time(os);

    b = Buf(&scratch, 256);
    print(&b, name);
    print(&b, " is ");
    print(&b, pf.name);
    print(&b, " (");
    print(&b, pf.about);
    print(&b, pf.shell==DEPLOY_CMD ? Str("; cmd.exe") : Str(""));
    print(&b, ")");
    deploynote(&d, scratch, "probe", finish(&b));
    Str       cmd   = deploycmd(&scratch, pf.shell, DEPLOY_PREPARE, rpath, stamp);
    DeployRun prep  = deployssh(&d, &scratch, "probe", cmd);
    b32       ready = 0;
    b32       have  = 0;
    for (Str rest = prep.out; rest.len;) {
        Cut l = cut(rest, '\n');
        rest  = l.tail;
        ready |= trim(l.head) == "ready";
        have  |= trim(l.head) == "have";
    }
    Str there = have ? deployhash(&scratch, prep.out) : Str{};
    if (deploycancelled(&d)) {
        return deployfail(&d, perm, scratch, "cancelled");
    } else if (!ready) {
        b = Buf(&scratch, 256);
        print(&b, "cannot create ~/.harness on ");
        print(&b, name);
        print(&b, ": ");
        print(&b, deploysaid(&prep));
        return deployfail(&d, perm, scratch, finish(&b));
    } else if (have && !there.len) {
        b = Buf(&scratch, 256);
        print(&b, "cannot compute a SHA-256 on ");
        print(&b, name);
        print(&b, pf.shell==DEPLOY_CMD ? Str(": no certutil")
                                       : Str(": no sha256sum, shasum or openssl"));
        return deployfail(&d, perm, scratch, finish(&b));
    } else if (have && there!=sha && !conf->force) {
        b = Buf(&scratch, 256);
        print(&b, name);
        print(&b, " has another build of harness " HARNESS_VERSION " at ");
        print(&b, shown);
        print(&b, ": --force replaces it (what runs from it keeps running)");
        return deployfail(&d, perm, scratch, finish(&b));
    }

    // Upload and verify, unless it is there already
    b32 same = have && there==sha && !conf->force;
    for (i32 n = 1; !same; n++) {
        b = Buf(&scratch, 256);
        print(&b, "uploading ");
        print(&b, basename(local));
        print(&b, " (");
        printsize(&b, eng.data.len);
        print(&b, ") to ");
        print(&b, shown);
        print(&b, ".part");
        if (n > 1) {
            print(&b, ", try ");
            print(&b, (i64)n);
        }
        deploynote(&d, scratch, "upload", finish(&b));

        Arena      a    = scratch;
        Slice<Str> argv = {};
        argv = push(&a, argv, conf->scp.len ? conf->scp : Str("scp"));
        argv = push(&a, argv, Str("-q"));
        for (iz i = 0; i < countof(deployopts); i++) {
            argv = push(&a, argv, deployopts[i]);
        }
        argv = push(&a, argv, local);
        Str dest = concat(&a, conf->ssh, ":");
        argv = push(&a, argv, concat(&a, dest, upath));
        i64       limit = DEPLOY_UPTIME + eng.data.len/DEPLOY_UPRATE*SEC;
        DeployRun up    = deployrun(&d, &a, argv, limit);
        Str       why   = {};
        if (deploycancelled(&d)) {
            return deployfail(&d, perm, a, "cancelled");
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
            if (deploypermanent(up.err)) {
                return deployfail(&d, perm, a, why);
            }
        } else {
            deploynote(&d, a, "verify", "verifying its SHA-256 there");
            Str       vc  = deploycmd(&a, pf.shell, DEPLOY_VERIFY, rpath, stamp);
            DeployRun v   = deployssh(&d, &a, "verify", vc);
            Str       got = deployhash(&a, v.out);
            if (got == sha) {
                break;
            } else if (got.len) {
                why = concat(&a, "the upload arrived damaged, its SHA-256 ", got);
            } else if (v.code == 0) {
                b = Buf(&a, 256);
                print(&b, "cannot compute a SHA-256 on ");
                print(&b, name);
                print(&b, ": ");
                print(&b, deploysaid(&v));
                return deployfail(&d, perm, a, finish(&b));
            } else {
                why = concat(&a, "cannot verify the upload: ", deploysaid(&v));
            }
        }
        if (n >= DEPLOY_TRIES) {
            return deployfail(&d, perm, a, why);
        }
        i64 wait = deploywaits[n-1];
        b = Buf(&a, 256);
        print(&b, why);
        print(&b, ", trying again in ");
        print(&b, wait/SEC);
        print(&b, " s");
        deploynote(&d, a, "upload", finish(&b));
        if (!deploynap(&d, wait)) {
            return deployfail(&d, perm, a, "cancelled");
        }
    }

    // Into place, and run once
    deploynote(&d, scratch, "install", same ? Str("already there, running it once")
                                            : Str("renaming it into place, running it once"));
    cmd = deploycmd(&scratch, pf.shell, same ? DEPLOY_RUN : DEPLOY_INSTALL, rpath, stamp);
    DeployRun run = deployssh(&d, &scratch, "install", cmd);
    if (deploycancelled(&d)) {
        return deployfail(&d, perm, scratch, "cancelled");
    } else if (find(run.out, "harness " HARNESS_VERSION) < 0) {
        b = Buf(&scratch, 256);
        print(&b, "the engine does not run on ");
        print(&b, name);
        Str said = deploysaid(&run);
        if (said.len) {
            print(&b, ": ");
            print(&b, said);
        }
        Str why = finish(&b);
        if (!same && run.code!=255) {
            Str rm = deploycmd(&scratch, pf.shell, DEPLOY_REMOVE, rpath, stamp);
            deployssh(&d, &scratch, "install", rm);  // put there now, and useless
        }
        return deployfail(&d, perm, scratch, why);
    }

    // Recorded, for connecting
    b = Buf(&scratch, 512);
    print(&b, "host = ");
    print(&b, conf->ssh);
    print(&b, "\nplatform = ");
    print(&b, pf.name);
    print(&b, "\nshell = ");
    print(&b, pf.shell==DEPLOY_CMD ? Str("cmd") : Str("sh"));
    print(&b, "\npath = ");
    print(&b, rpath);
    print(&b, "\nsha256 = ");
    print(&b, sha);
    print(&b, "\nversion = " HARNESS_VERSION "\nproto = ");
    print(&b, (i64)HOST_PROTO);
    print(&b, "\ndeployed = ");
    print(&b, os_time(os));
    putbyte(&b, '\n');
    Str rec = deployfile(&scratch, conf->localstate, conf->ssh, ".ini");
    if (!os_write(os, scratch, rec, finish(&b))) {
        Str err = os_lasterr(os, &scratch);
        Str why = concat(&scratch, "cannot record the deployment: ", err);
        return deployfail(&d, perm, scratch, why);
    }

    Deployed r = {};
    r.ok       = 1;
    r.path     = clone(perm, rpath);
    r.shown    = clone(perm, shown);
    r.platform = clone(perm, pf.name);
    b = Buf(&scratch, 256);
    print(&b, same ? Str("already deployed: ") : Str("deployed: "));
    print(&b, shown);
    deploynote(&d, scratch, "done", finish(&b), shown, pf.name);
    return r;
}


// harness deploy

static Str deployusage =
"usage: harness deploy NAME|DEST [--force] [options]\n"
"Put an engine for the platform of a remote machine, a [remote NAME] or\n"
"an ssh destination, in ~/.harness there, over ssh and scp, where\n"
"\"harness connect\" then runs it. Prints where it went.\n"
"options:\n"
"  --force             replace another build of this version there\n"
"  --engines DIR       the engines to choose from (default: engines\n"
"                      next to this program)\n"
"  --ssh-program PROG  ssh itself (default: ssh)\n"
"  --scp-program PROG  scp itself (default: scp)\n"
"  -c FILE             configuration file\n"
"  --state DIR         session storage directory here\n";

static i32 deploymain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    DeployConf dc = {};
    Str target = {}, config = {}, state = {};
    for (i32 i = 2; i < nargs; i++) {
        Str a    = args[i];
        b32 more = i+1 < nargs;
        if (a == "--force") {
            dc.force = 1;
        } else if (a=="--engines" && more) {
            dc.engines = args[++i];
        } else if (a=="--ssh-program" && more) {
            dc.program = args[++i];
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
    if (!target.len) {
        os_print(os, 2, deployusage);
        return 2;
    }
    dc.localstate = state.len ? state : defaultstate(os, &perm);
    Config *cfg   = cliconfig(os, &perm, scratch, config, dc.localstate);
    if (!cfg) {
        return 2;
    }
    // A remote's name, or else a destination
    Str dest = cfgget(cfg, "remote", target, "ssh", 0);
    dc.ssh     = dest.len ? dest : target;
    dc.name    = target;
    dc.verbose = 1;

    Deployed r = deploy(os, &perm, scratch, &dc);
    if (!r.ok) {
        return 1;
    }
    os_print(os, 1, concat(&scratch, r.shown, "\n"));
    return 0;
}

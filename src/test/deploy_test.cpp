// Deploy tests: telling platforms and shells from probe output, reading
// hashes as each tool prints them, the commands in each shell's dialect,
// the deployment records, what ssh's end says about the engine there,
// and how the remote command's arguments go to its shell.

static Str showplatform(Arena *a, Str out)
{
    Platform p = deployplatform(a, out);
    if (!p.ok) {
        return "none";
    }
    Buf b(a, 128);
    print(&b, p.name);
    print(&b, p.shell==DEPLOY_CMD ? Str(" cmd ") : Str(" sh "));
    print(&b, p.about);
    return finish(&b);
}

static void test_deploy(Test *t)
{
    Arena a = t->perm;

    // Probes: sh prints uname then "%OS%"; cmd.exe expands it
    struct { Str out; Str want; } probes[] = {
        {"Linux aarch64\n%OS%\n", "linux-aarch64 sh Linux aarch64"},
        {"Linux x86_64\n%OS%\n", "linux-x86_64 sh Linux x86_64"},
        {"Darwin arm64\n%OS%\n", "macos-arm64 sh Darwin arm64"},
        {"Darwin x86_64\r\n%OS%\r\n", "macos-x86_64 sh Darwin x86_64"},
        // busybox-w32 sh on XP
        {"Windows_NT i686\n%OS%\n", "windows-i686 sh Windows_NT i686"},
        {"Windows_NT x86_64\n%OS%\n", "windows-x86_64 sh Windows_NT x86_64"},
        // cmd.exe, no uname (as on Windows 11), with ver after a blank line
        {"Windows_NT AMD64 %PROCESSOR_ARCHITEW6432% \r\n\r\n"
         "Microsoft Windows [Version 10.0.26200.9711]\r\n",
         "windows-x86_64 cmd Microsoft Windows [Version 10.0.26200.9711]"},
        {"Windows_NT x86 %PROCESSOR_ARCHITEW6432% \r\n\r\n"
         "Microsoft Windows XP [Version 5.1.2600]\r\n",
         "windows-i686 cmd Microsoft Windows XP [Version 5.1.2600]"},
        // a 32-bit cmd.exe on 64-bit Windows, and Windows on Arm
        {"Windows_NT x86 AMD64\r\n\r\nMicrosoft Windows [Version 6.1.7601]\r\n",
         "windows-x86_64 cmd Microsoft Windows [Version 6.1.7601]"},
        {"Windows_NT ARM64 %PROCESSOR_ARCHITEW6432%\r\n",
         "windows-x86_64 cmd Windows_NT ARM64 %PROCESSOR_ARCHITEW6432%"},
        // cmd.exe with Git's uname on its PATH
        {"MINGW64_NT-10.0-26200 x86_64\r\nWindows_NT\r\n",
         "windows-x86_64 cmd MINGW64_NT-10.0-26200 x86_64"},
        // a chatty login, and a machine without an engine anywhere
        {"Welcome to the machine\nLinux armv7l\n%OS%\n", "linux-armv7l sh Linux armv7l"},
        {"FreeBSD amd64\n%OS%\n", "freebsd-x86_64 sh FreeBSD amd64"},
        // nothing to go by
        {"%OS% %PROCESSOR_ARCHITECTURE% %PROCESSOR_ARCHITEW6432%\n", "none"},
        {"%OS%\n", "none"},
        {"", "none"},
        {"Linux\n%OS%\n", "none"},
    };
    for (iz i = 0; i < countof(probes); i++) {
        Arena tmp = a;
        Buf   name(&tmp, 32);
        print(&name, "deploy.probe.");
        print(&name, (i64)i);
        expect(t, finish(&name), showplatform(&tmp, probes[i].out), probes[i].want);
    }

    // Hashes, as each tool prints them
    Str h   = "3f2a0b1c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8";
    Str sp  = "3f 2a 0b 1c 4d 5e 6f 70 81 92 a3 b4 c5 d6 e7 f8 09 1a 2b 3c "
              "4d 5e 6f 70 81 92 a3 b4 c5 d6 e7 f8";
    struct { Str name; Str out; Str want; } hashes[] = {
        {"sha256sum", concat(&a, h, "  .prospero/engine.part\n"), h},
        {"shasum", concat(&a, h, "  .prospero/engine.part\n"), h},
        {"openssl", concat(&a, h, " *.prospero/engine.part\n"), h},
        {"escaped", concat(&a, concat(&a, "\\", h), "  .prospero\\\\x\n"), h},
        {"certutil", concat(&a, concat(&a, "SHA256 hash of .prospero\\x.part:\r\n", h),
                            "\r\nCertUtil: -hashfile command completed successfully.\r\n"), h},
        {"certutil.old", concat(&a, concat(&a, "SHA256 hash of file x:\r\n", sp),
                                "\r\nCertUtil: -hashfile command completed successfully.\r\n"), h},
        {"upper", "3F2A0B1C4D5E6F708192A3B4C5D6E7F8091A2B3C4D5E6F708192A3B4C5D6E7F8  x\n", h},
        {"after", concat(&a, "ready\nhave\n", concat(&a, h, "  x\n")), h},
        {"none", "ready\n", ""},
        {"short", "3f2a0b1c  x\n", ""},
        {"nothex", "3g2a0b1c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8  x\n", ""},
    };
    for (iz i = 0; i < countof(hashes); i++) {
        Arena tmp  = a;
        Str   name = concat(&tmp, "deploy.hash.", hashes[i].name);
        expect(t, name, deployhash(&tmp, hashes[i].out), hashes[i].want);
    }

    // Commands in each dialect
    {
        Str p  = ".prospero/engine-0-1.2.3-linux-aarch64";
        Str w  = ".prospero\\engine-0-1.2.3-windows-x86_64.exe";
        expect(t, "deploy.cmd.sh.prepare", deploycmd(&a, DEPLOY_SH, DEPLOY_PREPARE, p, 7),
               "sh -c 'mkdir -p .prospero && chmod 700 .prospero && echo ready; "
               "{ command -v sha256sum || command -v shasum || command -v openssl; } "
               ">/dev/null 2>&1 || echo nohash; "
               "if [ -f .prospero/engine-0-1.2.3-linux-aarch64 ]; then echo have; "
               "{ sha256sum .prospero/engine-0-1.2.3-linux-aarch64 || "
               "shasum -a 256 .prospero/engine-0-1.2.3-linux-aarch64 || "
               "openssl dgst -sha256 -r .prospero/engine-0-1.2.3-linux-aarch64; } "
               "2>/dev/null; fi'");
        expect(t, "deploy.cmd.sh.verify", deploycmd(&a, DEPLOY_SH, DEPLOY_VERIFY, p, 7),
               "sh -c 'chmod 755 .prospero/engine-0-1.2.3-linux-aarch64.part && "
               "{ sha256sum .prospero/engine-0-1.2.3-linux-aarch64.part || "
               "shasum -a 256 .prospero/engine-0-1.2.3-linux-aarch64.part || "
               "openssl dgst -sha256 -r .prospero/engine-0-1.2.3-linux-aarch64.part || "
               "echo nohash; } 2>/dev/null'");
        expect(t, "deploy.cmd.sh.install", deploycmd(&a, DEPLOY_SH, DEPLOY_INSTALL, p, 7),
               "sh -c '{ mv -f .prospero/engine-0-1.2.3-linux-aarch64.part "
               ".prospero/engine-0-1.2.3-linux-aarch64 2>/dev/null || "
               "{ mv -f .prospero/engine-0-1.2.3-linux-aarch64 "
               ".prospero/engine-0-1.2.3-linux-aarch64.old-7 && "
               "mv -f .prospero/engine-0-1.2.3-linux-aarch64.part "
               ".prospero/engine-0-1.2.3-linux-aarch64; }; } && "
               ".prospero/engine-0-1.2.3-linux-aarch64 --version'");
        expect(t, "deploy.cmd.sh.run", deploycmd(&a, DEPLOY_SH, DEPLOY_RUN, p, 7),
               "sh -c 'chmod 755 .prospero/engine-0-1.2.3-linux-aarch64; "
               ".prospero/engine-0-1.2.3-linux-aarch64 --version'");
        expect(t, "deploy.cmd.sh.remove", deploycmd(&a, DEPLOY_SH, DEPLOY_REMOVE, p, 7),
               "sh -c 'rm -f .prospero/engine-0-1.2.3-linux-aarch64'");
        expect(t, "deploy.cmd.cmd.prepare", deploycmd(&a, DEPLOY_CMD, DEPLOY_PREPARE, w, 7),
               "(if not exist .prospero mkdir .prospero) & (if exist .prospero echo ready) & "
               "(where certutil >nul 2>&1 || echo nohash) & "
               "(if exist .prospero\\engine-0-1.2.3-windows-x86_64.exe (echo have & "
               "certutil -hashfile .prospero\\engine-0-1.2.3-windows-x86_64.exe SHA256))");
        expect(t, "deploy.cmd.cmd.verify", deploycmd(&a, DEPLOY_CMD, DEPLOY_VERIFY, w, 7),
               "certutil -hashfile .prospero\\engine-0-1.2.3-windows-x86_64.exe.part SHA256");
        expect(t, "deploy.cmd.cmd.install", deploycmd(&a, DEPLOY_CMD, DEPLOY_INSTALL, w, 7),
               "(move /y .prospero\\engine-0-1.2.3-windows-x86_64.exe.part "
               ".prospero\\engine-0-1.2.3-windows-x86_64.exe >nul 2>&1 || "
               "(move /y .prospero\\engine-0-1.2.3-windows-x86_64.exe "
               ".prospero\\engine-0-1.2.3-windows-x86_64.exe.old-7 >nul && "
               "move /y .prospero\\engine-0-1.2.3-windows-x86_64.exe.part "
               ".prospero\\engine-0-1.2.3-windows-x86_64.exe >nul)) && "
               ".prospero\\engine-0-1.2.3-windows-x86_64.exe --version");
        expect(t, "deploy.cmd.cmd.remove", deploycmd(&a, DEPLOY_CMD, DEPLOY_REMOVE, w, 7),
               "del /f /q .prospero\\engine-0-1.2.3-windows-x86_64.exe");
    }

    // Names: a release's here, and beside each other there, where the
    // engine finds its helper by its own name
    {
        Str         v = PROSPERO_VERSION;
        DeployNames w = deploynames(&a, "windows-i686", ".exe");
        expect(t, "deploy.names.engine", w.engine,
               concat(&a, concat(&a, "prospero-engine-", v), "-windows-i686.exe"));
        expect(t, "deploy.names.desktop", w.desktop,
               concat(&a, concat(&a, "prospero-desktop-", v), "-windows-i686.exe"));
        Str re = concat(&a, concat(&a, "engine-0-", v), "-windows-i686.exe");
        Str rd = concat(&a, concat(&a, "desktop-0-", v), "-windows-i686.exe");
        expect(t, "deploy.names.rengine", w.rengine, re);
        expect(t, "deploy.names.rdesktop", w.rdesktop, rd);
        DeployNames l = deploynames(&a, "linux-x86_64", "");
        expect(t, "deploy.names.linux", l.desktop,
               concat(&a, concat(&a, "prospero-desktop-", v), "-linux-x86_64"));

        Slice<Str> h = deskhelpers(&a, "/home/u/.prospero/engine-0-1.2.3-linux-aarch64");
        expect(t, "deploy.helpers.sh", istr(&a, h.len==2 &&
               h[0]=="/home/u/.prospero/desktop-0-1.2.3-linux-aarch64" &&
               h[1]=="/home/u/.prospero/prospero-desktop"), "1");
        h = deskhelpers(&a, "/opt/prospero/prospero");
        expect(t, "deploy.helpers.build",
               istr(&a, h.len==1 && h[0]=="/opt/prospero/prospero-desktop"), "1");
    }

    // Records, by destination
    {
        Os *os = newos(&a, 1<<16);
        Str rec = "host = me@host.example\nplatform = linux-aarch64\nshell = sh\n"
                  "path = .prospero/engine-0-1-linux-aarch64\nsha256 = ab\n"
                  "version = 1\nproto = 0\ndeployed = 5\n";
        Str path = deployfile(&a, "/st", "me@host.example", ".ini");
        expect(t, "deploy.record.file", path, "/st/deploy/me@host.example.ini");
        expect(t, "deploy.record.safe", deployfile(&a, "/st", ".a/b:c d", ".ini"),
               "/st/deploy/_a_b_c_d.ini");
        os_write(os, a, path, rec);
        DeployRec r = deployrecord(os, &a, "/st", "me@host.example");
        expect(t, "deploy.record.ok", r.ok ? Str("ok") : Str("no"), "ok");
        expect(t, "deploy.record.path", r.path, ".prospero/engine-0-1-linux-aarch64");
        expect(t, "deploy.record.platform", r.platform, "linux-aarch64");
        expect(t, "deploy.record.proto", r.proto==0 ? Str("0") : Str("?"), "0");
        // Another destination that maps to the same file name
        Str file = deployfile(&a, "/st", "x:y", ".ini");
        os_write(os, a, file, "host = x_y\npath = p\n");
        r = deployrecord(os, &a, "/st", "x:y");
        expect(t, "deploy.record.other", r.ok ? Str("ok") : Str("no"), "no");
        r = deployrecord(os, &a, "/st", "nowhere");
        expect(t, "deploy.record.none", r.ok ? Str("ok") : Str("no"), "no");
    }

    // What ssh's end says about the engine there
    struct { Str log; i32 code; i32 want; } needs[] = {
        {"sh: prospero: not found\n", 127, NEED_MISSING},
        {"bash: line 1: prospero: command not found\n", 127, NEED_MISSING},
        {"'prospero' is not recognized as an internal or external command,\r\n"
         "operable program or batch file.\r\n", 1, NEED_MISSING},
        {"The system cannot find the path specified.\r\n", 1, NEED_MISSING},
        {"usage: prospero [options]\n       prospero run [options] PROMPT\n", 1, NEED_MISMATCH},
        {"usage: prospero attach [--reconnect] [options]\n", 2, NEED_MISMATCH},
        {"sh: .prospero/engine: cannot execute binary file\n", 126, NEED_MISMATCH},
        {"prospero: cannot create the state directory: denied\n", 1, NEED_NONE},
        {"ssh: connect to host x port 22: Connection refused\n", 255, NEED_NONE},
        {"prospero: cannot run ssh: No such file or directory\n", 127, NEED_NONE},
        {"", 0, NEED_NONE},
    };
    for (iz i = 0; i < countof(needs); i++) {
        Arena tmp = a;
        Buf   name(&tmp, 32);
        print(&name, "deploy.need.");
        print(&name, (i64)i);
        i32 got = remoteneed(needs[i].log, needs[i].code);
        expect(t, finish(&name), remoteneeds[got], remoteneeds[needs[i].want]);
    }

    // Arguments of the remote command: plain, else quoted for sh and
    // cmd.exe alike, a backslash and a leading - included, or refused
    struct { Str arg; Str want; } args[] = {
        {"prospero", " prospero"},
        {".prospero/engine-0-1.2.3-linux-aarch64", " .prospero/engine-0-1.2.3-linux-aarch64"},
        {"~/bin/prospero", " ~/bin/prospero"},
        {"C:/tools/prospero.exe", " C:/tools/prospero.exe"},
        {"C:\\tools\\prospero.exe", " \"C:\\tools\\prospero.exe\""},
        {".prospero\\engine-0-1.2.3-windows-x86_64.exe",
         " \".prospero\\engine-0-1.2.3-windows-x86_64.exe\""},
        {"/opt/my prospero/p", " \"/opt/my prospero/p\""},
        {"-oProxyCommand=/usr/bin/touch", " \"-oProxyCommand=/usr/bin/touch\""},
        {"", " \"\""},
        {"\\\\srv\\share\\h.exe", "refused"},
        {"C:\\dir\\", "refused"},
        {"a\"b", "refused"},
        {"$HOME/h", "refused"},
        {"%USERPROFILE%\\h", "refused"},
        {"a`b`", "refused"},
    };
    for (iz i = 0; i < countof(args); i++) {
        Arena tmp = a;
        Buf   name(&tmp, 32);
        print(&name, "deploy.remotearg.");
        print(&name, (i64)i);
        Buf b(&tmp, 64);
        Str got = remotearg(&b, args[i].arg) ? finish(&b) : Str("refused");
        expect(t, finish(&name), got, args[i].want);
    }
}

static Str joinwords(Arena *a, Slice<Str> s)
{
    Buf b(a, 256);
    for (iz i = 0; i < s.len; i++) {
        if (i) putbyte(&b, '|');
        print(&b, s[i]);
    }
    return finish(&b);
}

static Str showhosts(Arena *a, Slice<LinkTo> s)
{
    Buf b(a, 256);
    for (iz i = 0; i < s.len; i++) {
        if (i) putbyte(&b, ' ');
        print(&b, s[i].name);
        putbyte(&b, '=');
        print(&b, s[i].wsl);
    }
    return finish(&b);
}

// WSL: its command lines, the listing of distros as wsl.exe writes it,
// and the distros as remotes beside configured ones
static void test_deploywsl(Test *t)
{
    Arena a = t->perm;

    Slice<Str> w = linkargv(&a, {}, {}, "Debian", "prospero attach");
    expect(t, "deploy.wsl.argv", joinwords(&a, w),
           "wsl.exe|-d|Debian|--cd|~|--exec|/bin/sh|-c|prospero attach");
    w = linkargv(&a, "fakewsl", "host", "Deb", "x");
    expect(t, "deploy.wsl.program", joinwords(&a, w),
           "fakewsl|-d|Deb|--cd|~|--exec|/bin/sh|-c|x");
    Slice<Str> s = linkargv(&a, {}, "pi", {}, "uname");
    expect(t, "deploy.ssh.argv", joinwords(&a, s),
           "ssh|-T|-o|BatchMode=yes|-o|ConnectTimeout=15|-o|ServerAliveInterval=15|"
           "-o|ServerAliveCountMax=3|-o|EscapeChar=none|--|pi|uname");
    expect(t, "deploy.wsl.dest", linkdest(&a, "pi", {}), "pi");
    expect(t, "deploy.wsl.dest2", linkdest(&a, {}, "Debian"), "wsl:Debian");
    expect(t, "deploy.wsl.bad", linkbad({}, "-x"), "a WSL distro name cannot begin with -");
    expect(t, "deploy.wsl.good", linkbad({}, "Debian"), "");
    expect(t, "deploy.ssh.bad", linkbad("-oX", {}), "an ssh destination cannot begin with -");

    Slice<Str> env = {};
    env = push(&a, env, Str("PATH=/bin"));
    env = push(&a, env, Str("WSL_UTF8=0"));
    expect(t, "deploy.wsl.env", joinwords(&a, linkenv(&a, env, 1)), "PATH=/bin|WSL_UTF8=1");
    expect(t, "deploy.wsl.env2", joinwords(&a, linkenv(&a, env, 0)), "PATH=/bin|WSL_UTF8=0");

    // wsl.exe -l -q: UTF-16, with or without a BOM, unless WSL_UTF8
    Str utf8 = "Debian\r\ndocker-desktop\r\nUbuntu-24.04\r\ndocker-desktop-data\r\n";
    Buf u16(&a, 256);
    for (iz i = 0; i < utf8.len; i++) {
        putbyte(&u16, utf8[i]);
        putbyte(&u16, 0);
    }
    Str wide = finish(&u16);
    Str bom  = concat(&a, "\xff\xfe", wide);
    expect(t, "deploy.wsl.list8", joinwords(&a, wsldistros(&a, utf8)), "Debian|Ubuntu-24.04");
    expect(t, "deploy.wsl.list16", joinwords(&a, wsldistros(&a, wide)), "Debian|Ubuntu-24.04");
    expect(t, "deploy.wsl.listbom", joinwords(&a, wsldistros(&a, bom)), "Debian|Ubuntu-24.04");
    expect(t, "deploy.wsl.listbom8",
           joinwords(&a, wsldistros(&a, "\xef\xbb\xbf" "Debian\n")), "Debian");
    expect(t, "deploy.wsl.listodd",
           joinwords(&a, wsldistros(&a, "Arch Linux\nok.1\n\n")), "ok.1");
    expect(t, "deploy.wsl.listnone", joinwords(&a, wsldistros(&a, "")), "");
    // a character beyond ASCII in UTF-16: not a plain name
    Buf na(&a, 16);
    u8  units[] = {'D', 0, 0xe9, 1, 'b', 0, '\n', 0, 'X', 0};
    for (iz i = 0; i < countof(units); i++) {
        putbyte(&na, units[i]);
    }
    Str nonascii = finish(&na);
    expect(t, "deploy.wsl.listwide", joinwords(&a, wsldistros(&a, nonascii)), "X");

    // Distros as remotes: claimed by a section, renamed beside another
    Slice<Str> distros = {};
    distros = push(&a, distros, Str("Debian"));
    distros = push(&a, distros, Str("Ubuntu"));
    distros = push(&a, distros, Str("Arch"));
    distros = push(&a, distros, Str("Fedora"));
    Slice<LinkTo> remotes = {};
    remotes = push(&a, remotes, LinkTo{"deb", {}, "Debian"});      // claims Debian
    remotes = push(&a, remotes, LinkTo{"Ubuntu", "ubuntu.lan", {}});  // the name, by ssh
    remotes = push(&a, remotes, LinkTo{"Arch", {}, {}});          // its settings only
    remotes = push(&a, remotes, LinkTo{"x", "Fedora", {}});        // ssh, not a claim
    expect(t, "deploy.wsl.hosts", showhosts(&a, wslhosts(&a, distros, remotes)),
           "Ubuntu-wsl=Ubuntu Arch=Arch Fedora=Fedora");
    expect(t, "deploy.wsl.hostsnone", showhosts(&a, wslhosts(&a, distros, {})),
           "Debian=Debian Ubuntu=Ubuntu Arch=Arch Fedora=Fedora");

    // A Windows path typed for a WSL distro
    expect(t, "wsl.drive", wslpath(&a, "C:\\Users\\me"), "/mnt/c/Users/me");
    expect(t, "wsl.slash", wslpath(&a, "d:/src"), "/mnt/d/src");
    expect(t, "wsl.bare", wslpath(&a, "C:"), "/mnt/c");
    expect(t, "wsl.root", wslpath(&a, "C:\\"), "/mnt/c/");
    expect(t, "wsl.relative", wslpath(&a, "C:x"), "C:x");
    expect(t, "wsl.typing", wslpath(&a, "/mnt/c/Users\\"), "/mnt/c/Users/");
    expect(t, "wsl.mnt", wslpath(&a, "/mnt/c\\x"), "/mnt/c/x");
    expect(t, "wsl.mntonly", wslpath(&a, "/mnt/c"), "/mnt/c");
    expect(t, "wsl.notdrive", wslpath(&a, "/mnt/cd\\x"), "/mnt/cd\\x");
    expect(t, "wsl.linux", wslpath(&a, "/home/u\\x"), "/home/u\\x");
    expect(t, "wsl.home", wslpath(&a, "~/src"), "~/src");

    // And one into the distro's own files
    expect(t, "wsl.unc", wsluncpath(&a, "\\\\wsl.localhost\\Deb\\home\\u", "Deb"), "/home/u");
    expect(t, "wsl.uncold", wsluncpath(&a, "\\\\wsl$\\Deb\\x", "Deb"), "/x");
    expect(t, "wsl.uncslash", wsluncpath(&a, "//WSL.LocalHost/deb/x/y", "Deb"), "/x/y");
    expect(t, "wsl.uncroot", wsluncpath(&a, "\\\\wsl.localhost\\Deb", "Deb"), "/");
    expect(t, "wsl.uncroot2", wsluncpath(&a, "\\\\wsl.localhost\\Deb\\", "Deb"), "/");
    expect(t, "wsl.uncother", wsluncpath(&a, "\\\\wsl.localhost\\Arch\\x", "Deb"), "");
    expect(t, "wsl.uncprefix", wsluncpath(&a, "\\\\wsl.localhost\\Debian\\x", "Deb"), "");
    expect(t, "wsl.uncserver", wsluncpath(&a, "\\\\server\\Deb\\x", "Deb"), "");
    expect(t, "wsl.uncdrive", wsluncpath(&a, "C:\\Users", "Deb"), "");
    expect(t, "wsl.uncnone", wsluncpath(&a, "\\\\wsl.localhost\\x", {}), "");
}

// wsl.exe's own messages: UTF-16 from an inbox wsl.exe, whatever
// WSL_UTF8 says, and the line worth showing
static void test_deploywslsaid(Test *t)
{
    Arena a   = t->perm;
    Str   msg = "The Windows Subsystem for Linux is not installed.\r\n"
                "For more information please visit https://aka.ms/wslinstall\r\n";
    Buf   b(&a, 512);
    for (iz i = 0; i < msg.len; i++) {
        putbyte(&b, msg[i]);
        putbyte(&b, 0);
    }
    Str wide = finish(&b);
    expect(t, "deploy.wslutf8", wslutf8(&a, wide), msg);
    expect(t, "deploy.wslutf8.same", wslutf8(&a, msg), msg);
    Buf bom(&a, 4);
    putbyte(&bom, 0xff);
    putbyte(&bom, 0xfe);
    putbyte(&bom, 'A');
    putbyte(&bom, 0);
    expect(t, "deploy.wslutf8.bom", wslutf8(&a, finish(&bom)), "A");
    expect(t, "deploy.wslsaid", lastsaid(wslutf8(&a, wide)),
           "The Windows Subsystem for Linux is not installed.");
    DeployRun r = {};
    r.err = msg;
    expect(t, "deploy.wslpermanent", deploypermanent(&r) ? Str("yes") : Str("no"), "yes");
}

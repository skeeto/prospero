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
        os_write(os, a, deployfile(&a, "/st", "x:y", ".ini"), "host = x_y\npath = p\n");
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

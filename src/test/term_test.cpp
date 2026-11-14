// Terminal tests (ptyhost.cpp): the flow control's arithmetic, and the
// shell and environment that term.open gives a terminal. The terminals
// themselves are the term_* scenarios'.

// Words joined by '|', to compare an argv or an environment whole.
static Str termwords(Arena *a, Slice<Str> s)
{
    Buf b(a, 256);
    for (iz i = 0; i < s.len; i++) {
        if (i) putbyte(&b, '|');
        print(&b, s[i]);
    }
    return finish(&b);
}

static Slice<Str> termlist(Arena *a, Str words)
{
    Slice<Str> r = {};
    Cut c = {};
    c.tail = words;
    c.ok   = words.len > 0;
    while (c.ok) {
        c = cut(c.tail, '|');
        r = push(a, r, c.head);
    }
    return r;
}

static void test_term(Test *t)
{
    Arena *a = &t->perm;

    // How much the next record may hold: a line, within the window
    {
        static struct { i64 sent, acked; iz want; } cases[] = {
            {0,      0,      16},
            {10,     0,      16},
            {50,     0,      14},
            {64,     0,      0},
            {64,     10,     10},
            {100,    90,     16},
            {70,     0,      0},   // past the window, as after a resend
        };
        Buf b(a, 256);
        for (iz i = 0; i < countof(cases); i++) {
            if (i) putbyte(&b, ' ');
            print(&b, (i64)termcan(cases[i].sent, cases[i].acked, 64, 16));
        }
        Buf w(a, 256);
        for (iz i = 0; i < countof(cases); i++) {
            if (i) putbyte(&w, ' ');
            print(&w, (i64)cases[i].want);
        }
        expect(t, "termcan", finish(&b), finish(&w));
    }

    // Acks: within what was sent, never backward; resend rewinds
    {
        static struct { i64 sent, acked, off; b32 resend; Str want; } cases[] = {
            {100, 0,  100, 0, "ok take 100 sent 100"},
            {100, 0,  40,  0, "ok take 40 sent 100"},
            {100, 40, 40,  0, "ok take 0 sent 100"},
            {100, 40, 60,  1, "ok take 20 sent 60"},
            {100, 40, 40,  1, "ok take 0 sent 40"},
            {100, 40, 30,  0, "no"},   // backward
            {100, 40, 101, 0, "no"},   // past what was sent
            {100, 40, -1,  0, "no"},   // none given
            {0,   0,  0,   0, "ok take 0 sent 0"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            TermAck k = termack(cases[i].sent, cases[i].acked, cases[i].off, cases[i].resend);
            Buf     b(a, 64);
            if (!k.ok) {
                print(&b, "no");
            } else {
                print(&b, "ok take ");
                print(&b, k.take);
                print(&b, " sent ");
                print(&b, k.sent);
            }
            expect(t, concat(a, "termack ", istr(a, i)), finish(&b), cases[i].want);
        }
    }

    // [terminal] shell's words, with quotes for a word with blanks
    {
        static struct { Str in, want; } cases[] = {
            {"", ""},
            {"   ", ""},
            {"zsh", "zsh"},
            {"  /bin/zsh   -l  ", "/bin/zsh|-l"},
            {"\"/Applications/My Shell/fish\" -l", "/Applications/My Shell/fish|-l"},
            {"a\"b c", "a\"b|c"},          // a quote inside a word is a quote
            {"x \"\" y", "x||y"},          // an empty word, quoted
            {"tmux \"new -A\"", "tmux|new -A"},
            {"~/bin/fish -C ~/x.fish", "/h/bin/fish|-C|/h/x.fish"},
            {"~ ~user a~/b", "/h|~user|a~/b"},
            {"\"~/my shell\" -l", "~/my shell|-l"},  // quoted, as written
        };
        for (iz i = 0; i < countof(cases); i++) {
            expect(t, concat(a, "termsplit ", cases[i].in),
                   termwords(a, termsplit(a, cases[i].in, "/h", 0)), cases[i].want);
        }
        expect(t, "termsplit open", istr(a, termsplit(a, "sh \"-c", "/h", 0).len), "0");
        expect(t, "termsplit win", termwords(a, termsplit(a, "~\\bin\\nu.exe", "C:/Users/u", 1)),
               "C:/Users/u\\bin\\nu.exe");
    }

    // The shell: configured, else the session's unless the system's
    // default, else $SHELL, else that default, else /bin/sh
    {
        static struct {
            Str conf, kind, path, envshell;
            b32 win, login;
            Str want;
        } cases[] = {
            {"fish -i", "sh", "/bin/zsh", "/bin/bash", 0, 1, "fish|-i"},
            {"", "sh", "/bin/zsh", "/bin/bash", 0, 1, "/bin/zsh|-l"},
            {"", "sh", "/bin/zsh", "/bin/bash", 0, 0, "/bin/zsh"},
            {"", "sh", "/bin/bash", "/usr/bin/fish", 0, 1, "/usr/bin/fish|-l"},
            {"", "sh", "/bin/sh", "/bin/zsh", 0, 0, "/bin/zsh"},
            {"", "sh", "/bin/bash", "", 0, 1, "/bin/bash|-l"},
            {"", "sh", "", "", 0, 1, "/bin/sh|-l"},
            {"", "busybox", "/opt/busybox", "", 0, 0, "/opt/busybox|sh|-l"},
            {"", "pwsh", "C:/pwsh/pwsh.exe", "", 1, 1, "C:/pwsh/pwsh.exe|-NoLogo"},
            {"", "cmd", "C:/Windows/system32/cmd.exe", "", 1, 1,
             "C:/Windows/system32/cmd.exe"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Slice<Str> argv = termargv(a, cases[i].conf, "/h", cases[i].kind, cases[i].path,
                                       {}, cases[i].envshell, cases[i].win,
                                       cases[i].login);
            expect(t, concat(a, "termargv ", istr(a, i)), termwords(a, argv), cases[i].want);
        }
        Slice<Str> wsl = termargv(a, {}, "/h", "wsl", "C:/Windows/system32/wsl.exe",
                                  "Ubuntu", {}, 1, 1);
        expect(t, "termargv wsl", termwords(a, wsl),
               "C:/Windows/system32/wsl.exe|-d|Ubuntu");
    }

    // The environment: other terminals' variables out, this one's in,
    // and a UTF-8 locale on macOS where none is set
    {
        Str in = "HOME=/h|COLUMNS=80|LINES=24|TERM=dumb|TERM_PROGRAM=iTerm.app|"
                 "ITERM_SESSION_ID=w0|KITTY_WINDOW_ID=1|TMUX=/tmp/t|"
                 "TERMINFO=/x|PATH=/bin|LC_TERMINAL=iTerm2|TERM_SESSION_ID=s";
        Str want = "HOME=/h|TERMINFO=/x|PATH=/bin|TERM=xterm-256color|"
                   "COLORTERM=truecolor|TERM_PROGRAM=Prospero|"
                   "TERM_PROGRAM_VERSION=" PROSPERO_VERSION;
        Slice<Str> env = termenv(a, termlist(a, in), 0, 0, "xterm-256color");
        expect(t, "termenv", termwords(a, env), want);
        env = termenv(a, termlist(a, in), 0, 1, "xterm");
        expect(t, "termenv mac", termwords(a, env),
               concat(a, "HOME=/h|TERMINFO=/x|PATH=/bin|TERM=xterm|"
                      "COLORTERM=truecolor|TERM_PROGRAM=Prospero|"
                      "TERM_PROGRAM_VERSION=" PROSPERO_VERSION, "|LANG=en_US.UTF-8"));
        env = termenv(a, termlist(a, "LC_CTYPE=UTF-8|HOME=/h"), 0, 1, "xterm");
        expect(t, "termenv mac locale", envget(env, "LANG", 0), {});
        expect(t, "termenv mac ctype", envget(env, "LC_CTYPE", 0), "UTF-8");
        // Names in any case on Windows: Term is TERM
        env = termenv(a, termlist(a, "Path=C:/w|WT_SESSION=1|Term=x"), 1, 0, "xterm-256color");
        expect(t, "termenv win", termwords(a, env),
               "Path=C:/w|TERM=xterm-256color|COLORTERM=truecolor|"
               "TERM_PROGRAM=Prospero|TERM_PROGRAM_VERSION=" PROSPERO_VERSION);
    }

    // term.open's edit: checked whole, clamped, or refused with a why
    {
        static struct { Str json; b32 win; Str want; } cases[] = {
            {"{}", 0, "none"},
            {"{\"edit\":null}", 0, "why edit must be an object"},
            {"{\"edit\":\"x\"}", 0, "why edit must be an object"},
            {"{\"edit\":[1]}", 0, "why edit must be an object"},
            {"{\"edit\":{}}", 0, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":3}}", 0, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"a/b\"}}", 0, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"/a\\u0000b\"}}", 0,
             "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"/a b\"}}", 0, "path /a b line 0 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"C:/a\"}}", 0, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"C:/a\"}}", 1, "path C:/a line 0 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"\\\\\\\\h\\\\s\\\\a\"}}", 1,
             "path \\\\h\\s\\a line 0 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\"}}", 1, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"\\\\a\"}}", 1, "why edit.path must be an absolute path"},
            {"{\"edit\":{\"path\":\"C:a\"}}", 1, "why edit.path must be an absolute path"},
            // cmd.exe's refusals (C10), on Windows alone
            {"{\"edit\":{\"path\":\"C:/100%\"}}", 1,
             "why edit.path holds %, \" or a control character, which cmd.exe cannot pass on"},
            {"{\"edit\":{\"path\":\"C:/a\\\"b\"}}", 1,
             "why edit.path holds %, \" or a control character, which cmd.exe cannot pass on"},
            {"{\"edit\":{\"path\":\"C:/a\\nb\"}}", 1,
             "why edit.path holds %, \" or a control character, which cmd.exe cannot pass on"},
            // ...nor what an ANSI editor may get best-fit as " or \ (WorstFit)
            {"{\"edit\":{\"path\":\"C:/a\\uff02 +!calc \\uff02\"}}", 1,
             "why edit.path holds a character Windows may pass to the editor as \" or \\"},
            {"{\"edit\":{\"path\":\"C:/a\\uff3c\"}}", 1,
             "why edit.path holds a character Windows may pass to the editor as \" or \\"},
            {"{\"edit\":{\"path\":\"C:/a\\u02bab\"}}", 1,
             "why edit.path holds a character Windows may pass to the editor as \" or \\"},
            {"{\"edit\":{\"path\":\"C:/\\uff21\\uff22\"}}", 1,
             "path C:/\xef\xbc\xa1\xef\xbc\xa2 line 0 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\\uff02\"}}", 0,
             "path /a\xef\xbc\x82 line 0 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/100%\\\"\\n\"}}", 0,
             "path /100%\"\n line 0 col 0 editor  tmpl "},
            // Numbers clamped
            {"{\"edit\":{\"path\":\"/a\",\"line\":12,\"col\":3}}", 0,
             "path /a line 12 col 3 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"line\":0,\"col\":-4}}", 0,
             "path /a line 1 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"line\":-7}}", 0, "path /a line 1 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"line\":1e30,\"col\":5000000000}}", 0,
             "path /a line 1000000000 col 1000000000 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"line\":2.5}}", 0, "path /a line 2 col 0 editor  tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"line\":\"12\"}}", 0, "why edit.line must be a number"},
            {"{\"edit\":{\"path\":\"/a\",\"col\":true}}", 0, "why edit.col must be a number"},
            // An editor or a template, not both
            {"{\"edit\":{\"path\":\"/a\",\"editor\":\" vim -p \"}}", 0,
             "path /a line 0 col 0 editor vim -p tmpl "},
            {"{\"edit\":{\"path\":\"/a\",\"template\":\"vi +{line} {file}\"}}", 0,
             "path /a line 0 col 0 editor  tmpl vi +{line} {file}"},
            {"{\"edit\":{\"path\":\"/a\",\"editor\":\"vi\",\"template\":\"vi {file}\"}}", 0,
             "why edit: give editor or template, not both"},
            {"{\"edit\":{\"path\":\"/a\",\"editor\":\"\",\"template\":\"vi {file}\"}}", 0,
             "path /a line 0 col 0 editor  tmpl vi {file}"},
            {"{\"edit\":{\"path\":\"/a\",\"editor\":7}}", 0, "why edit.editor must be a string"},
            {"{\"edit\":{\"path\":\"/a\",\"template\":[]}}", 0,
             "why edit.template must be a string"},
            {"{\"edit\":{\"path\":\"/a\",\"editor\":\"v\\u0000i\"}}", 0,
             "why edit.editor must be under 4 KiB, with no NUL"},
            {"{\"edit\":{\"path\":\"/a\",\"template\":\"v\\u0000i {file}\"}}", 0,
             "why edit.template must be under 4 KiB, with no NUL"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena       s = *a;
            JsonDoc     d = jsonparse(&s, cases[i].json);
            TermEditAsk r = termeditask(d.root, cases[i].win);
            Buf         b(&s, 256);
            if (r.why.len) {
                print(&b, "why ");
                print(&b, r.why);
            } else if (!r.on) {
                print(&b, "none");
            } else {
                print(&b, "path ");
                print(&b, r.path);
                print(&b, " line ");
                print(&b, (i64)r.line);
                print(&b, " col ");
                print(&b, (i64)r.col);
                print(&b, " editor ");
                print(&b, r.editor);
                print(&b, " tmpl ");
                print(&b, r.tmpl);
            }
            expect(t, concat(a, "termeditask ", istr(a, i)), finish(&b), cases[i].want);
        }

        // Lengths: a path under 32 KiB, an editor or template under 4 KiB
        Arena s    = *a;
        Buf   path(&s, 40000);
        print(&path, "{\"edit\":{\"path\":\"/");
        for (iz i = 0; i < TERM_EDITPATH; i++) putbyte(&path, 'a');
        print(&path, "\"}}");
        JsonDoc d = jsonparse(&s, finish(&path));
        expect(t, "termeditask long path", termeditask(d.root, 0).why, "edit.path is too long");
        Buf ed(&s, 8000);
        print(&ed, "{\"edit\":{\"path\":\"/a\",\"editor\":\"");
        for (iz i = 0; i < TERM_EDITCMD; i++) putbyte(&ed, 'v');
        print(&ed, "\"}}");
        d = jsonparse(&s, finish(&ed));
        expect(t, "termeditask long editor", termeditask(d.root, 0).why,
               "edit.editor must be under 4 KiB, with no NUL");
        Buf tm(&s, 8000);
        print(&tm, "{\"edit\":{\"path\":\"/a\",\"template\":\"");
        for (iz i = 0; i < TERM_EDITCMD; i++) putbyte(&tm, 'v');
        print(&tm, "\"}}");
        d = jsonparse(&s, finish(&tm));
        expect(t, "termeditask long template", termeditask(d.root, 0).why,
               "edit.template must be under 4 KiB, with no NUL");
        Buf wp(&s, 40000);
        print(&wp, "{\"edit\":{\"path\":\"C:/");
        for (iz i = 0; i < TERM_EDITPATH; i++) putbyte(&wp, 'a');
        print(&wp, "\"}}");
        d = jsonparse(&s, finish(&wp));
        expect(t, "termeditask long path win", termeditask(d.root, 1).why,
               "edit.path is too long");
    }

    // The reply's name of a Windows editor
    expect(t, "termeditname", termeditname("C:/bin/Vim.BAT"), "Vim");
    expect(t, "termeditname exe", termeditname("C:\\nvim\\nvim.exe"), "nvim");
    expect(t, "termeditname bare", termeditname("vi"), "vi");
}

// A Run's classification as "KIND|TEXT" or "KIND why WHY", N for none.
static Str termrunshow(Arena *a, i32 kind, Str text, Str why)
{
    static char const names[] = "NSPC";
    Buf b(a, 64 + text.len + why.len);
    putbyte(&b, (u8)names[kind]);
    if (why.len) {
        print(&b, " why ");
        print(&b, why);
    } else if (kind) {
        putbyte(&b, '|');
        print(&b, text);
    }
    return finish(&b);
}

static void test_termrun(Test *t)
{
    Arena *a = &t->perm;

    {
        struct {
            Str lang;
            i32 want;
        } cases[] = {
            {"sh", RUN_SH}, {"Bash", RUN_SH}, {" zsh ", RUN_SH}, {"ksh", RUN_SH},
            {"shell", RUN_SH}, {"PowerShell", RUN_PS}, {"pwsh", RUN_PS}, {"ps1", RUN_PS},
            {"cmd", RUN_CMD}, {"BAT", RUN_CMD}, {"batch", RUN_CMD}, {"console", RUN_NONE},
            {"shellsession", RUN_NONE}, {"fish", RUN_NONE}, {"", RUN_NONE},
            {"python", RUN_NONE}, {"bashx", RUN_NONE},
        };
        for (iz i = 0; i < countof(cases); i++) {
            expect(t, concat(a, "termrunlang ", cases[i].lang),
                   istr(a, termrunlang(cases[i].lang)), istr(a, cases[i].want));
        }
    }

    {
        struct {
            Str lang;
            Str code;
            Str want;
        } cases[] = {
            {"sh", "make test\n", "S|make test"},
            {"bash", "\n\nmake\r\nmake install\r\n\n", "S|make\nmake install"},
            {"sh", "  indented\n", "S|  indented"},
            {"sh", "$ make\n$ make install\n", "S|make\nmake install"},
            {"sh", "$ make\nok\n", "N"},                        // a transcript
            {"sh", "$ docker run \\\n    -it img\n", "S|docker run \\\n    -it img"},
            {"sh", "docker run \\\n$ x\n", "S|docker run \\\n$ x"},  // continued, no prompt
            {"sh", "", "N"},
            {"sh", "\n  \n", "N"},
            {"text", "make", "N"},
            {"console", "$ make", "N"},
            {"powershell", "PS> Get-Item .\nPS C:\\src> dir\n", "P|Get-Item .\ndir"},
            {"pwsh", "Get-ChildItem `\n  -Recurse\n", "P|Get-ChildItem `\n  -Recurse"},
            {"pwsh", "PS> ls\noutput\n", "N"},
            {"cmd", "dir /b\n", "C|dir /b"},
            {"cmd", "dir\ncd ..\n", "C why cmd.exe runs one line, and this is more"},
            {"sh", "echo \x1b[31m", "S why the command holds a control or invisible "
                                    "character, so what runs would not be what shows"},
            {"sh", "rm -rf \xe2\x80\xae/", "S why the command holds a control or invisible "
                                          "character, so what runs would not be what shows"},
            {"sh", "a\xe2\x80\x8b" "b", "S why the command holds a control or invisible "
                                        "character, so what runs would not be what shows"},
            {"sh", "echo \xc2\x85", "S why the command holds a control or invisible "
                                    "character, so what runs would not be what shows"},
            {"sh", "echo \xff", "S why the command is not UTF-8"},
            {"sh", "echo a\rb", "S why the command holds a control or invisible "
                                "character, so what runs would not be what shows"},
            {"sh", "echo caf\xc3\xa9\tok", "S|echo caf\xc3\xa9\tok"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena        s = *a;
            TermRunBlock r = termrunblock(&s, cases[i].lang, cases[i].code);
            expect(t, concat(&s, "termrunblock ", istr(&s, i)),
                   termrunshow(&s, r.kind, r.text, r.why), cases[i].want);
        }

        // Lengths: 16 KiB of text, and cmd's 8191 units
        Arena s = *a;
        Buf   big(&s, TERM_RUNTEXT + 8);
        for (iz i = 0; i < TERM_RUNTEXT; i++) putbyte(&big, 'a');
        Str   most = finish(&big);
        expect(t, "termrunblock 16k", istr(&s, termrunblock(&s, "sh", most).text.len),
               istr(&s, TERM_RUNTEXT));
        Str over = concat(&s, most, "a");
        expect(t, "termrunblock over", termrunblock(&s, "sh", over).why,
               "the command is over 16 KiB");
        Str cmd = takehead(most, TERM_RUNCMD);
        expect(t, "termrunblock cmd most", termrunblock(&s, "cmd", cmd).why, {});
        expect(t, "termrunblock cmd over", termrunblock(&s, "cmd", concat(&s, cmd, "a")).why,
               "the command is longer than cmd.exe takes (8191 characters)");
    }

    {
        struct {
            Str json;
            b32 win;
            Str want;
        } cases[] = {
            {"{}", 0, "none"},
            {"{\"run\":1}", 0, "why run must be an object"},
            {"{\"run\":{\"text\":\"ls\",\"lang\":\"sh\"},\"edit\":{\"path\":\"/a\"}}", 0,
             "why term.open takes run or edit, not both"},
            {"{\"run\":{\"text\":\"ls\"}}", 0, "why run.text and run.lang must be strings"},
            {"{\"run\":{\"text\":\"ls\",\"lang\":\"sh\",\"from\":\"../x\"}}", 0,
             "why run.from must be a session id"},
            {"{\"run\":{\"text\":\"ls\",\"lang\":\"python\"}}", 0,
             "why run.lang names no shell this runs: sh, bash, zsh, ksh, powershell, "
             "pwsh or cmd"},
            {"{\"run\":{\"text\":\"dir\",\"lang\":\"cmd\"}}", 0, "why run: cmd runs only on Windows"},
            {"{\"run\":{\"text\":\"dir\",\"lang\":\"cmd\"}}", 1, "C|dir from "},
            {"{\"run\":{\"text\":\"\\n\",\"lang\":\"sh\"}}", 0, "why run: the command is empty"},
            {"{\"run\":{\"text\":\"a\\u0000b\",\"lang\":\"sh\"}}", 0,
             "why run: the command holds a control or invisible character, so what runs "
             "would not be what shows"},
            {"{\"run\":{\"text\":\"make\\r\\n\",\"lang\":\"bash\",\"from\":\"s-1\"}}", 0,
             "S|make from s-1"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena      s = *a;
            JsonDoc    d = jsonparse(&s, cases[i].json);
            TermRunAsk r = termrunask(&s, d.root, cases[i].win);
            Str        got = !r.on ? Str("none")
                           : r.why.len ? concat(&s, "why ", r.why)
                           : concat(&s, concat(&s, termrunshow(&s, r.kind, r.text, {}), " from "),
                                    r.from);
            expect(t, concat(&s, "termrunask ", istr(&s, i)), got, cases[i].want);
        }
    }

    i32 n = 0;
    expect(t, "termrunfirst", termrunfirst("make\nmake install", &n), "make");
    expect(t, "termrunfirst lines", istr(a, n), "2");
    expect(t, "termrunfirst one", termrunfirst("ls", &n), "ls");
    expect(t, "termrunfirst one lines", istr(a, n), "1");
}

// Lines committed from typed input, as KIND:TEXT joined by '|', K for a
// candidate and S for a secret.
static Str termcommits(Arena *a, TermLine *l, Str in, i32 kind)
{
    Slice<TermCommit> m = termlinefeed(a, l, in, kind);
    Buf b(a, 256);
    for (iz i = 0; i < m.len; i++) {
        if (i) putbyte(&b, '|');
        putbyte(&b, m[i].kind==TL_SECRET ? 'S' : 'K');
        putbyte(&b, ':');
        print(&b, m[i].text);
    }
    return finish(&b);
}

// A session table for termrootwalk: "sid parent sideof worktree cwd"
// rows, '-' for empty.
static TermNode termlookrows(void *ctx, Arena *a, Str sid)
{
    Str     *rows = (Str *)ctx;
    TermNode n    = {};
    for (iz i = 0; rows[i].len; i++) {
        Slice<Str> w = termsplit(a, rows[i], {}, 0);
        if (w.len==5 && w[0]==sid) {
            n.found    = 1;
            n.parent   = w[1]==Str("-") ? Str{} : w[1];
            n.sideof   = w[2]==Str("-") ? Str{} : w[2];
            n.worktree = w[3]==Str("1");
            n.cwd      = w[4];
        }
    }
    return n;
}

static void test_termsecret(Test *t)
{
    Arena *a = &t->perm;

    // Typed lines, their editing, and the sequences that are not text
    {
        static struct { Str in; i32 kind; Str want; } cases[] = {
            {"hunter2\r",                  TL_SECRET, "S:hunter2"},
            {"hunter2\n",                  TL_CAND,   "K:hunter2"},
            {"hunter2\r",                  TL_SKIP,   ""},
            {"hux\x7fnter2\r",             TL_SECRET, "S:hunter2"},
            {"wrong\x15hunter2\r",         TL_SECRET, "S:hunter2"},
            {"a b\x17hunter2\r",           TL_SECRET, "S:a hunter2"},
            {"ab cd  \x17\x17x\r",         TL_SECRET, "S:x"},
            {"h\xc3\xa9\x7f" "e\r",        TL_SECRET, "S:he"},
            {"one\rtwo\r",                 TL_SECRET, "S:one|S:two"},
            {"abc\x03hunter2\r",           TL_SECRET, "S:hunter2"},
            {"abc\x1b",                    TL_SECRET, ""},
            {"\x1b[Ihunter\x1b[O" "2\r",   TL_SECRET, "S:hunter2"},  // focus
            {"hun\x1b[<0;3;4Mter2\r",      TL_SECRET, "S:hunter2"},  // mouse
            {"hun\x1b]11;rgb:0/0/0\x1b\\ter2\r", TL_SECRET, "S:hunter2"},
            {"hun\x1b]10;x\x07ter2\r",     TL_SECRET, "S:hunter2"},
            {"hun\x1bP1$r0m\x1b\\ter2\r",  TL_SECRET, "S:hunter2"},
            {"\x1b[200~hun\x15ter2\x1b[201~\r", TL_SECRET, "S:hunter2"},
            {"hunter\x1b[D2\r",            TL_SECRET, ""},   // moved within
            {"\x1b[Ahunter2\r",            TL_SECRET, "S:hunter2"},
            {"hun\tter\r",                 TL_CAND,   ""},   // completion
            {"hun\x1b" "bter\r",           TL_CAND,   "K:ter"},  // Alt+b
            {"\r\r",                       TL_SECRET, ""},
        };
        for (iz i = 0; i < countof(cases); i++) {
            TermLine l = {};
            Arena    s = *a;
            Buf name(&s, 64);
            print(&name, "termlinefeed ");
            print(&name, (i64)i);
            expect(t, finish(&name), termcommits(&s, &l, cases[i].in, cases[i].kind),
                   cases[i].want);
        }
    }

    // A kind latches at a line's first byte, across chunks; a long line
    // is not kept; a sequence may span chunks
    {
        TermLine l = {};
        Arena    s = *a;
        expect(t, "termlinefeed latch", termcommits(&s, &l, "hun", TL_CAND), "");
        expect(t, "termlinefeed latch 2", termcommits(&s, &l, "ter2\rpass\r", TL_SECRET),
               "K:hunter2|S:pass");
        Buf b(&s, 512);
        for (i32 i = 0; i < TERMLINE_CAP+1; i++) putbyte(&b, 'x');
        putbyte(&b, '\r');
        expect(t, "termlinefeed long", termcommits(&s, &l, finish(&b), TL_SECRET), "");
        expect(t, "termlinefeed after long", termcommits(&s, &l, "ok12\r", TL_SECRET),
               "S:ok12");
        expect(t, "termlinefeed split", termcommits(&s, &l, "ab\x1b[<0;1", TL_SECRET), "");
        expect(t, "termlinefeed split 2", termcommits(&s, &l, ";1Mcd\r", TL_SECRET),
               "S:abcd");
    }

    // Password prompts, by the text before the cursor
    {
        static struct { Str row; b32 want; } cases[] = {
            {"Password:",                                 1},
            {"[sudo] password for chris: ",               1},
            {"Enter passphrase for key '/h/.ssh/id':  ",  1},
            {"Enter PIN:",                                1},
            {"Pass phrase:",                              1},
            {"API token:",                                1},
            {"Client secret:",                            1},
            {"New passwd:",                               1},
            {"Password",                                  0},
            {"Password: x",                               0},
            {"spinner:",                                  0},
            {"tokenizer:",                                0},
            {"passwords.txt:",                            0},
            {"$ ",                                        0},
            {"",                                          0},
        };
        Buf got(a, 256), want(a, 256);
        for (iz i = 0; i < countof(cases); i++) {
            putbyte(&got, termprompt(cases[i].row) ? '1' : '0');
            putbyte(&want, cases[i].want ? '1' : '0');
        }
        expect(t, "termprompt", finish(&got), finish(&want));
    }

    // Masking: long secrets anywhere, short ones as whole words, the
    // longest where two begin at once
    {
        TermSecrets ss = {};
        expect(t, "termsecretadd short", termsecretadd(&ss, "abc", 2) ? Str("kept") : Str("no"), "no");
        termsecretadd(&ss, "hunter2", 2);
        termsecretadd(&ss, "pass", 1);
        termsecretadd(&ss, "hunter22", 1);
        Arena s = *a;
        iz    n = 0;
        Str   in = "x hunter2y passport pass hunter22 (pass)";
        expect(t, "termmask", termmask(&s, in, &ss, &n),
               "x [hidden]y passport [hidden] [hidden] ([hidden])");
        expect(t, "termmask count", n==4 ? Str("4") : Str("not 4"), "4");
        Str plain = "nothing here";
        Str r     = termmask(&s, plain, &ss, &n);
        expect(t, "termmask none", r.data==plain.data && !n ? Str("same") : Str("copied"), "same");
        expect(t, "termmask edges", termmask(&s, "pass", &ss, &n), "[hidden]");
        expect(t, "termmask unicode", termmask(&s, "\xc3\xa9pass", &ss, &n), "\xc3\xa9pass");
    }

    // The secret set: deduplicated, the lowest rank evicted first, and
    // zeroed
    {
        TermSecrets ss = {};
        termsecretadd(&ss, "keepme-2", 2);
        for (i32 i = 0; i < TERMSECRETS; i++) {
            u8 v[8] = {'s', 'e', 'c', 'r', 'e', 't', (u8)('a'+i/26), (u8)('a'+i%26)};
            termsecretadd(&ss, Str{v, 8}, 1);
        }
        termsecretadd(&ss, "keepme-2", 1);
        i32 n = 0, keep = 0;
        for (i32 i = 0; i < TERMSECRETS; i++) {
            n    += ss.s[i].len > 0;
            keep += Str{ss.s[i].buf, ss.s[i].len} == Str("keepme-2");
        }
        expect(t, "termsecretadd full", n==TERMSECRETS ? Str("full") : Str("not"), "full");
        expect(t, "termsecretadd rank", keep==1 ? Str("once") : Str("not"), "once");
        Arena s = *a;
        iz    h = 0;
        expect(t, "termsecretadd evicted", termmask(&s, "secretaa secretap", &ss, &h),
               "secretaa [hidden]");
        TermSecrets top = {};
        for (i32 i = 0; i < TERMSECRETS; i++) {
            u8 v[6] = {'t', 'o', 'p', '-', (u8)('a'+i), 'x'};
            termsecretadd(&top, Str{v, 6}, 2);
        }
        expect(t, "termsecretadd outranked", termsecretadd(&top, "lowly1", 1) ? Str("kept") : Str("no"),
               "no");
    }

    // The owner of a session's terminals
    {
        static Str rows[] = {
            "root - - 0 /p",
            "kid root - 0 /p",
            "grandkid kid - 0 /p",
            "side - root 0 /p",
            "sidekid side - 0 /p",
            "wtside - root 1 /w",
            "away - root 0 /q",
            "orphan gone - 0 /p",
            "loop loop - 0 /p",
            {},
        };
        Arena s = *a;
        expect(t, "termrootwalk root", termrootwalk(&s, "root", termlookrows, rows), "root");
        expect(t, "termrootwalk kid", termrootwalk(&s, "grandkid", termlookrows, rows), "root");
        expect(t, "termrootwalk side", termrootwalk(&s, "sidekid", termlookrows, rows), "root");
        expect(t, "termrootwalk worktree", termrootwalk(&s, "wtside", termlookrows, rows),
               "wtside");
        expect(t, "termrootwalk folder", termrootwalk(&s, "away", termlookrows, rows), "away");
        expect(t, "termrootwalk orphan", termrootwalk(&s, "orphan", termlookrows, rows),
               "orphan");
        expect(t, "termrootwalk loop", termrootwalk(&s, "loop", termlookrows, rows), "loop");
        expect(t, "termrootwalk unknown", termrootwalk(&s, "who", termlookrows, rows), "who");
    }
}

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

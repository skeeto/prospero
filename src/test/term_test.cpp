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
}

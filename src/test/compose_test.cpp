// Compose box tests: slash commands as a message's first word only, their
// completion, Up and Down through the session's history, the draft's
// debounce, the editor's command line, and the '@' mentions the caret can
// be inside while they are typed.

static Str slashstr(Arena *a, Str text)
{
    Slash s = parseslash(text);
    Buf   b(a, 64);
    print(&b, s.cmd ? slashinfo[s.cmd].name : Str("text"));
    putbyte(&b, '|');
    print(&b, s.arg);
    if (s.err.len) {
        print(&b, "|err");
    }
    return finish(&b);
}

static Str histstr(Arena *a, HistKey k)
{
    static Str acts[] = {"none", "start", "end", "recall"};
    Buf b(a, 32);
    print(&b, acts[k.act]);
    putbyte(&b, ' ');
    print(&b, (i64)k.pos);
    return finish(&b);
}

// Arguments joined by '|', to compare as one string.
static Str argstr(Arena *a, Slice<Str> argv)
{
    Buf b(a, 256);
    for (iz i = 0; i < argv.len; i++) {
        print(&b, i ? Str("|") : Str(""));
        print(&b, argv[i]);
    }
    return finish(&b);
}

// A mention parse as "on beg end [query]", or "off" for none.
static Str mentionstr(Arena *a, Str text, iz caret)
{
    MentionAt m = mentionat(text, caret);
    Buf       b(a, 64);
    print(&b, m.on ? Str("on") : Str("off"));
    putbyte(&b, ' ');
    print(&b, (i64)m.beg);
    putbyte(&b, ' ');
    print(&b, (i64)m.end);
    putbyte(&b, ' ');
    putbyte(&b, '[');
    print(&b, m.query);
    putbyte(&b, ']');
    return finish(&b);
}

// A pick list as "index:score" entries, or just the indices.
static Str pickstr(Arena *a, Slice<MentionPick> p, b32 scores)
{
    Buf b(a, 64);
    for (iz i = 0; i < p.len; i++) {
        if (i) {
            putbyte(&b, ' ');
        }
        print(&b, (i64)p[i].i);
        if (scores) {
            putbyte(&b, ':');
            print(&b, (i64)p[i].score);
        }
    }
    return finish(&b);
}

// A pick list's paths in order: what the same query gives on any
// filesystem, whatever order the candidates came in.
static Str pickpaths(Arena *a, Slice<Str> paths, Slice<MentionPick> p)
{
    Buf b(a, 64);
    for (iz i = 0; i < p.len; i++) {
        if (i) {
            putbyte(&b, ' ');
        }
        print(&b, paths[p[i].i]);
    }
    return finish(&b);
}

static void test_compose(Test *t)
{
    Arena a = t->perm;

    // Exact first words only
    expect(t, "slash.compact", slashstr(&a, "/compact"), "/compact|");
    expect(t, "slash.focus", slashstr(&a, "/compact  the parser \n and tests "),
           "/compact|the parser \n and tests");
    expect(t, "slash.lead", slashstr(&a, "  /model large"), "/model|large");
    expect(t, "slash.model", slashstr(&a, "/model"), "/model|");
    expect(t, "slash.rename", slashstr(&a, "/rename A new title"), "/rename|A new title");
    expect(t, "slash.renameempty", slashstr(&a, "/rename  "), "/rename||err");
    expect(t, "slash.fork", slashstr(&a, "/fork"), "/fork|");
    expect(t, "slash.forkargs", slashstr(&a, "/fork now"), "/fork|now|err");
    expect(t, "slash.export", slashstr(&a, "/export\n"), "/export|");
    expect(t, "slash.background", slashstr(&a, "/background"), "/background|");
    expect(t, "slash.bgargs", slashstr(&a, "/background now"), "/background|now|err");
    expect(t, "slash.pause", slashstr(&a, "/pause"), "/pause|");
    expect(t, "slash.continue", slashstr(&a, " /continue "), "/continue|");
    expect(t, "slash.contargs", slashstr(&a, "/continue please"), "/continue|please|err");
    expect(t, "slash.forkturn", slashstr(&a, "/fork 3"), "/fork|3");
    expect(t, "slash.forkn", istr(&a, parseslash("/fork 3").turn), "3");
    expect(t, "slash.forkall", istr(&a, parseslash("/fork").turn), "-1");
    expect(t, "slash.forkneg", slashstr(&a, "/fork -1"), "/fork|-1|err");
    expect(t, "slash.raw", istr(&a, parseslash("/export raw").raw), "1");
    expect(t, "slash.md", slashstr(&a, "/export md"), "/export|md");
    expect(t, "slash.exportbad", slashstr(&a, "/export pdf"), "/export|pdf|err");
    expect(t, "slash.newline", slashstr(&a, "/compact\nkeep the API"), "/compact|keep the API");
    expect(t, "slash.path", slashstr(&a, "/usr/bin/foo is broken"), "text|");
    expect(t, "slash.prefix", slashstr(&a, "/comp"), "text|");
    expect(t, "slash.longer", slashstr(&a, "/compacted"), "text|");
    expect(t, "slash.case", slashstr(&a, "/Compact"), "text|");
    expect(t, "slash.second", slashstr(&a, "please /compact"), "text|");
    expect(t, "slash.alone", slashstr(&a, "/"), "text|");
    expect(t, "slash.side", slashstr(&a, "/side"), "/side|");
    expect(t, "slash.sideargs", slashstr(&a, "/side why"), "/side|why|err");

    // A side chat's quote of the selection
    expect(t, "side.quote", sidequote(&a, "one\n\ntwo\n"), "> one\n>\n> two\n\n");
    expect(t, "side.crlf", sidequote(&a, "a\r\nb"), "> a\n> b\n\n");
    expect(t, "side.empty", sidequote(&a, "\n"), "");
    expect(t, "slash.empty", slashstr(&a, ""), "text|");

    // Completion while the first word is typed
    u32 all = slashmatches("/");
    expect(t, "slash.all", istr(&a, slashcount(all)), istr(&a, SLASH_COUNT-1));
    expect(t, "slash.first", slashinfo[slashnth(all, 0)].name, "/compact");
    expect(t, "slash.last", slashinfo[slashnth(all, SLASH_COUNT-2)].name, "/continue");
    expect(t, "slash.past", istr(&a, slashnth(all, SLASH_COUNT-1)), "0");
    u32 r = slashmatches("/r");
    expect(t, "slash.r", istr(&a, slashcount(r)), "1");
    expect(t, "slash.rname", slashinfo[slashnth(r, 0)].name, "/rename");
    expect(t, "slash.space", istr(&a, slashmatches("/rename ")), "0");
    expect(t, "slash.nomatch", istr(&a, slashmatches("/usr")), "0");
    expect(t, "slash.noslash", istr(&a, slashmatches("compact")), "0");
    expect(t, "slash.whole", istr(&a, slashcount(slashmatches("/fork"))), "1");
    expect(t, "slash.completes", istr(&a, slashcompletes("/fo", SLASH_FORK)), "1");
    expect(t, "slash.complete", istr(&a, slashcompletes("/fork", SLASH_FORK)), "0");

    // Mentions: '@' at a word's start, the word it runs through, quotes
    expect(t, "mention.trigger", mentionstr(&a, "@src/ui", 7), "on 0 7 [src/ui]");
    expect(t, "mention.spacebefore", mentionstr(&a, "hi @src", 7), "on 3 7 [src]");
    expect(t, "mention.newline", mentionstr(&a, "hi\n@src", 7), "on 3 7 [src]");
    expect(t, "mention.paren", mentionstr(&a, "(see @src", 9), "on 5 9 [src]");
    expect(t, "mention.lead", mentionstr(&a, "@", 1), "on 0 1 []");
    expect(t, "mention.word", mentionstr(&a, "foo@bar.com", 11), "off 0 0 []");
    expect(t, "mention.scope", mentionstr(&a, "packages/@scope/pkg", 18), "off 0 0 []");
    expect(t, "mention.tight", mentionstr(&a, "a@b", 3), "off 0 0 []");
    // The caret mid-word: beg and end span the whole word
    expect(t, "mention.mid", mentionstr(&a, "@src/ui/draw.cpp", 7), "on 0 16 [src/ui]");
    // Quoted, with and without the closing quote
    expect(t, "mention.quote", mentionstr(&a, "@\"my file.txt\"", 14), "on 0 14 [my file.txt]");
    expect(t, "mention.quotemid", mentionstr(&a, "@\"my file.txt\"", 7), "on 0 14 [my fi]");
    expect(t, "mention.quoteafter", mentionstr(&a, "@\"my file.txt\" x", 16), "off 0 0 []");
    expect(t, "mention.quoteempty", mentionstr(&a, "@\"my file.txt\"", 2), "on 0 14 []");
    expect(t, "mention.quoteatstart", mentionstr(&a, "@\"my file.txt\"", 1), "on 0 14 []");
    expect(t, "mention.quoteat", mentionstr(&a, "@\"my file.txt\"", 13), "on 0 14 [my file.txt]");
    // A space inside the quotes does not end the token
    expect(t, "mention.quotespace", mentionstr(&a, "@\"my file.txt\"", 5), "on 0 14 [my ]");
    // Nor does the space after the closing quote stay in the token
    expect(t, "mention.quotespacedone", mentionstr(&a, "@\"my file.txt\" ", 15), "off 0 0 []");
    // A quoted folder's '/' is inside the quotes, as mentiontext makes it
    expect(t, "mention.quotedir", mentionstr(&a, "@\"my dir/\"", 10), "on 0 10 [my dir/]");
    expect(t, "mention.quotedirdone", mentionstr(&a, "@\"my dir/\" x", 12), "off 0 0 []");
    // A quoted token with no closing quote yet runs to the end of the
    // line, spaces and all, so accepting it replaces the whole path
    expect(t, "mention.quoteopen", mentionstr(&a, "@\"my file.tx", 12), "on 0 12 [my file.tx]");
    expect(t, "mention.quoteopenmid", mentionstr(&a, "@\"my file.tx", 5), "on 0 12 [my ]");
    expect(t, "mention.quoteopenline", mentionstr(&a, "@\"my file\nnext", 9), "on 0 9 [my file]");
    // A space outside them does, and the caret after it is in none
    expect(t, "mention.space", mentionstr(&a, "@src more", 4), "on 0 4 [src]");
    expect(t, "mention.spaceat", mentionstr(&a, "@src more", 5), "off 0 0 []");
    expect(t, "mention.spaceend", mentionstr(&a, "@src more", 9), "off 0 0 []");
    // A second '@' in the word
    expect(t, "mention.second", mentionstr(&a, "@a@b", 4), "off 0 0 []");
    expect(t, "mention.secondmid", mentionstr(&a, "@a@b", 2), "off 0 0 []");
    // The token is capped at MENTION_QUERY bytes from the '@'
    {
        Buf b(&a, MENTION_QUERY+32);
        putbyte(&b, '@');
        for (i32 i = 0; i < MENTION_QUERY-1; i++) {
            putbyte(&b, 'a');
        }
        Str at = finish(&b);
        expect(t, "mention.cap", istr(&a, mentionat(at, at.len).on), "1");
        MentionAt m = mentionat(at, at.len);
        expect(t, "mention.capend", istr(&a, m.end), istr(&a, MENTION_QUERY));
        expect(t, "mention.capquery", istr(&a, m.query.len), istr(&a, MENTION_QUERY-1));
        b = Buf(&a, MENTION_QUERY+32);
        putbyte(&b, '@');
        for (i32 i = 0; i < MENTION_QUERY; i++) {
            putbyte(&b, 'a');
        }
        at = finish(&b);
        expect(t, "mention.caplong", istr(&a, mentionat(at, at.len).on), "0");
    }
    expect(t, "mention.textdirparse", mentionstr(&a, mentiontext(&a, "my dir", 1), 10),
           "on 0 10 [my dir/]");
    expect(t, "mention.textparse", mentionstr(&a, mentiontext(&a, "my file.txt", 0), 14),
           "on 0 14 [my file.txt]");
    // The text accepting a pick makes
    expect(t, "mention.text", mentiontext(&a, "src/ui/draw.cpp", 0), "@src/ui/draw.cpp ");
    expect(t, "mention.textspace", mentiontext(&a, "my file.txt", 0), "@\"my file.txt\" ");
    expect(t, "mention.textdir", mentiontext(&a, "src/ui", 1), "@src/ui/");
    expect(t, "mention.textdirspace", mentiontext(&a, "my dir", 1), "@\"my dir/\"");
    // A path that cannot be read back is refused
    expect(t, "mention.refuse", istr(&a, mentiontext(&a, "a@b", 0).len), "0");
    expect(t, "mention.refusequote", istr(&a, mentiontext(&a, "a\"b", 0).len), "0");
    expect(t, "mention.refuseempty", istr(&a, mentiontext(&a, "", 0).len), "0");
    // Ranking a query against candidate paths: the basename first, the
    // letters in order, a '/' honoured segment by segment, and one fixed
    // order for equal scores
    {
        Str draw[] = {"src/ui/draw.cpp", "src/ui/draw.h", "docs/draw.md",
                      "docs/drawing/notes.txt"};
        Slice<Str> dp = {draw, countof(draw)};
        // The three basename matches tie on score and the shorter path
        // leads: docs/draw.md, src/ui/draw.h, src/ui/draw.cpp. The folder
        // that merely holds the letters is last; without
        // MENTION_BASE_BONUS the two under src/ui win at 94 instead, which
        // is this case failing.
        expect(t, "mention.rank.basename", pickstr(&a, mentionpicks(&a, dp, "draw", 10), 1),
               "2:158 1:158 0:158 3:83");
        Str one[] = {"src/ui/draw.cpp"};
        Slice<Str> op = {one, countof(one)};
        expect(t, "mention.rank.sub", pickstr(&a, mentionpicks(&a, op, "drw", 10), 0), "0");
        expect(t, "mention.rank.none", istr(&a, mentionpicks(&a, op, "zzz", 10).len), "0");
        // A query with '/' matches inside one segment per part, so a path
        // that only straddles two of them does not
        Str seg[] = {"src/ui/draw.cpp", "src/uindex/dr/w.cpp"};
        Slice<Str> sp = {seg, countof(seg)};
        expect(t, "mention.rank.segment", pickstr(&a, mentionpicks(&a, sp, "ui/drw", 10), 0), "0");
        expect(t, "mention.rank.case", pickstr(&a, mentionpicks(&a, dp, "DRAW", 10), 0),
               "2 1 0 3");
        // Equal scores: the shorter path, then its bytes, whatever order
        // the candidates arrive in
        Str ord1[] = {"z/abc.cpp", "a/abc.cpp"};
        Str ord2[] = {"a/abc.cpp", "z/abc.cpp"};
        Slice<Str> o1 = {ord1, countof(ord1)};
        Slice<Str> o2 = {ord2, countof(ord2)};
        expect(t, "mention.rank.order", pickpaths(&a, o1, mentionpicks(&a, o1, "abc", 10)),
               "a/abc.cpp z/abc.cpp");
        expect(t, "mention.rank.order2", pickpaths(&a, o2, mentionpicks(&a, o2, "abc", 10)),
               "a/abc.cpp z/abc.cpp");
        expect(t, "mention.rank.max", istr(&a, mentionpicks(&a, dp, "draw", 2).len), "2");
        expect(t, "mention.rank.maxstr", pickstr(&a, mentionpicks(&a, dp, "draw", 2), 0), "2 1");
        // An empty, or blank, query ranks nothing and keeps the list's own
        // order, truncated to max
        Str rec[] = {"c.txt", "a.txt", "b.txt"};
        Slice<Str> rp = {rec, countof(rec)};
        expect(t, "mention.rank.empty", pickstr(&a, mentionpicks(&a, rp, "", 2), 1), "0:0 1:0");
        expect(t, "mention.rank.emptyall", pickstr(&a, mentionpicks(&a, rp, "", 10), 0), "0 1 2");
        expect(t, "mention.rank.space", pickstr(&a, mentionpicks(&a, rp, "  ", 10), 0), "0 1 2");
        // With nodots, a dot file or a dot folder's file is no candidate,
        // unless the query has a part beginning with '.', as a folder's
        // listing hides them; without it, every path is
        Str ds[] = {".claude/skills/a.md", "src/a.md", ".env", "a.md", "src/.hidden/a.md"};
        Slice<Str> dp2 = {ds, countof(ds), countof(ds)};
        expect(t, "mention.rank.dots", pickpaths(&a, dp2, mentionpicks(&a, dp2, "a.md", 10, 1)),
               "a.md src/a.md");
        expect(t, "mention.rank.dotsempty", pickstr(&a, mentionpicks(&a, dp2, "", 10, 1), 0),
               "1 3");
        expect(t, "mention.rank.dotsasked",
               pickpaths(&a, dp2, mentionpicks(&a, dp2, ".claude/a", 10, 1)),
               ".claude/skills/a.md");
        expect(t, "mention.rank.dotsall", istr(&a, mentionpicks(&a, dp2, "a.md", 10).len),
               "4");
    }
    // Skills: /name exactly, after the commands, which keep their names
    {
        SkillItem items[] = {{"pdf", "Read PDFs", "/s/pdf/SKILL.md", 0, 0, {}},
                             {"compact", "", "/s/compact/SKILL.md", 0, 0, {}},
                             {"review", "", "/s/review/SKILL.md", 1, 0, {}}};
        Slice<SkillItem> sk = {items, countof(items)};
        Slash k = parseskill("  /pdf  file.pdf ", sk);
        expect(t, "skill.parse", concat(&a, concat(&a, k.skill, "|"), k.arg), "pdf|file.pdf");
        expect(t, "skill.hidden", parseskill("/review", sk).skill, "review");
        expect(t, "skill.taken", istr(&a, parseskill("/compact", sk).skill.len), "0");
        expect(t, "skill.prefix", istr(&a, parseskill("/pd x", sk).skill.len), "0");
        expect(t, "skill.path", istr(&a, parseskill("/pdf/x", sk).skill.len), "0");
        Slice<i32> p = slashpicks(&a, "/", sk);
        Buf b(&a, 64);
        for (iz i = 0; i < p.len; i++) {
            print(&b, slashpickinfo(&a, p[i], sk).name);
            putbyte(&b, ' ');
        }
        expect(t, "skill.picks", finish(&b),
               "/compact /model /rename /fork /export /plan /side /worktree /background /shell "
               "/pause /continue /pdf /review ");
        p = slashpicks(&a, "/pd", sk);  // not /plan
        expect(t, "skill.pick", istr(&a, p.len), "1");
        expect(t, "skill.pickdesc", slashpickinfo(&a, p[0], sk).desc, "Read PDFs");
        expect(t, "skill.pickspace", istr(&a, slashpicks(&a, "/pdf x", sk).len), "0");
    }

    // History: empty box, Up begins at the newest
    expect(t, "hist.begin", histstr(&a, histkey(1, 1, 0, 3, 1)), "recall 1");
    expect(t, "hist.none", histstr(&a, histkey(1, 1, 0, 0, 1)), "start 0");
    expect(t, "hist.down0", histstr(&a, histkey(0, 1, 0, 3, 1)), "end 0");
    // Text of the draft's own: the edges go to its start and end
    expect(t, "hist.top", histstr(&a, histkey(1, 1, 0, 3, 0)), "start 0");
    expect(t, "hist.bottom", histstr(&a, histkey(0, 1, 0, 3, 0)), "end 0");
    expect(t, "hist.middle", histstr(&a, histkey(1, 0, 0, 3, 0)), "none 0");
    // Recalling: further back at the top, forward at the bottom, past
    // the newest to the empty draft, and no further than the oldest
    expect(t, "hist.back", histstr(&a, histkey(1, 1, 1, 3, 0)), "recall 2");
    expect(t, "hist.oldest", histstr(&a, histkey(1, 1, 3, 3, 0)), "none 3");
    expect(t, "hist.fwd", histstr(&a, histkey(0, 1, 2, 3, 0)), "recall 1");
    expect(t, "hist.draft", histstr(&a, histkey(0, 1, 1, 3, 0)), "recall 0");
    expect(t, "hist.inside", histstr(&a, histkey(1, 0, 2, 3, 0)), "none 2");
    expect(t, "hist.insidedown", histstr(&a, histkey(0, 0, 2, 3, 0)), "none 2");

    // The debounce: due a second after the last key, once
    DraftClock c = {};
    expect(t, "draft.idle", istr(&a, draftdue(&c, 5000000)), "0");
    expect(t, "draft.loads", istr(&a, draftloads(&c)), "1");
    drafttyped(&c, 1000000);
    drafttyped(&c, 1400000);
    expect(t, "draft.early", istr(&a, draftdue(&c, 2300000)), "0");
    expect(t, "draft.due", istr(&a, draftdue(&c, 2400000)), "1");
    expect(t, "draft.at", istr(&a, c.due), "2400000");
    draftdone(&c);
    expect(t, "draft.sent", istr(&a, draftdue(&c, 9000000)), "0");
    expect(t, "draft.typed", istr(&a, draftloads(&c)), "0");

    // The editor: which, its name, its command line, its result
    expect(t, "editor.setting", editorpick(" code --wait ", "vim", "ed", 1), "code --wait");
    expect(t, "editor.visual", editorpick("", "gvim -f", "ed", 0), "gvim -f");
    expect(t, "editor.editor", editorpick("", "", "subl -w", 0), "subl -w");
    expect(t, "editor.mac", editorpick("", "", "", 1), "open -W -n -t");
    expect(t, "editor.none", istr(&a, editorpick("", " ", "", 0).len), "0");
    expect(t, "editor.name", editorname("/usr/local/bin/code --wait"), "code");
    expect(t, "editor.quoted", editorname("\"C:\\Program Files\\Sub\\subl.exe\" -w"), "subl.exe");
    expect(t, "editor.open", editorname("open -W -n -t"), "the default text editor");
    expect(t, "editor.sh", editorline(&a, "code --wait", "/tmp/it's here.md", 0),
           "code --wait '/tmp/it'\\''s here.md'");
    expect(t, "editor.cmd", editorline(&a, "notepad", "C:\\Temp\\d.md", 1),
           "notepad \"C:\\Temp\\d.md\"");
    expect(t, "editor.text", editortext("line one\nline two\r\n\n"), "line one\nline two");
    expect(t, "editor.keep", editortext("  indented\n\nend"), "  indented\n\nend");
    // The terminal: its arguments by platform and program, and the
    // commands for a remote's shell
    Slice<Str> none = {};
    Slice<Str> ssh  = termssh(&a, "ssh", "me@box", "/home/me/it's", 0);
    expect(t, "term.ssh", argstr(&a, ssh),
           "ssh|-t|--|me@box|cd '/home/me/it'\\''s' && exec \"$SHELL\" -l");
    expect(t, "term.sshwin", argstr(&a, termssh(&a, "ssh", "box", "C:\\w", 1)),
           "ssh|-t|--|box|cmd /k cd /d \"C:\\w\"");
    expect(t, "term.sshport", argstr(&a, termssh(&a, "ssh", "me@[::1]:2222", "/w", 0)),
           "ssh|-t|-p|2222|--|me@::1|cd '/w' && exec \"$SHELL\" -l");
    Slice<Str> wsl = termwsl(&a, "wsl.exe", "Debian", "/mnt/c/w");
    expect(t, "term.wsl", argstr(&a, wsl), "wsl.exe|-d|Debian|--cd|/mnt/c/w");
    TermRun tr = termargv(&a, TERM_MAC, {}, "/p", none);
    expect(t, "term.mac", argstr(&a, tr.argv), "open|-a|Terminal|/p");
    tr = termargv(&a, TERM_MAC, "iTerm", "/p", termssh(&a, "ssh", "me@box", "/home/me", 0));
    expect(t, "term.macssh", argstr(&a, tr.argv), "open|-a|iTerm");
    expect(t, "term.script", tr.script,
           "#!/bin/sh\nrm -f \"$0\"\nexec 'ssh' '-t' '--' 'me@box' "
           "'cd '\\''/home/me'\\'' && exec \"$SHELL\" -l'\n");
    tr = termargv(&a, TERM_WIN, "C:\\x\\wt.exe", "C:\\p", none);
    expect(t, "term.wt", argstr(&a, tr.argv), "C:\\x\\wt.exe|-d|C:\\p");
    Slice<Str> semi = {};
    semi = push(&a, semi, Str("a;b"));
    tr = termargv(&a, TERM_WIN, "wt.exe", "C:\\p", semi);
    expect(t, "term.wtsemi", argstr(&a, tr.argv), "wt.exe|-d|C:\\p|a\\;b");
    tr = termargv(&a, TERM_WIN, {}, "C:\\p", none);
    expect(t, "term.console", argstr(&a, tr.argv), "cmd.exe");
    tr = termargv(&a, TERM_WIN, {}, "C:\\p", wsl);
    expect(t, "term.consolecmd", argstr(&a, tr.argv), "wsl.exe|-d|Debian|--cd|/mnt/c/w");
    tr = termargv(&a, TERM_LINUX, "/usr/bin/gnome-terminal", "/p", none);
    expect(t, "term.gnome", argstr(&a, tr.argv), "/usr/bin/gnome-terminal|--working-directory=/p");
    tr = termargv(&a, TERM_LINUX, "ptyxis", "/p", wsl);
    expect(t, "term.ptyxis", argstr(&a, tr.argv),
           "ptyxis|--new-window|--working-directory=/p|--|wsl.exe|-d|Debian|--cd|/mnt/c/w");
    tr = termargv(&a, TERM_LINUX, "konsole", "/p", semi);
    expect(t, "term.konsole", argstr(&a, tr.argv), "konsole|--workdir|/p|-e|a;b");
    tr = termargv(&a, TERM_LINUX, "wezterm", "/p", semi);
    expect(t, "term.wezterm", argstr(&a, tr.argv), "wezterm|start|--cwd|/p|--|a;b");
    tr = termargv(&a, TERM_LINUX, "kitty", "/p", semi);
    expect(t, "term.kitty", argstr(&a, tr.argv), "kitty|--directory|/p|a;b");
    tr = termargv(&a, TERM_LINUX, "xterm", "/p", semi);
    expect(t, "term.xterm", argstr(&a, tr.argv), "xterm|-e|a;b");
    tr = termargv(&a, TERM_LINUX, "st", "/p", none);
    expect(t, "term.unknown", argstr(&a, tr.argv), "st");

    // A session's own shell, by hand: none for a POSIX /bin/sh, else as
    // a login shell, and on macOS changing to its folder first
    expect(t, "term.binsh", argstr(&a, termshell(&a, "sh", "/bin/sh", 0)), "");
    expect(t, "term.bash", argstr(&a, termshell(&a, "sh", "/opt/homebrew/bin/bash", 0)), "");
    expect(t, "term.noshell", argstr(&a, termshell(&a, "sh", {}, 1)), "");
    expect(t, "term.w64sh", argstr(&a, termshell(&a, "sh", "C:/w64devkit/bin/sh.exe", 1)),
           "C:/w64devkit/bin/sh.exe|-l");
    expect(t, "term.busybox", argstr(&a, termshell(&a, "busybox", "C:/bb/busybox.exe", 1)),
           "C:/bb/busybox.exe|sh|-l");
    expect(t, "term.pwsh", argstr(&a, termshell(&a, "pwsh", "C:/ps/pwsh.exe", 1)),
           "C:/ps/pwsh.exe|-NoLogo");
    expect(t, "term.cmd", argstr(&a, termshell(&a, "cmd", "C:\\W\\cmd.exe", 1)),
           "C:\\W\\cmd.exe");
    expect(t, "term.zsh", argstr(&a, termshell(&a, "sh", "/bin/zsh", 0)), "/bin/zsh|-l");
    expect(t, "term.wslshell", argstr(&a, termshell(&a, "wsl", "C:\\W\\wsl.exe", 1)),
           "C:\\W\\wsl.exe");
    expect(t, "term.wsldistro",
           argstr(&a, termshell(&a, "wsl", "C:\\W\\wsl.exe", 1, "Debian")),
           "C:\\W\\wsl.exe|-d|Debian");
    Slice<Str> zsh = termshell(&a, "sh", "/bin/zsh", 0);
    tr = termargv(&a, TERM_WIN, "wt.exe", "C:\\p", termshell(&a, "sh", "C:/w/sh.exe", 1), 1);
    expect(t, "term.wtshell", argstr(&a, tr.argv), "wt.exe|-d|C:\\p|C:/w/sh.exe|-l");
    tr = termargv(&a, TERM_MAC, {}, "/p/it's", zsh, 1);
    expect(t, "term.machere", argstr(&a, tr.argv), "open|-a|Terminal");
    expect(t, "term.macherescript", tr.script,
           "#!/bin/sh\nrm -f \"$0\"\ncd -- '/p/it'\\''s' || exit\n"
           "exec '/bin/zsh' '-l'\n");
}

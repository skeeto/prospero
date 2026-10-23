// Compose box tests: slash commands as a message's first word only, their
// completion, Up and Down through the session's history, the draft's
// debounce, and the editor's command line.

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
    expect(t, "slash.newline", slashstr(&a, "/compact\nkeep the API"), "/compact|keep the API");
    expect(t, "slash.path", slashstr(&a, "/usr/bin/foo is broken"), "text|");
    expect(t, "slash.prefix", slashstr(&a, "/comp"), "text|");
    expect(t, "slash.longer", slashstr(&a, "/compacted"), "text|");
    expect(t, "slash.case", slashstr(&a, "/Compact"), "text|");
    expect(t, "slash.second", slashstr(&a, "please /compact"), "text|");
    expect(t, "slash.alone", slashstr(&a, "/"), "text|");
    expect(t, "slash.empty", slashstr(&a, ""), "text|");

    // Completion while the first word is typed
    u32 all = slashmatches("/");
    expect(t, "slash.all", istr(&a, slashcount(all)), istr(&a, SLASH_COUNT-1));
    expect(t, "slash.first", slashinfo[slashnth(all, 0)].name, "/compact");
    expect(t, "slash.last", slashinfo[slashnth(all, SLASH_COUNT-2)].name, "/export");
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
}

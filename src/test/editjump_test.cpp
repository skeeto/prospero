// Editor preset tests (editjump.cpp): Open in editor's command lines,
// cmd.exe's by the full path, a terminal editor's name elsewhere and on
// another host, the sh -c words that keep a path from any shell, which
// editor a host runs, and the login shell that starts it. Outside
// PROSPERO_GUI, so the engine-only builds run them too.

// Words joined by '|', to compare as one string.
static Str ejwords(Arena *a, Slice<Str> argv)
{
    Buf b(a, 256);
    for (iz i = 0; i < argv.len; i++) {
        print(&b, i ? Str("|") : Str(""));
        print(&b, argv[i]);
    }
    return finish(&b);
}

// An editorrun as "SCRIPT|ARGS...", else "why: " and why.
static Str ejrun(Arena *a, EditRun r)
{
    if (r.why.len) {
        return concat(a, "why: ", r.why);
    }
    Slice<Str> w = {};
    w = push(a, w, r.script);
    for (iz i = 0; i < r.args.len; i++) {
        w = push(a, w, r.args[i]);
    }
    return ejwords(a, w);
}

// A host for editpick: the programs it has, as commands name them, and
// the one that is busybox's.
struct EjHost {
    Str have[3];
    Str box;
};

static i32 ejfind(void *ctx, Arena *, Str prog)
{
    EjHost *h = (EjHost *)ctx;
    for (iz i = 0; i < countof(h->have); i++) {
        if (h->have[i].len && prog==h->have[i]) {
            return prog==h->box ? EDITOR_BOX : EDITOR_FOUND;
        }
    }
    return EDITOR_MISSING;
}

static void test_editjump(Test *t)
{
    Arena a = t->perm;

    // cmd.exe's ~, the home directory
    expect(t, "editor.home", cmdhome(&a, "~\\bin\\ed.exe -w", "C:\\Users\\J S"),
               "\"C:\\Users\\J S\\bin\\ed.exe\" -w");
    expect(t, "editor.homealone", cmdhome(&a, " ~/ed.exe ", "C:/U"), "\"C:/U/ed.exe\"");
    expect(t, "editor.nohome", cmdhome(&a, "~x -w", "C:/U"), "~x -w");

    // Open in editor: each preset, quotes and spaces, cmd.exe's refusal
    // of %, a terminal word before the editor, terminal editors marked,
    // and [ui] editor_line's templates
    struct {
        Str name, cmd, tmpl, path;
        i32 line, col;
        b32 win;
        Str want;  // the command ("term: " ahead for a terminal's), else "why: " and why
    } jumps[] = {
        {"gvim", "gvim -f", {}, "/a/b.c", 12, 0, 0, "gvim -f +12 '/a/b.c'"},
        {"mvim", "mvim", {}, "/a/b.c", 3, 4, 0, "mvim +3 '/a/b.c'"},
        {"emacs", "emacs", {}, "/a/b.c", 3, 0, 0, "emacs +3 '/a/b.c'"},
        {"emacsclient", "emacsclient -c", {}, "/a/b.c", 3, 0, 0, "emacsclient -c +3 '/a/b.c'"},
        {"gedit", "/usr/bin/gedit", {}, "/a/b.c", 9, 0, 0, "/usr/bin/gedit +9 '/a/b.c'"},
        {"code", "code --wait", {}, "/a/b.c", 12, 5, 0, "code --wait -g '/a/b.c:12:5'"},
        {"codenocol", "code", {}, "/a/b.c", 12, 0, 0, "code -g '/a/b.c:12'"},
        {"insiders", "code-insiders", {}, "/a/b.c", 1, 0, 0, "code-insiders -g '/a/b.c:1'"},
        {"codium", "codium", {}, "/a/b.c", 2, 0, 0, "codium -g '/a/b.c:2'"},
        {"cursor", "cursor", {}, "/a/b.c", 2, 0, 0, "cursor -g '/a/b.c:2'"},
        {"windsurf", "windsurf", {}, "/a/b.c", 2, 0, 0, "windsurf -g '/a/b.c:2'"},
        {"zed", "zed", {}, "/a/b.c", 7, 2, 0, "zed '/a/b.c:7:2'"},
        {"zeditor", "zeditor", {}, "/a/b.c", 7, 0, 0, "zeditor '/a/b.c:7'"},
        {"subl", "subl -w", {}, "/a/b.c", 7, 0, 0, "subl -w '/a/b.c:7'"},
        {"idea", "idea", {}, "/a/b.c", 8, 0, 0, "idea --line 8 '/a/b.c'"},
        {"pycharm", "/opt/pycharm/bin/pycharm.sh", {}, "/a/b.c", 8, 0, 0,
         "/opt/pycharm/bin/pycharm.sh '/a/b.c'"},
        {"clion", "clion64.exe", {}, "C:\\a\\b.c", 8, 0, 1, "clion64.exe --line 8 \"C:\\a\\b.c\""},
        {"mate", "mate -w", {}, "/a/b.c", 4, 0, 0, "mate -w -l 4 '/a/b.c'"},
        {"kate", "kate", {}, "/a/b.c", 4, 0, 0, "kate -l 4 '/a/b.c'"},
        {"npp", "\"C:\\Program Files\\Notepad++\\notepad++.exe\"", {}, "C:\\x y\\b.c", 6, 0, 1,
         "\"C:\\Program Files\\Notepad++\\notepad++.exe\" -n6 \"C:\\x y\\b.c\""},
        {"codecmd", "Code.CMD", {}, "C:\\a\\b.c", 6, 2, 1, "Code.CMD -g \"C:\\a\\b.c:6:2\""},
        {"unknown", "myedit -x", {}, "/a/b.c", 6, 0, 0, "myedit -x '/a/b.c'"},
        {"quote", "code", {}, "/it's here/b.c", 6, 0, 0, "code -g '/it'\\''s here/b.c:6'"},
        {"env", "LANG=C FOO=1 code", {}, "/a/b.c", 6, 0, 0, "LANG=C FOO=1 code -g '/a/b.c:6'"},
        {"alacritty", "alacritty -e nvim", {}, "/a/b.c", 6, 0, 0, "alacritty -e nvim +6 '/a/b.c'"},
        {"kitty hx", "kitty hx", {}, "/a/b.c", 6, 3, 0, "kitty hx '/a/b.c:6:3'"},
        {"open", "open -W -n -t", {}, "/a/b.c", 6, 0, 0, "open -t '/a/b.c'"},
        {"percent", "code", {}, "C:\\100%\\b.c", 6, 0, 1,
         "why: Its path holds %, \" or a control character, which cmd.exe cannot pass on"},
        {"control", "code", {}, "C:\\a\x01\\b.c", 6, 0, 1,
         "why: Its path holds %, \" or a control character, which cmd.exe cannot pass on"},
        {"shpercent", "code", {}, "/100%/b.c", 6, 0, 0, "code -g '/100%/b.c:6'"},
        {"vim", "vim", {}, "/a/b.c", 6, 0, 0,
         "term: vim +6 '/a/b.c'"},
        {"nvim", "/usr/bin/nvim -p", {}, "/a/b.c", 6, 0, 0,
         "term: /usr/bin/nvim -p +6 '/a/b.c'"},
        {"nano", "EDITOR=x nano", {}, "/a/b.c", 6, 0, 0,
         "term: EDITOR=x nano +6 '/a/b.c'"},
        {"hx", "hx", {}, "/a/b.c", 6, 0, 0,
         "term: hx '/a/b.c:6'"},
        {"vimexe", "vim.exe", {}, "C:\\b.c", 6, 0, 1,
         "term: vim.exe +6 \"C:\\b.c\""},
        {"emacsnw", "emacs -nw", {}, "/a/b.c", 6, 0, 0,
         "term: emacs -nw +6 '/a/b.c'"},
        {"emacsclientt", "emacsclient -t", {}, "/a/b.c", 6, 0, 0,
         "term: emacsclient -t +6 '/a/b.c'"},
        {"emacstty", "emacsclient --tty", {}, "/a/b.c", 6, 0, 0,
         "term: emacsclient --tty +6 '/a/b.c'"},
        {"none", " ", {}, "/a/b.c", 6, 0, 0,
         "why: No editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)"},
        {"tmpl", "vim", "sh /t/e.sh +{line} {file}", "/a b/c.c", 6, 0, 0,
         "sh /t/e.sh +6 '/a b/c.c'"},
        {"tmplcol", {}, "ed --at {file}:{line}:{col}", "/a/c.c", 6, 0, 0, "ed --at '/a/c.c':6:1"},
        {"tmplcol2", "code", " ed {col}x{line} ", "/a/c.c", 6, 9, 0, "ed 9x6 '/a/c.c'"},
        {"tmplwin", {}, "ed.exe /g {line} {file}", "C:\\a b\\c.c", 6, 0, 1,
         "ed.exe /g 6 \"C:\\a b\\c.c\""},
        {"tmplterm", "code", "vim +{line} {file}", "/a/c.c", 6, 0, 0,
         "term: vim +6 '/a/c.c'"},
        {"tmplbrace", {}, "ed {nope} {file}", "/a/c.c", 6, 0, 0, "ed {nope} '/a/c.c'"},
        {"quotewin", "code", {}, "C:/x\"&calc&\"y.c", 6, 0, 1,
         "why: Its path holds %, \" or a control character, which cmd.exe cannot pass on"},
        {"quotesh", "code", {}, "/x\"y.c", 6, 0, 0, "code -g '/x\"y.c:6'"},
        {"tmpldq", {}, "myedit --goto \"{file}:{line}\"", "/a/c.c", 6, 0, 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"tmplsq", {}, "myedit '{file}'", "/a/c.c", 6, 0, 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"tmplwindq", {}, "myedit \"{file}\"", "C:\\a.c", 6, 0, 1,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"tmplclosed", {}, "myedit \"--x\" '-y' {file}", "/a/c.c", 6, 0, 0,
         "myedit \"--x\" '-y' '/a/c.c'"},
        {"tmplescaped", {}, "myedit \\\" {file}", "/a/c.c", 6, 0, 0,
         "myedit \\\" '/a/c.c'"},
        {"tmplwinsq", {}, "myedit '{file}'", "C:\\a.c", 6, 0, 1, "myedit '\"C:\\a.c\"'"},
        {"opena", "open -a Zed", {}, "/p/f.c", 12, 0, 0, "open -a Zed '/p/f.c'"},
        {"openwn", "open -W -n -a Cursor", {}, "/p/f.c", 12, 0, 0,
         "open -W -n -a Cursor '/p/f.c'"},
        {"openemacs", "open -a Emacs", {}, "/p/f.c", 12, 0, 0, "open -a Emacs '/p/f.c'"},
        {"emacsnws", "emacs --no-window-system", {}, "/a/b.c", 6, 0, 0,
         "term: emacs --no-window-system +6 '/a/b.c'"},
        {"helix", "helix", {}, "/a/b.c", 6, 2, 0, "term: helix '/a/b.c:6:2'"},
        {"ne", "ne", {}, "/a/b.c", 6, 0, 0, "term: ne +6 '/a/b.c'"},
        {"mcedit", "mcedit", {}, "/a/b.c", 6, 0, 0, "term: mcedit +6 '/a/b.c'"},
        {"tmpltermsq", {}, "vim '{file}'", "/a/c.c", 6, 0, 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
    };
    for (iz i = 0; i < countof(jumps); i++) {
        EditJump j = editorjump(&a, jumps[i].cmd, jumps[i].tmpl, jumps[i].path,
                                jumps[i].line, jumps[i].col, jumps[i].win);
        Str got = j.why.len ? concat(&a, "why: ", j.why)   :
                  j.term    ? concat(&a, "term: ", j.line) : j.line;
        expect(t, concat(&a, "editor.jump.", jumps[i].name), got, jumps[i].want);
    }
    // The window refuses a terminal editor, ahead of its template's why
    struct {
        Str name, cmd, tmpl, path;
        b32 win;
        Str want;  // the command, else "why: " and why
    } windows[] = {
        {"vim", "vim", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: vim runs in a terminal"},
        {"nvim", "/usr/bin/nvim -p", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: nvim runs in a terminal"},
        {"nano", "EDITOR=x nano", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: nano runs in a terminal"},
        {"hx", "hx", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: hx runs in a terminal"},
        {"vimexe", "vim.exe", {}, "C:\\b.c", 1,
         "why: [ui] editor must open a window of its own: vim.exe runs in a terminal"},
        {"emacsnw", "emacs -nw", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: emacs runs in a terminal"},
        {"emacsclientt", "emacsclient -t", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: emacsclient runs in a terminal"},
        {"emacstty", "emacsclient --tty", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: emacsclient runs in a terminal"},
        {"emacsnws", "emacs --no-window-system", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: emacs runs in a terminal"},
        {"tmplterm", "code", "vim +{line} {file}", "/a/c.c", 0,
         "why: [ui] editor must open a window of its own: vim runs in a terminal"},
        {"tmpltermsq", {}, "vim '{file}'", "/a/c.c", 0,
         "why: [ui] editor must open a window of its own: vim runs in a terminal"},
        {"ne", "ne", {}, "/a/b.c", 0,
         "why: [ui] editor must open a window of its own: ne runs in a terminal"},
        {"code", "code", {}, "/a/b.c", 0, "code -g '/a/b.c:6'"},
        {"alacritty", "alacritty -e nvim", {}, "/a/b.c", 0, "alacritty -e nvim +6 '/a/b.c'"},
        {"tmpldq", {}, "myedit \"{file}\"", "/a/c.c", 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"none", " ", {}, "/a/b.c", 0,
         "why: No editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)"},
    };
    for (iz i = 0; i < countof(windows); i++) {
        EditJump j = editorjump(&a, windows[i].cmd, windows[i].tmpl, windows[i].path,
                                6, 0, windows[i].win);
        j = editorwindow(&a, j);
        Str got = j.why.len ? concat(&a, "why: ", j.why) : j.line;
        expect(t, concat(&a, "editor.window.", windows[i].name), got, windows[i].want);
    }

    EditJump ej = editorjump(&a, "LANG=C \"/opt/my edit/ed\" -x", {}, "/f", 1, 0, 0);
    expect(t, "editor.jump.prog", ej.prog, "/opt/my edit/ed");
    expect(t, "editor.jump.unknown", istr(&a, ej.known), "0");
    ej = editorjump(&a, "code", {}, "/f", 1, 0, 0);
    expect(t, "editor.jump.known", istr(&a, ej.known), "1");
    ej = editorjump(&a, "open -a Zed", {}, "/f", 1, 0, 0);
    expect(t, "editor.jump.openunknown", istr(&a, ej.known), "0");
    // cmd.exe takes no VAR=value words: the program is the first word
    ej = editorjump(&a, "LANG=C code", {}, "C:\\f", 1, 0, 1);
    expect(t, "editor.jump.winassign", ej.prog, "LANG=C");
    ej = editorjump(&a, "\"C:\\Program Files\\VS Code\\code.cmd\" -n", {}, "C:\\f", 1, 0, 1);
    expect(t, "editor.jump.winprog", ej.prog, "C:\\Program Files\\VS Code\\code.cmd");
    ej = editorjump(&a, {}, "sh C:/t/e.sh +{line} {file}", "C:\\f", 3, 0, 1);
    expect(t, "editor.jump.wintmpl", ej.prog, "sh");
    // On Windows the program by the full path found for it, so that
    // cmd.exe never looks in the current folder: quoted, with
    // backslashes, the rest of the line as it was; refused when not a
    // full path, or with what cmd.exe cannot pass on in quotes
    struct {
        Str name, line, exe, want;
    } abss[] = {
        {"plain", "code -g \"C:\\a\\b.c:6\"", "C:\\Tools\\code.cmd",
         "\"C:\\Tools\\code.cmd\" -g \"C:\\a\\b.c:6\""},
        {"blanks", "\"C:\\Program Files\\VS Code\\code.cmd\" -g \"C:\\a b\\c.c:1\"",
         "C:\\Program Files\\VS Code\\code.cmd",
         "\"C:\\Program Files\\VS Code\\code.cmd\" -g \"C:\\a b\\c.c:1\""},
        {"slashes", "sh C:/t/e.sh +3 \"C:\\f\"", "C:/w64devkit/bin/sh.EXE",
         "\"C:\\w64devkit\\bin\\sh.EXE\" C:/t/e.sh +3 \"C:\\f\""},
        {"alone", "notepad", "C:\\Windows\\notepad.exe", "\"C:\\Windows\\notepad.exe\""},
        {"lead", "  vim.bat \"C:\\f\"", "C:\\bin\\vim.bat", "\"C:\\bin\\vim.bat\" \"C:\\f\""},
        {"amp", "ed \"C:\\f\"", "C:\\a&b ^c\\ed.exe", "\"C:\\a&b ^c\\ed.exe\" \"C:\\f\""},
        {"unc", "ed \"C:\\f\"", "\\\\srv\\share\\ed.exe", "\"\\\\srv\\share\\ed.exe\" \"C:\\f\""},
        {"relative", "ed \"C:\\f\"", "bin\\ed.exe", ""},
        {"drel", "ed \"C:\\f\"", "C:ed.exe", ""},
        {"rooted", "ed \"C:\\f\"", "\\bin\\ed.exe", ""},
        {"percent", "ed \"C:\\f\"", "C:\\100%\\ed.exe", ""},
        {"quote", "ed \"C:\\f\"", "C:\\a\"b\\ed.exe", ""},
        {"control", "ed \"C:\\f\"", "C:\\a\x01\\ed.exe", ""},
        {"none", "ed \"C:\\f\"", "", ""},
    };
    for (iz i = 0; i < countof(abss); i++) {
        expect(t, concat(&a, "editor.abs.", abss[i].name),
               cmdabs(&a, abss[i].line, abss[i].exe), abss[i].want);
    }

    // The command line by which cmd.exe runs a line
    expect(t, "editor.cline", cmdcline(&a, "C:\\Windows\\system32\\cmd.exe",
                                       "\"C:\\bin\\vim.bat\" \"C:\\f\""),
           "\"C:\\Windows\\system32\\cmd.exe\" /d /s /c \"\"C:\\bin\\vim.bat\" \"C:\\f\"\"");

    // A terminal editor, by its program past VAR=value words: a
    // terminal program running one opens a window of its own
    struct {
        Str name, cmd, want;
    } terms[] = {
        {"alacritty", "alacritty -e nvim", "0"},
        {"assign", "VAR=x vim", "1"},
        {"emacsnw", "emacs -nw", "1"},
        {"emacs", "emacs", "0"},
        {"emacsclient", "emacsclient --no-window-system", "1"},
        {"quoted", " \"/opt/my vi/nvim\" -p ", "1"},
        {"exe", "C:\\Vim\\VIM.EXE", "1"},
        {"code", "code -w", "0"},
        {"ne", "ne", "1"},
        {"helix", "/usr/bin/helix", "1"},
        {"nvi", "nvi", "1"},
        {"vis", "vis", "1"},
        {"mcedit", "mcedit", "1"},
        {"none", "", "0"},
    };
    for (iz i = 0; i < countof(terms); i++) {
        expect(t, concat(&a, "editor.term.", terms[i].name),
               istr(&a, editorterm(terms[i].cmd)), terms[i].want);
    }

    // A terminal editor's name for a host of another OS family
    struct {
        Str name, cmd, want;
    } bares[] = {
        {"exe", "C:/x/nvim.exe", "nvim"},
        {"upper", "C:\\Vim\\VIM.EXE -p", "vim"},
        {"quoted", "LANG=C \"C:\\Program Files\\Helix\\hx.exe\" --vsplit", "hx"},
        {"emacsclient", "emacsclient -t", "emacsclient -t"},
        {"emacsclienttty", "emacsclient --tty -a ''", "emacsclient -t"},
        {"emacs", "emacs -nw --debug-init", "emacs -nw"},
        {"emacsnws", "/usr/bin/emacs --no-window-system", "emacs -nw"},
        {"ne", "C:\\bin\\ne.exe", "ne"},
        {"helix", "/usr/bin/helix --vsplit", "helix"},
        {"window", "emacs", ""},
        {"code", "code", ""},
        {"alacritty", "alacritty -e nvim", ""},
        {"unknown", "myedit", ""},
        {"none", "", ""},
    };
    for (iz i = 0; i < countof(bares); i++) {
        expect(t, concat(&a, "editor.bare.", bares[i].name),
               editorbare(bares[i].cmd), bares[i].want);
    }

    // The program cut to its file name, the rest as written
    struct {
        Str name, cmd, want;
    } rebases[] = {
        {"path", "/opt/homebrew/bin/nvim -p", "nvim -p"},
        {"quoted", "LANG=C \"/opt/my edit/nvim\" -p", "LANG=C nvim -p"},
        {"single", "'/a b/hx' --vsplit", "hx --vsplit"},
        {"blanks", "'/a b/my editor' -x", "'my editor' -x"},
        {"bare", "  vim -u NONE ", "vim -u NONE"},
        {"alone", "/usr/bin/vi", "vi"},
        {"tmpl", "/opt/x/hx {file}:{line}", "hx {file}:{line}"},
        {"assignonly", "LANG=C", "LANG=C"},
        {"none", "", ""},
    };
    for (iz i = 0; i < countof(rebases); i++) {
        expect(t, concat(&a, "editor.rebase.", rebases[i].name),
               editorrebase(&a, rebases[i].cmd), rebases[i].want);
    }

    // sh -c's script and words: the path only ever a word after it
    struct {
        Str name, cmd, tmpl, path;
        i32 line, col;
        b32 nojump;
        Str want;
    } runs[] = {
        {"nvim", "nvim", {}, "/a/b.c", 12, 0, 0, "nvim \"$@\"|+12|/a/b.c"},
        {"hx", "hx", {}, "/a/b.c", 12, 3, 0, "hx \"$@\"|/a/b.c:12:3"},
        {"hxnocol", "/usr/bin/hx", {}, "/a/b.c", 12, 0, 0, "/usr/bin/hx \"$@\"|/a/b.c:12"},
        {"code", "code -w", {}, "/a/b.c", 3, 0, 0, "code -w \"$@\"|-g|/a/b.c:3"},
        {"idea", "idea", {}, "/a/b.c", 5, 0, 0, "idea \"$@\"|--line|5|/a/b.c"},
        {"mate", "mate", {}, "/a/b.c", 5, 0, 0, "mate \"$@\"|-l|5|/a/b.c"},
        {"npp", "notepad++", {}, "/a/b.c", 5, 0, 0, "notepad++ \"$@\"|-n5|/a/b.c"},
        {"env", "TERM=xterm vim", {}, "/a/b.c", 2, 0, 0, "TERM=xterm vim \"$@\"|+2|/a/b.c"},
        {"nojump", "vi", {}, "/a/b.c", 12, 0, 1, "vi \"$@\"|/a/b.c"},
        {"noline", "vim", {}, "/a/b.c", 0, 0, 0, "vim \"$@\"|/a/b.c"},
        {"unknown", "myedit -x", {}, "/a/b.c", 12, 0, 0, "myedit -x \"$@\"|/a/b.c"},
        {"tmpl", "vim", "ed +{line}:{col} {file} --x", "/a/b.c", 6, 2, 0,
         "ed +6:2 \"$1\" --x|/a/b.c"},
        {"tmplnofile", {}, "ed -c {line}", "/a/b.c", 6, 0, 0, "ed -c 6 \"$1\"|/a/b.c"},
        {"tmplnoline", {}, "ed +{line} {file}", "/a/b.c", 0, 0, 1, "ed +1 \"$1\"|/a/b.c"},
        {"tmplsq", {}, "ed '{file}'", "/a/b.c", 6, 0, 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"tmpldq", {}, "ed \"{file}:{line}\"", "/a/b.c", 6, 0, 0,
         "why: [ui] editor_line must not put {file} in quotes: it is quoted already"},
        {"none", " ", {}, "/a/b.c", 6, 0, 0,
         "why: No editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)"},
    };
    for (iz i = 0; i < countof(runs); i++) {
        EditRun r = editorrun(&a, runs[i].cmd, runs[i].tmpl, runs[i].path,
                              runs[i].line, runs[i].col, runs[i].nojump);
        expect(t, concat(&a, "editor.run.", runs[i].name), ejrun(&a, r), runs[i].want);
    }
    Str nasty = "/a b/it's \"$x\" `id` $(id)\nz.c";
    EditRun er = editorrun(&a, "nvim", {}, nasty, 3, 0, 0);
    expect(t, "editor.run.nastyscript", er.script, "nvim \"$@\"");
    expect(t, "editor.run.nastypath", er.args.len ? er.args[er.args.len-1] : Str{}, nasty);
    er = editorrun(&a, "hx", {}, nasty, 3, 4, 0);
    expect(t, "editor.run.nastycolon", er.args.len ? er.args[0] : Str{},
           concat(&a, nasty, ":3:4"));
    er = editorrun(&a, {}, "ed {file}", nasty, 3, 0, 0);
    expect(t, "editor.run.nastytmpl", ejrun(&a, er), concat(&a, "ed \"$1\"|", nasty));
    er = editorrun(&a, "LANG=C /usr/bin/nvim -p", {}, "/f", 1, 0, 0);
    expect(t, "editor.run.name", er.name, "nvim");
    er = editorrun(&a, "code", "/opt/hx {file}", "/f", 1, 0, 0);
    expect(t, "editor.run.tmplname", er.name, "hx");

    // Which editor a host runs: the window's, its template, $VISUAL,
    // $EDITOR, vi, each as written and then by its file name
    struct {
        Str    name, cmd, tmpl, visual, editor;
        EjHost host;
        Str    want;
    } picks[] = {
        {"window", "/usr/bin/nvim -p", {}, "nano", {}, {{"/usr/bin/nvim", "nano"}, {}},
         "cmd:/usr/bin/nvim -p"},
        {"rebased", "/opt/homebrew/bin/nvim -p", {}, "nano", {}, {{"nano", "nvim"}, {}},
         "cmd:nvim -p"},
        {"tmpl", "vim", "/opt/x/hx {file}:{line}", {}, {}, {{"vim", "hx"}, {}},
         "tmpl:hx {file}:{line}"},
        {"visual", "nvim", {}, " nano ", "micro", {{"nano", "micro", "vi"}, {}}, "cmd:nano"},
        {"editor", "nvim", {}, "nano", "micro -x", {{"micro", "vi"}, {}}, "cmd:micro -x"},
        {"vi", {}, {}, {}, {}, {{"vi"}, {}}, "cmd:vi"},
        {"none", "nvim", {}, "nano", {}, {{"ed"}, {}}, "none"},
        {"busybox", {}, {}, {}, {}, {{"vi"}, "vi"}, "cmd:vi nojump"},
        {"busyboxpath", "/bin/vi", {}, {}, {}, {{"/bin/vi"}, "/bin/vi"}, "cmd:/bin/vi nojump"},
    };
    for (iz i = 0; i < countof(picks); i++) {
        EditPick p = editpick(&a, picks[i].cmd, picks[i].tmpl, picks[i].visual,
                              picks[i].editor, ejfind, &picks[i].host);
        Str got = p.tmpl.len ? concat(&a, "tmpl:", p.tmpl) :
                  p.cmd.len  ? concat(&a, "cmd:", p.cmd)   : Str("none");
        if (p.nojump) {
            got = concat(&a, got, " nojump");
        }
        expect(t, concat(&a, "editor.pick.", picks[i].name), got, picks[i].want);
    }

    // The login shell's flags, by $SHELL's file name
    Str self = "'/opt/p/prospero' --tty-edit-in";
    Str shs[] = {"sh", "bash", "zsh", "dash", "ksh", "mksh", "ash", "yash", "fish"};
    for (iz i = 0; i < countof(shs); i++) {
        Str sh = concat(&a, "/usr/bin/", shs[i]);
        expect(t, concat(&a, "editor.shell.", shs[i]), ejwords(&a, editshell(&a, sh, self, 1)),
               concat(&a, sh, "|-l|-i|-c|exec '/opt/p/prospero' --tty-edit-in"));
        expect(t, concat(&a, "editor.shell.nologin.", shs[i]),
               ejwords(&a, editshell(&a, sh, self, 0)),
               concat(&a, sh, "|-i|-c|exec '/opt/p/prospero' --tty-edit-in"));
    }
    struct {
        Str name, shell;
        b32 login;
        Str want;
    } shells[] = {
        {"csh", "/bin/csh", 1, "/bin/csh|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"tcsh", "/bin/tcsh", 1, "/bin/tcsh|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"tcshnologin", "/bin/tcsh", 0, "/bin/tcsh|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"pwsh", "/usr/local/bin/pwsh", 1,
         "/usr/local/bin/pwsh|-Login|-NoLogo|-Command|"
         "& '/opt/p/prospero' --tty-edit-in; exit $LASTEXITCODE"},
        {"pwshnologin", "/usr/local/bin/pwsh", 0,
         "/usr/local/bin/pwsh|-NoLogo|-Command|"
         "& '/opt/p/prospero' --tty-edit-in; exit $LASTEXITCODE"},
        {"nu", "/opt/homebrew/bin/nu", 1, "/bin/sh|-l|-i|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"xonsh", "/usr/bin/xonsh", 0, "/bin/sh|-i|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"powershell", "/usr/bin/powershell", 1,
         "/bin/sh|-l|-i|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"relative", "zsh", 1, "/bin/sh|-l|-i|-c|exec '/opt/p/prospero' --tty-edit-in"},
        {"empty", "", 1, "/bin/sh|-l|-i|-c|exec '/opt/p/prospero' --tty-edit-in"},
    };
    for (iz i = 0; i < countof(shells); i++) {
        expect(t, concat(&a, "editor.shell.", shells[i].name),
               ejwords(&a, editshell(&a, shells[i].shell, self, shells[i].login)),
               shells[i].want);
    }

    // A path that means the same in single quotes to every shell kind
    struct {
        Str name, path, want;
    } safes[] = {
        {"plain", "/usr/local/bin/prospero", "1"},
        {"blank", "/Applications/My App/prospero", "1"},
        {"utf8", "/home/jos\xc3\xa9/prospero", "1"},
        {"dollar", "/a$b/`c`/prospero", "1"},
        {"quote", "/a'b/prospero", "0"},
        {"curly", "/a\xe2\x80\x99s/prospero", "0"},
        {"backslash", "/a\\b/prospero", "0"},
        {"bang", "/a!b/prospero", "0"},
        {"newline", "/a\nb/prospero", "0"},
        {"control", "/a\x01/prospero", "0"},
        {"del", "/a\x7f/prospero", "0"},
        {"empty", "", "0"},
    };
    for (iz i = 0; i < countof(safes); i++) {
        expect(t, concat(&a, "editor.shellsafe.", safes[i].name),
               istr(&a, editshellsafe(safes[i].path)), safes[i].want);
    }
}

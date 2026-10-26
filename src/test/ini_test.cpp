// INI reader tests: line syntax, quoting, spans, layering, includes

// Render a file's lines: _ blank, # comment, [kind:name], key=<value>,
// ~key=<value> for a key outside any section, @<include path>, ! invalid.
static Str inishow(Arena *a, IniFile *f)
{
    Buf b(a, 256);
    for (iz i = 0; i < f->lines.len; i++) {
        IniLine *l = &f->lines[i];
        if (i) {
            putbyte(&b, ' ');
        }
        switch (l->type) {
        case INI_BLANK:   putbyte(&b, '_'); break;
        case INI_COMMENT: putbyte(&b, '#'); break;
        case INI_INVALID: putbyte(&b, '!'); break;
        case INI_SECTION:
            putbyte(&b, '[');
            print(&b, l->key);
            putbyte(&b, ':');
            print(&b, l->value);
            putbyte(&b, ']');
            break;
        case INI_KEY:
            if (!l->section) {
                putbyte(&b, '~');
            }
            print(&b, l->key);
            print(&b, "=<");
            print(&b, l->value);
            putbyte(&b, '>');
            break;
        case INI_INCLUDE:
            print(&b, "@<");
            print(&b, l->value);
            putbyte(&b, '>');
            break;
        }
    }
    return finish(&b);
}

// All diagnostics, one per line.
static Str inidiags(Arena *a, Ini *ini)
{
    Buf b(a, 256);
    for (iz i = 0; i < ini->diags.len; i++) {
        if (i) {
            putbyte(&b, '\n');
        }
        print(&b, ini->diags[i]);
    }
    return finish(&b);
}

static Str initext(Arena *a, IniFile *f)
{
    Buf b(a, 256);
    printinifile(&b, f);
    return finish(&b);
}

// Provenance of a key's definition and of each one it overrode.
static Str inihistory(Arena *a, Ini *ini, Str kind, Str name, Str key)
{
    IniEntry *e = inilookup(ini, kind, name, key);
    if (!e) {
        return "undefined";
    }
    Buf b(a, 128);
    for (; e; e = e->older) {
        if (b.len) {
            putbyte(&b, ' ');
        }
        print(&b, e->file->path);
        putbyte(&b, ':');
        print(&b, (i64)e->line->num);
    }
    return finish(&b);
}

static Str inishowvalue(Str v)
{
    return v.data ? v : Str("null");
}

// Section names of a kind in order, or all sections as kind:name.
static Str inishowsections(Arena *a, Ini *ini, Str kind, b32 all)
{
    Buf b(a, 128);
    IniSection *s = all ? ini->sections : inifirst(ini, kind);
    for (; s; s = all ? s->next : ininext(s)) {
        if (b.len) {
            putbyte(&b, ' ');
        }
        if (all) {
            print(&b, s->kind);
            putbyte(&b, ':');
        }
        print(&b, s->name);
    }
    return finish(&b);
}

static Str inishowkeys(Arena *a, IniSection *s)
{
    Buf b(a, 128);
    for (IniEntry *e = s ? s->keys : 0; e; e = e->next) {
        if (b.len) {
            putbyte(&b, ' ');
        }
        print(&b, e->key);
    }
    return finish(&b);
}

// Each file as path, with <-parent:line when included, and its depth.
static Str inishowfiles(Arena *a, Ini *ini)
{
    Buf b(a, 256);
    for (IniFile *f = ini->files; f; f = f->next) {
        if (b.len) {
            putbyte(&b, ' ');
        }
        print(&b, f->path);
        if (f->parent) {
            print(&b, "<-");
            print(&b, f->parent->path);
            putbyte(&b, ':');
            print(&b, (i64)f->via->num);
        }
        putbyte(&b, '@');
        print(&b, (i64)f->depth);
    }
    return finish(&b);
}

// Replace a line's value span with an encoded value.
static Str inisplice(Arena *a, IniLine *l, Str value)
{
    Buf b(a, 64);
    print(&b, takehead(l->raw, l->valbeg));
    printinivalue(&b, value);
    print(&b, cuthead(l->raw, l->valend));
    return finish(&b);
}

static void testiniparse(Test *t)
{
    static struct { Str in, lines, diags; } cases[] = {
        // Blank lines and whole-line comments
        {"", "", ""},
        {"\n", "_", ""},
        {"  \t\r\n", "_", ""},
        {"# c\n; c\n  # c\n\t; c\n#no space\n", "# # # # #", ""},

        // Keys and trailing comments
        {"a = 1\n", "a=<1>", ""},
        {"a=1", "a=<1>", ""},
        {"a = 1 # c", "a=<1>", ""},
        {"a = 1\t#\tc", "a=<1>", ""},
        {"a = 1 #", "a=<1>", ""},
        {"a = 1 #c", "a=<1 #c>", ""},
        {"a = #ff99ff", "a=<#ff99ff>", ""},
        {"a = #ff99ff # pink", "a=<#ff99ff>", ""},
        {"a = x#y # c", "a=<x#y>", ""},
        {"a = http://h/p#frag # c", "a=<http://h/p#frag>", ""},
        {"a = # c", "a=<>", ""},
        {"a =", "a=<>", ""},
        {"a =# c", "a=<# c>", ""},
        {"a = 1 ; c", "a=<1 ; c>", ""},
        {"a = b = c", "a=<b = c>", ""},
        {"  a   =   b  c  ", "a=<b  c>", ""},
        {"a\t=\tb\t", "a=<b>", ""},
        {"a.b-c_d/e = 1", "a.b-c_d/e=<1>", ""},

        // Quoted values
        {"a = \"  x  \"", "a=<  x  >", ""},
        {"a = \"x # y\" # c", "a=<x # y>", ""},
        {"a = \"q\\\"q\"", "a=<q\"q>", ""},
        {"a = \"b\\\\s\"", "a=<b\\s>", ""},
        {"a = \"C:\\dir\\x\"", "a=<C:\\dir\\x>", ""},
        {"a = \"\"", "a=<>", ""},
        {"a = \"x\"\t", "a=<x>", ""},
        {"a = x \"y\"", "a=<x \"y\">", ""},
        {"a = \"x", "!", "1:5: error: unterminated quoted value"},
        {"a = \"x\\\"", "!", "1:5: error: unterminated quoted value"},
        {"a = \"x\" y", "!", "1:9: error: unexpected text after quoted value"},
        {"a = \"x\"#c", "!", "1:8: error: unexpected text after quoted value"},
        {"a = \"x\" #c", "!", "1:9: error: unexpected text after quoted value"},
        {"a = \"x\" ; c", "!",
         "1:9: error: unexpected text after quoted value"},

        // Section headers
        {"[provider local]", "[provider:local]", ""},
        {"[ui]", "[ui:]", ""},
        {"[ role \t explore ]", "[role:explore]", ""},
        {"[model local/qwen-flash]", "[model:local/qwen-flash]", ""},
        {"  [a b] # c", "[a:b]", ""},
        {"[a b];c", "[a:b]", ""},
        {"[a b]#c", "[a:b]", ""},
        {"[a b] junk", "[a:b]",
         "1:7: warning: unexpected text after section header"},
        {"[a b c]", "!",
         "1:6: error: expected [kind] or [kind name], section ignored"},
        {"[]", "!",
         "1:1: error: expected [kind] or [kind name], section ignored"},
        {"[ \t]", "!",
         "1:1: error: expected [kind] or [kind name], section ignored"},
        {"[a [b]", "!",
         "1:4: error: expected [kind] or [kind name], section ignored"},
        {"[[a]]", "!",
         "1:2: error: expected [kind] or [kind name], section ignored"},
        {"[a b", "!",
         "1:1: error: unterminated section header, section ignored"},
        {"[a b c]\nx = 1\n[ok]\ny = 2\n", "! ~x=<1> [ok:] y=<2>",
         "1:6: error: expected [kind] or [kind name], section ignored"},
        {"x = 0\n[a\nx = 1\n\n[]\ny = 2\n[s]\nx = 3",
         "x=<0> ! ~x=<1> _ ! ~y=<2> [s:] x=<3>",
         "2:1: error: unterminated section header, section ignored\n"
         "5:1: error: expected [kind] or [kind name], section ignored"},

        // Malformed lines
        {"junk", "!", "1:1: error: expected key = value"},
        {"  junk here", "!", "1:3: error: expected key = value"},
        {"]", "!", "1:1: error: expected key = value"},
        {"  = v", "!", "1:3: error: missing key before ="},
        {"a b = c", "!", "1:2: error: whitespace in key"},
        {"a\n\"b\"\nc = 1", "! ! c=<1>",
         "1:1: error: expected key = value\n"
         "2:1: error: expected key = value"},

        // Line endings and byte order marks
        {"a = 1\r\nb = 2\r\n", "a=<1> b=<2>", ""},
        {"a = \"x\"\r\n", "a=<x>", ""},
        {"a = 1 #\r\n", "a=<1>", ""},
        {"a = 1\n\nb = 2", "a=<1> _ b=<2>", ""},
        {"a = 1\r", "a=<1>", ""},
        {"\xef\xbb\xbf# c\r\na = 1", "# a=<1>", ""},
        {"\xef\xbb\xbf" "junk", "!", "1:1: error: expected key = value"},
        {"\xef\xbb\xbf", "", ""},

        // Include syntax (not followed without a platform)
        {"include = x.ini", "@<x.ini>", ""},
        {"include = \"a b.ini\" # c", "@<a b.ini>", ""},
        {"a = 1\ninclude = x\nb = 2", "a=<1> @<x> b=<2>", ""},
        {"include =", "!", "1:10: error: missing include path"},
        {"include = # c", "!", "1:11: error: missing include path"},
        {"[s]\ninclude = x", "[s:] !",
         "2:1: error: include must come before the first section"},
        {"[a b c]\ninclude = x", "! !",
         "1:6: error: expected [kind] or [kind name], section ignored\n"
         "2:1: error: include must come before the first section"},
        {"include = https://example.com/t.ini", "@<https://example.com/t.ini>", ""},
        {"include = HTTP://x", "@<HTTP://x>", ""},
        {"include = https://h/t.ini   sha256=0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF # c",
         "@<https://h/t.ini   sha256=0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF>", ""},
        {"include = https://h/t.ini sha256=0123", "!",
         "1:11: error: expected sha256= and 64 hex digits after the URL, "
         "include ignored"},
        {"include = https://h/t.ini md5=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "!",
         "1:11: error: expected sha256= and 64 hex digits after the URL, "
         "include ignored"},
        {"include = https://", "!",
         "1:11: error: expected an http:// or https:// URL, include ignored"},
        {"include = https://user@h/t.ini", "!",
         "1:11: error: expected an http:// or https:// URL, include ignored"},
        {"include = //./pipe/x", "@<//./pipe/x>",
         "1:11: warning: device paths are not supported"},
        {"include = \\\\?\\C:\\x.ini", "@<\\\\?\\C:\\x.ini>",
         "1:11: warning: device paths are not supported"},

        // Duplicate keys within one file
        {"a = 1\na = 2", "a=<1> a=<2>",
         "2:1: warning: duplicate key overrides line 1"},
        {"a = 1\na = 2\na = 3", "a=<1> a=<2> a=<3>",
         "2:1: warning: duplicate key overrides line 1\n"
         "3:1: warning: duplicate key overrides line 2"},
        {"[s]\na = 1\n[t]\na = 1\n[s]\n  a = 2\n",
         "[s:] a=<1> [t:] a=<1> [s:] a=<2>",
         "6:3: warning: duplicate key overrides line 2"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", cases[i].in);
        expect(t, cases[i].in, inishow(&a, f), cases[i].lines);
        expect(t, cases[i].in, inidiags(&a, &ini), cases[i].diags);
        expect(t, cases[i].in, initext(&a, f), cases[i].in);
    }

    Arena a = t->perm;
    {
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", "\xef\xbb\xbf" "a = 1\r\nb = 2");
        expect(t, "bom", istr(&a, f->bom), "1");
        expect(t, "eol.crlf", f->lines[0].eol, "\r\n");
        expect(t, "eol.none", istr(&a, f->lines[1].eol.len), "0");
        expect(t, "line.num", istr(&a, f->lines[1].num), "2");
    }

    // Rendering diagnostics
    struct { IniDiag d; Str want; } diags[] = {
        {{"f.ini", 3, 7, INI_ERROR, "m"}, "f.ini:3:7: error: m"},
        {{"f.ini", 3, 0, INI_WARNING, "m"}, "f.ini:3: warning: m"},
        {{"f.ini", 0, 0, INI_WARNING, "m"}, "f.ini: warning: m"},
        {{{}, 2, 1, INI_ERROR, "m"}, "2:1: error: m"},
        {{{}, 0, 0, INI_WARNING, "m"}, "warning: m"},
    };
    for (iz i = 0; i < countof(diags); i++) {
        Buf b(&a, 64);
        print(&b, diags[i].d);
        expect(t, diags[i].want, finish(&b), diags[i].want);
    }
}

// Splicing a value span with an encoded value changes only that value.
static void testiniedit(Test *t)
{
    static struct { Str in, value, want; } edits[] = {
        {"url = http://x  # local server", "http://y",
         "url = http://y  # local server"},
        {"  url=http://x", "http://y", "  url=http://y"},
        {"\tkey\t=\t\"v\"\t# c", "w", "\tkey\t=\tw\t# c"},
        {"a = \"  x  \" # c", "y", "a = y # c"},
        {"a = 1", "  padded  ", "a = \"  padded  \""},
        {"a = 1 # c", "x # y", "a = \"x # y\" # c"},
        {"a = 1", "#ff99ff", "a = #ff99ff"},
        {"a = 1", "# x", "a = \"# x\""},
        {"a = 1", "x #", "a = \"x #\""},
        {"a = 1", "x #y", "a = x #y"},
        {"a = 1", "\"q\"", "a = \"\\\"q\\\"\""},
        {"a = 1", "C:\\dir\\", "a = C:\\dir\\"},
        {"a = 1", " C:\\dir\\", "a = \" C:\\dir\\\\\""},
        {"a = 1", "", "a = "},
        {"a = 1 # c", "", "a =  # c"},
        {"a =", "x", "a =x"},
        {"a = ", "x", "a = x"},
        {"a = 1\r\n", "2", "a = 2"},
        {"[provider local] # c", "cloud", "[provider cloud] # c"},
        {"include = team.ini # c", "base.ini", "include = base.ini # c"},
    };
    for (iz i = 0; i < countof(edits); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", edits[i].in);
        Str      got = inisplice(&a, &f->lines[0], edits[i].value);
        expect(t, edits[i].in, got, edits[i].want);

        Ini      again = {};
        IniFile *g     = iniparse(&again, &a, "", got);
        expect(t, got, g->lines[0].value, edits[i].value);
    }

    // Every encoded value parses back to itself, with or without a
    // trailing comment or CRLF after it.
    static Str values[] = {
        "", " ", "a", " a", "a ", "\"", "\"a\"", "a\"", "#", "# x", "x #",
        "x # y", "x #y", "#x", "x#", "\\", "a\\", "\\\"", "\\\\", "\\\\\\",
        "C:\\x\\", "a\tb", "\t", "\r", "a\r", "a\rb", "x = y", "[s]", ";",
        " ; ", "\"\\", "\xc3\xbc",
    };
    static Str suffixes[] = {"", " # c", "\r\n", "\t#"};
    i32 bad = 0;
    for (iz i = 0; i < countof(values); i++) {
        for (iz j = 0; j < countof(suffixes); j++) {
            Arena a = t->perm;
            Buf   b(&a, 64);
            print(&b, "k = ");
            printinivalue(&b, values[i]);
            print(&b, suffixes[j]);
            Str      line = finish(&b);
            Ini      ini  = {};
            IniFile *f    = iniparse(&ini, &a, "", line);
            if (f->lines[0].type!=INI_KEY || !(f->lines[0].value==values[i])) {
                expect(t, "encode", line, values[i]);
                bad++;
            }
        }
    }
    Arena a = t->perm;
    expect(t, "encode.roundtrip", istr(&a, bad), "0");

    Buf b(&a, 16);
    expect(t, "encode.newline", istr(&a, printinivalue(&b, "a\nb")), "0");
    expect(t, "encode.nothing", istr(&a, b.len), "0");
}

// Layering texts in memory: overrides, provenance, and iteration.
static void testinilayers(Test *t)
{
    Arena    a    = t->perm;
    Ini      ini  = {};
    IniFile *team = iniparse(&ini, &a, "team.ini",
        "top = 1\n"
        "[provider local]\n"
        "url = http://team\n"
        "key = k\n"
        "[provider cloud]\n"
        "url = https://cloud\n");
    IniFile *mine = iniparse(&ini, &a, "mine.ini",
        "[role explore]\n"
        "model = local/x\n"
        "[provider local]\n"
        "url = http://mine # mine\n"
        "[provider extra]\n"
        "[bad section here]\n"
        "url = ignored\n"
        "[provider cloud]\n"
        "empty =\n");

    // Each line's section, by line number, as kind:name or - for none
    {
        Buf b(&a, 128);
        for (iz i = 0; i < mine->lines.len; i++) {
            IniSection *s = mine->lines[i].section;
            print(&b, i ? Str(" ") : Str(""));
            print(&b, (i64)i+1);
            putbyte(&b, '=');
            if (!s) {
                putbyte(&b, '-');
            } else {
                print(&b, s->kind);
                putbyte(&b, ':');
                print(&b, s->name);
            }
        }
        expect(t, "layer.linesections", finish(&b),
               "1=role:explore 2=role:explore 3=provider:local "
               "4=provider:local 5=provider:extra 6=- 7=- 8=provider:cloud "
               "9=provider:cloud");
        expect(t, "layer.topsection",
               istr(&a, team->lines[0].section==inisection(&ini, "", "")),
               "1");
    }

    IniEntry *url = inilookup(&ini, "provider", "local", "url");
    expect(t, "layer.value", url->value, "http://mine");
    expect(t, "layer.history", inihistory(&a, &ini, "provider", "local", "url"),
           "mine.ini:4 team.ini:3");
    expect(t, "layer.older", url->older->value, "http://team");
    expect(t, "layer.raw", url->line->raw, "url = http://mine # mine");
    expect(t, "layer.kept", inihistory(&a, &ini, "provider", "local", "key"),
           "team.ini:4");
    expect(t, "layer.top", iniget(&ini, "", "", "top"), "1");
    expect(t, "layer.empty",
           inishowvalue(iniget(&ini, "provider", "cloud", "empty")), "");
    expect(t, "layer.nosection",
           inishowvalue(iniget(&ini, "provider", "nope", "url")), "null");
    expect(t, "layer.nokey",
           inishowvalue(iniget(&ini, "provider", "local", "nope")), "null");
    expect(t, "layer.ignored",
           inishowvalue(iniget(&ini, "provider", "extra", "url")), "null");
    expect(t, "layer.badsection",
           istr(&a, !inisection(&ini, "bad", "section")), "1");
    expect(t, "layer.providers",
           inishowsections(&a, &ini, "provider", 0), "local cloud extra");
    expect(t, "layer.roles", inishowsections(&a, &ini, "role", 0), "explore");
    expect(t, "layer.none", inishowsections(&a, &ini, "model", 0), "");
    expect(t, "layer.all", inishowsections(&a, &ini, "", 1),
           ": provider:local provider:cloud role:explore provider:extra");
    expect(t, "layer.keys",
           inishowkeys(&a, inisection(&ini, "provider", "local")), "url key");
    expect(t, "layer.keys2",
           inishowkeys(&a, inisection(&ini, "provider", "cloud")), "url empty");
    expect(t, "layer.nokeys",
           inishowkeys(&a, inisection(&ini, "provider", "extra")), "");
    expect(t, "layer.diags", inidiags(&a, &ini),
           "mine.ini:6:14: error: "
           "expected [kind] or [kind name], section ignored");
}

static void testinipaths(Test *t)
{
    Arena a = t->perm;

    static struct { Str dir, name, want; } paths[] = {
        {"", "a.ini", "a.ini"},
        {"/cfg", "team.ini", "/cfg/team.ini"},
        {"/cfg/", "team.ini", "/cfg/team.ini"},
        {"/cfg", "./team.ini", "/cfg/team.ini"},
        {"/cfg", "../x/./y//z.ini", "/x/y/z.ini"},
        {"/", "x.ini", "/x.ini"},
        {"/cfg", "/etc/x.ini", "/etc/x.ini"},
        {"/cfg", "../../../x.ini", "/x.ini"},
        {"cfg", "../../x.ini", "../x.ini"},
        {"", "../a/../b.ini", "../b.ini"},
        {"", "./x//y/", "x/y"},
        {"a/b", "..", "a"},
        {"a", "..", ""},
        {"C:\\cfg", "t.ini", "C:/cfg/t.ini"},
        {"C:\\cfg", "..\\..\\t.ini", "C:/t.ini"},
        {"", "c:\\x\\..\\..\\t.ini", "c:/t.ini"},
        {"C:\\cfg", "D:\\t.ini", "D:/t.ini"},
        {"\\\\srv\\share", "t.ini", "//srv/share/t.ini"},
        {"/cfg", "\\\\srv\\s\\t.ini", "//srv/s/t.ini"},

        // A drive needs a slash to be a root
        {"C:\\cfg", "D:t.ini", "C:/cfg/D:t.ini"},
        {"/cfg", "a:b.ini", "/cfg/a:b.ini"},
        {"", "C:h.ini", "C:h.ini"},
        {"", "C:", "C:"},
        {"x", "C:..\\..\\t.ini", "x/t.ini"},
        {"", "1:/t.ini", "1:/t.ini"},
    };
    for (iz i = 0; i < countof(paths); i++) {
        Str got = inipath(&a, paths[i].dir, paths[i].name);
        expect(t, paths[i].want, got, paths[i].want);
    }

    static struct { Str path, want; } dirs[] = {
        {"/cfg/h.ini", "/cfg"},
        {"h.ini", ""},
        {"/h.ini", "/"},
        {"C:\\h.ini", "C:\\"},
        {"C:\\cfg\\h.ini", "C:\\cfg"},
        {"C:h.ini", ""},
        {"C:x/h.ini", "C:x"},
        {"//srv/share/h.ini", "//srv/share"},
        {"a\\b/c", "a\\b"},
    };
    for (iz i = 0; i < countof(dirs); i++) {
        expect(t, dirs[i].path, inidir(dirs[i].path), dirs[i].want);
    }

    static struct { Str path; i32 want; } devices[] = {
        {"//./pipe/x", 1},
        {"\\\\.\\COM1", 1},
        {"\\\\?\\C:\\x.ini", 1},
        {"//?/UNC/srv/s/x", 1},
        {"\\/.\\x", 1},
        {"//srv/x", 0},
        {"//.x/y", 0},
        {"//.", 0},
        {"/./x", 0},
        {"./x", 0},
        {"", 0},
    };
    for (iz i = 0; i < countof(devices); i++) {
        expect(t, devices[i].path, istr(&a, inidevice(devices[i].path)),
               istr(&a, devices[i].want));
    }
}

// Includes through the test platform's in-memory file system.
static void testiniinclude(Test *t)
{
    Arena a  = t->perm;
    Os   *os = newos(&a, 8<<20);

    // Layers: each include lands where its line is
    os_write(os, a, "/cfg/prospero.ini",
        "include = team.ini\n"
        "include = ../shared/base.ini # base\n"
        "top = mine\n"
        "[provider local]\n"
        "url = http://mine\n");
    os_write(os, a, "/cfg/team.ini",
        "include = \"../shared/base.ini\"\n"
        "top = team\n"
        "[provider local]\n"
        "url = http://team\n"
        "key = team-key\n"
        "[provider team]\n"
        "url = http://teamserver\n");
    os_write(os, a, "/shared/base.ini",
        "[provider local]\n"
        "url = http://base\n"
        "model = base-model\n");
    {
        Ini      ini = {};
        IniFile *f   = iniload(&ini, os, &a, "/cfg/prospero.ini");
        expect(t, "inc.root", inishow(&a, f),
               "@<team.ini> @<../shared/base.ini> "
               "top=<mine> [provider:local] url=<http://mine>");
        expect(t, "inc.diags", inidiags(&a, &ini), "");
        expect(t, "inc.files", inishowfiles(&a, &ini),
               "/cfg/prospero.ini@0 "
               "/cfg/team.ini<-/cfg/prospero.ini:1@1 "
               "/shared/base.ini<-/cfg/team.ini:1@2 "
               "/shared/base.ini<-/cfg/prospero.ini:2@1");
        expect(t, "inc.url", inihistory(&a, &ini, "provider", "local", "url"),
               "/cfg/prospero.ini:5 /shared/base.ini:2 /cfg/team.ini:4 "
               "/shared/base.ini:2");
        expect(t, "inc.model",
               inihistory(&a, &ini, "provider", "local", "model"),
               "/shared/base.ini:3 /shared/base.ini:3");
        expect(t, "inc.key", inihistory(&a, &ini, "provider", "local", "key"),
               "/cfg/team.ini:5");
        expect(t, "inc.top", inihistory(&a, &ini, "", "", "top"),
               "/cfg/prospero.ini:3 /cfg/team.ini:2");
        expect(t, "inc.value", iniget(&ini, "provider", "team", "url"),
               "http://teamserver");
        expect(t, "inc.sections", inishowsections(&a, &ini, "", 1),
               "provider:local : provider:team");
    }

    // A file included twice reports each problem once
    os_write(os, a, "/g/root.ini", "include = team.ini\ninclude = base.ini\n");
    os_write(os, a, "/g/team.ini", "include = base.ini\n");
    os_write(os, a, "/g/base.ini", "include = gone.ini\njunk\nk = 1\nk = 2\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "/g/root.ini");
        expect(t, "diamond.files", inishowfiles(&a, &ini),
               "/g/root.ini@0 /g/team.ini<-/g/root.ini:1@1 "
               "/g/base.ini<-/g/team.ini:1@2 /g/base.ini<-/g/root.ini:2@1");
        expect(t, "diamond.diags", inidiags(&a, &ini),
               "/g/base.ini:1:11: warning: "
               "cannot read include /g/gone.ini (not found)\n"
               "/g/base.ini:2:1: error: expected key = value\n"
               "/g/base.ini:4:1: warning: duplicate key overrides line 3");
        expect(t, "diamond.k", inihistory(&a, &ini, "", "", "k"),
               "/g/base.ini:4 /g/base.ini:3 /g/base.ini:4 /g/base.ini:3");
    }

    // Different problems from each copy are all kept
    os_write(os, a, "/h/root.ini", "include = team.ini\ninclude = base.ini\n");
    os_write(os, a, "/h/team.ini", "include = base.ini\n");
    os_write(os, a, "/h/base.ini", "include = team.ini\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "/h/root.ini");
        expect(t, "diamond2.files", inishowfiles(&a, &ini),
               "/h/root.ini@0 /h/team.ini<-/h/root.ini:1@1 "
               "/h/base.ini<-/h/team.ini:1@2 /h/base.ini<-/h/root.ini:2@1 "
               "/h/team.ini<-/h/base.ini:1@2");
        expect(t, "diamond2.diags", inidiags(&a, &ini),
               "/h/base.ini:1:11: warning: include cycle: /h/team.ini\n"
               "/h/team.ini:1:11: warning: include cycle: /h/base.ini");
    }

    // Byte order marks and CRLF in an included file
    Str bomroot = "\xef\xbb\xbf" "include = sub.ini\r\nafter = 1\r\n";
    Str bomsub  = "\xef\xbb\xbf" "# sub\r\n[s]\r\nk = \"v\" # c\r\nk = w";
    os_write(os, a, "/e/root.ini", bomroot);
    os_write(os, a, "/e/sub.ini", bomsub);
    {
        Ini      ini  = {};
        IniFile *root = iniload(&ini, os, &a, "/e/root.ini");
        IniFile *sub  = root->next;
        expect(t, "bominc.diags", inidiags(&a, &ini),
               "/e/sub.ini:4:1: warning: duplicate key overrides line 3");
        expect(t, "bominc.root", inishow(&a, root), "@<sub.ini> after=<1>");
        expect(t, "bominc.sub", inishow(&a, sub), "# [s:] k=<v> k=<w>");
        expect(t, "bominc.roottext", initext(&a, root), bomroot);
        expect(t, "bominc.subtext", initext(&a, sub), bomsub);
        expect(t, "bominc.bom", istr(&a, root->bom && sub->bom), "1");
        expect(t, "bominc.k", inihistory(&a, &ini, "s", "", "k"),
               "/e/sub.ini:4 /e/sub.ini:3");
        expect(t, "bominc.after", iniget(&ini, "", "", "after"), "1");
    }

    // Cycles, including a file including itself
    os_write(os, a, "/c/a.ini", "include = b.ini\nx = a\n");
    os_write(os, a, "/c/b.ini",
        "include = ./a.ini\n"
        "include = ../c/b.ini\n"
        "x = b\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "/c/a.ini");
        expect(t, "cycle.diags", inidiags(&a, &ini),
               "/c/b.ini:1:11: warning: include cycle: /c/a.ini\n"
               "/c/b.ini:2:11: warning: include cycle: /c/b.ini");
        expect(t, "cycle.x", inihistory(&a, &ini, "", "", "x"),
               "/c/a.ini:2 /c/b.ini:3");
    }

    // Unreadable includes are skipped with a warning
    os_write(os, a, "/m/root.ini",
        "include = nope.ini\n"
        "include = dir\n"
        "include = https://example.com/x.ini\n"
        "x = 1\n");
    os_mkdirs(os, a, "/m/dir");
    {
        Ini      ini = {};
        IniFile *f   = iniload(&ini, os, &a, "/m/root.ini");
        expect(t, "missing.diags", inidiags(&a, &ini),
               "/m/root.ini:1:11: warning: "
               "cannot read include /m/nope.ini (not found)\n"
               "/m/root.ini:2:11: warning: "
               "cannot read include /m/dir (not a file)\n"
               "/m/root.ini:3:11: warning: URL includes are not read here");
        expect(t, "missing.lines", inishow(&a, f),
               "@<nope.ini> @<dir> @<https://example.com/x.ini> x=<1>");
        expect(t, "missing.x", iniget(&ini, "", "", "x"), "1");
    }
    {
        Ini      ini = {};
        IniFile *f   = iniload(&ini, os, &a, "/m/none.ini");
        expect(t, "noroot.file", istr(&a, !f), "1");
        expect(t, "noroot.diags", inidiags(&a, &ini),
               "/m/none.ini: warning: cannot read file (not found)");
        expect(t, "noroot.files", istr(&a, ini.nfiles), "0");
    }

    // Nesting deeper than the limit
    for (i32 i = 0; i < 10; i++) {
        Arena tmp = a;
        Buf path(&tmp, 16);
        print(&path, "/d/");
        print(&path, (i64)i);
        print(&path, ".ini");
        Buf text(&tmp, 64);
        print(&text, "include = ");
        print(&text, (i64)i+1);
        print(&text, ".ini\nlevel = ");
        print(&text, (i64)i);
        putbyte(&text, '\n');
        os_write(os, tmp, finish(&path), finish(&text));
    }
    {
        Ini ini = {};
        iniload(&ini, os, &a, "/d/0.ini");
        expect(t, "depth.diags", inidiags(&a, &ini),
               "/d/8.ini:1:11: warning: includes nested deeper than 8");
        expect(t, "depth.files", istr(&a, ini.nfiles), "9");
        expect(t, "depth.level", inihistory(&a, &ini, "", "", "level"),
               "/d/0.ini:2 /d/1.ini:2 /d/2.ini:2 /d/3.ini:2 /d/4.ini:2 "
               "/d/5.ini:2 /d/6.ini:2 /d/7.ini:2 /d/8.ini:2");
    }

    // Too many files, even without cycles
    {
        Arena tmp = a;
        Buf   text(&tmp, 1<<12);
        for (i32 i = 0; i < 70; i++) {
            print(&text, "include = leaf.ini\n");
        }
        os_write(os, tmp, "/f/root.ini", finish(&text));
        os_write(os, tmp, "/f/leaf.ini", "n = 1\n");

        Ini ini = {};
        iniload(&ini, os, &a, "/f/root.ini");
        Buf first(&a, 64);
        if (ini.diags.len) {
            print(&first, ini.diags[0]);
        }
        expect(t, "many.files", istr(&a, ini.nfiles), "64");
        expect(t, "many.count", istr(&a, ini.diags.len), "7");
        expect(t, "many.first", finish(&first),
               "/f/root.ini:64:11: warning: more than 64 configuration files");
    }

    // The size budget covers all files together
    {
        Str root = "include = big.ini\n";
        os_write(os, a, "/b/root.ini", root);
        for (i32 over = 0; over <= 1; over++) {
            Arena tmp = a;
            iz    len = INI_MAXBYTES - root.len + over;
            Str   big = {allocbytes(&tmp, len), len};
            for (iz i = 0; i < len; i++) {
                big[i] = '#';
            }
            os_write(os, tmp, "/b/big.ini", big);

            Ini ini = {};
            iniload(&ini, os, &tmp, "/b/root.ini");
            expect(t, "budget.files", istr(&tmp, ini.nfiles), over ? "1" : "2");
            expect(t, "budget.diags", inidiags(&tmp, &ini), over ?
                   Str("/b/root.ini:1:11: warning: cannot read include "
                       "/b/big.ini (over the 1 MiB configuration limit)") :
                   Str(""));
        }
    }

    // Roots count against the same budget
    {
        Arena tmp = a;
        iz    len = INI_MAXBYTES + 1;
        Str   big = {allocbytes(&tmp, len), len};
        for (iz i = 0; i < len; i++) {
            big[i] = '#';
        }
        os_write(os, tmp, "/r/big.ini", big);
        os_write(os, tmp, "/r/full.ini", cuttail(big, 1));
        os_write(os, tmp, "/r/empty.ini", "");
        os_write(os, tmp, "/r/one.ini", "x = 1\n");

        Ini ini = {};
        iniload(&ini, os, &tmp, "/r/big.ini");
        expect(t, "rootbudget.over", inidiags(&tmp, &ini),
               "/r/big.ini: warning: "
               "cannot read file (over the 1 MiB configuration limit)");
        expect(t, "rootbudget.none", istr(&tmp, ini.nfiles), "0");

        ini = {};
        iniload(&ini, os, &tmp, "/r/full.ini");
        iniload(&ini, os, &tmp, "/r/empty.ini");
        iniload(&ini, os, &tmp, "/r/one.ini");
        expect(t, "rootbudget.full", inidiags(&tmp, &ini),
               "/r/one.ini: warning: "
               "cannot read file (over the 1 MiB configuration limit)");
        expect(t, "rootbudget.files", istr(&tmp, ini.nfiles), "2");
        expect(t, "rootbudget.bytes", istr(&tmp, ini.nbytes),
               istr(&tmp, INI_MAXBYTES));
    }

    // Windows paths: drives, backslashes, UNC
    os_write(os, a, "C:/cfg/h.ini",
        "include = ..\\shared\\t.ini\n"
        "include = D:\\x.ini\n"
        "include = \\\\srv\\share\\u.ini\n");
    os_write(os, a, "C:/shared/t.ini", "a = 1\n");
    os_write(os, a, "D:/x.ini", "b = 2\n");
    os_write(os, a, "//srv/share/u.ini", "c = 3\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "C:\\cfg\\h.ini");
        expect(t, "win.diags", inidiags(&a, &ini), "");
        expect(t, "win.files", inishowfiles(&a, &ini),
               "C:/cfg/h.ini@0 "
               "C:/shared/t.ini<-C:/cfg/h.ini:1@1 "
               "D:/x.ini<-C:/cfg/h.ini:2@1 "
               "//srv/share/u.ini<-C:/cfg/h.ini:3@1");
        expect(t, "win.c", iniget(&ini, "", "", "c"), "3");
    }

    // A drive needs a slash to start a path, and device paths are refused
    os_write(os, a, "/w/h.ini",
        "include = a:b.ini\n"
        "include = D:t.ini\n"
        "include = \\\\.\\pipe\\x\n"
        "include = //?/C:/w/../x.ini\n");
    os_write(os, a, "/w/a:b.ini", "colon = 1\n");
    os_write(os, a, "D:t.ini", "drive = 1\n");
    {
        Ini      ini = {};
        IniFile *f   = iniload(&ini, os, &a, "/w/h.ini");
        expect(t, "names.lines", inishow(&a, f),
               "@<a:b.ini> @<D:t.ini> @<\\\\.\\pipe\\x> @<//?/C:/w/../x.ini>");
        expect(t, "names.files", inishowfiles(&a, &ini),
               "/w/h.ini@0 /w/a:b.ini<-/w/h.ini:1@1");
        expect(t, "names.diags", inidiags(&a, &ini),
               "/w/h.ini:2:11: warning: "
               "cannot read include /w/D:t.ini (not found)\n"
               "/w/h.ini:3:11: warning: device paths are not supported\n"
               "/w/h.ini:4:11: warning: device paths are not supported");
        expect(t, "names.colon", iniget(&ini, "", "", "colon"), "1");
    }
    {
        Ini ini = {};
        iniload(&ini, os, &a, "\\\\?\\C:\\cfg\\h.ini");
        expect(t, "device.root", inidiags(&a, &ini),
               "\\\\?\\C:\\cfg\\h.ini: warning: "
               "cannot read file (device paths are not supported)");
        expect(t, "device.files", istr(&a, ini.nfiles), "0");
    }

    // A drive-relative root is a relative name, as documented at iniload
    os_write(os, a, "C:h.ini", "include = t.ini\n");
    os_write(os, a, "t.ini", "x = cwd\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "C:h.ini");
        expect(t, "driverel.files", inishowfiles(&a, &ini),
               "C:h.ini@0 t.ini<-C:h.ini:1@1");
    }

    // An included file's sections do not leak back into the includer
    os_write(os, a, "/s/root.ini",
        "include = sub.ini\n"
        "after = 1\n"
        "[x]\n"
        "k = 1\n");
    os_write(os, a, "/s/sub.ini",
        "[y]\n"
        "include = root.ini\n"
        "k = 2\n");
    {
        Ini ini = {};
        iniload(&ini, os, &a, "/s/root.ini");
        expect(t, "leak.diags", inidiags(&a, &ini),
               "/s/sub.ini:2:1: error: "
               "include must come before the first section");
        expect(t, "leak.after", iniget(&ini, "", "", "after"), "1");
        expect(t, "leak.y", inishowkeys(&a, inisection(&ini, "y", "")), "k");
        expect(t, "leak.x", iniget(&ini, "x", "", "k"), "1");
    }
}

// A flood of problems in one file stops at INI_MAXDIAGS, other files
// still report, and dropped messages are never formatted.
static void testinicap(Test *t)
{
    Arena a  = t->perm;
    Os   *os = newos(&a, 1<<16);

    static struct { Str unit; iz count; iz ndiags; Str last; } floods[] = {
        {"x\n", 150, 101,
         "f:101: warning: too many diagnostics, rest suppressed"},
        {"k=\n", 150, 101,
         "f:102: warning: too many diagnostics, rest suppressed"},
        {"x\n", 100, 100, "f:100:1: error: expected key = value"},
        {"[a b c]\nk=\n", 150, 101,
         "f:201: warning: too many diagnostics, rest suppressed"},
    };
    for (iz i = 0; i < countof(floods); i++) {
        Arena tmp = a;
        Buf   b(&tmp, 256);
        for (iz n = 0; n < floods[i].count; n++) {
            print(&b, floods[i].unit);
        }
        Ini ini = {};
        iniparse(&ini, &tmp, "f", finish(&b));
        Buf last(&tmp, 64);
        print(&last, ini.diags[ini.diags.len-1]);
        expect(t, floods[i].last, finish(&last), floods[i].last);
        expect(t, floods[i].last, istr(&tmp, ini.diags.len),
               istr(&tmp, floods[i].ndiags));
    }

    // Each file has its own limit, which holds for include problems too
    {
        Arena tmp = a;
        Buf   junk(&tmp, 256);
        Buf   gone(&tmp, 256);
        for (i32 i = 0; i < 150; i++) {
            print(&junk, "x\n");
            print(&gone, "include = gone.ini\n");
        }
        os_write(os, tmp, "/cap/sub.ini", finish(&junk));
        os_write(os, tmp, "/cap/includes.ini", finish(&gone));
        os_write(os, tmp, "/cap/root.ini",
            "include = sub.ini\n"
            "include = ./sub.ini\n"
            "include = includes.ini\n"
            "x\n");

        Ini ini = {};
        iniload(&ini, os, &tmp, "/cap/root.ini");
        expect(t, "cap.count", istr(&tmp, ini.diags.len), "203");
        Str want[] = {
            "/cap/sub.ini:101: warning: too many diagnostics, rest suppressed",
            "/cap/includes.ini:1:11: warning: "
            "cannot read include /cap/gone.ini (not found)",
            "/cap/includes.ini:101: warning: "
            "too many diagnostics, rest suppressed",
            "/cap/root.ini:4:1: error: expected key = value",
        };
        iz at[] = {100, 101, 201, 202};
        for (iz i = 0; i < countof(at); i++) {
            Buf d(&tmp, 64);
            if (at[i] < ini.diags.len) {
                print(&d, ini.diags[at[i]]);
            }
            expect(t, want[i], finish(&d), want[i]);
        }
    }

    // Per line, a muted flood costs its line and the overridden entry
    {
        Arena tmp = a;
        iz    n   = 10000;
        Buf   b(&tmp, 3*n);
        for (iz i = 0; i < n; i++) {
            print(&b, "k=\n");
        }
        Str   src    = finish(&b);
        Arena before = tmp;
        Ini   ini    = {};
        iniparse(&ini, &tmp, "f", src);
        iz used  = tmp.beg - before.beg;
        iz limit = n*(iz)(sizeof(IniLine) + sizeof(IniEntry)) + (64<<10);
        expect(t, "cap.memory", istr(&tmp, used<=limit), "1");
    }
}

static b32 inisamediag(IniDiag a, IniDiag b)
{
    return a.file==b.file && a.line==b.line && a.col==b.col &&
           a.severity==b.severity && a.msg==b.msg;
}

// Invariants on one parsed file: the text round-trips, spans fit their
// lines and match the decoded parts, re-encoding a value round-trips,
// each line has the right section, and diagnostics are distinct and
// point into the file.
static b32 inicheck(Arena a, Ini *ini, IniFile *f, Str src)
{
    if (!(initext(&a, f) == src)) {
        return 0;
    }
    b32 orphan = 0;  // under an invalid section header
    for (iz i = 0; i < f->lines.len; i++) {
        IniLine *l = &f->lines[i];
        Str      r = l->raw;
        if (l->num != i+1) {
            return 0;
        }

        Str lead = trimleft(r);
        if (l->type == INI_SECTION) {
            orphan = 0;
            IniSection *s = l->section;
            if (!s || !(s->kind==l->key) || !(s->name==l->value)) {
                return 0;
            }
        } else if (l->type==INI_INVALID && lead.len && lead[0]=='[') {
            orphan = 1;
            if (l->section) {
                return 0;
            }
        } else if (l->type == INI_KEY) {
            if (orphan != !l->section) {
                return 0;
            }
            b32 found = !l->section;
            IniEntry *e = l->section ? inientry(l->section, l->key) : 0;
            for (; e; e = e->older) {
                found |= e->line == l;
            }
            if (!found) {
                return 0;
            }
        } else if (l->section) {
            return 0;
        }

        if (l->type!=INI_KEY && l->type!=INI_INCLUDE && l->type!=INI_SECTION) {
            continue;
        }
        if (l->keybeg<0 || l->keybeg>=l->keyend || l->keyend>l->valbeg ||
                l->valbeg>l->valend || l->valend>r.len) {
            return 0;
        }
        Str key = span(r.data+l->keybeg, r.data+l->keyend);
        Str val = span(r.data+l->valbeg, r.data+l->valend);
        if (!(key == l->key) || !l->value.data) {
            return 0;
        }
        b32 quoted = l->type!=INI_SECTION && val.len && val[0]=='"';
        if (quoted ? val.len<2 || val[val.len-1]!='"' : !(val == l->value)) {
            return 0;
        }
        if (l->type != INI_SECTION) {
            // Not as line 1, where a stray BOM in the key would be skipped
            Str      line  = concat(&a, "#\n", inisplice(&a, l, l->value));
            Ini      again = {};
            IniFile *g     = iniparse(&again, &a, "", line);
            if (g->lines.len!=2 || !(g->lines[1].value == l->value)) {
                return 0;
            }
        }
    }
    for (iz i = 0; i < ini->diags.len; i++) {
        IniDiag d = ini->diags[i];
        for (iz j = 0; j < i; j++) {
            if (inisamediag(d, ini->diags[j])) {
                return 0;
            }
        }
        if (d.line<1 || d.line>f->lines.len) {
            return 0;
        }
        if (d.col<1 || d.col>f->lines[d.line-1].raw.len+1) {
            return 0;
        }
    }
    return 1;
}

// Random texts assembled from syntax fragments.
static void testinirandom(Test *t)
{
    static Str pieces[] = {
        "a", "key", "include", "x y", " ", "  ", "\t", "=", " = ", "#", " # ",
        ";", "[", "]", "[k n]", "\"", "\\", "\\\"", "\r", "\n", "\n", "\r\n",
        "\xef\xbb\xbf", "\xc3\xbc", "/", "..", "http://",
    };
    u64 rng = 1;
    i32 bad = 0;
    for (i32 n = 0; n < 20000; n++) {
        Arena a = t->perm;
        Buf   b(&a, 256);
        rng = rng*0x9e3779b97f4a7c15 + 1;
        i32 count = (i32)(rng >> 59);
        for (i32 i = 0; i < count; i++) {
            rng = rng*0x9e3779b97f4a7c15 + 1;
            print(&b, pieces[(iz)(rng>>33) % countof(pieces)]);
        }
        Str      src = finish(&b);
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", src);
        if (!inicheck(a, &ini, f, src)) {
            if (!bad++) {
                expect(t, "random input passes checks", src, "");
            }
        }
    }
    Arena a = t->perm;
    expect(t, "random.failures", istr(&a, bad), "0");
}

static iz inirand(u64 *s)
{
    *s = *s*0x9e3779b97f4a7c15 + 1;
    return (iz)(*s >> 33);
}

static b32 iniowns(IniFile *f, IniLine *l)
{
    for (iz i = 0; i < f->lines.len; i++) {
        if (&f->lines[i] == l) {
            return 1;
        }
    }
    return 0;
}

// Invariants of a loaded Ini, as the name of the first one violated, or
// empty: each file reproduces its text and hangs from an include line in
// its parent with no ancestor of the same path, the limits hold (a root
// may open past INI_MAXFILES, an include never), every section and key
// is indexed, each definition is a key line of its file and section,
// every defined line is among the definitions, and the diagnostics are
// distinct and point into their files.
static Str inigraphcheck(Arena a, Ini *ini, Os *os)
{
    i32 nfiles = 0;
    iz  nbytes = 0;
    iz  nlines = 0;  // defined key lines
    for (IniFile *f = ini->files; f; f = f->next) {
        Read r = os_read(os, &a, f->path, INI_MAXBYTES);
        if (r.err || !(initext(&a, f) == r.data)) return "text";
        nfiles++;
        nbytes += r.data.len;
        for (iz i = 0; i < f->lines.len; i++) {
            nlines += f->lines[i].type==INI_KEY && f->lines[i].section;
        }

        if (!f->parent) {
            if (f->depth || f->via) return "root";
            continue;
        }
        if (nfiles > INI_MAXFILES) return "maxfiles";
        if (f->depth != f->parent->depth+1) return "depth";
        if (f->depth > INI_MAXDEPTH) return "maxdepth";
        if (!iniowns(f->parent, f->via)) return "via";
        if (f->via->type != INI_INCLUDE) return "via type";
        for (IniFile *p = f->parent; p; p = p->parent) {
            if (p->path == f->path) return "cycle";
        }
    }
    if (nfiles != ini->nfiles) return "nfiles";
    if (nbytes!=ini->nbytes || nbytes>INI_MAXBYTES) return "nbytes";

    iz ndefs = 0;
    for (IniSection *s = ini->sections; s; s = s->next) {
        if (inisection(ini, s->kind, s->name) != s) return "section";
        for (IniEntry *e = s->keys; e; e = e->next) {
            if (inientry(s, e->key) != e) return "entry";
            for (IniEntry *d = e; d; d = d->older) {
                IniLine *l = d->line;
                if (!iniowns(d->file, l)) return "definition line";
                if (l->type!=INI_KEY || l->section!=s) return "definition";
                if (!(l->key==e->key) || !(l->value==d->value)) {
                    return "definition text";
                }
                ndefs++;
            }
        }
    }
    if (ndefs != nlines) return "definitions";

    for (iz i = 0; i < ini->diags.len; i++) {
        IniDiag d = ini->diags[i];
        for (iz j = 0; j < i; j++) {
            if (inisamediag(d, ini->diags[j])) return "repeat";
        }
        if (!d.line) {
            continue;  // about a whole file
        }
        IniFile *f = ini->files;
        for (; f && !(f->path == d.file); f = f->next) {}
        if (!f || d.line>f->lines.len) return "diag line";
        if (d.col<1 || d.col>f->lines[d.line-1].raw.len+1) return "diag col";
    }
    return "";
}

// Random include graphs in the test file system, loaded from one or two
// roots. Each file opens with a few includes, naming relative, "..",
// absolute, self, missing, directory, device, and URL targets, followed
// by random fragments, which may include again after a section.
static void testinigraph(Test *t)
{
    static Str paths[] = {
        "/x/a.ini", "/x/b.ini", "/x/c.ini", "/y/a.ini", "/y/b.ini",
        "/y/c.ini", "/z/a.ini", "/z/b.ini", "/z/c.ini", "/x/d",
    };
    static Str targets[] = {
        "a.ini", "b.ini", "c.ini", "./a.ini", "../x/c.ini", "../y/a.ini",
        "../z/b.ini", "/x/c.ini", "/x/../y/b.ini", "\\z\\c.ini", "d",
        "/x/d", "..", "/y", "nope.ini", "http://h/x", "//./x/d", "a:b.ini",
    };
    static Str pieces[] = {
        "k = 1\n", "j = \"2\"\r\n", "k = \"q\n", "[s]\n", "[s t]\n",
        "[a b c]\n", "# c\n", "\n", "junk\n", "k = 3", "include = ",
    };
    u64 rng      = 1;
    i32 bad      = 0;
    i32 diamonds = 0;  // graphs where some file was opened twice
    i32 full     = 0;  // graphs that reached INI_MAXFILES
    i32 deep     = 0;  // graphs that reached INI_MAXDEPTH
    for (i32 n = 0; n < 4000; n++) {
        Arena a  = t->perm;
        Os   *os = newos(&a, 1<<14);
        for (iz i = 0; i < countof(paths); i++) {
            Buf b(&a, 256);
            if (!(inirand(&rng) % 8)) {
                print(&b, "\xef\xbb\xbf");
            }
            iz nincludes = inirand(&rng) % 5;
            iz count     = nincludes + inirand(&rng)%6;
            for (iz k = 0; k < count; k++) {
                Str p = pieces[inirand(&rng) % countof(pieces)];
                p = k<nincludes ? Str("include = ") : p;
                print(&b, p);
                if (p == "include = ") {
                    print(&b, targets[inirand(&rng) % countof(targets)]);
                    putbyte(&b, '\n');
                }
            }
            os_write(os, a, paths[i], finish(&b));
        }
        os_mkdirs(os, a, "/y");

        Ini ini = {};
        iniload(&ini, os, &a, paths[inirand(&rng) % countof(paths)]);
        if (inirand(&rng) % 2) {
            iniload(&ini, os, &a, paths[inirand(&rng) % countof(paths)]);
        }

        Str err = inigraphcheck(a, &ini, os);
        if (err.len && !bad++) {
            expect(t, "include graph invariants", err, "");
        }
        b32 twice = 0;
        b32 limit = 0;
        for (IniFile *f = ini.files; f; f = f->next) {
            limit |= f->depth == INI_MAXDEPTH;
            for (IniFile *g = f->next; g; g = g->next) {
                twice |= f->path == g->path;
            }
        }
        diamonds += twice;
        deep     += limit;
        full     += ini.nfiles >= INI_MAXFILES;
    }
    Arena a = t->perm;
    expect(t, "graph.failures", istr(&a, bad), "0");
    expect(t, "graph.diamonds", istr(&a, diamonds > 1000), "1");
    expect(t, "graph.full", istr(&a, full > 5), "1");
    expect(t, "graph.deep", istr(&a, deep > 5), "1");
}

// The writer: exact output for each kind of edit.
static void testiniwrite(Test *t)
{
    static struct {
        Str in, kind, name, key, value;
        b32 rm;
        Str want;
        iz  line;
        b32 changed;
    } cases[] = {
        // Set in place: only the value span changes
        {"# c\n[endpoint local]\nurl = http://a/  # here\n",
         "endpoint", "local", "url", "http://b/", 0,
         "# c\n[endpoint local]\nurl = http://b/  # here\n", 3, 1},
        {"[s]\n\tk\t=\t\"v\"\t# c\n", "s", "", "k", "w", 0,
         "[s]\n\tk\t=\tw\t# c\n", 2, 1},
        {"[s]\na = \"x\"\n", "s", "", "a", "x", 0, "[s]\na = \"x\"\n", 2, 0},
        {"[s]\na = 1 # c\n", "s", "", "a", "", 0, "[s]\na =  # c\n", 2, 1},
        // An empty value takes one apart from a comment and from "="
        {"[s]\na = # c\n", "s", "", "a", "1", 0, "[s]\na = 1 # c\n", 2, 1},
        {"[s]\na =\t#\n", "s", "", "a", "1", 0, "[s]\na =\t1 #\n", 2, 1},
        {"[s]\na =\n", "s", "", "a", "x", 0, "[s]\na = x\n", 2, 1},
        {"[s]\na=\n", "s", "", "a", "x", 0, "[s]\na=x\n", 2, 1},
        {"a =   # fill in later\n", "", "", "a", "120", 0,
         "a =   120 # fill in later\n", 1, 1},
        {"[s]\na = 1\n", "s", "", "a", "  x # y ", 0,
         "[s]\na = \"  x # y \"\n", 2, 1},
        {"[s]\na = 1\n[t]\n[s]\na = 2\n", "s", "", "a", "3", 0,
         "[s]\na = 1\n[t]\n[s]\na = 3\n", 5, 1},
        {"\xef\xbb\xbf[s]\r\na = 1\r\n", "s", "", "a", "2", 0,
         "\xef\xbb\xbf[s]\r\na = 2\r\n", 2, 1},
        {"x = 1", "", "", "x", "2", 0, "x = 2", 1, 1},

        // Add a key after the section's last key, indented alike
        {"[s]\n  a = 1\n  # note\n\n[t]\n", "s", "", "b", "2", 0,
         "[s]\n  a = 1\n  b = 2\n  # note\n\n[t]\n", 3, 1},
        {"[s]\na = 1\n[t]\nb = 1\n[s]\nc = 1\n[u]\n", "s", "", "d", "1", 0,
         "[s]\na = 1\n[t]\nb = 1\n[s]\nc = 1\nd = 1\n[u]\n", 7, 1},
        {"[s]\n\n[t]\nx = 1\n", "s", "", "b", "2", 0,
         "[s]\nb = 2\n\n[t]\nx = 1\n", 2, 1},
        {"  [s] # c\n", "s", "", "b", "2", 0, "  [s] # c\n  b = 2\n", 2, 1},
        {"[s]", "s", "", "b", "2", 0, "[s]\nb = 2\n", 2, 1},
        {"[s]\r\na = 1\r\n", "s", "", "b", "2", 0,
         "[s]\r\na = 1\r\nb = 2\r\n", 3, 1},

        // Add a section at the end, after a blank line
        {"a = 1\n", "endpoint", "x", "url", "http://h/", 0,
         "a = 1\n\n[endpoint x]\nurl = http://h/\n", 4, 1},
        {"[s]\nk = v\n\n", "t", "", "k", "1", 0,
         "[s]\nk = v\n\n[t]\nk = 1\n", 5, 1},
        {"[s]\nk = v", "t", "u", "k", "1", 0,
         "[s]\nk = v\n\n[t u]\nk = 1\n", 5, 1},
        {"", "guard", "", "roots", "/x; ~/y", 0,
         "[guard]\nroots = /x; ~/y\n", 2, 1},
        {"[a b c]\nk = 1\n", "a", "b", "k", "2", 0,
         "[a b c]\nk = 1\n\n[a b]\nk = 2\n", 5, 1},
        {"\xef\xbb\xbf# c\r\n", "s", "", "k", "1", 0,
         "\xef\xbb\xbf# c\r\n\r\n[s]\r\nk = 1\r\n", 4, 1},

        // Top-level keys: after the last top-level key or include, else
        // before the comments leading into the first header
        {"x = 1\ninclude = t.ini\n\n# about s\n[s]\n", "", "", "y", "2", 0,
         "x = 1\ninclude = t.ini\ny = 2\n\n# about s\n[s]\n", 3, 1},
        {"# header\n\n# about s\n[s]\nk = v\n", "", "", "y", "2", 0,
         "# header\n\ny = 2\n\n# about s\n[s]\nk = v\n", 3, 1},
        {"[s]\n", "", "", "y", "2", 0, "y = 2\n\n[s]\n", 1, 1},
        {"# only comments\n", "", "", "y", "2", 0,
         "# only comments\ny = 2\n", 2, 1},
        {"", "", "", "y", "2", 0, "y = 2\n", 1, 1},
        // ... but under a commented-out example of it, past any include
        {"x = 1\n# y = 9\n#y=8\n\n[s]\n", "", "", "y", "2", 0,
         "x = 1\n# y = 9\ny = 2\n#y=8\n\n[s]\n", 3, 1},
        {"# ys = 1\n# y is\n\n[s]\n", "", "", "y", "2", 0,
         "# ys = 1\n# y is\n\ny = 2\n\n[s]\n", 4, 1},
        {"# y = 9\ninclude = t.ini\n", "", "", "y", "2", 0,
         "# y = 9\ninclude = t.ini\ny = 2\n", 3, 1},
        {"[s]\n# y = 9\n", "", "", "y", "2", 0, "y = 2\n\n[s]\n# y = 9\n", 1, 1},

        // Remove: every definition in the section, nothing else
        {"[s]\na = 1 # c\nb = 2\n", "s", "", "a", {}, 1, "[s]\nb = 2\n", 0, 1},
        {"[s]\na = 1\n[s]\na = 2\n[t]\na = 3\n", "s", "", "a", {}, 1,
         "[s]\n[s]\n[t]\na = 3\n", 0, 1},
        {"[s]\na = 1\nb = 2", "s", "", "b", {}, 1, "[s]\na = 1\n", 0, 1},
        {"[s]\na = 1\n", "s", "", "z", {}, 1, "[s]\na = 1\n", 0, 0},
        {"a = 1\n[s]\na = 2\n", "", "", "a", {}, 1, "[s]\na = 2\n", 0, 1},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", cases[i].in);
        Str      v   = cases[i].rm ? Str{} : cases[i].value;
        IniEdit  e   = iniedit(&a, f, cases[i].kind, cases[i].name,
                               cases[i].key, v);
        Str name = concat(&a, cases[i].in, " => ");
        name = concat(&a, name, cases[i].key);
        expect(t, name, e.text, cases[i].want);
        expect(t, concat(&a, name, " (line)"), istr(&a, e.line),
               istr(&a, cases[i].line));
        expect(t, concat(&a, name, " (changed)"), istr(&a, e.changed),
               istr(&a, cases[i].changed));
    }

    // Refusals
    static struct { Str kind, name, key, value; } bad[] = {
        {"s", "", "a b", "1"},
        {"s", "", "include", "1"},
        {"s", "", "#a", "1"},
        {"s", "", "a=b", "1"},
        {"s", "", "", "1"},
        {"s]", "", "a", "1"},
        {"s", "x y", "a", "1"},
        {"", "x", "a", "1"},
        {"s", "", "a", "two\nlines"},
    };
    for (iz i = 0; i < countof(bad); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", "[s]\na = 1\n");
        IniEdit  e   = iniedit(&a, f, bad[i].kind, bad[i].name, bad[i].key,
                               bad[i].value);
        Str name = concat(&a, "refuse ", bad[i].key);
        expect(t, name, istr(&a, !e.text.data && e.err.len), "1");
    }
}

static void testinidelsection(Test *t)
{
    static struct {
        Str in, kind, name, want;
        b32 changed;
    } cases[] = {
        // The comment just above goes, one set apart by a blank stays
        {"# top\nx = 1\n\n# about a\n[endpoint a]\nurl = http://a/\n\n"
         "# about b\n[endpoint b]\nurl = http://b/\n",
         "endpoint", "a",
         "# top\nx = 1\n\n# about b\n[endpoint b]\nurl = http://b/\n", 1},
        {"[s]\nk = 1\n\n# about the group\n\n[a]\nk = 2\n\n[t]\n", "a", "",
         "[s]\nk = 1\n\n# about the group\n\n[t]\n", 1},
        {"[s]\nk = 1\n# about a\n# more\n[a]\nk = 2\n[t]\n", "a", "",
         "[s]\nk = 1\n[t]\n", 1},
        // Comments below and among its keys stay, as does the next one's
        {"[s]\nk = 1\n\n[a]\n# note in a\nk = 2\n\n# trailing\n\n[t]\n", "a", "",
         "[s]\nk = 1\n\n# note in a\n\n# trailing\n\n[t]\n", 1},
        {"[b]\ny = 2\n# about c\n[c]\n", "b", "", "# about c\n[c]\n", 1},
        {"[a]\nk = 1  # inline\n; after\n", "a", "", "; after\n", 1},
        // First, last, only, and between others with no blank lines
        {"# a's\n[a]\nk = 1\n\n# b's\n[b]\nk = 2\n", "a", "",
         "# b's\n[b]\nk = 2\n", 1},
        {"a = 1\n\n[endpoint x]\nurl = http://h/\n", "endpoint", "x",
         "a = 1\n", 1},
        {"a = 1\n\n# x\n[endpoint x]\nurl = http://h/\n\n", "endpoint", "x",
         "a = 1\n", 1},
        {"x = 1\n[a]\nk = 1\n", "a", "", "x = 1\n", 1},
        {"[s]\nk = 1\n", "s", "", "", 1},
        {"[a]\nx = 1\n[b]\ny = 2\n[c]\nz = 3\n", "b", "",
         "[a]\nx = 1\n[c]\nz = 3\n", 1},
        // Every header of it, with the blanks between
        {"[a]\nk = 1\n\n[b]\nj = 2\n\n[a]\nm = 3\n", "a", "", "[b]\nj = 2\n", 1},
        {"[a]\nk = 1\n\n[a]\nm = 3\n\n[b]\n", "a", "", "[b]\n", 1},
        // Malformed lines under it go, an invalid header's lines stay
        {"[a]\ninclude = x.ini\njunk\n[b]\n", "a", "", "[b]\n", 1},
        {"[a b c]\nk = 1\n[a]\nk = 2\n", "a", "", "[a b c]\nk = 1\n", 1},
        // Kinds alone, and named, are different sections
        {"[guard]\nroots = /x\n", "guard", "", "", 1},
        {"[model]\nk = 1\n[model a]\nk = 2\n", "model", "a", "[model]\nk = 1\n", 1},
        {"[model a]\nk = 1\n[model ab]\nk = 2\n", "model", "a", "[model ab]\nk = 2\n", 1},
        // Line terminators: CRLF, a BOM, and a missing final one
        {"# c\r\n[a]\r\nk = 1\r\n\r\n[b]\r\nk = 2\r\n", "a", "", "[b]\r\nk = 2\r\n", 1},
        {"[s]\r\nk = 1\r\n\r\n[a]\r\nk = 2\r\n\r\n[t]\r\n", "a", "",
         "[s]\r\nk = 1\r\n\r\n[t]\r\n", 1},
        {"\xef\xbb\xbf[a]\nk = 1\n\n[b]\n", "a", "", "\xef\xbb\xbf[b]\n", 1},
        {"[a]\nk = 1\n\n[b]\nk = 2", "b", "", "[a]\nk = 1", 1},
        {"[a]\nk = 1\n\n[b]\nk = 2", "a", "", "[b]\nk = 2", 1},
        {"[a]\nk = 1\n\n[b]\nk = 2\n", "b", "", "[a]\nk = 1\n", 1},
        {"[a]", "a", "", "", 1},
        // Absent: nothing changes
        {"[a]\nk = 1\n", "b", "", "[a]\nk = 1\n", 0},
        {"# [a]\n", "a", "", "# [a]\n", 0},
        {"", "a", "", "", 0},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", cases[i].in);
        IniEdit  e   = inidelsection(&a, f, cases[i].kind, cases[i].name);
        Str name = concat(&a, cases[i].in, " -= ");
        name = concat(&a, name, cases[i].kind);
        expect(t, name, e.text, cases[i].want);
        expect(t, concat(&a, name, " (changed)"), istr(&a, e.changed),
               istr(&a, cases[i].changed));
    }

    // Adding a section and deleting it gives the text back
    static Str texts[] = {
        "a = 1\n", "# c\n[s]\nk = v\n", "[s]\nk = v", "", "\xef\xbb\xbf# c\r\n",
    };
    for (iz i = 0; i < countof(texts); i++) {
        Arena    a   = t->perm;
        Ini      one = {};
        IniFile *f   = iniparse(&one, &a, "", texts[i]);
        IniEdit  e   = iniedit(&a, f, "endpoint", "x", "url", "http://h/");
        Ini      two = {};
        IniFile *g   = iniparse(&two, &a, "", e.text);
        IniEdit  d   = inidelsection(&a, g, "endpoint", "x");
        Str      want = texts[i];
        if (want.len && want[want.len-1]!='\n') {
            want = concat(&a, want, "\n");  // the add ended its last line
        }
        expect(t, concat(&a, "add, delete: ", texts[i]), d.text, want);
    }

    // Refusals
    static struct { Str kind, name; } bad[] = {
        {"", ""}, {"a b", ""}, {"a", "x y"}, {"a]", ""}, {"a", "[x"},
    };
    for (iz i = 0; i < countof(bad); i++) {
        Arena    a   = t->perm;
        Ini      ini = {};
        IniFile *f   = iniparse(&ini, &a, "", "[a]\nk = 1\n");
        IniEdit  e   = inidelsection(&a, f, bad[i].kind, bad[i].name);
        Str name = concat(&a, "refuse delete ", bad[i].kind);
        expect(t, name, istr(&a, !e.text.data && e.err.len), "1");
    }
}

// Every winning definition but section (kind, name)'s, as inidump.
static Str inidumpwithout(Arena *a, Ini *ini, Str kind, Str name)
{
    Buf b(a, 256);
    for (IniSection *s = ini->sections; s; s = s->next) {
        for (IniEntry *e = s->keys; !(s->kind==kind && s->name==name) && e; e = e->next) {
            print(&b, s->kind);
            putbyte(&b, ':');
            print(&b, s->name);
            putbyte(&b, ':');
            print(&b, e->key);
            putbyte(&b, '=');
            print(&b, e->value);
            putbyte(&b, '\n');
        }
    }
    return finish(&b);
}

// Random deletions from random texts: the section is gone, every other
// keeps its values, the lines left are lines of the text, in order, and
// every comment but those just above its headers is still there.
static void testinidelrandom(Test *t)
{
    static Str pieces[] = {
        "k = 1\n", "j = \"2\" # c\n", "[s]\n", "[s t]\n", "  [t] # c\n",
        "[a b c]\n", "# c\n", "; d\n", "\n", "\r\n", "junk\n",
        "include = x.ini\n", "k = 4", "[s]", "j=5\r\n", "[u]\n",
    };
    static Str kinds[] = {"s", "s", "t", "u"};
    static Str names[] = {"", "", "t"};
    u64 rng = 11;
    i32 bad = 0;
    for (i32 n = 0; n < 20000; n++) {
        Arena a = t->perm;
        Buf   b(&a, 256);
        iz    count = inirand(&rng) % 10;
        if (!(inirand(&rng) % 8)) {
            print(&b, "\xef\xbb\xbf");
        }
        for (iz i = 0; i < count; i++) {
            print(&b, pieces[inirand(&rng) % countof(pieces)]);
        }
        Str src  = finish(&b);
        Str kind = kinds[inirand(&rng) % countof(kinds)];
        Str name = names[inirand(&rng) % countof(names)];

        Ini      before = {};
        IniFile *f      = iniparse(&before, &a, "", src);
        IniEdit  e      = inidelsection(&a, f, kind, name);
        Ini      after  = {};
        IniFile *g      = iniparse(&after, &a, "", e.text);

        // The comments that should stay: all but those just above its
        // headers
        iz want = 0;
        for (iz i = 0; i < f->lines.len; i++) {
            if (f->lines[i].type != INI_COMMENT) {
                continue;
            }
            iz k = i;
            for (; k<f->lines.len && f->lines[k].type==INI_COMMENT; k++) {}
            IniLine *h = k<f->lines.len ? &f->lines[k] : 0;
            want += !h || h->type!=INI_SECTION || !iniin(h, kind, name);
        }
        iz got = 0;
        for (iz i = 0; i < g->lines.len; i++) {
            got += g->lines[i].type == INI_COMMENT;
        }
        // Each line left is the next of the text's, in order
        iz  at  = 0;
        b32 sub = 1;
        for (iz i = 0; sub && i < g->lines.len; i++) {
            for (; at<f->lines.len && !(f->lines[at].raw==g->lines[i].raw); at++) {}
            sub = at++ < f->lines.len;
        }
        Str why = {};
        if (!e.text.data) {
            why = "failed";
        } else if (inisection(&after, kind, name)) {
            why = "still there";
        } else if (!(inidumpwithout(&a, &before, kind, name) ==
                     inidumpwithout(&a, &after, kind, name))) {
            why = "others";
        } else if (!sub) {
            why = "lines";
        } else if (got != want) {
            why = "comments";
        } else if (g->bom != f->bom) {
            why = "bom";
        } else if (e.changed != !(e.text == src)) {
            why = "changed";
        }
        if (why.len && !bad++) {
            Str msg = concat(&a, concat(&a, "random delete: ", why), " ");
            msg = concat(&a, concat(&a, msg, kind), concat(&a, " ", name));
            expect(t, concat(&a, concat(&a, msg, ": "), src), e.text, "");
        }
    }
    Arena a = t->perm;
    expect(t, "delete.random.failures", istr(&a, bad), "0");
}

// Line count of a text, as the parser counts after a BOM.
static iz inilinecount(Str s)
{
    if (startswith(s, "\xef\xbb\xbf")) {
        s = cuthead(s, 3);
    }
    iz n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s[i] == '\n';
    }
    return n + (s.len && s[s.len-1]!='\n');
}

// Every winning definition in a single-file Ini as "kind:name:key=value"
// lines, skipping one key.
static Str inidump(Arena *a, Ini *ini, Str kind, Str name, Str key)
{
    Buf b(a, 256);
    for (IniSection *s = ini->sections; s; s = s->next) {
        for (IniEntry *e = s->keys; e; e = e->next) {
            if (s->kind==kind && s->name==name && e->key==key) {
                continue;
            }
            print(&b, s->kind);
            putbyte(&b, ':');
            print(&b, s->name);
            putbyte(&b, ':');
            print(&b, e->key);
            putbyte(&b, '=');
            print(&b, e->value);
            putbyte(&b, '\n');
        }
    }
    return finish(&b);
}

// Random edits of random texts: the edited key takes the value (or is
// gone), everything else keeps its value, no line becomes invalid, and
// the line count changes only by what the edit adds or drops.
static void testiniwriterandom(Test *t)
{
    static Str pieces[] = {
        "k = 1\n", "j = \"2\" # c\n", "  k = 3\n", "[s]\n", "[s t]\n",
        "  [t] # c\n", "[a b c]\n", "# c\n", "; c\n", "\n", "\r\n", "junk\n",
        "include = x.ini\n", "k = 4", "[s]", "j=5\r\n",
        // Empty values, next to trailing comments or not
        "k = # c\n", "j =\t#\n", "k =\n", "k=\"\" # c\n", "new = 1 #c\n",
        "j=\n",
    };
    static Str kinds[] = {"", "s", "s", "t", "u"};
    static Str names[] = {"", "", "t"};
    static Str keys[]  = {"k", "j", "new"};
    static Str values[] = {"1", "v", "", " padded ", "x # y", "\"q\"", "#z"};
    u64 rng = 7;
    i32 bad = 0;
    for (i32 n = 0; n < 20000; n++) {
        Arena a = t->perm;
        Buf   b(&a, 256);
        iz    count = inirand(&rng) % 9;
        // A BOM only leads: one mid-file means something else once an
        // edit moves it to the start
        if (!(inirand(&rng) % 8)) {
            print(&b, "\xef\xbb\xbf");
        }
        for (iz i = 0; i < count; i++) {
            print(&b, pieces[inirand(&rng) % countof(pieces)]);
        }
        Str src  = finish(&b);
        Str kind = kinds[inirand(&rng) % countof(kinds)];
        Str name = kind.len ? names[inirand(&rng) % countof(names)] : Str("");
        Str key  = keys[inirand(&rng) % countof(keys)];
        b32 rm   = !(inirand(&rng) % 4);
        Str val  = rm ? Str{} : values[inirand(&rng) % countof(values)];

        Ini      before = {};
        IniFile *f      = iniparse(&before, &a, "", src);
        iz       invalid0 = 0;
        for (iz i = 0; i < f->lines.len; i++) {
            invalid0 += f->lines[i].type == INI_INVALID;
        }
        IniEntry *old = inilookup(&before, kind, name, key);
        IniEdit   e   = iniedit(&a, f, kind, name, key, val);

        Ini      after = {};
        IniFile *g     = iniparse(&after, &a, "", e.text);
        iz       invalid1 = 0;
        for (iz i = 0; i < g->lines.len; i++) {
            invalid1 += g->lines[i].type == INI_INVALID;
        }
        IniEntry *now  = inilookup(&after, kind, name, key);
        Str       why  = {};
        iz        diff = inilinecount(e.text) - inilinecount(src);
        if (!e.text.data) {
            why = "failed";
        } else if (rm ? now!=0 : !now || !(now->value==val)) {
            why = "value";
        } else if (!rm && (e.line<1 || e.line>g->lines.len ||
                           &g->lines[e.line-1]!=now->line)) {
            why = "line";
        } else if (!(inidump(&a, &before, kind, name, key) ==
                     inidump(&a, &after, kind, name, key))) {
            why = "others";
        } else if (invalid0 != invalid1) {
            why = "invalid";
        } else if (g->bom != f->bom) {
            why = "bom";
        } else if (e.changed != !(e.text == src)) {
            why = "changed";
        } else if (rm ? diff>0 : old ? diff!=0 : diff<1 || diff>3) {
            why = "line count";
        }
        if (why.len && !bad++) {
            expect(t, concat(&a, concat(&a, "random edit: ", why), src),
                   e.text, "");
        }
    }
    Arena a = t->perm;
    expect(t, "edit.random.failures", istr(&a, bad), "0");
}

// URL includes from a table, as a loader's hook gives them, recording
// what was asked for: "URL" or "URL pin", separated by "|".
struct IniWeb {
    Str  urls[8];
    Str  texts[8];
    i32  n;
    Buf  asked;
};

static IniSource iniwebfetch(void *ctx, Arena *perm, Str url, Str pin)
{
    IniWeb   *w = (IniWeb *)ctx;
    IniSource r = {};
    print(&w->asked, w->asked.len ? Str("|") : Str(""));
    print(&w->asked, url);
    if (pin.data) {
        putbyte(&w->asked, ' ');
        print(&w->asked, pin);
    }
    for (i32 i = 0; i < w->n; i++) {
        if (w->urls[i] == url) {
            r.data = w->texts[i];
            return r;
        }
    }
    r.msg      = concat(perm, "no such page ", url);
    r.severity = INI_WARNING;
    return r;
}

static void testiniurls(Test *t)
{
    Arena a = t->perm;

    static struct { Str in, want; } cleans[] = {
        {"HTTPS://Example.COM", "https://example.com/"},
        {"http://h/a/./b/../c?x=1&y=/../#frag", "http://h/a/c?x=1&y=/../"},
        {"http://h:8080/A/", "http://h:8080/A/"},
        {"http://h?q", "http://h/?q"},
        {"http://h/../../x", "http://h/x"},
        {"http://h/a/..", "http://h/"},
        {"http://h/a/b/..", "http://h/a/"},
        {"http://h//a/./", "http://h//a/"},
        {"http://[::1]:80/x", "http://[::1]:80/x"},
        {"ftp://h/x", "(null)"},
        {"http://", "(null)"},
        {"http:///x", "(null)"},
        {"http://u:p@h/", "(null)"},
        {"http://h/a b", "(null)"},
        {"http://h/a\\b", "(null)"},
        {"team.ini", "(null)"},
    };
    for (iz i = 0; i < countof(cleans); i++) {
        Str got = iniurlclean(&a, cleans[i].in);
        expect(t, concat(&a, "urlclean ", cleans[i].in),
               got.data ? got : Str("(null)"), cleans[i].want);
    }

    // RFC 3986, 5.4, without fragments, which are dropped
    static struct { Str ref, want; } refs[] = {
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g/"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q"},
        {"g#s", "http://a/b/c/g"},
        {"g;x", "http://a/b/c/g;x"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"HTTPS://Other/x.ini", "https://other/x.ini"},
        {"file:///etc/passwd", "(null)"},
        {"C:/x.ini", "(null)"},
        {"a b.ini", "(null)"},
    };
    for (iz i = 0; i < countof(refs); i++) {
        Str got = iniurlresolve(&a, "http://a/b/c/d;p?q", refs[i].ref);
        expect(t, concat(&a, "urlresolve ", refs[i].ref),
               got.data ? got : Str("(null)"), refs[i].want);
    }

    // Random references resolve to canonical URLs, or to none: cleaning
    // again changes nothing, and no path segment is "." or ".."
    {
        static Str const parts[] = {
            "a", "b/", "/", ".", "..", "./", "../", "?q/../", "#f", "//h",
            ":", "http:", "HTTPS://H", "%2e", "\\", " ", "@", "", ";p",
        };
        u64 rng = 1;
        i32 bad = 0;
        for (i32 n = 0; n < 20000; n++) {
            Arena tmp = a;
            Buf   ref(&tmp, 64);
            for (iz k = inirand(&rng)%8; k > 0; k--) {
                print(&ref, parts[inirand(&rng) % countof(parts)]);
            }
            Str got = iniurlresolve(&tmp, "http://h/x/y?z", finish(&ref));
            if (!got.data) {
                continue;
            }
            b32 ok   = iniurlclean(&tmp, got) == got;
            Str path = cut(cuthead(got, find(got, "://")+3), '?').head;
            path = cuthead(path, cut(path, '/').head.len);  // after the host
            for (Str rest = path; rest.len;) {
                Cut c = cut(rest, '/');
                ok &= !(c.head==".") && !(c.head=="..");
                rest = c.tail;
            }
            if (!ok && !bad++) {
                expect(t, "urlresolve.random", got, finish(&ref));
            }
        }
        expect(t, "urlresolve.random.bad", istr(&a, bad), "0");
    }

    // Includes in a file from the web are references to more of the web
    Os    *os  = newos(&a, 1<<16);
    Str    pin = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    IniWeb w   = {};
    w.asked = Buf(&a, 1024);
    os_write(os, a, "/w/root.ini",
        "include = HTTPS://H/team/./a.ini#top\n"
        "mine = 1\n");
    Str pages[][2] = {
        {"https://h/team/a.ini",
         "include = b.ini\n"
         "include = ../c.ini sha256=00112233445566778899AABBCCDDEEFF"
         "00112233445566778899aabbccddeeff\n"
         "include = //other/d.ini\n"
         "include = /etc/passwd\n"
         "include = a.ini\n"
         "include = C:\\x.ini\n"
         "from = a\n"
         "[endpoint t]\n"
         "url = http://t/\n"},
        {"https://h/team/b.ini", "from = b\n"},
        {"https://h/c.ini", "from = c\n"},
        {"https://other/d.ini", "include = a.ini\nfrom = d\n"},
    };
    for (iz i = 0; i < countof(pages); i++) {
        w.urls[w.n]    = pages[i][0];
        w.texts[w.n++] = pages[i][1];
    }
    {
        Ini ini = {};
        ini.fetch    = iniwebfetch;
        ini.fetchctx = &w;
        iniload(&ini, os, &a, "/w/root.ini");
        expect(t, "web.files", inishowfiles(&a, &ini),
               "/w/root.ini@0 "
               "https://h/team/a.ini<-/w/root.ini:1@1 "
               "https://h/team/b.ini<-https://h/team/a.ini:1@2 "
               "https://h/c.ini<-https://h/team/a.ini:2@2 "
               "https://other/d.ini<-https://h/team/a.ini:3@2");
        expect(t, "web.diags", inidiags(&a, &ini),
               "https://other/d.ini:1:11: warning: no such page "
               "https://other/a.ini\n"
               "https://h/team/a.ini:4:11: warning: no such page "
               "https://h/etc/passwd\n"
               "https://h/team/a.ini:5:11: warning: include cycle: "
               "https://h/team/a.ini\n"
               "https://h/team/a.ini:6:11: error: expected an http:// or "
               "https:// URL, include ignored");
        expect(t, "web.asked", finish(&w.asked), concat(&a,
               "https://h/team/a.ini|https://h/team/b.ini|https://h/c.ini ",
               concat(&a, pin, "|https://other/d.ini|https://other/a.ini|"
                               "https://h/etc/passwd")));
        expect(t, "web.from", inihistory(&a, &ini, "", "", "from"),
               "https://h/team/a.ini:7 https://other/d.ini:2 "
               "https://h/c.ini:1 https://h/team/b.ini:1");
        expect(t, "web.url", inihistory(&a, &ini, "endpoint", "t", "url"),
               "https://h/team/a.ini:9");
    }

    // Text past the budget is refused like a file's
    {
        Ini ini = {};
        IniWeb big = {};
        big.asked    = Buf(&a, 64);
        big.urls[0]  = "http://h/big.ini";
        big.texts[0] = {allocbytes(&a, INI_MAXBYTES), INI_MAXBYTES};
        for (iz i = 0; i < INI_MAXBYTES; i++) {
            big.texts[0][i] = '\n';
        }
        big.n        = 1;
        ini.fetch    = iniwebfetch;
        ini.fetchctx = &big;
        os_write(os, a, "/w/big.ini", "include = http://h/big.ini\n");
        iniload(&ini, os, &a, "/w/big.ini");
        expect(t, "web.big", inidiags(&a, &ini),
               "/w/big.ini:1:11: warning: cannot read include "
               "http://h/big.ini (over the 1 MiB configuration limit)");
    }
}

static void test_ini(Test *t)
{
    testiniurls(t);
    testiniparse(t);
    testiniedit(t);
    testiniwrite(t);
    testiniwriterandom(t);
    testinidelsection(t);
    testinidelrandom(t);
    testinilayers(t);
    testinipaths(t);
    testiniinclude(t);
    testinicap(t);
    testinirandom(t);
    testinigraph(t);
}

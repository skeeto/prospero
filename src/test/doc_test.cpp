// Markdown document tests: structure, styles, source offsets, code
// exactness, and pathological input.

// Render a document compactly, one block per line:
//   TYPE[attrs]: piece|piece|...
// with each piece as "styles:text", styles from "bicslhd" (bold,
// italic, code, strike, link, header, dim), "~" for synthetic text,
// "/class" for a highlighted token, and newlines shown as \n.
static Str docshow(Arena *a, Doc *d)
{
    Buf b(a, 256);
    for (iz i = 0; i < d->blocks.len; i++) {
        DocBlock *k = &d->blocks[i];
        static char const types[] = "PHCIRTL";
        putbyte(&b, (u8)types[k->type]);
        if (k->type == DOC_HEADING) print(&b, (i64)k->level);
        if (k->depth) { print(&b, " d"); print(&b, (i64)k->depth); }
        if (k->quote) { print(&b, " q"); print(&b, (i64)k->quote); }
        if (k->item) {
            if (k->number < 0) {
                print(&b, " -");
            } else {
                putbyte(&b, ' ');
                print(&b, (i64)k->number);
                putbyte(&b, '.');
            }
        }
        if (k->task) print(&b, k->task==2 ? Str(" [x]") : Str(" [ ]"));
        if (k->lang.len) { print(&b, " lang="); print(&b, k->lang); }
        if (k->type == DOC_CELL) {
            print(&b, " t");  print(&b, (i64)k->table);
            print(&b, "r");   print(&b, (i64)k->row);
            print(&b, "c");   print(&b, (i64)k->col);
            if (k->align) { print(&b, "a"); print(&b, (i64)k->align); }
        }
        print(&b, ":");
        for (i32 j = 0; j < k->npiece; j++) {
            Piece *p = &d->pieces[k->piece0+j];
            putbyte(&b, j ? '|' : ' ');
            static char const names[] = "bicslhd";
            b32 any = 0;
            for (i32 s = 0; s < 7; s++) {
                if (p->style & (1<<s)) {
                    putbyte(&b, (u8)names[s]);
                    any = 1;
                }
            }
            if (p->flags & PIECE_SYNTH) { putbyte(&b, '~'); any = 1; }
            if (p->hl) {
                putbyte(&b, '/');
                print(&b, hlclasses[p->hl]);
                any = 1;
            }
            if (any) putbyte(&b, ':');
            for (iz c = 0; c < p->text.len; c++) {
                if (p->text[c] == '\n') {
                    print(&b, "\\n");
                } else {
                    putbyte(&b, p->text[c]);
                }
            }
        }
        putbyte(&b, '\n');
    }
    return finish(&b);
}

// The code of a code block, as its piece texts concatenated.
static Str doccode(Arena *a, Doc *d, iz block)
{
    Buf    b(a, 64);
    DocBlock *k = &d->blocks[block];
    for (i32 j = 0; j < k->npiece; j++) {
        print(&b, d->pieces[k->piece0+j].text);
    }
    return finish(&b);
}

// Check the offset invariants of every piece, returning a description
// of the first violation, or "" if none.
static Str doccheck(Arena *a, Doc *d)
{
    Str src = d->src;
    for (iz i = 0; i < d->blocks.len; i++) {
        DocBlock *k = &d->blocks[i];
        if (k->off0<0 || k->off0>k->off1 || k->off1>src.len) {
            return concat(a, "block range ", istr(a, i));
        }
        i32 last = k->off0;
        for (i32 j = 0; j < k->npiece; j++) {
            Piece *p = &d->pieces[k->piece0+j];
            if (p->off<0 || p->len<0 || p->off+p->len>src.len) {
                return concat(a, "piece range ", istr(a, k->piece0+j));
            }
            if (p->off < last) {
                return concat(a, "piece order ", istr(a, k->piece0+j));
            }
            last = p->off;
            if (p->flags & PIECE_SYNTH) {
                if (p->len) return concat(a, "synthetic len ", istr(a, k->piece0+j));
            } else if (p->len == p->text.len) {
                Str at = takehead(cuthead(src, p->off), p->len);
                if (!(at == p->text)) {
                    return concat(a, "piece text ", istr(a, k->piece0+j));
                }
            }
        }
    }
    return "";
}

static Str mdshow(Arena *a, Str src)
{
    Doc *d   = parsedoc(a, src, 0);
    Str  bad = doccheck(a, d);
    return bad.len ? bad : docshow(a, d);
}

static void test_doc(Test *t)
{
    Arena perm = t->perm;

    struct { Str name, src, want; } cases[] = {
        {"doc.empty", "", ""},
        {"doc.para", "Hello **world**, *it* is `code` and ~~gone~~.",
         "P: Hello |b:world|, |i:it| is |c:code| and |s:gone|.\n"},
        {"doc.soft", "one\ntwo  \nthree\\\nfour",
         "P: one|~:\\n|two|~:\\n|three|~:\\n|four\n"},
        {"doc.paras", "a\n\nb", "P: a\nP: b\n"},
        {"doc.heading", "# One\n### Three *x*",
         "H1: One\nH3: Three |i:x\n"},
        {"doc.link", "see [the site](http://x.y) now",
         "P: see |l:the site| now\n"},
        {"doc.autolink", "go to https://example.com.",
         "P: go to |l:https://example.com|.\n"},
        {"doc.nohtml", "a <b>bold</b> Vec<T> <thinking>",
         "P: a <b>bold</b> Vec<T> <thinking>\n"},
        {"doc.entity", "&amp; &lt;x&gt; &#65;&#x42; &nbsp; &bogus;",
         "P: &| |<|x|>| |A|B| |&nbsp;| |&bogus;\n"},
        {"doc.escape", "\\*not\\*", "P: *|not|*\n"},
        {"doc.tight", "- a\n- b\n  - c\n- d",
         "I d1 -: a\nI d1 -: b\nI d2 -: c\nI d1 -: d\n"},
        {"doc.loose", "1. a\n\n   more\n\n2. b",
         "P d1 1.: a\nP d1: more\nP d1 2.: b\n"},
        {"doc.start", "7) x\n8) y", "I d1 7.: x\nI d1 8.: y\n"},
        {"doc.tasks", "- [ ] todo\n- [x] done",
         "I d1 - [ ]: todo\nI d1 - [x]: done\n"},
        {"doc.emptyitem", "- \n- b", "I d1 -:\nI d1 -: b\n"},
        {"doc.quote", "> a\n>> b\n\nc", "P q1: a\nP q2: b\nP: c\n"},
        {"doc.listquote", "- > q", "P d1 q1 -: q\n"},
        {"doc.hr", "a\n\n---\n\nb", "P: a\nR:\nP: b\n"},
        {"doc.table", "| a | b |\n|:-:|--:|\n| 1 | 2 |",
         "T t0r0c0a1: h:a\nT t0r0c1a2: h:b\nT t0r1c0a1: 1\nT t0r1c1a2: 2\n"},
        {"doc.code", "```c\nint x;\n```",
         "C lang=c: c/t:int|c: x;|c~:\\n\n"},
        {"doc.indented", "    plain\n", "C: c:plain|c~:\\n\n"},
        {"doc.unclosed", "text\n```py\nx = 1\n\nmore",
         "P: text\nC lang=py: c:x = |c/m:1|c~:\\n|c~:\\n|c:more|c~:\\n\n"},
        {"doc.codeinlist", "- x\n\n  ```\n  y\n  ```",
         "P d1 -: x\nC d1: c:y|c~:\\n\n"},
        {"doc.footnote", "a[^1]\n\n[^1]: note",
         "P: a|l~:[1]\nP: note\n"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena a = perm;
        expect(t, cases[i].name, mdshow(&a, cases[i].src), cases[i].want);
    }

    // Code is byte-exact: tabs, trailing spaces, inner blank lines, and
    // CRLF line ends (the CR is not code, as in a CRLF file it is not)
    {
        Arena a = perm;
        struct { Str src, code; } cases[] = {
            {"```\n\tx\t \n\n  y  \n```\n", "\tx\t \n\n  y  \n"},
            {"```make\nall:\n\tcc -o x x.c\n```", "all:\n\tcc -o x x.c\n"},
            {"> ```\n> \tq\n> ```", "\tq\n"},
            {"- ```\n  \t  t\n  ```", "\t  t\n"},
            {"```\n                    deep\n```", "                    deep\n"},
            {"```\r\na\r\n```\r\n", "a\n"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Doc *d = parsedoc(&a, cases[i].src, 0);
            iz   c = -1;
            for (iz k = 0; k < d->blocks.len && c<0; k++) {
                if (d->blocks[k].type == DOC_CODE) c = k;
            }
            Str name = concat(&a, "doc.exact", istr(&a, i));
            expect(t, name, c<0 ? Str("(no code)") : doccode(&a, d, c), cases[i].code);
            expect(t, concat(&a, name, ".check"), doccheck(&a, d), "");
        }
        // The tab came from the source, not md4c's spaces
        Doc   *d = parsedoc(&a, "```\n\tx\n```", 0);
        Piece *p = &d->pieces[d->blocks[0].piece0];
        expect(t, "doc.tabsource", istr(&a, p->off*10 + p->len), "41");
    }

    // Block ranges, and rules and empty code at their own lines
    {
        Arena a = perm;
        Str   s = "# T\n\npara text\n\n---\n\n```\n```\n";
        Doc  *d = parsedoc(&a, s, 0);
        Buf   b(&a, 64);
        for (iz i = 0; i < d->blocks.len; i++) {
            print(&b, (i64)d->blocks[i].off0);
            putbyte(&b, '-');
            print(&b, (i64)d->blocks[i].off1);
            putbyte(&b, ' ');
        }
        expect(t, "doc.ranges", finish(&b), "2-3 5-14 16-19 21-24 ");
        Doc *d0 = parsedoc(&a, "---", 0);
        expect(t, "doc.hrfirst", istr(&a, d0->blocks[0].off1), "3");
    }

    // The layout key follows content, not position
    {
        Arena a  = perm;
        Doc  *d1 = parsedoc(&a, "same\n\nother", 0);
        Doc  *d2 = parsedoc(&a, "x\n\nsame", 0);
        Doc  *d3 = parsedoc(&a, "- same", 0);
        Doc  *d4 = parsedoc(&a, "*same*", 0);
        u64 h1 = d1->blocks[0].hash, h2 = d2->blocks[1].hash;
        u64 h3 = d3->blocks[0].hash, h4 = d4->blocks[0].hash;
        expect(t, "doc.hashpos", istr(&a, h1 == h2), "1");
        expect(t, "doc.hashctx", istr(&a, h1!=h3 && h1!=h4 && h3!=h4), "1");
    }

    // Plain text: a block per line, nothing interpreted
    {
        Arena a = perm;
        Doc  *d = parseplain(&a, "# no\n\n**x**", STYLE_DIM);
        expect(t, "doc.plain", docshow(&a, d), "L: d:# no\nL:\nL: d:**x**\n");
        expect(t, "doc.plaincheck", doccheck(&a, d), "");
    }

    // Highlighted code is memoized by content
    {
        Arena   a    = perm;
        HlMemo *memo = 0;
        Doc *d1 = parsedoc(&a, "```c\nint x;\n```", &memo);
        Doc *d2 = parsedoc(&a, "intro\n\n```c\nint x;\n```", &memo);
        Str  s1 = doccode(&a, d1, 0);
        Str  s2 = doccode(&a, d2, 1);
        expect(t, "doc.memo", istr(&a, memo && !memo->child[0] && s1==s2), "1");
    }

    // Every prefix of a document, as while streaming, parses with sound
    // offsets
    {
        Str src =
            "# Title\n\nSome *text* with `code` and a [link](u).\n\n"
            "- one\n- two\n  1. a\n  2. b\n\n> quote\n> > deeper\n\n"
            "| x | y |\n|---|---|\n| 1 | 2 |\n\n"
            "```sh\nfor f in *; do\n\techo \"$f\" &amp;\ndone\n```\n\n"
            "Done &amp; dusted.\n";
        Str bad = {};
        for (iz n = 0; n <= src.len && !bad.len; n++) {
            Arena a = perm;
            Doc  *d = parsedoc(&a, takehead(src, n), 0);
            bad = doccheck(&a, d);
            if (bad.len) bad = concat(&a, concat(&a, istr(&a, n), ": "), bad);
        }
        expect(t, "doc.prefixes", bad, "");
    }

    // Pathological input finishes, with sound offsets
    {
        Arena a = perm;
        struct { Str name; Str unit; iz count; Str tail; } cases[] = {
            {"doc.deepquote",  ">",   200,    " x"},
            {"doc.deeplist",   "- ",  200,    "x"},
            {"doc.backticks",  "`",   100000, "x"},
            {"doc.stars",      "*a",  50000,  ""},
            {"doc.brackets",   "[",   50000,  "]"},
            {"doc.fences",     "```\n", 20000, ""},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena tmp = a;
            Buf   b(&tmp, 1<<16);
            for (iz n = 0; n < cases[i].count; n++) {
                if (cases[i].unit == "- ") {
                    for (iz k = 0; k < n; k++) print(&b, "  ");
                    print(&b, "- x\n");
                } else {
                    print(&b, cases[i].unit);
                }
            }
            print(&b, cases[i].tail);
            Str  src = finish(&b);
            Doc *d   = parsedoc(&tmp, src, 0);
            expect(t, cases[i].name, doccheck(&tmp, d), "");
        }
    }

    // An oversized message falls back to plain text
    {
        Arena a   = perm;
        iz    len = DOC_MAXSIZE + 1;
        Str   src = {allocbytes(&a, len), len};
        for (iz i = 0; i < len; i++) src[i] = i%80==79 ? '\n' : '*';
        Doc *d = parsedoc(&a, src, 0);
        expect(t, "doc.huge", istr(&a, d->plain), "1");
    }
}

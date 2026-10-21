// Edit tests: the replace ladder, hashline edits and tags, patches

// The new content, or the message when nothing would change.
static Str rep(Arena *a, Str data, Str oldstr, Str newstr, b32 all)
{
    EditOut o = editreplace(a, data, "f.c", oldstr, newstr, all);
    return o.data.data ? o.data : concat(a, "!", o.text);
}

static Str repnote(Arena *a, Str data, Str oldstr, Str newstr)
{
    return editreplace(a, data, "f.c", oldstr, newstr, 0).text;
}

static HashEdit hedit(i32 op, i64 line, i64 end, Str text)
{
    HashEdit h = {};
    h.op   = op;
    h.line = line;
    h.end  = end;
    h.text = text;
    return h;
}

static Str hashrun(Arena *a, Str data, Str tag, HashEdit *e, iz n)
{
    Slice<HashEdit> edits = {};
    for (iz i = 0; i < n; i++) {
        edits = push(a, edits, e[i]);
    }
    EditOut o = edithash(a, data, "f.c", tag, edits);
    return o.data.data ? o.data : concat(a, "!", o.text);
}

// Apply a patch of one update to data, or the error.
static Str patchrun(Arena *a, Str data, Str patch)
{
    Patch p = parsepatch(a, patch);
    if (p.err.data) {
        return concat(a, "parse: ", p.err);
    }
    EditOut o = patchupdate(a, data, "f.c", &p.files[0]);
    return o.data.data ? o.data : concat(a, "!", o.text);
}

static void test_edit(Test *t)
{
    Arena *a = &t->perm;

    expect(t, "editformat", istr(a, editformat(" Patch ")*10 + editformat("hashline")),
           istr(a, EDIT_PATCH*10 + EDIT_HASHLINE));
    expect(t, "editformat unknown", istr(a, editformat("sloppy")), istr(a, EDIT_REPLACE));

    // Text as lines, written back byte for byte
    {
        Str  data = "\xef\xbb\xbf" "a\r\nb\nc";
        Text tx   = splittext(a, data);
        expect(t, "splittext", istr(a, tx.lines.len*100 + tx.crlf*10 + tx.final), "310");
        expect(t, "jointext", jointext(a, &tx, tx.lines), data);
        Text e = splittext(a, "");
        expect(t, "splittext empty", istr(a, e.lines.len*10 + e.final), "1");
        Slice<Str> g = givenlines(a, "x\r\ny\n");
        expect(t, "givenlines", concat(a, concat(a, g[0], "|"), g[1]), "x|y");
        expect(t, "givenlines empty", istr(a, givenlines(a, "").len*10 + givenlines(a, "\n").len), "1");
        expect(t, "normtext", normtext(a, "  \xe2\x80\x9cx\xe2\x80\x9d \t\xe2\x80\x94  y "), "\"x\" - y");
        // Invalid UTF-8 (found by AFL): kept as its bytes, never encoded
        expect(t, "normtext invalid", normtext(a, "a \xff\xbd\xbd\xbd  \xc3"), "a \xff\xbd\xbd\xbd \xc3");
        expect(t, "rep invalid", rep(a, "\xff\xbd\xbd\xbd x\n", "\xff\xbd\xbd\xbd  x", "y", 0), "y\n");
        expect(t, "uncomment", uncomment("// # x = 1; */"), "x = 1;");
        expect(t, "distance", istr(a, distance(*a, "kitten", "sitting", 10, 0)), "3");
        expect(t, "distance limit", istr(a, distance(*a, "kitten", "sitting", 2, 0)), "3");
        expect(t, "distance fold", istr(a, distance(*a, "ABC", "abc", 5, 1)), "0");
    }

    // The ladder, rung by rung
    {
        Str src = "int f(void)\n{\n    int x = 1;   \n    return x;\n}\n";
        expect(t, "exact", rep(a, src, "x = 1", "x = 2", 0),
               "int f(void)\n{\n    int x = 2;   \n    return x;\n}\n");
        expect(t, "rtrim", rep(a, src, "    int x = 1;\n    return x;\n", "    int x = 2;\n    return x;\n", 0),
               "int f(void)\n{\n    int x = 2;\n    return x;\n}\n");
        // The line kept as it was, trailing blanks and all
        expect(t, "rtrim keeps", rep(a, src, "    int x = 1;\n    return x;", "    int x = 1;\n    return -x;", 0),
               "int f(void)\n{\n    int x = 1;   \n    return -x;\n}\n");
        expect(t, "rtrim note", repnote(a, src, "    int x = 1;\n    return x;", "    int x = 1;\n    return -x;"),
               "Edited f.c: replaced 1 match at line 3 (matched ignoring trailing whitespace).");
        // Indentation the agent got wrong: kept lines stay, added ones move
        expect(t, "trim reindents", rep(a, src, "int x = 1;\nreturn x;\n", "int x = 1;\nx++;\nreturn x;\n", 0),
               "int f(void)\n{\n    int x = 1;   \n    x++;\n    return x;\n}\n");
        expect(t, "trim note", istr(a, find(repnote(a, src, "int x = 1;\nreturn x;", "return 0;"),
                                            "ignoring indentation")>0), "1");
        Str py = "def f():\n\tif x:\n\t\ty = 1\n";
        expect(t, "trim tabs", rep(a, py, "    if x:\n        y = 1\n", "    if x:\n        y = 2\n", 0),
               "def f():\n\tif x:\n\t    y = 2\n");
        expect(t, "comment", rep(a, "a\n# old code\nb\n", "// old code\n", "// new code\n", 0),
               "a\n// new code\nb\n");
        expect(t, "norm bytes", rep(a, "say \xe2\x80\x9chi\xe2\x80\x9d\n", "\"hi\"", "'yo'", 0),
               "say 'yo'\n");
        expect(t, "norm lines", rep(a, "a\n  x \xe2\x80\x94  y\nb\n", "x - y\n", "z\n", 0),
               "a\n  z\nb\n");
        // Whole lines: a newline at the end of old_string or not
        expect(t, "lines no newline", rep(a, "a\nb  \nc\n", "b\nc", "", 0), "a\n\n");
        expect(t, "lines newline", rep(a, "a\nb  \nc\nd\n", "b\nc\n", "", 0), "a\nd\n");
        // CRLF files get CRLF lines, and keep their BOM
        // (one line indented alike, one not: a tie, so as given)
        expect(t, "crlf added", rep(a, "\xef\xbb\xbf" "a\r\n  b\r\nc\r\n", "a\nb \n", "a\nB\nB2\n", 0),
               "\xef\xbb\xbf" "a\r\nB\r\nB2\r\nc\r\n");
        expect(t, "crlf reindented", rep(a, "\xef\xbb\xbf" "a\r\n  b\r\n  c\r\n", "b\nc \n",
                                         "b\nB2\nc\n", 0),
               "\xef\xbb\xbf" "a\r\n  b\r\n  B2\r\n  c\r\n");
    }

    // Ambiguity is shown, never guessed
    {
        Str src = "x = 1;\ny = 2;\nx = 1;\nz = 3;\nx = 1;\nw\nx = 1;\nv\nx = 1;\nu\nx = 1;\n";
        Str msg = repnote(a, src, "x = 1;", "x = 9;");
        expect(t, "ambiguous count", istr(a, find(msg, "matches 6 places in f.c (lines 1, 3, 5, 7, 9, 11)")>0), "1");
        expect(t, "ambiguous previews", istr(a, find(msg, "5. line 9:")>0 && find(msg, "6. line")<0 &&
                                                find(msg, "(and 1 more)")>0), "1");
        expect(t, "ambiguous context", istr(a, find(msg, "1. line 1:\n     1\tx = 1;\n     2\ty = 2;\n")>0), "1");
        expect(t, "replace all", rep(a, "a\nb\na\n", "a\n", "c\n", 1), "c\nb\nc\n");
        expect(t, "loose ambiguous", istr(a, startswith(rep(a, "  a\nb\n    a\n", "a\n", "c\n", 0),
                                                        "!old_string matches 2 places")), "1");
        expect(t, "loose replace all", rep(a, "  a\nb\n    a\n", "a\n", "c\n", 1), "  c\nb\n    c\n");
    }

    // Fuzzy: used when clearly the place, else shown
    {
        Str src = "static int add(int a, int b)\n{\n    return a + b;\n}\n\n"
                  "static int mul(int a, int b)\n{\n    return a * b;\n}\n";
        // One character off in a long line, the change elsewhere: the
        // file's line is kept, only the changed line is the agent's
        Str got = rep(a, src, "static int mul(int a, int c)\n{\n    return a * b;\n",
                      "static int mul(int a, int c)\n{\n    return a * b * 2;\n", 0);
        expect(t, "fuzzy win", got, "static int add(int a, int b)\n{\n    return a + b;\n}\n\n"
                                    "static int mul(int a, int b)\n{\n    return a * b * 2;\n}\n");
        Str note = repnote(a, src, "static int mul(int a, int c)\n{\n    return a * b;\n",
                           "static int mul(int a, int c)\n{\n    return a * b * 2;\n");
        expect(t, "fuzzy note", istr(a, startswith(note, "Edited f.c: replaced 1 match at line 6, "
                                                   "approximately.") &&
                                        find(note, "% alike")>0 && find(note, "     8\t    return a * b * 2;")>0), "1");
        // Two places equally close: not guessed
        Str two = "int alpha_beta_gamma = compute(one, two, three);\n"
                  "int other = 0;\n"
                  "int alpha_beta_gamma = compute(one, two, three);\n";
        Str amb = rep(a, two, "int alpha_beta_gamma = compute(one, two, threE);", "x", 0);
        expect(t, "fuzzy ambiguous", istr(a, startswith(amb, "!old_string was not found exactly") &&
                                             find(amb, "1. line 1, ")>0 && find(amb, "2. line 3, ")>0), "1");
        // Too far off to use, but shown as the closest
        Str far = rep(a, src, "static int mul(long a, long b)\n{\n", "x", 0);
        expect(t, "fuzzy closest", istr(a, startswith(far, "!old_string was not found in f.c") &&
                                           find(far, "The closest text, ")>0 && find(far, "is at lines 6-7")>0), "1");
        expect(t, "first line hint", istr(a, find(rep(a, "abcd\nxyz\n", "abcd\nQQQQQQQQQQQQQQQQQQQ\n", "x", 0),
                                                  "Its first line matches line 1")>0), "1");
        expect(t, "fuzzy not with all", istr(a, startswith(rep(a, src, "static int mul(int a, int c)\n", "x", 1),
                                                           "!old_string was not found exactly in f.c, and "
                                                           "replace_all needs exact matches")), "1");
    }

    // Kept lines: only what the agent changed changes
    {
        Slice<Str> old = givenlines(a, "a\nb\nc\n");
        Slice<Str> now = givenlines(a, "a\nB\nc\nd\n");
        Text tx = splittext(a, "  a \n  b\n  c\t\n");
        Indent in = {"", "  "};
        Slice<Str> k = keeplines(a, &tx, 0, old, now, in);
        expect(t, "keeplines", jointext(a, &tx, k), "  a \n  B\n  c\t\n  d\n");
    }

    // Tags
    {
        Str tag = filetag(a, "a\nb\n");
        expect(t, "tag form", istr(a, tag.len), "4");
        expect(t, "tag ignores trailing blanks", filetag(a, "a  \r\nb\t\n"), tag);
        expect(t, "tag ignores bom", filetag(a, "\xef\xbb\xbf" "a\nb\n"), tag);
        expect(t, "tag sees content", istr(a, filetag(a, "a\nc\n") != tag), "1");
        expect(t, "tag sees lines", istr(a, filetag(a, "a\nb") != tag), "1");
        expect(t, "tagof", tagof(" [src/x.c#AB12] "), "AB12");
    }

    // hashline
    {
        Str src = "one\ntwo\nthree\nfour\n";
        Str tag = filetag(a, src);
        HashEdit e1[] = {hedit(HASH_REPLACE, 2, 3, "TWO\nTHREE\nTHREE+\n")};
        expect(t, "hash replace", hashrun(a, src, tag, e1, 1), "one\nTWO\nTHREE\nTHREE+\nfour\n");
        HashEdit e2[] = {hedit(HASH_INSERT, 0, 0, "zero"), hedit(HASH_INSERT, 4, 4, "five\n"),
                         hedit(HASH_DELETE, 2, 2, {}), hedit(HASH_INSERT, 2, 2, "2a\n2b")};
        expect(t, "hash several", hashrun(a, src, tag, e2, 4), "zero\none\n2a\n2b\nthree\nfour\nfive\n");
        HashEdit e3[] = {hedit(HASH_REPLACE, 1, 1, "")};
        expect(t, "hash empty text", hashrun(a, src, tag, e3, 1), "two\nthree\nfour\n");
        HashEdit e4[] = {hedit(HASH_REPLACE, 1, 1, "\n")};
        expect(t, "hash blank line", hashrun(a, src, tag, e4, 1), "\ntwo\nthree\nfour\n");
        expect(t, "hash stale", istr(a, startswith(hashrun(a, src, "0000", e1, 1),
                                                   "!The tag 0000 does not match f.c")), "1");
        expect(t, "hash tag case", hashrun(a, src, concat(a, "f.c#", tag), e1, 1),
               "one\nTWO\nTHREE\nTHREE+\nfour\n");
        HashEdit e5[] = {hedit(HASH_REPLACE, 1, 2, "x"), hedit(HASH_DELETE, 2, 3, {})};
        Str ov = hashrun(a, src, tag, e5, 2);
        expect(t, "hash overlap", istr(a, find(ov, "lines 1-2 and 2-3 overlap")>0), "1");
        HashEdit e6[] = {hedit(HASH_REPLACE, 1, 3, "x"), hedit(HASH_INSERT, 2, 2, "y")};
        expect(t, "hash insert inside", istr(a, find(hashrun(a, src, tag, e6, 2),
                                                     "insert after line 2 falls inside")>0), "1");
        HashEdit e7[] = {hedit(HASH_REPLACE, 1, 3, "x"), hedit(HASH_INSERT, 3, 3, "y")};
        expect(t, "hash insert after", hashrun(a, src, tag, e7, 2), "x\ny\nfour\n");
        HashEdit e8[] = {hedit(HASH_DELETE, 4, 5, {})};
        expect(t, "hash past end", istr(a, find(hashrun(a, src, tag, e8, 1),
                                                "lines 4-5 are not lines of the file, which has 4 lines")>0), "1");
        HashEdit e9[] = {hedit(HASH_REPLACE, 2, 2, "two")};
        expect(t, "hash no change", istr(a, startswith(hashrun(a, src, tag, e9, 1),
                                                       "!These edits leave f.c as it was")), "1");
        // Endings: CRLF, no final newline, BOM
        Str crlf = "\xef\xbb\xbf" "a\r\nb";
        HashEdit e10[] = {hedit(HASH_INSERT, 2, 2, "c"), hedit(HASH_REPLACE, 1, 1, "A")};
        expect(t, "hash crlf", hashrun(a, crlf, filetag(a, crlf), e10, 2), "\xef\xbb\xbf" "A\r\nb\r\nc");
        HashEdit e11[] = {hedit(HASH_INSERT, 0, 0, "first")};
        expect(t, "hash empty file", hashrun(a, "", filetag(a, ""), e11, 1), "first\n");
        // The result: the new tag and the changed lines, renumbered
        Slice<HashEdit> one = {};
        one = push(a, one, hedit(HASH_INSERT, 0, 0, "zero"));
        EditOut o = edithash(a, src, "f.c", tag, one);
        Buf want(a, 256);
        print(&want, "Edited f.c (1 edit). Its new tag and the changed lines, numbered "
                     "as the file is now:\n[f.c#");
        print(&want, filetag(a, o.data));
        print(&want, "]\n     1\tzero\n     2\tone\n     3\ttwo\n");
        expect(t, "hash result", o.text, finish(&want));
    }

    // Parsing patches
    {
        Patch p = parsepatch(a, "cat <<'EOF'\n*** Begin Patch\n*** Add File: n.txt\n+hi\n\n+there\n"
                                "*** Delete File: old.txt\n*** Update File: a.c\n*** Move to: b.c\n"
                                "@@ -3,4 +3,4 @@ int main()\n-x\n+y\n*** End Patch\nEOF\n");
        expect(t, "patch parsed", p.err, Str{});
        expect(t, "patch files", istr(a, p.files.len), "3");
        expect(t, "patch add", p.files[0].content, "hi\n\nthere\n");
        expect(t, "patch move", concat(a, concat(a, p.files[2].path, ">"), p.files[2].to), "a.c>b.c");
        expect(t, "patch unified ctx", p.files[2].hunks[0].ctx, "int main()");
        Patch q = parsepatch(a, "```\n*** Begin Patch\n*** Update File: a.c\n@@ -1,2 +1,2 @@\n x\n-y\n+z\n"
                                "*** End of File\n*** End Patch\n```");
        expect(t, "patch fence", q.err, Str{});
        expect(t, "patch no ctx", istr(a, !q.files[0].hunks[0].ctx.data && q.files[0].hunks[0].eof), "1");
        Patch ne = parsepatch(a, "\n*** Update File: a\n-x\n+y\n\n```\n");
        expect(t, "patch no envelope", concat(a, ne.err, istr(a, ne.files.len ?
               ne.files[0].hunks[0].lines.len : -1)), "2");
        expect(t, "patch no begin", parsepatch(a, "x = 1\n-x\n").err,
               "it does not start with a line \"*** Begin Patch\"");
        expect(t, "patch no end", parsepatch(a, "*** Begin Patch\n*** Update File: a\n-x\n").err,
               "it does not end with a line \"*** End Patch\"");
        expect(t, "patch before", parsepatch(a, "Here it is:\n*** Begin Patch\n*** Delete File: a\n*** End Patch").err,
               "line 1 of the patch: unexpected text before *** Begin Patch");
        expect(t, "patch header", parsepatch(a, "*** Begin Patch\n*** Rename File: a\n*** End Patch").err,
               "line 2 of the patch: unknown header: a file starts with *** Add File:, *** Delete "
               "File: or *** Update File:");
        expect(t, "patch no change", parsepatch(a, "*** Begin Patch\n*** Update File: a\n@@\n x\n*** End Patch").err,
               "line 2 of the patch: this update changes nothing: mark lines to remove with - and "
               "lines to add with +");
        expect(t, "patch twice", parsepatch(a, "*** Begin Patch\n*** Delete File: a\n*** Update File: a\n"
                                               "-x\n*** End Patch").err,
               "line 3 of the patch: a file appears twice: give each file one section, with all its hunks");
        expect(t, "patch add plus", parsepatch(a, "*** Begin Patch\n*** Add File: a\nno plus\n*** End Patch").err,
               "line 3 of the patch: each line of an added file starts with +");
        Slice<Str> paths = patchpaths(a, "*** Begin Patch\n*** Update File: a\n*** Move to: b\n-x\n"
                                         "*** Add File: c\n+y\n*** End Patch\n");
        expect(t, "patchpaths", istr(a, paths.len==3 && paths[1]=="b" && paths[2]=="c"), "1");
    }

    // Applying hunks
    {
        Str src = "def f():\n    a = 1\n    b = 2\n    return a\n\ndef g():\n    a = 1\n    return a\n";
        expect(t, "hunk exact", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@ def g():\n"
                                                 "     a = 1\n-    return a\n+    return -a\n*** End Patch"),
               "def f():\n    a = 1\n    b = 2\n    return a\n\ndef g():\n    a = 1\n    return -a\n");
        expect(t, "hunk no @@", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n"
                                                 "     a = 1\n-    return a\n+    return 0\n*** End Patch"),
               "def f():\n    a = 1\n    b = 2\n    return a\n\ndef g():\n    a = 1\n    return 0\n");
        expect(t, "hunk first ok", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n"
                                                    "     a = 1\n-    b = 2\n+    b = 3\n*** End Patch"),
               "def f():\n    a = 1\n    b = 3\n    return a\n\ndef g():\n    a = 1\n    return a\n");
        // Indentation off by the same amount throughout: kept lines stay
        expect(t, "hunk trim", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@\n"
                                                " def g():\n   a = 1\n-  return a\n+  a += 1\n+  return a\n"
                                                "*** End Patch"),
               "def f():\n    a = 1\n    b = 2\n    return a\n\ndef g():\n    a = 1\n    a += 1\n    return a\n");
        // Context lines without their space, and two hunks in order
        expect(t, "hunk lenient", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@\ndef f():\n"
                                                   "-    a = 1\n+    a = 9\n@@\n     a = 1\n-    return a\n"
                                                   "+    return 7\n*** End Patch"),
               "def f():\n    a = 9\n    b = 2\n    return a\n\ndef g():\n    a = 1\n    return 7\n");
        expect(t, "hunk eof", patchrun(a, "x\ny\nx\ny\n", "*** Begin Patch\n*** Update File: f.c\n@@\n"
                                                          " x\n-y\n+z\n*** End of File\n*** End Patch"),
               "x\ny\nx\nz\n");
        expect(t, "hunk add at end", patchrun(a, "a\n", "*** Begin Patch\n*** Update File: f.c\n@@\n+b\n"
                                                        "*** End Patch"), "a\nb\n");
        expect(t, "hunk add after ctx", patchrun(a, "a\nb\n", "*** Begin Patch\n*** Update File: f.c\n@@ a\n"
                                                              "+a2\n*** End Patch"), "a\na2\nb\n");
        expect(t, "hunk crlf", patchrun(a, "a\r\nb\r\n", "*** Begin Patch\n*** Update File: f.c\n-a\n+A\n"
                                                         "+A2\n b\n*** End Patch"), "A\r\nA2\r\nb\r\n");
        expect(t, "hunk trailing empty", patchrun(a, "a\nb", "*** Begin Patch\n*** Update File: f.c\n a\n"
                                                             "-b\n+c\n \n*** End Patch"), "a\nc");
        Str miss = patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@\n     a = 1\n-    b = 3\n"
                                    "+    b = 4\n*** End Patch");
        expect(t, "hunk missing", istr(a, startswith(miss, "!hunk 1 of f.c (patch line 3): its kept and "
                                                     "removed lines were not found together") &&
                                          find(miss, "The closest text, ")>0), "1");
        Str ctx = patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@ def h():\n-    a = 1\n"
                                   "+    a = 2\n*** End Patch");
        expect(t, "hunk ctx missing", istr(a, startswith(ctx, "!hunk 1 of f.c (patch line 3): the line "
                                                         "after @@, \"def h():\", was not found")), "1");
        // A kept line that lost its marker space looks indented one less:
        // the line added keeps its own indentation (seen from qwen)
        expect(t, "hunk lost space", patchrun(a, "  a\n  b\n  c\n", "*** Begin Patch\n"
                                                 "*** Update File: f.c\n@@\n   a\n+  x\n  b\n*** End Patch"),
               "  a\n  x\n  b\n  c\n");
        // The line to change given after @@ as well (seen from qwen-flash)
        expect(t, "hunk at its @@", patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n"
                                                     "@@     b = 2\n-    b = 2\n+    b = 3\n*** End Patch"),
               "def f():\n    a = 1\n    b = 3\n    return a\n\ndef g():\n    a = 1\n    return a\n");
        Str order = patchrun(a, src, "*** Begin Patch\n*** Update File: f.c\n@@ def g():\n-    a = 1\n"
                                     "+    a = 5\n@@ def f():\n-    b = 2\n+    b = 6\n*** End Patch");
        expect(t, "hunk order", istr(a, startswith(order, "!hunk 2 of f.c") &&
                                        find(order, "was not found after line 7")>0), "1");
    }
}

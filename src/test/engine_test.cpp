// Engine tests: the pure parts (the turn machine itself runs end to end
// in tests/scenarios.py)

static void test_engine(Test *t)
{
    Arena *a = &t->perm;

    Str long60 = "Refactor the parser so that it handles nested quotes and "
                 "escapes correctly everywhere";
    struct { Str in; Str want; } titles[] = {
        {"go", "go"},
        {"  fix   the\tbug \n", "fix the bug"},
        {"line one\nline two\r\nline three", "line one line two line three"},
        {"", ""},
        {" \n\t ", ""},
        {long60, "Refactor the parser so that it handles nested quotes and\xe2\x80\xa6"},
        // A word that ends exactly at the limit, then more
        {"aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff ggg",
         "aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff\xe2\x80\xa6"},
        // Code points, not bytes
        {"\xc3\xa9\xc3\xa9\xc3\xa9", "\xc3\xa9\xc3\xa9\xc3\xa9"},
        {"tab\x7f" "del\xc2\xa0nbsp", "tab del nbsp"},
    };
    for (iz i = 0; i < countof(titles); i++) {
        expect(t, concat(a, "title ", titles[i].in), maketitle(a, titles[i].in),
               titles[i].want);
    }

    // Exactly 60 code points of two-byte characters: no ellipsis
    Buf b(a, 256);
    for (i32 i = 0; i < 60; i++) {
        print(&b, "\xc3\xa9");
    }
    Str sixty = finish(&b);
    expect(t, "title of 60 code points", maketitle(a, sixty), sixty);
    Str more = concat(a, sixty, "\xc3\xa9");
    expect(t, "title of 61 code points",
           maketitle(a, more), concat(a, sixty, "\xe2\x80\xa6"));

    // No space in its second half: cut inside the long word
    Buf in(a, 128);
    print(&in, "x ");
    for (i32 i = 0; i < 70; i++) {
        putbyte(&in, 'a');
    }
    Str word = finish(&in);
    expect(t, "title cut in a word", maketitle(a, word),
           concat(a, takehead(word, 60), "\xe2\x80\xa6"));
}

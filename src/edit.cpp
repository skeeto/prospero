// Edits: the edit tool's three formats, as changes to a file's text
//
// The format is chosen per model ([model] edit_format), since which one
// a model handles best differs from model to model (tests/bench).
//
// replace: old_string and new_string. The old text is looked for by a
// ladder of rules, each tried only when those before it found nothing:
// the exact bytes; whole lines ignoring trailing whitespace; ignoring
// indentation too; ignoring comment markers too; and with typographic
// quotes, dashes and spaces as ASCII. These are oh-my-pi's rules, with
// its confidences of 1.0 down to 0.97 (fuzzy.rs), and every place one
// of them finds counts, so that two places are ambiguous. Last comes
// the fuzzy rule: runs of lines compared by edit distance, where the
// best place is used only if it is 97% alike or more and 8 points ahead
// of the next; other places at least 95% alike are shown as candidates,
// never guessed at. Where lines matched loosely, lines that old_string
// and new_string have in common stay as the file has them, so that a
// loose match changes only what the agent changed, and the lines it
// added are indented as the file is.
//
// patch: the apply_patch envelope of Codex. Each hunk is looked for
// after the one before it, by the passes OpenCode uses: exact, trailing
// whitespace ignored, both ends ignored, then the normal form. Context
// lines keep the file's text, as above.
//
// hashline: read shows a file under a [PATH#TAG] header, where TAG is a
// hash of its content, and edits address its lines by number. A tag
// that no longer matches means the file changed since it was read, and
// nothing is changed.
//
// Every format checks everything before anything is written, and keeps
// line endings and a byte order mark as the file has them.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    EDIT_REPLACE,
    EDIT_PATCH,
    EDIT_HASHLINE,
    EDIT_COUNT,
};

static Str editnames[] = {"replace", "patch", "hashline"};
static_assert(countof(editnames) == EDIT_COUNT);

// A format by name; replace for anything else.
static i32 editformat(Str name)
{
    name = trim(name);
    for (i32 i = 0; i < EDIT_COUNT; i++) {
        if (equalfold(name, editnames[i])) {
            return i;
        }
    }
    return EDIT_REPLACE;
}

// The rules of the replace ladder, in order, with oh-my-pi's confidences
// in thousandths. Patch hunks use EXACT, RTRIM, TRIM and NORM.
enum : i32 {
    RUNG_EXACT,    // 1000: the bytes
    RUNG_RTRIM,    //  990: lines, ignoring trailing whitespace
    RUNG_TRIM,     //  980: lines, ignoring indentation too
    RUNG_COMMENT,  //  975: lines, ignoring comment markers too
    RUNG_NORM,     //  970: typographic quotes, dashes and spaces as ASCII
    RUNG_FUZZY,    //       lines, by edit distance
};

enum : i64 {
    FUZZ_MIN      = 950,     // thousandths alike to be a candidate
    FUZZ_WIN      = 970,     // to be used
    FUZZ_LEAD     = 80,      // ahead of the next candidate, to be used
    FUZZ_SUGGEST  = 750,     // to be shown as the closest text
    FUZZ_CHARS    = 6,       // old text needed, in normal form
    FUZZ_LINES    = 20000,   // largest file compared by edit distance
    FUZZ_WANT     = 200,     // most lines of old text so compared
    EDIT_PREVIEWS = 5,       // places shown
    EDIT_PREVIEWW = 80,      // bytes of a line shown in one
    EDIT_KEEPMAX  = 1 << 20, // largest table for finding shared lines
    EDIT_SHOWN    = 60,      // changed lines shown after a hashline edit
    EDIT_LOOSEMAX = 8 << 20, // largest file matched loosely
    EDIT_LINESMAX = 16 << 20,// largest file edited by line (patch, hashline)
    HASH_EDITS    = 100,     // edits in one hashline call
};


// Text

static iz countlines(Str s)
{
    iz n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s[i] == '\n';
    }
    return n + (s.len && s[s.len-1]!='\n');
}

// Cut at most max bytes, backing up to a UTF-8 boundary.
static Str utf8head(Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    iz n = max;
    for (; n>0 && (s[n]&0xc0)==0x80; n--) {}
    return takehead(s, n);
}

static Str utf8tail(Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    iz n = s.len - max;
    for (; n<s.len && (s[n]&0xc0)==0x80; n++) {}
    return cuthead(s, n);
}

// The line number, from 1, of a byte offset.
static iz lineat(Str s, iz off)
{
    iz n = 1;
    for (iz i = 0; i < off; i++) {
        n += s[i] == '\n';
    }
    return n;
}

static Str lfcrlf(Arena *a, Str s)
{
    Buf b(a, s.len + 64);
    for (iz i = 0; i < s.len; i++) {
        if (s[i]=='\n' && (!i || s[i-1]!='\r')) putbyte(&b, '\r');
        putbyte(&b, s[i]);
    }
    return finish(&b);
}

static Str rtrimline(Str s)
{
    for (; s.len && (s[s.len-1]==' ' || s[s.len-1]=='\t' || s[s.len-1]=='\r'); s.len--) {}
    return s;
}

// A line without the CR of a CRLF.
static Str bare(Str line)
{
    return line.len && line[line.len-1]=='\r' ? cuttail(line, 1) : line;
}

static Str indentof(Str s)
{
    iz i = 0;
    for (; i<s.len && (s[i]==' ' || s[i]=='\t'); i++) {}
    return takehead(s, i);
}

// Edit distance, ignoring ASCII case if fold, or limit+1 once past limit.
static iz distance(Arena scratch, Str x, Str y, iz limit, b32 fold)
{
    if (x.len-y.len>limit || y.len-x.len>limit) {
        return limit + 1;
    }
    iz *row = alloc<iz>(&scratch, y.len+1);
    for (iz j = 0; j <= y.len; j++) {
        row[j] = j;
    }
    for (iz i = 1; i <= x.len; i++) {
        iz diag = row[0];
        iz best = row[0] = i;
        for (iz j = 1; j <= y.len; j++) {
            u8 cx   = fold ? lowercase(x[i-1]) : x[i-1];
            u8 cy   = fold ? lowercase(y[j-1]) : y[j-1];
            iz up   = row[j];
            iz v    = diag + (cx != cy);
            v = up+1<v ? up+1 : v;
            v = row[j-1]+1<v ? row[j-1]+1 : v;
            row[j] = v;
            diag   = up;
            best   = v<best ? v : best;
        }
        if (best > limit) {
            return limit + 1;
        }
    }
    return row[y.len];
}

// A file's text as lines, each without its newline but keeping a CR
// before it, so that lines left alone are written back byte for byte.
struct Text {
    Str        bom;
    Slice<Str> lines;
    Str        tail;   // the last line, if no newline ends it
    b32        crlf;   // lines added end in CRLF, as most lines do
    b32        final;  // the last line ends in a newline
};

static Text splittext(Arena *a, Str data)
{
    Text r = {};
    if (startswith(data, "\xef\xbb\xbf")) {
        r.bom = takehead(data, 3);
        data  = cuthead(data, 3);
    }
    iz crs = 0, lfs = 0;
    r.final = 1;  // an empty file gains whole lines
    for (Str rest = data; rest.len;) {
        Cut c = cut(rest, '\n');
        r.lines = push(a, r.lines, c.head);
        lfs    += c.ok;
        crs    += c.ok && c.head.len && c.head[c.head.len-1]=='\r';
        r.final = c.ok;
        rest    = c.tail;
    }
    r.tail = r.final ? Str{} : r.lines[r.lines.len-1];
    r.crlf = crs && crs*2>=lfs;
    return r;
}

// The text again, with lines changed. The old last line, if no newline
// ended it, gains the file's ending when lines follow it now; a new
// last line stays without one, as the old one was.
static Str jointext(Arena *a, Text *t, Slice<Str> lines)
{
    iz size = t->bom.len;
    for (iz i = 0; i < lines.len; i++) {
        size += lines[i].len + 2;
    }
    Buf b(a, size + 1);
    print(&b, t->bom);
    for (iz i = 0; i < lines.len; i++) {
        Str line = lines[i];
        b32 tail = t->tail.data && line.data==t->tail.data && line.len==t->tail.len;
        if (i+1==lines.len && !t->final) {
            print(&b, tail ? line : bare(line));
        } else {
            print(&b, line);
            print(&b, tail && t->crlf ? Str("\r\n") : Str("\n"));
        }
    }
    return finish(&b);
}

// A line the agent gave, to add to the file: with a CR if lines there
// end in CRLF.
static Str addline(Arena *a, Text *t, Str line)
{
    return t->crlf ? concat(a, line, "\r") : line;
}

// The lines of text the agent gave: CRs dropped, and a newline at the
// end ends the last line rather than starting another. Empty is none.
static Slice<Str> givenlines(Arena *a, Str s)
{
    Slice<Str> r = {};
    if (!s.len) {
        return r;
    }
    if (s[s.len-1] == '\n') {
        s = cuttail(s, 1);
    }
    for (Str rest = s;;) {
        Cut c = cut(rest, '\n');
        r = push(a, r, bare(c.head));
        if (!c.ok) break;
        rest = c.tail;
    }
    return r;
}


// Normal forms

static i32 normcp(i32 c)
{
    switch (c) {
    case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032:
        return '\'';
    case 0x201c: case 0x201d: case 0x201e: case 0x201f: case 0x2033:
        return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212:
        return '-';
    case 0x00a0: case 0x2007: case 0x202f:
        return ' ';
    }
    return c>=0x2000 && c<=0x200a ? ' ' : c;
}

// A trimmed line without the comment markers at its start and end.
static Str uncomment(Str s)
{
    static Str const marks[] = {"<!--", "//", "/*", "--", "#", ";", "*"};
    for (iz i = 0; i < countof(marks);) {
        if (startswith(s, marks[i])) {
            s = trimleft(cuthead(s, marks[i].len));
            i = 0;
        } else {
            i++;
        }
    }
    if (endswith(s, "*/")) s = trimright(cuttail(s, 2));
    if (endswith(s, "-->")) s = trimright(cuttail(s, 3));
    return s;
}

// Trimmed, with normcp applied and each run of blanks as one space.
// Other code points keep their bytes, invalid UTF-8 included.
static Str normtext(Arena *a, Str s)
{
    s = trim(s);
    Buf b(a, s.len);
    b32 blank = 0;
    for (iz i = 0; i < s.len;) {
        iz  at = i;
        i32 cp = utf8decode(s, &i);
        i32 c  = normcp(cp);
        if (c==' ' || c=='\t') {
            blank = 1;
            continue;
        }
        if (blank) {
            putbyte(&b, ' ');
            blank = 0;
        }
        if (c != cp) {
            putbyte(&b, (u8)c);  // normcp maps only to ASCII
        } else {
            print(&b, span(s.data+at, s.data+i));
        }
    }
    return finish(&b);
}

// A line in the normal form of a rule.
static Str normline(Arena *a, Str line, i32 rung)
{
    line = bare(line);
    switch (rung) {
    case RUNG_EXACT:   return line;
    case RUNG_RTRIM:   return rtrimline(line);
    case RUNG_TRIM:    return trim(line);
    case RUNG_COMMENT: return uncomment(trim(line));
    }
    return normtext(a, line);
}

static Slice<Str> normlines(Arena *a, Slice<Str> lines, i32 rung)
{
    Slice<Str> r = {};
    r.data = alloc<Str>(a, lines.len);
    r.len  = r.cap = lines.len;
    for (iz i = 0; i < lines.len; i++) {
        r[i] = normline(a, lines[i], rung);
    }
    return r;
}


// Finding text

struct Span {
    iz beg;
    iz end;
};

static Slice<Span> matchexact(Arena *a, Str hay, Str needle)
{
    Slice<Span> r = {};
    for (iz off = 0; off+needle.len <= hay.len;) {
        iz i = find(cuthead(hay, off), needle);
        if (i < 0) break;
        Span s = {off+i, off+i+needle.len};
        r = push(a, r, s);
        off = s.end;
    }
    return r;
}

// Code points compared after normcp; spans are bytes of hay.
static Slice<Span> matchnorm(Arena *a, Str hay, Str needle)
{
    Slice<Span> r = {};
    if (!needle.len) return r;
    for (iz beg = 0; beg < hay.len;) {
        iz i = beg, j = 0;
        while (i<hay.len && j<needle.len) {
            iz ii = i, jj = j;
            if (normcp(utf8decode(hay, &ii)) != normcp(utf8decode(needle, &jj))) {
                break;
            }
            i = ii;
            j = jj;
        }
        if (j == needle.len) {
            Span s = {beg, i};
            r = push(a, r, s);
            beg = i;
            continue;
        }
        iz next = beg;
        utf8decode(hay, &next);
        beg = next;
    }
    return r;
}

// Runs of have's lines equal to want's, not overlapping, as line
// numbers from 0, end exclusive.
static Slice<Span> matchrun(Arena *a, Slice<Str> have, Slice<Str> want)
{
    Slice<Span> r = {};
    if (!want.len) {
        return r;
    }
    for (iz i = 0; i+want.len <= have.len;) {
        iz k = 0;
        for (; k<want.len && have[i+k]==want[k]; k++) {}
        if (k < want.len) {
            i++;
            continue;
        }
        Span s = {i, i+want.len};
        r = push(a, r, s);
        i += want.len;
    }
    return r;
}

static b32 blanklines(Slice<Str> lines)
{
    for (iz i = 0; i < lines.len; i++) {
        if (trim(lines[i]).len) {
            return 0;
        }
    }
    return 1;
}

// A run of lines and how alike it is to the text looked for.
struct Near {
    iz  beg;    // lines from 0, end exclusive
    iz  end;
    i64 score;  // thousandths alike
};

static b32 nearless(Near *a, Near *b)
{
    return a->score > b->score;
}

// How alike a run of have's lines from at is to want's, in thousandths:
// one less their edit distance over the longer one's length, each line
// counting one more for its break. Returns 0 once below least.
static i64 alike(Arena scratch, Slice<Str> have, iz at, Slice<Str> want,
                 i64 wlen, i64 hlen, i64 least)
{
    i64 total = wlen>hlen ? wlen : hlen;
    i64 limit = total * (1000 - least) / 1000;
    if (wlen-hlen>limit || hlen-wlen>limit) {
        return 0;
    }
    i64 d = 0;
    for (iz k = 0; k < want.len; k++) {
        d += distance(scratch, have[at+k], want[k], (iz)(limit-d), 0);
        if (d > limit) {
            return 0;
        }
    }
    return total ? 1000 - d*1000/total : 1000;
}

// The best few runs of have's lines, as many as want's, at least least
// alike, best first, leaving out any that overlap a better one: enough
// to show, and one more to tell whether the best is clearly so. Both are
// lines in the normal form RUNG_NORM. Nothing when want is too short to
// tell, or either is too long to compare.
static Slice<Near> nearby(Arena *a, Slice<Str> have, Slice<Str> want, i64 least)
{
    Slice<Near> r = {};
    i64 wlen = 0;
    for (iz k = 0; k < want.len; k++) {
        wlen += want[k].len + 1;
    }
    if (!want.len || wlen-want.len<FUZZ_CHARS || want.len>FUZZ_WANT ||
            have.len>FUZZ_LINES || want.len>have.len) {
        return r;
    }
    r.cap  = EDIT_PREVIEWS + 1;
    r.data = alloc<Near>(a, r.cap);

    Arena scratch = *a;
    i64  *sums    = alloc<i64>(&scratch, have.len+1);  // line lengths so far
    for (iz i = 0; i < have.len; i++) {
        sums[i+1] = sums[i] + have[i].len + 1;
    }
    Slice<Near> all = {};
    for (iz i = 0; i+want.len <= have.len; i++) {
        i64 hlen  = sums[i+want.len] - sums[i];
        i64 score = alike(scratch, have, i, want, wlen, hlen, least);
        if (score >= least) {
            Near n = {i, i+want.len, score};
            all = push(&scratch, all, n);
        }
    }
    sort(all.data, all.len, nearless, scratch);
    for (iz i = 0; i<all.len && r.len<r.cap; i++) {
        b32 apart = 1;
        for (iz k = 0; apart && k<r.len; k++) {
            apart = r[k].end<=all[i].beg || all[i].end<=r[k].beg;
        }
        if (apart) {
            r.data[r.len++] = all[i];
        }
    }
    return r;
}


// Changing lines

// The indentation of old text as the agent gave it (from) and as the
// file has it (to), from the first line of old text indented otherwise
// than the file's, for the lines the agent adds.
struct Indent {
    Str from;
    Str to;
};

static Indent indentpair(Slice<Str> old, Slice<Str> file, iz at)
{
    Indent r = {};
    for (iz k = 0; k < old.len; k++) {
        Str from = indentof(old[k]);
        Str to   = indentof(bare(file[at+k]));
        if (trim(old[k]).len && from!=to) {
            r.from = from;
            r.to   = to;
            break;
        }
    }
    return r;
}

static Str reindent(Arena *a, Str line, Indent in)
{
    if (in.from==in.to || !startswith(line, in.from) || !trim(line).len) {
        return line;
    }
    return concat(a, in.to, cuthead(line, in.from.len));
}

// The lines replacing the run of file lines from at that old matched
// loosely: lines old and now have in common (their longest common
// subsequence) stay as the file has them, the rest of now is added,
// indented as the file is, and the rest of old's lines go.
static Slice<Str> keeplines(Arena *a, Text *t, iz at, Slice<Str> old,
                            Slice<Str> now, Indent in)
{
    iz  n    = old.len;
    iz  m    = now.len;
    iz *pair = alloc<iz>(a, m+1);  // now's line to old's, or -1
    for (iz j = 0; j < m; j++) {
        pair[j] = -1;
    }
    if ((n+1) <= EDIT_KEEPMAX/(m+1)) {
        Arena tmp = *a;
        iz   *len = alloc<iz>(&tmp, (n+1)*(m+1));  // common lines of the tails
        for (iz i = n-1; i >= 0; i--) {
            for (iz j = m-1; j >= 0; j--) {
                iz *v = len + i*(m+1) + j;
                if (old[i] == now[j]) {
                    *v = v[m+2] + 1;
                } else {
                    iz down = v[m+1], right = v[1];
                    *v = down>right ? down : right;
                }
            }
        }
        for (iz i = 0, j = 0; i<n && j<m;) {
            iz *v = len + i*(m+1) + j;
            if (old[i] == now[j]) {
                pair[j++] = i++;
            } else if (v[m+1] >= v[1]) {
                i++;
            } else {
                j++;
            }
        }
    }

    Slice<Str> r = {};
    r.data = alloc<Str>(a, m);
    r.cap  = m;
    for (iz j = 0; j < m; j++) {
        Str line = pair[j]>=0 ? t->lines[at+pair[j]]
                              : addline(a, t, reindent(a, now[j], in));
        r.data[r.len++] = line;
    }
    return r;
}

// One numbered line, as read shows it, cut to width bytes if width.
static void numbered(Buf *b, iz n, Str line, iz width)
{
    line = bare(line);
    i32 digits = 1;
    for (iz v = n; v >= 10; v /= 10) digits++;
    for (i32 i = digits; i < 6; i++) putbyte(b, ' ');
    print(b, (i64)n);
    putbyte(b, '\t');
    if (width && line.len>width) {
        print(b, utf8head(line, width));
        print(b, "...");
    } else {
        print(b, line);
    }
    putbyte(b, '\n');
}

// Places in a file, numbered, each as its lines with one more around
// it, of its own only the first three and the last: lines are from 0,
// end exclusive. With scores, each says how alike it is.
static void previews(Buf *b, Text *t, Slice<Span> at, Slice<i64> scores)
{
    for (iz i = 0; i<at.len && i<EDIT_PREVIEWS; i++) {
        Span s = at[i];
        print(b, (i64)i+1);
        print(b, s.end-s.beg>1 ? Str(". lines ") : Str(". line "));
        print(b, (i64)s.beg+1);
        if (s.end-s.beg > 1) {
            putbyte(b, '-');
            print(b, (i64)s.end);
        }
        if (scores.len) {
            print(b, ", ");
            print(b, scores[i]/10);
            print(b, "% alike");
        }
        print(b, ":\n");
        iz beg = s.beg>0 ? s.beg-1 : 0;
        iz end = s.end<t->lines.len ? s.end+1 : t->lines.len;
        for (iz k = beg; k < end; k++) {
            if (k>=s.beg+3 && k<s.end-1) {
                if (k == s.beg+3) print(b, "   ...\n");
                continue;
            }
            numbered(b, k+1, t->lines[k], EDIT_PREVIEWW);
        }
    }
    if (at.len > EDIT_PREVIEWS) {
        print(b, "(and ");
        print(b, (i64)(at.len - EDIT_PREVIEWS));
        print(b, " more)\n");
    }
}

// Byte spans, in order, as the lines they cover.
static Slice<Span> spanlines(Arena *a, Str data, Slice<Span> m)
{
    Slice<Span> r    = {};
    iz          line = 0;  // newlines before off
    iz          off  = 0;
    for (iz i = 0; i < m.len; i++) {
        for (; off < m[i].beg; off++) {
            line += data[off] == '\n';
        }
        Span s = {line, line+1};
        for (iz k = m[i].beg; k < m[i].end-1; k++) {
            s.end += data[k] == '\n';
        }
        r = push(a, r, s);
    }
    return r;
}

// Lines from beg to end (from 0, exclusive) with two more around them,
// numbered, at most max of them.
static void showlines(Buf *b, Slice<Str> lines, iz beg, iz end, iz max)
{
    beg = beg>2 ? beg-2 : 0;
    end = end+2<lines.len ? end+2 : lines.len;
    for (iz k = beg; k<end && k-beg<max; k++) {
        numbered(b, k+1, lines[k], 0);
    }
    if (end-beg > max) {
        print(b, "   ...\n");
    }
}


// replace

struct EditOut {
    Str data;  // the new content, null if nothing is to change
    Str text;  // what the agent reads
};

static Str rungnote(i32 rung)
{
    switch (rung) {
    case RUNG_RTRIM:   return " (matched ignoring trailing whitespace)";
    case RUNG_TRIM:    return " (matched ignoring indentation)";
    case RUNG_COMMENT: return " (matched ignoring indentation and comment markers)";
    case RUNG_NORM:    return " (matched with typographic quotes, dashes and "
                              "spaces as ASCII)";
    }
    return "";
}

static Str ambiguous(Arena *a, Text *t, Str show, Slice<Span> at)
{
    Buf b(a, 1024);
    print(&b, "old_string matches ");
    print(&b, (i64)at.len);
    print(&b, " places in ");
    print(&b, show);
    print(&b, " (lines");
    for (iz i = 0; i<at.len && i<10; i++) {
        print(&b, i ? Str(", ") : Str(" "));
        print(&b, (i64)at[i].beg+1);
    }
    print(&b, at.len>10 ? Str(", ...") : Str(""));
    print(&b, "), so nothing was changed. Include more of the lines around "
              "the one you mean to make old_string unique, or set "
              "replace_all to true to change them all. The places:\n");
    previews(&b, t, at, {});
    return finish(&b);
}

// Splice new text over byte spans.
static Str splice(Arena *a, Str bom, Str data, Slice<Span> m, Str newstr)
{
    iz grow = 0;
    for (iz i = 0; i < m.len; i++) {
        grow += newstr.len - (m[i].end - m[i].beg);
    }
    Buf out(a, data.len + bom.len + (grow>0 ? grow : 0) + 64);
    print(&out, bom);
    iz last = 0;
    for (iz i = 0; i < m.len; i++) {
        print(&out, span(data.data+last, data.data+m[i].beg));
        print(&out, newstr);
        last = m[i].end;
    }
    print(&out, cuthead(data, last));
    return finish(&out);
}

static Str replaced(Arena *a, Str show, iz n, iz line, Str note)
{
    Buf b(a, 256);
    print(&b, "Edited ");
    print(&b, show);
    print(&b, ": replaced ");
    print(&b, (i64)n);
    print(&b, n==1 ? Str(" match at line ") : Str(" matches, the first at line "));
    print(&b, (i64)line);
    print(&b, note);
    putbyte(&b, '.');
    return finish(&b);
}

// The file's lines with each run in m, which want matched loosely,
// replaced by now as keeplines does, indented as the file is if indent.
static Slice<Str> relines(Arena *a, Text *t, Slice<Span> m, Slice<Str> want,
                          Slice<Str> now, b32 indent)
{
    Slice<Str> out  = {};
    iz         last = 0;
    for (iz i = 0; i < m.len; i++) {
        for (iz k = last; k < m[i].beg; k++) {
            out = push(a, out, t->lines[k]);
        }
        Indent     in   = indent ? indentpair(want, t->lines, m[i].beg) : Indent{};
        Slice<Str> kept = keeplines(a, t, m[i].beg, want, now, in);
        for (iz k = 0; k < kept.len; k++) {
            out = push(a, out, kept[k]);
        }
        last = m[i].end;
    }
    for (iz k = last; k < t->lines.len; k++) {
        out = push(a, out, t->lines[k]);
    }
    return out;
}

// Replace oldstr with newstr in a file's content (data), which show
// names to the agent; every match if all, else exactly one.
static EditOut editreplace(Arena *a, Str data, Str show, Str oldstr, Str newstr, b32 all)
{
    EditOut r     = {};
    Str     given = oldstr;
    Str     gnew  = newstr;
    Str     bom   = {};
    if (startswith(data, "\xef\xbb\xbf")) {
        bom  = takehead(data, 3);
        data = cuthead(data, 3);
    }
    if (find(data, "\r\n")>=0 && find(oldstr, "\r")<0) {
        oldstr = lfcrlf(a, oldstr);
        newstr = lfcrlf(a, newstr);
    }
    Text t     = splittext(a, data);
    t.bom      = bom;
    b32  loose = data.len <= EDIT_LOOSEMAX;

    // Whole lines, compared loosely. When old_string does not end in a
    // newline, the newline after the lines it matched stays, as it would
    // have for the bytes.
    Slice<Str> want = givenlines(a, given);
    if (given.len && given[given.len-1]!='\n') {
        gnew = concat(a, gnew, "\n");
    }
    Slice<Str> now = givenlines(a, gnew);

    for (i32 rung = RUNG_EXACT; rung <= RUNG_NORM; rung++) {
        // The bytes, exactly or in normal form, may be part of a line
        if (rung==RUNG_EXACT || (rung==RUNG_NORM && loose)) {
            Slice<Span> m = rung ? matchnorm(a, data, oldstr)
                                 : matchexact(a, data, oldstr);
            if (m.len>1 && !all) {
                r.text = ambiguous(a, &t, show, spanlines(a, data, m));
                return r;
            } else if (m.len) {
                r.data = splice(a, bom, data, m, newstr);
                r.text = replaced(a, show, m.len, lineat(data, m[0].beg), rungnote(rung));
                return r;
            }
        }
        if (rung==RUNG_EXACT || !loose || blanklines(want)) {
            continue;
        }
        Slice<Str>  have  = normlines(a, t.lines, rung);
        Slice<Str>  wantn = normlines(a, want, rung);
        Slice<Span> m     = matchrun(a, have, wantn);
        if (m.len>1 && !all) {
            r.text = ambiguous(a, &t, show, m);
            return r;
        } else if (m.len) {
            Slice<Str> out = relines(a, &t, m, want, now, rung>RUNG_RTRIM);
            r.data = jointext(a, &t, out);
            r.text = replaced(a, show, m.len, m[0].beg+1, rungnote(rung));
            return r;
        }
    }

    // By edit distance: used only when clearly the place meant
    Slice<Near> near = {};
    if (loose) {
        Slice<Str> have  = normlines(a, t.lines, RUNG_FUZZY);
        Slice<Str> wantn = normlines(a, want, RUNG_FUZZY);
        near = nearby(a, have, wantn, FUZZ_SUGGEST);
    }
    if (!all && near.len && near[0].score>=FUZZ_WIN &&
            (near.len==1 || near[0].score-near[1].score>=FUZZ_LEAD)) {
        Near        w   = near[0];
        Slice<Span> one = {};
        Span        s   = {w.beg, w.end};
        one = push(a, one, s);
        Slice<Str> out = relines(a, &t, one, want, now, 1);
        r.data = jointext(a, &t, out);

        Buf b(a, 1024);
        print(&b, "Edited ");
        print(&b, show);
        print(&b, ": replaced 1 match at line ");
        print(&b, (i64)w.beg+1);
        print(&b, ", approximately. old_string was not found exactly, but "
                  "lines ");
        print(&b, (i64)w.beg+1);
        putbyte(&b, '-');
        print(&b, (i64)w.end);
        print(&b, " are ");
        print(&b, w.score/10);
        print(&b, "% alike, so they were replaced. Lines that old_string and "
                  "new_string have in common were kept as the file had them. "
                  "Check that this is what you meant. Those lines now read:\n");
        iz added = out.len - t.lines.len + (w.end - w.beg);
        showlines(&b, out, w.beg, w.beg+added, 40);
        r.text = finish(&b);
        return r;
    }

    Buf b(a, 1024);
    iz  close = 0;
    for (; close<near.len && near[close].score>=FUZZ_MIN; close++) {}
    if (close) {
        print(&b, "old_string was not found exactly in ");
        print(&b, show);
        print(&b, all ? Str(", and replace_all needs exact matches")
                      : Str(", and the places that come close are not "
                            "clearly the one meant"));
        print(&b, ", so nothing was changed. The closest:\n");
        Slice<Span> at     = {};
        Slice<i64>  scores = {};
        for (iz i = 0; i < close; i++) {
            Span s = {near[i].beg, near[i].end};
            at     = push(a, at, s);
            scores = push(a, scores, near[i].score);
        }
        previews(&b, &t, at, scores);
        print(&b, "Copy old_string exactly from the file, without line numbers.");
        r.text = finish(&b);
        return r;
    }
    print(&b, "old_string was not found in ");
    print(&b, show);
    print(&b, ", so nothing was changed.");
    if (near.len) {
        print(&b, " The closest text, ");
        print(&b, near[0].score/10);
        print(&b, "% alike, is at lines ");
        print(&b, (i64)near[0].beg+1);
        putbyte(&b, '-');
        print(&b, (i64)near[0].end);
        print(&b, ":\n");
        showlines(&b, t.lines, near[0].beg, near[0].end, 12);
    } else {
        Str first = trim(cut(given, '\n').head);
        for (iz i = 0; first.len>=4 && i<t.lines.len; i++) {
            if (trim(t.lines[i]) == first) {
                print(&b, " Its first line matches line ");
                print(&b, (i64)i+1);
                print(&b, ", but the following lines differ.");
                break;
            }
        }
    }
    print(&b, " Read the file again and copy the text exactly, without "
              "line numbers.");
    r.text = finish(&b);
    return r;
}


// hashline

// The tag of a file's content: 16 bits of a 32-bit FNV-1a hash of its
// lines, cut of trailing blanks and CRs, as four hex digits. A byte
// order mark is no part of it.
static Str filetag(Arena *a, Str data)
{
    if (startswith(data, "\xef\xbb\xbf")) {
        data = cuthead(data, 3);
    }
    u32 h = 0x811c9dc5;
    for (Str rest = data; rest.len;) {
        Cut c    = cut(rest, '\n');
        Str line = rtrimline(c.head);
        for (iz i = 0; i < line.len; i++) {
            h ^= line[i];
            h *= 0x01000193;
        }
        if (c.ok) {
            h ^= '\n';
            h *= 0x01000193;
        }
        rest = c.tail;
    }
    h ^= h >> 16;
    Buf b(a, 4);
    for (i32 i = 3; i >= 0; i--) {
        putbyte(&b, (u8)"0123456789ABCDEF"[h>>(4*i) & 15]);
    }
    return finish(&b);
}

enum : i32 {
    HASH_REPLACE,
    HASH_INSERT,
    HASH_DELETE,
};

struct HashEdit {
    i32 op;    // HASH_*
    i64 line;  // first line, from 1; for an insert, the line before, or 0
    i64 end;   // last line of a replace or delete
    Str text;  // the lines of a replace or insert
};

static b32 hashless(HashEdit **a, HashEdit **b)
{
    return (*a)->line < (*b)->line;
}

// The tag as given, forgiving a whole [PATH#TAG] header.
static Str tagof(Str tag)
{
    tag = trim(tag);
    if (tag.len && tag[tag.len-1]==']') tag = cuttail(tag, 1);
    for (iz i = tag.len-1; i >= 0; i--) {
        if (tag[i] == '#') return cuthead(tag, i+1);
    }
    return tag;
}

// The lines of a file after hashline edits, and where each edit's own
// lines went among them.
struct HashOut {
    Slice<Str>  out;
    Slice<Span> done;
};

static void hashput(Arena *a, Text *t, HashOut *w, Str text)
{
    Slice<Str> add = givenlines(a, text);
    Span       s   = {w->out.len, 0};
    for (iz i = 0; i < add.len; i++) {
        w->out = push(a, w->out, addline(a, t, add[i]));
    }
    s.end   = w->out.len;
    w->done = push(a, w->done, s);
}

// Apply line edits to a file's content (data), named show, whose tag
// the agent read as tag.
static EditOut edithash(Arena *a, Str data, Str show, Str tag, Slice<HashEdit> edits)
{
    EditOut r   = {};
    Str     now = filetag(a, data);
    if (!equalfold(tagof(tag), now)) {
        Buf b(a, 256);
        print(&b, "The tag ");
        print(&b, trim(tag));
        print(&b, " does not match ");
        print(&b, show);
        print(&b, ": the file changed since you read it, or the tag is "
                  "not the one read showed. Nothing was changed. Read the "
                  "file again, then edit with the line numbers and tag it "
                  "shows.");
        r.text = finish(&b);
        return r;
    }

    Text t = splittext(a, data);
    iz   n = t.lines.len;
    Buf  e(a, 256);
    iz   issues = 0;
    Slice<HashEdit *> ranges = {};
    Slice<HashEdit *> inserts = {};
    for (iz i = 0; i < edits.len; i++) {
        HashEdit *h = &edits[i];
        b32 bad = h->op==HASH_INSERT ? h->line>n
                                     : h->line<1 || h->end>n || h->end<h->line;
        if (bad && issues++<5) {
            print(&e, "- edit ");
            print(&e, (i64)i+1);
            if (h->op == HASH_INSERT) {
                print(&e, ": cannot insert after line ");
                print(&e, h->line);
            } else {
                print(&e, ": lines ");
                print(&e, h->line);
                putbyte(&e, '-');
                print(&e, h->end);
                print(&e, " are not lines of the file");
            }
            print(&e, ", which has ");
            print(&e, (i64)n);
            print(&e, n==1 ? Str(" line\n") : Str(" lines\n"));
        } else if (!bad) {
            if (h->op == HASH_INSERT) {
                inserts = push(a, inserts, h);
            } else {
                ranges = push(a, ranges, h);
            }
        }
    }
    sort(ranges.data, ranges.len, hashless, *a);
    sort(inserts.data, inserts.len, hashless, *a);
    for (iz i = 1; i < ranges.len; i++) {
        HashEdit *p = ranges[i-1], *h = ranges[i];
        if (h->line<=p->end && issues++<5) {
            print(&e, "- the edits of lines ");
            print(&e, p->line);
            putbyte(&e, '-');
            print(&e, p->end);
            print(&e, " and ");
            print(&e, h->line);
            putbyte(&e, '-');
            print(&e, h->end);
            print(&e, " overlap: make them one edit\n");
        }
    }
    for (iz i = 0, k = 0; i < inserts.len; i++) {
        HashEdit *h = inserts[i];
        for (; k<ranges.len && ranges[k]->end<=h->line; k++) {}
        if (k<ranges.len && ranges[k]->line<=h->line && issues++<5) {
            print(&e, "- the insert after line ");
            print(&e, h->line);
            print(&e, " falls inside the edit of lines ");
            print(&e, ranges[k]->line);
            putbyte(&e, '-');
            print(&e, ranges[k]->end);
            print(&e, ": put its text into that edit instead\n");
        }
    }
    if (issues) {
        Buf b(a, 256 + e.len);
        print(&b, "Nothing was changed in ");
        print(&b, show);
        print(&b, ":\n");
        print(&b, finish(&e));
        print(&b, "Line numbers are those of the file as read showed it, "
                  "before any of these edits. Fix the edits and send them "
                  "all again.");
        r.text = finish(&b);
        return r;
    }

    // Walk the file once, in order of position
    HashOut w = {};
    iz      q = 0;  // next insert
    for (; q<inserts.len && inserts[q]->line==0; q++) {
        hashput(a, &t, &w, inserts[q]->text);
    }
    for (iz i = 1, k = 0; i <= n;) {
        iz last = i;  // the line after which inserts go
        if (k<ranges.len && ranges[k]->line==i) {
            HashEdit *h = ranges[k++];
            hashput(a, &t, &w, h->op==HASH_REPLACE ? h->text : Str{});
            last = (iz)h->end;
            i    = (iz)h->end + 1;
        } else {
            w.out = push(a, w.out, t.lines[i-1]);
            i++;
        }
        for (; q<inserts.len && inserts[q]->line==last; q++) {
            hashput(a, &t, &w, inserts[q]->text);
        }
    }
    Slice<Str>  out  = w.out;
    Slice<Span> done = w.done;
    Str result = jointext(a, &t, out);
    if (result == data) {
        Buf b(a, 128);
        print(&b, "These edits leave ");
        print(&b, show);
        print(&b, " as it was, so nothing was changed.");
        r.text = finish(&b);
        return r;
    }

    Str newtag = filetag(a, result);
    Buf b(a, 2048);
    print(&b, "Edited ");
    print(&b, show);
    print(&b, " (");
    print(&b, (i64)edits.len);
    print(&b, edits.len==1 ? Str(" edit") : Str(" edits"));
    print(&b, "). Its new tag and the changed lines, numbered as the file is now:\n[");
    print(&b, show);
    putbyte(&b, '#');
    print(&b, newtag);
    print(&b, "]\n");
    iz  shown = 0, upto = 0;  // the edits' lines are in order in out
    b32 more  = 0;
    for (iz i = 0; i<done.len && !more; i++) {
        iz beg = done[i].beg>2 ? done[i].beg-2 : 0;
        iz end = done[i].end+2<out.len ? done[i].end+2 : out.len;
        beg = beg>upto ? beg : upto;
        if (beg>upto && upto) print(&b, "   ...\n");
        for (iz k = beg; k<end && !more; k++, shown++) {
            more = shown == EDIT_SHOWN;
            if (!more) numbered(&b, k+1, out[k], 0);
        }
        upto = end>upto ? end : upto;
    }
    if (more) {
        print(&b, "   ... (read the file to see the rest)\n");
    }
    r.data = result;
    r.text = finish(&b);
    return r;
}


// patch

enum : i32 {
    PATCH_ADD,
    PATCH_DELETE,
    PATCH_UPDATE,
};

struct PatchLine {
    u8  kind;  // ' ' kept, '-' removed, '+' added
    Str text;
};

struct PatchHunk {
    Str              ctx;    // a line to find first (after @@), or null
    Slice<PatchLine> lines;
    b32              eof;    // the lines end the file
    i64              at;     // its line in the patch, from 1
};

struct PatchFile {
    i32              op;       // PATCH_*
    Str              path;     // as the patch names it
    Str              to;       // *** Move to, or null
    Str              content;  // an added file's
    Slice<PatchHunk> hunks;
    i64              at;
    Str              file;     // absolute paths, for the caller to fill
    Str              fileto;
};

struct Patch {
    Slice<PatchFile> files;
    Str              err;  // why it is not a patch, or null
};

// A line that may wrap a patch: a heredoc's start or end, or a fence.
static b32 patchwrap(Str line)
{
    line = trim(line);
    if (!line.len || startswith(line, "```") || find(line, "<<")>=0) {
        return 1;
    }
    for (iz i = 0; i < line.len; i++) {
        if (!alnum(line[i]) && line[i]!='_' && line[i]!='\'' && line[i]!='"') {
            return 0;
        }
    }
    return 1;  // a heredoc's last line, such as EOF
}

// The line to find after @@, or null: a unified diff's line numbers
// ("@@ -12,5 +12,6 @@ name") are not part of it.
static Str patchctx(Str rest)
{
    Str s = trim(rest);
    if (startswith(s, "-") && s.len>1 && digit(s[1])) {
        iz at = find(s, "@@");
        s = at<0 ? Str{} : trim(cuthead(s, at+2));
    } else if (endswith(s, "@@")) {
        s = trim(cuttail(s, 2));
    }
    return s.len ? s : Str{};
}

static Str patcherr(Arena *a, i64 line, Str msg)
{
    Buf b(a, 64 + msg.len);
    if (line) {
        print(&b, "line ");
        print(&b, line);
        print(&b, " of the patch: ");
    }
    print(&b, msg);
    return finish(&b);
}

// Parse the apply_patch envelope, forgiving a heredoc or code fence
// around it, context lines without their space, and a unified diff's
// line numbers after @@.
static Patch parsepatch(Arena *a, Str text)
{
    Patch      r     = {};
    Slice<Str> lines = {};
    for (Str rest = text; rest.len;) {
        Cut c = cut(rest, '\n');
        lines = push(a, lines, bare(c.head));
        rest  = c.tail;
    }
    iz beg = -1, end = -1;
    for (iz i = 0; i < lines.len; i++) {
        Str l = trim(lines[i]);
        beg = beg<0 && l=="*** Begin Patch" ? i : beg;
        end = l=="*** End Patch" ? i : end;
    }
    if (beg < 0) {
        r.err = "it does not start with a line \"*** Begin Patch\"";
        return r;
    } else if (end < beg) {
        r.err = "it does not end with a line \"*** End Patch\"";
        return r;
    }
    for (iz i = 0; i < lines.len; i++) {
        if ((i<beg || i>end) && !patchwrap(lines[i])) {
            r.err = patcherr(a, i+1, i<beg ? Str("unexpected text before *** Begin Patch")
                                           : Str("unexpected text after *** End Patch"));
            return r;
        }
    }

    PatchFile *f = 0;
    PatchHunk *h = 0;
    for (iz i = beg+1; i < end; i++) {
        Str line = lines[i];
        i64 at   = i + 1;
        Str hd   = trim(line);
        if (startswith(line, "***") && hd!="*** End of File") {
            Str path = {};
            i32 op   = -1;
            if (startswith(hd, "*** Add File:")) {
                op = PATCH_ADD;
                path = trim(cuthead(hd, 13));
            } else if (startswith(hd, "*** Delete File:")) {
                op = PATCH_DELETE;
                path = trim(cuthead(hd, 16));
            } else if (startswith(hd, "*** Update File:")) {
                op = PATCH_UPDATE;
                path = trim(cuthead(hd, 16));
            } else if (startswith(hd, "*** Move to:")) {
                if (!f || f->op!=PATCH_UPDATE || f->hunks.len || f->to.data) {
                    r.err = patcherr(a, at, "*** Move to must follow its "
                                     "*** Update File line");
                    return r;
                }
                f->to = trim(cuthead(hd, 12));
                if (!f->to.len) {
                    r.err = patcherr(a, at, "*** Move to has no path");
                    return r;
                }
                continue;
            } else {
                r.err = patcherr(a, at, "unknown header: a file starts with "
                                 "*** Add File:, *** Delete File: or *** Update File:");
                return r;
            }
            if (!path.len) {
                r.err = patcherr(a, at, "the header has no path");
                return r;
            }
            PatchFile nf = {};
            nf.op   = op;
            nf.path = path;
            nf.at   = at;
            r.files = push(a, r.files, nf);
            f = &r.files[r.files.len-1];
            h = 0;
            continue;
        }

        if (!f) {
            if (hd.len) {
                r.err = patcherr(a, at, "expected *** Add File:, *** Delete File: "
                                 "or *** Update File:");
                return r;
            }
            continue;
        }
        switch (f->op) {
        case PATCH_ADD:
            if (line.len && line[0]!='+') {
                r.err = patcherr(a, at, "each line of an added file starts with +");
                return r;
            }
            f->content = concat(a, f->content, cuthead(line, line.len>0));
            f->content = concat(a, f->content, "\n");
            break;
        case PATCH_DELETE:
            if (hd.len) {
                r.err = patcherr(a, at, "a deleted file has no lines after its header");
                return r;
            }
            break;
        case PATCH_UPDATE:
            if (startswith(line, "@@")) {
                PatchHunk nh = {};
                nh.ctx = patchctx(cuthead(line, 2));
                nh.at  = at;
                f->hunks = push(a, f->hunks, nh);
                h = &f->hunks[f->hunks.len-1];
                break;
            } else if (startswith(line, "\\")) {
                break;  // "\ No newline at end of file"
            }
            if (!h) {  // the first hunk may leave out its @@
                PatchHunk nh = {};
                nh.at = at;
                f->hunks = push(a, f->hunks, nh);
                h = &f->hunks[f->hunks.len-1];
            }
            if (hd == "*** End of File") {
                h->eof = 1;
                break;
            }
            PatchLine pl = {};
            if (line.len && (line[0]==' ' || line[0]=='-' || line[0]=='+')) {
                pl.kind = line[0];
                pl.text = cuthead(line, 1);
            } else {
                pl.kind = ' ';  // kept, without its space
                pl.text = line;
            }
            h->lines = push(a, h->lines, pl);
            break;
        }
    }

    if (!r.files.len) {
        r.err = "it changes no files";
        return r;
    }
    for (iz i = 0; i < r.files.len; i++) {
        PatchFile *p = &r.files[i];
        b32 changes = 0;
        for (iz k = 0; k < p->hunks.len; k++) {
            for (iz j = 0; j < p->hunks[k].lines.len; j++) {
                changes |= p->hunks[k].lines[j].kind != ' ';
            }
        }
        if (p->op==PATCH_UPDATE && !changes && !p->to.data) {
            r.err = patcherr(a, p->at, "this update changes nothing: mark lines "
                             "to remove with - and lines to add with +");
            return r;
        }
        for (iz k = 0; k < i; k++) {
            PatchFile *o = &r.files[k];
            if (o->path==p->path || (o->to.data && o->to==p->path) ||
                    (p->to.data && (p->to==o->path || p->to==o->to))) {
                r.err = patcherr(a, p->at, "a file appears twice: give each "
                                 "file one section, with all its hunks");
                return r;
            }
        }
    }
    return r;
}

// The paths a patch names, for summaries of a conversation.
static Slice<Str> patchpaths(Arena *a, Str text)
{
    Slice<Str> r = {};
    Patch      p = parsepatch(a, text);
    for (iz i = 0; i < p.files.len; i++) {
        r = push(a, r, p.files[i].path);
        if (p.files[i].to.data) {
            r = push(a, r, p.files[i].to);
        }
    }
    return r;
}

struct Seek {
    iz  at;    // the line, from 0, or -1 if not found
    i32 rung;  // the pass that found it
};

// Where want's lines first appear in have's at or after from, by the
// first of OpenCode's passes that finds them anywhere. Only the last
// place is tried for the end of the file.
static Seek seeklines(Arena scratch, Slice<Str> have, iz from, Slice<Str> want, b32 eof)
{
    static i32 const passes[] = {RUNG_EXACT, RUNG_RTRIM, RUNG_TRIM, RUNG_NORM};
    Seek r = {-1, 0};
    iz   n = have.len - want.len;
    iz   i = eof ? n : from;
    if (n<0 || i<0) {
        return r;
    }
    for (iz p = 0; p < countof(passes); p++) {
        Arena      t  = scratch;
        Slice<Str> wn = normlines(&t, want, passes[p]);
        for (iz k = i; k <= n; k++) {
            iz j = 0;
            for (; j < wn.len; j++) {
                Arena u = t;
                if (normline(&u, have[k+j], passes[p]) != wn[j]) break;
            }
            if (j == wn.len) {
                r.at   = k;
                r.rung = passes[p];
                return r;
            }
        }
    }
    return r;
}

struct Splice {
    iz         at;     // file lines from 0
    iz         len;    // lines replaced
    Slice<Str> lines;  // their replacement
    i32        hunk;
    i32        rung;
};

static b32 spliceless(Splice *a, Splice *b)
{
    return a->at < b->at;
}

// Apply an update's hunks to a file's content (data), named show.
// Returns the new content, with notes on loose matches; or null data
// and why not.
static EditOut patchupdate(Arena *a, Str data, Str show, PatchFile *f)
{
    EditOut       r      = {};
    Text          t      = splittext(a, data);
    Slice<Splice> sp     = {};
    iz            cursor = 0;
    for (iz hi = 0; hi < f->hunks.len; hi++) {
        PatchHunk *h     = &f->hunks[hi];
        iz         ctxat = -1;  // the line @@ found
        if (h->ctx.data) {
            Slice<Str> one = {};
            one = push(a, one, h->ctx);
            Seek s = seeklines(*a, t.lines, cursor, one, 0);
            if (s.at < 0) {
                Buf b(a, 256);
                print(&b, "hunk ");
                print(&b, (i64)hi+1);
                print(&b, " of ");
                print(&b, show);
                print(&b, " (patch line ");
                print(&b, h->at);
                print(&b, "): the line after @@, \"");
                print(&b, utf8head(h->ctx, EDIT_PREVIEWW));
                print(&b, "\", was not found");
                if (cursor) {
                    print(&b, " after line ");
                    print(&b, (i64)cursor);
                }
                print(&b, ". It must be a line of the file, before the change, "
                          "copied exactly; or leave it out.");
                r.text = finish(&b);
                return r;
            }
            ctxat  = s.at;
            cursor = s.at + 1;
        }

        Slice<Str> old = {}, now = {};
        for (iz k = 0; k < h->lines.len; k++) {
            PatchLine *pl = &h->lines[k];
            if (pl->kind != '+') old = push(a, old, pl->text);
            if (pl->kind != '-') now = push(a, now, pl->text);
        }
        Splice x = {};
        x.hunk = (i32)hi;
        if (!old.len) {
            // Only added lines: after the line @@ found, else at the end
            x.at = h->ctx.data ? cursor : t.lines.len;
            for (iz k = 0; k < now.len; k++) {
                x.lines = push(a, x.lines, addline(a, &t, now[k]));
            }
            sp = push(a, sp, x);
            continue;
        }

        // The line after @@ comes before the hunk, but some agents give
        // the hunk's own first line there, so that is tried last
        Slice<PatchLine> hl = h->lines;
        Seek             s  = seeklines(*a, t.lines, cursor, old, h->eof);
        if (s.at<0 && ctxat>=0) {
            s = seeklines(*a, t.lines, ctxat, old, h->eof);
        }
        if (s.at<0 && old.len>1 && !old[old.len-1].len) {
            // A last kept empty line may stand for the file's final
            // newline (as in Codex)
            old.len--;
            for (iz k = hl.len-1; k >= 0; k--) {
                if (hl[k].kind != '+') {
                    Slice<PatchLine> fewer = {};
                    for (iz j = 0; j < hl.len; j++) {
                        if (j != k) fewer = push(a, fewer, hl[j]);
                    }
                    hl = fewer;
                    break;
                }
            }
            s = seeklines(*a, t.lines, ctxat>=0 ? ctxat : cursor, old, h->eof);
        }
        if (s.at < 0) {
            Buf b(a, 1024);
            print(&b, "hunk ");
            print(&b, (i64)hi+1);
            print(&b, " of ");
            print(&b, show);
            print(&b, " (patch line ");
            print(&b, h->at);
            print(&b, "): its kept and removed lines were not found together");
            if (cursor) {
                print(&b, " after line ");
                print(&b, (i64)cursor);
            }
            print(&b, ". They must be lines of the file, in order, copied "
                      "exactly.");
            Slice<Str>  have = normlines(a, t.lines, RUNG_FUZZY);
            Slice<Str>  want = normlines(a, old, RUNG_FUZZY);
            Slice<Near> near = nearby(a, have, want, FUZZ_SUGGEST);
            if (near.len) {
                print(&b, " The closest text, ");
                print(&b, near[0].score/10);
                print(&b, "% alike, is at lines ");
                print(&b, (i64)near[0].beg+1);
                putbyte(&b, '-');
                print(&b, (i64)near[0].end);
                print(&b, ":\n");
                showlines(&b, t.lines, near[0].beg, near[0].end, 12);
            }
            r.text = finish(&b);
            return r;
        }

        // Kept lines stay as the file has them
        Indent in = s.rung>=RUNG_TRIM ? indentpair(old, t.lines, s.at) : Indent{};
        x.at   = s.at;
        x.len  = old.len;
        x.rung = s.rung;
        for (iz k = 0, j = s.at; k < hl.len; k++) {
            switch (hl[k].kind) {
            case ' ': x.lines = push(a, x.lines, t.lines[j++]); break;
            case '-': j++; break;
            case '+':
                x.lines = push(a, x.lines, addline(a, &t, reindent(a, hl[k].text, in)));
                break;
            }
        }
        sp     = push(a, sp, x);
        cursor = s.at + old.len;
    }

    sort(sp.data, sp.len, spliceless, *a);
    Slice<Str> out  = {};
    iz         last = 0;
    Buf        note(a, 256);
    for (iz i = 0; i < sp.len; i++) {
        for (iz k = last; k < sp[i].at; k++) {
            out = push(a, out, t.lines[k]);
        }
        for (iz k = 0; k < sp[i].lines.len; k++) {
            out = push(a, out, sp[i].lines[k]);
        }
        last = sp[i].at + sp[i].len;
        if (sp[i].rung > RUNG_EXACT) {
            print(&note, " Hunk ");
            print(&note, (i64)sp[i].hunk+1);
            print(&note, rungnote(sp[i].rung));
            putbyte(&note, '.');
        }
    }
    for (iz k = last; k < t.lines.len; k++) {
        out = push(a, out, t.lines[k]);
    }
    r.data = jointext(a, &t, out);
    r.text = finish(&note);
    return r;
}

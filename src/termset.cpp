// A root session's terminal tabs as a set, and the titles shown on them,
// apart from ImGui and the engine (term.cpp draws and runs them).
//
// The set's array is in the tab bar's order, front the index shown. A
// tab added goes last and shows; a tab dropped shows its right
// neighbour, else its left. The bar may reorder the tabs, by drag, and
// the set follows.
//
// This is free and unencumbered software released into the public domain.

struct TermTab;

enum : i32 {
    TERMTABS     = 8,    // tabs a root may have
    TERMTITLE    = 40,   // code points of a title shown
    TERMTITLECAP = TERMTITLE*4 + 4,  // its bytes, with an ellipsis
};

struct TermSet {
    TermTab *tabs[TERMTABS];
    i32      n;
    i32      front;      // the tab shown, while n > 0
    b32      frontset;   // front changed here: the bar must select it
};

static i32 termsetfind(TermSet *s, TermTab *tt)
{
    for (i32 i = 0; i < s->n; i++) {
        if (s->tabs[i] == tt) {
            return i;
        }
    }
    return -1;
}

static TermTab *termsetfront(TermSet *s)
{
    return s->n ? s->tabs[s->front] : 0;
}

static void termsetshow(TermSet *s, i32 i)
{
    if (i>=0 && i<s->n && (i!=s->front || !s->frontset)) {
        s->front    = i;
        s->frontset = 1;
    }
}

// Add a tab last, shown. False when the set is full.
static b32 termsetadd(TermSet *s, TermTab *tt)
{
    if (s->n == TERMTABS) {
        return 0;
    }
    s->tabs[s->n++] = tt;
    s->front    = s->n - 1;
    s->frontset = 1;
    return 1;
}

// Remove tab i, returning it.
static TermTab *termsetdrop(TermSet *s, i32 i)
{
    assert(i>=0 && i<s->n);
    TermTab *tt = s->tabs[i];
    for (i32 k = i; k < s->n-1; k++) {
        s->tabs[k] = s->tabs[k+1];
    }
    s->tabs[--s->n] = 0;
    if (!s->n) {
        s->front    = 0;
        s->frontset = 0;
    } else if (i < s->front) {
        s->front--;  // the same tab, moved down
    } else if (i == s->front) {
        // Its right neighbour, now at i, else its left
        s->front    = i<s->n ? i : s->n-1;
        s->frontset = 1;
    }
    return tt;
}

// Take the bar's order, n tabs as it has them: the set's tabs in that
// order, then any it doesn't have yet (not yet drawn) as they were.
// Tabs not in the set are ignored. The front tab stays front.
static void termsetorder(TermSet *s, TermTab **order, i32 n)
{
    TermTab *front = termsetfront(s);
    TermTab *was[TERMTABS];
    i32      nwas = s->n;
    for (i32 i = 0; i < nwas; i++) {
        was[i] = s->tabs[i];
    }
    i32 k = 0;
    for (i32 i = 0; i < n; i++) {
        i32 at = -1;
        for (i32 j = 0; j < nwas; j++) {
            if (was[j] == order[i]) {
                at = j;
                break;
            }
        }
        if (at >= 0) {
            s->tabs[k++] = was[at];
            was[at] = 0;
        }
    }
    for (i32 j = 0; j < nwas; j++) {
        if (was[j]) {
            s->tabs[k++] = was[j];
        }
    }
    assert(k == s->n);
    if (front) {
        s->front = termsetfind(s, front);
    }
}

// [terminal] scrollback as a tab's history lines, 0 to 100000, the
// default for what isn't a number.
static i32 termscrollparse(Str v)
{
    Int n = parseint(v);
    if (!n.ok) {
        return 10000;
    }
    return n.val<0 ? 0 : n.val>100000 ? 100000 : (i32)n.val;
}

// History for that many lines: ariel's ring in proportion to its
// default (10000 lines in 4 MiB), at least 64 KiB; none for 0.
static iz termscrollbytes(i32 lines)
{
    iz n = (iz)lines * 420;
    return !lines ? 0 : n<((iz)1<<16) ? (iz)1<<16 : n;
}

// A title as a tab shows it, from the bytes a program set (OSC 0 or 2),
// into dst (TERMTITLECAP bytes): what the eye can't see dropped (the
// bidi and zero-width controls, C0 and C1), blank runs as one space,
// no blank at either end, cut at TERMTITLE code points with an
// ellipsis, and "##" broken up, which ImGui would take as an ID.
static iz termtitle(u8 *dst, Str raw)
{
    iz  len   = 0;
    i32 count = 0;
    b32 blank = 0;
    i32 prev  = 0;
    for (iz i = 0; i < raw.len;) {
        i32 c = utf8decode(raw, &i);
        if (c=='\t' || c=='\n' || c=='\r' || c==' ' || c==0x3000 ||
            (c>=0x2000 && c<=0x200a) || c==0xa0) {
            blank = len > 0;
            continue;
        } else if (webinvisible(c) || (c>=0xd800 && c<=0xdfff)) {
            continue;
        }
        i32 add = blank + 1 + (c=='#' && prev=='#' && !blank);
        if (count+add > TERMTITLE) {
            len += utf8encode(dst+len, 0x2026);
            return len;
        }
        if (blank) {
            dst[len++] = ' ';
            count++;
            prev = ' ';
        } else if (c=='#' && prev=='#') {
            dst[len++] = ' ';
            count++;
        }
        blank  = 0;
        len   += utf8encode(dst+len, c);
        count++;
        prev   = c;
    }
    return len;
}

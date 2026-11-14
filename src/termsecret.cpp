// Secrets typed into terminals, kept from what the agent reads of them
//
// read_terminal (ptyhost.cpp) shows the agent a terminal's screen. Text
// typed at a password prompt is never echoed, so it never reaches the
// screen; the leaks are the slips: a password typed ahead, before the
// prompt turned echo off, that a program then reads already echoed; one
// typed again into a shell after sudo timed out; a program that echoes
// it, or reads it without the terminal's line mode. So the engine learns
// secrets from what is typed, and hides them wherever they show.
//
// Each line typed has a kind, latched at its first byte from how the
// terminal reads it then: a secret where echo is off in line mode, or
// where the cursor's row reads as a password prompt (termprompt), which
// covers what reads a password raw (ssh, pinentry); a candidate where
// the line is echoed; else ignored. A candidate becomes a secret if echo
// goes off within moments of it (type-ahead, the caller's). termlinefeed
// follows the line's editing as a terminal's line discipline would,
// with the escape sequences that arrive as input (keys, focus and mouse
// reports, replies to queries) taken whole; a key that moves within the
// line or recalls another spoils it, as does a line too long to be one.
//
// termmask hides the secrets known, each occurrence as "[hidden]": one
// of TERMSECRET_SUB bytes or more anywhere, a shorter one only as a
// whole word, so that common short words are not taken from the text.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    TERMLINE_CAP   = 256,   // a line longer than this is no password
    TERMSECRET_MIN = 4,     // a secret shorter is not kept
    TERMSECRET_SUB = 6,     // from this long, hidden inside words too
    TERMSECRETS    = 16,    // kept at once, engine-wide
};

enum : i32 {
    TL_UNSET,    // no byte of the line yet
    TL_SKIP,     // not kept
    TL_CAND,     // echoed: a secret if echo goes off soon after
    TL_SECRET,
};

enum : i32 {
    TLE_NONE,
    TLE_ESC,     // after ESC
    TLE_CSI,     // in ESC [ ... final
    TLE_SS3,     // after ESC O: one more byte
    TLE_STR,     // OSC, DCS, SOS, PM, APC: until BEL or ST
    TLE_STRESC,  // ESC within one: ST if '\'
};

struct TermLine {
    u8  buf[TERMLINE_CAP];
    i32 len;
    i32 kind;
    i32 esc;
    u8  csi[16];     // the CSI's parameters, enough for 200 and 201
    i32 csilen;
    b32 spoiled;     // edited out of sight, or too long
    b32 paste;       // within a bracketed paste
};

struct TermCommit {
    i32 kind;        // TL_CAND or TL_SECRET
    Str text;
};

static void termlinereset(TermLine *l)
{
    l->len     = 0;
    l->kind    = TL_UNSET;
    l->spoiled = 0;
}

// A byte of the line's text, the line's kind latched at its first.
static void termlineput(TermLine *l, u8 c, i32 kind)
{
    if (l->kind == TL_UNSET) {
        l->kind = kind;
    }
    if (l->len == TERMLINE_CAP) {
        l->spoiled = 1;
    } else {
        l->buf[l->len++] = c;
    }
}

// Typed bytes into the line, kind the kind a line begun now would have.
// Returns the lines committed (Enter) that may be kept, in order.
static Slice<TermCommit> termlinefeed(Arena *a, TermLine *l, Str in, i32 kind)
{
    Slice<TermCommit> r = {};
    for (iz i = 0; i < in.len; i++) {
        u8 c = in[i];
        switch (l->esc) {
        case TLE_ESC:
            l->esc = TLE_NONE;
            if (c == '[') {
                l->esc    = TLE_CSI;
                l->csilen = 0;
            } else if (c == 'O') {
                l->esc = TLE_SS3;
            } else if (c==']' || c=='P' || c=='X' || c=='^' || c=='_') {
                l->esc = TLE_STR;
            } else if (!l->paste) {
                termlinereset(l);  // Alt+key: an edit unseen
            }
            continue;
        case TLE_CSI:
            if (c>=0x40 && c<=0x7e) {
                Str p  = {l->csi, l->csilen};
                l->esc = TLE_NONE;
                if (c=='~' && p==Str("200")) {
                    l->paste = 1;
                } else if (c=='~' && p==Str("201")) {
                    l->paste = 0;
                } else if (!l->paste && (c=='A' || c=='B' || c=='C' || c=='D' ||
                                         c=='F' || c=='H' || c=='~')) {
                    // A cursor key, Home, End, Delete, Insert: the line's
                    // text is no longer what was typed in order
                    l->spoiled = l->kind != TL_UNSET;
                }
            } else if (c < 0x20) {
                l->esc = TLE_NONE;  // malformed: drop it
            } else if (l->csilen < (i32)countof(l->csi)) {
                l->csi[l->csilen++] = c;
            }
            continue;
        case TLE_SS3:
            l->esc = TLE_NONE;
            if (!l->paste && (c=='A' || c=='B' || c=='C' || c=='D' || c=='F' || c=='H')) {
                l->spoiled = l->kind != TL_UNSET;
            }
            continue;
        case TLE_STR:
            if (c == 0x07) {
                l->esc = TLE_NONE;
            } else if (c == 0x1b) {
                l->esc = TLE_STRESC;
            }
            continue;
        case TLE_STRESC:
            l->esc = c=='\\' ? TLE_NONE : c==0x1b ? TLE_STRESC : TLE_STR;
            continue;
        }

        if (c == 0x1b) {
            if (i+1 == in.len && !l->paste) {
                termlinereset(l);  // a lone Escape
            } else {
                l->esc = TLE_ESC;
            }
        } else if (c=='\r' || c=='\n') {
            if ((l->kind==TL_CAND || l->kind==TL_SECRET) && !l->spoiled && l->len) {
                TermCommit m = {};
                m.kind = l->kind;
                m.text = clone(a, Str{l->buf, l->len});
                r      = push(a, r, m);
            }
            termlinereset(l);
        } else if (l->paste) {
            if (c>=0x20 || c=='\t') {
                termlineput(l, c, kind);
            }
        } else if (c==0x7f || c==0x08) {
            // Erase a code point: its continuation bytes, then its lead
            while (l->len && (l->buf[l->len-1]&0xc0)==0x80) {
                l->len--;
            }
            l->len -= l->len > 0;
        } else if (c == 0x15) {  // ^U
            l->len = 0;
        } else if (c == 0x17) {  // ^W: blanks, then the word before them
            while (l->len && whitespace(l->buf[l->len-1])) {
                l->len--;
            }
            while (l->len && !whitespace(l->buf[l->len-1])) {
                l->len--;
            }
        } else if (c==0x03 || c==0x04 || c==0x1a) {  // ^C ^D ^Z
            termlinereset(l);
        } else if (c < 0x20) {
            l->spoiled = l->kind != TL_UNSET;  // Tab, ^R, ^A...: unseen edits
        } else {
            termlineput(l, c, kind);
        }
    }
    return r;
}

// Whether a byte belongs to a word, for the whole-word tests.
static b32 termwordbyte(u8 c)
{
    return alnum(c) || c=='_' || c>=0x80;
}

// Whether a word, ASCII case-insensitive, stands whole in s.
static b32 termhasword(Str s, Str word)
{
    for (iz i = 0; i+word.len <= s.len; i++) {
        if (equalfold(Str{s.data+i, word.len}, word) &&
            (i==0 || !termwordbyte(s[i-1])) &&
            (i+word.len==s.len || !termwordbyte(s[i+word.len]))) {
            return 1;
        }
    }
    return 0;
}

// Whether the text before a terminal's cursor, on its row, asks for a
// password: it ends in a colon, and names one.
static b32 termprompt(Str row)
{
    static Str words[] = {
        "password", "passphrase", "pass phrase", "passcode", "passwd",
        "pin", "secret", "token",
    };
    row = trimright(row);
    if (!row.len || row[row.len-1]!=':') {
        return 0;
    }
    for (iz i = 0; i < countof(words); i++) {
        if (termhasword(row, words[i])) {
            return 1;
        }
    }
    return 0;
}

// A known secret, and how it was learned: by echo off, which outranks
// a prompt's reading or a setting's value when one must go.
struct TermSecret {
    u8  buf[TERMLINE_CAP];
    i32 len;
    i32 rank;   // 2 echo off, 1 otherwise
    i64 at;     // the order it was learned or seen again
};

struct TermSecrets {
    TermSecret s[TERMSECRETS];
    i64        clock;
};

// Learn a secret, if long enough: seen again, it is only freshened; a
// new one takes an empty entry, else the oldest of the lowest rank
// (zeroed first). Returns whether it was kept.
static b32 termsecretadd(TermSecrets *ss, Str v, i32 rank)
{
    if (v.len<TERMSECRET_MIN || v.len>TERMLINE_CAP) {
        return 0;
    }
    i64 at = ++ss->clock;
    for (i32 i = 0; i < TERMSECRETS; i++) {
        TermSecret *s = ss->s + i;
        if (s->len && Str{s->buf, s->len}==v) {
            s->rank = rank>s->rank ? rank : s->rank;
            s->at   = at;
            return 1;
        }
    }
    TermSecret *pick = 0;
    for (i32 i = 0; i < TERMSECRETS; i++) {
        TermSecret *s = ss->s + i;
        if (!s->len) {
            pick = s;
            break;
        } else if (!pick || s->rank<pick->rank || (s->rank==pick->rank && s->at<pick->at)) {
            pick = s;
        }
    }
    if (pick->len && pick->rank>rank) {
        return 0;  // all outrank it
    }
    *pick = {};
    __builtin_memcpy(pick->buf, v.data, (uz)v.len);
    pick->len  = (i32)v.len;
    pick->rank = rank;
    pick->at   = at;
    return 1;
}

// Text with each occurrence of a secret as "[hidden]", the longest
// first where several begin at once; *hidden counts them. The text
// itself if none occurs.
static Str termmask(Arena *a, Str text, TermSecrets *ss, iz *hidden)
{
    *hidden = 0;
    Buf b   = {};
    iz  from = 0;  // text not yet copied
    for (iz i = 0; i < text.len;) {
        i32 best = 0;
        for (i32 k = 0; k < TERMSECRETS; k++) {
            TermSecret *s = ss->s + k;
            if (!s->len || s->len<=best || s->len>text.len-i || s->buf[0]!=text[i] ||
                !(Str{text.data+i, s->len} == Str{s->buf, s->len})) {
                continue;
            }
            if (s->len < TERMSECRET_SUB) {
                b32 left  = i==0 || !termwordbyte(text[i-1]);
                b32 right = i+s->len==text.len || !termwordbyte(text[i+s->len]);
                if (!left || !right) {
                    continue;
                }
            }
            best = s->len;
        }
        if (!best) {
            i++;
            continue;
        }
        if (!b.data) {
            b = Buf(a, text.len + 64);
        }
        print(&b, Str{text.data+from, i-from});
        print(&b, "[hidden]");
        (*hidden)++;
        i   += best;
        from = i;
    }
    if (!b.data) {
        return text;
    }
    print(&b, Str{text.data+from, text.len-from});
    return finish(&b);
}

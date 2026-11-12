// File references: where a link points, and the path it names
//
// A reference is a Markdown link's destination as models write them and
// as Claude Desktop takes them: a path, relative or not, or a file: URL,
// then maybe a line or a range, after "#L" or ":", or a heading's slug
// after "#". fileref parses one without touching any file system.
// filepath resolves it on the session's host, against its working
// directory and home, and refuses what must not be opened: network
// paths, other users' homes, and for a reference nobody asked about
// (an inline-code span, say), anything outside the working directory.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    FREF_PATH,    // a file, maybe at a line
    FREF_NET,     // a file: URL naming another computer
    FREF_BAD,     // no name, a control character, a folder
    FREF_SCHEME,  // some other URL: https:, mailto:, javascript:
};

enum : i32 {
    FREF_MAXLINE = 10000000,  // lines above are no line
};

enum : iz {
    // Raw bytes of one fs.read part, its reply about 350 kB in base64:
    // under the i686 host's reserve for a client's replies, keepmax/32 =
    // 512 KiB (host.cpp), which a larger reply could overrun on a busy
    // link, dropping the client (remote.py fs_busy)
    FILES_PART = (iz)256 << 10,
};

struct FileRef {
    Str path;     // decoded, as written: relative, "~", or absolute
    i32 line0;    // first line, 1-based, or 0 for none
    i32 line1;    // last line, line0 if only one
    i32 col;      // column on line0, 1-based, or 0
    Str section;  // a heading's slug, decoded, or null
    i32 kind;     // FREF_*
};

struct FrefLines {
    i32 line0;
    i32 line1;
    i32 col;
    b32 ok;
};

// Digits at s[*i] as a number, advancing *i: above FREF_MAXLINE as one
// past it, -1 if there are none.
static i32 frefnum(Str s, iz *i)
{
    iz  beg = *i;
    i64 v   = 0;
    for (; *i<s.len && digit(s[*i]); (*i)++) {
        v = v*10 + (s[*i] - '0');
        v = v>FREF_MAXLINE ? FREF_MAXLINE+1 : v;
    }
    return *i==beg ? -1 : (i32)v;
}

// A fragment that is a line range, all of it: "L10", "L10C3",
// "L10-L20", "L10-20", "L10C3-L20C5".
static FrefLines freffrag(Str f)
{
    FrefLines r = {};
    if (!f.len || f[0]!='L') {
        return r;
    }
    iz  i  = 1;
    i32 l0 = frefnum(f, &i);
    i32 c0 = 0;
    if (l0 < 0) {
        return r;
    }
    if (i<f.len && f[i]=='C') {
        i++;
        if ((c0 = frefnum(f, &i)) < 0) {
            return r;
        }
    }
    i32 l1 = l0;
    if (i<f.len && f[i]=='-') {
        i++;
        i += i<f.len && f[i]=='L';
        if ((l1 = frefnum(f, &i)) < 0) {
            return r;
        }
        if (i<f.len && f[i]=='C') {
            i++;
            if (frefnum(f, &i) < 0) {
                return r;
            }
        }
    }
    if (i == f.len) {
        r = {l0, l1, c0, 1};
    }
    return r;
}

// The length of a trailing ":N", ":N:C" or ":A-B", each maybe with a
// stray ':' after, or of a stray ':' alone; 0 if none, or if all left
// before it is one letter (a drive). Its lines in *out, ok if any.
static iz frefsuffix(Str s, FrefLines *out)
{
    *out = {};
    iz end = s.len;
    end -= end && s[end-1]==':';
    iz j = end;
    for (; j>0 && digit(s[j-1]); j--) {}
    iz        start = end;
    FrefLines l     = {};
    if (j<end && j>0 && (s[j-1]==':' || s[j-1]=='-')) {
        iz k = j - 1;
        iz m = k;
        for (; m>0 && digit(s[m-1]); m--) {}
        iz  at = j;
        i32 b  = frefnum(s, &at);
        if (m<k && m>0 && s[m-1]==':') {
            at = m;
            i32 a = frefnum(s, &at);
            l = s[k]=='-' ? FrefLines{a, b, 0, 1} : FrefLines{a, a, b, 1};
            start = m - 1;
        } else if (s[k] == ':') {
            l     = {b, b, 0, 1};
            start = k;
        }
    }
    if (start==1 && letter(s[0])) {
        return 0;
    }
    *out = l;
    return s.len - start;
}

// Lines as kept: none for line 0 or past FREF_MAXLINE, a bad end as no
// end, a reversed range swapped (and its column dropped).
static void freflines(FileRef *r, FrefLines l)
{
    if (!l.ok || l.line0<1 || l.line0>FREF_MAXLINE) {
        return;
    }
    i32 l1 = l.line1<1 || l.line1>FREF_MAXLINE ? l.line0 : l.line1;
    r->line0 = l.line0<l1 ? l.line0 : l1;
    r->line1 = l.line0<l1 ? l1 : l.line0;
    r->col   = l1>=l.line0 && l.col>=1 && l.col<=FREF_MAXLINE ? l.col : 0;
}

// Length of a URL scheme at the start of s, its ':' included, or 0: a
// letter, then letters, digits, '+', '-' and '.'.
static iz frefscheme(Str s)
{
    if (!s.len || !letter(s[0])) {
        return 0;
    }
    iz i = 1;
    for (; i<s.len && (alnum(s[i]) || s[i]=='+' || s[i]=='-' || s[i]=='.'); i++) {}
    return i<s.len && s[i]==':' ? i+1 : 0;
}

// Whether a fragment is a heading's slug: letters, digits, '-', '_',
// escapes, and any non-ASCII.
static b32 frefslug(Str f)
{
    for (iz i = 0; i < f.len; i++) {
        u8 c = f[i];
        if (!alnum(c) && c!='-' && c!='_' && c!='%' && c<0x80) {
            return 0;
        }
    }
    return f.len > 0;
}

// %XX as the byte it stands for, the rest as it is. Null if a control
// character results, or appears.
static Str frefdecode(Arena *a, Str s)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        i32 hi = s[i]=='%' && i+2<s.len ? hexval(s[i+1]) : -1;
        i32 lo = hi>=0 ? hexval(s[i+2]) : -1;
        u8  c  = s[i];
        if (lo >= 0) {
            c  = (u8)(hi<<4 | lo);
            i += 2;
        }
        if (c<0x20 || c==0x7f) {
            return {};
        }
        putbyte(&b, c);
    }
    return finish(&b);
}

// Parse a link's destination (see above). A path that is only a drive
// and more ("C:12", "C:x") is BAD, as is one ending in a separator.
static FileRef fileref(Arena *a, Str href)
{
    FileRef r   = {};
    Str     s   = href;
    b32     url = 0;
    r.kind = FREF_BAD;

    // A file: URL, with no host or localhost
    if (s.len>=5 && equalfold(takehead(s, 5), "file:")) {
        url = 1;
        s   = cuthead(s, 5);
        if (startswith(s, "//")) {
            Cut host = cut(cuthead(s, 2), '/');
            if (host.head.len && !equalfold(host.head, "localhost")) {
                r.kind = FREF_NET;
                return r;
            } else if (!host.ok) {
                return r;
            }
            s = cuthead(s, 2 + host.head.len);
        }
        if (!s.len || s[0]!='/') {
            return r;
        }
        if (s.len>=3 && letter(s[1]) && s[2]==':') {
            s = cuthead(s, 1);  // "/C:/x"
        } else if (s.len>=5 && letter(s[1]) && s[2]=='%' && s[3]=='3' &&
                   lowercase(s[4])=='a') {
            Buf b(a, s.len);  // "/c%3A/x", as VS Code writes it
            putbyte(&b, s[1]);
            putbyte(&b, ':');
            print(&b, cuthead(s, 5));
            s = finish(&b);
        }
    }

    // The last '#': a line range, a section, or part of the name
    FrefLines lines = {};
    Str       slug  = {};
    for (iz i = s.len-1; i >= 0; i--) {
        if (s[i] == '#') {
            Str frag = cuthead(s, i+1);
            lines = freffrag(frag);
            if (lines.ok || frefslug(frag)) {
                slug = lines.ok ? Str{} : frag;
                s    = takehead(s, i);
            }
            break;
        }
    }

    // Drive paths, and other schemes, unless a line suffix follows
    if (isdrive(s)) {
        if (s.len<3 || !issep(s[2], 1)) {
            return r;
        }
    } else if (!url) {
        iz        n = frefscheme(s);
        FrefLines l = {};
        if (n && !(frefsuffix(s, &l)==s.len-n+1 && l.ok)) {
            r.kind = FREF_SCHEME;
            return r;
        }
    }
    if (url) {
        s = cut(s, '?').head;
    }

    // A line suffix, unless a fragment gave lines
    FrefLines l = {};
    s = cuttail(s, frefsuffix(s, &l));
    freflines(&r, lines.ok ? lines : l);

    // Decoded, after the split
    Str path    = frefdecode(a, s);
    Str section = slug.len ? frefdecode(a, slug) : Str{};
    if (!path.len || issep(path[path.len-1], 1) || (slug.len && !section.data)) {
        return {{}, 0, 0, 0, {}, FREF_BAD};
    }
    r.path    = path;
    r.section = section;
    r.kind    = FREF_PATH;
    return r;
}

struct FilePath {
    Str path;  // absolute and normalized, or null
    Str err;   // why not, or null
};

// A reference's file on a host (win for Windows paths; a WSL host's are
// Linux paths), relative to base, its working directory, if any. Asked
// is a reference someone opened; one not asked (unasked) must also lie
// within base, and not come from "~".
static FilePath filepath(Arena *a, FileRef ref, Str base, Str home, b32 win, b32 unasked)
{
    FilePath r = {};
    switch (ref.kind) {
    case FREF_NET:    r.err = "it is on another computer";  return r;
    case FREF_SCHEME: r.err = "it is not a file";           return r;
    case FREF_BAD:    r.err = "it does not name a file";    return r;
    }

    Str p = win ? msysdrive(a, ref.path) : ref.path;
    if (p.len && p[0]=='~') {
        if (p.len>1 && !issep(p[1], win)) {
            r.err = "it is in another user's home";
            return r;
        } else if (unasked) {
            r.err = "it is outside the working directory";
            return r;
        } else if (!home.len) {
            r.err = "the home directory is not known";
            return r;
        }
        p = expandhome(a, p, home, win);
    }
    if (netpath(p, win)) {
        r.err = "it is a network path";
        return r;
    } else if (!pathroot(p, win) && !base.len) {
        r.err = "it is relative, and there is no working directory";
        return r;
    }

    Str path = normpath(a, base, p, win);
    if (netpath(path, win)) {
        r.err = "it is a network path";
        return r;
    }
    if (unasked && (!base.len || !within(normpath(a, base, ".", win), path, win))) {
        r.err = "it is outside the working directory";
        return r;
    }
    r.path = path;
    return r;
}

// Whether an inline-code span may name a file, before it is looked
// for: no blanks or shell punctuation, no URL, not an option, then a
// '/' or an extension with a letter, maybe a line suffix after.
static b32 codepath(Str span)
{
    if (!span.len || span.len>=256 || span[0]=='-' || startswith(span, "::") ||
            find(span, "://")>=0) {
        return 0;
    }
    for (iz i = 0; i < span.len; i++) {
        u8 c = span[i];
        if (c<=' ' || c==0x7f) {
            return 0;
        }
        for (u8 const *p = (u8 const *)"()*?\"<>|$;{}"; *p; p++) {
            if (c == *p) {
                return 0;
            }
        }
    }
    FrefLines l    = {};
    Str       name = cuttail(span, frefsuffix(span, &l));
    iz        dot  = -1;
    for (iz i = 0; i < name.len; i++) {
        if (name[i] == '/') {
            return 1;
        }
        dot = name[i]=='.' ? i : dot;
    }
    Str ext = dot<0 ? Str{} : cuthead(name, dot+1);
    b32 any = 0;
    for (iz i = 0; i < ext.len; i++) {
        if (!alnum(ext[i])) {
            return 0;
        }
        any |= letter(ext[i]);
    }
    return any && ext.len<=10;
}

// INI configuration reader: sections, keys, comments, layered includes
//
// The hand-edited, shareable configuration format:
//
//   # Whole-line comments start with # or ;
//   include = team.ini                 # only before the first section
//   [provider local]                   # [kind name], or [kind] alone
//   url    = http://localhost:8080/    # trailing comment
//   color  = #ff99ff                   # a value, not a comment
//   prompt = "  padded, or with a literal # inside  "
//
// - A trailing comment starts at a # preceded by whitespace and followed
//   by whitespace or the end of the line (elfeed2's rule), so values like
//   #ff99ff and URL fragments survive. ; is not a trailing comment, but
//   after a section header either # or ; starts one.
// - Section headers hold one or two words: [kind] or [kind name]. A
//   section may appear more than once, and its keys merge. Keys before
//   the first header belong to the top-level section, whose kind and name
//   are empty.
// - Keys are one word. Values are trimmed and may be empty. A value that
//   starts with " is quoted: it ends at the next unescaped ", the only
//   escapes are \" and \\ (any other backslash is literal), and only a
//   trailing comment may follow it.
// - Kinds, names, keys, and values are case-sensitive bytes. Unknown
//   kinds and keys are for the schema layer to judge, not this one.
// - Lines end in LF or CRLF, and a leading UTF-8 BOM is skipped.
//
// Includes: "include = path" before the first section reads another file
// at that point. The included file starts at its own top level, and the
// including file is still at its top level afterward, since its first
// section comes later. A relative path is resolved against the including
// file's directory, lexically: / and \ both separate, ".." drops the
// previous component, and only /, // (UNC), and a drive with a slash
// (C:/) start an absolute path, so C:x.ini is a relative name. Device
// paths (\\.\ and \\?\) are not supported. Includes nest at most 8 deep,
// a cycle is skipped, an include is skipped once the Ini holds 64 files,
// and reads, roots included, stop at 1 MiB of text in total. Inside a
// section, "include" is an error rather than a key, so no section can
// use it.
//
// URL includes: "include = https://host/team.ini", optionally pinned
// with "include = URL sha256=HEX" (64 hex digits). The URL is the
// included file's path, so diagnostics and provenance name it, and an
// include inside it resolves against it as a URL reference (RFC 3986):
// "base.ini", "../x.ini", "/x.ini" and "//host/x.ini" all name URLs, so
// a file from the web can include only more of the web, never a local
// file. Only http and https are taken, compared after lowercasing the
// scheme and host and dropping a fragment and dot segments, which the
// cycle check uses. This reader never fetches: the loader supplies the
// text through a hook (Ini.fetch), along with a diagnostic for the
// include line, and decides about caching, pins and timeouts.
//
// Layering: a later definition of a key overrides an earlier one in load
// order (an include's definitions land where the include line is), and
// each value keeps the file and line that defined it. Defining a key
// twice in one section of one file is a warning.
//
// Diagnostics never stop a load. An error means a line was ignored (for
// an invalid section header, also the keys under it); a warning means the
// line was used anyway, or that an include was skipped. A file reports at
// most 100 diagnostics, then one warning that the rest were suppressed,
// and one identical to an earlier one, as from a file included twice, is
// dropped.
//
// Every line is kept verbatim with the byte spans of its parts, so that
// a writer can replace one value in place and preserve the rest exactly.
// The writer (iniedit) edits one file's text: it sets a key by splicing
// its value span, adds a key after the section's last one or a section
// at the end, and removes a key by dropping its line. All other bytes
// come through unchanged, so one edit is a one-line diff. Another
// (inidelsection) deletes a section: its headers, the lines under them
// but for comments and blank lines, and the comment lines directly
// above each header, and the rest stays as it was but for blank lines
// that set it apart.
//
// This is free and unencumbered software released into the public domain.

// Each line costs an IniLine on top of its text, about 120 bytes on a
// 64-bit host, so the byte limit still allows over 100 MiB of lines (all
// blank) in the worst case: load into an arena discarded with the Ini.
enum {
    INI_MAXDEPTH = 8,      // include nesting below a loaded file
    INI_MAXFILES = 64,     // past this, includes are skipped
    INI_MAXBYTES = 1<<20,  // text per Ini in total, enforced on reads
    INI_MAXDIAGS = 100,    // per file, then one warning for the rest
};

// Line types
enum {
    INI_BLANK,
    INI_COMMENT,
    INI_SECTION,
    INI_KEY,
    INI_INCLUDE,
    INI_INVALID,  // ignored, with an error diagnostic
};

// Diagnostic severities
enum {
    INI_WARNING,  // the line was used, or an include was skipped
    INI_ERROR,    // the line was ignored
};

struct IniSection;

// One physical line, verbatim. Spans are byte offsets into raw: for keys
// and includes, the key and the value as written (quotes included); for
// a section header, the kind and the name. A writer replaces a value by
// splicing raw at [valbeg, valend). An empty value's span is empty and
// sits after the blanks following the =, so a writer inserting before a
// trailing comment must add a space. A missing name's span is empty and
// sits at the end of the kind. The section is the one a header opens or
// a key was defined in, and null on other lines, including keys ignored
// under an invalid header.
struct IniLine {
    Str         raw;      // without its line terminator
    Str         eol;      // "\n", "\r\n", or empty at the end of the text
    Str         key;      // kind for a section header
    Str         value;    // decoded: name for a header, path for include
    IniSection *section;
    iz          keybeg;
    iz          keyend;
    iz          valbeg;
    iz          valend;
    iz          num;      // 1-based line number
    i32         type;     // INI_BLANK, INI_COMMENT, ...
};

struct IniFile {
    IniFile       *next;    // in order of opening
    IniFile       *parent;  // the including file, or null for a root
    IniLine       *via;     // the include line in parent
    Str            path;
    Slice<IniLine> lines;
    i32            depth;   // 0 for a root
    i32            ndiags;  // reported, counted against INI_MAXDIAGS
    b32            bom;     // the text began with a UTF-8 byte order mark
};

// The winning definition of a key, linked to those it overrode.
struct IniEntry {
    IniEntry *next;   // next key in the section, by first definition
    IniEntry *older;  // overridden definitions, newest first
    Str       key;
    Str       value;
    IniFile  *file;   // provenance
    IniLine  *line;
};

struct IniSection {
    IniSection      *child[4];  // hash trie over (kind, name)
    IniSection      *next;      // in order of first appearance
    Str              kind;      // empty for the top level
    Str              name;      // empty for [kind] and the top level
    IniEntry        *keys;      // in order of first definition
    IniEntry        *lastkey;
    Map<IniEntry *> *index;
};

struct IniDiag {
    Str file;
    iz  line;      // 1-based, or 0 for the whole file
    iz  col;       // 1-based byte column, or 0 for the whole line
    i32 severity;  // INI_WARNING or INI_ERROR
    Str msg;
};

// The text of a URL include, from the loader's hook.
struct IniSource {
    Str data;      // null to skip the include
    Str msg;       // a diagnostic for the include line, or null
    i32 severity;  // of msg
};

// A layered configuration. The zero value is empty and ready to use.
struct Ini {
    IniFile        *files;        // in order of opening
    IniFile        *lastfile;
    IniSection     *sections;     // in order of first appearance
    IniSection     *lastsection;
    IniSection     *index;
    Slice<IniDiag>  diags;        // in order of discovery, without repeats
    Map<b32>       *seen;         // diagnostics recorded, for dropping repeats
    i32             nfiles;
    iz              nbytes;       // counted against INI_MAXBYTES

    // The text of a URL include, as canonical (iniurlclean), with its
    // pin as 64 lowercase hex digits or null. Null: URL includes are
    // skipped with a warning.
    IniSource     (*fetch)(void *ctx, Arena *perm, Str url, Str pin);
    void           *fetchctx;
};

struct IniParser {
    Ini        *ini;
    Os         *os;       // null: includes are not followed
    Arena      *perm;
    IniFile    *file;
    IniLine    *line;
    IniSection *section;  // null under an invalid header
    b32         top;      // before the first section header
};


// Sections and keys

static u64 inihash(Str kind, Str name)
{
    u64 h = hash(kind);
    h ^= ' ';
    h *= 1111111111111111111u;
    for (iz i = 0; i < name.len; i++) {
        h ^= name[i];
        h *= 1111111111111111111u;
    }
    return h;
}

// Find a section, or with an arena, create it as the last in order.
static IniSection *inisectionat(Ini *ini, Arena *perm, Str kind, Str name)
{
    IniSection **m = &ini->index;
    for (u64 h = inihash(kind, name); *m; h <<= 2) {
        if ((*m)->kind==kind && (*m)->name==name) {
            return *m;
        }
        m = &(*m)->child[h>>62];
    }
    if (!perm) {
        return 0;
    }
    IniSection *s = *m = alloc<IniSection>(perm);
    s->kind = kind;
    s->name = name;
    if (ini->lastsection) {
        ini->lastsection->next = s;
    } else {
        ini->sections = s;
    }
    ini->lastsection = s;
    return s;
}

// Section by kind and name, or null. The top level is ("", "").
static IniSection *inisection(Ini *ini, Str kind, Str name)
{
    return inisectionat(ini, 0, kind, name);
}

// Winning definition of a key, or null. The section may be null.
static IniEntry *inientry(IniSection *s, Str key)
{
    if (!s) {
        return 0;
    }
    IniEntry **e = upsert(&s->index, key, 0);
    return e ? *e : 0;
}

static IniEntry *inilookup(Ini *ini, Str kind, Str name, Str key)
{
    return inientry(inisection(ini, kind, name), key);
}

// Value of a key, or a null string if undefined. An empty value is not
// null.
static Str iniget(Ini *ini, Str kind, Str name, Str key)
{
    IniEntry *e = inilookup(ini, kind, name, key);
    return e ? e->value : Str{};
}

static IniSection *inikindfrom(IniSection *s, Str kind)
{
    for (; s && s->kind!=kind; s = s->next) {}
    return s;
}

// Iterate the sections of a kind in order of first appearance:
//   for (IniSection *s = inifirst(ini, "provider"); s; s = ininext(s))
static IniSection *inifirst(Ini *ini, Str kind)
{
    return inikindfrom(ini->sections, kind);
}

static IniSection *ininext(IniSection *s)
{
    return inikindfrom(s->next, s->kind);
}


// Paths

static b32 inisep(u8 c)
{
    return c=='/' || c=='\\';
}

// Length of a path's root: "//" (UNC), "/", "C:/", or 0. A drive without
// a slash has no root: on Windows a drive-relative path cannot be resolved
// lexically, and elsewhere "a:b.ini" is an ordinary name.
static iz iniroot(Str p)
{
    if (p.len>=2 && inisep(p[0]) && inisep(p[1])) {
        return 2;
    } else if (p.len>=1 && inisep(p[0])) {
        return 1;
    } else if (p.len>=3 && letter(p[0]) && p[1]==':' && inisep(p[2])) {
        return 3;
    }
    return 0;
}

// Whether a path has a Windows device or verbatim prefix, \\.\ or \\?\,
// whose meaning lexical cleaning would change (\\.\pipe would become a
// server named pipe).
static b32 inidevice(Str p)
{
    return p.len>=4 && inisep(p[0]) && inisep(p[1]) &&
           (p[2]=='.' || p[2]=='?') && inisep(p[3]);
}

// Directory of a path, keeping its root, or empty if it has none.
static Str inidir(Str p)
{
    iz i = p.len;
    for (; i>0 && !inisep(p[i-1]); i--) {}
    if (!i) {
        return {};
    }
    iz root = iniroot(p);
    return takehead(p, i-1<root ? root : i-1);
}

// Resolve a name against a directory lexically: either slash separates,
// "." components vanish, ".." drops the previous component, and the
// result uses / throughout. Equal results mean the same file, barring
// links and case folding, which the include depth limit covers. Device
// paths (inidevice) are not supported.
static Str inipath(Arena *perm, Str dir, Str name)
{
    Str p = {};
    if (iniroot(name) || !dir.len) {
        p = clone(perm, name);
    } else {
        p = clone(perm, dir);
        if (!inisep(dir[dir.len-1])) {
            p = concat(perm, p, "/");
        }
        p = concat(perm, p, name);
    }

    // Clean in place: the write position never passes the read position
    iz root = iniroot(p);
    for (iz i = 0; i < root; i++) {
        p[i] = inisep(p[i]) ? '/' : p[i];
    }
    iz len  = root;
    iz keep = 0;  // components that ".." may drop
    for (iz i = root; i < p.len;) {
        iz beg = i;
        for (; i<p.len && !inisep(p[i]); i++) {}
        Str comp = span(p.data+beg, p.data+i);
        i += i < p.len;

        if (!comp.len || comp==".") {
            continue;
        } else if (comp == "..") {
            if (keep) {
                for (; len>root && p[len-1]!='/'; len--) {}
                len -= len > root;
                keep--;
                continue;
            } else if (root) {
                continue;  // nothing is above the root
            }
        } else {
            keep++;
        }

        if (len > root) {
            p[len++] = '/';
        }
        for (iz k = 0; k < comp.len; k++) {
            p[len++] = comp[k];
        }
    }
    p.len = len;
    return p;
}

static b32 iniurl(Str s)
{
    return (s.len>=7 && equalfold(takehead(s, 7), "http://")) ||
           (s.len>=8 && equalfold(takehead(s, 8), "https://"));
}


// URLs

// Length of a reference's scheme with its colon ("http:"), or 0 if it
// has none, so that it is relative.
static iz inischeme(Str s)
{
    if (!s.len || !letter(s[0])) {
        return 0;
    }
    for (iz i = 1; i < s.len; i++) {
        u8 c = s[i];
        if (c == ':') {
            return i + 1;
        } else if (!alnum(c) && c!='+' && c!='-' && c!='.') {
            return 0;
        }
    }
    return 0;
}

// Append an absolute URL path with its "." and ".." segments removed
// (RFC 3986, 5.2.4). Empty segments stay, and ".." stops at the root.
static void printinipath(Buf *b, Str path)
{
    iz root = b->len;
    for (iz i = 0; i < path.len;) {
        iz beg = i + (path[i] == '/');
        iz end = beg;
        for (; end<path.len && path[end]!='/'; end++) {}
        Str seg  = span(path.data+beg, path.data+end);
        b32 last = end == path.len;
        if (seg==".." || seg==".") {
            if (seg == "..") {
                for (; b->len>root && b->data[b->len-1]!='/'; b->len--) {}
                b->len -= b->len > root;
            }
            if (last) {
                putbyte(b, '/');
            }
        } else {
            putbyte(b, '/');
            print(b, seg);
        }
        i = end;
    }
    if (b->len == root) {
        putbyte(b, '/');
    }
}

// An absolute http or https URL in canonical form: scheme and host in
// lowercase, a path of at least "/" without dot segments, any query as
// written, and no fragment. Null if it is not such a URL.
static Str iniurlclean(Arena *perm, Str url)
{
    url = cut(url, '#').head;
    if (!iniurl(url)) {
        return {};
    }
    iz  sep  = find(url, "://");
    Str rest = cuthead(url, sep+3);
    iz  end  = 0;
    for (; end<rest.len && rest[end]!='/' && rest[end]!='?'; end++) {}
    Str auth = takehead(rest, end);
    Cut path = cut(cuthead(rest, end), '?');
    if (!auth.len || cut(auth, '@').ok || auth[0]==':') {
        return {};  // no host, or credentials
    }

    Buf b(perm, url.len + 2);
    for (iz i = 0; i < sep+3+end; i++) {
        putbyte(&b, lowercase(url[i]));
    }
    printinipath(&b, path.head);
    if (path.ok) {
        putbyte(&b, '?');
        print(&b, path.tail);
    }
    Str r = finish(&b);
    for (iz i = 0; i < r.len; i++) {
        if (r[i]<=' ' || r[i]==0x7f || r[i]=='\\') {
            return {};
        }
    }
    return r;
}

// Resolve a reference against a canonical base URL (iniurlclean), as
// RFC 3986 does, into canonical form. Null if the result is not an http
// or https URL.
static Str iniurlresolve(Arena *perm, Str base, Str ref)
{
    ref = cut(ref, '#').head;
    if (inischeme(ref)) {
        return iniurlclean(perm, ref);
    } else if (!ref.len) {
        return base;  // a fragment alone
    }
    iz  sep  = find(base, "://");
    Str rest = cuthead(base, sep+3);
    iz  end  = 0;
    for (; end<rest.len && rest[end]!='/' && rest[end]!='?'; end++) {}
    Str scheme = takehead(base, sep+1);         // "https:"
    Str origin = takehead(base, sep+3+end);     // "https://host"
    Str path   = cut(cuthead(rest, end), '?').head;

    Str url = {};
    if (startswith(ref, "//")) {
        url = concat(perm, scheme, ref);
    } else if (startswith(ref, "/")) {
        url = concat(perm, origin, ref);
    } else if (startswith(ref, "?")) {
        url = concat(perm, concat(perm, origin, path), ref);
    } else {
        iz dir = path.len;
        for (; dir>0 && path[dir-1]!='/'; dir--) {}
        url = concat(perm, origin, takehead(path, dir));
        url = concat(perm, url, ref);
    }
    return iniurlclean(perm, url);
}

// A pin, "sha256=HEX", as 64 lowercase hex digits, or null.
static Str inipin(Arena *perm, Str s)
{
    if (s.len!=7+64 || !startswith(s, "sha256=")) {
        return {};
    }
    Str r = clone(perm, cuthead(s, 7));
    for (iz i = 0; i < r.len; i++) {
        u8 c = lowercase(r[i]);
        if (!digit(c) && (c<'a' || c>'f')) {
            return {};
        }
        r[i] = c;
    }
    return r;
}


// Parsing

// Record a diagnostic unless an identical one was recorded before, as
// when a file is included twice and parsed again.
static void inidiag(Ini *ini, Arena *perm, Str file, iz line, iz col,
                    i32 severity, Str msg)
{
    // Numbers first, then the file's length, so keys are unambiguous
    Buf key(perm, 64 + file.len + msg.len);
    print(&key, (i64)line);
    putbyte(&key, ' ');
    print(&key, (i64)col);
    putbyte(&key, ' ');
    print(&key, (i64)severity);
    putbyte(&key, ' ');
    print(&key, (i64)file.len);
    putbyte(&key, ' ');
    print(&key, file);
    print(&key, msg);
    b32 *seen = upsert(&ini->seen, finish(&key), perm);
    if (*seen) {
        return;
    }
    *seen = 1;

    IniDiag d  = {};
    d.file     = file;
    d.line     = line;
    d.col      = col;
    d.severity = severity;
    d.msg      = msg;
    ini->diags = push(perm, ini->diags, d);
}

// Whether the current file has used up its diagnostics, so that a caller
// can skip formatting a message inireport would drop.
static b32 inimuted(IniParser *p)
{
    return p->file->ndiags >= INI_MAXDIAGS;
}

// Report at a byte offset in the current line. In each file, the report
// after INI_MAXDIAGS becomes a whole-line warning that the rest were
// suppressed, and later ones are dropped, so msg may be null when muted.
static void inireport(IniParser *p, i32 severity, iz off, Str msg)
{
    IniFile *f = p->file;
    if (f->ndiags > INI_MAXDIAGS) {
        return;
    } else if (f->ndiags++ == INI_MAXDIAGS) {
        severity = INI_WARNING;
        off      = -1;
        msg      = "too many diagnostics, rest suppressed";
    }
    inidiag(p->ini, p->perm, f->path, p->line->num, off+1, severity, msg);
}

// Why os_read failed, in parentheses.
static void printinireason(Buf *b, i32 err)
{
    print(b, " (");
    switch (err) {
    case READ_NOTFOUND: print(b, "not found");  break;
    case READ_NOTFILE:  print(b, "not a file"); break;
    case READ_TOOBIG:
        print(b, "over the ");
        print(b, (i64)(INI_MAXBYTES >> 20));
        print(b, " MiB configuration limit");
        break;
    default:
        print(b, "read error");
    }
    putbyte(b, ')');
}

static iz inibudget(Ini *ini)
{
    iz left = INI_MAXBYTES - ini->nbytes;
    return left<0 ? 0 : left;
}

// Whether s[i] starts a trailing comment.
static b32 inicomment(Str s, iz i)
{
    return s[i]=='#' && i>0 && whitespace(s[i-1]) &&
           (i+1==s.len || whitespace(s[i+1]));
}

// Split text into lines, without interpreting them.
static Slice<IniLine> inisplit(Arena *perm, Str src)
{
    iz n = 0;
    for (iz i = 0; i < src.len; i++) {
        n += src[i] == '\n';
    }
    n += src.len && src[src.len-1]!='\n';

    Slice<IniLine> r = {};
    r.data = alloc<IniLine>(perm, n);
    r.len  = r.cap = n;
    Str rest = src;
    for (iz i = 0; i < n; i++) {
        Cut      c  = cut(rest, '\n');
        b32      cr = c.ok && c.head.len && c.head[c.head.len-1]=='\r';
        IniLine *l  = &r[i];
        l->raw = cuttail(c.head, cr);
        l->eol = {l->raw.data+l->raw.len, c.ok+cr};
        l->num = i + 1;
        rest = c.tail;
    }
    return r;
}

struct IniValue {
    Str value;  // decoded
    iz  beg;    // span as written
    iz  end;
    Str err;
    iz  errat;
    b32 ok;
};

// Parse the value starting at raw[beg], after the blanks following =.
static IniValue inivalue(Arena *perm, Str raw, iz beg)
{
    IniValue r = {};
    r.beg = r.end = beg;

    if (beg==raw.len || raw[beg]!='"') {
        iz end = beg;
        for (; end<raw.len && !inicomment(raw, end); end++) {}
        for (; end>beg && whitespace(raw[end-1]); end--) {}
        r.end   = end;
        r.value = span(raw.data+beg, raw.data+end);
        r.ok    = 1;
        return r;
    }

    iz i       = beg + 1;
    iz escapes = 0;
    for (; i<raw.len && raw[i]!='"'; i++) {
        if (raw[i]=='\\' && i+1<raw.len && (raw[i+1]=='"' || raw[i+1]=='\\')) {
            i++;
            escapes++;
        }
    }
    if (i == raw.len) {
        r.err   = "unterminated quoted value";
        r.errat = beg;
        return r;
    }
    r.end = i + 1;

    iz j = r.end;
    for (; j<raw.len && whitespace(raw[j]); j++) {}
    if (j<raw.len && !inicomment(raw, j)) {
        r.err   = "unexpected text after quoted value";
        r.errat = j;
        return r;
    }

    Str body = span(raw.data+beg+1, raw.data+i);
    r.value = body;
    if (escapes) {
        r.value.data = allocbytes(perm, body.len-escapes);
        r.value.len  = 0;
        for (iz k = 0; k < body.len; k++) {
            k += body[k]=='\\' && k+1<body.len &&
                 (body[k+1]=='"' || body[k+1]=='\\');
            r.value.data[r.value.len++] = body[k];
        }
    }
    r.ok = 1;
    return r;
}

// A section header starting with the [ at raw[lb].
static void iniheader(IniParser *p, IniLine *l, iz lb)
{
    Str raw = l->raw;
    p->top     = 0;
    p->section = 0;
    l->type    = INI_INVALID;

    iz rb = lb + 1;
    for (; rb<raw.len && raw[rb]!=']'; rb++) {}
    if (rb == raw.len) {
        Str msg = "unterminated section header, section ignored";
        inireport(p, INI_ERROR, lb, msg);
        return;
    }

    // At most two words, the kind and the name: [beg, end) each
    Str bad        = "expected [kind] or [kind name], section ignored";
    iz  word[2][2] = {};
    i32 nwords     = 0;
    for (iz i = lb+1; i < rb;) {
        for (; i<rb && whitespace(raw[i]); i++) {}
        if (i == rb) {
            break;
        }
        iz beg = i;
        for (; i<rb && !whitespace(raw[i]) && raw[i]!='['; i++) {}
        if (nwords == 2) {
            inireport(p, INI_ERROR, beg, bad);
            return;
        } else if (i<rb && raw[i]=='[') {
            inireport(p, INI_ERROR, i, bad);
            return;
        }
        word[nwords][0] = beg;
        word[nwords][1] = i;
        nwords++;
    }
    if (!nwords) {
        inireport(p, INI_ERROR, lb, bad);
        return;
    }

    l->type   = INI_SECTION;
    l->keybeg = word[0][0];
    l->keyend = word[0][1];
    l->valbeg = nwords==2 ? word[1][0] : l->keyend;
    l->valend = nwords==2 ? word[1][1] : l->keyend;
    l->key    = span(raw.data+l->keybeg, raw.data+l->keyend);
    l->value  = span(raw.data+l->valbeg, raw.data+l->valend);

    iz j = rb + 1;
    for (; j<raw.len && whitespace(raw[j]); j++) {}
    if (j<raw.len && raw[j]!='#' && raw[j]!=';') {
        inireport(p, INI_WARNING, j, "unexpected text after section header");
    }
    p->section = inisectionat(p->ini, p->perm, l->key, l->value);
    l->section = p->section;
}

static void inidefine(IniParser *p, IniLine *l)
{
    IniSection *s = p->top ? inisectionat(p->ini, p->perm, "", "") : p->section;
    if (!s) {
        return;  // under an invalid section header, as l->section shows
    }
    l->section = s;

    IniEntry **slot = upsert(&s->index, l->key, p->perm);
    IniEntry  *e    = *slot;
    if (!e) {
        e = *slot = alloc<IniEntry>(p->perm);
        if (s->lastkey) {
            s->lastkey->next = e;
        } else {
            s->keys = e;
        }
        s->lastkey = e;
    } else {
        for (IniEntry *d = e; d; d = d->older) {
            if (d->file == p->file) {
                Str msg = {};
                if (!inimuted(p)) {
                    Buf b(p->perm, 48);
                    print(&b, "duplicate key overrides line ");
                    print(&b, (i64)d->line->num);
                    msg = finish(&b);
                }
                inireport(p, INI_WARNING, l->keybeg, msg);
                break;
            }
        }
        IniEntry *old = alloc<IniEntry>(p->perm);
        *old = *e;
        old->next = 0;
        e->older  = old;
    }
    e->key   = l->key;
    e->value = l->value;
    e->file  = p->file;
    e->line  = l;
}

static IniFile *ininewfile(Ini *ini, Arena *perm, Str path, Str src)
{
    IniFile *f = alloc<IniFile>(perm);
    f->path = path;
    ini->nbytes += src.len;
    if (startswith(src, "\xef\xbb\xbf")) {
        f->bom = 1;
        src = cuthead(src, 3);
    }
    f->lines = inisplit(perm, src);

    if (ini->lastfile) {
        ini->lastfile->next = f;
    } else {
        ini->files = f;
    }
    ini->lastfile = f;
    ini->nfiles++;
    return f;
}

static void iniparsefile(Ini *, Os *, Arena *, IniFile *);

// Whether an include of path (a file's, or a URL) may be followed from
// the current file: not a cycle, nor too deep, nor too many files.
// Otherwise it reports why.
static b32 inifollow(IniParser *p, IniLine *l, Str path)
{
    // Messages are formatted only when they will be kept
    IniFile *f   = p->file;
    Str      msg = {};
    for (IniFile *a = f; a; a = a->parent) {
        if (a->path == path) {
            if (!inimuted(p)) {
                msg = concat(p->perm, "include cycle: ", path);
            }
            inireport(p, INI_WARNING, l->valbeg, msg);
            return 0;
        }
    }

    if (f->depth == INI_MAXDEPTH) {
        if (!inimuted(p)) {
            Buf b(p->perm, 48);
            print(&b, "includes nested deeper than ");
            print(&b, (i64)INI_MAXDEPTH);
            msg = finish(&b);
        }
        inireport(p, INI_WARNING, l->valbeg, msg);
        return 0;
    } else if (p->ini->nfiles >= INI_MAXFILES) {
        if (!inimuted(p)) {
            Buf b(p->perm, 48);
            print(&b, "more than ");
            print(&b, (i64)INI_MAXFILES);
            print(&b, " configuration files");
            msg = finish(&b);
        }
        inireport(p, INI_WARNING, l->valbeg, msg);
        return 0;
    }
    return 1;
}

// Parse included text as a file under the current one.
static void iniopen(IniParser *p, IniLine *l, Str path, Str src)
{
    IniFile *f   = p->file;
    IniFile *inc = ininewfile(p->ini, p->perm, path, src);
    inc->parent = f;
    inc->via    = l;
    inc->depth  = f->depth + 1;
    iniparsefile(p->ini, p->os, p->perm, inc);
}

// An include of a URL: one written as a URL, or any include in a file
// that came from a URL, which is a reference against it. A pin may
// follow the URL.
static void iniweb(IniParser *p, IniLine *l)
{
    IniFile *f   = p->file;
    Str      v   = l->value;
    iz       i   = 0;
    for (; i<v.len && !whitespace(v[i]); i++) {}
    Str      ref = takehead(v, i);
    Str      opt = trim(cuthead(v, i));
    Str      pin = {};
    if (opt.len) {
        pin = inipin(p->perm, opt);
        if (!pin.data) {
            Str msg = "expected sha256= and 64 hex digits after the URL, "
                      "include ignored";
            l->type = INI_INVALID;
            inireport(p, INI_ERROR, l->valbeg, msg);
            return;
        }
    }
    Str url = iniurl(f->path) ? iniurlresolve(p->perm, f->path, ref)
                              : iniurlclean(p->perm, ref);
    if (!url.data) {
        Str msg = "expected an http:// or https:// URL, include ignored";
        l->type = INI_INVALID;
        inireport(p, INI_ERROR, l->valbeg, msg);
        return;
    } else if (!p->os || !inifollow(p, l, url)) {
        return;
    } else if (!p->ini->fetch) {
        inireport(p, INI_WARNING, l->valbeg, "URL includes are not read here");
        return;
    }

    IniSource s = p->ini->fetch(p->ini->fetchctx, p->perm, url, pin);
    if (s.msg.data) {
        inireport(p, s.severity, l->valbeg, s.msg);
    }
    if (!s.data.data) {
        return;
    } else if (s.data.len > inibudget(p->ini)) {
        Str msg = {};
        if (!inimuted(p)) {
            Buf b(p->perm, 64 + url.len);
            print(&b, "cannot read include ");
            print(&b, url);
            printinireason(&b, READ_TOOBIG);
            msg = finish(&b);
        }
        inireport(p, INI_WARNING, l->valbeg, msg);
        return;
    }
    iniopen(p, l, url, s.data);
}

static void iniinclude(IniParser *p, IniLine *l)
{
    if (!p->top) {
        Str msg = "include must come before the first section";
        l->type = INI_INVALID;
        inireport(p, INI_ERROR, l->keybeg, msg);
        return;
    } else if (!l->value.len) {
        l->type = INI_INVALID;
        inireport(p, INI_ERROR, l->valbeg, "missing include path");
        return;
    }
    l->type = INI_INCLUDE;

    IniFile *f = p->file;
    if (iniurl(l->value) || iniurl(f->path)) {
        iniweb(p, l);
        return;
    } else if (inidevice(l->value)) {
        inireport(p, INI_WARNING, l->valbeg, "device paths are not supported");
        return;
    } else if (!p->os) {
        return;  // text only
    }

    Str path = inipath(p->perm, inidir(f->path), l->value);
    if (!inifollow(p, l, path)) {
        return;
    }

    Read r = os_read(p->os, p->perm, path, inibudget(p->ini));
    if (r.err) {
        Str msg = {};
        if (!inimuted(p)) {
            Buf b(p->perm, 64);
            print(&b, "cannot read include ");
            print(&b, path);
            printinireason(&b, r.err);
            msg = finish(&b);
        }
        inireport(p, INI_WARNING, l->valbeg, msg);
        return;
    }
    iniopen(p, l, path, r.data);
}

// A key line whose first non-blank byte is raw[beg].
static void inikeyline(IniParser *p, IniLine *l, iz beg)
{
    Str raw = l->raw;
    l->type = INI_INVALID;

    iz eq = beg;
    for (; eq<raw.len && raw[eq]!='='; eq++) {}
    if (eq == raw.len) {
        inireport(p, INI_ERROR, beg, "expected key = value");
        return;
    }
    iz end = eq;
    for (; end>beg && whitespace(raw[end-1]); end--) {}
    if (end == beg) {
        inireport(p, INI_ERROR, eq, "missing key before =");
        return;
    }
    for (iz i = beg; i < end; i++) {
        if (whitespace(raw[i])) {
            inireport(p, INI_ERROR, i, "whitespace in key");
            return;
        }
    }

    iz vbeg = eq + 1;
    for (; vbeg<raw.len && whitespace(raw[vbeg]); vbeg++) {}
    IniValue v = inivalue(p->perm, raw, vbeg);
    if (!v.ok) {
        inireport(p, INI_ERROR, v.errat, v.err);
        return;
    }

    l->type   = INI_KEY;
    l->keybeg = beg;
    l->keyend = end;
    l->valbeg = v.beg;
    l->valend = v.end;
    l->key    = span(raw.data+beg, raw.data+end);
    l->value  = v.value;
    if (l->key == "include") {
        iniinclude(p, l);
    } else {
        inidefine(p, l);
    }
}

static void iniparsefile(Ini *ini, Os *os, Arena *perm, IniFile *f)
{
    IniParser p = {};
    p.ini  = ini;
    p.os   = os;
    p.perm = perm;
    p.file = f;
    p.top  = 1;
    for (iz n = 0; n < f->lines.len; n++) {
        IniLine *l   = &f->lines[n];
        Str      raw = l->raw;
        iz       i   = 0;
        p.line = l;
        for (; i<raw.len && whitespace(raw[i]); i++) {}
        if (i == raw.len) {
            l->type = INI_BLANK;
        } else if (raw[i]=='#' || raw[i]==';') {
            l->type = INI_COMMENT;
        } else if (raw[i] == '[') {
            iniheader(&p, l, i);
        } else {
            inikeyline(&p, l, i);
        }
    }
}

// Parse text on top of ini as a file at the given path, for diagnostics
// and provenance. Includes are checked but not followed. Lines, keys,
// values, and diagnostics point into path and src, which must outlive
// ini. The text counts toward INI_MAXBYTES but is never refused.
static IniFile *iniparse(Ini *ini, Arena *perm, Str path, Str src)
{
    IniFile *f = ininewfile(ini, perm, path, src);
    iniparsefile(ini, 0, perm, f);
    return f;
}

// Read a file on top of ini, following includes, with all results in
// perm. The path is normalized as by inipath, so it should be absolute:
// a Windows drive-relative path (C:x.ini) is a relative name, and its
// includes resolve against the working directory. Returns null, with a
// warning, if the file cannot be read.
static IniFile *iniload(Ini *ini, Os *os, Arena *perm, Str path)
{
    if (inidevice(path)) {
        Str msg = "cannot read file (device paths are not supported)";
        inidiag(ini, perm, clone(perm, path), 0, 0, INI_WARNING, msg);
        return 0;
    }
    path = inipath(perm, {}, path);
    Read r = os_read(os, perm, path, inibudget(ini));
    if (r.err) {
        Buf b(perm, 48);
        print(&b, "cannot read file");
        printinireason(&b, r.err);
        inidiag(ini, perm, path, 0, 0, INI_WARNING, finish(&b));
        return 0;
    }
    IniFile *f = ininewfile(ini, perm, path, r.data);
    iniparsefile(ini, os, perm, f);
    return f;
}


// Output

// As "file:line:col: severity: message", omitting what is unknown.
static void print(Buf *b, IniDiag d)
{
    iz start = b->len;
    if (d.file.len) {
        print(b, d.file);
        putbyte(b, ':');
    }
    if (d.line) {
        print(b, (i64)d.line);
        putbyte(b, ':');
    }
    if (d.col) {
        print(b, (i64)d.col);
        putbyte(b, ':');
    }
    if (b->len > start) {
        putbyte(b, ' ');
    }
    print(b, d.severity==INI_ERROR ? Str("error: ") : Str("warning: "));
    print(b, d.msg);
}

// Reproduce a file's text exactly.
static void printinifile(Buf *b, IniFile *f)
{
    if (f->bom) {
        print(b, "\xef\xbb\xbf");
    }
    for (iz i = 0; i < f->lines.len; i++) {
        print(b, f->lines[i].raw);
        print(b, f->lines[i].eol);
    }
}

// Append a value as written after "key = ", quoted only when necessary,
// or always with quote, so that it parses back to itself. Returns 0,
// appending nothing, if the value contains a newline, which has no
// representation.
static b32 printinivalue(Buf *b, Str v, b32 quote = 0)
{
    quote |= v.len && (v[0]=='"' || whitespace(v[0]) ||
                       whitespace(v[v.len-1]));
    for (iz i = 0; i < v.len; i++) {
        if (v[i] == '\n') {
            return 0;
        }
        // Would start a comment: assume a blank precedes the value
        quote |= v[i]=='#' && (!i || whitespace(v[i-1])) &&
                 (i+1==v.len || whitespace(v[i+1]));
    }
    if (!quote) {
        print(b, v);
        return 1;
    }

    putbyte(b, '"');
    for (iz i = 0; i < v.len; i++) {
        b32 last = i+1 == v.len;
        if (v[i]=='"' ||
            (v[i]=='\\' && (last || v[i+1]=='"' || v[i+1]=='\\'))) {
            putbyte(b, '\\');
        }
        putbyte(b, v[i]);
    }
    putbyte(b, '"');
    return 1;
}


// Editing
//
// Edits work on one file's lines, never on the layered result: the
// caller picks the file (a user's own, never an include) and writes the
// new text back. Lines not edited come through byte for byte, comments,
// blanks, order, a BOM and CRLF included.

struct IniEdit {
    Str text;     // the whole new text, null on failure
    Str err;
    iz  line;     // 1-based line of the key in text, 0 once removed
    b32 changed;  // text differs from the file's
};

// Whether a word can be a section kind or name, or with key set a key,
// such that the edited text parses back to it.
static b32 iniword(Str s, b32 key)
{
    if (!s.len) {
        return 0;
    }
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (whitespace(c) || c<0x20 || c==0x7f || c=='[' || c==']' ||
                (key && c=='=')) {
            return 0;
        }
    }
    return !key || (s[0]!='#' && s[0]!=';' && !(s == "include"));
}

// The line terminator for new lines: the file's first one, else LF.
static Str inieol(IniFile *f)
{
    for (iz i = 0; i < f->lines.len; i++) {
        if (f->lines[i].eol.len) {
            return f->lines[i].eol;
        }
    }
    return "\n";
}

static Str iniindent(Str raw)
{
    iz i = 0;
    for (; i<raw.len && whitespace(raw[i]); i++) {}
    return takehead(raw, i);
}

static b32 iniin(IniLine *l, Str kind, Str name)
{
    IniSection *s = l->section;
    return s && s->kind==kind && s->name==name;
}

// Set a key of section (kind, name) in one file's text to value, or
// remove it given a null value, with everything in perm. The top level
// is ("", "").
//
// - A key the file defines gets the new value in place of its last
//   definition (the one in effect), unless that already decodes to it.
//   With move, its definitions go instead, and it is added anew as a
//   new key is, which for the top level is after the includes: where it
//   wins over an include that also defines it.
// - A new key goes after the section's last key in the file, or after
//   its header, with the same indentation. A new top-level key goes
//   after a commented-out example of it (# key = ...) past the last
//   include, else after the last top-level key or include, or else
//   before the comments leading into the first section header, with a
//   blank line after it.
// - A new section goes at the end, after a blank line.
// - Removing drops every definition of the key in the section.
//
// Values are quoted only when they need it (printinivalue), and new
// lines end like the file's first line. With quote 1 a value is quoted
// regardless, and with 0 only as it needs, and either way it replaces
// a definition written otherwise, even one that decodes to it.
static IniEdit iniedit(Arena *perm, IniFile *f, Str kind, Str name,
                       Str key, Str value, b32 move = 0, i32 quote = -1)
{
    IniEdit r   = {};
    b32     top = !kind.len;
    if (!iniword(key, 1) || (top ? name.len>0 : !iniword(kind, 0)) ||
            (name.len && !iniword(name, 0))) {
        r.err = "invalid section or key name";
        return r;
    }

    Str enc = {};
    if (value.data) {
        Buf v(perm, value.len + 8);
        if (!printinivalue(&v, value, quote>0)) {
            r.err = "a value cannot contain a newline";
            return r;
        }
        enc = finish(&v);
    }

    iz n       = f->lines.len;
    iz last    = -1;  // the definition in effect
    iz anchor  = -1;  // the line a new key follows
    iz header  = -1;  // the section's last header
    iz first   = -1;  // the first line that starts any section
    iz incl    = -1;  // the last include
    iz example = -1;  // a top-level comment "# key = ..."
    for (iz i = 0; i < n; i++) {
        IniLine *l  = &f->lines[i];
        b32      in = iniin(l, kind, name);
        if (l->type==INI_KEY && in) {
            anchor = i;
            last   = l->key==key ? i : last;
        } else if (l->type==INI_INCLUDE && top) {
            anchor = i;
            incl   = i;
        } else if (l->type==INI_SECTION && in) {
            header = i;
        } else if (l->type==INI_COMMENT && top && first<0 && example<0) {
            Str c = trimleft(cuthead(trimleft(l->raw), 1));
            if (startswith(c, key) && startswith(trimleft(cuthead(c, key.len)), "=")) {
                example = i;
            }
        }
        if (first<0 && startswith(trimleft(l->raw), "[")) {
            first = i;
        }
    }

    Str eol  = inieol(f);
    iz  at   = -1;  // insert before this line
    b32 gap  = 0;   // a blank line after the inserted key
    b32 drop = 0;   // remove the key's definitions
    Buf add(perm, 64 + kind.len + name.len + key.len + enc.len);
    if (value.data && move && last>=0) {
        drop = 1;
        last = -1;
    }
    if (!value.data) {
        for (iz i = 0; i < n; i++) {
            IniLine *l = &f->lines[i];
            drop |= l->type==INI_KEY && iniin(l, kind, name) && l->key==key;
        }
        r.changed = drop;
    } else if (last >= 0) {
        IniLine *l   = &f->lines[last];
        Str      was = span(l->raw.data+l->valbeg, l->raw.data+l->valend);
        r.line    = last + 1;
        r.changed = quote<0 ? !(l->value == value) : !(was == enc);
    } else if (top && example>incl) {
        at = example + 1;
    } else if (anchor>=0 || header>=0) {
        iz after = anchor>=0 ? anchor : header;
        at = after + 1;
        print(&add, iniindent(f->lines[after].raw));
    } else if (top && first>=0) {
        at  = first;
        gap = 1;
        for (; at>0 && f->lines[at-1].type==INI_COMMENT; at--) {}
    } else if (top) {
        at = n;
    } else {
        at = n;
        if (n && f->lines[n-1].type!=INI_BLANK) {
            print(&add, eol);
        }
        putbyte(&add, '[');
        print(&add, kind);
        if (name.len) {
            putbyte(&add, ' ');
            print(&add, name);
        }
        putbyte(&add, ']');
        print(&add, eol);
    }
    if (at >= 0) {
        // The inserted text holds whole lines: count those before the key,
        // and those dropped before it
        r.line = at + 1;
        for (iz i = 0; i < add.len; i++) {
            r.line += add.data[i] == '\n';
        }
        for (iz i = 0; drop && i < at; i++) {
            IniLine *l = &f->lines[i];
            r.line -= l->type==INI_KEY && iniin(l, kind, name) && l->key==key;
        }
        print(&add, key);
        print(&add, " = ");
        print(&add, enc);
        print(&add, eol);
        if (gap) {
            print(&add, eol);
        }
        r.changed = 1;
    }

    Buf b(perm, 64);
    if (f->bom) {
        print(&b, "\xef\xbb\xbf");
    }
    for (iz i = 0; i < n; i++) {
        IniLine *l = &f->lines[i];
        if (i == at) {
            print(&b, finish(&add));
        }
        if (drop && l->type==INI_KEY && iniin(l, kind, name) && l->key==key) {
            continue;
        }
        if (i==last && value.data && r.changed) {
            // An empty value's span sits right before a trailing comment,
            // and maybe right after "=": keep the value apart from both
            Str head = takehead(l->raw, l->valbeg);
            Str rest = cuthead(l->raw, l->valend);
            b32 was  = l->valbeg < l->valend;
            print(&b, head);
            if (!was && enc.len && head.len>=2 && head[head.len-1]=='=' &&
                    whitespace(head[head.len-2])) {
                putbyte(&b, ' ');  // "a =" becomes "a = x", not "a =x"
            }
            print(&b, enc);
            if (enc.len && rest.len && !whitespace(rest[0])) {
                putbyte(&b, ' ');
            }
            print(&b, rest);
        } else {
            print(&b, l->raw);
        }
        // Only the last line may lack a terminator
        print(&b, l->eol.len || at!=n ? l->eol : eol);
    }
    if (at == n) {
        print(&b, finish(&add));
    }
    r.text = finish(&b);
    return r;
}

// Delete section (kind, name) from one file's text, with everything in
// perm: each of its headers, the keys and malformed lines under them,
// and the comment block directly above each header (comment lines with
// no blank line between them and it). Comments and blank lines among
// its keys stay, since they may be about anything, as does everything
// else. (A malformed line goes because one, such as an include inside a
// section, might mean something where it would land.) Where a deleted
// stretch was set apart by blank lines, the blanks after it go too, and
// at the end of the text those before it, so that deleting a section
// iniedit added gives the text back. A text without a final line
// terminator still lacks one. Deleting a section the file lacks changes
// nothing.
static IniEdit inidelsection(Arena *perm, IniFile *f, Str kind, Str name)
{
    IniEdit r = {};
    if (!iniword(kind, 0) || (name.len && !iniword(name, 0))) {
        r.err = "invalid section name";
        return r;
    }

    iz   n    = f->lines.len;
    b32 *drop = alloc<b32>(perm, n);
    b32  in   = 0;  // under one of its headers
    for (iz i = 0; i < n; i++) {
        IniLine *l = &f->lines[i];
        if (startswith(trimleft(l->raw), "[")) {
            in = l->type==INI_SECTION && iniin(l, kind, name);
            for (iz k = i; in && k>0 && f->lines[k-1].type==INI_COMMENT; k--) {
                drop[k-1] = 1;
            }
            drop[i]    = in;
            r.changed |= in;
        } else if (in && (l->type==INI_KEY || l->type==INI_INVALID)) {
            drop[i] = 1;
        }
    }

    // Each deleted stretch, joined to the next across blank lines
    for (iz i = 0; i < n;) {
        if (!drop[i]) {
            i++;
            continue;
        }
        iz lo = i;
        iz hi = i;
        iz j  = i;
        for (;;) {
            for (; hi<n && drop[hi]; hi++) {}
            for (j = hi; j<n && f->lines[j].type==INI_BLANK; j++) {}
            if (j==n || !drop[j]) {
                break;
            }
            for (; hi < j; hi++) {
                drop[hi] = 1;
            }
        }
        b32 last  = j == n;
        b32 apart = !lo || f->lines[lo-1].type==INI_BLANK;
        for (iz k = hi; (last || apart) && k < j; k++) {
            drop[k] = 1;
        }
        for (iz k = lo; last && k>0 && f->lines[k-1].type==INI_BLANK; k--) {
            drop[k-1] = 1;
        }
        i = j;
    }

    iz keep = -1;  // the last line kept
    for (iz i = 0; i < n; i++) {
        keep = drop[i] ? keep : i;
    }
    b32 open = n && !f->lines[n-1].eol.len;  // no final terminator
    Buf b(perm, 64);
    if (f->bom) {
        print(&b, "\xef\xbb\xbf");
    }
    for (iz i = 0; i < n; i++) {
        if (!drop[i]) {
            print(&b, f->lines[i].raw);
            print(&b, i==keep && open ? Str{} : f->lines[i].eol);
        }
    }
    r.text = finish(&b);
    return r;
}

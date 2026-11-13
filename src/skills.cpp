// Agent skills: SKILL.md files, as Claude Code and other harnesses keep them
//
// A skill is a directory holding SKILL.md: frontmatter between --- lines
// that names it and says when to use it, then instructions, which the
// agent reads when it uses the skill (the skill tool), or which the
// user's prompt carries when the user invokes it (/name). Skills sit in
// skill directories, DIR/NAME/SKILL.md, or below folders that group
// them, DIR/GROUP/NAME/SKILL.md, as Cursor allows. The harness looks
// through skill directories in this order: .claude/skills,
// .agents/skills, .cursor/skills and .codex/skills in each directory
// from the working directory up to the repository root (instrroot),
// then [skills] paths, then the user's own: skills next to the
// configuration (~/.config/prospero/skills), then the same four under
// ~. A skill found earlier wins over a later one of the
// same name: nearer the working directory, the project's over the
// user's. A project's skill must lie within the project, symlinks
// resolved, as its instruction files must.
//
// The frontmatter is a small part of YAML: key: value lines, a value
// plain, quoted, or continued on indented lines, or a block scalar (|
// or >). The name defaults to the directory's, the description to the
// body's first paragraph, and disable-model-invocation: true keeps the
// skill out of the agent's list and its tool, for the user alone. Other
// keys are ignored. A SKILL.md with malformed frontmatter is skipped,
// as is one too large, and the skills past SKILL_MAX.
//
// One skill is built in, Prospero's own (help.cpp), listed after those
// found unless one of the user's own has its name. A project's may not:
// a repository cannot pass its instructions off as Prospero's.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    SKILL_MAX   = 200,        // skills at most
    SKILL_DESC  = 300,        // bytes of a description
    SKILL_BYTES = 256 << 10,  // largest SKILL.md
    SKILL_HEAD  = 16 << 10,   // its frontmatter ends within this
    SKILL_NAME  = 64,         // bytes of a name
    SKILL_DEPTH = 4,          // levels a skill may sit below its skills directory
    SKILL_DIRS  = 1000,       // directories listed in one skills directory
    SKILL_PACK  = 2 << 20,    // a package's JSON (skillpack), at most
    SKILL_FILES = 500,        // files in a package
    SKILL_PACKDEPTH = 8,      // levels of a path in a package
    SKILL_PATHLEN   = 200,    // bytes of a path in a package
};

struct Skill {
    Str name;
    Str desc;     // one line, at most SKILL_DESC bytes
    Str dir;      // its directory, normalized; null if built in or relayed
    Str path;     // its SKILL.md; null if built in or relayed
    b32 hidden;   // disable-model-invocation: the user's alone
    b32 builtin;  // Prospero's own (help.cpp), with no files
    i64 peer;     // relayed: the computer that offers it (relay.cpp), else 0
    Str from;     // and that computer's name
};

// The built-in skill's name, which only the user's own skills may take.
static Str skillbuiltin = "prospero";

struct Skills {
    Slice<Skill> list;     // by name
    Slice<Str>   skipped;  // "PATH: why", for those left out
};

// A directory of skills, and the project it must lie in, if any.
struct SkillDir {
    Str path;
    Str inside;
};

// A SKILL.md's frontmatter, and the rest.
struct SkillMeta {
    Str name;    // null if not given
    Str desc;    // null if not given
    Str body;
    Str err;     // why the frontmatter is malformed, else null
    b32 hidden;
};

// Whether a name is one a skill may have: what /name takes as a word.
static b32 skillname(Str s)
{
    if (!s.len || s.len>SKILL_NAME || s[0]=='.' || s[0]=='-') {
        return 0;
    }
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (!alnum(c) && c!='-' && c!='_' && c!='.') {
            return 0;
        }
    }
    return 1;
}

// Runs of whitespace as single spaces, trimmed.
static Str skilloneline(Arena *a, Str s)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        if (!whitespace(s[i])) {
            putbyte(&b, s[i]);
        } else if (b.len && !whitespace(s[i-1])) {
            putbyte(&b, ' ');
        }
    }
    return trimright(finish(&b));
}

// A scalar's value: quoted ("..." with backslash escapes, '...' with ''
// for a quote), else plain, without a trailing comment. Not ok for a
// quote that never closes, or text after it.
static Str skillscalar(Arena *a, Str v, b32 *ok)
{
    *ok = 1;
    v = trim(v);
    if (!v.len || (v[0]!='"' && v[0]!='\'')) {
        for (iz i = 1; i < v.len; i++) {
            if (v[i]=='#' && whitespace(v[i-1])) {
                return trimright(takehead(v, i));
            }
        }
        return v;
    }
    u8  q = v[0];
    Buf b(a, v.len);
    iz  i = 1;
    for (; i < v.len; i++) {
        u8 c = v[i];
        if (c==q && q=='\'' && i+1<v.len && v[i+1]=='\'') {
            i++;
        } else if (c == q) {
            break;
        } else if (c=='\\' && q=='"' && i+1<v.len) {
            c = v[++i];
            c = c=='n' ? '\n' : c=='t' ? '\t' : c;
        }
        putbyte(&b, c);
    }
    Str rest = i<v.len ? trim(cuthead(v, i+1)) : Str{};
    *ok = i<v.len && (!rest.len || rest[0]=='#');
    return finish(&b);
}

static b32 skillindented(Str line)
{
    return line.len && (line[0]==' ' || line[0]=='\t');
}

// A line's content without its carriage return.
static Str skillline(Str line)
{
    return line.len && line[line.len-1]=='\r' ? takehead(line, line.len-1) : line;
}

// Parse a SKILL.md: frontmatter, if it opens with ---, through the next
// --- (or ...), then the body. No frontmatter is no error.
static SkillMeta skillparse(Arena *a, Str text)
{
    SkillMeta r = {};
    if (startswith(text, "\xef\xbb\xbf")) {
        text = cuthead(text, 3);
    }
    Cut first = cut(text, '\n');
    if (trimright(skillline(first.head)) != "---") {
        r.body = text;
        return r;
    }

    // The frontmatter's lines, up to the closing one
    Slice<Str> lines  = {};
    b32        closed = 0;
    Str        rest   = first.tail;
    while (rest.len && !closed) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str line = skillline(c.head);
        Str t    = trimright(line);
        closed = t=="---" || t=="...";
        if (!closed) {
            lines = push(a, lines, line);
        }
    }
    if (!closed) {
        r.err = "its frontmatter has no closing ---";
        return r;
    }
    r.body = rest;

    for (iz i = 0; i < lines.len;) {
        Str line = lines[i++];
        Str t    = trim(line);
        if (!t.len || t[0]=='#') {
            continue;
        } else if (skillindented(line) || t[0]=='-') {
            r.err = "its frontmatter has a line that belongs to no key";
            return r;
        }
        Cut kv = cut(line, ':');
        Str key = trim(kv.head);
        if (!kv.ok || !key.len || (kv.tail.len && !whitespace(kv.tail[0]))) {
            r.err = "its frontmatter has a line that is not key: value";
            return r;
        }

        // The key's other lines: indented, blank, or items of a list
        iz beg = i;
        for (; i < lines.len; i++) {
            Str l = lines[i];
            if (!skillindented(l) && trim(l).len && l[0]!='-' && l[0]!='#') {
                break;
            }
        }
        Str v   = trim(kv.tail);
        b32 ok  = 1;
        Buf b(a, 256);
        if (v.len && (v[0]=='|' || v[0]=='>')) {
            // A block scalar: its lines, less the first one's indent
            iz ind = -1;
            for (iz k = beg; k < i; k++) {
                Str l = lines[k];
                if (!trim(l).len) {
                    putbyte(&b, '\n');
                    continue;
                }
                if (ind < 0) {
                    for (ind = 0; ind<l.len && whitespace(l[ind]); ind++) {}
                }
                iz n = 0;
                for (; n<ind && n<l.len && whitespace(l[n]); n++) {}
                if (b.len && b.data[b.len-1]!='\n') {
                    putbyte(&b, v[0]=='|' ? '\n' : ' ');
                }
                print(&b, cuthead(l, n));
            }
        } else {
            // Plain or quoted, its lines folded into one
            print(&b, v);
            for (iz k = beg; k < i; k++) {
                Str l = trim(lines[k]);
                if (l.len) {
                    putbyte(&b, ' ');
                    print(&b, l);
                }
            }
        }
        Str val = skillscalar(a, finish(&b), &ok);
        if (!ok) {
            r.err = concat(a, concat(a, "its frontmatter's ", key),
                           " has a quote that does not close");
            return r;
        }
        if (key == "name") {
            r.name = val;
        } else if (key == "description") {
            r.desc = val;
        } else if (key == "disable-model-invocation") {
            Str s = trim(val);
            r.hidden = equalfold(s, "true") || equalfold(s, "yes") || equalfold(s, "on");
        }
    }
    if (r.name.data && !skillname(trim(r.name))) {
        r.err = "its name must be letters, digits, - _ and . (at most 64)";
        return r;
    }
    r.name = r.name.data ? trim(r.name) : Str{};
    return r;
}

// The first paragraph of a body, as a description when none is given:
// its heading marks dropped.
static Str skillpara(Arena *a, Str body)
{
    Buf b(a, 256);
    for (Str rest = body; rest.len && b.len<=SKILL_DESC;) {
        Cut c = cut(rest, '\n');
        Str t = trim(c.head);
        rest = c.tail;
        if (!t.len && b.len) {
            break;
        } else if (t.len) {
            print(&b, b.len ? Str(" ") : Str(""));
            print(&b, t);
        }
    }
    Str p = finish(&b);
    for (; p.len && (p[0]=='#' || whitespace(p[0])); p = cuthead(p, 1)) {}
    return p;
}

static Skill *skillbyname(Slice<Skill> list, Str name)
{
    for (iz i = 0; i < list.len; i++) {
        if (list[i].name == name) {
            return &list[i];
        }
    }
    return 0;
}

static b32 skillless(Skill *a, Skill *b)
{
    return compare(a->name, b->name) < 0;
}

// The directories under a skills directory that hold a SKILL.md, in a
// fixed order. A skill may sit below folders that group skills, as
// Cursor allows, down to SKILL_DEPTH levels, though a skill's own
// directory is not searched further. A bound on directories listed keeps
// a stray tree, or a symlink loop, cheap.
static Slice<Str> skillwalk(Os *os, Arena *a, Str root, b32 win)
{
    Slice<Str> r     = {};
    Slice<Str> stack = {};  // directories to list, deepest last
    Slice<i32> depth = {};
    stack = push(a, stack, root);
    depth = push(a, depth, 1);
    for (iz listed = 0; stack.len && listed<SKILL_DIRS; listed++) {
        Str dir = stack[stack.len-1];
        i32 dep = depth[depth.len-1];
        stack.len--;
        depth.len--;
        Slice<Dirent *> ents = {};
        for (Dirent *e = os_list(os, a, dir); e; e = e->next) {
            if (e->isdir && e->name.len && e->name[0]!='.') {
                ents = push(a, ents, e);
            }
        }
        sort(ents.data, ents.len, dirless, *a);  // the same skips each time
        Slice<Str> sub = {};
        for (iz k = 0; k < ents.len; k++) {
            Str d    = normpath(a, dir, ents[k]->name, win);
            Str file = normpath(a, d, "SKILL.md", win);
            if (os_stat(os, *a, file).type == STAT_FILE) {
                r = push(a, r, d);
            } else if (dep < SKILL_DEPTH) {
                sub = push(a, sub, d);
            }
        }
        for (iz k = sub.len-1; k >= 0; k--) {  // first listed first
            stack = push(a, stack, sub[k]);
            depth = push(a, depth, dep+1);
        }
    }
    return r;
}

// The skills of these directories, by precedence: the first of a name
// wins, the rest are dropped.
static Skills skillfind(Os *os, Arena *a, Slice<SkillDir> dirs, b32 win)
{
    Skills r    = {};
    b32    full = 0;
    for (iz i = 0; i<dirs.len && !full; i++) {
        SkillDir *d = &dirs[i];
        if (os_stat(os, *a, d->path).type != STAT_DIR ||
                (d->inside.len && !instrinside(os, *a, d->inside, d->path, win))) {
            continue;
        }
        Slice<Str> found = skillwalk(os, a, d->path, win);
        for (iz k = 0; k < found.len; k++) {
            Str  dir  = found[k];
            Str  path = normpath(a, dir, "SKILL.md", win);
            Stat st   = os_stat(os, *a, path);
            Str  why  = {};
            if (r.list.len >= SKILL_MAX) {
                r.skipped = push(a, r.skipped, concat(a, d->path,
                                 ": more than 200 skills, so the rest were left out"));
                full = 1;
                break;
            } else if (d->inside.len && !instrinside(os, *a, d->inside, path, win)) {
                why = "it leads out of the project";
            } else if (st.size > SKILL_BYTES) {
                why = "it is larger than 256 KiB";
            }
            Read      rd = {};
            SkillMeta m  = {};
            if (!why.data) {
                rd  = os_readat(os, a, path, 0, (iz)SKILL_HEAD);
                why = rd.err ? Str("it cannot be read") : Str{};
            }
            if (!why.data) {
                m   = skillparse(a, rd.data);
                why = m.err;
            }
            Str name = m.name.data ? m.name : basename(dir);
            if (!why.data && !skillname(name)) {
                why = "its directory's name must be letters, digits, - _ and . "
                      "(or its frontmatter must give a name)";
            } else if (!why.data && d->inside.len && name==skillbuiltin) {
                why = "prospero is Prospero's own skill, which only your own "
                      "skills may replace, not a project's";
            }
            if (why.data) {
                r.skipped = push(a, r.skipped, concat(a, concat(a, path, ": "), why));
                continue;
            } else if (skillbyname(r.list, name)) {
                continue;  // one before it wins
            }
            Skill s = {};
            s.name   = name;
            s.desc   = skilloneline(a, m.desc.data ? m.desc : skillpara(a, m.body));
            s.desc   = utf8head(s.desc, SKILL_DESC);
            s.dir    = dir;
            s.path   = path;
            s.hidden = m.hidden;
            r.list = push(a, r.list, s);
        }
    }
    sort(r.list.data, r.list.len, skillless, *a);
    return r;
}

// A skill's whole SKILL.md, parsed: its body, or why not.
static SkillMeta skillload(Os *os, Arena *a, Skill *k)
{
    SkillMeta r  = {};
    Read      rd = os_readat(os, a, k->path, 0, (iz)SKILL_BYTES+1);
    if (rd.err) {
        r.err = "its SKILL.md cannot be read";
    } else if (rd.data.len > SKILL_BYTES) {
        r.err = "its SKILL.md is larger than 256 KiB";
    } else {
        r = skillparse(a, rd.data);
    }
    return r;
}

// What the skill tool gives the agent: where the skill is, its
// instructions, and the arguments, if any.
static Str skilltext(Arena *a, Skill *k, Str body, Str args)
{
    Buf b(a, 256 + body.len + args.len);
    print(&b, "Base directory for this skill: ");
    print(&b, k->dir);
    print(&b, "\n\n");
    print(&b, trim(body));
    if (trim(args).len) {
        print(&b, "\n\nARGUMENTS: ");
        print(&b, trim(args));
    }
    return finish(&b);
}

// The text of a prompt that invokes a skill: its instructions in a
// skill block, which names its directory if it has one, then what the
// user typed after /name.
static Str skillprompt(Arena *a, Skill *k, Str body, Str args)
{
    Buf b(a, 256 + body.len + args.len);
    print(&b, "<skill name=\"");
    print(&b, k->name);
    print(&b, k->dir.len ? Str("\" dir=\"") : Str(""));
    for (iz i = 0; i < k->dir.len; i++) {
        if (k->dir[i] == '"') {
            print(&b, "&quot;");
        } else {
            putbyte(&b, k->dir[i]);
        }
    }
    print(&b, "\">\n");
    print(&b, trim(body));
    print(&b, "\n</skill>");
    if (trim(args).len) {
        print(&b, "\n\n");
        print(&b, trim(args));
    }
    return finish(&b);
}

struct SkillCall {
    Str name;
    Str args;  // empty if none
    b32 ok;
};

// The skill a prompt's text invokes (skillprompt), if it does.
static SkillCall skillcalled(Str text)
{
    SkillCall r    = {};
    Str       open = "<skill name=\"";
    if (!startswith(text, open)) {
        return r;
    }
    Cut name = cut(cuthead(text, open.len), '"');
    Cut tag  = cut(name.tail, '\n');
    if (!name.ok || !skillname(name.head) || !tag.ok || !endswith(tag.head, ">")) {
        return r;
    }
    iz end = find(tag.tail, "\n</skill>");
    if (end < 0) {
        return r;
    }
    Str rest = cuthead(tag.tail, end + 9);
    if (rest.len && !startswith(rest, "\n\n")) {
        return r;
    }
    r.name = name.head;
    r.args = trim(rest);
    r.ok   = 1;
    return r;
}


// Packages: a skill as a computer sends it to a thin engine, which
// relays its calls there, to copy for a session (relay.cpp, engine.cpp)
//
// JSON {"name", "files":[{"path", "text"|"data"}], "cut"?}: the files
// under the skill's directory, but not symlinks nor special files
// (os_list), nor dot-names, nor paths a copy may not have; sorted, with
// SKILL.md first; each as text if it is valid UTF-8 with no NUL, else
// as base64 data. Past its size, as encoded, SKILL.md alone, with cut.

static Str cfgsha256(Arena *, Str);

struct SkillFile {
    Str path;  // relative, '/'-separated
    Str data;
};

struct SkillPack {
    Slice<SkillFile> files;
    b32              cut;  // SKILL.md alone, the rest too large
    Str              err;  // why it is unsound, else null
};

// Whether a package's path is one a copy may have: relative, '/'-
// separated, at most SKILL_PACKDEPTH parts and SKILL_PATHLEN bytes, of
// valid UTF-8, no part empty or beginning with '.' (".", ".." and dot
// files), no '\\', ':' or control character; on Windows (win), no
// device's name in it, and no part ending in a dot or space, which
// Windows would drop.
static b32 skillpackpath(Str p, b32 win)
{
    if (!p.len || p.len>SKILL_PATHLEN) {
        return 0;
    }
    i32 parts = 0;
    for (Str rest = p;;) {
        Cut c    = cut(rest, '/');
        Str part = c.head;
        if (!part.len || part[0]=='.' || ++parts>SKILL_PACKDEPTH) {
            return 0;
        } else if (win && (part[part.len-1]=='.' || part[part.len-1]==' ')) {
            return 0;
        }
        for (iz i = 0; i < part.len;) {
            i32 ch = utf8strict(part, &i);
            if (ch<0x20 || ch==0x7f || ch=='\\' || ch==':' || (ch>=0xd800 && ch<=0xdfff)) {
                return 0;  // -1 too, invalid UTF-8
            }
        }
        if (!c.ok) {
            break;
        }
        rest = c.tail;
    }
    return !win || !devicepath(p, 1);
}

// Whether a file goes as text: valid UTF-8, with no NUL.
static b32 skilltextual(Str s)
{
    for (iz i = 0; i < s.len;) {
        i32 c = utf8strict(s, &i);
        if (c<=0 || (c>=0xd800 && c<=0xdfff)) {
            return 0;
        }
    }
    return 1;
}

static b32 skillfileless(SkillFile *a, SkillFile *b)
{
    b32 am = a->path=="SKILL.md";
    b32 bm = b->path=="SKILL.md";
    return am!=bm ? am : compare(a->path, b->path)<0;
}

// The paths of the files under a skill's directory that a package may
// have, with their sizes in data.len (no data), up to SKILL_FILES+1.
static Slice<SkillFile> skillpackfiles(Os *os, Arena *a, Str dir)
{
    Slice<SkillFile> r     = {};
    Slice<Str>       stack = {};  // relative folders to list
    stack = push(a, stack, Str{});
    while (stack.len && r.len<=SKILL_FILES) {
        Str rel = stack[stack.len-1];
        stack.len--;
        Str abs = rel.len ? concat(a, concat(a, dir, "/"), rel) : dir;
        for (Dirent *d = os_list(os, a, abs); d && r.len<=SKILL_FILES; d = d->next) {
            Str p = rel.len ? concat(a, concat(a, rel, "/"), d->name) : clone(a, d->name);
            if (!skillpackpath(p, 0)) {
                continue;  // dot-names too
            } else if (d->isdir) {
                stack = push(a, stack, p);
            } else {
                SkillFile f = {};
                f.path      = p;
                f.data.len  = (iz)d->size;
                r = push(a, r, f);
            }
        }
    }
    sort(r.data, r.len, skillfileless, *a);
    return r;
}

static void skillpackfile(JsonOut *w, Arena scratch, SkillFile *f)
{
    putobject(w);
    putstrkey(w, "path", f->path);
    if (skilltextual(f->data)) {
        putstrkey(w, "text", f->data);
    } else {
        putstrkey(w, "data", base64(&scratch, f->data));
    }
    putend(w);
}

// A skill's package, at most max bytes but for SKILL.md alone (cut).
static Str skillpack(Os *os, Arena *a, Skill *k, iz max)
{
    Arena            rd    = carve(a, max + (iz)SKILL_BYTES + (1<<16));
    Slice<SkillFile> files = skillpackfiles(os, &rd, k->dir);
    Buf              b(a, 1<<12);
    JsonOut          w = {&b};
    putobject(&w);
    putstrkey(&w, "name", k->name);
    putkey(&w, "files");
    putarray(&w);
    b32 cut = files.len>SKILL_FILES || !files.len || !(files[0].path=="SKILL.md");
    for (iz i = 0; !cut && i < files.len; i++) {
        Arena t  = rd;
        iz    sz = files[i].data.len;
        Read  r  = sz<=max ? os_read(os, &t, concat(&t, concat(&t, k->dir, "/"), files[i].path),
                                     max) : Read{};
        if (sz>max || r.err) {
            cut = sz > max;
            continue;  // too large, or gone or unreadable since listed
        }
        SkillFile f = {files[i].path, r.data};
        skillpackfile(&w, t, &f);
        cut = b.len > max;
    }
    if (cut) {
        Arena t = rd;
        Read  r = os_read(os, &t, k->path, SKILL_BYTES);
        b.len = 0;
        w     = {&b};
        putobject(&w);
        putstrkey(&w, "name", k->name);
        putkey(&w, "files");
        putarray(&w);
        SkillFile f = {"SKILL.md", r.err ? Str{} : r.data};
        skillpackfile(&w, t, &f);
        putend(&w);
        putintkey(&w, "cut", 1);
        putend(&w);
        return finish(&b);
    }
    putend(&w);
    putend(&w);
    return finish(&b);
}

// A package as a thin engine takes it, to copy (win, its own platform):
// every path one a copy may have (skillpackpath), none twice, even
// ignoring case, nor a file's also a folder's, at most SKILL_FILES, with
// SKILL.md among them; data as base64. Else why not, in err.
static SkillPack skillunpack(Arena *a, Json *pkg, b32 win)
{
    SkillPack r = {};
    r.cut = jsonint(jsonget(pkg, "cut"), 0) != 0;
    b32 md = 0;
    for (Json *x = jsonget(pkg, "files")->child; x; x = x->next) {
        SkillFile f    = {};
        Json     *text = jsonget(x, "text");
        f.path = jsonstr(jsonget(x, "path"));
        if (r.files.len == SKILL_FILES) {
            r.err = "it has too many files";
            return r;
        } else if (!skillpackpath(f.path, win)) {
            r.err = "it names a file that a copy may not have";
            return r;
        } else if (text->type == JSON_STRING) {
            f.data = jsonstr(text);
        } else {
            Unbase64 d = unbase64(a, jsonstr(jsonget(x, "data")));
            if (!d.ok) {
                r.err = "a file's data is not base64";
                return r;
            }
            f.data = d.data;
        }
        for (iz i = 0; i < r.files.len; i++) {
            Str  p = r.files[i].path;
            Str  q = f.path;
            iz   n = p.len<q.len ? p.len : q.len;
            b32  folder = equalfold(takehead(p, n), takehead(q, n)) &&
                          ((p.len>n && p[n]=='/') || (q.len>n && q[n]=='/'));
            if (equalfold(p, q) || folder) {
                r.err = "it names a file twice, or a file as a folder";
                return r;
            }
        }
        md |= f.path == "SKILL.md";
        r.files = push(a, r.files, f);
    }
    if (!md) {
        r.err = "it has no SKILL.md";
    }
    return r;
}

// A copy's name for a package's files, in order: the first 16 hex
// digits of the SHA-256 of each one's path, NUL, length, NUL, data.
static Str skillhash(Arena *a, Slice<SkillFile> files)
{
    iz n = 0;
    for (iz i = 0; i < files.len; i++) {
        n += files[i].path.len + files[i].data.len + 24;
    }
    Buf b(a, n);
    for (iz i = 0; i < files.len; i++) {
        print(&b, files[i].path);
        putbyte(&b, 0);
        print(&b, (i64)files[i].data.len);
        putbyte(&b, 0);
        print(&b, files[i].data);
    }
    return takehead(cfgsha256(a, finish(&b)), 16);
}

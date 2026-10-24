// Agent skills: SKILL.md files, as Claude Code and other harnesses keep them
//
// A skill is a directory holding SKILL.md: frontmatter between --- lines
// that names it and says when to use it, then instructions, which the
// agent reads when it uses the skill (the skill tool), or which the
// user's prompt carries when the user invokes it (/name). Skills sit one
// level down in skill directories, DIR/NAME/SKILL.md, which the harness
// looks through in this order: .claude/skills and .agents/skills in
// each directory from the working directory up to the repository root
// (instrroot), then [skills] paths, then the user's own: skills next to
// the configuration (~/.config/prospero/skills), ~/.claude/skills and
// ~/.agents/skills. A skill found earlier wins over a later one of the
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
// This is free and unencumbered software released into the public domain.

enum : i64 {
    SKILL_MAX   = 200,        // skills at most
    SKILL_DESC  = 300,        // bytes of a description
    SKILL_BYTES = 256 << 10,  // largest SKILL.md
    SKILL_HEAD  = 16 << 10,   // its frontmatter ends within this
    SKILL_NAME  = 64,         // bytes of a name
};

struct Skill {
    Str name;
    Str desc;    // one line, at most SKILL_DESC bytes
    Str dir;     // its directory, normalized
    Str path;    // its SKILL.md
    b32 hidden;  // disable-model-invocation: the user's alone
};

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
        Slice<Dirent *> ents = {};
        for (Dirent *e = os_list(os, a, d->path); e; e = e->next) {
            if (e->isdir && e->name.len && e->name[0]!='.') {
                ents = push(a, ents, e);
            }
        }
        sort(ents.data, ents.len, dirless, *a);  // the same skips each time
        for (iz k = 0; k < ents.len; k++) {
            Dirent *e    = ents[k];
            Str     dir  = normpath(a, d->path, e->name, win);
            Str     path = normpath(a, dir, "SKILL.md", win);
            Stat    st   = os_stat(os, *a, path);
            if (st.type != STAT_FILE) {
                continue;  // not a skill
            }
            Str why = {};
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
            Str name = m.name.data ? m.name : e->name;
            if (!why.data && !skillname(name)) {
                why = "its directory's name must be letters, digits, - _ and . "
                      "(or its frontmatter must give a name)";
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
// skill block, then what the user typed after /name.
static Str skillprompt(Arena *a, Skill *k, Str body, Str args)
{
    Buf b(a, 256 + body.len + args.len);
    print(&b, "<skill name=\"");
    print(&b, k->name);
    print(&b, "\" dir=\"");
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

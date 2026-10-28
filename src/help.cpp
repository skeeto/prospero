// Prospero's help: the Help pane's pages, Markdown built in from
// assets/help; the links that open them and other parts of the window;
// and the built-in prospero skill, through which the agent reads the
// same pages and the settings' schema
//
// Each page is a topic, whose first line is its heading, "# Title",
// which names it in the pane's list. Its name (helpnames) is what a link
// to it gives, as [Plan mode](help:plan), and what the layout file keeps
// for the page shown. The pane shows one at a time as a document
// (app.cpp, DocView). A link may name a section too, by its heading, as
// help:plan#approving: by its letters and digits, whatever their case.
//
// The window follows these links (helpdest) in the transcript, in Help
// and in the Plan pane. None changes anything but what the window shows:
//
//   help:NAME[#SECTION]          a Help page, at a section
//   settings:                    the Settings pane
//   settings:KIND[/NAME][#KEY]   its page for [KIND] or [KIND NAME], at a
//                                setting: KIND defaults for the keys
//                                above the first section, or a page's
//                                label, as interface for ui
//   settings:diagnostics         the problems found in the files
//   view:PANE                    a pane (viewnames)
//   http://... and https://...   the browser
//
// The built-in skill, assets/skills/prospero/SKILL.md, is offered as if
// it were the user's own (engine.cpp, sessskills), and its {{...}} are
// filled with what the engine serving it knows of itself. Its skill
// tool's args name topics: a page, "settings", or "settings KIND".
//
// This is free and unencumbered software released into the public domain.

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const help01[] = {
    #embed "../assets/help/01-start.md"
};
static u8 const help02[] = {
    #embed "../assets/help/02-sessions.md"
};
static u8 const help03[] = {
    #embed "../assets/help/03-permissions.md"
};
static u8 const help04[] = {
    #embed "../assets/help/04-plan.md"
};
static u8 const help05[] = {
    #embed "../assets/help/05-instructions.md"
};
static u8 const help06[] = {
    #embed "../assets/help/06-jobs.md"
};
static u8 const help07[] = {
    #embed "../assets/help/07-hosts.md"
};
static u8 const help08[] = {
    #embed "../assets/help/08-desktop.md"
};
static u8 const help09[] = {
    #embed "../assets/help/09-mcp.md"
};
static u8 const help10[] = {
    #embed "../assets/help/10-web.md"
};
static u8 const help11[] = {
    #embed "../assets/help/11-config.md"
};
static u8 const help12[] = {
    #embed "../assets/help/12-keys.md"
};
static u8 const help13[] = {
    #embed "../assets/help/13-trouble.md"
};
static u8 const helpskillbytes[] = {
    #embed "../assets/skills/prospero/SKILL.md"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

enum : i32 {
    HELP_START,
    HELP_SESSIONS,
    HELP_PERMISSIONS,
    HELP_PLAN,
    HELP_INSTRUCTIONS,
    HELP_JOBS,
    HELP_HOSTS,
    HELP_DESKTOP,
    HELP_MCP,
    HELP_WEB,
    HELP_CONFIG,
    HELP_KEYS,
    HELP_TROUBLE,
    HELP_COUNT,
};

static Str helpnames[] = {
    "start", "sessions", "permissions", "plan", "instructions", "jobs",
    "hosts", "desktop", "mcp", "web", "config", "keys", "trouble",
};
static_assert(countof(helpnames) == HELP_COUNT);

// Page i's Markdown, as its file has it.
static Str helptext(i32 i)
{
    switch (i) {
    case HELP_START:        return {(u8 *)help01, countof(help01)};
    case HELP_SESSIONS:     return {(u8 *)help02, countof(help02)};
    case HELP_PERMISSIONS:  return {(u8 *)help03, countof(help03)};
    case HELP_PLAN:         return {(u8 *)help04, countof(help04)};
    case HELP_INSTRUCTIONS: return {(u8 *)help05, countof(help05)};
    case HELP_JOBS:         return {(u8 *)help06, countof(help06)};
    case HELP_HOSTS:        return {(u8 *)help07, countof(help07)};
    case HELP_DESKTOP:      return {(u8 *)help08, countof(help08)};
    case HELP_MCP:          return {(u8 *)help09, countof(help09)};
    case HELP_WEB:          return {(u8 *)help10, countof(help10)};
    case HELP_CONFIG:       return {(u8 *)help11, countof(help11)};
    case HELP_KEYS:         return {(u8 *)help12, countof(help12)};
    case HELP_TROUBLE:      return {(u8 *)help13, countof(help13)};
    }
    return {};
}

// Page i's title, its heading without the "# ", or empty if its first
// line is no heading.
static Str helptitle(i32 i)
{
    Str line = cut(helptext(i), '\n').head;
    if (line.len && line[line.len-1]=='\r') {
        line = cuttail(line, 1);
    }
    return startswith(line, "# ") ? cuthead(line, 2) : Str{};
}

static Str helpname(i32 i)
{
    return helpnames[i];
}

// The page of a name, or -1 if none has it.
static i32 helpnamed(Str name)
{
    for (i32 i = 0; i < HELP_COUNT; i++) {
        if (name == helpname(i)) {
            return i;
        }
    }
    return -1;
}

// The page a link names, as help:NAME or help:NAME#SECTION, or -1 if it
// names none.
static i32 helplink(Str link)
{
    return startswith(link, "help:") ? helpnamed(cut(cuthead(link, 5), '#').head) : -1;
}

// Whether s begins with prefix, ignoring ASCII case.
static b32 helpstarts(Str s, Str prefix)
{
    return s.len>=prefix.len && equalfold(takehead(s, prefix.len), prefix);
}

// Whether two names are the same by their letters and digits alone,
// whatever their case: a heading and a link's #section
// ("settings-worth-knowing", "Settings worth knowing"), or a kind of
// section and its label ("mcp-servers", "MCP servers").
static b32 helpsame(Str a, Str b)
{
    iz i = 0;
    iz j = 0;
    for (;;) {
        for (; i<a.len && !alnum(a[i]); i++) {}
        for (; j<b.len && !alnum(b[j]); j++) {}
        if (i==a.len || j==b.len) {
            return i==a.len && j==b.len;
        } else if (lowercase(a[i++]) != lowercase(b[j++])) {
            return 0;
        }
    }
}

// A heading of a page, as its Markdown has it.
struct HelpHead {
    Str text;   // without its #s
    iz  off;    // its line's, in the page
    i32 level;  // 1 for #, 2 for ##, ...
};

// A page's headings, those outside fenced code.
static Slice<HelpHead> helpheads(Arena *a, Str md)
{
    Slice<HelpHead> r     = {};
    b32             fence = 0;
    for (Str rest = md; rest.len;) {
        iz  off  = md.len - rest.len;
        Cut c    = cut(rest, '\n');
        Str line = trimright(c.head);
        rest = c.tail;
        if (startswith(line, "```") || startswith(line, "~~~")) {
            fence = !fence;
            continue;
        }
        i32 level = 0;
        for (; level<line.len && line[level]=='#'; level++) {}
        if (!fence && level>=1 && level<=6 && level<line.len && line[level]==' ') {
            HelpHead h = {};
            h.text  = trim(cuthead(line, level));
            h.off   = off;
            h.level = level;
            r = push(a, r, h);
        }
    }
    return r;
}

// The heading of page's section that a link names after its #, or null
// if it names none.
static Str helpsection(Arena *a, i32 page, Str section)
{
    Slice<HelpHead> heads = helpheads(a, helptext(page));
    for (iz i = 0; section.len && i < heads.len; i++) {
        if (helpsame(heads[i].text, section)) {
            return heads[i].text;
        }
    }
    return {};
}

// The panes a view: link shows, in the order of app.cpp's PANE_*.
static Str viewnames[] = {
    "sessions", "transcript", "prompt", "todo", "plan", "changes", "jobs", "log",
    "hosts", "settings", "help", "metrics",
};

// %XX as the byte it stands for, the rest as it is.
static Str helpunescape(Arena *a, Str s)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        i32 hi = s[i]=='%' && i+2<s.len ? hexval(s[i+1]) : -1;
        i32 lo = hi>=0 ? hexval(s[i+2]) : -1;
        if (lo >= 0) {
            putbyte(&b, (u8)(hi<<4 | lo));
            i += 2;
        } else {
            putbyte(&b, s[i]);
        }
    }
    return finish(&b);
}

// The kind of section that a word of a settings: link names: the kind
// itself, or its page's label, alone or for its group ("ui",
// "interface", "defaults", "endpoints").
static CfgKind *helpkind(Arena *a, Str word)
{
    for (iz i = 0; i < countof(cfgkinds); i++) {
        CfgKind *k = &cfgkinds[i];
        if ((k->kind.len && helpsame(word, k->kind)) ||
                helpsame(word, cfgkindlabel(a, k->kind, 0)) ||
                helpsame(word, cfgkindlabel(a, k->kind, 1))) {
            return k;
        }
    }
    return 0;
}

enum : i32 {
    DEST_NONE,  // nothing Prospero opens
    DEST_HELP,
    DEST_SETTINGS,
    DEST_VIEW,
    DEST_WEB,
};

// Where a link goes in the window (helpdest).
struct Dest {
    i32 kind;
    i32 page;     // DEST_HELP: -1 if no page has the name
    Str section;  // DEST_HELP: the heading named after #, null for none
    Str cfgkind;  // DEST_SETTINGS: the section's kind, "" the top level
    Str cfgname;  // its name, null for none
    Str cfgkey;   // the setting, null for the page alone
    Str find;     // what to search for, if the setting is not there
    b32 known;    // cfgkind is a kind of section
    b32 named;    // a kind whose sections have names
    b32 diag;     // the Diagnostics page
    b32 whole;    // the pane as it is, for "settings:" alone
    i32 pane;     // DEST_VIEW: in viewnames, -1 if none
};

static Dest helpdest(Arena *a, Str link)
{
    Dest r = {};
    r.page = -1;
    r.pane = -1;
    if (helpstarts(link, "help:")) {
        Cut c = cut(cuthead(link, 5), '#');
        r.kind    = DEST_HELP;
        r.section = c.ok ? helpunescape(a, c.tail) : Str{};
        for (i32 i = 0; i < HELP_COUNT; i++) {
            r.page = equalfold(c.head, helpname(i)) ? i : r.page;
        }

    } else if (helpstarts(link, "settings:")) {
        Cut c    = cut(cuthead(link, 9), '#');
        Cut p    = cut(c.head, '/');
        Str word = helpunescape(a, p.head);
        r.kind    = DEST_SETTINGS;
        r.cfgname = p.ok ? helpunescape(a, p.tail) : Str{};
        r.cfgkey  = c.ok ? helpunescape(a, c.tail) : Str{};
        if (!c.head.len && !r.cfgkey.len) {
            r.whole = 1;
        } else if (word.len && helpsame(word, "diagnostics")) {
            r.diag = 1;
        } else if (!word.len) {
            r.known = 1;  // settings:#KEY, the top level's
        } else if (CfgKind *k = helpkind(a, word)) {
            r.cfgkind = k->kind;
            r.known   = 1;
            r.named   = k->named;
        } else {
            r.cfgkind = word;
        }
        r.find = r.cfgkey.len ? r.cfgkey : r.cfgname.len ? r.cfgname : word;

    } else if (helpstarts(link, "view:")) {
        Str name = cuthead(link, 5);
        r.kind = DEST_VIEW;
        for (i32 i = 0; i < countof(viewnames); i++) {
            r.pane = equalfold(name, viewnames[i]) ? i : r.pane;
        }

    } else if (helpstarts(link, "http://") || helpstarts(link, "https://")) {
        r.kind = DEST_WEB;
    }
    return r;
}


// The built-in skill

static Str helpskillmd()
{
    return {(u8 *)helpskillbytes, countof(helpskillbytes)};
}

// The built-in skill as skillfind lists one.
static Skill helpskill(Arena *a)
{
    SkillMeta m = skillparse(a, helpskillmd());
    Skill     k = {};
    k.name    = m.name;
    k.desc    = utf8head(skilloneline(a, m.desc), SKILL_DESC);
    k.builtin = 1;
    return k;
}

// What the skill tells the agent of the engine serving it.
struct HelpSelf {
    Str version;
    Str config;  // the user's file, through any symbolic link to it
    Str state;   // the state directory
    Str agents;  // the user's own instruction file
    Str skills;  // the user's own skills' directory
};

// The topics, as the skill's body lists them: each page by name, with
// its title and sections, then the settings.
static Str helptopics(Arena *a)
{
    Buf b(a, 2048);
    print(&b, "The topics:\n");
    for (i32 i = 0; i < HELP_COUNT; i++) {
        print(&b, "\n- `");
        print(&b, helpname(i));
        print(&b, "`: ");
        print(&b, helptitle(i));
        Slice<HelpHead> heads = helpheads(a, helptext(i));
        i32             n     = 0;
        for (iz k = 0; k < heads.len; k++) {
            if (heads[k].level > 1) {
                print(&b, n++ ? Str("; ") : Str(" ("));
                print(&b, heads[k].text);
            }
        }
        print(&b, n ? Str(")") : Str(""));
    }
    print(&b, "\n- `settings`: each section of prospero.ini, its page in "
              "the Settings pane, and its keys");
    print(&b, "\n- `settings KIND`: one section's keys in full, such as "
              "`settings ui`, `settings model` or `settings defaults` for "
              "the keys above the first section: each one's type, default, "
              "meaning and link");
    return finish(&b);
}

// The built-in skill's instructions, its {{...}} filled.
static Str helpskillbody(Arena *a, HelpSelf *self)
{
    Str body   = trim(skillparse(a, helpskillmd()).body);
    Str topics = helptopics(a);
    Buf b(a, body.len + topics.len + 1024);
    for (Str rest = body; rest.len;) {
        iz at = find(rest, "{{");
        iz to = at<0 ? -1 : find(cuthead(rest, at), "}}");
        if (to < 0) {
            print(&b, rest);
            break;
        }
        Str name = takehead(cuthead(rest, at+2), to-2);
        Str v    = name=="version" ? self->version
                 : name=="config"  ? self->config
                 : name=="state"   ? self->state
                 : name=="agents"  ? self->agents
                 : name=="skills"  ? self->skills
                 : name=="topics"  ? topics
                 : takehead(cuthead(rest, at), to+2);  // as written
        print(&b, takehead(rest, at));
        print(&b, v.len ? v : Str("(unknown)"));
        rest = cuthead(rest, at+to+2);
    }
    return finish(&b);
}

// A setting's type, in words.
static Str helptype(Arena *a, CfgKey *k)
{
    switch (k->type) {
    case CFG_STRING:   return "text";
    case CFG_INT:      return "a whole number";
    case CFG_NUMBER:   return "a number";
    case CFG_SECONDS:  return "seconds";
    case CFG_BOOL:     return "true or false";
    case CFG_ENUM:     return concat(a, "one of: ", k->choices);
    case CFG_URL:      return "an http:// or https:// URL";
    case CFG_SECRET:   return "env:NAME or file:PATH";
    case CFG_LIST:     return concat(a, "any of: ", k->choices);
    case CFG_PATHS:    return "paths separated by ;";
    case CFG_TIMES:    return "seconds separated by blanks";
    case CFG_TEMPLATE: return "a chat template's argument";
    }
    return "text";
}

// A kind of section as a header writes it: [kind], [kind NAME], or for
// the top level, what is above the first section.
static Str helpheader(Arena *a, CfgKind *k)
{
    if (!k->kind.len) {
        return "Above the first section";
    }
    Buf b(a, 32);
    print(&b, "`[");
    print(&b, k->kind);
    print(&b, k->named ? Str(" NAME]`") : Str("]`"));
    return finish(&b);
}

// A kind's page in the Settings pane, and its link: "Interface
// (settings:ui)", "Endpoints → NAME (settings:endpoint/NAME)".
static Str helppage(Arena *a, CfgKind *k)
{
    Buf b(a, 64);
    print(&b, "Settings → ");
    print(&b, cfgkindlabel(a, k->kind, k->named));
    print(&b, k->named ? Str(" → NAME (settings:") : Str(" (settings:"));
    print(&b, k->kind.len ? k->kind : Str("defaults"));
    print(&b, k->named ? Str("/NAME)") : Str(")"));
    return finish(&b);
}

// The settings topic: each kind of section with its page and its keys'
// names. With kind, that one kind's keys in full, and for a named kind,
// the sections c has of it.
static Str helpsettings(Arena *a, Config *c, CfgKind *one)
{
    Buf b(a, 4096);
    if (!one) {
        print(&b, "Each section of prospero.ini is a page in the Settings "
                  "pane. Read `settings KIND` for one section's keys in "
                  "full.\n");
    }
    for (iz i = 0; i < countof(cfgkinds); i++) {
        CfgKind *k = &cfgkinds[i];
        if (one && one!=k) {
            continue;
        }
        print(&b, one ? Str("") : Str("\n- "));
        print(&b, helpheader(a, k));
        print(&b, ", ");
        print(&b, helppage(a, k));
        print(&b, ": ");
        print(&b, k->desc);
        print(&b, one ? Str(".") : Str(". Keys:"));

        if (one && k->named && c) {
            i32 n = 0;
            for (IniSection *s = inifirst(c->ini, k->kind); s; s = ininext(s)) {
                if (s->name.len) {
                    print(&b, n++ ? Str(", `[") : Str(" This configuration has `["));
                    print(&b, k->kind);
                    print(&b, " ");
                    print(&b, s->name);
                    print(&b, "]`");
                }
            }
            print(&b, n ? Str(".") : Str(" This configuration has none."));
        }
        print(&b, one ? Str("\n") : Str(""));

        for (iz j = 0; j < countof(cfgschema); j++) {
            CfgKey *key = &cfgschema[j];
            if (key->kind != k->kind) {
                continue;
            }
            Str name = cfgfamily(key) ? concat(a, key->key, "NAME") : key->key;
            if (!one) {
                print(&b, " ");
                print(&b, name);
                continue;
            }
            print(&b, "\n- `");
            print(&b, name);
            print(&b, "`: ");
            print(&b, helptype(a, key));
            if (key->dflt.len) {
                print(&b, "; default ");
                print(&b, key->dflt);
            }
            if (key->local.data) {
                print(&b, ", or ");
                print(&b, key->local.len ? key->local : Str("none"));
                print(&b, " at a local endpoint");
            }
            print(&b, ". ");
            print(&b, key->desc);
            print(&b, ". (settings:");
            print(&b, k->kind.len ? k->kind : Str("defaults"));
            print(&b, k->named ? Str("/NAME#") : Str("#"));
            print(&b, name);
            print(&b, ")");
        }
    }
    if (!one) {
        print(&b, "\n- Diagnostics, Settings → Diagnostics "
                  "(settings:diagnostics): each problem found in the files, "
                  "with where it is.");
    }
    return finish(&b);
}

// The topics a skill call's args name, each page by its name or title,
// "settings", "settings KIND": words separated by blanks or commas, case
// ignored, and any quotes, brackets, help: or settings:, and #section
// too. The text is null if none matched; missed has the words that
// matched nothing.
struct HelpPick {
    Str text;
    Str missed;
};

static HelpPick helpargs(Arena *a, Config *c, Str args)
{
    Slice<Str> words = {};
    for (Str rest = args; rest.len;) {
        iz i = 0;
        for (; i<rest.len && !whitespace(rest[i]) && rest[i]!=','; i++) {}
        Str w = takehead(rest, i);
        rest  = cuthead(rest, i+(i<rest.len));
        for (; w.len && !alnum(w[0]); w = cuthead(w, 1)) {}
        for (; w.len && !alnum(w[w.len-1]); w = cuttail(w, 1)) {}
        w = helpstarts(w, "help:") ? cuthead(w, 5) : w;
        w = cut(w, '#').head;
        if (w.len) {
            words = push(a, words, w);
        }
    }

    HelpPick r    = {};
    Buf      text(a, 4096);
    Buf      missed(a, 64);
    u32      shown = 0;  // pages, then all settings, by bit
    for (iz i = 0; i < words.len; i++) {
        Str      w    = words[i];
        CfgKind *kind = 0;
        b32      all  = 0;
        i32      page = -1;
        if (helpstarts(w, "settings:")) {
            Str rest = cut(cuthead(w, 9), '/').head;
            kind = rest.len ? helpkind(a, rest) : 0;
            all  = !rest.len;
        } else if (helpsame(w, "settings")) {
            kind = i+1<words.len ? helpkind(a, words[i+1]) : 0;
            i   += kind != 0;
            all  = !kind;
        } else {
            for (i32 k = 0; k < HELP_COUNT; k++) {
                b32 hit = helpsame(w, helpname(k)) || helpsame(w, helptitle(k));
                page = hit ? k : page;
            }
        }

        Str part = {};
        if (page>=0 && !(shown & 1u<<page)) {
            shown |= 1u<<page;
            Buf p(a, 64);
            print(&p, "Help topic `");
            print(&p, helpname(page));
            print(&p, "` (help:");
            print(&p, helpname(page));
            print(&p, "):\n\n");
            print(&p, trim(helptext(page)));
            part = finish(&p);
        } else if (all && !(shown & 1u<<HELP_COUNT)) {
            shown |= 1u<<HELP_COUNT;
            part = helpsettings(a, c, 0);
        } else if (kind) {
            part = helpsettings(a, c, kind);
        } else if (page<0 && !all) {
            print(&missed, missed.len ? Str(", ") : Str(""));
            print(&missed, w);
        }
        if (part.len) {
            print(&text, text.len ? Str("\n\n---\n\n") : Str(""));
            print(&text, part);
        }
    }
    r.text   = text.len ? finish(&text) : Str{};
    r.missed = missed.len ? finish(&missed) : Str{};
    return r;
}

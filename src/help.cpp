// The Help pane's pages, Markdown built in from assets/help
//
// Each page is a topic, whose first line is its heading, "# Title",
// which names it in the pane's list. Its name (helpnames) is what a link
// to it gives, as [Plan mode](help:plan), and what the layout file keeps
// for the page shown. The pane shows one at a time as a document
// (app.cpp, DocView).
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
    #embed "../assets/help/10-config.md"
};
static u8 const help11[] = {
    #embed "../assets/help/11-keys.md"
};
static u8 const help12[] = {
    #embed "../assets/help/12-trouble.md"
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
    HELP_CONFIG,
    HELP_KEYS,
    HELP_TROUBLE,
    HELP_COUNT,
};

static char const *helpnames[] = {
    "start", "sessions", "permissions", "plan", "instructions", "jobs",
    "hosts", "desktop", "mcp", "config", "keys", "trouble",
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
    case HELP_CONFIG:       return {(u8 *)help10, countof(help10)};
    case HELP_KEYS:         return {(u8 *)help11, countof(help11)};
    case HELP_TROUBLE:      return {(u8 *)help12, countof(help12)};
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
    return {(u8 *)helpnames[i], (iz)__builtin_strlen(helpnames[i])};
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

// The page a link names, as help:NAME, or -1 if it names none.
static i32 helplink(Str link)
{
    return startswith(link, "help:") ? helpnamed(cuthead(link, 5)) : -1;
}

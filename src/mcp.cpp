// MCP: tools from Model Context Protocol servers
//
// An MCP server offers tools, which the agent calls as it calls the
// built-in ones. A server is a program run as a child, speaking JSON-RPC
// one message a line on its standard input and output (stdio), as a
// [mcp NAME] section's command names it:
//
//   [mcp stars]
//   command = uv run tools/starsmcp.py
//   cwd = ~/src/StarsVM
//   env.STARS_HOME = ~/games/stars
//   trust = no
//
// The engine starts each server a session offers, asks it for its tools
// (initialize, then tools/list), and offers them to the model as
// functions named mcp__SERVER__TOOL beside the built-ins: the model sees
// nothing of the transport. A call goes to the server as tools/call, and
// its content (text, images) becomes the call's result. Every MCP tool
// asks the user first, as a write outside the workspace does, unless the
// server's section says trust = yes or the session is in Bypass mode; an
// "always" answer covers every tool of that server for the session's
// tree, and a project's server may be trusted for good ("trust"), until
// its definition changes. Plan mode
// refuses a tool whose server does not say it only reads (readOnlyHint).
//
// Servers are shared by the sessions that offer them: one per name and
// working directory. The first request of a session waits for its
// servers to be ready (or failed, or past startup_timeout); its tools
// are fixed then, and logged with the prefix (EV_PREFIX, its "mcp"
// table), so that the session opened again offers the same. A server
// ready later adds its tools at the next request, which logs a new
// prefix; tools are never taken away mid-session.
//
// This file holds what needs no engine: names, a command's words,
// schemas, results, and the server's state. The engine's part is in
// engine.cpp ("MCP servers").
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    MCP_STOPPED,
    MCP_STARTING,  // spawned, initialize sent
    MCP_LISTING,   // initialized, tools/list under way
    MCP_READY,
    MCP_FAILED,
};

enum : i64 {
    MCP_NAMEMAX   = 64,                // longest tool name the APIs take
    MCP_FRAMEMAX  = (i64)16 << 20,     // longest message from a server
    MCP_PAGES     = 100,               // tools/list pages, at most
    MCP_INSTRMAX  = 4 << 10,           // bytes of a server's instructions
    MCP_ERRTAIL   = 2 << 10,           // bytes of its stderr in an error
    MCP_TEXTMAX   = 50 << 10,          // result text inline, the rest spilled
    MCP_DESCMAX   = 2 << 10,           // bytes of a tool's description
};

// What we send, and what we take from a server.
static Str const mcpversion = "2025-06-18";
static Str const mcpversions[] = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25",
};

// A tool as a session offers it: its function name, and where it goes.
struct McpTool {
    Str name;    // mcp__SERVER__TOOL, as the model calls it
    Str server;  // the server's name
    Str tool;    // the server's name for it
    Str desc;
    Str schema;  // parameters, as raw JSON
    b32 ro;      // readOnlyHint
};

enum : i32 {
    MCPREQ_INIT,
    MCPREQ_LIST,
    MCPREQ_CALL,
};

// An HTTP request to a server: a message POSTed, its response a JSON
// body or a stream of events, in a region of its own, released once it
// is done.
struct McpHttp {
    McpHttp *next;
    Region  *mem;
    Arena    arena;
    Http    *h;
    u64      hid;
    i64      rpc;      // the request it carries, 0 for any other message
    Str      body;     // what it sent, for a retry
    Str      method;
    i32      status;
    b32      sse;      // the response is a stream of events
    b32      retried;
    Str      challenge; // a 401's WWW-Authenticate
    Buf      in;       // a JSON response, or an error's start
    Sse      events;
};

// A request awaiting the server's response.
struct McpPending {
    i64 id;
    i32 kind;    // MCPREQ_*
    Str sid;     // a call's session and call id
    Str call;
};

struct McpServer {
    McpServer        *next;
    Str               name;
    Str               cwd;       // where it runs: its key, with name
    Slice<Str>        argv;
    Slice<Str>        env;       // NAME=value, added to the engine's
    b32               trust;
    Str               only;      // its section's tools, null for all
    i64               startms;   // startup_timeout
    i64               callms;    // timeout
    i32               state;     // MCP_*
    Str               err;       // why it failed
    Str               log;       // its stderr, a file
    Region           *mem;       // what follows lives here, made again
    Arena             arena;     // at each start
    Proc             *proc;
    u64               procid;
    Buf               in;        // what it sent, not yet a whole line
    i64               nextid;
    Slice<McpPending> pending;
    Slice<McpTool>    tools;
    Str               instr;     // its instructions, cut
    Str               version;   // the protocol it answered
    Str               cursor;    // tools/list's next page
    i32               pages;
    i64               startedat; // ticks
    i64               readyby;   // ticks: failed if not ready then
    i64               lastuse;   // ticks: a session offered it
    i64               retryat;   // ticks: started again no sooner
    i64               killat;    // ticks: once stopping, asked or killed then
    i32               killstage; // 0 before it is asked, 1 after
    b32               stopping;
    b32               config;    // still in the configuration
    Str               project;   // a project's: its .mcp.json, else null
    Str               raw;       // and its definition there
    Str               url;       // over HTTP: its endpoint, else null
    Str               headers;   // and its own headers, "Name: value\r\n"
    Str               session;   // Mcp-Session-Id, once it gave one
    McpHttp          *http;      // requests under way
    Str               bearer;    // signed in: its Authorization header
    b32               needauth;  // failed, until the user signs in
    b32               refreshed; // its tokens refreshed since it was ready
    Str               prm;       // a 401's resource_metadata, and scope
    Str               chscope;
};


// Names

// A name as the APIs take it: what is not a letter, a digit, _ or -
// becomes _.
static Str mcpident(Arena *a, Str s)
{
    Str r = clone(a, s);
    for (iz i = 0; i < r.len; i++) {
        u8 c = r[i];
        r[i] = alnum(c) || c=='_' || c=='-' ? c : (u8)'_';
    }
    return r;
}

// A tool's function name: mcp__SERVER__TOOL, or when that is too long,
// cut with a hash of the whole after it, so that it stays unique.
static Str mcptoolname(Arena *a, Str server, Str tool)
{
    Buf b(a, 16 + server.len + tool.len);
    print(&b, "mcp__");
    print(&b, mcpident(a, server));
    print(&b, "__");
    print(&b, mcpident(a, tool));
    Str name = finish(&b);
    if (name.len <= MCP_NAMEMAX) {
        return name;
    }
    Buf c(a, MCP_NAMEMAX);
    print(&c, takehead(name, MCP_NAMEMAX-9));
    putbyte(&c, '_');
    printhex(&c, hash(name), 8);
    return finish(&c);
}

// Whether a role's list of tools offers a server's tool: unless it names
// tools at all (null), only by "*", "mcp__SERVER" or "mcp__SERVER__TOOL",
// read left to right, a leading "-" taking it away again.
static b32 mcpallowed(Str list, Str server, Str fname)
{
    if (!list.data) {
        return 1;
    }
    b32 have = 0;
    for (Str rest = list; rest.len;) {
        iz i = 0;
        for (; i<rest.len && (whitespace(rest[i]) || rest[i]==','); i++) {}
        rest = cuthead(rest, i);
        for (i = 0; i<rest.len && !whitespace(rest[i]) && rest[i]!=','; i++) {}
        Str word = takehead(rest, i);
        rest     = cuthead(rest, i);
        b32 less = word.len>1 && word[0]=='-';
        Str w    = less ? cuthead(word, 1) : word;
        b32 hits = w=="*";
        if (startswith(w, "mcp__")) {
            // The tool itself, or its whole server
            hits = w==fname || (startswith(fname, w) &&
                                startswith(cuthead(fname, w.len), "__") &&
                                find(cuthead(w, 5), "__")<0);
        }
        have = hits ? !less : have;
    }
    return have;
}

// Whether a server's own tools key offers a tool, by the server's name
// for it: all when null.
static b32 mcponly(Str only, Str tool)
{
    if (!only.data) {
        return 1;
    }
    for (Str rest = only; rest.len;) {
        iz i = 0;
        for (; i<rest.len && (whitespace(rest[i]) || rest[i]==','); i++) {}
        rest = cuthead(rest, i);
        for (i = 0; i<rest.len && !whitespace(rest[i]) && rest[i]!=','; i++) {}
        if (takehead(rest, i) == tool) {
            return 1;
        }
        rest = cuthead(rest, i);
    }
    return 0;
}


// Commands

struct McpWords {
    Slice<Str> words;
    Str        err;   // null if it splits
};

// A command line as words, without a shell: blanks separate them,
// single quotes keep what they hold as it is, and double quotes too but
// for \" and \\.
static McpWords mcpwords(Arena *a, Str cmd)
{
    McpWords r = {};
    for (iz i = 0; i < cmd.len;) {
        for (; i<cmd.len && whitespace(cmd[i]); i++) {}
        if (i == cmd.len) {
            break;
        }
        Buf w(a, 64);
        while (i<cmd.len && !whitespace(cmd[i])) {
            u8 c = cmd[i++];
            if (c == '\'') {
                for (; i<cmd.len && cmd[i]!='\''; i++) {
                    putbyte(&w, cmd[i]);
                }
                if (i == cmd.len) {
                    r.err = "a single quote is not closed";
                    return r;
                }
                i++;
            } else if (c == '"') {
                for (; i<cmd.len && cmd[i]!='"'; i++) {
                    if (cmd[i]=='\\' && i+1<cmd.len && (cmd[i+1]=='"' || cmd[i+1]=='\\')) {
                        i++;
                    }
                    putbyte(&w, cmd[i]);
                }
                if (i == cmd.len) {
                    r.err = "a double quote is not closed";
                    return r;
                }
                i++;
            } else {
                putbyte(&w, c);
            }
        }
        r.words = push(a, r.words, finish(&w));
    }
    if (!r.words.len) {
        r.err = "the command is empty";
    }
    return r;
}

// Why a word cannot pass through cmd.exe as it is, which runs a .cmd or
// .bat file: its metacharacters, which no quoting makes safe. Null if
// it can.
static Str mcpcmdbad(Str w)
{
    for (iz i = 0; i < w.len; i++) {
        switch (w[i]) {
        case '&': case '|': case '<': case '>': case '^': case '%':
        case '"': case '\n': case '\r': case '!': case '(': case ')':
            return "an argument holds a character that cmd.exe, which runs "
                   ".cmd and .bat files, takes for its own: & | < > ^ % \" ! ( )";
        }
    }
    return {};
}


// Messages

// A request or notification as a line: params is raw JSON, or null for
// none; id 0 makes a notification.
static Str mcpmessage(Arena *a, i64 id, Str method, Str params)
{
    Buf     b(a, 128 + method.len + params.len);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "jsonrpc", "2.0");
    if (id) {
        putintkey(&w, "id", id);
    }
    putstrkey(&w, "method", method);
    if (params.data) {
        putkey(&w, "params");
        putraw(&w, params);
    }
    putend(&w);
    putbyte(&b, '\n');
    return finish(&b);
}

// The response to a server's request: its result as raw JSON, or else an
// error. id is raw JSON too, a number or a string, as it came.
static Str mcpresponse(Arena *a, Str id, Str result, i32 code, Str msg)
{
    Buf     b(a, 128 + id.len + result.len + msg.len);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "jsonrpc", "2.0");
    putkey(&w, "id");
    putraw(&w, id);
    if (result.data) {
        putkey(&w, "result");
        putraw(&w, result);
    } else {
        putkey(&w, "error");
        putobject(&w);
        putintkey(&w, "code", code);
        putstrkey(&w, "message", msg);
        putend(&w);
    }
    putend(&w);
    putbyte(&b, '\n');
    return finish(&b);
}

static Str mcpinitparams(Arena *a)
{
    Buf     b(a, 256);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "protocolVersion", mcpversion);
    putkey(&w, "capabilities");
    putobject(&w);
    putend(&w);
    putkey(&w, "clientInfo");
    putobject(&w);
    putstrkey(&w, "name", "prospero");
    putstrkey(&w, "version", PROSPERO_VERSION);
    putend(&w);
    putend(&w);
    return finish(&b);
}

static b32 mcpversionok(Str v)
{
    for (iz i = 0; i < countof(mcpversions); i++) {
        if (mcpversions[i] == v) {
            return 1;
        }
    }
    return 0;
}

// An error object as a message: its message, and its code.
static Str mcperror(Arena *a, Json *err)
{
    Str msg = jsonstr(jsonget(err, "message"));
    Buf b(a, 64 + msg.len);
    print(&b, msg.len ? msg : Str("an error"));
    print(&b, " (");
    print(&b, jsonint(jsonget(err, "code"), 0));
    putbyte(&b, ')');
    return finish(&b);
}


// Tools

// A tool's inputSchema as parameters the APIs take: an object, its
// $schema left out, with type and properties added where missing, as
// some servers leave them out and some APIs refuse that.
static Str mcpschema(Arena *a, Json *s)
{
    Buf     b(a, 256 + s->raw.len);
    JsonOut w = {&b};
    putobject(&w);
    b32 type = 0, props = 0;
    for (Json *m = s->type==JSON_OBJECT ? s->child : 0; m; m = m->next) {
        if (m->key == "$schema") {
            continue;
        } else if (m->key == "type") {
            putstrkey(&w, "type", "object");
            type = 1;
            continue;
        }
        props |= m->key=="properties" && m->type==JSON_OBJECT;
        if (m->key=="properties" && m->type!=JSON_OBJECT) {
            continue;
        }
        putkey(&w, m->key);
        putjson(&w, m);
    }
    if (!type) {
        putstrkey(&w, "type", "object");
    }
    if (!props) {
        putkey(&w, "properties");
        putobject(&w);
        putend(&w);
    }
    putend(&w);
    return finish(&b);
}

// A tool of a tools/list page, or one with no name if it has none.
static McpTool mcptool(Arena *a, Str server, Json *t)
{
    McpTool r = {};
    r.tool = clone(a, jsonstr(jsonget(t, "name")));
    if (!r.tool.len) {
        return r;
    }
    r.server = server;
    r.name   = mcptoolname(a, server, r.tool);
    Str desc = jsonstr(jsonget(t, "description"));
    Str title = jsonstr(jsonget(t, "title"));
    if (!desc.len) {
        desc = title;
    }
    r.desc   = clone(a, utf8head(trim(desc), MCP_DESCMAX));
    r.schema = mcpschema(a, jsonget(t, "inputSchema"));
    r.ro     = jsonbool(jsonget(jsonget(t, "annotations"), "readOnlyHint"), 0);
    return r;
}

// A tool as a function of a request's tools array, as tooljson writes
// them: its description says whose it is.
static void putmcptool(JsonOut *w, McpTool *t)
{
    putobject(w);
    putstrkey(w, "type", "function");
    putkey(w, "function");
    putobject(w);
    putstrkey(w, "name", t->name);
    Buf b(w->b->a, 64 + t->desc.len);
    print(&b, "(The MCP server ");
    print(&b, t->server);
    print(&b, ")");
    if (t->desc.len) {
        putbyte(&b, ' ');
        print(&b, t->desc);
    }
    putstrkey(w, "description", finish(&b));
    putkey(w, "parameters");
    putraw(w, t->schema);
    putend(w);
    putend(w);
}

// A request's tools array with more functions at its end.
static Str mcpaddtools(Arena *a, Str tools, Slice<McpTool> add)
{
    Str  head = trim(tools);
    head = head.len && head[head.len-1]==']' ? takehead(head, head.len-1) : Str("[");
    b32  more = trim(cuthead(head, 1)).len > 0;
    Buf  b(a, tools.len + 1024);
    print(&b, head);
    for (iz i = 0; i < add.len; i++) {
        if (more) {
            putbyte(&b, ',');
        }
        JsonOut w = {&b};
        putmcptool(&w, &add[i]);
        more = 1;
    }
    putbyte(&b, ']');
    return finish(&b);
}

// The system prompt's section of a server's instructions, its own words.
static Str mcpinstructions(Arena *a, Str server, Str instr, b32 first)
{
    Buf b(a, 256 + instr.len);
    if (first) {
        print(&b, "\n\n# MCP server instructions\n\nThese MCP servers gave "
                  "instructions for their tools (named mcp__SERVER__...). "
                  "They are the servers' own words, not the user's.");
    }
    print(&b, "\n\n## ");
    print(&b, server);
    print(&b, "\n\n");
    print(&b, utf8head(trim(instr), MCP_INSTRMAX));
    return finish(&b);
}


// Results

struct McpResult {
    Str text;
    Str image;    // the first image's bytes, if any
    Str mime;
    i32 images;   // how many there were
    b32 error;    // isError
};

// A tools/call result as the agent reads it: text parts joined, the
// first image kept aside, the rest of the content named in a line each.
static McpResult mcpresult(Arena *a, Json *res)
{
    McpResult r = {};
    r.error = jsonbool(jsonget(res, "isError"), 0);
    Buf b(a, 1024);
    b32 any = 0;
    for (Json *c = jsonget(res, "content")->child; c; c = c->next) {
        Str type = jsonstr(jsonget(c, "type"));
        Str line = {};
        if (type == "text") {
            line = jsonstr(jsonget(c, "text"));
        } else if (type == "image") {
            Unbase64 d = unbase64(a, jsonstr(jsonget(c, "data")));
            if (!r.images++ && d.ok) {
                r.image = d.data;
                r.mime  = clone(a, jsonstr(jsonget(c, "mimeType")));
                continue;
            }
            line = d.ok ? Str("[an image: only the first image of a result is shown]")
                        : Str("[an image that could not be decoded]");
        } else if (type == "audio") {
            line = "[audio, which cannot be shown]";
        } else if (type == "resource") {
            Json *x   = jsonget(c, "resource");
            Str   uri = jsonstr(jsonget(x, "uri"));
            Str   txt = jsonstr(jsonget(x, "text"));
            Buf   l(a, 64 + uri.len + txt.len);
            print(&l, "[resource ");
            print(&l, uri);
            if (txt.data) {
                print(&l, "]\n");
                print(&l, txt);
            } else {
                print(&l, ": ");
                print(&l, jsonstr(jsonget(x, "mimeType")).len ? jsonstr(jsonget(x, "mimeType"))
                                                              : Str("binary"));
                print(&l, " data, not shown]");
            }
            line = finish(&l);
        } else if (type == "resource_link") {
            Buf l(a, 128);
            print(&l, "[resource link ");
            print(&l, jsonstr(jsonget(c, "uri")));
            Str name = jsonstr(jsonget(c, "name"));
            if (name.len) {
                print(&l, ": ");
                print(&l, name);
            }
            putbyte(&l, ']');
            line = finish(&l);
        } else {
            line = concat(a, concat(a, "[content of type ", type), ", not shown]");
        }
        if (any) {
            putbyte(&b, '\n');
        }
        print(&b, line);
        any = 1;
    }
    Json *sc = jsonget(res, "structuredContent");
    if (!any && sc->type!=JSON_MISSING) {
        print(&b, sc->raw);
        any = 1;
    }
    r.text = finish(&b);
    return r;
}


// Project servers: .mcp.json
//
// As Claude Code reads it: {"mcpServers": {NAME: {"command", "args",
// "env"} or {"type": "http", "url", "headers"}}}, with ${VAR} and
// ${VAR:-default} expanded in each string from the environment that the
// engine gives its children (which leaves out the configuration's
// secrets). A stdio server runs in the file's folder.

struct McpDef {
    Str        name;
    Str        file;     // the .mcp.json
    Str        keyfile;  // its path for approvals: a worktree's, as in
                         // the main checkout (wtmainof)
    Str        dir;      // its folder
    Slice<Str> argv;     // stdio
    Slice<Str> env;      // NAME=value, for stdio
    Str        url;      // HTTP
    Slice<Str> headers;  // "Name: value", for HTTP
    Str        raw;      // the definition as written, for its approval
    Str        err;      // why it cannot be used, or null
};

static Str mcpenvget(Slice<Str> env, Str name, b32 win)
{
    for (iz i = 0; i < env.len; i++) {
        if (envnameis(env[i], name, win)) {
            return cuthead(env[i], name.len+1);
        }
    }
    return {};
}

// A string with ${VAR} and ${VAR:-default} expanded. A variable that is
// not set, with no default, makes missing its name.
static Str mcpexpand(Arena *a, Str s, Slice<Str> env, b32 win, Str *missing)
{
    if (find(s, "${") < 0) {
        return s;
    }
    Buf b(a, s.len + 64);
    for (iz i = 0; i < s.len;) {
        if (s[i]!='$' || i+1>=s.len || s[i+1]!='{') {
            putbyte(&b, s[i++]);
            continue;
        }
        Str rest = cuthead(s, i+2);
        iz  end  = find(rest, "}");
        if (end < 0) {
            print(&b, cuthead(s, i));
            break;
        }
        Str inner = takehead(rest, end);
        iz  dash  = find(inner, ":-");
        Str name  = dash<0 ? inner : takehead(inner, dash);
        Str value = mcpenvget(env, name, win);
        if (!value.len && dash>=0) {
            value = cuthead(inner, dash+2);
        } else if (!value.data && missing && !missing->len) {
            *missing = name;
        }
        print(&b, value);
        i += 2 + end + 1;
    }
    return finish(&b);
}

// A .mcp.json's servers, in its order. A file that cannot be read as
// one gives a single entry with no name and the error.
static Slice<McpDef> mcpprojects(Arena *a, Str file, Str dir, Str text, Slice<Str> env,
                                 b32 win)
{
    Slice<McpDef> r = {};
    JsonDoc d = jsonparse(a, text);
    Json   *servers = jsonget(d.root, "mcpServers");
    if (d.root->type!=JSON_OBJECT || servers->type!=JSON_OBJECT) {
        McpDef x = {};
        x.file = file;
        x.err  = "it is not a JSON object with mcpServers";
        return push(a, r, x);
    }
    for (Json *s = servers->child; s; s = s->next) {
        McpDef x = {};
        x.name = s->key;
        x.file = file;
        x.dir  = dir;
        x.raw  = s->raw;
        Str missing = {};
        Str type    = jsonstr(jsonget(s, "type"));
        Str url     = jsonstr(jsonget(s, "url"));
        Str command = jsonstr(jsonget(s, "command"));
        if (s->type != JSON_OBJECT) {
            x.err = "its definition is not an object";
        } else if (type=="sse") {
            x.err = "the SSE transport is not supported, only stdio and HTTP";
        } else if (type=="http" || type=="streamable-http" || (!type.len && url.len && !command.len)) {
            x.url = mcpexpand(a, url, env, win, &missing);
            x.err = x.url.len ? Str{} : Str("it has no url");
            for (Json *h = jsonget(s, "headers")->child; h; h = h->next) {
                Str v  = mcpexpand(a, jsonstr(h), env, win, &missing);
                x.headers = push(a, x.headers, concat(a, concat(a, h->key, ": "), v));
            }
        } else if (type.len && type!="stdio") {
            x.err = concat(a, "its type is not one this harness knows: ", type);
        } else if (!command.len) {
            x.err = "it has no command";
        } else {
            x.argv = push(a, x.argv, mcpexpand(a, command, env, win, &missing));
            for (Json *v = jsonget(s, "args")->child; v; v = v->next) {
                x.argv = push(a, x.argv, mcpexpand(a, jsonstr(v), env, win, &missing));
            }
            for (Json *v = jsonget(s, "env")->child; v; v = v->next) {
                Str val = mcpexpand(a, jsonstr(v), env, win, &missing);
                x.env   = push(a, x.env, concat(a, concat(a, v->key, "="), val));
            }
        }
        if (!x.err.len && missing.len) {
            x.err = concat(a, concat(a, "it uses ${", missing), "}, which is not set");
        }
        r = push(a, r, x);
    }
    return r;
}

// A project server as its ask shows it: its name, and its command line
// or URL.
static Str mcpshowdef(Arena *a, McpDef *x)
{
    Buf b(a, 128);
    print(&b, x->name);
    print(&b, ": ");
    if (x->url.len) {
        print(&b, x->url);
    }
    for (iz i = 0; i < x->argv.len; i++) {
        Str w   = x->argv[i];
        b32 odd = !w.len || find(w, " ")>=0 || find(w, "\"")>=0;
        print(&b, i ? Str(" ") : Str(""));
        print(&b, odd ? Str("\"") : Str(""));
        print(&b, w);
        print(&b, odd ? Str("\"") : Str(""));
    }
    return finish(&b);
}


// Signing in (OAuth 2.1, as MCP's authorization asks for it)
//
// A server over HTTP that answers 401 wants a token. Its protected
// resource metadata (RFC 9728) names its authorization server, whose
// metadata (RFC 8414, or OpenID's) gives the endpoints; a client
// registers itself there (RFC 7591), unless its section names one, and
// the user signs in through the browser with PKCE (S256), the redirect
// coming back to a listener on this machine's loopback. The resource
// parameter (RFC 8707) names the server's URL throughout.

enum : i32 {
    MCPAUTH_PRM,      // the protected resource's metadata
    MCPAUTH_ASM,      // the authorization server's
    MCPAUTH_REG,      // registering the client
    MCPAUTH_WAIT,     // the user signs in, in the browser
    MCPAUTH_TOKEN,    // the code exchanged for tokens
    MCPAUTH_REFRESH,  // a refresh token exchanged for new ones
};

// A sign-in under way, or a refresh: in a region of its own.
struct McpAuth {
    McpAuth   *next;
    Region    *mem;
    Arena      arena;
    Str        name;      // the server's
    Str        resource;  // and its URL
    i32        stage;     // MCPAUTH_*
    Slice<Str> tries;     // metadata URLs to try, in order
    i32        at;
    Str        redirect;
    Str        issuer;
    Str        authz;     // endpoints
    Str        tokenurl;
    Str        reg;
    Str        scope;
    Str        client;
    Str        clientname;  // registering
    Str        secret;
    Str        refresh;   // the refresh token, refreshing
    Str        verifier;  // PKCE
    Str        state;
    i64        cmd;       // the command that waits for it, 0 if none
    i64        by;        // ticks: given up then
    Http      *h;
    u64        hid;
    i32        status;
    Buf        body;
};

static Str base64url(Arena *a, Str s)
{
    Str r = base64(a, s);
    while (r.len && r[r.len-1]=='=') {
        r.len--;
    }
    for (iz i = 0; i < r.len; i++) {
        r[i] = r[i]=='+' ? (u8)'-' : r[i]=='/' ? (u8)'_' : r[i];
    }
    return r;
}

static i32 hexval(u8 c)  // -1 if not one
{
    return c>='0' && c<='9' ? c-'0'
         : c>='a' && c<='f' ? c-'a'+10
         : c>='A' && c<='F' ? c-'A'+10
         : -1;
}

// SHA-256 of s, its 32 bytes.
static Str mcpsha256(Arena *a, Str s)
{
    Str hex = cfgsha256(a, s);
    Str r   = {alloc<u8>(a, 32), 32};
    for (iz i = 0; i < 32; i++) {
        r[i] = (u8)(hexval(hex[2*i])<<4 | hexval(hex[2*i+1]));
    }
    return r;
}

// PKCE's S256 challenge of a verifier.
static Str mcpchallenge(Arena *a, Str verifier)
{
    return base64url(a, mcpsha256(a, verifier));
}

// Percent-encoded, as a query's or a form's value: all but the
// unreserved characters.
static Str pctencode(Arena *a, Str s)
{
    Buf b(a, s.len + 16);
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (alnum(c) || c=='-' || c=='.' || c=='_' || c=='~') {
            putbyte(&b, c);
        } else {
            putbyte(&b, '%');
            putbyte(&b, "0123456789ABCDEF"[c>>4]);
            putbyte(&b, "0123456789ABCDEF"[c&15]);
        }
    }
    return finish(&b);
}

static Str pctdecode(Arena *a, Str s)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        i32 hi = c=='%' && i+2<s.len ? hexval(s[i+1]) : -1;
        i32 lo = hi>=0 ? hexval(s[i+2]) : -1;
        if (lo >= 0) {
            putbyte(&b, (u8)(hi<<4 | lo));
            i += 2;
        } else {
            putbyte(&b, c=='+' ? (u8)' ' : c);
        }
    }
    return finish(&b);
}

// A value of a query (or a form), decoded: null if absent.
static Str mcpquery(Arena *a, Str query, Str key)
{
    if (startswith(query, "?")) {
        query = cuthead(query, 1);
    }
    for (Str rest = query; rest.len;) {
        Cut c  = cut(rest, '&');
        rest   = c.tail;
        Cut kv = cut(c.head, '=');
        if (pctdecode(a, kv.head) == key) {
            Str v = pctdecode(a, kv.tail);
            return v.data ? v : Str("");
        }
    }
    return {};
}

// A parameter of a WWW-Authenticate challenge (Bearer key="value", or
// key=token), or null.
static Str mcpchallengeparam(Arena *a, Str header, Str key)
{
    for (iz i = 0; i < header.len; i++) {
        b32 start = !i || header[i-1]==' ' || header[i-1]==',' || header[i-1]=='\t';
        if (!start || header.len-i<key.len+1 ||
                !equalfold(Str{header.data+i, key.len}, key) || header[i+key.len]!='=') {
            continue;
        }
        iz j = i + key.len + 1;
        Buf b(a, 64);
        if (j<header.len && header[j]=='"') {
            for (j++; j<header.len && header[j]!='"'; j++) {
                if (header[j]=='\\' && j+1<header.len) {
                    j++;
                }
                putbyte(&b, header[j]);
            }
        } else {
            for (; j<header.len && header[j]!=',' && !whitespace(header[j]); j++) {
                putbyte(&b, header[j]);
            }
        }
        return finish(&b);
    }
    return {};
}

// A URL's scheme and authority (https://host:port), and its path.
static Str urlorigin(Str url)
{
    iz s = find(url, "://");
    if (s < 0) {
        return {};
    }
    iz p = s + 3;
    for (; p<url.len && url[p]!='/' && url[p]!='?' && url[p]!='#'; p++) {}
    return takehead(url, p);
}

static Str urlpath(Str url)
{
    Str o = urlorigin(url);
    Str p = cuthead(url, o.len);
    iz  q = 0;
    for (; q<p.len && p[q]!='?' && p[q]!='#'; q++) {}
    p = takehead(p, q);
    while (p.len && p[p.len-1]=='/') {
        p.len--;
    }
    return p;
}

// Where a server's protected resource metadata may be: where its 401
// said, else at the well-known path under its path, then the root's.
static Slice<Str> mcpprmurls(Arena *a, Str url, Str said)
{
    Slice<Str> r = {};
    if (said.len) {
        return push(a, r, said);
    }
    Str o = urlorigin(url), p = urlpath(url);
    if (p.len) {
        r = push(a, r, concat(a, concat(a, o, "/.well-known/oauth-protected-resource"), p));
    }
    return push(a, r, concat(a, o, "/.well-known/oauth-protected-resource"));
}

// Where an authorization server's metadata may be, as RFC 8414 and
// OpenID put it for an issuer with or without a path.
static Slice<Str> mcpasmurls(Arena *a, Str issuer)
{
    Slice<Str> r = {};
    Str o = urlorigin(issuer), p = urlpath(issuer);
    if (p.len) {
        r = push(a, r, concat(a, concat(a, o, "/.well-known/oauth-authorization-server"), p));
        r = push(a, r, concat(a, concat(a, o, "/.well-known/openid-configuration"), p));
        r = push(a, r, concat(a, concat(a, o, p), "/.well-known/openid-configuration"));
    } else {
        r = push(a, r, concat(a, o, "/.well-known/oauth-authorization-server"));
        r = push(a, r, concat(a, o, "/.well-known/openid-configuration"));
    }
    return r;
}

// The authorization request's URL, for the browser.
static Str mcpauthurl(Arena *a, McpAuth *x, Str challenge)
{
    Buf b(a, 512);
    print(&b, x->authz);
    putbyte(&b, find(x->authz, "?")<0 ? '?' : '&');
    print(&b, "response_type=code&client_id=");
    print(&b, pctencode(a, x->client));
    print(&b, "&redirect_uri=");
    print(&b, pctencode(a, x->redirect));
    print(&b, "&code_challenge=");
    print(&b, challenge);
    print(&b, "&code_challenge_method=S256&state=");
    print(&b, pctencode(a, x->state));
    print(&b, "&resource=");
    print(&b, pctencode(a, x->resource));
    if (x->scope.len) {
        print(&b, "&scope=");
        print(&b, pctencode(a, x->scope));
    }
    return finish(&b);
}

// The loopback listener's (os_loopback): the target of a request for
// its path, from its head, else null; and what it answers.
static Str loopbacktarget(Str head, Str path)
{
    Cut line = cut(head, '\n');
    Cut verb = cut(trim(line.head), ' ');
    Cut tgt  = cut(verb.tail, ' ');
    b32 ok   = verb.head=="GET" && cut(tgt.head, '?').head==path;
    return ok ? tgt.head : Str{};
}

static Str const loopbackpage =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html><title>Prospero</title>"
    "<p style=\"font:16px sans-serif;margin:3em\">Prospero has the sign-in. "
    "You may close this tab.</p>\n";

static Str const loopbackmissing =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

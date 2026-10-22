// Providers: URLs, the canonical transcript, OpenAI Chat Completions,
// error classification, and llama.cpp router discovery
//
// A tool result may carry a screenshot (the desktop tool's), which goes
// back to the model as an image: Chat Completions takes images only in
// user messages, so a run of tool results is followed by one with their
// screenshots, and Anthropic's tool results hold them as image blocks.
// A screenshot left out has a short text in its place, so that the
// request keeps its shape.
//
// The transcript is append-only: an assistant message keeps the blocks
// the model produced (reasoning with any signature, text, tool calls
// with their raw argument strings), and requests are encoded from it
// deterministically, so that each request's prefix is byte-identical to
// the previous one and the server's prompt cache keeps hitting.
// Anthropic's Messages API, the other request format, is anthropic.cpp.
//
// Everything here is pure: the engine does the I/O.
//
// This is free and unencumbered software released into the public domain.


// URLs: scheme://host[:port][/path], as much as HTTP clients need

struct Url {
    Str scheme;  // lowercase
    Str host;    // brackets kept for IPv6
    Str path;    // "/" at least, with any query
    i32 port;
    b32 ok;
};

static Url parseurl(Str s)
{
    Url r   = {};
    iz  sep = find(s, "://");
    if (sep <= 0) {
        return r;
    }
    Str scheme = takehead(s, sep);
    if (equalfold(scheme, "http")) {
        r.scheme = "http";
        r.port   = 80;
    } else if (equalfold(scheme, "https")) {
        r.scheme = "https";
        r.port   = 443;
    } else {
        return r;
    }

    Str rest = cuthead(s, sep+3);
    iz  end  = 0;
    for (; end<rest.len && rest[end]!='/' && rest[end]!='?'; end++) {}
    Str auth = takehead(rest, end);
    r.path   = cuthead(rest, end);
    if (!r.path.len) {
        r.path = "/";
    } else if (r.path[0] == '?') {
        return r;  // would need a "/" inserted
    }

    // host, [v6], host:port, or [v6]:port
    iz colon = -1;
    if (auth.len && auth[0]=='[') {
        Cut c = cut(auth, ']');
        if (!c.ok) {
            return r;
        }
        r.host = takehead(auth, c.head.len+1);
        if (c.tail.len) {
            if (c.tail[0] != ':') {
                return r;
            }
            colon = r.host.len;
        }
    } else {
        Cut c = cut(auth, ':');
        r.host = c.head;
        colon  = c.ok ? c.head.len : -1;
    }
    if (!r.host.len || cut(auth, '@').ok) {
        return r;  // no credentials in URLs
    }
    if (colon >= 0) {
        Int port = parseint(cuthead(auth, colon+1));
        if (!port.ok || port.val<1 || port.val>65535) {
            return r;
        }
        r.port = (i32)port.val;
    }
    for (iz i = 0; i < r.host.len; i++) {
        u8 c = r.host[i];
        if (c<=' ' || c==0x7f || c=='/' || c=='\\') {
            return r;
        }
    }

    r.ok = 1;
    return r;
}

// The origin with the port always explicit: scheme://host:port.
static Str urlorigin(Arena *a, Url u)
{
    Buf b(a, 32 + u.host.len);
    print(&b, u.scheme);
    print(&b, "://");
    print(&b, u.host);
    putbyte(&b, ':');
    print(&b, (i64)u.port);
    return finish(&b);
}

static b32 ipv4(Str host, u8 out[4])
{
    i32 part = 0;
    for (Str rest = host;;) {
        Cut c = cut(rest, '.');
        Int v = parseint(c.head);
        if (!v.ok || v.val<0 || v.val>255 || !c.head.len || c.head[0]=='-' ||
                part==4) {
            return 0;
        }
        out[part++] = (u8)v.val;
        if (!c.ok) break;
        rest = c.tail;
    }
    return part == 4;
}

// Loopback, link-local, private (RFC 1918), and .local hosts: generous
// first-byte deadlines, and never through a proxy.
static b32 localhost(Str host)
{
    u8 ip[4];
    if (equalfold(host, "localhost") || equalfold(host, "[::1]") ||
            (host.len>6 && equalfold(taketail(host, 6), ".local")) ||
            (host.len>10 && equalfold(taketail(host, 10), ".localhost"))) {
        return 1;
    } else if (ipv4(host, ip)) {
        return ip[0]==127 || ip[0]==10 ||
               (ip[0]==172 && ip[1]>=16 && ip[1]<32) ||
               (ip[0]==192 && ip[1]==168) ||
               (ip[0]==169 && ip[1]==254);
    }
    return 0;
}

// The API root of an endpoint URL: given "http://h/" or "http://h/v1",
// returns the URL of name under /v1/, e.g. "http://h/v1/models".
static Str apiurl(Arena *a, Str base, Str name)
{
    for (; base.len && base[base.len-1]=='/'; base.len--) {}
    if (endswith(base, "/v1")) {
        base.len -= 3;
    }
    Buf b(a, base.len + name.len + 8);
    print(&b, base);
    print(&b, "/v1/");
    print(&b, name);
    return finish(&b);
}

// The URL of name at the server's root, beside /v1/: given "http://h/"
// or "http://h/v1", "http://h/props".
static Str rooturl(Arena *a, Str base, Str name)
{
    for (; base.len && base[base.len-1]=='/'; base.len--) {}
    if (endswith(base, "/v1")) {
        base.len -= 3;
    }
    Buf b(a, base.len + name.len + 8);
    print(&b, base);
    putbyte(&b, '/');
    print(&b, name);
    return finish(&b);
}


// The canonical transcript

enum : i32 {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL,
};

enum : i32 {
    BLOCK_THINK,
    BLOCK_TEXT,
    BLOCK_CALL,
    BLOCK_REDACTED,  // reasoning the server keeps opaque (Anthropic)
};

struct Block {
    i32 type;
    Str text;  // think and text blocks; a redacted block's data
    Str sig;   // think blocks: the server's signature, empty if none
    Str id;    // call blocks
    Str name;
    Str args;  // the raw argument string, as the model produced it
};

struct Msg {
    i32          role;
    Str          text;    // user and tool messages
    Slice<Block> blocks;  // assistant messages
    Str          callid;  // tool messages
    Str          spill;   // tool messages: the full output, relative to
                          // the session's directory, if one was kept
    b32          error;   // tool messages: the call failed or was refused
    i64          seq;     // of the record it came from, 0 if none
    Str          image;   // tool messages: a screenshot (PNG), relative
    i32          imgw;    // to the session's directory, and its size
    i32          imgh;
    Str          imgb64;  // in a request's history: the screenshot as
                          // base64, or null where it is left out,
    Str          imgalt;  // with this text in its place
    i32          floor;   // FLOOR_*: a cache breakpoint just before it
};

// Where a request's prefix stops matching the one before, at a step of
// screenshots dropped (deskdropped), so that Anthropic's cache can be
// read up to there and written up to where the next step changes it.
enum : i32 {
    FLOOR_NONE,
    FLOOR_WRITE,  // the oldest screenshot shown: the next step's change
    FLOOR_READ,   // the oldest dropped at this step: its change
};

struct Usage {
    i64 in;       // prompt tokens, cached ones included
    i64 out;      // completion tokens
    i64 cached;   // prompt tokens served from the cache
    i64 written;  // prompt tokens written to the cache (Anthropic)
};

static Str blockname(i32 type)
{
    switch (type) {
    case BLOCK_THINK:    return "think";
    case BLOCK_TEXT:     return "text";
    case BLOCK_REDACTED: return "redacted";
    }
    return "call";
}

static i32 blocktype(Str name)
{
    return name=="think"    ? BLOCK_THINK :
           name=="text"     ? BLOCK_TEXT  :
           name=="redacted" ? BLOCK_REDACTED : BLOCK_CALL;
}

// Whether a tool result's status (tool.end) tells the model it failed.
static b32 toolfailed(Str status)
{
    return status!="ok" && status!="background";
}


// Arguments: repairing what the model wrote

// Escape raw control characters inside strings, which some models emit
// as literal newlines and tabs.
static Str escapecontrols(Arena *a, Str s)
{
    Buf b(a, s.len + 16);
    b32 instr = 0;
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (instr && c=='\\' && i+1<s.len) {
            putbyte(&b, c);
            putbyte(&b, s[++i]);
            continue;
        } else if (c == '"') {
            instr = !instr;
        } else if (instr && c<0x20) {
            switch (c) {
            case '\n': print(&b, "\\n"); continue;
            case '\r': print(&b, "\\r"); continue;
            case '\t': print(&b, "\\t"); continue;
            }
            print(&b, "\\u00");
            printhex(&b, c, 2);
            continue;
        }
        putbyte(&b, c);
    }
    return finish(&b);
}

struct Repair {
    Json *root;  // an object, or JSON_MISSING
    Str   how;   // what was repaired, empty if nothing
};

// Parse tool arguments, repairing the syntax only where the intent is
// unambiguous: empty means {}, a string holding an object is unwrapped,
// raw control characters are escaped, and one missing or extra closing
// bracket at the end is fixed. Strings are never closed: a truncated
// value must not be acted on.
static Repair repairargs(Arena *a, Str raw)
{
    Repair r = {};
    r.root = (Json *)&jsonmissing;
    Str s  = trim(raw);
    if (!s.len) {
        r.root = jsonparse(a, "{}").root;
        r.how  = "empty arguments taken as {}";
        return r;
    }

    JsonDoc d = jsonparse(a, s);
    if (d.ok && d.root->type==JSON_OBJECT) {
        r.root = d.root;
        return r;
    }
    if (d.ok && d.root->type==JSON_STRING) {
        JsonDoc in = jsonparse(a, trim(d.root->str));
        if (in.ok && in.root->type==JSON_OBJECT) {
            r.root = in.root;
            r.how  = "arguments were a JSON string holding the object";
        }
        return r;
    }

    Str esc = escapecontrols(a, s);
    d = jsonparse(a, esc);
    if (d.ok && d.root->type==JSON_OBJECT) {
        r.root = d.root;
        r.how  = "raw control characters escaped";
        return r;
    }

    Str body = esc.len && esc[esc.len-1]=='}' ? cuttail(esc, 1) : esc;
    Str tries[] = {
        concat(a, esc, "}"),
        concat(a, esc, "]}"),
        concat(a, esc, "}}"),
        concat(a, body, "]}"),
        cuttail(esc, 1),
    };
    for (iz i = 0; i < countof(tries); i++) {
        d = jsonparse(a, tries[i]);
        if (d.ok && d.root->type==JSON_OBJECT) {
            r.root = d.root;
            r.how  = "a bracket at the end was fixed";
            return r;
        }
    }
    return r;
}

// The arguments as sent back in later requests: the raw string when it
// is a valid object, keeping the bytes the model produced, else the
// repaired object, else {}. Servers render arguments as objects, and
// llama.cpp rejects the whole request if one fails to parse.
static Str wireargs(Arena *a, Str raw)
{
    Arena   tmp = *a;  // dead before anything else is allocated in a
    JsonDoc d   = jsonparse(&tmp, raw);
    if (d.ok && d.root->type==JSON_OBJECT) {
        return raw;
    }
    Repair r = repairargs(a, raw);
    if (r.root->type != JSON_OBJECT) {
        return "{}";
    }
    Buf     b(a, raw.len + 16);
    JsonOut w = {&b};
    putjson(&w, r.root);
    return finish(&b);
}


// OpenAI Chat Completions requests

// A sampling parameter or other extra, with its value as JSON text.
struct Extra {
    Str key;
    Str json;
};

// The request format an endpoint takes ([endpoint] kind).
enum : i32 {
    EP_OPENAI,     // Chat Completions
    EP_ANTHROPIC,  // Messages (anthropic.cpp)
};

// How an Anthropic model is asked to think ([model] thinking).
enum : i32 {
    THINK_ADAPTIVE,  // the model decides how much
    THINK_BUDGET,    // a fixed budget of tokens, for older models
    THINK_OFF,       // sent as disabled
    THINK_BETWEEN,   // between_tools: Sonnet 5.5's off
    THINK_DEFAULT,   // not sent: the model's own default
};

struct ModelConf {
    Str          name;       // as sent in requests
    Str          reasoning;  // field to send reasoning back in, empty: never
    Slice<Extra> extras;     // sampling parameters, only if configured
    b32          llamacpp;   // llama.cpp extras (return_progress,
                             // reasoning_control)
    i64          ctx;        // context limit in tokens, 0 if unknown
    i64          thinkmax;   // reasoning budget in tokens, 0 for none
    i32          edit;       // the edit tool's format, EDIT_* (edit.cpp)
    // Anthropic
    i64          maxtokens;  // [model] max_tokens, 0 if unset
    i64          maxout;     // the server's limit on output, 0 if unknown
    i32          think;      // THINK_*
    b32          hidethink;  // thinking_display omitted, else summarized
    Str          effort;     // output_config.effort, empty: not sent
    // The desktop tool's screenshots (desktop.cpp)
    i32          coords;     // COORDS_*: how the model gives points
    b32          novision;   // the model takes no images
    i64          imgedge;    // [model] image_edge, 0 if unset
    i64          imgpixels;  // [model] image_pixels, 0 if unset
};

struct ChatReq {
    ModelConf *mc;
    Str        system;
    Str        tools;    // the tools array, already encoded
    Slice<Msg> history;
    i64        thinkfrom;  // reasoning of messages older than this seq
                           // is not sent back (Anthropic: compaction
                           // changed what it was bound to)
};

static void encodemsg(JsonOut *w, ModelConf *mc, Msg *m, Arena *a)
{
    putobject(w);
    switch (m->role) {
    case ROLE_USER:
        putkey(w, "role");
        putstr(w, "user");
        putkey(w, "content");
        putstr(w, m->text);
        break;

    case ROLE_TOOL:
        putkey(w, "role");
        putstr(w, "tool");
        putkey(w, "tool_call_id");
        putstr(w, m->callid);
        putkey(w, "content");
        putstr(w, m->text);
        break;

    case ROLE_ASSISTANT: {
        Buf  think(a, 256);
        Buf  text(a, 256);
        iz   ncalls = 0;
        for (iz i = 0; i < m->blocks.len; i++) {
            Block *b = &m->blocks[i];
            switch (b->type) {
            case BLOCK_THINK: print(&think, b->text); break;
            case BLOCK_TEXT:  print(&text, b->text);  break;
            case BLOCK_CALL:  ncalls++;               break;
            }
        }
        putkey(w, "role");
        putstr(w, "assistant");
        putkey(w, "content");
        putstr(w, finish(&text));
        if (think.len && mc->reasoning.len) {
            putkey(w, mc->reasoning);
            putstr(w, finish(&think));
        }
        if (ncalls) {
            putkey(w, "tool_calls");
            putarray(w);
            for (iz i = 0; i < m->blocks.len; i++) {
                Block *b = &m->blocks[i];
                if (b->type != BLOCK_CALL) continue;
                putobject(w);
                putkey(w, "id");
                putstr(w, b->id);
                putkey(w, "type");
                putstr(w, "function");
                putkey(w, "function");
                putobject(w);
                putkey(w, "name");
                putstr(w, b->name);
                putkey(w, "arguments");
                putstr(w, wireargs(a, b->args));
                putend(w);
                putend(w);
            }
            putend(w);
        }
    } break;
    }
    putend(w);
}

// The screenshots of a run of tool results, h[beg, end), as a user
// message after them: Chat Completions takes images only from the user.
static void encodeimages(JsonOut *w, Arena tmp, Slice<Msg> h, iz beg, iz end)
{
    b32 any = 0;
    for (iz i = beg; i < end; i++) {
        any |= h[i].imgb64.len>0 || h[i].imgalt.len>0;
    }
    if (!any) {
        return;
    }
    putobject(w);
    putstrkey(w, "role", "user");
    putkey(w, "content");
    putarray(w);
    for (iz i = beg; i < end; i++) {
        Msg *m = &h[i];
        if (!m->imgb64.len) {
            if (m->imgalt.len) {
                putobject(w);
                putstrkey(w, "type", "text");
                putstrkey(w, "text", m->imgalt);
                putend(w);
            }
            continue;
        }
        Arena t = tmp;
        putobject(w);
        putstrkey(w, "type", "text");
        putstrkey(w, "text", concat(&t, concat(&t, "The screenshot of tool call ",
                                                m->callid), ":"));
        putend(w);
        putobject(w);
        putstrkey(w, "type", "image_url");
        putkey(w, "image_url");
        putobject(w);
        putkey(w, "url");
        Str url = concat(&t, "\"data:image/png;base64,", m->imgb64);
        putraw(w, concat(&t, url, "\""));  // base64 needs no escapes
        putend(w);
        putend(w);
    }
    putend(w);
    putend(w);
}

// Encode a streaming request into a, using tmp for temporaries. Keys
// come in a fixed order, so equal histories give equal bytes.
static Str encodechat(Arena *a, Arena tmp, ChatReq *r)
{
    Buf     b(a, 1<<16);
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "model");
    putstr(&w, r->mc->name);
    putkey(&w, "messages");
    putarray(&w);
    if (r->system.len) {
        putobject(&w);
        putkey(&w, "role");
        putstr(&w, "system");
        putkey(&w, "content");
        putstr(&w, r->system);
        putend(&w);
    }
    iz results = -1;  // where the run of tool results began
    for (iz i = 0; i < r->history.len; i++) {
        Arena t    = tmp;
        b32   tool = r->history[i].role == ROLE_TOOL;
        if (!tool && results>=0) {
            encodeimages(&w, tmp, r->history, results, i);
            results = -1;
        }
        results = tool && results<0 ? i : results;
        encodemsg(&w, r->mc, &r->history[i], &t);
    }
    if (results >= 0) {
        encodeimages(&w, tmp, r->history, results, r->history.len);
    }
    putend(&w);
    if (r->tools.len) {
        putkey(&w, "tools");
        putraw(&w, r->tools);
    }
    putkey(&w, "stream");
    putbool(&w, 1);
    putkey(&w, "stream_options");
    putobject(&w);
    putkey(&w, "include_usage");
    putbool(&w, 1);
    putend(&w);
    if (r->mc->llamacpp) {
        // Prefill progress, and the means to end reasoning early
        putkey(&w, "return_progress");
        putbool(&w, 1);
        putkey(&w, "reasoning_control");
        putbool(&w, 1);
    }
    for (iz i = 0; i < r->mc->extras.len; i++) {
        putkey(&w, r->mc->extras[i].key);
        putraw(&w, r->mc->extras[i].json);
    }
    putend(&w);
    return finish(&b);
}


// Streamed responses
//
// The partial response accumulates blocks in order of first appearance:
// at most one reasoning block and one text block (OpenAI chat has one
// field for each), then the calls in index order as they start. Block
// numbers in deltas match the blocks of the committed message. A server
// that numbers its blocks itself (Anthropic) may send any number of each
// kind, which go to parts instead, while think and text still gather
// all of their kind, in order, for the checks that read them whole.

struct PartCall {
    i64 index;    // the server's tool call (or content block) index
    i32 block;
    Buf id;
    Buf name;
    Buf args;
    b32 stopped;  // the server said the block is complete (Anthropic)
};

struct PartBlock {
    i64 index;    // the server's content block index
    i32 block;
    i32 type;     // BLOCK_THINK, BLOCK_TEXT or BLOCK_REDACTED
    Buf text;     // a redacted block's data
    Buf sig;
};

struct Partial {
    Arena           *a;
    Buf              think;
    Buf              text;
    i32              thinkblock;  // -1 until it starts
    i32              textblock;
    i32              nblocks;
    Slice<PartCall>  calls;
    Slice<PartBlock> parts;       // blocks numbered by the server
    b32              indexed;     // so numbered: parts, not think and text
    Str              finish;      // finish_reason, null until one arrives
    Str              id;          // the completion's, null until one arrives
    Str              refusal;     // why the model declined, if it did
    Usage            usage;
    b32              hasusage;
    b32              done;        // [DONE]
};

static Partial *newpartial(Arena *a)
{
    Partial *p = alloc<Partial>(a);
    p->a          = a;
    p->think      = Buf(a, 1<<10);
    p->text       = Buf(a, 1<<10);
    p->thinkblock = -1;
    p->textblock  = -1;
    return p;
}

enum : i32 {
    DELTA_TEXT,
    DELTA_THINK,
    DELTA_ARGS,
};

struct Delta {
    i32 block;
    i32 kind;
    Str text;
    Str id;    // the first delta of a call
    Str name;
};

struct Chunk {
    Slice<Delta> deltas;
    b32          content;   // text, reasoning, or arguments arrived
    b32          prefill;   // prompt_progress arrived
    b32          hidden;    // reasoning goes on unseen (anchunk)
    i64          ptotal;
    i64          pdone;
    i64          pcached;
    Str          error;     // a mid-stream error, null if none
    i32          errcode;   // its code, if numeric
    b32          bad;       // not a chunk at all
};

static Str firststr(Json *j, Str a, Str b, Str c)
{
    Str v = jsonstr(jsonget(j, a));
    if (!v.len) v = jsonstr(jsonget(j, b));
    if (!v.len) v = jsonstr(jsonget(j, c));
    return v;
}

static PartCall *partcall(Partial *p, i64 index)
{
    for (iz i = 0; i < p->calls.len; i++) {
        if (p->calls[i].index == index) {
            return &p->calls[i];
        }
    }
    PartCall c = {};
    c.index = index;
    c.block = p->nblocks++;
    c.id    = Buf(p->a, 64);
    c.name  = Buf(p->a, 64);
    c.args  = Buf(p->a, 1<<10);
    p->calls = push(p->a, p->calls, c);
    return &p->calls[p->calls.len-1];
}

// The text of an error object, or of a string.
static Str errortext(Json *e)
{
    if (e->type == JSON_STRING) {
        return e->str;
    }
    Str msg = jsonstr(jsonget(e, "message"));
    return msg.data ? msg : e->raw;
}

// Apply one data event of the stream (already parsed JSON) to the
// partial response. Deltas are allocated in scratch.
static Chunk applychunk(Partial *p, Json *j, Arena *scratch)
{
    Chunk r = {};
    if (j->type != JSON_OBJECT) {
        r.bad = 1;
        return r;
    }

    Json *err = jsonget(j, "error");
    if (err->type != JSON_MISSING) {
        r.error   = errortext(err);
        r.errcode = (i32)jsonint(jsonget(err, "code"), 0);
        if (!r.error.len) r.error = "error event without a message";
        return r;
    }

    // llama.cpp ends reasoning early given this id (reasoning_end)
    Str id = jsonstr(jsonget(j, "id"));
    if (id.len && !p->id.data) {
        p->id = clone(p->a, id);
    }

    Json *pp = jsonget(j, "prompt_progress");
    if (pp->type == JSON_OBJECT) {
        r.prefill = 1;
        r.ptotal  = jsonint(jsonget(pp, "total"), 0);
        r.pdone   = jsonint(jsonget(pp, "processed"), 0);
        r.pcached = jsonint(jsonget(pp, "cache"), 0);
    }

    Json *u = jsonget(j, "usage");
    if (u->type == JSON_OBJECT) {
        p->hasusage   = 1;
        p->usage.in   = jsonint(jsonget(u, "prompt_tokens"), 0);
        p->usage.out  = jsonint(jsonget(u, "completion_tokens"), 0);
        Json *details = jsonget(u, "prompt_tokens_details");
        Json *hit     = jsonget(u, "prompt_cache_hit_tokens");  // DeepSeek
        p->usage.cached = jsonint(jsonget(details, "cached_tokens"), jsonint(hit, 0));
    }
    Json *t = jsonget(j, "timings");
    if (t->type == JSON_OBJECT) {
        // llama.cpp: exact cache hits, the proof of a byte-stable prefix
        p->usage.cached = jsonint(jsonget(t, "cache_n"), p->usage.cached);
    }

    Json *choice = jsonat(jsonget(j, "choices"), 0);
    Json *delta  = jsonget(choice, "delta");
    if (delta->type == JSON_MISSING) {
        delta = jsonget(choice, "message");  // a non-streamed shape
    }

    Str think = firststr(delta, "reasoning_content", "reasoning", "reasoning_text");
    if (think.len) {
        if (p->thinkblock < 0) {
            p->thinkblock = p->nblocks++;
        }
        print(&p->think, think);
        Delta d = {p->thinkblock, DELTA_THINK, think, {}, {}};
        r.deltas  = push(scratch, r.deltas, d);
        r.content = 1;
    }

    Str text = jsonstr(jsonget(delta, "content"));
    if (text.len) {
        if (p->textblock < 0) {
            p->textblock = p->nblocks++;
        }
        print(&p->text, text);
        Delta d = {p->textblock, DELTA_TEXT, text, {}, {}};
        r.deltas  = push(scratch, r.deltas, d);
        r.content = 1;
    }

    Json *calls = jsonget(delta, "tool_calls");
    i64   n     = 0;
    for (Json *c = calls->child; c; c = c->next, n++) {
        i64       index = jsonint(jsonget(c, "index"), n);
        b32       fresh = 1;
        for (iz i = 0; i < p->calls.len; i++) {
            fresh &= p->calls[i].index != index;
        }
        PartCall *pc   = partcall(p, index);
        Json     *fn   = jsonget(c, "function");
        Str       id   = jsonstr(jsonget(c, "id"));
        Str       name = jsonstr(jsonget(fn, "name"));
        Str       args = jsonstr(jsonget(fn, "arguments"));
        if (id.len && !pc->id.len) {
            print(&pc->id, id);
        }
        // Some servers repeat the name in every delta
        if (name.len && finish(&pc->name)!=name) {
            print(&pc->name, name);
        }
        print(&pc->args, args);
        if (fresh || args.len) {
            Delta d = {pc->block, DELTA_ARGS, args, {}, {}};
            if (fresh) {
                d.id   = finish(&pc->id);
                d.name = finish(&pc->name);
            }
            r.deltas  = push(scratch, r.deltas, d);
            r.content = 1;
        }
    }

    Str fr = jsonstr(jsonget(choice, "finish_reason"));
    if (fr.len) {
        p->finish = clone(p->a, fr);
    }
    return r;
}

// Whether committing the partial response is required rather than
// retrying it: it shows visible text or holds a tool call. Reasoning
// alone can be discarded.
static b32 committed(Partial *p)
{
    return trim(finish(&p->text)).len || p->calls.len;
}

// Call ids are kept as the server sent them, but must exist, be safe as
// file names ([A-Za-z0-9_-]{1,64}), and be unique in the session, unlike
// an earlier call's of the message (prior) or of the session (used),
// since rewinds, spill files and frontends go by them.
static Str callid(Arena *a, Str id, i64 turn, i64 step, iz n, Slice<Block> prior,
                  Slice<Str> used)
{
    Buf b(a, 72);
    for (iz i = 0; i<id.len && b.len<64; i++) {
        u8 c = id[i];
        putbyte(&b, alnum(c) || c=='_' || c=='-' ? c : '_');
    }
    if (!b.len) {
        print(&b, "call_");
        print(&b, turn);
        putbyte(&b, '_');
        print(&b, step);
        putbyte(&b, '_');
        print(&b, (i64)n);
    }
    Str base = finish(&b);
    Str r    = base;
    for (i32 dup = 2;; dup++) {
        b32 taken = 0;
        for (iz i = 0; i < prior.len; i++) {
            taken |= prior[i].type==BLOCK_CALL && prior[i].id==r;
        }
        for (iz i = 0; i<used.len && !taken; i++) {
            taken = used[i] == r;
        }
        if (!taken) {
            return r;
        }
        Buf d(a, base.len + 16);
        print(&d, base);
        print(&d, "_dup");
        print(&d, (i64)dup);
        r = finish(&d);
    }
}

struct Finished {
    Slice<Block> blocks;     // the message to commit
    iz           ncalls;     // complete calls among them
    iz           dropped;    // incomplete calls left out
};

// Turn the partial response into message blocks in perm. With complete
// set, every call is complete; otherwise only those followed by another
// call are known to be, or that the server said were (indexed), and the
// rest are dropped as incomplete. Text cut short (a cycle cut away)
// leaves out what follows the cut. Call ids differ from those used
// before in the session.
static Finished finishpartial(Arena *perm, Partial *p, b32 complete,
                              i64 turn, i64 step, Slice<Str> used = {})
{
    Finished r = {};
    iz       nblocks = p->nblocks;
    Block   *order   = alloc<Block>(perm, nblocks);
    b32     *keep    = alloc<b32>(perm, nblocks);
    if (p->thinkblock >= 0) {
        order[p->thinkblock].type = BLOCK_THINK;
        order[p->thinkblock].text = clone(perm, finish(&p->think));
        keep[p->thinkblock] = 1;
    }
    if (p->textblock >= 0) {
        order[p->textblock].type = BLOCK_TEXT;
        order[p->textblock].text = clone(perm, finish(&p->text));
        keep[p->textblock] = 1;
    }
    iz left = p->text.len;
    for (iz i = 0; i < p->parts.len; i++) {
        PartBlock *k = &p->parts[i];
        Str        t = finish(&k->text);
        if (k->type == BLOCK_TEXT) {
            t     = takehead(t, t.len<left ? t.len : left);
            left -= t.len;
            if (!t.len) {
                continue;
            }
        }
        Block *b = &order[k->block];
        b->type = k->type;
        b->text = clone(perm, t);
        b->sig  = clone(perm, finish(&k->sig));
        keep[k->block] = 1;
    }
    i64 lastindex = I64MIN;
    for (iz i = 0; i < p->calls.len; i++) {
        lastindex = p->calls[i].index>lastindex ? p->calls[i].index : lastindex;
    }
    for (iz i = 0; i < p->calls.len; i++) {
        PartCall *c = &p->calls[i];
        b32 incomplete = p->indexed ? !c->stopped : c->index==lastindex;
        if (!complete && incomplete) {
            r.dropped++;
            continue;
        }
        Block *b = &order[c->block];
        b->type = BLOCK_CALL;
        b->name = clone(perm, finish(&c->name));
        b->args = clone(perm, finish(&c->args));
        keep[c->block] = 1;
    }
    for (iz i = 0; i < nblocks; i++) {
        if (!keep[i]) continue;
        Block b = order[i];
        if (b.type == BLOCK_CALL) {
            PartCall *c = 0;
            for (iz k = 0; k < p->calls.len; k++) {
                c = p->calls[k].block==i ? &p->calls[k] : c;
            }
            b.id = callid(perm, finish(&c->id), turn, step, r.ncalls, r.blocks,
                          used);
            r.ncalls++;
        }
        r.blocks = push(perm, r.blocks, b);
    }
    return r;
}


// Errors

enum : i32 {
    CLASS_TRANSIENT,      // retry with backoff, or continue
    CLASS_DETERMINISTIC,  // the same request fails the same way
    CLASS_OVERFLOW,       // the context is full
    CLASS_FATAL,          // credentials, permissions
    CLASS_LOOP,           // the model is repeating itself
    CLASS_CANCEL,         // the user stopped it
    CLASS_LIMIT,          // a usage limit: spending, quota
};

static Str classname(i32 c)
{
    switch (c) {
    case CLASS_TRANSIENT:     return "transient";
    case CLASS_DETERMINISTIC: return "deterministic";
    case CLASS_OVERFLOW:      return "overflow";
    case CLASS_FATAL:         return "fatal";
    case CLASS_LOOP:          return "loop";
    case CLASS_LIMIT:         return "limit";
    }
    return "cancel";
}

// Where a case-insensitive ASCII substring begins, or -1.
static iz findfold(Str s, Str needle)
{
    for (iz i = 0; i+needle.len <= s.len; i++) {
        if (equalfold(Str{s.data+i, needle.len}, needle)) {
            return i;
        }
    }
    return -1;
}

static b32 contains(Str s, Str needle)
{
    return findfold(s, needle) >= 0;
}

// The message of an error response body: {"error":{"message":..}},
// {"error":".."}, {"message":..}, or the text itself. Anthropic's,
// {"type":"error","error":{"type":..,"message":..}}, is "TYPE: MESSAGE",
// since its type says what kind of error it is.
static Str errormessage(Arena *a, Str body)
{
    JsonDoc d = jsonparse(a, body);
    if (d.ok) {
        Json *e    = jsonget(d.root, "error");
        Str   type = jsonstr(jsonget(e, "type"));
        if (jsonstr(jsonget(d.root, "type"))=="error" && type.len) {
            return concat(a, concat(a, type, ": "), errortext(e));
        } else if (e->type != JSON_MISSING) {
            return errortext(e);
        }
        Str msg = jsonstr(jsonget(d.root, "message"));
        if (msg.len) {
            return msg;
        }
    }
    return trim(body);
}

// Classify an HTTP status and error text (a body or a stream error).
// Status 0 means the error came mid-stream, already past the headers.
// Anthropic's error types say as much as statuses do, so a stream's
// error event is classified by its type too.
static i32 classify(i32 status, Str text)
{
    static Str const overflow[] = {
        "exceed_context_size_error",
        "exceeds the available context size",
        "maximum context length",
        "context length exceeded",
        "context_length_exceeded",
        "prompt is too long",
        "greater than the context length",
        "too many tokens",
        "input is too long",
        "reduce the length of the messages",
        "request_too_large",          // Anthropic: the body, 413
        "exceed context limit",       // Anthropic: input plus max_tokens
    };
    if (contains(text, "failed to parse tool call arguments") ||
            contains(text, "raise_exception") ||
            contains(text, "template")) {
        return CLASS_DETERMINISTIC;  // llama.cpp: rendering or parsing
    }
    if (contains(text, "Context size has been exceeded")) {
        return CLASS_TRANSIENT;  // llama.cpp: the shared KV pool is full
    }
    for (iz i = 0; i < countof(overflow); i++) {
        if (contains(text, overflow[i])) {
            return CLASS_OVERFLOW;
        }
    }
    static Str const limits[] = {
        "enforced_spend_limit",
        "insufficient_quota",
        "exceeded your current quota",
        "quota exceeded",
        "usage limit",
        "billing_error",              // Anthropic
        "credit balance is too low",  // Anthropic, as a 400
    };
    for (iz i = 0; i < countof(limits); i++) {
        if (contains(text, limits[i])) {
            return CLASS_LIMIT;
        }
    }
    if (contains(text, "authentication_error") ||
            contains(text, "permission_error")) {
        return CLASS_FATAL;  // Anthropic, mid-stream too
    } else if (!status && contains(text, "invalid_request_error")) {
        return CLASS_DETERMINISTIC;
    }
    switch (status) {
    case 0:  // mid-stream
    case 408: case 409: case 425: case 429:
    case 500: case 502: case 503: case 504: case 529:
        return CLASS_TRANSIENT;
    case 402:
        return CLASS_LIMIT;
    case 401: case 403:
        return CLASS_FATAL;
    case 413:
        return CLASS_OVERFLOW;
    }
    if (status>=500 && status<600) {
        return CLASS_TRANSIENT;
    }
    return CLASS_DETERMINISTIC;
}

// The whole number just after a phrase in text, or 0.
static i64 numberafter(Str text, Str phrase)
{
    iz at = findfold(text, phrase);
    if (at < 0) {
        return 0;
    }
    iz i = at + phrase.len;
    for (; i<text.len && text[i]==' '; i++) {}
    iz j = i;
    for (; j<text.len && j-i<12 && digit(text[j]); j++) {}
    Int n = parseint(span(text.data+i, text.data+j));
    return n.ok && n.val>0 ? n.val : 0;
}

struct Overflow {
    i64 prompt;  // tokens in the rejected request, 0 if not said
    i64 ctx;     // the context limit, 0 if not said
};

// The sizes an overflow error reports: llama.cpp's n_prompt_tokens and
// n_ctx, OpenAI's and vLLM's "maximum context length is N tokens" with
// "(N in the messages", or Anthropic's "prompt is too long: N tokens >
// M maximum".
static Overflow overflowsizes(Arena scratch, Str body)
{
    Overflow r = {};
    JsonDoc  d = jsonparse(&scratch, body);
    Json    *e = jsonget(d.root, "error");
    e = e->type==JSON_OBJECT ? e : d.root;
    r.prompt = jsonint(jsonget(e, "n_prompt_tokens"), 0);
    r.ctx    = jsonint(jsonget(e, "n_ctx"), 0);
    if (!r.ctx) {
        r.ctx = numberafter(body, "maximum context length is");
    }
    if (!r.prompt && findfold(body, "prompt is too long:")>=0) {
        r.prompt = numberafter(body, "prompt is too long:");
        r.ctx    = r.ctx ? r.ctx : numberafter(body, "tokens >");
    }
    if (!r.prompt) {
        Str m  = errormessage(&scratch, body);
        iz  at = findfold(m, " in the messages");
        iz  i  = at;
        for (; i>0 && digit(m[i-1]); i--) {}
        if (at>0 && i<at && i>0 && m[i-1]=='(') {
            Int n = parseint(span(m.data+i, m.data+at));
            r.prompt = n.ok ? n.val : 0;
        }
    }
    r.prompt = r.prompt>0 ? r.prompt : 0;
    r.ctx    = r.ctx>0 ? r.ctx : 0;
    return r;
}

static Str headervalue(Str headers, Str name)
{
    for (Str rest = headers; rest.len;) {
        Cut line = cut(rest, '\n');
        rest = line.tail;
        Cut kv = cut(trim(line.head), ':');
        if (kv.ok && equalfold(trim(kv.head), name)) {
            return trim(kv.tail);
        }
    }
    return {};
}

// Days since 1970-01-01 of a proleptic Gregorian date.
static i64 epochdays(i64 y, i64 m, i64 d)
{
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y-399) / 400;
    i64 yoe = y - era*400;
    i64 doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1;
    i64 doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return era*146097 + doe - 719468;
}

// "Sun, 06 Nov 1994 08:49:37 GMT" as epoch milliseconds, or -1.
static i64 httpdate(Str s)
{
    static Str const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    Cut c = cut(s, ',');
    if (!c.ok) {
        return -1;
    }
    Str f[4] = {};
    Str rest = trim(c.tail);
    for (i32 i = 0; i < 4; i++) {
        Cut w = cut(rest, ' ');
        f[i] = w.head;
        rest = trim(w.tail);
    }
    Int day  = parseint(f[0]);
    Int year = parseint(f[2]);
    i32 mon  = 0;
    for (; mon<12 && !equalfold(months[mon], f[1]); mon++) {}
    Str hms = f[3];
    if (!day.ok || !year.ok || mon==12 || hms.len!=8) {
        return -1;
    }
    Int hh = parseint(takehead(hms, 2));
    Int mm = parseint(Str{hms.data+3, 2});
    Int ss = parseint(Str{hms.data+6, 2});
    if (!hh.ok || !mm.ok || !ss.ok) {
        return -1;
    }
    i64 days = epochdays(year.val, mon+1, day.val);
    return ((days*24 + hh.val)*60 + mm.val)*60000 + ss.val*1000;
}

// The wait a response asks for, in milliseconds, or -1 if none: from
// retry-after-ms, then retry-after (seconds or an HTTP date).
static i64 retryafter(Str headers, i64 nowms)
{
    Str ms = headervalue(headers, "retry-after-ms");
    if (ms.len) {
        Float f = parsefloat(ms);
        if (f.ok && f.val>=0 && f.val<1e12) {
            return (i64)f.val;
        }
    }
    Str ra = headervalue(headers, "retry-after");
    if (!ra.len) {
        return -1;
    }
    Float f = parsefloat(ra);
    if (f.ok) {
        return f.val<0 ? -1 : f.val>1e9 ? (i64)1e12 : (i64)(f.val*1000);
    }
    i64 at = httpdate(ra);
    return at<0 ? -1 : at>nowms ? at-nowms : 0;
}

// Backoff before retry n (from 1): min(500 ms * 2^(n-1), 8 s), scaled by
// a factor in [0.75, 1] from rnd.
static i64 backoffms(i32 n, u64 rnd)
{
    i64 base = n>=5 ? 8000 : (i64)500<<(n-1);
    base = base<8000 ? base : 8000;
    f64 u = 0.75 + 0.25*(f64)(rnd>>11)*0x1p-53;
    return (i64)((f64)base * u);
}


// llama.cpp router discovery: GET /v1/models only
//
// Never probe /props, /slots or /metrics without autoload=false: they
// load the model, and the router keeps one model loaded at a time. The
// per-conversation context limit is min(n_ctx_train, n_ctx). n_ctx_train
// (scaled when YaRN or rope scaling is configured) is known only while
// the model is loaded; until then the limit comes from --ctx-size, which
// under --kv-unified is the shared pool and each sequence may use all of
// it, and otherwise is divided among --parallel slots.
//
// Anthropic's listing gives each model's limits outright: the context
// (max_input_tokens) and the output (max_tokens).

struct ModelInfo {
    b32 found;     // the endpoint lists the model
    b32 llamacpp;  // served by llama.cpp (the router or llama-server)
    b32 loaded;
    b32 learned;   // ctx came from a loaded model's metadata
    i64 ctx;       // context limit, 0 if unknown
    i64 trained;   // n_ctx_train, scaled, 0 if unknown
    i32 slots;     // --parallel, the server's total_slots, 0 if unknown
    i64 maxout;    // most output tokens in a response, 0 if unknown
};

static Str argvalue(Json *args, Str name)
{
    for (Json *a = args->child; a; a = a->next) {
        if (jsonstr(a)==name && a->next) {
            return jsonstr(a->next);
        }
    }
    return {};
}

static b32 hasarg(Json *args, Str name)
{
    for (Json *a = args->child; a; a = a->next) {
        if (jsonstr(a) == name) {
            return 1;
        }
    }
    return 0;
}

// Read one model's entry of a /v1/models listing. trained is a cached
// n_ctx_train from an earlier listing, 0 if never seen.
static ModelInfo modelinfo(Json *m, i64 trained)
{
    ModelInfo r = {};
    if (m->type != JSON_OBJECT) {
        return r;
    }
    r.found = 1;

    i64 maxin  = jsonint(jsonget(m, "max_input_tokens"), 0);
    i64 maxout = jsonint(jsonget(m, "max_tokens"), 0);
    if (maxin > 0) {
        r.ctx     = maxin;  // Anthropic: the server's own word
        r.learned = 1;
        r.maxout  = maxout>0 ? maxout : 0;
        return r;
    }

    Json *status = jsonget(m, "status");
    Json *args   = jsonget(status, "args");
    Str   owner  = jsonstr(jsonget(m, "owned_by"));
    r.llamacpp = owner=="llamacpp" || args->type==JSON_ARRAY;
    r.loaded   = jsonstr(jsonget(status, "value")) == "loaded";
    Int np = parseint(argvalue(args, "--parallel"));
    if (!np.ok) np = parseint(argvalue(args, "-np"));
    r.slots = np.ok && np.val>0 ? (i32)(np.val<1024 ? np.val : 1024) : 0;

    // Rope scaling stretches the trained context
    f64 scale = 1;
    Str kind  = argvalue(args, "--rope-scaling");
    Float rs  = parsefloat(argvalue(args, "--rope-scale"));
    if ((kind=="yarn" || kind=="linear") && rs.ok && rs.val>1) {
        scale = rs.val;
    }

    Json *meta = jsonget(m, "meta");
    i64   nctx = jsonint(jsonget(meta, "n_ctx"), 0);
    i64   ntr  = jsonint(jsonget(meta, "n_ctx_train"), 0);
    if (ntr > 0) {
        trained = (i64)((f64)ntr * scale);
    }
    r.trained = trained;

    if (nctx > 0) {
        r.ctx     = trained>0 && trained<nctx ? trained : nctx;
        r.learned = 1;
        return r;
    }

    Int size = parseint(argvalue(args, "--ctx-size"));
    if (!size.ok) size = parseint(argvalue(args, "-c"));
    if (size.ok && size.val>0) {
        i64 per = size.val;
        if (!hasarg(args, "--kv-unified") && !hasarg(args, "-kvu") &&
                np.ok && np.val>1) {
            per = size.val / np.val;
        }
        r.ctx = trained>0 && trained<per ? trained : per;
    }
    return r;
}

// Read a /v1/models listing for one model, as modelinfo.
static ModelInfo parsemodels(Arena scratch, Str body, Str model, i64 trained)
{
    JsonDoc d = jsonparse(&scratch, body);
    Json   *m = (Json *)&jsonmissing;
    for (Json *e = jsonget(d.root, "data")->child; e; e = e->next) {
        if (jsonstr(jsonget(e, "id")) == model) {
            m = e;
        }
    }
    return modelinfo(m, trained);
}


// Text cycles
//
// A model stuck in a loop repeats an exact unit of text. Over the last
// 4 KiB of a stream, every 128 bytes, look for a unit of up to 1 KiB at
// the very end repeated back to back: a unit of 60 bytes or less must
// repeat 4 times and cover 180 bytes, a longer one 3 times and 1 KiB.

enum : iz {
    CYCLE_TAIL  = 1<<12,
    CYCLE_UNIT  = 1<<10,
    CYCLE_EVERY = 128,
};

struct Cycle {
    u8 buf[CYCLE_TAIL];
    iz len;
    iz since;  // bytes since the last scan
    iz unit;   // the unit found, 0 if none
    iz reps;
};

static b32 cyclescan(Cycle *c)
{
    u8 *s = c->buf;
    iz  n = c->len;
    for (iz unit = 1; unit<=CYCLE_UNIT && unit*3<=n; unit++) {
        b32 small = unit <= 60;
        iz  need  = small ? 4 : 3;
        iz  cover = small ? 180 : 1024;
        iz  reps  = 1;
        while ((reps+1)*unit <= n &&
               !__builtin_memcmp(s+n-unit, s+n-(reps+1)*unit, (uz)unit)) {
            reps++;
        }
        if (reps>=need && reps*unit>=cover) {
            c->unit = unit;
            c->reps = reps;  // all of them, so a cut can keep just one
            return 1;
        }
    }
    return 0;
}

// Feed streamed text. Once a cycle is found, returns how much of the
// text was consumed when it was, else -1.
static iz cyclefeed(Cycle *c, Str text)
{
    for (iz i = 0; i < text.len; i++) {
        if (c->len == CYCLE_TAIL) {
            iz drop = CYCLE_TAIL/2;
            for (iz k = drop; k < CYCLE_TAIL; k++) {
                c->buf[k-drop] = c->buf[k];
            }
            c->len -= drop;
        }
        c->buf[c->len++] = text[i];
        if (++c->since >= CYCLE_EVERY) {
            c->since = 0;
            if (cyclescan(c)) {
                return i + 1;
            }
        }
    }
    return -1;
}


// Unexpected stops
//
// A model sometimes ends its turn with a promise instead of the work,
// "Let me run the tests now." and no tool call. A final response is
// taken to announce work it never did when its text ends with a colon,
// or when its last sentence holds one of stopphrases, unless it also
// holds one of stopexcepts or is a question. Words match whole, in any
// ASCII case, with a typographic apostrophe taken as a plain one. The
// lists are data: a phrase missing here only lets a turn end early, as
// before, while one too eager costs a needless reminder, so the
// exceptions err toward letting a turn end.

static Str const stopphrases[] = {
    "let me", "let's",
    "i'll now", "i will now", "now i'll", "now i will",
    "next, i'll", "next i'll", "next, i will", "next i will", "i'll next",
    "i'm going to", "i am going to", "i'm about to",
    "i'll start", "i'll begin", "i'll proceed", "i will proceed",
    "i'll go ahead", "i'll continue", "i will continue", "proceeding to",
    "i'll run", "i will run", "i'll check", "i will check",
    "i'll look", "i'll read", "i'll write", "i'll create", "i'll add",
    "i'll update", "i'll fix", "i'll implement", "i'll edit",
    "i'll make", "i'll try", "i'll test", "i'll verify",
};

static Str const stopexcepts[] = {
    "let me know", "if", "unless", "when you", "once you", "would you",
    "want me to", "shall i", "should i", "do you want", "feel free",
};

// Whether a lowercase phrase occurs in s with word boundaries around it.
static b32 hasphrase(Str s, Str phrase)
{
    for (iz i = 0; i+phrase.len <= s.len; i++) {
        if (Str{s.data+i, phrase.len} != phrase) {
            continue;
        }
        iz  e      = i + phrase.len;
        b32 before = i==0 || (!alnum(s[i-1]) && s[i-1]!='\'');
        b32 after  = e==s.len || !alnum(s[e]);
        if (before && after) {
            return 1;
        }
    }
    return 0;
}

static b32 announces(Str text)
{
    Str t = trim(text);
    if (!t.len) {
        return 0;
    } else if (t[t.len-1] == ':') {
        return 1;
    }

    // Back over the closing punctuation and markup of the last sentence,
    // then to the end of the one before: a terminator followed by a
    // blank, so that "main.c" stays whole
    iz end = t.len;
    for (; end > 0; end--) {
        u8 c = t[end-1];
        if (c == '?') {
            return 0;  // a question for the user
        } else if (c!='.' && c!='!' && c!='*' && c!='_' && c!='`' &&
                   c!=')' && c!='"' && c!='\'' && !whitespace(c)) {
            break;
        }
    }
    iz beg = end;
    for (; beg > 0; beg--) {
        u8 c = t[beg-1];
        if (c=='\n' || ((c=='.' || c=='!' || c=='?') && beg<end &&
                        whitespace(t[beg]))) {
            break;
        }
    }

    // Lowercase, with U+2019 as an apostrophe, the last 512 bytes at most
    u8 buf[512];
    iz len = 0;
    iz i   = end-beg>countof(buf) ? end-countof(buf) : beg;
    for (; i<end && len<countof(buf); i++) {
        if (i+2<end && t[i]==0xe2 && t[i+1]==0x80 && t[i+2]==0x99) {
            buf[len++] = '\'';
            i += 2;
        } else {
            buf[len++] = lowercase(t[i]);
        }
    }
    Str last = {buf, len};
    for (iz k = 0; k < countof(stopexcepts); k++) {
        if (hasphrase(last, stopexcepts[k])) {
            return 0;
        }
    }
    for (iz k = 0; k < countof(stopphrases); k++) {
        if (hasphrase(last, stopphrases[k])) {
            return 1;
        }
    }
    return 0;
}

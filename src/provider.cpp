// Providers: URLs, the canonical transcript, OpenAI Chat Completions,
// error classification, and llama.cpp router discovery
//
// The transcript is append-only: an assistant message keeps the blocks
// the model produced (reasoning, text, tool calls with their raw
// argument strings), and requests are encoded from it deterministically,
// so that each request's prefix is byte-identical to the previous one
// and the server's prompt cache keeps hitting.
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
};

struct Block {
    i32 type;
    Str text;  // think and text blocks
    Str id;    // call blocks
    Str name;
    Str args;  // the raw argument string, as the model produced it
};

struct Msg {
    i32          role;
    Str          text;    // user and tool messages
    Slice<Block> blocks;  // assistant messages
    Str          callid;  // tool messages
};

struct Usage {
    i64 in;      // prompt tokens
    i64 out;     // completion tokens
    i64 cached;  // prompt tokens served from the cache
};

static Str blockname(i32 type)
{
    switch (type) {
    case BLOCK_THINK: return "think";
    case BLOCK_TEXT:  return "text";
    }
    return "call";
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

struct ModelConf {
    Str          name;       // as sent in requests
    Str          reasoning;  // field to send reasoning back in, empty: never
    Slice<Extra> extras;     // sampling parameters, only if configured
    b32          llamacpp;   // llama.cpp extras (return_progress)
    i64          ctx;        // context limit in tokens, 0 if unknown
};

struct ChatReq {
    ModelConf *mc;
    Str        system;
    Str        tools;    // the tools array, already encoded
    Slice<Msg> history;
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
    for (iz i = 0; i < r->history.len; i++) {
        Arena t = tmp;
        encodemsg(&w, r->mc, &r->history[i], &t);
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
        putkey(&w, "return_progress");
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
// numbers in deltas match the blocks of the committed message.

struct PartCall {
    i64 index;  // the server's tool call index
    i32 block;
    Buf id;
    Buf name;
    Buf args;
};

struct Partial {
    Arena          *a;
    Buf             think;
    Buf             text;
    i32             thinkblock;  // -1 until it starts
    i32             textblock;
    i32             nblocks;
    Slice<PartCall> calls;
    Str             finish;      // finish_reason, null until one arrives
    Usage           usage;
    b32             hasusage;
    b32             done;        // [DONE]
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

// Call ids are kept as the server sent them, but must exist, be unique
// in the message, and be safe as file names: [A-Za-z0-9_-]{1,64}.
static Str callid(Arena *a, Str id, i64 turn, i64 step, iz n, Slice<Block> prior)
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
// call are known to be, and the last is dropped as incomplete.
static Finished finishpartial(Arena *perm, Partial *p, b32 complete,
                              i64 turn, i64 step)
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
    i64 lastindex = I64MIN;
    for (iz i = 0; i < p->calls.len; i++) {
        lastindex = p->calls[i].index>lastindex ? p->calls[i].index : lastindex;
    }
    for (iz i = 0; i < p->calls.len; i++) {
        PartCall *c = &p->calls[i];
        if (!complete && c->index==lastindex) {
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
            b.id = callid(perm, finish(&c->id), turn, step, r.ncalls, r.blocks);
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
    CLASS_FATAL,          // credentials, permissions, spending
    CLASS_LOOP,           // the model is repeating itself
    CLASS_CANCEL,         // the user stopped it
};

static Str classname(i32 c)
{
    switch (c) {
    case CLASS_TRANSIENT:     return "transient";
    case CLASS_DETERMINISTIC: return "deterministic";
    case CLASS_OVERFLOW:      return "overflow";
    case CLASS_FATAL:         return "fatal";
    case CLASS_LOOP:          return "loop";
    }
    return "cancel";
}

// Case-insensitive substring search over ASCII.
static b32 contains(Str s, Str needle)
{
    for (iz i = 0; i+needle.len <= s.len; i++) {
        if (equalfold(Str{s.data+i, needle.len}, needle)) {
            return 1;
        }
    }
    return 0;
}

// The message of an error response body: {"error":{"message":..}},
// {"error":".."}, {"message":..}, or the text itself.
static Str errormessage(Arena *a, Str body)
{
    JsonDoc d = jsonparse(a, body);
    if (d.ok) {
        Json *e = jsonget(d.root, "error");
        if (e->type != JSON_MISSING) {
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
    if (contains(text, "enforced_spend_limit") || contains(text, "insufficient_quota")) {
        return CLASS_FATAL;
    }
    switch (status) {
    case 0:  // mid-stream
    case 408: case 409: case 425: case 429:
    case 500: case 502: case 503: case 504: case 529:
        return CLASS_TRANSIENT;
    case 401: case 402: case 403:
        return CLASS_FATAL;
    }
    if (status>=500 && status<600) {
        return CLASS_TRANSIENT;
    }
    return CLASS_DETERMINISTIC;
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

struct ModelInfo {
    b32 found;     // the endpoint lists the model
    b32 llamacpp;  // served by llama.cpp (the router or llama-server)
    b32 loaded;
    b32 learned;   // ctx came from a loaded model's metadata
    i64 ctx;       // context limit, 0 if unknown
    i64 trained;   // n_ctx_train, scaled, 0 if unknown
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

// Read a /v1/models listing for one model. trained is a cached
// n_ctx_train from an earlier listing, 0 if never seen.
static ModelInfo parsemodels(Arena scratch, Str body, Str model, i64 trained)
{
    ModelInfo r = {};
    JsonDoc   d = jsonparse(&scratch, body);
    Json     *m = (Json *)&jsonmissing;
    for (Json *e = jsonget(d.root, "data")->child; e; e = e->next) {
        if (jsonstr(jsonget(e, "id")) == model) {
            m = e;
        }
    }
    if (m->type != JSON_OBJECT) {
        return r;
    }
    r.found = 1;

    Json *status = jsonget(m, "status");
    Json *args   = jsonget(status, "args");
    Str   owner  = jsonstr(jsonget(m, "owned_by"));
    r.llamacpp = owner=="llamacpp" || args->type==JSON_ARRAY;
    r.loaded   = jsonstr(jsonget(status, "value")) == "loaded";

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
        Int np  = parseint(argvalue(args, "--parallel"));
        if (!np.ok) np = parseint(argvalue(args, "-np"));
        if (!hasarg(args, "--kv-unified") && !hasarg(args, "-kvu") &&
                np.ok && np.val>1) {
            per = size.val / np.val;
        }
        r.ctx = trained>0 && trained<per ? trained : per;
    }
    return r;
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

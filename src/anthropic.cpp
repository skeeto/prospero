// Anthropic's Messages API: requests and streamed responses
//
// An endpoint of kind anthropic takes requests at URL/v1/messages, with
// its key as x-api-key and anthropic-version: 2023-06-01, and lists its
// models at URL/v1/models (provider.cpp reads their limits). Requests
// are encoded from the canonical transcript (provider.cpp) as
// deterministically as chat requests are, so that each one's prefix is
// the previous one's and the prompt cache keeps hitting:
//
// - The tools, converted from the chat request's array of functions,
//   then the system prompt, which holds the instruction files, each end
//   with a cache breakpoint. The conversation has two more, within the
//   API's limit of four: the last message, which the next request reads
//   back, and the message before the latest response, where the request
//   that produced it wrote its own.
// - Screenshots leave requests in steps (desktop.cpp), which change the
//   prefix at the oldest dropped. The request at a step reads up to just
//   before that one (FLOOR_READ) instead of at the message before the
//   latest response, which no longer matches, and writes up to just
//   before the oldest still shown (FLOOR_WRITE), the next step's change,
//   which the request where the first screenshot arrives also does.
//   Those take the tools' breakpoint when four are in use, since the
//   system prompt's covers the tools too.
// - Messages of one role in a row are merged into one, its tool results
//   first, since the API wants the roles to alternate and a message's
//   tool results before its text. Blank texts, which the API rejects,
//   are left out, and so is an assistant message with nothing else.
// - Reasoning goes back verbatim with its signature, which the API
//   checks against the conversation before it. Reasoning without one
//   (another model's, or a response cut off) cannot go back, and neither
//   can reasoning from before the latest compaction, which changed what
//   came before it: removing the oldest is all the API allows.
// - A tool result with a screenshot (the desktop tool's) holds it as an
//   image block after its text.
// - Tool input streams as the model writes it (eager_input_streaming),
//   so a long file shows progress and keeps the progress watchdog fed.
//   It comes unchecked, and repairargs and the tools check it as they
//   would any server's.
//
// A response streams as message_start, then each content block by its
// index (content_block_start, content_block_delta, content_block_stop),
// then message_delta with the stop reason and the output's usage, then
// message_stop, with ping events between and an error event should it
// fail. Its blocks go into the partial response by index. Stop reasons
// become the chat ones the engine knows: end_turn and stop_sequence
// "stop", tool_use "tool_calls", and max_tokens and
// model_context_window_exceeded "length". The rest stay as they are:
// pause_turn, which the engine continues, and refusal, which ends the
// turn, saying why.
//
// Reasoning not shown (thinking_display omitted) streams no text, only
// its signature at the end, so a long think is pings alone. A ping in a
// reasoning block not yet signed is reported (Chunk.hidden), and the
// engine counts it as progress for as long as max_tokens would take at
// AN_SLOWRATE, so that a stream stuck that way still ends.
//
// Everything here is pure: the engine does the I/O.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    AN_MAXTOKENS = 32000,  // max_tokens unless configured
    AN_BUDGET    = 8192,   // thinking budget_tokens unless configured
    AN_MINBUDGET = 1024,   // the least budget the API takes
    AN_SLOWRATE  = 10,     // output tokens a second, below any model's
};

// The max_tokens of a request: configured, or the default, within the
// model's limit.
static i64 anmaxtokens(ModelConf *mc)
{
    i64 most = mc->maxtokens ? mc->maxtokens : AN_MAXTOKENS;
    return mc->maxout && most>mc->maxout ? mc->maxout : most;
}

static Str const anversion = "2023-06-01";

static void ancache(JsonOut *w)
{
    putkey(w, "cache_control");
    putobject(w);
    putstrkey(w, "type", "ephemeral");
    putend(w);
}


// Requests

// The tools of a chat request (an array of functions) as the Messages
// API takes them, the last with a cache breakpoint if cache.
static void antools(JsonOut *w, Arena scratch, Str tools, b32 cache)
{
    JsonDoc d = jsonparse(&scratch, tools);
    putarray(w);
    for (Json *t = d.root->child; d.root->type==JSON_ARRAY && t; t = t->next) {
        Json *fn = jsonget(t, "function");
        putobject(w);
        putstrkey(w, "name", jsonstr(jsonget(fn, "name")));
        putstrkey(w, "description", jsonstr(jsonget(fn, "description")));
        putkey(w, "input_schema");
        putjson(w, jsonget(fn, "parameters"));
        putkey(w, "eager_input_streaming");
        putbool(w, 1);
        if (cache && !t->next) {
            ancache(w);
        }
        putend(w);
    }
    putend(w);
}

// One content block of a request message, as the history holds it.
struct AnPart {
    i32    n;      // the request message it goes in, merged
    b32    asst;
    Msg   *msg;
    Block *block;  // of an assistant message, else null
};

static b32 anblank(Str s)
{
    return !trim(s).len;
}

// Whether an assistant block can go back to the API: reasoning only
// with what the API needs to check it, and when its message is no
// older than old allows.
static b32 ansendable(Block *b, b32 old)
{
    switch (b->type) {
    case BLOCK_THINK:    return !old && b->sig.len;
    case BLOCK_REDACTED: return !old && b->text.len;
    case BLOCK_TEXT:     return !anblank(b->text);
    }
    return 1;
}

// The history as request content blocks, numbered by the merged
// message each goes in.
static Slice<AnPart> anparts(Arena *a, Slice<Msg> h, i64 thinkfrom)
{
    Slice<AnPart> r = {};
    i32           n = -1;
    for (iz i = 0; i < h.len; i++) {
        Msg *m    = &h[i];
        b32  asst = m->role == ROLE_ASSISTANT;
        b32  next = !r.len || r[r.len-1].asst!=asst;
        if (asst) {
            // Only with text or a call: reasoning alone stays out
            b32 shows = 0;
            for (iz k = 0; k < m->blocks.len; k++) {
                Block *b = &m->blocks[k];
                shows |= ansendable(b, 0) && b->type!=BLOCK_THINK &&
                         b->type!=BLOCK_REDACTED;
            }
            if (!shows) {
                continue;
            }
            n += next;
            for (iz k = 0; k < m->blocks.len; k++) {
                Block *b = &m->blocks[k];
                if (ansendable(b, m->seq<thinkfrom)) {
                    AnPart p = {n, 1, m, b};
                    r = push(a, r, p);
                }
            }
        } else if (m->role==ROLE_TOOL || !anblank(m->text) || m->images.len) {
            n += next;
            AnPart p = {n, 0, m, 0};
            r = push(a, r, p);
        }
    }
    return r;
}

// An image block, or the text in its place.
static void animage(JsonOut *w, Arena scratch, MsgImage *g, b32 cache)
{
    putobject(w);
    if (g->b64.len) {
        putstrkey(w, "type", "image");
        putkey(w, "source");
        putobject(w);
        putstrkey(w, "type", "base64");
        putstrkey(w, "media_type", g->mime);
        putkey(w, "data");
        putb64(w, scratch, {}, g->b64);
        putend(w);
    } else {
        putstrkey(w, "type", "text");
        putstrkey(w, "text", imagealt(g));
    }
    if (cache) {
        ancache(w);
    }
    putend(w);
}

static void anblock(JsonOut *w, Arena scratch, AnPart *p, b32 cache)
{
    Msg *m = p->msg;
    if (!p->block && m->role==ROLE_USER && m->images.len) {
        // Its images first, then its text, if any, which has the
        // breakpoint, else the last image
        b32 text = !anblank(m->text);
        for (iz i = 0; i < m->images.len; i++) {
            animage(w, scratch, &m->images[i], cache && !text && i==m->images.len-1);
        }
        if (!text) {
            return;
        }
    }
    putobject(w);
    if (!p->block && m->role==ROLE_TOOL && (m->imgb64.len || m->imgalt.len)) {
        // A screenshot goes in the result, after its text, or what
        // stands in its place
        putstrkey(w, "type", "tool_result");
        putstrkey(w, "tool_use_id", m->callid);
        putkey(w, "content");
        putarray(w);
        if (!anblank(m->text)) {
            putobject(w);
            putstrkey(w, "type", "text");
            putstrkey(w, "text", m->text);
            putend(w);
        }
        putobject(w);
        if (m->imgb64.len) {
            putstrkey(w, "type", "image");
            putkey(w, "source");
            putobject(w);
            putstrkey(w, "type", "base64");
            putstrkey(w, "media_type", "image/png");
            putkey(w, "data");
            Str data = concat(&scratch, "\"", m->imgb64);
            putraw(w, concat(&scratch, data, "\""));  // base64 needs no escapes
            putend(w);
        } else {
            putstrkey(w, "type", "text");
            putstrkey(w, "text", m->imgalt);
        }
        putend(w);
        putend(w);
        if (m->error) {
            putkey(w, "is_error");
            putbool(w, 1);
        }
    } else if (!p->block && m->role==ROLE_TOOL) {
        putstrkey(w, "type", "tool_result");
        putstrkey(w, "tool_use_id", m->callid);
        if (m->text.len) {
            putstrkey(w, "content", m->text);
        }
        if (m->error) {
            putkey(w, "is_error");
            putbool(w, 1);
        }
    } else if (!p->block) {
        putstrkey(w, "type", "text");
        putstrkey(w, "text", m->text);
    } else {
        Block *b = p->block;
        switch (b->type) {
        case BLOCK_THINK:
            putstrkey(w, "type", "thinking");
            putstrkey(w, "thinking", b->text);
            putstrkey(w, "signature", b->sig);
            break;
        case BLOCK_REDACTED:
            putstrkey(w, "type", "redacted_thinking");
            putstrkey(w, "data", b->text);
            break;
        case BLOCK_TEXT:
            putstrkey(w, "type", "text");
            putstrkey(w, "text", b->text);
            break;
        case BLOCK_CALL:
            putstrkey(w, "type", "tool_use");
            putstrkey(w, "id", b->id);
            putstrkey(w, "name", b->name);
            putkey(w, "input");
            putraw(w, wireargs(&scratch, b->args));
            break;
        }
    }
    if (cache) {
        ancache(w);
    }
    putend(w);
}

// Whether a request part may hold a cache breakpoint: reasoning may not.
static b32 ancacheable(AnPart *p)
{
    return !p->block || (p->block->type!=BLOCK_THINK &&
                         p->block->type!=BLOCK_REDACTED);
}

// Where a request's parts go and which hold cache breakpoints.
struct AnPlan {
    Slice<AnPart> parts;
    Slice<iz>     order;  // parts in the order they go, message by message
    Slice<b32>    mark;   // by position in order: a cache breakpoint
    b32           tools;  // the tools' breakpoint too
};

static AnPlan anplan(Arena *a, ChatReq *r)
{
    AnPlan        p     = {};
    Slice<AnPart> parts = anparts(a, r->history, r->thinkfrom);
    i32 nmsgs    = parts.len ? parts[parts.len-1].n+1 : 0;
    i32 lastasst = -1;
    b32 dropped  = 0;
    for (iz i = 0; i < parts.len; i++) {
        lastasst = parts[i].asst ? parts[i].n : lastasst;
        dropped |= !parts[i].block && parts[i].msg->floor==FLOOR_READ;
    }
    // Roles alternate: a user message, or none. Where screenshots were
    // just dropped, the prefix changed before it, so it reads nothing
    i32 prev = dropped ? -1 : lastasst-1;

    p.parts = parts;
    p.mark  = {alloc<b32>(a, parts.len), parts.len, parts.len};
    for (iz beg = 0, end = 0; beg < parts.len; beg = end) {
        i32 n    = parts[beg].n;
        b32 asst = parts[beg].asst;
        for (end = beg; end<parts.len && parts[end].n==n; end++) {}

        // The order in which its parts go: a user message's tool results
        // first, then its texts; the breakpoint goes on the last that
        // may hold one
        iz first = p.order.len;
        for (i32 pass = 0; pass < 2; pass++) {
            for (iz i = beg; i < end; i++) {
                b32 result = !parts[i].block && parts[i].msg->role==ROLE_TOOL;
                b32 lead   = asst || result;
                if (lead == !pass) {
                    p.order = push(a, p.order, i);
                }
            }
        }
        if (n==nmsgs-1 || n==prev) {
            iz mark = -1;
            for (iz k = first; k < p.order.len; k++) {
                mark = ancacheable(&parts[p.order[k]]) ? k : mark;
            }
            if (mark >= 0) {
                p.mark[mark] = 1;
            }
        }
    }

    // The screenshot floors: a breakpoint on the last part before each
    // that may hold one
    for (iz k = 0; k < p.order.len; k++) {
        AnPart *q = &parts[p.order[k]];
        if (q->block || q->msg->floor==FLOOR_NONE) {
            continue;
        }
        for (iz j = k-1; j >= 0; j--) {
            if (ancacheable(&parts[p.order[j]])) {
                p.mark[j] = 1;
                break;
            }
        }
    }

    // Four at most: the tools' gives way, as the system prompt's
    // breakpoint covers the tools too
    i32 marks = r->system.len ? 1 : 0;
    for (iz k = 0; k < p.mark.len; k++) {
        marks += p.mark[k];
    }
    assert(marks <= 4);
    p.tools = marks < 4;
    return p;
}

static void anmessages(JsonOut *w, Arena scratch, AnPlan *p)
{
    putarray(w);
    for (iz beg = 0, end = 0; beg < p->order.len; beg = end) {
        AnPart *first = &p->parts[p->order[beg]];
        i32     n     = first->n;
        for (end = beg; end<p->order.len && p->parts[p->order[end]].n==n; end++) {}
        putobject(w);
        putstrkey(w, "role", first->asst ? Str("assistant") : Str("user"));
        putkey(w, "content");
        putarray(w);
        for (iz k = beg; k < end; k++) {
            anblock(w, scratch, &p->parts[p->order[k]], p->mark[k]);
        }
        putend(w);
        putend(w);
    }
    putend(w);
}

// Encode a streaming Messages request into a, using tmp for
// temporaries. Keys come in a fixed order, so equal histories give
// equal bytes.
static Str anencode(Arena *a, Arena tmp, ChatReq *r)
{
    ModelConf *mc   = r->mc;
    i64        most = anmaxtokens(mc);

    Buf     b(a, 1<<16);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "model", mc->name);
    putintkey(&w, "max_tokens", most);
    if (r->system.len) {
        putkey(&w, "system");
        putarray(&w);
        putobject(&w);
        putstrkey(&w, "type", "text");
        putstrkey(&w, "text", r->system);
        ancache(&w);
        putend(&w);
        putend(&w);
    }
    AnPlan plan = anplan(&tmp, r);
    putkey(&w, "messages");
    anmessages(&w, tmp, &plan);
    if (r->tools.len) {
        putkey(&w, "tools");
        antools(&w, tmp, r->tools, plan.tools);
        putkey(&w, "tool_choice");
        putobject(&w);
        putstrkey(&w, "type", "auto");
        putend(&w);
    }

    i64 budget = mc->thinkmax ? mc->thinkmax : AN_BUDGET;
    budget = budget<most ? budget : most-1;
    switch (mc->think) {
    case THINK_ADAPTIVE:
        putkey(&w, "thinking");
        putobject(&w);
        putstrkey(&w, "type", "adaptive");
        putstrkey(&w, "display", mc->hidethink ? Str("omitted") : Str("summarized"));
        putend(&w);
        break;
    case THINK_BUDGET:
        if (budget >= AN_MINBUDGET) {
            putkey(&w, "thinking");
            putobject(&w);
            putstrkey(&w, "type", "enabled");
            putintkey(&w, "budget_tokens", budget);
            putend(&w);
        }
        break;
    case THINK_OFF:
        putkey(&w, "thinking");
        putobject(&w);
        putstrkey(&w, "type", "disabled");
        putend(&w);
        break;
    case THINK_BETWEEN:
        // Alone: the API refuses it with any other field
        putkey(&w, "thinking");
        putobject(&w);
        putstrkey(&w, "type", "between_tools");
        putend(&w);
        break;
    }
    if (mc->effort.len) {
        putkey(&w, "output_config");
        putobject(&w);
        putstrkey(&w, "effort", mc->effort);
        putend(&w);
    }
    for (iz i = 0; i < mc->extras.len; i++) {
        // Of the sampling parameters, only these exist here
        Str key = mc->extras[i].key;
        if (key=="temperature" || key=="top_p" || key=="top_k") {
            putkey(&w, key);
            putraw(&w, mc->extras[i].json);
        }
    }
    putkey(&w, "stream");
    putbool(&w, 1);
    putend(&w);
    return finish(&b);
}


// Streamed responses

// An error object of the API as text, "TYPE: MESSAGE", whose type
// classify reads.
static Str anerror(Arena *a, Json *e)
{
    Str type = jsonstr(jsonget(e, "type"));
    Str msg  = e->type==JSON_MISSING ? Str{} : errortext(e);
    if (!msg.len) {
        msg = "error event without a message";
    }
    return type.len ? concat(a, concat(a, type, ": "), msg) : msg;
}

// A stop reason as the engine knows it (finish_reason), in a.
static Str anstop(Arena *a, Str reason)
{
    if (reason=="end_turn" || reason=="stop_sequence") {
        return "stop";
    } else if (reason == "tool_use") {
        return "tool_calls";
    } else if (reason=="max_tokens" || reason=="model_context_window_exceeded") {
        return "length";
    }
    return clone(a, reason);
}

// Update the usage from a usage object. message_start has the prompt's
// counts, message_delta the output's, and maybe the prompt's again.
static void anusage(Partial *p, Json *u)
{
    if (u->type != JSON_OBJECT) {
        return;
    }
    i64 plain = p->usage.in - p->usage.cached - p->usage.written;
    i64 in    = jsonint(jsonget(u, "input_tokens"), -1);
    i64 wrote = jsonint(jsonget(u, "cache_creation_input_tokens"), -1);
    i64 read  = jsonint(jsonget(u, "cache_read_input_tokens"), -1);
    i64 out   = jsonint(jsonget(u, "output_tokens"), -1);
    plain            = in>=0 ? in : plain;
    p->usage.written = wrote>=0 ? wrote : p->usage.written;
    p->usage.cached  = read>=0 ? read : p->usage.cached;
    p->usage.out     = out>=0 ? out : p->usage.out;
    p->usage.in      = plain + p->usage.written + p->usage.cached;
    p->hasusage      = 1;
}

// The block of a content index, made at its first sight.
static PartBlock *anpartblock(Partial *p, i64 index, i32 type)
{
    for (iz i = 0; i < p->parts.len; i++) {
        if (p->parts[i].index == index) {
            return &p->parts[i];
        }
    }
    PartBlock k = {};
    k.index = index;
    k.block = p->nblocks++;
    k.type  = type;
    k.text  = Buf(p->a, 1<<10);
    k.sig   = Buf(p->a, 64);
    p->parts = push(p->a, p->parts, k);
    return &p->parts[p->parts.len-1];
}

static PartCall *ancall(Partial *p, i64 index)
{
    for (iz i = 0; i < p->calls.len; i++) {
        if (p->calls[i].index == index) {
            return &p->calls[i];
        }
    }
    return 0;
}

// Text for a block: visible text and reasoning also gather whole.
static void anappend(Partial *p, Chunk *r, Arena *scratch, PartBlock *k, Str s)
{
    if (!s.len) {
        return;
    }
    print(&k->text, s);
    if (k->type == BLOCK_REDACTED) {
        return;
    }
    b32   think = k->type == BLOCK_THINK;
    Delta d     = {k->block, think ? DELTA_THINK : DELTA_TEXT, s, {}, {}};
    print(think ? &p->think : &p->text, s);
    r->deltas  = push(scratch, r->deltas, d);
    r->content = 1;
}

static void anargs(Chunk *r, Arena *scratch, PartCall *c, Str s)
{
    if (s.len) {
        print(&c->args, s);
        Delta d = {c->block, DELTA_ARGS, s, {}, {}};
        r->deltas  = push(scratch, r->deltas, d);
        r->content = 1;
    }
}

// A block begins: content_block_start, or one of a whole message, whose
// tool input is then complete rather than to follow in deltas.
static void anstart(Partial *p, Chunk *r, Arena *scratch, i64 index, Json *cb,
                    b32 whole)
{
    Str kind = jsonstr(jsonget(cb, "type"));
    if (kind == "tool_use") {
        if (ancall(p, index)) {
            return;  // started already
        }
        PartCall *c = partcall(p, index);
        print(&c->id, jsonstr(jsonget(cb, "id")));
        print(&c->name, jsonstr(jsonget(cb, "name")));
        Delta d = {c->block, DELTA_ARGS, {}, finish(&c->id), finish(&c->name)};
        r->deltas  = push(scratch, r->deltas, d);
        r->content = 1;
        Json *in = jsonget(cb, "input");
        if (whole && in->type!=JSON_MISSING) {
            anargs(r, scratch, c, in->raw);
            c->stopped = 1;
        }
    } else if (kind == "text") {
        PartBlock *k = anpartblock(p, index, BLOCK_TEXT);
        anappend(p, r, scratch, k, jsonstr(jsonget(cb, "text")));
    } else if (kind == "thinking") {
        PartBlock *k = anpartblock(p, index, BLOCK_THINK);
        anappend(p, r, scratch, k, jsonstr(jsonget(cb, "thinking")));
        print(&k->sig, jsonstr(jsonget(cb, "signature")));
    } else if (kind == "redacted_thinking") {
        PartBlock *k = anpartblock(p, index, BLOCK_REDACTED);
        anappend(p, r, scratch, k, jsonstr(jsonget(cb, "data")));
        r->content = 1;
    }
    // Any other kind (server tools, ...) is not ours to send back
}

static void andelta(Partial *p, Chunk *r, Arena *scratch, i64 index, Json *d)
{
    Str kind = jsonstr(jsonget(d, "type"));
    if (kind == "text_delta") {
        PartBlock *k = anpartblock(p, index, BLOCK_TEXT);
        anappend(p, r, scratch, k, jsonstr(jsonget(d, "text")));
    } else if (kind == "thinking_delta") {
        PartBlock *k = anpartblock(p, index, BLOCK_THINK);
        anappend(p, r, scratch, k, jsonstr(jsonget(d, "thinking")));
    } else if (kind == "signature_delta") {
        PartBlock *k = anpartblock(p, index, BLOCK_THINK);
        print(&k->sig, jsonstr(jsonget(d, "signature")));
        r->content = 1;  // it is output, at the end of long reasoning
    } else if (kind == "input_json_delta") {
        PartCall *c = ancall(p, index);
        c = c ? c : partcall(p, index);
        anargs(r, scratch, c, jsonstr(jsonget(d, "partial_json")));
    }
}

// Why the model declined, from stop_details: its category and
// explanation, either of which may be missing.
static Str anrefusal(Arena *a, Json *sd)
{
    Str cat = jsonstr(jsonget(sd, "category"));
    Str why = jsonstr(jsonget(sd, "explanation"));
    Buf b(a, 64 + cat.len + why.len);
    if (cat.len) {
        print(&b, "category ");
        print(&b, cat);
    }
    if (why.len) {
        print(&b, cat.len ? Str(": ") : Str(""));
        print(&b, why);
    }
    return finish(&b);
}

static void anstopped(Partial *p, Json *d, Json *sd)
{
    Str reason = jsonstr(jsonget(d, "stop_reason"));
    if (reason.len) {
        p->finish = anstop(p->a, reason);
    }
    sd = sd->type==JSON_OBJECT ? sd : jsonget(d, "stop_details");
    if (sd->type == JSON_OBJECT) {
        p->refusal = anrefusal(p->a, sd);
    }
}

// Apply one event of the stream (already parsed JSON) to the partial
// response, by the type it holds. A whole message, as a server that
// does not stream answers, is applied as if streamed. Deltas are
// allocated in scratch.
static Chunk anchunk(Partial *p, Json *j, Arena *scratch)
{
    Chunk r = {};
    if (j->type != JSON_OBJECT) {
        r.bad = 1;
        return r;
    }
    p->indexed = 1;

    Str type = jsonstr(jsonget(j, "type"));
    if (type == "error") {
        r.error = anerror(scratch, jsonget(j, "error"));

    } else if (type == "message_start") {
        Json *m  = jsonget(j, "message");
        Str   id = jsonstr(jsonget(m, "id"));
        if (id.len && !p->id.data) {
            p->id = clone(p->a, id);
        }
        anusage(p, jsonget(m, "usage"));

    } else if (type == "content_block_start") {
        i64 index = jsonint(jsonget(j, "index"), -1);
        anstart(p, &r, scratch, index, jsonget(j, "content_block"), 0);

    } else if (type == "content_block_delta") {
        i64 index = jsonint(jsonget(j, "index"), -1);
        andelta(p, &r, scratch, index, jsonget(j, "delta"));

    } else if (type == "content_block_stop") {
        PartCall *c = ancall(p, jsonint(jsonget(j, "index"), -1));
        if (c) {
            c->stopped = 1;
        }

    } else if (type == "message_delta") {
        anstopped(p, jsonget(j, "delta"), jsonget(j, "stop_details"));
        anusage(p, jsonget(j, "usage"));

    } else if (type == "message_stop") {
        p->done = 1;

    } else if (type == "ping") {
        // Reasoning not shown (thinking_display omitted) streams nothing
        // until its signature, at the end: a ping meanwhile, while the
        // newest block is reasoning not yet signed, says it goes on
        PartBlock *k = p->parts.len ? &p->parts[p->parts.len-1] : 0;
        r.hidden = k && k->block==p->nblocks-1 && k->type==BLOCK_THINK &&
                   !k->sig.len && !p->finish.data;

    } else if (type == "message") {
        i64 index = 0;
        for (Json *cb = jsonget(j, "content")->child; cb; cb = cb->next) {
            anstart(p, &r, scratch, index++, cb, 1);
        }
        anstopped(p, j, jsonget(j, "stop_details"));
        anusage(p, jsonget(j, "usage"));
        p->done = 1;
    }
    // Types yet unknown change nothing
    return r;
}

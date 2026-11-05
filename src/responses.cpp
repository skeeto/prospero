// OpenAI's Responses API: requests and streamed responses
//
// Two endpoint kinds speak it. Kind responses takes requests at
// URL/v1/responses with its key as a bearer token: OpenAI's API, or a
// server that serves the same (llama.cpp's llama-server does). Kind
// chatgpt is ChatGPT's Codex backend, signed in with a ChatGPT account
// rather than a key, which takes a narrower request: no sampling
// parameters, and no max_output_tokens.
//
// Requests are encoded from the canonical transcript (provider.cpp) as
// deterministically as chat requests are, so that each one's prefix is
// the previous one's and the prompt cache keeps hitting:
//
// - The system prompt goes in instructions, and the history in input as
//   a flat list of items: user messages, assistant messages, function
//   calls, their outputs, and reasoning, each in the order the model
//   produced it. Nothing is stored at the server (store false), so every
//   request carries the whole conversation, as the transcript does.
// - Reasoning goes back as it came: with its encrypted_content (kept in
//   the block's sig), which OpenAI's models need, and its summary. Item
//   ids are left out, since with nothing stored the API refuses them.
//   Reasoning with no encrypted_content goes back only to llama.cpp,
//   as reasoning_text, when the model's reasoning setting says to send
//   reasoning back at all; OpenAI cannot check it.
// - A tool result's screenshot follows the run of results as a user
//   message, as Chat Completions does, since llama.cpp takes only text
//   in a function_call_output.
// - Tools are flat functions, strict false: OpenAI's strict mode wants
//   schemas the tools do not write.
//
// A response streams as events, each its type in the data: the items of
// the output by output_item.added, their deltas (output_text,
// reasoning_summary_text, reasoning_text, function_call_arguments), and
// output_item.done with each whole, then completed, incomplete or
// failed with the usage. llama.cpp names items by item_id alone, with
// no output_index, so items are numbered as their ids first appear. A
// whole item at its end fills in only what did not stream, so a server
// that sends items only whole is read alike, as is a response not
// streamed at all (object "response").
//
// Everything here is pure: the engine does the I/O.
//
// This is free and unencumbered software released into the public domain.


// Requests

// The tools of a chat request (an array of functions) as the Responses
// API takes them.
static void rstools(JsonOut *w, Arena scratch, Str tools)
{
    JsonDoc d = jsonparse(&scratch, tools);
    putarray(w);
    for (Json *t = d.root->child; d.root->type==JSON_ARRAY && t; t = t->next) {
        Json *fn = jsonget(t, "function");
        putobject(w);
        putstrkey(w, "type", "function");
        putstrkey(w, "name", jsonstr(jsonget(fn, "name")));
        putstrkey(w, "description", jsonstr(jsonget(fn, "description")));
        putkey(w, "parameters");
        putjson(w, jsonget(fn, "parameters"));
        putkey(w, "strict");
        putbool(w, 0);
        putend(w);
    }
    putend(w);
}

static void rstext(JsonOut *w, Str type, Str text)
{
    putobject(w);
    putstrkey(w, "type", type);
    putstrkey(w, "text", text);
    putend(w);
}

// An input_image of base64 data, as a data URL.
static void rsimage(JsonOut *w, Arena scratch, Str mime, Str b64)
{
    putobject(w);
    putstrkey(w, "type", "input_image");
    putkey(w, "image_url");
    putb64(w, scratch, mime, b64);
    putend(w);
}

static void rsuser(JsonOut *w, Arena scratch, Msg *m)
{
    putobject(w);
    putstrkey(w, "type", "message");
    putstrkey(w, "role", "user");
    putkey(w, "content");
    putarray(w);
    if (m->text.len || !m->images.len) {
        rstext(w, "input_text", m->text);
    }
    for (iz i = 0; i < m->images.len; i++) {
        MsgImage *g = &m->images[i];
        if (g->b64.len) {
            rsimage(w, scratch, g->mime, g->b64);
        } else {
            rstext(w, "input_text", imagealt(g));
        }
    }
    putend(w);
    putend(w);
}

// The screenshots of a run of tool results, h[beg, end), as a user
// message after them (encodeimages's, in this API's shape).
static void rsimages(JsonOut *w, Arena tmp, Slice<Msg> h, iz beg, iz end)
{
    b32 any = 0;
    for (iz i = beg; i < end; i++) {
        any |= h[i].imgb64.len>0 || h[i].imgalt.len>0;
    }
    if (!any) {
        return;
    }
    putobject(w);
    putstrkey(w, "type", "message");
    putstrkey(w, "role", "user");
    putkey(w, "content");
    putarray(w);
    for (iz i = beg; i < end; i++) {
        Msg  *m = &h[i];
        Arena t = tmp;
        if (!m->imgb64.len) {
            if (m->imgalt.len) {
                rstext(w, "input_text", m->imgalt);
            }
            continue;
        }
        Str what = m->imgread ? Str("The image read by tool call ")
                              : Str("The screenshot of tool call ");
        rstext(w, "input_text", concat(&t, concat(&t, what, m->callid), ":"));
        rsimage(w, t, "image/png", m->imgb64);
    }
    putend(w);
    putend(w);
}

// Whether reasoning goes back: with its encrypted content, or as text
// to llama.cpp when the model's settings send reasoning back.
static b32 rsthinks(ModelConf *mc, Block *b)
{
    return b->sig.len || (mc->llamacpp && mc->reasoning.len && b->text.len);
}

static void rsassistant(JsonOut *w, Arena scratch, ModelConf *mc, Msg *m)
{
    for (iz i = 0; i < m->blocks.len; i++) {
        Block *b = &m->blocks[i];
        switch (b->type) {
        case BLOCK_THINK:
            if (!rsthinks(mc, b)) {
                break;
            }
            putobject(w);
            putstrkey(w, "type", "reasoning");
            putkey(w, "summary");
            putarray(w);
            if (b->sig.len && b->text.len) {
                rstext(w, "summary_text", b->text);
            }
            putend(w);
            if (b->sig.len) {
                putstrkey(w, "encrypted_content", b->sig);
            } else {
                putkey(w, "content");
                putarray(w);
                rstext(w, "reasoning_text", b->text);
                putend(w);
            }
            putend(w);
            break;
        case BLOCK_TEXT:
            if (!trim(b->text).len) {
                break;
            }
            putobject(w);
            putstrkey(w, "type", "message");
            putstrkey(w, "role", "assistant");
            putkey(w, "content");
            putarray(w);
            rstext(w, "output_text", b->text);
            putend(w);
            putend(w);
            break;
        case BLOCK_CALL:
            putobject(w);
            putstrkey(w, "type", "function_call");
            putstrkey(w, "call_id", b->id);
            putstrkey(w, "name", b->name);
            putstrkey(w, "arguments", wireargs(&scratch, b->args));
            putend(w);
            break;
        }
        // Redacted reasoning is Anthropic's alone
    }
}

static void rsinput(JsonOut *w, Arena tmp, ModelConf *mc, Slice<Msg> h)
{
    putarray(w);
    iz results = -1;  // where the run of tool results began
    for (iz i = 0; i < h.len; i++) {
        Arena t    = tmp;
        Msg  *m    = &h[i];
        b32   tool = m->role == ROLE_TOOL;
        if (!tool && results>=0) {
            rsimages(w, tmp, h, results, i);
            results = -1;
        }
        results = tool && results<0 ? i : results;
        switch (m->role) {
        case ROLE_USER:
            rsuser(w, t, m);
            break;
        case ROLE_ASSISTANT:
            rsassistant(w, t, mc, m);
            break;
        case ROLE_TOOL:
            putobject(w);
            putstrkey(w, "type", "function_call_output");
            putstrkey(w, "call_id", m->callid);
            putstrkey(w, "output", m->text);
            putend(w);
            break;
        }
    }
    if (results >= 0) {
        rsimages(w, tmp, h, results, h.len);
    }
    putend(w);
}

// Encode a streaming Responses request into a, using tmp for
// temporaries. Keys come in a fixed order, so equal histories give equal
// bytes. Kind chatgpt (codex) gets no sampling parameters.
static Str rsencode(Arena *a, Arena tmp, ChatReq *r, b32 codex)
{
    ModelConf *mc = r->mc;
    Buf     b(a, 1<<16);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "model", mc->name);
    putstrkey(&w, "instructions", r->system);
    putkey(&w, "input");
    rsinput(&w, tmp, mc, r->history);
    if (r->tools.len) {
        putkey(&w, "tools");
        rstools(&w, tmp, r->tools);
        putstrkey(&w, "tool_choice", "auto");
        putkey(&w, "parallel_tool_calls");
        putbool(&w, 1);
    }
    if (mc->effort.len || codex) {
        putkey(&w, "reasoning");
        putobject(&w);
        if (mc->effort.len) {
            putstrkey(&w, "effort", mc->effort);
        }
        putstrkey(&w, "summary", "auto");
        putend(&w);
    }
    for (iz i = 0; !codex && i < mc->extras.len; i++) {
        Str key = mc->extras[i].key;
        putkey(&w, key=="max_tokens" ? Str("max_output_tokens") : key);
        putraw(&w, mc->extras[i].json);
    }
    if (!codex && mc->kwargs.len) {
        putkey(&w, "chat_template_kwargs");  // llama.cpp passes it on
        putraw(&w, mc->kwargs);
    }
    if (mc->llamacpp) {
        putkey(&w, "return_progress");  // prefill progress
        putbool(&w, 1);
    }
    putkey(&w, "store");
    putbool(&w, 0);
    putkey(&w, "stream");
    putbool(&w, 1);
    putkey(&w, "include");
    putarray(&w);
    putstr(&w, "reasoning.encrypted_content");
    putend(&w);
    if (r->cachekey.len) {
        putstrkey(&w, "prompt_cache_key", r->cachekey);
    }
    putend(&w);
    return finish(&b);
}


// Streamed responses

// The number of an output item, by its id as first seen, else by its
// output_index n. Items are told apart by id across servers that send
// one or the other.
static i64 rsindexof(Partial *p, Str id, i64 n)
{
    Buf b(p->a, 24);
    if (!id.len) {
        putbyte(&b, '#');
        print(&b, n);
        id = finish(&b);
    }
    for (iz i = 0; i < p->items.len; i++) {
        if (p->items[i] == id) {
            return i;
        }
    }
    p->items = push(p->a, p->items, clone(p->a, id));
    return p->items.len - 1;
}

// The number of an event's item.
static i64 rsindex(Partial *p, Json *j)
{
    Str id = jsonstr(jsonget(j, "item_id"));
    if (!id.len) {
        id = jsonstr(jsonget(jsonget(j, "item"), "id"));
    }
    return rsindexof(p, id, jsonint(jsonget(j, "output_index"), 0));
}

// An error object as text, "CODE: MESSAGE", whose code classify reads.
static Str rserror(Arena *a, Json *e)
{
    Str code = jsonstr(jsonget(e, "code"));
    code = code.len ? code : jsonstr(jsonget(e, "type"));
    Str msg  = e->type==JSON_MISSING ? Str{} : errortext(e);
    if (!msg.len) {
        msg = "error event without a message";
    }
    return code.len && code!="error" ? concat(a, concat(a, code, ": "), msg) : msg;
}

static void rsusage(Partial *p, Json *u)
{
    if (u->type != JSON_OBJECT) {
        return;
    }
    p->usage.in     = jsonint(jsonget(u, "input_tokens"), p->usage.in);
    p->usage.out    = jsonint(jsonget(u, "output_tokens"), p->usage.out);
    p->usage.cached = jsonint(jsonget(jsonget(u, "input_tokens_details"), "cached_tokens"),
                              p->usage.cached);
    p->hasusage     = 1;
}

// The texts of an item's content parts of a type, joined.
static Str rsparts(Arena *a, Json *parts, Str type, Str field)
{
    Buf b(a, 256);
    for (Json *c = parts->child; c; c = c->next) {
        if (jsonstr(jsonget(c, "type")) == type) {
            print(&b, jsonstr(jsonget(c, field)));
        }
    }
    return finish(&b);
}

static void rsrefusal(Partial *p, Str s)
{
    if (s.len) {
        p->refusal = p->refusal.len ? concat(p->a, p->refusal, s) : clone(p->a, s);
    }
}

// A call item begins, or is seen whole: its id and name go out with the
// first delta.
static PartCall *rscall(Partial *p, Chunk *r, Arena *scratch, i64 index, Json *item)
{
    PartCall *c = ancall(p, index);
    if (c) {
        return c;
    }
    c = partcall(p, index);
    print(&c->id, jsonstr(jsonget(item, "call_id")));
    print(&c->name, jsonstr(jsonget(item, "name")));
    Delta d = {c->block, DELTA_ARGS, {}, finish(&c->id), finish(&c->name)};
    r->deltas  = push(scratch, r->deltas, d);
    r->content = 1;
    return c;
}

// An item begins (output_item.added), or is seen whole (done, or in a
// whole response): whole, it fills in what did not stream.
static void rsitem(Partial *p, Chunk *r, Arena *scratch, i64 index, Json *item, b32 whole)
{
    Str type = jsonstr(jsonget(item, "type"));
    if (type == "function_call") {
        PartCall *c = rscall(p, r, scratch, index, item);
        if (whole) {
            if (!c->args.len) {
                anargs(r, scratch, c, jsonstr(jsonget(item, "arguments")));
            }
            c->stopped = 1;
        }
    } else if (type == "message") {
        PartBlock *k = anpartblock(p, index, BLOCK_TEXT);
        if (whole) {
            Json *content = jsonget(item, "content");
            if (!k->text.len) {
                anappend(p, r, scratch, k, rsparts(scratch, content, "output_text", "text"));
            }
            rsrefusal(p, rsparts(scratch, content, "refusal", "refusal"));
        }
    } else if (type == "reasoning") {
        PartBlock *k = anpartblock(p, index, BLOCK_THINK);
        if (whole && !k->text.len) {
            Str t = rsparts(scratch, jsonget(item, "summary"), "summary_text", "text");
            t = t.len ? t : rsparts(scratch, jsonget(item, "content"), "reasoning_text", "text");
            anappend(p, r, scratch, k, t);
        }
        Str enc = jsonstr(jsonget(item, "encrypted_content"));
        if (enc.len && !k->sig.len) {
            print(&k->sig, enc);
            r->content = 1;  // output, at the end of long reasoning
        }
    }
    // Any other kind (web search, ...) is not ours to send back
}

// A response's end: its usage, why it stopped, and any of its items
// that did not stream.
static void rsend(Partial *p, Chunk *r, Arena *scratch, Json *resp)
{
    i64 n = 0;
    for (Json *item = jsonget(resp, "output")->child; item; item = item->next, n++) {
        i64 index = rsindexof(p, jsonstr(jsonget(item, "id")), n);
        rsitem(p, r, scratch, index, item, 1);
    }
    rsusage(p, jsonget(resp, "usage"));
    Str status = jsonstr(jsonget(resp, "status"));
    Str why    = jsonstr(jsonget(jsonget(resp, "incomplete_details"), "reason"));
    if (status=="incomplete" && why=="content_filter") {
        rsrefusal(p, p->refusal.len ? Str{} : Str("content filter"));
        p->finish = "refusal";
    } else if (status == "incomplete") {
        p->finish = "length";
    } else if (p->refusal.len) {
        p->finish = "refusal";
    } else {
        p->finish = p->calls.len ? Str("tool_calls") : Str("stop");
    }
    p->done = 1;
}

// Apply one event of the stream (already parsed JSON) to the partial
// response, by the type it holds. A whole response, as a server that
// does not stream answers, is applied as if streamed. Deltas are
// allocated in scratch.
static Chunk rschunk(Partial *p, Json *j, Arena *scratch)
{
    Chunk r = {};
    if (j->type != JSON_OBJECT) {
        r.bad = 1;
        return r;
    }
    p->indexed = 1;

    // llama.cpp's prefill progress rides on events of any type
    Json *pp = jsonget(j, "prompt_progress");
    if (pp->type == JSON_OBJECT) {
        r.prefill = 1;
        r.ptotal  = jsonint(jsonget(pp, "total"), 0);
        r.pdone   = jsonint(jsonget(pp, "processed"), 0);
        r.pcached = jsonint(jsonget(pp, "cache"), 0);
        r.pms     = jsonint(jsonget(pp, "time_ms"), -1);
    }

    Str type = jsonstr(jsonget(j, "type"));
    if (type == "error") {
        Json *e = jsonget(j, "error");
        r.error = rserror(scratch, e->type==JSON_OBJECT ? e : j);

    } else if (type=="response.created" || type=="response.in_progress") {
        Str id = jsonstr(jsonget(jsonget(j, "response"), "id"));
        if (id.len && !p->id.data) {
            p->id = clone(p->a, id);
        }

    } else if (type=="response.output_item.added" || type=="response.output_item.done") {
        b32 whole = type == "response.output_item.done";
        rsitem(p, &r, scratch, rsindex(p, j), jsonget(j, "item"), whole);

    } else if (type == "response.output_text.delta") {
        PartBlock *k = anpartblock(p, rsindex(p, j), BLOCK_TEXT);
        anappend(p, &r, scratch, k, jsonstr(jsonget(j, "delta")));

    } else if (type == "response.refusal.delta") {
        rsrefusal(p, jsonstr(jsonget(j, "delta")));

    } else if (type == "response.reasoning_summary_part.added") {
        // Summaries after the first begin a paragraph of their own
        PartBlock *k = anpartblock(p, rsindex(p, j), BLOCK_THINK);
        if (k->text.len) {
            anappend(p, &r, scratch, k, "\n\n");
        }

    } else if (type=="response.reasoning_summary_text.delta" ||
               type=="response.reasoning_text.delta") {
        PartBlock *k = anpartblock(p, rsindex(p, j), BLOCK_THINK);
        anappend(p, &r, scratch, k, jsonstr(jsonget(j, "delta")));

    } else if (type == "response.function_call_arguments.delta") {
        i64       index = rsindex(p, j);
        PartCall *c     = ancall(p, index);
        c = c ? c : partcall(p, index);
        anargs(&r, scratch, c, jsonstr(jsonget(j, "delta")));

    } else if (type=="response.completed" || type=="response.incomplete") {
        rsend(p, &r, scratch, jsonget(j, "response"));

    } else if (type == "response.failed") {
        Json *e = jsonget(jsonget(j, "response"), "error");
        r.error = rserror(scratch, e);

    } else if (!type.len && jsonstr(jsonget(j, "object"))=="response") {
        Json *e = jsonget(j, "error");
        if (e->type == JSON_OBJECT) {
            r.error = rserror(scratch, e);
        } else {
            rsend(p, &r, scratch, j);
        }
    }
    // Types yet unknown change nothing
    return r;
}


// ChatGPT
//
// Kind chatgpt signs in as the Codex CLI does, with its public client
// (engine.cpp, "Signing in"), and sends each request with the access
// token, the account's id, and the Codex headers. Its model listing is
// the Codex backend's own shape, and a usage limit is an error of its
// own, which waits hours rather than seconds.

static Str const chatgptissuer     = "https://auth.openai.com";
static Str const chatgptclient     = "app_EMoamEEZ73f0CkXaXp7hrann";
static Str const chatgptscope      = "openid profile email offline_access";
static Str const chatgptoriginator = "prospero";

// The Codex backend's listing, {models: [{slug, context_window}]}, as
// a /v1/models listing: {data: [{id, max_input_tokens}]}. Any other
// body is returned as it is. tmp is another arena than a's.
static Str rslisting(Arena *a, Arena tmp, Str body)
{
    Json *ms = jsonget(jsonparse(&tmp, body).root, "models");
    if (ms->type != JSON_ARRAY) {
        return body;
    }
    Buf     b(a, 64 + body.len/4);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "object", "list");
    putkey(&w, "data");
    putarray(&w);
    for (Json *m = ms->child; m; m = m->next) {
        Str id  = jsonstr(jsonget(m, "slug"));
        i64 ctx = jsonint(jsonget(m, "context_window"), 0);
        if (!id.len) {
            continue;
        }
        putobject(&w);
        putstrkey(&w, "id", id);
        putstrkey(&w, "object", "model");
        if (ctx > 0) {
            putintkey(&w, "max_input_tokens", ctx);
        }
        putend(&w);
    }
    putend(&w);
    putend(&w);
    return finish(&b);
}

// An error response that says ChatGPT's usage limit was reached: the
// seconds until it resets, 0 if it does not say, else -1 if it is not
// one. now is Unix seconds.
static i64 rsusagelimit(Arena scratch, Str body, i64 now)
{
    Json *e    = jsonget(jsonparse(&scratch, body).root, "error");
    Str   type = jsonstr(jsonget(e, "type"));
    Str   code = jsonstr(jsonget(e, "code"));
    if (type!="usage_limit_reached" && code!="usage_limit_reached" &&
            type!="usage_not_included" && code!="usage_not_included") {
        return -1;
    }
    i64 in = jsonint(jsonget(e, "resets_in_seconds"), 0);
    i64 at = jsonint(jsonget(e, "resets_at"), 0);
    return in>0 ? in : at>now ? at-now : 0;
}

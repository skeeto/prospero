// The built-in model's chat (builtin.cpp): a Chat Completions request in
// Gemma 4's chat template, the model's output as Chat Completions SSE,
// sampling, and a cache of sequences shared by conversations
//
// The template follows Gemma 4's own (chat_template.jinja), as gemma4.c's
// server renders it: the system text, the thinking switch and the tools'
// declarations in a system turn, then the turns, the model's tool calls
// and their results in its own turn, in control tokens of their own:
//
//   <|turn>system\n<|think|>\nSYSTEM<|tool>declaration:NAME{...}<tool|><turn|>\n
//   <|turn>user\nTEXT<turn|>\n
//   <|turn>model\n<|channel>thought\n...<channel|><|tool_call>call:NAME{k:v}<tool_call|>
//   <|tool_response>response:NAME{value:<|"|>...<|"|>}<tool_response|>TEXT<turn|>\n
//
// Strings in that syntax are delimited by the <|"|> token, not quoted.
// A message's text is never markup: the prompt carries a mask of where a
// control token may start (gmencode), so a file a tool read that holds
// "<|turn>" is only text.
//
// The cache keeps a sequence for each session that called (by the
// call's session), as llama.cpp's unified cache keeps slots: its tokens,
// its keys and values, and checkpoints. A sliding layer's ring holds
// only its newest GM_RING positions, so going back more than about a
// window (GM_WINDOW) needs a checkpoint, a copy of the rings at that
// position. One is taken where the system turn ends, and where a prompt
// ends with the user's message: the template drops a turn's thoughts
// once the next user message follows, and the prompt goes back to there.
// A new session starts from the longest prefix any sequence can give.
//
// Like gemma.cpp, this file is under gemma4.c's MIT license, as its end
// says.

static Str const gmmodelid = "gemma-4-e2b";
static Str const gmquote   = "<|\"|>";

enum {
    GM_CONTEXT  = 65536,    // the context listed, unless [model] context lowers it
    GM_MAXNEW   = 4096,     // a response's tokens, unless the request says
    GM_TOPK     = 64,       // sampling's candidates at most
    GM_CHECKS   = 4,        // checkpoints a sequence keeps
    GM_SEQS     = 8,        // sequences the cache keeps
    GM_SIDMAX   = 64,
    GM_HELDMAX  = 1<<20,    // a tool call's text, at most
    GM_PINGUS   = 5000000,  // silence before a keep-alive, in µs
};

// Control tokens, by their text in the file's vocabulary
struct GmIds {
    i32 turn;        // <|turn>
    i32 turnend;     // <turn|>
    i32 call;        // <|tool_call>
    i32 callend;     // <tool_call|>
    i32 response;    // <|tool_response>
    i32 quote;       // <|"|>
    i32 channel;     // <|channel>
    i32 channelend;  // <channel|>
    b32 ok;
};

static i32 gmspecial(GmTok *t, Str text)
{
    for (i32 i = 0; i < t->nspecials; i++) {
        if (t->specials[i].text == text) {
            return t->specials[i].id;
        }
    }
    return -1;
}

static GmIds gmids(GmTok *t)
{
    GmIds r = {};
    r.turn       = gmspecial(t, "<|turn>");
    r.turnend    = gmspecial(t, "<turn|>");
    r.call       = gmspecial(t, "<|tool_call>");
    r.callend    = gmspecial(t, "<tool_call|>");
    r.response   = gmspecial(t, "<|tool_response>");
    r.quote      = gmspecial(t, gmquote);
    r.channel    = gmspecial(t, "<|channel>");
    r.channelend = gmspecial(t, "<channel|>");
    r.ok = r.turn>=0 && r.turnend>=0 && r.call>=0 && r.callend>=0 &&
           r.response>=0 && r.quote>=0 && r.channel>=0 && r.channelend>=0;
    return r;
}


// The prompt

// The prompt's text, and beside it a mask of the bytes where a control
// token may start: the template's own markup.
struct GmPrompt {
    u8  *text;
    u8  *mask;
    iz   len;
    iz   cap;
    b32  full;  // out of room
};

static void gmput(GmPrompt *p, Str s, u8 markup)
{
    assert(s.len >= 0);
    if (s.len > p->cap-p->len) {
        p->full = 1;
        return;
    }
    copybytes(p->text+p->len, s.data, s.len);
    for (iz i = 0; i < s.len; i++) {
        p->mask[p->len+i] = markup;
    }
    p->len += s.len;
}

static void gmsyn(GmPrompt *p, Str s) { gmput(p, s, 1); }
static void gmraw(GmPrompt *p, Str s) { gmput(p, s, 0); }

static b32 gmspace(u8 c)
{
    return c==' ' || c=='\t' || c=='\r' || c=='\n';
}

// Without surrounding whitespace, as Jinja's trim.
static Str gmtrim(Str s)
{
    for (; s.len && gmspace(s[0]); s = cuthead(s, 1)) {}
    for (; s.len && gmspace(s[s.len-1]); s.len--) {}
    return s;
}

static void gmquoted(GmPrompt *p, Str s)
{
    gmsyn(p, gmquote);
    gmraw(p, s);
    gmsyn(p, gmquote);
}

// A message's text: a string, or the text of an array of parts. Images
// and other parts are dropped.
static Str gmcontent(Arena *a, Json *v)
{
    if (v->type == JSON_STRING) {
        return v->str;
    }
    Str r = {};
    for (Json *part = v->type==JSON_ARRAY ? v->child : 0; part; part = part->next) {
        Json *type = jsonget(part, "type");
        if (type->type==JSON_MISSING || jsonstr(type)=="text") {
            r = concat(a, r, jsonstr(jsonget(part, "text")));
        }
    }
    return r;
}

static b32 gmkeyless(Json **a, Json **b)
{
    Str x = (*a)->key, y = (*b)->key;
    iz  n = x.len<y.len ? x.len : y.len;
    i32 c = n ? __builtin_memcmp(x.data, y.data, (uz)n) : 0;
    return c<0 || (!c && x.len<y.len);
}

// An object's members, in order, or sorted by name (the template's
// dictsort).
static Slice<Json *> gmmembers(Arena *a, Json *obj, b32 sorted)
{
    Slice<Json *> r = {};
    if (obj->type != JSON_OBJECT) {
        return r;
    }
    r.data = alloc<Json *>(a, obj->len);
    r.cap  = obj->len;
    for (Json *m = obj->child; m; m = m->next) {
        r.data[r.len++] = m;
    }
    if (sorted) {
        sort(r.data, r.len, gmkeyless, *a);
    }
    return r;
}

// A JSON value in the template's syntax (its format_argument macro).
static void gmvalue(GmPrompt *p, Arena scratch, Json *v, b32 esckeys, b32 sorted)
{
    switch (v->type) {
    case JSON_STRING:
        gmquoted(p, v->str);
        break;
    case JSON_OBJECT: {
        Slice<Json *> m = gmmembers(&scratch, v, sorted);
        gmsyn(p, "{");
        for (iz i = 0; i < m.len; i++) {
            if (i) gmsyn(p, ",");
            if (esckeys) {
                gmquoted(p, m[i]->key);
            } else {
                gmraw(p, m[i]->key);
            }
            gmsyn(p, ":");
            gmvalue(p, scratch, m[i], esckeys, sorted);
        }
        gmsyn(p, "}");
    } break;
    case JSON_ARRAY:
        gmsyn(p, "[");
        for (Json *e = v->child; e; e = e->next) {
            if (e != v->child) gmsyn(p, ",");
            gmvalue(p, scratch, e, esckeys, sorted);
        }
        gmsyn(p, "]");
        break;
    case JSON_BOOL:
        gmraw(p, v->boolean ? Str("true") : Str("false"));
        break;
    case JSON_NULL:
        gmraw(p, "null");
        break;
    case JSON_NUMBER:
        gmraw(p, v->raw);
        break;
    }
}

// A schema's type, upper-cased; of a list of types, the first not null.
static Str gmtype(Arena *a, Json *type)
{
    if (type->type == JSON_ARRAY) {
        Json *t = type->child;
        for (; t && jsonstr(t)=="null"; t = t->next) {}
        type = t ? t : type->child;
    }
    Str r = type ? clone(a, jsonstr(type)) : Str{};
    for (iz i = 0; i < r.len; i++) {
        r[i] = r[i]>='a' && r[i]<='z' ? (u8)(r[i] - 32) : r[i];
    }
    return r;
}

static b32 gmhas(Json *array)
{
    return array->type==JSON_ARRAY && array->child;
}

// A comma before all but the first of a property's fields.
static void gmsep(GmPrompt *p, b32 *comma)
{
    if (*comma) gmsyn(p, ",");
    *comma = 1;
}

// A schema's properties (the template's format_parameters macro).
static void gmparams(GmPrompt *p, Arena scratch, Json *props)
{
    Slice<Json *> m = gmmembers(&scratch, props, 1);
    for (iz i = 0; i < m.len; i++) {
        Json *v     = m[i];
        b32   comma = 0;
        if (i) gmsyn(p, ",");
        gmraw(p, v->key);
        gmsyn(p, ":{");
        Json *desc = jsonget(v, "description");
        if (desc->type == JSON_STRING) {
            gmsyn(p, "description:");
            gmquoted(p, desc->str);
            comma = 1;
        }
        Str type = gmtype(&scratch, jsonget(v, "type"));
        if (type == "STRING") {
            Json *options = jsonget(v, "enum");
            if (gmhas(options)) {
                gmsep(p, &comma);
                gmsyn(p, "enum:");
                gmvalue(p, scratch, options, 1, 1);
            }
        } else if (type == "ARRAY") {
            Slice<Json *> items = gmmembers(&scratch, jsonget(v, "items"), 1);
            if (items.len) {
                gmsep(p, &comma);
                gmsyn(p, "items:{");
                b32 first = 1;
                for (iz k = 0; k < items.len; k++) {
                    Json *item = items[k];
                    if (item->type == JSON_NULL) {
                        continue;
                    }
                    if (!first) gmsyn(p, ",");
                    first = 0;
                    if (item->key == "properties") {
                        gmsyn(p, "properties:{");
                        gmparams(p, scratch, item);
                        gmsyn(p, "}");
                    } else if (item->key == "type") {
                        gmsyn(p, "type:");
                        gmquoted(p, gmtype(&scratch, item));
                    } else {
                        gmraw(p, item->key);
                        gmsyn(p, ":");
                        gmvalue(p, scratch, item, 1, 1);
                    }
                }
                gmsyn(p, "}");
            }
        }
        Json *nullable = jsonget(v, "nullable");
        if (nullable->type==JSON_BOOL && nullable->boolean) {
            gmsep(p, &comma);
            gmsyn(p, "nullable:true");
        }
        if (type == "OBJECT") {
            Json *nested = jsonget(v, "properties");
            if (nested->type == JSON_OBJECT) {
                gmsep(p, &comma);
                gmsyn(p, "properties:{");
                gmparams(p, scratch, nested);
                gmsyn(p, "}");
            }
            Json *required = jsonget(v, "required");
            if (gmhas(required)) {
                gmsep(p, &comma);
                gmsyn(p, "required:");
                gmvalue(p, scratch, required, 1, 0);
            }
        }
        gmsep(p, &comma);
        gmsyn(p, "type:");
        gmquoted(p, type);
        gmsyn(p, "}");
    }
}

// A tool's declaration (the template's format_function_declaration).
static void gmtool(GmPrompt *p, Arena scratch, Json *tool)
{
    Json *fn = jsonget(tool, "function");
    if (fn->type != JSON_OBJECT) {
        return;
    }
    gmsyn(p, "<|tool>declaration:");
    gmraw(p, jsonstr(jsonget(fn, "name")));
    gmsyn(p, "{description:");
    gmquoted(p, jsonstr(jsonget(fn, "description")));
    Json *params = jsonget(fn, "parameters");
    if (params->type == JSON_OBJECT) {
        gmsyn(p, ",parameters:{");
        Json *props = jsonget(params, "properties");
        if (props->type==JSON_OBJECT && props->child) {
            gmsyn(p, "properties:{");
            gmparams(p, scratch, props);
            gmsyn(p, "},");
        }
        Json *required = jsonget(params, "required");
        if (gmhas(required)) {
            gmsyn(p, "required:");
            gmvalue(p, scratch, required, 1, 0);
            gmsyn(p, ",");
        }
        Str type = gmtype(&scratch, jsonget(params, "type"));
        gmsyn(p, "type:");
        gmquoted(p, type.len ? type : Str("OBJECT"));
        gmsyn(p, "}");
    }
    gmsyn(p, "}<tool|>");
}

// Whether the request asks for thinking: chat_template_kwargs'
// enable_thinking if given, else any reasoning_effort but none.
static b32 gmthinking(Json *req)
{
    Json *on = jsonget(jsonget(req, "chat_template_kwargs"), "enable_thinking");
    if (on->type == JSON_BOOL) {
        return on->boolean;
    }
    Json *effort = jsonget(req, "reasoning_effort");
    return effort->type==JSON_STRING && effort->str!="none";
}

static b32 gmrole(Json *msg, Str role)
{
    return jsonstr(jsonget(msg, "role")) == role;
}

struct GmRender {
    GmPrompt p;
    b32      system;     // it has a system turn
    b32      userlast;   // it ends with the user's message
    b32      inthought;  // the reply starts inside a thought
    Str      err;
};

// A request's conversation in the template, in at most cap bytes, the
// prompt in perm and the rest of perm for scratch.
static GmRender gmrender(Arena *perm, Json *req, b32 thinking, iz cap)
{
    GmRender r = {};
    r.p.text = allocbytes(perm, cap);
    r.p.mask = allocbytes(perm, cap);
    r.p.cap  = cap;
    GmPrompt *p = &r.p;
    Arena scratch = *perm;

    Json *list = jsonget(req, "messages");
    iz    n    = list->type==JSON_ARRAY ? list->len : 0;
    if (!n) {
        r.err = "request has no messages";
        return r;
    }
    Json **msgs = alloc<Json *>(&scratch, n);
    {
        iz i = 0;
        for (Json *m = list->child; m; m = m->next) {
            msgs[i++] = m;
        }
    }

    gmsyn(p, "<bos>");
    Json *tools = jsonget(req, "tools");
    iz    start = gmrole(msgs[0], "system") || gmrole(msgs[0], "developer");
    if (thinking || gmhas(tools) || start) {
        r.system = 1;
        gmsyn(p, "<|turn>system\n");
        if (thinking) {
            gmsyn(p, "<|think|>\n");
        }
        if (start) {
            Arena t = scratch;
            gmraw(p, gmtrim(gmcontent(&t, jsonget(msgs[0], "content"))));
        }
        for (Json *tool = tools->type==JSON_ARRAY ? tools->child : 0; tool; tool = tool->next) {
            gmtool(p, scratch, tool);
        }
        gmsyn(p, "<turn|>\n");
    }

    iz lastuser = -1;
    for (iz i = start; i < n; i++) {
        lastuser = gmrole(msgs[i], "user") ? i : lastuser;
    }

    b32 prevmodel = 0, open = 0;
    for (iz i = start; i < n; i++) {
        Arena t   = scratch;
        Json *msg = msgs[i];
        if (gmrole(msg, "tool")) {
            continue;  // with the call that asked for it
        }
        if (!gmrole(msg, "assistant")) {
            gmsyn(p, gmrole(msg, "user") ? Str("<|turn>user\n") : Str("<|turn>system\n"));
            gmraw(p, gmtrim(gmcontent(&t, jsonget(msg, "content"))));
            gmsyn(p, "<turn|>\n");
            prevmodel = 0;
            continue;
        }

        if (!prevmodel) {
            gmsyn(p, "<|turn>model\n");
        }
        prevmodel = 1;
        // Thoughts are kept only for the replies since the last user message
        Json *thought = jsonget(msg, "reasoning_content");
        thought = thought->type==JSON_STRING ? thought : jsonget(msg, "reasoning");
        Str   th      = gmtrim(jsonstr(thought));
        if (i>lastuser && th.len) {
            gmsyn(p, "<|channel>thought\n");
            gmraw(p, th);
            gmsyn(p, "\n<channel|>");
        }
        // Unlike the template, text goes ahead of the calls, where the
        // model wrote it, so the cache matches what it generated
        gmraw(p, gmtrim(gmcontent(&t, jsonget(msg, "content"))));

        Json *calls = jsonget(msg, "tool_calls");
        b32   any   = gmhas(calls);
        for (Json *c = any ? calls->child : 0; c; c = c->next) {
            Json   *fn   = jsonget(c, "function");
            Arena   u    = t;
            JsonDoc args = jsonparse(&u, jsonstr(jsonget(fn, "arguments")));
            gmsyn(p, "<|tool_call>call:");
            gmraw(p, jsonstr(jsonget(fn, "name")));
            if (args.root->type == JSON_OBJECT) {
                gmvalue(p, u, args.root, 0, 0);
            } else {
                gmsyn(p, "{}");
            }
            gmsyn(p, "<tool_call|>");
        }

        b32 responses = 0;
        iz  next      = i + 1;
        for (; next<n && gmrole(msgs[next], "tool"); next++) {
            // The tool's name, by the id of the call it answers
            Str id   = jsonstr(jsonget(msgs[next], "tool_call_id"));
            Str name = "unknown";
            for (Json *c = any ? calls->child : 0; c; c = c->next) {
                if (id.len && jsonstr(jsonget(c, "id"))==id) {
                    name = jsonstr(jsonget(jsonget(c, "function"), "name"));
                }
            }
            gmsyn(p, "<|tool_response>response:");
            gmraw(p, name);
            gmsyn(p, "{value:");
            gmquoted(p, gmcontent(&t, jsonget(msgs[next], "content")));
            gmsyn(p, "}<tool_response|>");
            responses = 1;
        }
        i = next - 1;

        // The turn stays open if the reply goes on: an assistant message
        // follows, or the reply follows the tool results
        open = 0;
        if (next<n && gmrole(msgs[next], "assistant") && (!any || responses)) {
            // the next message goes on in this turn
        } else if (responses && next==n) {
            open = 1;
        } else if (any && !responses) {
            gmsyn(p, "<|tool_response>");
        } else {
            gmsyn(p, "<turn|>\n");
            prevmodel = 0;
        }
    }

    r.userlast = gmrole(msgs[n-1], "user");
    if (!open) {
        gmsyn(p, "<|turn>model\n");
    } else if (thinking) {
        gmsyn(p, "<|channel>thought\n");
        r.inthought = 1;
    }
    r.err = p->full ? Str("request too large") : Str{};
    return r;
}


// Tool calls in the model's output

static Str gmws(Str s)
{
    for (; s.len && gmspace(s[0]); s = cuthead(s, 1)) {}
    return s;
}

// Whether lit, a bare value, is a JSON literal as written.
static b32 gmliteral(Arena scratch, Str lit)
{
    if (lit=="true" || lit=="false" || lit=="null") {
        return 1;
    }
    JsonDoc d = jsonparse(&scratch, lit);
    return lit.len && d.ok && d.root->type==JSON_NUMBER;
}

static i32 gmunfinished(Str);

// One value of the call syntax at *s, as JSON into out, advancing *s.
static b32 gmcallvalue(Str *s, Buf *out, i32 depth)
{
    Str c = gmws(*s);
    if (startswith(c, gmquote)) {
        Str rest = cuthead(c, gmquote.len);
        iz  end  = find(rest, gmquote);
        if (end < 0) {
            return 0;
        }
        printjsonstr(out, takehead(rest, end));
        *s = cuthead(rest, end+gmquote.len);
        return 1;
    }
    if (c.len && (c[0]=='{' || c[0]=='[')) {
        b32 object = c[0] == '{';
        u8  close  = object ? '}' : ']';
        if (depth >= JSON_MAXDEPTH) {
            return 0;
        }
        putbyte(out, c[0]);
        c = gmws(cuthead(c, 1));
        for (b32 first = 1; !(c.len && c[0]==close); first = 0) {
            if (!first) {
                if (!c.len || c[0]!=',') {
                    return 0;
                }
                c = gmws(cuthead(c, 1));
                putbyte(out, ',');
            }
            if (object) {
                Str key = {};
                if (startswith(c, gmquote)) {
                    Str rest = cuthead(c, gmquote.len);
                    iz  end  = find(rest, gmquote);
                    if (end < 0) {
                        return 0;
                    }
                    key = takehead(rest, end);
                    c   = cuthead(rest, end+gmquote.len);
                } else {
                    iz colon = find(c, ":");
                    if (colon < 0) {
                        return 0;
                    }
                    key = takehead(c, colon);
                    for (; key.len && gmspace(key[key.len-1]); key.len--) {}
                    c = cuthead(c, colon);
                }
                c = gmws(c);
                if (!c.len || c[0]!=':') {
                    return 0;
                }
                c = cuthead(c, 1);
                printjsonstr(out, key);
                putbyte(out, ':');
            }
            if (!gmcallvalue(&c, out, depth+1)) {
                return 0;
            }
            c = gmws(c);
        }
        putbyte(out, close);
        *s = cuthead(c, 1);
        return 1;
    }
    // A bare value: kept if a JSON literal, else a string
    iz end = 0;
    for (; end<c.len && c[end]!=',' && c[end]!='}' && c[end]!=']'; end++) {}
    Str lit = takehead(c, end);
    for (; lit.len && gmspace(lit[lit.len-1]); lit.len--) {}
    Arena t = *out->a;
    if (gmliteral(t, lit)) {
        print(out, lit);
    } else {
        printjsonstr(out, lit);
    }
    *s = cuthead(c, end);
    return 1;
}

struct GmCall {
    Str name;
    Str args;  // a JSON object
    b32 ok;
};

// call:NAME{key:value,...} as a name and its arguments.
static GmCall gmparsecall(Arena *a, Str text)
{
    GmCall r = {};
    text = gmws(text);
    if (!startswith(text, "call:")) {
        return r;
    }
    text = cuthead(text, 5);
    iz brace = find(text, "{");
    if (brace<=0 || brace>128) {
        return r;
    }
    r.name = takehead(text, brace);
    Str c  = cuthead(text, brace);
    Buf b(a, 2*c.len + 16);
    if (!gmcallvalue(&c, &b, 0) || gmws(c).len) {
        return r;
    }
    r.args = finish(&b);
    r.ok   = 1;
    return r;
}

// As gmcallvalue, for a call still being written: what is settled of
// its JSON so far, a prefix of what the whole makes, into out. Returns 1
// if the value ended, 0 if more may change it (nothing past that goes).
// An open string goes as far as its last whole character; a key or a
// bare value only once it ends.
static b32 gmcallprefix(Str *s, Buf *out, i32 depth)
{
    Str c = gmws(*s);
    if (startswith(c, gmquote)) {
        Str rest = cuthead(c, gmquote.len);
        iz  end  = find(rest, gmquote);
        if (end < 0) {
            // Held back: a delimiter's start, and a character's
            iz keep = 0;
            for (iz k = gmquote.len-1; k>0 && !keep; k--) {
                keep = endswith(rest, takehead(gmquote, k)) ? k : 0;
            }
            Str body = takehead(rest, rest.len-keep);
            body = takehead(body, body.len-gmunfinished(body));
            printjsonstr(out, body);
            out->len--;  // still open: its quote yet to come
            return 0;
        }
        printjsonstr(out, takehead(rest, end));
        *s = cuthead(rest, end+gmquote.len);
        return 1;
    }
    if (c.len && (c[0]=='{' || c[0]=='[')) {
        b32 object = c[0] == '{';
        u8  close  = object ? '}' : ']';
        if (depth >= JSON_MAXDEPTH) {
            return 0;
        }
        putbyte(out, c[0]);
        c = gmws(cuthead(c, 1));
        for (b32 first = 1;; first = 0) {
            if (!c.len) {
                return 0;
            } else if (c[0] == close) {
                putbyte(out, close);
                *s = cuthead(c, 1);
                return 1;
            } else if (!first) {
                if (c[0] != ',') {
                    return 0;
                }
                c = gmws(cuthead(c, 1));
                putbyte(out, ',');
            }
            if (object) {
                Str key = {};
                if (startswith(c, gmquote)) {
                    Str rest = cuthead(c, gmquote.len);
                    iz  end  = find(rest, gmquote);
                    if (end < 0) {
                        return 0;
                    }
                    key = takehead(rest, end);
                    c   = cuthead(rest, end+gmquote.len);
                } else {
                    iz colon = find(c, ":");
                    if (colon < 0) {
                        return 0;
                    }
                    key = takehead(c, colon);
                    for (; key.len && gmspace(key[key.len-1]); key.len--) {}
                    c = cuthead(c, colon);
                }
                c = gmws(c);
                if (!c.len || c[0]!=':') {
                    return 0;
                }
                c = cuthead(c, 1);
                printjsonstr(out, key);
                putbyte(out, ':');
            }
            if (!gmcallprefix(&c, out, depth+1)) {
                return 0;
            }
            c = gmws(c);
        }
    }
    // A bare value: once it ends, kept if a JSON literal, else a string
    iz end = 0;
    for (; end<c.len && c[end]!=',' && c[end]!='}' && c[end]!=']'; end++) {}
    if (end == c.len) {
        return 0;
    }
    Str lit = takehead(c, end);
    for (; lit.len && gmspace(lit[lit.len-1]); lit.len--) {}
    Arena t = *out->a;
    if (gmliteral(t, lit)) {
        print(out, lit);
    } else {
        printjsonstr(out, lit);
    }
    *s = cuthead(c, end);
    return 1;
}


// The response, as Chat Completions SSE

enum { GM_CONTENT, GM_REASONING };

struct GmStream {
    GmTok     *tok;
    GmIds      ids;
    Slice<Str> heads;    // call:NAME{ for each tool offered
    Os        *os;
    void     (*emit)(void *, Str);
    void      *ctx;
    Str        callid;   // unique to the response, before each call's index
    i64        lastsent;
    u8         pend[4];  // an unfinished UTF-8 sequence, held back
    i32        npend;
    i32        pendfield;
    u8        *held;     // a channel's label, or a tool call, so far
    iz         nheld;
    i32        thought;  // in a thought channel
    i32        label;    // reading its label, "thought\n"
    i32        call;     // in a tool call
    i32        calls;    // tool calls sent, or begun
    i32        callsent; // this call's name went: its arguments stream
    iz         argsent;  // bytes of its arguments sent
};

static GmStream gmstream(Arena *perm, GmTok *tok, GmIds ids, Json *req, Os *os,
                         void (*emit)(void *, Str), void *ctx, Str callid)
{
    GmStream s = {};
    s.tok    = tok;
    s.ids    = ids;
    s.os     = os;
    s.emit   = emit;
    s.ctx    = ctx;
    s.callid = clone(perm, callid);
    s.held   = allocbytes(perm, GM_HELDMAX);
    Json *tools = jsonget(req, "tools");
    for (Json *t = tools->type==JSON_ARRAY ? tools->child : 0; t; t = t->next) {
        Str name = jsonstr(jsonget(jsonget(t, "function"), "name"));
        if (name.len && s.heads.len<256) {
            Str head = concat(perm, concat(perm, "call:", name), "{");
            s.heads  = push(perm, s.heads, head);
        }
    }
    s.lastsent = os ? os_ticks(os) : 0;
    return s;
}

static void gmsend(GmStream *s, Str bytes)
{
    s->emit(s->ctx, bytes);
    s->lastsent = s->os ? os_ticks(s->os) : 0;
}

// A chunk's opening, up to its choice's delta.
static JsonOut gmchunkopen(Buf *b)
{
    print(b, "data: ");
    JsonOut w = {b};
    putobject(&w);
    putstrkey(&w, "id", "chatcmpl-builtin");
    putstrkey(&w, "object", "chat.completion.chunk");
    putintkey(&w, "created", 0);
    putstrkey(&w, "model", gmmodelid);
    return w;
}

static void gmtext(GmStream *s, Arena scratch, i32 field, Str text)
{
    if (!text.len) {
        return;
    }
    Buf     b(&scratch, 256 + 2*text.len);
    JsonOut w = gmchunkopen(&b);
    putkey(&w, "choices");
    putarray(&w);
    putobject(&w);
    putintkey(&w, "index", 0);
    putkey(&w, "delta");
    putobject(&w);
    putstrkey(&w, field==GM_REASONING ? Str("reasoning_content") : Str("content"), text);
    putend(&w);
    putkey(&w, "finish_reason");
    putnull(&w);
    putend(&w);
    putend(&w);
    putend(&w);
    print(&b, "\n\n");
    gmsend(s, finish(&b));
}

// The number of bytes at the end of s in an unfinished UTF-8 sequence.
static i32 gmunfinished(Str s)
{
    for (i32 i = 1; i<=3 && i<=s.len; i++) {
        u8 c = s[s.len-i];
        if ((c & 0xc0) == 0x80) {
            continue;
        }
        i32 need = c>=0xf0 ? 4 : c>=0xe0 ? 3 : c>=0xc0 ? 2 : 1;
        return need>i ? i : 0;
    }
    return 0;
}

static void gmflushtext(GmStream *s, Arena scratch)
{
    if (s->npend) {
        gmtext(s, scratch, s->pendfield, Str(s->pend, s->npend));
        s->npend = 0;
    }
}

// Text for field, holding back a UTF-8 sequence that a byte token has
// yet to finish, as JSON strings carry whole characters.
static void gmdelta(GmStream *s, Arena scratch, i32 field, Str text)
{
    if (s->npend && s->pendfield!=field) {
        gmflushtext(s, scratch);
    }
    Str all  = concat(&scratch, Str(s->pend, s->npend), text);
    i32 keep = gmunfinished(all);
    gmtext(s, scratch, field, takehead(all, all.len-keep));
    copybytes(s->pend, all.data+all.len-keep, keep);
    s->npend     = keep;
    s->pendfield = field;
}

// A tool call's chunk: its first, with its id and name, or more of its
// arguments, as llama.cpp streams them.
static void gmcallchunk(GmStream *s, Arena scratch, i32 i, Str name, Str args)
{
    Buf     b(&scratch, 256 + name.len + 2*args.len);
    JsonOut w = gmchunkopen(&b);
    putkey(&w, "choices");
    putarray(&w);
    putobject(&w);
    putintkey(&w, "index", 0);
    putkey(&w, "delta");
    putobject(&w);
    putkey(&w, "tool_calls");
    putarray(&w);
    putobject(&w);
    putintkey(&w, "index", i);
    if (name.len) {
        Buf id(&scratch, s->callid.len + 16);
        print(&id, s->callid);
        putbyte(&id, '_');
        print(&id, (i64)i);
        putstrkey(&w, "id", finish(&id));
        putstrkey(&w, "type", "function");
    }
    putkey(&w, "function");
    putobject(&w);
    if (name.len) {
        putstrkey(&w, "name", name);
    }
    putstrkey(&w, "arguments", args);
    putend(&w);
    putend(&w);
    putend(&w);
    putend(&w);
    putkey(&w, "finish_reason");
    putnull(&w);
    putend(&w);
    putend(&w);
    putend(&w);
    print(&b, "\n\n");
    gmsend(s, finish(&b));
}

// A call being written: its name once its header ends, then what is
// settled of its arguments, so a frontend sees it grow.
static void gmcallgrow(GmStream *s, Arena scratch)
{
    Str held = gmws(Str(s->held, s->nheld));
    if (!startswith(held, "call:")) {
        return;
    }
    held = cuthead(held, 5);
    iz brace = find(held, "{");
    if (brace<=0 || brace>128) {
        return;
    }
    if (!s->callsent) {
        gmflushtext(s, scratch);
        s->callsent = 1;
        s->argsent  = 0;
        gmcallchunk(s, scratch, s->calls++, takehead(held, brace), "");
    }
    Str c = cuthead(held, brace);
    Buf b(&scratch, 2*c.len + 16);
    gmcallprefix(&c, &b, 0);
    if (b.len > s->argsent) {
        Str more = cuthead(finish(&b), s->argsent);
        s->argsent += more.len;
        gmcallchunk(s, scratch, s->calls-1, {}, more);
    }
}

static void gmhold(GmStream *s, Str text)
{
    iz n = text.len<GM_HELDMAX-s->nheld ? text.len : GM_HELDMAX-s->nheld;
    copybytes(s->held+s->nheld, text.data, n);
    s->nheld += n;
}

// Pass on one generated token. Returns 1 at the end of the response.
static b32 gmstreamtoken(GmStream *s, Arena scratch, i32 token)
{
    GmIds *d = &s->ids;
    if (token==GM_EOS || token==d->turnend || token==d->response) {
        return 1;
    } else if (token == d->channel) {
        s->thought = s->label = 1;
        s->nheld   = 0;
        return 0;
    } else if (token == d->channelend) {
        if (s->label) {
            gmdelta(s, scratch, GM_REASONING, Str(s->held, s->nheld));
        }
        s->thought = s->label = 0;
        return 0;
    } else if (token == d->call) {
        s->thought  = s->label = 0;
        s->call     = 1;
        s->callsent = 0;
        s->nheld    = 0;
        return 0;
    } else if (token == d->callend) {
        s->call = 0;
        Str    held = Str(s->held, s->nheld);
        GmCall c    = gmparsecall(&scratch, held);
        if (s->callsent) {
            // The rest of its arguments: what streamed is their prefix,
            // unless they were malformed, which the harness then says
            if (c.ok && c.args.len>s->argsent) {
                gmcallchunk(s, scratch, s->calls-1, {}, cuthead(c.args, s->argsent));
            }
            s->callsent = 0;
        } else if (c.ok) {
            gmflushtext(s, scratch);
            gmcallchunk(s, scratch, s->calls++, c.name, c.args);
        } else {
            // Not a call after all: it goes on as text
            gmdelta(s, scratch, GM_CONTENT, "<|tool_call>");
            gmdelta(s, scratch, GM_CONTENT, held);
            gmdelta(s, scratch, GM_CONTENT, "<tool_call|>");
        }
        return 0;
    }

    // The string delimiter prints as nothing, but a call needs it
    Str text = token==d->quote ? gmquote : s->tok->text[token];
    if (s->call) {
        gmhold(s, text);
        gmcallgrow(s, scratch);
    } else if (s->label) {
        gmhold(s, text);
        Str h  = Str(s->held, s->nheld);
        iz  nl = find(h, "\n");
        if (nl>=0 || h.len>16) {
            s->label = 0;
            b32 named = startswith(h, "thought\n");
            gmdelta(s, scratch, GM_REASONING, named ? cuthead(h, 8) : h);
        }
    } else {
        gmdelta(s, scratch, s->thought ? GM_REASONING : GM_CONTENT, text);
    }
    return 0;
}

// Whether token text may extend a call's header so far toward one of
// the tools offered.
static b32 gmheaderallows(GmStream *s, Str header, Str text)
{
    if (!text.len) {
        return 0;
    }
    for (iz i = 0; i < s->heads.len; i++) {
        Str target = s->heads[i];
        if (target.len<=header.len || !startswith(target, header)) {
            continue;
        }
        Str left = cuthead(target, header.len);
        iz  n    = text.len<left.len ? text.len : left.len;
        if (takehead(text, n) == takehead(left, n)) {
            return 1;
        }
    }
    return 0;
}

// Mask logits so the next token is one the protocol allows: a call names
// a tool offered, and ends before the turn does.
static void gmconstrain(GmStream *s, f32 *logits)
{
    f32 no = -__builtin_inff();
    if (!s->heads.len) {
        logits[s->ids.call] = no;
    }
    if (!s->call) {
        return;
    }
    logits[GM_EOS] = logits[s->ids.turnend] = logits[s->ids.response] = no;
    Str header = Str(s->held, s->nheld);
    if (find(header, "{") >= 0) {
        return;  // its name is settled
    }
    for (i32 t = 0; t < GM_VOCAB; t++) {
        if (logits[t]>no && !gmheaderallows(s, header, s->tok->text[t])) {
            logits[t] = no;
        }
    }
}

// llama.cpp's prefill progress, which Prospero shows with an ETA.
static void gmprogress(GmStream *s, Arena scratch, i32 total, i32 cached,
                       i32 done, i64 ms)
{
    Buf     b(&scratch, 256);
    JsonOut w = gmchunkopen(&b);
    putkey(&w, "choices");
    putarray(&w);
    putend(&w);
    putkey(&w, "prompt_progress");
    putobject(&w);
    putintkey(&w, "total", total);
    putintkey(&w, "cache", cached);
    putintkey(&w, "processed", done);
    putintkey(&w, "time_ms", ms);
    putend(&w);
    putend(&w);
    print(&b, "\n\n");
    gmsend(s, finish(&b));
}

// A comment after a stretch of silence, a call being written say, which
// keeps a watchdog waiting.
static void gmkeepalive(GmStream *s)
{
    if (s->os && os_ticks(s->os)-s->lastsent>=GM_PINGUS) {
        gmsend(s, ": keepalive\n\n");
    }
}

static void gmkeepalivefn(void *s)
{
    gmkeepalive((GmStream *)s);
}

static void gmfinish(GmStream *s, Arena scratch, Str why, i32 prompt,
                     i32 cached, i32 made)
{
    Buf     b(&scratch, 512);
    JsonOut w = gmchunkopen(&b);
    putkey(&w, "choices");
    putarray(&w);
    putobject(&w);
    putintkey(&w, "index", 0);
    putkey(&w, "delta");
    putobject(&w);
    putend(&w);
    putstrkey(&w, "finish_reason", why);
    putend(&w);
    putend(&w);
    putkey(&w, "usage");
    putobject(&w);
    putintkey(&w, "prompt_tokens", prompt);
    putintkey(&w, "completion_tokens", made);
    putintkey(&w, "total_tokens", prompt+made);
    putkey(&w, "prompt_tokens_details");
    putobject(&w);
    putintkey(&w, "cached_tokens", cached);
    putend(&w);
    putend(&w);
    putkey(&w, "timings");  // as llama.cpp's: the cache's hits are exact
    putobject(&w);
    putintkey(&w, "cache_n", cached);
    putintkey(&w, "prompt_n", prompt-cached);
    putintkey(&w, "predicted_n", made);
    putend(&w);
    putend(&w);
    print(&b, "\n\ndata: [DONE]\n\n");
    gmsend(s, finish(&b));
}

// An error once the response has begun.
static void gmstreamerror(GmStream *s, Arena scratch, Str msg)
{
    Buf     b(&scratch, 128 + 2*msg.len);
    print(&b, "data: ");
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "error");
    putobject(&w);
    putstrkey(&w, "message", msg);
    putend(&w);
    putend(&w);
    print(&b, "\n\n");
    gmsend(s, finish(&b));
}


// Sampling

struct GmSampler {
    f32 temp;  // 0 for the likeliest token
    f32 topp;
    f32 minp;
    i32 topk;
    u64 rng;
};

static u64 gmnext(u64 *rng)  // splitmix64
{
    u64 z = (*rng += 0x9e3779b97f4a7c15u);
    z = (z ^ (z>>30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z>>27)) * 0x94d049bb133111ebu;
    return z ^ (z>>31);
}

static f32 gmuniform(u64 *rng)  // [0, 1)
{
    return (f32)(gmnext(rng) >> 40) * (1.0f/(f32)(1<<24));
}

static i32 gmsample(GmSampler *sp, f32 *logits, i32 n)
{
    if (sp->temp <= 0) {
        return gmargmax(logits, n);
    }
    // The k best, likeliest first
    i32 k = sp->topk>0 && sp->topk<GM_TOPK ? sp->topk : GM_TOPK;
    f32 v[GM_TOPK];
    i32 ids[GM_TOPK];
    i32 have = 0;
    f32 no   = -__builtin_inff();
    for (i32 t = 0; t < n; t++) {
        f32 x = logits[t];
        if (x==no || (have==k && x<=v[k-1])) {
            continue;
        }
        i32 i = have<k ? have++ : k-1;
        for (; i>0 && v[i-1]<x; i--) {
            v[i]   = v[i-1];
            ids[i] = ids[i-1];
        }
        v[i]   = x;
        ids[i] = t;
    }
    if (!have) {
        return gmargmax(logits, n);
    }

    f32 sum = 0;
    for (i32 i = 0; i < have; i++) {
        v[i] = __builtin_expf((v[i] - v[0]) / sp->temp);
        sum += v[i];
    }
    // The smallest prefix holding top_p of it, each at least min_p of
    // the likeliest
    f32 mass  = 0;
    i32 count = 0;
    while (count<have && (!count || mass<sp->topp*sum) && v[count]>=sp->minp*v[0]) {
        mass += v[count++];
    }
    f32 at = gmuniform(&sp->rng) * mass;
    for (i32 i = 0; i < count; i++) {
        at -= v[i];
        if (at < 0) {
            return ids[i];
        }
    }
    return ids[count-1];
}


// The cache of sequences

struct GmCheck {
    Region *region;  // the rings at pos
    i32     pos;
    i32     ringlo;
    i64     stamp;
    b32     system;  // where the system turn ends
};

struct GmSeq {
    GmSeq  *next;      // most recently used first
    u8      sid[GM_SIDMAX];
    i32     sidlen;
    GmKv    kv;
    i32     n;         // positions held
    i32     ringlo;    // the first position the rings still hold
    i32     hi;        // positions committed
    GmCheck checks[GM_CHECKS];
};

struct GmCache {
    Os    *os;
    GmSeq *seqs;
    GmSeq *free;    // structs to reuse, their regions released
    Arena *perm;
    i64    budget;  // bytes
    i64    stamp;
    i32    cap;     // positions per sequence
};

static iz gmringbytes()
{
    return GM_RINGFLOATS * (iz)sizeof(f32);
}

static i64 gmseqbytes(GmSeq *s)
{
    i64 r = (i64)gmringbytes() + (i64)s->kv.cap*4 + (i64)s->hi*GM_POSFLOATS*4;
    for (i32 i = 0; i < GM_CHECKS; i++) {
        r += s->checks[i].region ? gmringbytes() : 0;
    }
    return r;
}

static Str gmsidkey(Str sid)
{
    return takehead(sid, sid.len<GM_SIDMAX ? sid.len : (iz)GM_SIDMAX);
}

static GmSeq *gmfindseq(GmCache *c, Str sid)
{
    sid = gmsidkey(sid);
    for (GmSeq *s = c->seqs; s; s = s->next) {
        if (Str(s->sid, s->sidlen) == sid) {
            return s;
        }
    }
    return 0;
}

static void gmdropcheck(GmCache *c, GmCheck *k)
{
    if (k->region) {
        os_release(c->os, k->region);
    }
    *k = {};
}

static void gmunlink(GmCache *c, GmSeq *s)
{
    for (GmSeq **p = &c->seqs; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            return;
        }
    }
}

static void gmevict(GmCache *c, GmSeq *s)
{
    gmunlink(c, s);
    for (i32 i = 0; i < GM_CHECKS; i++) {
        gmdropcheck(c, s->checks+i);
    }
    if (s->kv.region) {
        os_release(c->os, s->kv.region);
    }
    *s = {};
    s->next = c->free;
    c->free = s;
}

// Fit the budget, and the count, evicting the least recently used but
// keep.
static void gmfit(GmCache *c, GmSeq *keep)
{
    for (;;) {
        i64    total = 0;
        i32    count = 0;
        GmSeq *last  = 0;
        for (GmSeq *s = c->seqs; s; s = s->next) {
            total += gmseqbytes(s);
            count++;
            last = s!=keep ? s : last;
        }
        if (!last || (total<=c->budget && count<=GM_SEQS)) {
            return;
        }
        gmevict(c, last);
    }
}

// Where to start a prompt of n tokens: the sequence whose state to take,
// how many of its positions, and whether from a checkpoint.
struct GmPlan {
    GmSeq *from;
    i32    start;
    i32    check;  // from's checkpoint, or -1 for its own rings
};

static GmPlan gmplanseq(GmSeq *s, i32 *tokens, i32 n)
{
    GmPlan r = {s, 0, -1};
    // At least one token to run, for the next one's logits
    i32 lim    = s->n<n-1 ? s->n : n-1;
    i32 common = 0;
    for (; common<lim && s->kv.tokens[common]==tokens[common]; common++) {}
    // Its rings must hold the window the next position attends to
    i32 need = common>GM_WINDOW-1 ? common-(GM_WINDOW-1) : 0;
    if (need >= s->ringlo) {
        r.start = common;
        return r;
    }
    for (i32 i = 0; i < GM_CHECKS; i++) {
        GmCheck *k = s->checks + i;
        if (k->region && k->pos<=common && k->pos>r.start) {
            r.start = k->pos;
            r.check = i;
        }
    }
    return r;
}

// The best start for session sid's prompt: its own sequence's, or a
// longer one another gives.
static GmPlan gmplan(GmCache *c, Str sid, i32 *tokens, i32 n)
{
    GmSeq *own  = gmfindseq(c, sid);
    GmPlan best = {};
    best.check  = -1;
    if (own) {
        best = gmplanseq(own, tokens, n);
    }
    for (GmSeq *s = c->seqs; s; s = s->next) {
        GmPlan p = s!=own ? gmplanseq(s, tokens, n) : GmPlan{};
        best = p.start>best.start ? p : best;
    }
    if (!best.start) {
        best = {own, 0, -1};
    }
    return best;
}

static void gmsetrings(GmSeq *dst, f32 *rings, i32 ringlo)
{
    copybytes(dst->kv.ring, rings, gmringbytes());
    dst->ringlo = ringlo;
}

static b32 gmgrowseq(GmSeq *s, i32 end)
{
    if (end > s->hi) {
        if (!gmkvgrow(&s->kv, end)) {
            return 0;
        }
        s->hi = end;
    }
    return 1;
}

// Session sid's sequence, set up as plan says: its own gone back to the
// plan's start, or a copy of another's prefix, into its own or a new
// one. Null if there is no memory for it.
static GmSeq *gmtake(GmCache *c, Str sid, GmPlan p)
{
    sid = gmsidkey(sid);
    GmSeq *own = gmfindseq(c, sid);
    if (!own) {
        own = c->free;
        if (own) {
            c->free = own->next;
        } else {
            own = alloc<GmSeq>(c->perm);
        }
        *own = {};
        for (i32 tries = 0; !own->kv.region && tries<GM_SEQS+1; tries++) {
            own->kv = gmkvnew(c->os, c->cap);
            if (!own->kv.region) {
                // Make room: the least recently used goes
                GmSeq *last = 0;
                for (GmSeq *s = c->seqs; s; s = s->next) {
                    last = s!=p.from ? s : last;
                }
                if (!last) {
                    break;
                }
                gmevict(c, last);
            }
        }
        if (!own->kv.region) {
            own->next = c->free;
            c->free   = own;
            return 0;
        }
        copybytes(own->sid, sid.data, sid.len);
        own->sidlen = (i32)sid.len;
    } else {
        gmunlink(c, own);
    }
    own->next = c->seqs;
    c->seqs   = own;

    GmSeq *from = p.from ? p.from : own;
    if (from != own) {
        // Another's prefix: its tokens, full caches and rings
        for (i32 i = 0; i < GM_CHECKS; i++) {
            gmdropcheck(c, own->checks+i);
        }
        own->n = 0;
        if (!gmgrowseq(own, p.start)) {
            p.start = 0;
        } else {
            copybytes(own->kv.tokens, from->kv.tokens, (iz)p.start*4);
            copybytes(own->kv.full, from->kv.full, (iz)p.start*GM_POSFLOATS*4);
            if (p.check >= 0) {
                GmCheck *k = from->checks + p.check;
                gmsetrings(own, (f32 *)k->region->base, k->ringlo);
            } else {
                gmsetrings(own, from->kv.ring, from->ringlo);
            }
        }
    } else if (p.check >= 0) {
        GmCheck *k = own->checks + p.check;
        gmsetrings(own, (f32 *)k->region->base, k->ringlo);
    }
    own->n      = p.start;
    own->ringlo = p.start ? own->ringlo : 0;
    for (i32 i = 0; i < GM_CHECKS; i++) {
        if (own->checks[i].pos > p.start) {
            gmdropcheck(c, own->checks+i);  // positions about to change
        }
    }
    gmfit(c, own);
    return own;
}

// Copy the rings at the sequence's end, as a checkpoint.
static void gmcheckpoint(GmCache *c, GmSeq *s, b32 system)
{
    GmCheck *slot = 0;
    for (i32 i = 0; i < GM_CHECKS; i++) {
        GmCheck *k = s->checks + i;
        if (k->region && k->pos==s->n) {
            return;  // already
        }
        slot = !slot && !k->region ? k : slot;
    }
    // Else the oldest, but a system turn's only for another
    b32 empty = slot != 0;
    for (i32 i = 0; !empty && i < GM_CHECKS; i++) {
        GmCheck *k = s->checks + i;
        slot = (system || !k->system) && (!slot || k->stamp<slot->stamp) ? k : slot;
    }
    if (!slot) {
        return;
    }
    gmdropcheck(c, slot);
    Region *r = os_reserve(c->os, gmringbytes() + (1<<16));
    if (!r || !os_commit(r, gmringbytes())) {
        if (r) os_release(c->os, r);
        return;
    }
    copybytes(r->base, s->kv.ring, gmringbytes());
    slot->region = r;
    slot->pos    = s->n;
    slot->ringlo = s->ringlo;
    slot->stamp  = ++c->stamp;
    slot->system = system;
    gmfit(c, s);
}


// A request, prepared and run

struct GmChat {
    Gemma  *g;
    GmIds   ids;
    GmCache cache;
    Os     *os;
    u64     rng;
    i64     responses;
};

static GmChat *gmchatnew(Os *os, Arena *perm, Gemma *g, i32 cap, i64 budget, u64 seed)
{
    GmChat *c = alloc<GmChat>(perm);
    c->g            = g;
    c->ids          = gmids(&g->tok);
    c->os           = os;
    c->rng          = seed;
    c->cache.os     = os;
    c->cache.perm   = perm;
    c->cache.cap    = cap;
    c->cache.budget = budget;
    return c;
}

struct GmTurn {
    Slice<i32> tokens;
    i32        sysend;    // where the system turn ends, or 0
    b32        userlast;  // a checkpoint at the prompt's end
    b32        inthought;
    GmSampler  sp;
    i32        maxnew;
    i32        status;    // an HTTP error's, else 0
    Str        err;       // its JSON body
};

static Str gmerrbody(Arena *a, Str msg)
{
    Buf     b(a, 128 + 2*msg.len);
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "error");
    putobject(&w);
    putstrkey(&w, "message", msg);
    putend(&w);
    putend(&w);
    return finish(&b);
}

static f32 gmnum(Json *v, f32 dflt)
{
    return v->type==JSON_NUMBER ? (f32)jsonfloat(v, dflt) : dflt;
}

// A request, of bodylen bytes, as tokens and settings, or the HTTP
// error to answer with.
static GmTurn gmturn(GmChat *c, Arena *perm, Arena scratch, Json *req, iz bodylen)
{
    GmTurn r = {};
    // The template turns each string's two bytes of quotes into ten
    iz       cap = 5*bodylen + 4096;
    GmRender rn  = gmrender(&scratch, req, gmthinking(req), cap);
    if (rn.err.len) {
        r.status = 400;
        r.err    = gmerrbody(perm, rn.err);
        return r;
    }
    r.tokens    = gmencode(&c->g->tok, perm, scratch, Str(rn.p.text, rn.p.len), rn.p.mask);
    r.inthought = rn.inthought;
    r.userlast  = rn.userlast;
    if (rn.system) {
        for (iz i = 0; i+1 < r.tokens.len; i++) {
            if (r.tokens[i] == c->ids.turnend) {
                r.sysend = (i32)(i + 2);  // and its newline
                break;
            }
        }
    }

    i32 ctx = c->cache.cap;
    if (r.tokens.len >= ctx) {
        // As llama.cpp says it, for the engine to compact
        Buf     b(perm, 256);
        JsonOut w = {&b};
        putobject(&w);
        putkey(&w, "error");
        putobject(&w);
        putintkey(&w, "code", 400);
        putstrkey(&w, "message", "the request exceeds the available context size, "
                                 "try increasing it");
        putstrkey(&w, "type", "exceed_context_size_error");
        putintkey(&w, "n_prompt_tokens", r.tokens.len);
        putintkey(&w, "n_ctx", ctx);
        putend(&w);
        putend(&w);
        r.status = 400;
        r.err    = finish(&b);
        return r;
    }

    Json *mt = jsonget(req, "max_tokens");
    mt = mt->type==JSON_NUMBER ? mt : jsonget(req, "max_completion_tokens");
    i64 maxnew = jsonint(mt, GM_MAXNEW);
    r.maxnew   = (i32)(maxnew>0 && maxnew<ctx ? maxnew : (i64)GM_MAXNEW);

    // Gemma's own defaults (its generation_config), as llama.cpp reads
    // them from a GGUF: greedy decoding falls into repeated calls
    r.sp.temp = gmnum(jsonget(req, "temperature"), 1);
    r.sp.topp = gmnum(jsonget(req, "top_p"), 0.95f);
    r.sp.minp = gmnum(jsonget(req, "min_p"), 0);
    i64 topk  = jsonint(jsonget(req, "top_k"), 0);
    r.sp.topk = (i32)(topk>0 && topk<GM_TOPK ? topk : (i64)GM_TOPK);
    Json *seed = jsonget(req, "seed");
    r.sp.rng   = seed->type==JSON_NUMBER ? (u64)jsonint(seed, 0) : gmnext(&c->rng);
    return r;
}

struct GmOut {
    void (*emit)(void *, Str);
    void  *ctx;
    i32   *cancel;  // nonzero to stop
};

struct GmStats {
    i32 prompt;
    i32 cached;
    i32 made;
    i64 prefillus;
    i64 generateus;
};

static b32 gmcancelled(GmOut *o)
{
    return o->cancel && __atomic_load_n(o->cancel, __ATOMIC_RELAXED);
}

// Run a prepared request for session sid, its SSE to out.
static GmStats gmgenerate(GmChat *c, Arena scratch, GmTurn *t, Str sid, Json *req,
                          GmOut *out)
{
    GmStats st  = {};
    Gemma  *g   = c->g;
    i32    *tok = t->tokens.data;
    i32     n   = (i32)t->tokens.len;
    i64     t0  = os_ticks(c->os);

    Buf idb(&scratch, 32);
    print(&idb, "call_");
    printhex(&idb, gmnext(&c->rng), 12);
    GmStream s = gmstream(&scratch, &g->tok, c->ids, req, c->os, out->emit, out->ctx,
                          finish(&idb));
    s.thought  = t->inthought;
    st.prompt  = n;

    GmSeq *q = gmtake(&c->cache, sid, gmplan(&c->cache, sid, tok, n));
    if (!q) {
        gmstreamerror(&s, scratch, "out of memory for the built-in model's cache");
        return st;
    }
    st.cached = q->n;

    // Prefill on the batch grid, stopping at the system turn's end
    i32 done = q->n, last = 0;
    gmprogress(&s, scratch, n, st.cached, done, 0);
    while (done<n && !gmcancelled(out)) {
        i32 chunk = GM_BATCH - done%GM_BATCH;
        chunk = chunk<n-done ? chunk : n-done;
        chunk = t->sysend>done && t->sysend<done+chunk ? t->sysend-done : chunk;
        copybytes(q->kv.tokens+done, tok+done, (iz)chunk*4);
        // A batch deep in a long context takes seconds: comments between
        // its layers keep the watchdog, and the window's clock, waiting
        g->tick    = gmkeepalivefn;
        g->tickctx = &s;
        b32 ok = gmgrowseq(q, done+chunk) && gmforward(g, &q->kv, tok+done, chunk, done);
        g->tick    = 0;
        g->tickctx = 0;
        if (!ok) {
            gmstreamerror(&s, scratch, "out of memory for the built-in model's cache");
            return st;
        }
        done += chunk;
        last  = chunk - 1;
        q->n      = done;
        q->ringlo = done-GM_RING>q->ringlo ? done-GM_RING : q->ringlo;
        if (done == t->sysend) {
            gmcheckpoint(&c->cache, q, 1);
        }
        gmprogress(&s, scratch, n, st.cached, done, (os_ticks(c->os) - t0)/1000);
    }
    if (gmcancelled(out)) {
        return st;  // what it prefilled stays, for a retry
    }
    if (t->userlast) {
        gmcheckpoint(&c->cache, q, 0);
    }
    i64 t1 = os_ticks(c->os);
    st.prefillus = t1 - t0;

    Str why = "length";
    i32 pos = n;
    i32 end = n+t->maxnew<q->kv.cap ? n+t->maxnew : q->kv.cap;
    end = end<g->ctx ? end : g->ctx;
    for (b32 first = 1; pos<end && !gmcancelled(out); first = 0) {
        f32 *v = gmlogits(g, first ? last : 0);
        gmconstrain(&s, v);
        i32 next = gmsample(&t->sp, v, GM_VOCAB);
        if (gmstreamtoken(&s, scratch, next)) {
            why = "stop";
            break;
        }
        q->kv.tokens[pos] = next;
        if (!gmgrowseq(q, pos+1) || !gmforward(g, &q->kv, &next, 1, pos)) {
            gmstreamerror(&s, scratch, "out of memory for the built-in model's cache");
            return st;
        }
        pos++;
        q->n      = pos;
        q->ringlo = pos-GM_RING>q->ringlo ? pos-GM_RING : q->ringlo;
        gmkeepalive(&s);
    }
    st.made       = pos - n;
    st.generateus = os_ticks(c->os) - t1;
    if (gmcancelled(out)) {
        return st;
    }
    if (s.call && s.callsent) {
        // Cut off in a call that began streaming: it stays "length"
    } else if (s.calls) {
        why = "tool_calls";
    } else if (s.call) {
        gmdelta(&s, scratch, GM_CONTENT, Str(s.held, s.nheld));  // cut off
    }
    gmflushtext(&s, scratch);
    gmfinish(&s, scratch, why, n, st.cached, st.made);
    return st;
}


#if PROSPERO_DEVTOOLS
static void gmdevemit(void *ctx, Str sse)
{
    os_print((Os *)ctx, 1, sse);
}

// builtin-render REQUEST: the prompt a request renders to, on stdout.
// builtin-chat MODEL [SID:]REQUEST...: each request in turn, as session
// SID ("s" if not given), its SSE on stdout, and how fast on stderr.
static i32 gmchatmain(Os *os, Arena perm, Str *args, i32 nargs)
{
    Arena scratch = newarena(os_reserve(os, (iz)1<<30));
    b32   render  = args[1] == "builtin-render";
    if ((render && nargs!=3) || (!render && nargs<4) || !scratch.beg) {
        os_print(os, 2, "usage: prospero builtin-render REQUEST\n"
                        "       prospero builtin-chat MODEL [SID:]REQUEST...\n");
        return 1;
    }
    if (render) {
        Read    f = os_read(os, &perm, args[2], (iz)1<<28);
        JsonDoc d = jsonparse(&perm, f.data);
        if (!d.ok) {
            os_print(os, 2, "prospero: not a JSON request\n");
            return 1;
        }
        GmRender r = gmrender(&perm, d.root, gmthinking(d.root), 5*f.data.len + 4096);
        os_print(os, 1, Str(r.p.text, r.p.len));
        return 0;
    }

    i32    ctx = GM_CONTEXT;
    Gemma *g   = gmdevload(os, &perm, scratch, args[2], ctx);
    if (!g) {
        return 1;
    }
    GmChat *c    = gmchatnew(os, &perm, g, ctx, (i64)2<<30, os_random(os));
    Arena   work = newarena(os_reserve(os, (iz)1<<30));
    for (i32 i = 3; i < nargs; i++) {
        Arena   t   = scratch;
        Arena   w   = work;
        Cut     sc  = cut(args[i], ':');
        Str     sid = sc.ok ? sc.head : Str("s");
        Str     path = sc.ok ? sc.tail : args[i];
        Read    f   = os_read(os, &t, path, (iz)1<<28);
        JsonDoc d   = jsonparse(&t, f.data);
        if (!d.ok) {
            os_print(os, 2, "prospero: not a JSON request\n");
            return 1;
        }
        GmTurn tn = gmturn(c, &t, w, d.root, f.data.len);
        if (tn.status) {
            os_print(os, 1, tn.err);
            os_print(os, 1, "\n");
            continue;
        }
        GmOut   out = {gmdevemit, os, 0};
        GmStats st  = gmgenerate(c, w, &tn, sid, d.root, &out);
        Buf b(&t, 256);
        print(&b, sid);
        print(&b, ": ");
        print(&b, (i64)st.prompt);
        print(&b, " prompt tokens (");
        print(&b, (i64)st.cached);
        print(&b, " cached, ");
        printfloat(&b, (f64)(st.prompt-st.cached)*1e6/(f64)(st.prefillus+1), 1);
        print(&b, " tok/s), ");
        print(&b, (i64)st.made);
        print(&b, " made (");
        printfloat(&b, (f64)st.made*1e6/(f64)(st.generateus+1), 1);
        print(&b, " tok/s)\n");
        os_print(os, 2, finish(&b));
    }
    return 0;
}
#endif

// This file is derived from gemma4.c, under its license:
//
// MIT License
//
// Copyright (c) 2026 Qmog AI
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

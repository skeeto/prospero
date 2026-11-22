// Prices: what a request cost at list prices, from its usage
//
// The engine prices each request as it logs its usage (putusage), in
// whole millionths of a dollar (microusd), so that sums are exact and a
// request keeps the price of its day. A model's price is its [model]
// price; else its endpoint's kind settles it, the built-in model being
// free and ChatGPT and Copilot billing a plan, so having none; else
// this table's, for the model the response names, else for the one
// asked for. Only Anthropic's models are here, at Anthropic's list
// prices: others need [model] price. Nothing is assumed of a local URL,
// as a proxy on this machine may forward to a paid service.
//
// From platform.claude.com/docs/en/about-claude/pricing, 2026-11-21.
// A cache write is at the 5-minute rate, as ancache never asks for the
// hour; nor are fast mode, inference_geo or batches asked for, each
// priced otherwise: should one be, its requests need its prices here.

enum : i64 {
    PRICE_SEARCH = 10000,  // microusd: an Anthropic web search, $10 a 1000
};

struct PriceRow {
    Str ids;      // separated by blanks, each as pricenorm leaves it
    f64 in;       // dollars per million tokens, as Price
    f64 written;  // (in the pricing page's order)
    f64 cached;
    f64 out;
    i64 above;
    f64 times;
};

static PriceRow const pricerows[] = {
    {"claude-fable-5-2 claude-mythos-5-2", 8, 10, 0.20, 40, 0, 0},
    {"claude-fable-5-1 claude-mythos-5-1", 10, 12.50, 0.25, 50, 0, 0},
    {"claude-fable-5 claude-mythos-5", 10, 12.50, 1, 50, 0, 0},
    {"claude-opus-5-5", 4, 5, 0.20, 20, 0, 0},
    {"claude-opus-5 claude-opus-4-8 claude-opus-4-7 claude-opus-4-6 "
     "claude-opus-4-5", 5, 6.25, 0.50, 25, 0, 0},
    {"claude-opus-4-1 claude-opus-4", 15, 18.75, 1.50, 75, 0, 0},
    {"claude-sonnet-5-5", 2, 2.50, 0.10, 10, 0, 0},
    {"claude-sonnet-5", 2, 2.50, 0.20, 10, 0, 0},
    {"claude-sonnet-4-6 claude-sonnet-4-5 claude-sonnet-4", 3, 3.75, 0.30, 15, 0, 0},
    // A prompt over 100,000 tokens, cache reads and writes included,
    // costs five times as much, every token of the request
    {"claude-haiku-5-5", 0.10, 0.125, 0.01, 0.50, 100000, 5},
    {"claude-haiku-4-5", 1, 1.25, 0.10, 5, 0, 0},
};

// A model's id as the table has it: lowercase, what follows its last
// '/' (OpenRouter's anthropic/), no Bedrock region and "anthropic.",
// no Vertex @version, no Bedrock -vN:M, no -YYYYMMDD, and '-' for '.'
// (OpenRouter's claude-opus-4.1). Rewritten to one key, never matched
// by part.
static Str pricenorm(Arena *a, Str id)
{
    Str s = clone(a, id);
    for (iz i = 0; i < s.len; i++) {
        s.data[i] = lowercase(s[i]);
    }
    for (iz i = s.len-1; i >= 0; i--) {
        if (s[i] == '/') {
            s = cuthead(s, i+1);
            break;
        }
    }
    s = cut(s, '@').head;
    s = cut(s, ':').head;
    iz v = s.len;
    for (; v>0 && digit(s[v-1]); v--) {}
    if (v<s.len && v>=2 && s[v-1]=='v' && s[v-2]=='-') {
        s = takehead(s, v-2);
    }
    iz at = find(s, "anthropic.");
    if (at>=0 && find(takehead(s, at), "-")<0) {
        s = cuthead(s, at + 10);  // a region before, such as "us."
    }
    if (s.len >= 9 && s[s.len-9] == '-') {
        b32 date = 1;
        for (iz i = s.len-8; i < s.len; i++) {
            date &= digit(s[i]);
        }
        s = date ? cuttail(s, 9) : s;
    }
    for (iz i = 0; i < s.len; i++) {
        s.data[i] = s[i]=='.' ? '-' : s[i];
    }
    return s;
}

// The table's row for a model id, null if none.
static PriceRow const *pricerow(Arena scratch, Str id)
{
    Str key = pricenorm(&scratch, id);
    for (iz i = 0; key.len && i < countof(pricerows); i++) {
        for (Str rest = pricerows[i].ids; rest.len;) {
            Cut c = cut(rest, ' ');
            rest = c.tail;
            if (c.head == key) {
                return &pricerows[i];
            }
        }
    }
    return 0;
}

// Parse [model] price, true if valid: empty (unset), none, 0 (free), or
// INPUT OUTPUT [CACHE_READ [CACHE_WRITE]] in dollars per million tokens,
// each 0 or more, separated by blanks. An omitted cache read costs a
// tenth of input, an omitted cache write a quarter more than input, as
// most services price them. Invalid, it is left unset.
static b32 priceparse(Str v, Price *p)
{
    *p = {};
    v  = trim(v);
    if (!v.len) {
        return 1;
    } else if (v == "none") {
        p->how = PRICE_NONE;
        return 1;
    }
    f64 f[4] = {};
    i32 n    = 0;
    for (iz i = 0; i < v.len;) {
        if (whitespace(v[i])) {
            i++;
            continue;
        }
        iz j = i;
        for (; j<v.len && !whitespace(v[j]); j++) {}
        Float x = parsefloat(Str(v.data+i, j-i));
        if (n==4 || !x.ok || !(x.val>=0 && x.val<1e6)) {
            return 0;
        }
        f[n++] = x.val;
        i = j;
    }
    if (n==1 ? f[0]!=0 : n<2) {
        return 0;  // one price alone is ambiguous, but free
    }
    p->how     = PRICE_SET;
    p->in      = f[0];
    p->out     = n>1 ? f[1] : 0;
    p->cached  = n>2 ? f[2] : f[0]*0.1;
    p->written = n>3 ? f[3] : f[0]*1.25;
    return 1;
}

// A request's price: its model's [model] price (cfg), else by its
// endpoint's kind, else the table's for the model the response named
// (answered), else for the one asked for. how is PRICE_NONE if none.
static Price pricefor(Arena scratch, Price *cfg, i32 kind, Str answered, Str asked)
{
    Price r = {};
    r.how = PRICE_NONE;
    if (cfg && cfg->how!=PRICE_UNSET) {
        return *cfg;
    } else if (kind == EP_BUILTIN) {
        r.how = PRICE_SET;  // free
        return r;
    } else if (kind==EP_CHATGPT || kind==EP_COPILOT) {
        return r;  // billed to a plan, not by the token
    }
    PriceRow const *row = answered.len ? pricerow(scratch, answered) : 0;
    row = row ? row : pricerow(scratch, asked);
    if (row) {
        r.how     = PRICE_SET;
        r.in      = row->in;
        r.out     = row->out;
        r.cached  = row->cached;
        r.written = row->written;
        r.above   = row->above;
        r.times   = row->times;
    }
    return r;
}

// A request's cost in microusd, from its usage and its web searches at a
// price (pricefor), or -1 if it has none.
static i64 costof(Price *p, Usage *u, i64 searches)
{
    if (p->how != PRICE_SET) {
        return -1;
    }
    i64 plain = u->in - u->cached - u->written;
    plain = plain>0 ? plain : 0;
    // Tokens times dollars per million tokens: microusd
    f64 c = (f64)plain*p->in + (f64)u->written*p->written +
            (f64)u->cached*p->cached + (f64)u->out*p->out;
    if (p->above && u->in>p->above) {
        c *= p->times;
    }
    c += (f64)(searches>0 ? searches : 0) * (f64)PRICE_SEARCH;
    return (i64)(c + 0.5);
}

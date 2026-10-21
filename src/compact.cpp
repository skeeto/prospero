// Compaction: making room in a long conversation's context
//
// The engine estimates the size of each request's prompt in tokens: its
// bytes at the rate the server last reported (usage.prompt_tokens over
// the bytes then sent), or 4 bytes a token until it has. When the
// estimate reaches the window less max(15%, 16384) tokens (compactat),
// the engine compacts before sending, in two stages:
//
// 1. Prune: tool results past the newest 40k tokens of tool output are
//    replaced by a line saying where the full output is, if that saves
//    20k tokens or more, but never a result under 50 tokens. No model
//    is involved, and often this is enough.
// 2. Summarize: the older messages go to the compactor, a model asked
//    for text only, which writes a summary in fixed sections; a later
//    compaction updates the previous summary. The newest 20k tokens
//    stay verbatim, cut at a message that is not a tool result, so that
//    a call keeps its result. The summary, capped at 16384 tokens, ends
//    with the files the calls read and wrote, kept by the harness.
//
// The transcript then becomes [system][a checkpoint user message with
// the summary][the messages kept]. This is the one deliberate break of
// the prompt cache: nothing else rewrites history. Everything here is
// pure; the engine sends the request and logs the result.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    COMPACT_RESERVE  = 16384,     // tokens kept free below the window
    COMPACT_KEEP     = 20000,     // newest tokens kept verbatim
    COMPACT_PROTECT  = 40000,     // newest tool output never pruned
    COMPACT_MINSAVE  = 20000,     // tokens a prune must save
    COMPACT_MINPRUNE = 50,        // smaller results are never pruned
    COMPACT_SUMMAX   = 16384,     // tokens of a summary, at most
    COMPACT_FILES    = 40,        // files listed of each kind
    COMPACT_TOOLIN   = 3 << 10,   // bytes of a tool result summarized
    COMPACT_TEXTIN   = 16 << 10,  // bytes of a message summarized
    COMPACT_ARGSIN   = 2 << 10,   // bytes of a call's arguments
};

// The rate of tokens to bytes the server last reported.
struct Rate {
    i64 tokens;
    i64 bytes;   // 0 until a report
};

static i64 tokensof(Rate r, i64 bytes)
{
    if (r.tokens>0 && r.bytes>0) {
        return (i64)((f64)bytes * (f64)r.tokens / (f64)r.bytes);
    }
    return bytes / 4;
}

static i64 bytesof(Rate r, i64 tokens)
{
    if (r.tokens>0 && r.bytes>0) {
        return (i64)((f64)tokens * (f64)r.bytes / (f64)r.tokens);
    }
    return tokens * 4;
}

// A message's size as estimates count it: its texts and a little for
// the structure around them.
static i64 msgbytes(Msg *m)
{
    i64 n = 16 + m->text.len;
    for (iz i = 0; i < m->blocks.len; i++) {
        Block *b = &m->blocks[i];
        n += 16 + b->text.len + b->args.len + b->name.len + b->id.len;
    }
    return n;
}

// The prompt size, in a window of w tokens, at which to compact: w less
// max(15% of w, COMPACT_RESERVE), or less only the 15% when the reserve
// would take more than half of a small window.
static i64 compactat(i64 w)
{
    i64 pct     = w * 15 / 100;
    i64 reserve = COMPACT_RESERVE>pct && COMPACT_RESERVE<=w/2 ? COMPACT_RESERVE : pct;
    return w - reserve;
}

// Tokens to plan for a summary when the prompt must come to goal.
static i64 sumbudget(i64 goal)
{
    i64 b = goal / 4;
    b = b>1024 ? b : 1024;
    return b<COMPACT_SUMMAX ? b : COMPACT_SUMMAX;
}


// Pruning

static Str const elidedmark = "[output elided: ";

static b32 iselided(Str text)
{
    return startswith(text, elidedmark);
}

// What replaces a pruned result.
static Str elision(Arena *a, i64 tokens, Str path)
{
    Buf b(a, 64 + path.len);
    print(&b, elidedmark);
    print(&b, tokens);
    print(&b, " tokens; full output in ");
    print(&b, path);
    putbyte(&b, ']');
    return finish(&b);
}

struct Prune {
    Slice<iz> idx;    // history indexes of results to elide, newest first
    i64       saved;  // tokens, about
};

// Old tool results to elide: those past the newest COMPACT_PROTECT
// tokens of tool output, of COMPACT_MINPRUNE tokens or more and not
// elided already, if together they save COMPACT_MINSAVE tokens. Else
// none.
static Prune pickprune(Arena *a, Slice<Msg> h, Rate rate)
{
    Prune r    = {};
    i64   seen = 0;
    i64   cost = tokensof(rate, 120);  // of an elision
    for (iz i = h.len-1; i >= 0; i--) {
        Msg *m = &h[i];
        if (m->role != ROLE_TOOL) {
            continue;
        }
        i64 t = tokensof(rate, m->text.len);
        if (seen < COMPACT_PROTECT) {
            seen += t;  // the result across the line is kept whole
            continue;
        } else if (t<COMPACT_MINPRUNE || iselided(m->text)) {
            continue;
        }
        r.idx    = push(a, r.idx, i);
        r.saved += t - cost;
    }
    if (r.saved < COMPACT_MINSAVE) {
        r = {};
    }
    return r;
}


// Summarizing

// Where the history kept verbatim begins: the earliest message, at or
// after start, from which the rest comes to keep tokens at most, and
// never a tool result, which stays with its call. h.len keeps nothing.
static iz cutpoint(Slice<Msg> h, iz start, Rate rate, i64 keep)
{
    iz  cut  = h.len;
    i64 kept = 0;
    for (iz i = h.len-1; i >= start; i--) {
        kept += tokensof(rate, msgbytes(&h[i]));
        if (kept > keep) {
            break;
        } else if (h[i].role != ROLE_TOOL) {
            cut = i;
        }
    }
    return cut;
}

// A text, cut in the middle to max bytes or so.
static Str clip(Arena *a, Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    Str head = utf8head(s, max*2/3);
    Str tail = utf8tail(s, max/3);
    Buf b(a, head.len + tail.len + 64);
    print(&b, head);
    print(&b, "\n[... ");
    print(&b, (i64)(s.len - head.len - tail.len));
    print(&b, " bytes omitted ...]\n");
    print(&b, tail);
    return finish(&b);
}

// The messages h[beg, cut) as plain text for the summarizer: reasoning
// left out, long texts clipped, and at most the newest limit bytes of
// it (0 for no limit).
static Str transcript(Arena *a, Arena scratch, Slice<Msg> h, iz beg, iz cut,
                      iz limit)
{
    Slice<Str> parts = {};
    for (iz i = beg; i < cut; i++) {
        Msg *m = &h[i];
        Buf  b(&scratch, 256);
        switch (m->role) {
        case ROLE_USER:
            print(&b, "[user]\n");
            print(&b, clip(&scratch, m->text, COMPACT_TEXTIN));
            print(&b, "\n\n");
            break;
        case ROLE_TOOL:
            print(&b, "[tool result]\n");
            print(&b, clip(&scratch, m->text, COMPACT_TOOLIN));
            print(&b, "\n\n");
            break;
        case ROLE_ASSISTANT:
            for (iz k = 0; k < m->blocks.len; k++) {
                Block *bk = &m->blocks[k];
                if (bk->type==BLOCK_TEXT && trim(bk->text).len) {
                    print(&b, "[assistant]\n");
                    print(&b, clip(&scratch, bk->text, COMPACT_TEXTIN));
                    print(&b, "\n\n");
                } else if (bk->type == BLOCK_CALL) {
                    print(&b, "[tool call: ");
                    print(&b, bk->name);
                    print(&b, "]\n");
                    print(&b, clip(&scratch, bk->args, COMPACT_ARGSIN));
                    print(&b, "\n\n");
                }
            }
            break;
        }
        parts = push(&scratch, parts, finish(&b));
    }

    // The newest parts that fit
    iz first = parts.len;
    iz total = 0;
    for (; first > 0; first--) {
        iz n = parts[first-1].len;
        if (limit && total+n>limit) {
            break;
        }
        total += n;
    }
    Buf out(a, total + 64);
    if (first > 0) {
        print(&out, "[earlier messages omitted]\n\n");
    }
    for (iz i = first; i < parts.len; i++) {
        print(&out, parts[i]);
    }
    return finish(&out);
}

static Str const compactsystem =
    "You summarize a coding session between a user and an AI agent that "
    "works through tools, so that the agent can carry on with your summary "
    "in place of the conversation. Respond with text only, do not call "
    "tools.";

static Str const compactsections =
    "Use exactly these sections:\n"
    "\n"
    "## Goal\n"
    "What the user asked for, in their words where it matters.\n"
    "\n"
    "## Constraints and preferences\n"
    "Requirements, limits and preferences the user stated.\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "### In progress\n"
    "### Blocked\n"
    "\n"
    "## Key decisions\n"
    "Choices made, and why.\n"
    "\n"
    "## Next steps\n"
    "What to do next, in order.\n"
    "\n"
    "## Critical context\n"
    "Exact file paths, names, commands, values and error messages the "
    "work depends on.\n";

// The summarizer's request: no tools offered, text only, the previous
// summary to update (if any), then the conversation (transcript).
static Str encodesummary(Arena *a, Arena scratch, ModelConf *mc, Str prev,
                         Str convo, i64 words)
{
    Buf u(&scratch, convo.len + prev.len + 4096);
    print(&u, "Summarize the conversation below for the agent that will "
              "continue it. That agent will see only your summary and the "
              "most recent messages, so keep everything it needs to carry "
              "on, and leave out what it does not. Respond with text only, "
              "do not call tools.\n\n");
    print(&u, compactsections);
    print(&u, "\nKeep it under ");
    print(&u, words);
    print(&u, " words.\n");
    if (prev.len) {
        print(&u, "\nThis updates an earlier summary: keep what still holds, "
                  "revise what changed, and drop what no longer matters.\n\n"
                  "<previous-summary>\n");
        print(&u, prev);
        print(&u, "\n</previous-summary>\n");
    }
    print(&u, "\n<conversation>\n");
    print(&u, convo);
    print(&u, "</conversation>\n");

    Buf     b(a, u.len + 1024);
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "model");
    putstr(&w, mc->name);
    putkey(&w, "messages");
    putarray(&w);
    putobject(&w);
    putkey(&w, "role");
    putstr(&w, "system");
    putkey(&w, "content");
    putstr(&w, compactsystem);
    putend(&w);
    putobject(&w);
    putkey(&w, "role");
    putstr(&w, "user");
    putkey(&w, "content");
    putstr(&w, finish(&u));
    putend(&w);
    putend(&w);
    putkey(&w, "stream");
    putbool(&w, 1);
    putkey(&w, "stream_options");
    putobject(&w);
    putkey(&w, "include_usage");
    putbool(&w, 1);
    putend(&w);
    if (mc->llamacpp) {
        putkey(&w, "return_progress");
        putbool(&w, 1);
        putkey(&w, "reasoning_control");
        putbool(&w, 1);
    }
    for (iz i = 0; i < mc->extras.len; i++) {
        putkey(&w, mc->extras[i].key);
        putraw(&w, mc->extras[i].json);
    }
    putend(&w);
    return finish(&b);
}

// The user message that stands for the compacted messages.
static Str checkpoint(Arena *a, Str summary)
{
    Buf b(a, summary.len + 512);
    print(&b, "<conversation-checkpoint>\n"
              "The earlier part of this conversation was compacted into the "
              "summary below. It records the work so far and is not a new "
              "request: carry on from the messages that follow it, or from "
              "its next steps if none do.\n"
              "<summary>\n");
    print(&b, summary);
    print(&b, "\n</summary>\n</conversation-checkpoint>");
    return finish(&b);
}


// Files read and written, kept by the harness across compactions as the
// last section of each summary, one file to a line

static Str const filesheading = "\n\n## Files (kept by the harness)\n";

struct Files {
    Slice<Str> read;
    Slice<Str> wrote;
};

// A summary without the harness's section.
static Str summarytext(Str summary)
{
    iz at = find(summary, filesheading);
    return at<0 ? summary : takehead(summary, at);
}

// Move or add a path to the end of a list, keeping the newest few.
static Slice<Str> addfile(Arena *a, Slice<Str> list, Str path)
{
    Slice<Str> r = {};
    iz first = list.len>=COMPACT_FILES ? list.len-COMPACT_FILES+1 : 0;
    for (iz i = first; i < list.len; i++) {
        if (list[i] != path) {
            r = push(a, r, list[i]);
        }
    }
    return push(a, r, path);
}

// The lists of the section at the end of a summary.
static Files summaryfiles(Arena *a, Str summary)
{
    Files f  = {};
    iz    at = find(summary, filesheading);
    if (at < 0) {
        return f;
    }
    for (Str rest = cuthead(summary, at+filesheading.len); rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        if (startswith(c.head, "- read ")) {
            f.read = addfile(a, f.read, cuthead(c.head, 7));
        } else if (startswith(c.head, "- wrote ")) {
            f.wrote = addfile(a, f.wrote, cuthead(c.head, 8));
        }
    }
    return f;
}

// Add the files the calls in h[beg, end) read, wrote and edited.
static Files addfiles(Arena *a, Files f, Slice<Msg> h, iz beg, iz end)
{
    for (iz i = beg; i < end; i++) {
        for (iz k = 0; k < h[i].blocks.len; k++) {
            Block *b = &h[i].blocks[k];
            b32    r = b->name == "read";
            if (b->type!=BLOCK_CALL || (!r && b->name!="write" && b->name!="edit")) {
                continue;
            }
            Repair rp    = repairargs(a, b->args);
            Str    path  = jsonstr(jsonget(rp.root, "path"));
            Json  *patch = jsonget(rp.root, "patch");
            if (!r && patch->type==JSON_STRING) {
                // An edit as a patch, which names its files inside
                Slice<Str> paths = patchpaths(a, patch->str);
                for (iz p = 0; p < paths.len; p++) {
                    f.wrote = addfile(a, f.wrote, clone(a, paths[p]));
                }
                continue;
            } else if (!path.len || cut(path, '\n').ok) {
                continue;
            } else if (r) {
                f.read = addfile(a, f.read, clone(a, path));
            } else {
                f.wrote = addfile(a, f.wrote, clone(a, path));
            }
        }
    }
    return f;
}

// A summary's text with the files section after it.
static Str withfiles(Arena *a, Str text, Files f)
{
    if (!f.read.len && !f.wrote.len) {
        return text;
    }
    Buf b(a, text.len + 1024);
    print(&b, text);
    print(&b, filesheading);
    for (iz i = 0; i < f.wrote.len; i++) {
        print(&b, "- wrote ");
        print(&b, f.wrote[i]);
        putbyte(&b, '\n');
    }
    for (iz i = 0; i < f.read.len; i++) {
        print(&b, "- read ");
        print(&b, f.read[i]);
        putbyte(&b, '\n');
    }
    b.len--;  // the last newline
    return finish(&b);
}

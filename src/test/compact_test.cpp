// Compaction tests: thresholds, estimates, pruning, the cut, the
// summarizer's request, and the files kept across summaries

static Msg testmsg(Arena *a, i32 role, iz bytes, Str name)
{
    Msg m = {};
    m.role = role;
    if (role == ROLE_ASSISTANT) {
        Block b = {};
        b.type = BLOCK_CALL;
        b.name = name;
        b.args = "{\"path\":\"x.c\"}";
        b.id   = "c";
        m.blocks = push(a, m.blocks, b);
    } else {
        Buf t(a, bytes);
        for (iz i = 0; i < bytes; i++) {
            putbyte(&t, (u8)('a' + i%26));
        }
        m.text = finish(&t);
    }
    return m;
}

static void test_compact(Test *t)
{
    Arena *a   = &t->perm;
    Arena  tmp = *a;  // scratch, apart from what a holds
    tmp.beg += (tmp.end - tmp.beg)/2;

    // Thresholds: max(15%, 16384) kept free, or 15% of a small window
    struct { i64 w, want; } at[] = {
        {262144, 262144 - 39321},
        {65536,  65536 - 16384},
        {32768,  32768 - 16384},
        {24000,  24000 - 3600},
        {8000,   8000 - 1200},
    };
    for (iz i = 0; i < countof(at); i++) {
        expect(t, concat(a, "compactat ", istr(a, at[i].w)),
               istr(a, compactat(at[i].w)), istr(a, at[i].want));
    }
    expect(t, "sumbudget small", istr(a, sumbudget(2000)), "1024");
    expect(t, "sumbudget mid", istr(a, sumbudget(40000)), "10000");
    expect(t, "sumbudget cap", istr(a, sumbudget(1000000)), "16384");

    // Estimates: 4 bytes a token until the server says otherwise
    Rate none = {};
    Rate half = {500, 1000};
    expect(t, "tokens by default", istr(a, tokensof(none, 4000)), "1000");
    expect(t, "tokens by rate", istr(a, tokensof(half, 4000)), "2000");
    expect(t, "bytes by rate", istr(a, bytesof(half, 2000)), "4000");

    // The cut: newest messages up to keep tokens, never at a result
    {
        Slice<Msg> h = {};
        h = push(a, h, testmsg(a, ROLE_USER, 4000, {}));        // 0
        h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "read"));  // 1
        h = push(a, h, testmsg(a, ROLE_TOOL, 4000, {}));        // 2
        h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "read"));  // 3
        h = push(a, h, testmsg(a, ROLE_TOOL, 4000, {}));        // 4
        h = push(a, h, testmsg(a, ROLE_USER, 400, {}));         // 5
        expect(t, "cut keeps a step", istr(a, cutpoint(h, 0, none, 1150)), "3");
        expect(t, "cut not at a result", istr(a, cutpoint(h, 0, none, 1110)), "5");
        expect(t, "cut keeps nothing", istr(a, cutpoint(h, 0, none, 50)), "6");
        expect(t, "cut keeps all", istr(a, cutpoint(h, 0, none, 100000)), "0");
        expect(t, "cut from start", istr(a, cutpoint(h, 2, none, 100000)), "3");
    }

    // Pruning: the newest 40k tokens of output stay, small ones stay,
    // and only a saving of 20k is worth it
    {
        Slice<Msg> h = {};
        for (i32 i = 0; i < 12; i++) {
            h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "read"));
            h = push(a, h, testmsg(a, ROLE_TOOL, i==2 ? 100 : 40000, {}));
        }
        h[1].text = elision(a, 10000, "/s/out/c.log");
        Prune pr = pickprune(a, h, none);
        Buf   b(a, 64);
        for (iz i = 0; i < pr.idx.len; i++) {
            print(&b, (i64)pr.idx[i]);
            putbyte(&b, ' ');
        }
        expect(t, "prune picks", finish(&b), "15 13 11 9 7 3 ");
        expect(t, "prune saves", istr(a, pr.saved > 50000), "1");
        Slice<Msg> few = {h.data+16, 8, 8};
        expect(t, "prune not worth it", istr(a, pickprune(a, few, none).idx.len), "0");
        expect(t, "elision", h[1].text,
               "[output elided: 10000 tokens; full output in /s/out/c.log]");
        expect(t, "elided", istr(a, iselided(h[1].text)), "1");
    }

    // The summarizer's input: reasoning left out, long texts clipped, the
    // newest part only when limited
    {
        Slice<Msg> h = {};
        Msg u = {};
        u.role = ROLE_USER;
        u.text = "Fix the bug.";
        h = push(a, h, u);
        Msg m = {};
        m.role = ROLE_ASSISTANT;
        Block k[3] = {};
        k[0].type = BLOCK_THINK;
        k[0].text = "secret thoughts";
        k[1].type = BLOCK_TEXT;
        k[1].text = "Reading it.";
        k[2].type = BLOCK_CALL;
        k[2].name = "read";
        k[2].args = "{\"path\":\"a.c\"}";
        m.blocks = {k, 3, 3};
        h = push(a, h, m);
        h = push(a, h, testmsg(a, ROLE_TOOL, 10000, {}));
        Str all  = transcript(a, tmp, h, 0, 3, 0);
        Str head = "[user]\nFix the bug.\n\n[assistant]\nReading it.\n\n"
                   "[tool call: read]\n{\"path\":\"a.c\"}\n\n[tool result]\n";
        expect(t, "transcript head", takehead(all, head.len), head);
        expect(t, "transcript no reasoning", istr(a, find(all, "secret")), "-1");
        expect(t, "transcript clipped", istr(a, find(all, " bytes omitted ...]") > 0), "1");
        expect(t, "transcript bounded", istr(a, all.len < 4000), "1");
        Str newest = transcript(a, tmp, h, 0, 3, 3150);
        expect(t, "transcript newest",
               takehead(newest, 42), "[earlier messages omitted]\n\n[tool result]\n");
        expect(t, "transcript newest end",
               istr(a, endswith(newest, taketail(all, 40))), "1");
    }

    // The request: text only, the template, the previous summary
    {
        ModelConf mc = {};
        mc.name     = "m";
        mc.llamacpp = 1;
        Str     body = encodesummary(a, tmp, &mc, "old", "[user]\nhi\n\n", 300);
        JsonDoc d    = jsonparse(a, body);
        expect(t, "summary request parses", istr(a, d.ok), "1");
        expect(t, "summary no tools",
               istr(a, jsonget(d.root, "tools")->type == JSON_MISSING), "1");
        Json *msgs = jsonget(d.root, "messages");
        expect(t, "summary system", takehead(jsonstr(jsonget(jsonat(msgs, 0), "content")), 30),
               "You summarize a coding session");
        Str user = jsonstr(jsonget(jsonat(msgs, 1), "content"));
        expect(t, "summary text only", istr(a, find(user, "do not call tools") > 0), "1");
        expect(t, "summary sections", istr(a, find(user, "### Blocked\n") > 0), "1");
        expect(t, "summary words", istr(a, find(user, "under 300 words") > 0), "1");
        expect(t, "summary previous",
               istr(a, find(user, "<previous-summary>\nold\n</previous-summary>") > 0), "1");
        expect(t, "summary conversation",
               istr(a, endswith(user, "<conversation>\n[user]\nhi\n\n</conversation>\n")), "1");
        expect(t, "summary llama.cpp", istr(a, find(body, "\"reasoning_control\":true") > 0), "1");
    }

    // Checkpoints and files
    {
        expect(t, "checkpoint", checkpoint(a, "S"),
               "<conversation-checkpoint>\nThe earlier part of this "
               "conversation was compacted into the summary below. It records "
               "the work so far and is not a new request: carry on from the "
               "messages that follow it, or from its next steps if none do.\n"
               "<summary>\nS\n</summary>\n</conversation-checkpoint>");
        Slice<Msg> h = {};
        h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "read"));
        h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "edit"));
        h = push(a, h, testmsg(a, ROLE_ASSISTANT, 0, "shell"));
        Str prev = "## Goal\nG\n\n## Files (kept by the harness)\n- wrote y.c\n- read x.c";
        expect(t, "summarytext", summarytext(prev), "## Goal\nG");
        Files f = summaryfiles(a, prev);
        f = addfiles(a, f, h, 0, h.len);
        expect(t, "withfiles", withfiles(a, "T", f),
               "T\n\n## Files (kept by the harness)\n- wrote y.c\n- wrote x.c\n"
               "- read x.c");
        expect(t, "withfiles none", withfiles(a, "T", {}), "T");
        Slice<Str> many = {};
        for (i32 i = 0; i < COMPACT_FILES+5; i++) {
            many = addfile(a, many, istr(a, i));
        }
        many = addfile(a, many, "3");
        expect(t, "files kept", istr(a, many.len), istr(a, COMPACT_FILES));
        expect(t, "files newest", many[many.len-1], "3");
    }
}

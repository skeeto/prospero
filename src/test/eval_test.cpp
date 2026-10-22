// Eval tests: what edits and calls record as forgiven, diffs, suites,
// and the summary's numbers and choices

static Str diffof(Arena *a, Str old, Str now)
{
    Buf b(a, 256);
    evaldiff(&b, *a, "f.txt", old, now);
    return finish(&b);
}

// A results line, as evalfinish writes one, of the fields the summary
// reads: a run of task tN, repetition rep, with fails failed edits, all
// of them misses.
static Str evalline(Arena *a, Str model, Str format, i32 task, i32 rep, b32 pass,
                    i64 steps, i64 fails, i64 in)
{
    Buf name(a, 16);
    putbyte(&name, 't');
    print(&name, (i64)task);
    Buf     b(a, 512);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "suite", "s");
    putstrkey(&w, "task", finish(&name));
    putstrkey(&w, "endpoint", "local");
    putstrkey(&w, "model", model);
    putstrkey(&w, "format", format);
    putintkey(&w, "rep", rep);
    putkey(&w, "pass");
    putbool(&w, pass);
    putstrkey(&w, "why", "done");
    putintkey(&w, "steps", steps);
    putintkey(&w, "calls", steps - 1);
    putkey(&w, "errors");
    putobject(&w);
    putintkey(&w, "miss", fails);
    putend(&w);
    putintkey(&w, "edit_fails", fails);
    putkey(&w, "tokens");
    putobject(&w);
    putintkey(&w, "in", in);
    putintkey(&w, "out", 10);
    putend(&w);
    putintkey(&w, "wall_ms", 2000);
    putintkey(&w, "queued_ms", 500);
    putend(&w);
    return finish(&b);
}

static void test_eval(Test *t)
{
    Arena *a   = &t->perm;
    Arena  tmp = *a;  // scratch, apart from what a holds
    tmp.beg += (tmp.end - tmp.beg)/2;

    // What edits forgave, as tool.end's repair names it
    {
        EditOut o = editreplace(a, "x = 1;\ny = 2;\n", "f.c", "y = 2;   \n", "y = 3;\n", 0);
        expect(t, "repair rtrim", editrepair(a, o.rung, o.forgave),
               "matched ignoring trailing whitespace");
        o = editreplace(a, "x = 1;\n", "f.c", "x = 1;", "x = 2;", 0);
        expect(t, "repair exact", istr(a, !editrepair(a, o.rung, o.forgave).data), "1");
        o = editreplace(a, "\tx = 1;\n", "f.c", "    x = 1;\n", "    x = 2;\n", 0);
        expect(t, "repair trim", editrepair(a, o.rung, o.forgave), "matched ignoring indentation");

        Patch p = parsepatch(a, "cat <<'EOF'\n*** Begin Patch\n*** Update File: a\n-x\n+y\n"
                                "*** End Patch\nEOF\n");
        expect(t, "forgave wrap", istr(a, p.forgave), istr(a, FORGAVE_WRAP));
        p = parsepatch(a, "*** Update File: a\nkept\n-x\n+y\n");
        expect(t, "forgave envelope", istr(a, p.forgave), istr(a, FORGAVE_ENVELOPE|FORGAVE_BARE));
        p = parsepatch(a, "*** Begin Patch\n*** Update File: a\n\n-x\n+y\n*** End Patch");
        expect(t, "an empty kept line is no repair", istr(a, p.forgave), "0");
        p = parsepatch(a, "*** Begin Patch\n*** Update File: a\n@@ b\n-b\n+B\n*** End Patch");
        o = patchupdate(a, "a\nb\nc\n", "a", &p.files[0]);
        expect(t, "hunk at its @@", concat(a, o.data, editrepair(a, o.rung, o.forgave)),
               "a\nB\nc\na hunk at its own @@ line");
        expect(t, "repair phrases", editrepair(a, RUNG_NORM, FORGAVE_ENVELOPE|FORGAVE_EOFLINE),
               "matched with typographic characters as ASCII, no *** Begin Patch, "
               "an empty last line for the final newline");

        Block c = {};
        c.args = "\"{\\\"path\\\":\\\"x\\\"}\"";
        expect(t, "call repair", callrepair(a, &c, "matched approximately"),
               "arguments were a JSON string holding the object; matched approximately");
        c.args = "{\"path\":\"x\"}";
        expect(t, "call repair none", istr(a, !callrepair(a, &c, {}).data), "1");

        Slice<EvalCount> n = {};
        n = evalfixes(a, n, "a bracket at the end was fixed; matched ignoring "
                            "indentation, no *** Begin Patch");
        n = evalfixes(a, n, "matched ignoring indentation");
        Buf b(a, 256);
        for (iz i = 0; i < n.len; i++) {
            print(&b, n[i].what);
            putbyte(&b, '=');
            print(&b, n[i].n);
            putbyte(&b, '|');
        }
        expect(t, "repair counts", finish(&b), "a bracket at the end was fixed=1|"
               "matched ignoring indentation=2|no *** Begin Patch=1|");
    }

    // Diffs
    expect(t, "diff same", diffof(a, "a\nb\n", "a\nb\n"), "--- a/f.txt\n+++ b/f.txt\n");
    expect(t, "diff change", diffof(a, "1\n2\n3\n4\n5\n6\n7\n8\n", "1\n2\n3\n4\nX\n6\n7\n8\n"),
           "--- a/f.txt\n+++ b/f.txt\n@@ -2,7 +2,7 @@\n 2\n 3\n 4\n-5\n+X\n 6\n 7\n 8\n");
    expect(t, "diff two hunks",
           diffof(a, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n", "X\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\nY\n"),
           "--- a/f.txt\n+++ b/f.txt\n@@ -1,4 +1,4 @@\n-1\n+X\n 2\n 3\n 4\n"
           "@@ -9,4 +9,4 @@\n 9\n 10\n 11\n-12\n+Y\n");
    expect(t, "diff merged", diffof(a, "1\n2\n3\n4\n5\n6\n7\n8\n", "X\n2\n3\n4\n5\n6\n7\nY\n"),
           "--- a/f.txt\n+++ b/f.txt\n@@ -1,8 +1,8 @@\n-1\n+X\n 2\n 3\n 4\n 5\n 6\n 7\n-8\n+Y\n");
    expect(t, "diff added", diffof(a, {}, "hi\n"), "--- /dev/null\n+++ b/f.txt\n@@ -0,0 +1 @@\n+hi\n");
    expect(t, "diff removed", diffof(a, "a\nb\n", {}),
           "--- a/f.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-a\n-b\n");
    expect(t, "diff final newline", diffof(a, "a\nb", "a\nb\n"),
           "--- a/f.txt\n+++ b/f.txt\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n");
    expect(t, "diff lcs", diffof(a, "a\nb\nc\nd\n", "a\nc\nb\nd\n"),
           "--- a/f.txt\n+++ b/f.txt\n@@ -1,4 +1,4 @@\n a\n-b\n c\n+b\n d\n");
    expect(t, "diff binary", diffof(a, Str{(u8 *)"a\0b", 3}, "ab"),
           "Binary files a/f.txt and b/f.txt differ\n");

    // Trees: a diff of every file that differs, .git aside, and expect
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, tmp, "/s");
        os_mkdirs(os, tmp, "/s/sub");
        os_mkdirs(os, tmp, "/s/.git");
        os_mkdirs(os, tmp, "/w");
        os_mkdirs(os, tmp, "/w/sub");
        os_mkdirs(os, tmp, "/w/.git");
        os_write(os, tmp, "/s/a.txt", "same\n");
        os_write(os, tmp, "/w/a.txt", "same\n");
        os_write(os, tmp, "/s/sub/b.txt", "old\n");
        os_write(os, tmp, "/w/sub/b.txt", "new\n");
        os_write(os, tmp, "/s/gone.txt", "bye\n");
        os_write(os, tmp, "/w/new.txt", "hi\n");
        os_write(os, tmp, "/w/.git/HEAD", "ref\n");
        expect(t, "tree diff", evaltreediff(os, a, tmp, "/s", "/w", 1<<16),
               "--- a/gone.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-bye\n"
               "--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1 @@\n+hi\n"
               "--- a/sub/b.txt\n+++ b/sub/b.txt\n@@ -1 +1 @@\n-old\n+new\n");
        expect(t, "tree diff cut", evaltreediff(os, a, tmp, "/s", "/w", 20),
               "--- a/gone.txt\n+++ /\n[the diff is cut here]\n");
        expect(t, "expect differs", evalexpect(os, a, tmp, "/s", "/w"), "gone.txt is missing");
        os_remove(os, tmp, "/s/gone.txt");
        expect(t, "expect differs", evalexpect(os, a, tmp, "/s", "/w"), "sub/b.txt differs");
        os_write(os, tmp, "/s/sub/b.txt", "new\n");
        expect(t, "expect holds", istr(a, !evalexpect(os, a, tmp, "/s", "/w").data), "1");

        Str why = evalpatch(os, a, tmp, "*** Begin Patch\n*** Update File: sub/b.txt\n@@\n"
                            "-new\n+NEW\n*** Add File: c/d.txt\n+made\n*** End Patch\n", "/w", 0);
        Read b = os_read(os, a, "/w/sub/b.txt", 100);
        Read d = os_read(os, a, "/w/c/d.txt", 100);
        expect(t, "patch applied", concat(a, concat(a, b.data, d.data), istr(a, !why.data)),
               "NEW\nmade\n1");
        why = evalpatch(os, a, tmp, "*** Begin Patch\n*** Add File: ../x.txt\n+no\n"
                        "*** End Patch\n", "/w", 0);
        expect(t, "patch outside", why, "the patch names a file outside the working "
               "directory: ../x.txt");
        expect(t, "patch outside left nothing", istr(a, os_stat(os, tmp, "/x.txt").type),
               istr(a, STAT_MISSING));
    }

    // A suite: suite.ini under each task, paths from the file that set
    // them, and a task's own prompt, of either key, over suite.ini's
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, tmp, "/b");
        os_mkdirs(os, tmp, "/b/suite");
        os_mkdirs(os, tmp, "/b/suite/fix");
        os_mkdirs(os, tmp, "/b/suite/fix/.git");
        os_mkdirs(os, tmp, "/b/common");
        os_write(os, tmp, "/b/suite/fix/.git/HEAD", "ref: refs/heads/master\n");
        os_write(os, tmp, "/b/suite/suite.ini", "timeout = 30\ntools = read edit\n"
                                                "expect = ../common\n"
                                                "prompt_file = common.md\n");
        os_write(os, tmp, "/b/suite/common.md", "From the suite.\n");
        os_write(os, tmp, "/b/suite/three.ini", "check = true\n");
        os_write(os, tmp, "/b/suite/two.ini", "prompt = \"Do # two\"\nfixture = fix\n"
                                              "steps = 7\ncheck = true\n");
        os_write(os, tmp, "/b/suite/one.ini", "prompt_file = one.md\ntimeout = 5\n"
                                              "patch = one.patch\n");
        os_write(os, tmp, "/b/suite/one.md", "  Do one.\n");
        os_write(os, tmp, "/b/suite/one.patch", "*** Begin Patch\n*** Add File: x\n+x\n"
                                                "*** End Patch\n");
        Slice<EvalTask> ts = {};
        b32 ok = evalsuite(os, a, tmp, "/b/suite", {}, &ts);
        expect(t, "suite loads", istr(a, ok*10 + ts.len), "13");
        Buf b(a, 512);
        for (iz i = 0; ok && i < ts.len; i++) {
            EvalTask *k = &ts[i];
            print(&b, k->suite);
            putbyte(&b, ':');
            print(&b, k->name);
            putbyte(&b, ':');
            print(&b, k->prompt);
            putbyte(&b, ':');
            print(&b, k->fixture.data ? k->fixture : Str("-"));
            putbyte(&b, ':');
            print(&b, k->expect);
            putbyte(&b, ':');
            print(&b, k->timeout);
            putbyte(&b, ':');
            print(&b, k->steps);
            putbyte(&b, ':');
            print(&b, k->tools);
            putbyte(&b, ':');
            print(&b, k->patch.data ? Str("patch") : Str("-"));
            putbyte(&b, '|');
        }
        expect(t, "suite tasks", finish(&b),
               "suite:one:Do one.:-:/b/common:5:100:read edit:patch|"
               "suite:three:From the suite.:-:/b/common:30:100:read edit:-|"
               "suite:two:Do # two:/b/suite/fix:/b/common:30:7:read edit:-|");
        Slice<Str> only = {};
        only = push(a, only, Str("two"));
        Slice<EvalTask> some = {};
        evalsuite(os, a, tmp, "/b/suite", only, &some);
        expect(t, "suite only", istr(a, some.len==1 && some[0].name=="two"), "1");

        // One file with both keys, and a fixture with a .git file (a
        // worktree's), which a copy would share: no suite
        os_mkdirs(os, tmp, "/b/both");
        os_write(os, tmp, "/b/both/x.ini", "prompt = a\nprompt_file = p.md\ncheck = true\n");
        os_write(os, tmp, "/b/both/p.md", "b\n");
        Slice<EvalTask> none = {};
        expect(t, "suite both prompts", istr(a, evalsuite(os, a, tmp, "/b/both", {}, &none)),
               "0");
        os_mkdirs(os, tmp, "/b/wt");
        os_mkdirs(os, tmp, "/b/wt/fix");
        os_mkdirs(os, tmp, "/b/wt/fix/sub");
        os_write(os, tmp, "/b/wt/fix/sub/.git", "gitdir: /elsewhere/.git/worktrees/x\n");
        os_write(os, tmp, "/b/wt/x.ini", "prompt = a\nfixture = fix\ncheck = true\n");
        expect(t, "suite worktree fixture",
               istr(a, evalsuite(os, a, tmp, "/b/wt", {}, &none)), "0");
        expect(t, "copy refuses a .git file",
               evalcopy(os, a, tmp, "/b/wt/fix", "/b/wt/copy", 0), "/b/wt/fix/sub/.git");
        expect(t, "a start copy skips it",
               istr(a, !evalcopy(os, a, tmp, "/b/wt/fix", "/b/wt/start", 1).data &&
                       os_stat(os, tmp, "/b/wt/start/sub/.git").type==STAT_MISSING), "1");
    }

    // Numbers as the summary prints them
    {
        Buf b(a, 256);
        evalmean(&b, 1, 3, 2);
        putbyte(&b, ' ');
        evalmean(&b, 2, 3, 2);
        putbyte(&b, ' ');
        evalmean(&b, 13898, 1, 0);
        putbyte(&b, ' ');
        evalmean(&b, 3900, 40, 1);
        putbyte(&b, ' ');
        evalmean(&b, 0, 0, 1);
        expect(t, "means", finish(&b), "0.33 0.67 13,898 97.5 0.0");

        // Sign tests: 2 P(X <= min) of a fair coin's n tosses
        Buf p(a, 256);
        f64 ps[] = {
            evalsign(7, 0), evalsign(0, 7), evalsign(4, 0), evalsign(0, 0),
            evalsign(5, 5), evalsign(20, 5), evalsign(3000, 0), evalsign(2000, 2000),
            evalsign(1, 0), 0.0004, 0.5,
        };
        for (iz i = 0; i < countof(ps); i++) {
            evalp(&p, ps[i]);
            putbyte(&p, '|');
        }
        expect(t, "sign tests", finish(&p), "p = 0.016|p = 0.016|p = 0.125|p = 1|"
               "p = 1|p = 0.004|p < 0.001|p = 1|p = 1|p < 0.001|p = 0.5|");
        expect(t, "sign test exact", istr(a, evalsign(7, 0)==1.0/64 &&
               evalsign(20, 5)==2.0*68406/33554432), "1");
    }

    // The format a model gets, and why
    {
        Slice<Str> lines = {};
        for (i32 i = 0; i < 4; i++) {
            lines = push(a, lines, evalline(a, "m", "replace", i, 1, 1, 4, 1, 400));
            lines = push(a, lines, evalline(a, "m", "patch", i, 1, i>0, 3, 1, 300));
            lines = push(a, lines, evalline(a, "m", "hashline", i, 1, 1, 3, 0, 300));
            lines = push(a, lines, evalline(a, "n", "replace", i, 1, 0, 3, 1, 300));
            lines = push(a, lines, evalline(a, "n", "hashline", i, 1, 0, 3, 1, 300));
        }
        lines = push(a, lines, Str("{\"endpoint\":\"local\",\"model\":\"m\","
                                   "\"format\":\"patch\",\"task\":\"t0\",\"rep\":2,"
                                   "\"error\":\"the setup command exited with status 1\"}"));
        Str s = evalsummary(a, tmp, lines, {});
        expect(t, "summary row", istr(a, find(s, "| m | patch | 4 | 75.0% (3) | 3.0 | 2.0 "
               "| 4 | 0 | 0 | 0 | 300 | 0 | 10 | 0 | 1.5 |\n") >= 0), "1");
        expect(t, "summary rows in order", istr(a, find(s, "| m | replace") <
               find(s, "| m | patch") && find(s, "| m | patch") < find(s, "| m | hashline") &&
               find(s, "| m | hashline") < find(s, "| n | replace")), "1");
        expect(t, "summary errors", istr(a, find(s, "\n1 run could not be made") >= 0), "1");
        Str want = "\nm: keep its own format: none did measurably better at this "
                   "many runs\n\n"
                   "- hashline: 4/4 passed, 0.00 failed edits, 3.0 steps and 310 tokens a run\n"
                   "- replace: 4/4 passed, 1.00 failed edits, 4.0 steps and 410 tokens a run\n"
                   "- patch: 3/4 passed, 1.00 failed edits, 3.0 steps and 310 tokens a run\n\n"
                   "On the 4 runs of each format with the same tasks and repetitions, "
                   "by the 4 tasks (sign tests):\n\n"
                   "- hashline and replace: no measurable difference (the pass rates "
                   "p = 1, failed edits p = 0.125, steps p = 0.125, tokens p = 0.125).\n"
                   "- hashline and patch: no measurable difference (the pass rates "
                   "p = 1, failed edits p = 0.125, steps p = 1, tokens p = 1).\n"
                   "- replace and patch: no measurable difference (the pass rates "
                   "p = 1, failed edits p = 1, steps p = 0.125, tokens p = 0.125).\n";
        expect(t, "summary no choice", istr(a, find(s, want) >= 0 &&
               find(s, "[model m]") < 0), "1");
        expect(t, "summary none", istr(a, find(s, "\nn: none, since no run passed\n") >= 0 &&
               find(s, "[model n]") < 0), "1");
        EvalModel own = {};
        own.endpoint = "local";
        own.model    = "m";
        own.own      = "patch";
        s = evalsummary(a, tmp, lines, {&own, 1, 1});
        expect(t, "summary keeps its own", istr(a, find(s, "\nm: keep patch, its own "
               "format: none did measurably better at this many runs\n") >= 0), "1");

        // 40 tasks, 33 of them passed with patch: beyond chance, on 7
        // tasks against none (exact McNemar), where 95% intervals of the
        // pass rates overlap
        Slice<Str> far = {};
        for (i32 i = 0; i < 40; i++) {
            far = push(a, far, evalline(a, "m", "replace", i, 1, 1, 3, 0, 300));
            far = push(a, far, evalline(a, "m", "patch", i, 1, i>=7, 3, 0, 300));
        }
        s = evalsummary(a, tmp, far, {});
        expect(t, "beyond chance", istr(a, find(s, "\nm: replace\n") >= 0 &&
               find(s, "- replace over patch: the pass rate decides (better on 7, worse "
                       "on 0 tasks, p = 0.016).\n\n    [model m]\n    edit_format = "
                       "replace\n") >= 0), "1");

        // Equal pass rates, and one step more on 4 tasks of 40: no
        // measurable difference, so the model keeps its own format
        Slice<Str> tie = {};
        for (i32 i = 0; i < 40; i++) {
            tie = push(a, tie, evalline(a, "m", "replace", i, 1, 1, 3 + (i<4), 0, 300));
            tie = push(a, tie, evalline(a, "m", "hashline", i, 1, 1, 3, 0, 300));
        }
        own.own = "hashline";
        s = evalsummary(a, tmp, tie, {&own, 1, 1});
        expect(t, "a tie keeps its own", istr(a, find(s, "\nm: keep hashline, its own "
               "format") >= 0 && find(s, "steps p = 0.125") >= 0 &&
               find(s, "[model m]") < 0), "1");

        // Equal pass rates, and failed edits on 8 tasks of 10, twice each
        // (repetitions summed by task): fewer failed edits decide
        Slice<Str> fewer = {};
        for (i32 i = 0; i < 10; i++) {
            for (i32 r = 1; r <= 2; r++) {
                fewer = push(a, fewer, evalline(a, "m", "replace", i, r, 1, 3, i<8, 300));
                fewer = push(a, fewer, evalline(a, "m", "hashline", i, r, 1, 3, 0, 300));
            }
        }
        own.own = "hashline";
        s = evalsummary(a, tmp, fewer, {&own, 1, 1});
        expect(t, "fewer failed edits", istr(a, find(s, "\nm: hashline, its own format "
               "already\n") >= 0 && find(s, "On the 20 runs of each format with the same "
               "tasks and repetitions, by the 10 tasks") >= 0 &&
               find(s, "- hashline over replace: the pass rates (p = 1) cannot be told "
                       "apart, and fewer failed edits decide (better on 8, worse on 0 "
                       "tasks, p = 0.008).\n") >= 0 && find(s, "[model m]") < 0), "1");

        // Formats compared on the tasks both ran alone
        Slice<Str> some = {};
        some = push(a, some, evalline(a, "m", "replace", 0, 1, 1, 3, 0, 300));
        some = push(a, some, evalline(a, "m", "replace", 1, 1, 0, 3, 0, 300));
        some = push(a, some, evalline(a, "m", "patch", 0, 1, 1, 3, 0, 300));
        s = evalsummary(a, tmp, some, {});
        expect(t, "the tasks both ran", istr(a, find(s, "- patch: 1/1 passed") >= 0 &&
               find(s, "- replace: 1/1 passed") >= 0 &&
               find(s, "(sign tests; 1 run that not every format made left out)") >= 0),
               "1");
        Slice<Str> apart = {};
        apart = push(a, apart, evalline(a, "m", "replace", 0, 1, 1, 3, 0, 300));
        apart = push(a, apart, evalline(a, "m", "patch", 1, 1, 1, 3, 0, 300));
        s = evalsummary(a, tmp, apart, {});
        expect(t, "no tasks in common", istr(a, find(s, "\nm: no task was run in every "
               "format, so nothing compares them\n") >= 0 && find(s, "[model") < 0), "1");
    }
}

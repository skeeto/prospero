# Edit-format benchmark

Which edit format ([model] edit_format) should each model get? This
benchmark measures it on the models of the local router: `large`
(DeepSeek V4.1, remote), `qwen-flash` (Qwen3.8 Flash Next, IQ4_XS) and
`qwen` (Qwen3.8 27B, Q8_0), the last two local under llama.cpp.

## Method

`tests/bench/editbench.py` makes 40 tasks from real code: twelve of this
repository's source files (`src/wide.cpp`, `mailbox.cpp`, `sse.cpp`,
`admit.cpp`, `sched.cpp`, `instructions.cpp`, `compact.cpp`,
`guard.cpp`, `base.cpp`, `json.cpp`, `doc.cpp`, `layout.cpp`), and three
Python and three JavaScript modules in `tests/bench/fixtures/`. Each
task applies one mutation to one line of one file, picked at random
(seed 1) among the lines of a function that hold no comment or string:

| kind | mutation | tasks |
|---|---|---:|
| operator | a comparison, logical or + - operator swapped (`<` for `<=`, `&&` for `\|\|`) | 7 |
| literal | an integer literal one more | 7 |
| identifier | one use of a name replaced by another name from the function | 7 |
| delete | a statement deleted | 7 |
| duplicate | a statement written twice | 6 |
| swap | the first two arguments of a call swapped | 6 |

24 tasks are in C++, 10 in Python and 6 in JavaScript. The prompt states
the bug in one sentence that quotes the line, for example: "In
`src/sched.cpp`, in `hashbytes`, the line `for (iz k = 0; i <= len;
i++, k++) {` uses `<=` where the correct code uses `<`." It asks for
the smallest fix, leaving the rest of the file as it is. So the task is
mostly the edit itself: finding the line, then changing it exactly,
whitespace included, in the format the model was given.

Each run is a fresh `prospero run` in a directory that holds only the
mutated file, under the role `bench`: the tools read, edit, grep and
glob, and no shell or write, so that every change goes through the edit
tool. A run succeeds when the file is byte for byte the original
afterwards. Each task ran twice in each cell (model × format) for large
and qwen-flash, 80 runs a cell; qwen, a dense 27B model, ran each task
once, 40 runs a cell, since two runs would have taken about three hours
of the shared router. In all, 600 runs, several at a time, in a random
order that mixes the cells.

Tool calls count every call; failed edits count edit calls whose result
was an error (bad arguments, text not found, a stale tag, a patch that
did not apply). Tokens are the sums of the server's usage over a run's
requests; prompt tokens include the cached prefix. Wall time includes
queueing at the router, which other sessions shared during part of the
run, so it compares cells only roughly.

The runs used the harness at commit 1fd0294 (a Debug build), which added
the formats. The fixes after it, prompted by these runs, are under
"Changes since the run".

The benchmark is now a suite for `prospero eval`, tests/bench/editbench,
made by `editbench.py suite` from the sources of a later commit than the
run's, and kept as made since: its 40 tasks are of the same kinds, but
not the same lines as the ones measured here. Each run there is a fresh
session in a directory holding only the mutated file, with the same
tools, prompt and ten-minute limit, and with the project's instruction
files only, as the run here had none. Run it on the router's models with:

    prospero eval -e http://localhost/ -m large,qwen-flash,qwen -f all \
        -n 2 -j 6 -o results.jsonl tests/bench/editbench
    prospero eval --report results.jsonl

Its table has these columns and more: failed edits, counted as here
(every edit call that failed, a patch that does not parse included),
bad calls, denials and repairs by kind, cached tokens, and the time
spent waiting at the router apart from the runs' own. With six runs at
once and models taking turns at the router, runs of the model not loaded
wait in its queue, which no longer counts against their ten minutes.

## Results

| model | format | runs | success | tool calls | reads | failed edits | tokens in | tokens out | wall s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| large | replace | 80 | 100.0% (80) | 3.6 | 1.40 | 0 | 9,112 | 334 | 3 |
| large | patch | 80 | 100.0% (80) | 3.7 | 1.40 | 3 | 9,279 | 333 | 4 |
| large | hashline | 80 | 100.0% (80) | **3.1** | **1.05** | 1 | 8,823 | 296 | 3 |
| qwen-flash | replace | 80 | 98.8% (79) | 4.9 | 1.84 | 0 | 13,584 | 634 | 110 |
| qwen-flash | patch | 80 | 98.8% (79) | 5.0 | 2.06 | 7 | 15,090 | 681 | 117 |
| qwen-flash | hashline | 80 | **100.0% (80)** | **4.3** | **1.45** | 0 | 13,781 | 558 | 98 |
| qwen | replace | 40 | **100.0% (40)** | 3.2 | 1.12 | 0 | **9,735** | **404** | **131** |
| qwen | patch | 40 | 95.0% (38) | 3.9 | 1.38 | 6 | 11,289 | 497 | 158 |
| qwen | hashline | 40 | 97.5% (39) | 3.2 | 1.12 | 2 | 13,898 | 470 | 147 |

Tool calls, reads, tokens and wall time are means per run; failed edits
are totals per cell. By mutation kind, every cell solved every task of
the operator, literal, identifier, duplicate and swap kinds; all five
failures are deletions:

- Four are one task, `t21-delete`, whose deleted line is `u8  c =
  lowercase(s[k]);` with two aligned spaces: the model wrote one space
  (qwen-flash with replace and with patch, qwen with patch and with
  hashline). The format did not cause these, though hashline was the
  only one qwen-flash got right both times.
- One, qwen with patch on `t15-delete`, was the harness's fault: the
  model's kept line had lost its marker space, the harness took the
  indentation of the added line from that one line and indented it a
  space too far, again on each retry, until the run's ten minutes ran
  out. That is fixed (below).

What tells the formats apart is the cost of the same success:

- **hashline needs the fewest reads.** Its edit result shows the changed
  lines with their new numbers, so models rarely read the file again to
  check the edit: large made 1.05 reads a run with hashline against
  1.40 with the others, and qwen-flash 1.45 against 1.84 and 2.06. For
  both it also had the fewest calls and output tokens and the shortest
  wall time. qwen read no more often with replace than with hashline,
  and two of its hashline edits failed on a guessed tag.
- **patch has the most failed edits**, 16 in 200 runs, against 3 for
  hashline and none for replace. Of large's and qwen-flash's ten, half
  were a changed line written as a kept line (a space instead of - and
  +), which the harness refuses as changing nothing; the rest had lines
  that were not the file's (a kept line cut short, a hunk repeated out
  of order, a removed line misremembered), a made-up `@@ def` line in
  C++, or the line being changed given after `@@` as well. Five of
  qwen's six left out `*** Begin Patch` (one wrote `+++` headers
  instead), and the sixth sent again a patch already applied. Every
  failed edit was followed by one that worked, at the cost of another
  request.
- **replace never failed an edit** and was qwen's cheapest format.

## Choice

The router's presets, then in the seed configuration (assets/prospero.ini),
set `edit_format = hashline` for `large` and `qwen-flash`, as reliable as
the others here with fewer calls, tokens and seconds, and `replace` for
`qwen`, which was as cheap with replace and never failed an edit with it
(40 runs a cell, so the margin is thin). The seed no longer has those
presets: a configuration of one's own sets them.

This benchmark saturates: the tasks are one-line fixes with the line
quoted, and every format solves nearly all of them. oh-my-pi
downgrades DeepSeek models to replace on the evidence of harder
fixtures (find the bug, multi-line changes) where formats spread by up
to 60 points on weaker models; nothing here shows that for DeepSeek
V4.1. Multi-line mutations (a block deleted, lines reordered, a block
indented) and prompts that do not quote the line would test it, at the
cost of measuring bug-finding along with editing.

## Changes since the run

From what the runs showed, the harness now:

- votes on the indentation of a loose match: the pair (given, in the
  file) that most old lines agree on, no change on a tie, so that one
  kept line without its marker space cannot shift the added lines (the
  `t15-delete` timeout);
- lets a hunk start at its own `@@` line when nothing matches after it;
- accepts a patch without `*** Begin Patch` when a file's header comes
  first (four of qwen's five);
- lists the [PATH#TAG] of grep's files in hashline sessions, so that an
  edit can follow a grep without a read.

A spot check with these changes (qwen, `t15-delete` and `t11-swap`, two
runs each with patch and hashline) passed 8 of 8, and one hashline run
edited straight after its grep.

## Claude Haiku 5.5, through prospero eval (2026-10-22)

The suite in tests/bench/editbench, each task once in each format, four
runs at once, against Anthropic's API (`kind = anthropic`, adaptive
thinking, the defaults):

| model | format | runs | pass | steps | calls | edit misses | bad calls | denials | repairs | tokens in | cached | tokens out | retries | time s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| claude-haiku-5-5 | replace | 40 | 100.0% (40) | 3.8 | 2.9 | 0 | 0 | 0 | 0 | 8,700 | 7,349 | 439 | 0 | 5.4 |
| claude-haiku-5-5 | patch | 40 | 100.0% (40) | 4.0 | 3.2 | 0 | 0 | 0 | 0 | 10,200 | 8,733 | 483 | 0 | 6.0 |
| claude-haiku-5-5 | hashline | 40 | 100.0% (40) | 3.7 | 2.9 | 0 | 0 | 0 | 0 | 9,804 | 8,384 | 448 | 0 | 5.4 |

Every run passed in every format, with no failed edit and nothing for
the harness to forgive, so this model is no test of the formats. (The
column "edit misses" then counted misses alone; there were no failed
edits of any kind.) The summary of the time chose hashline on 0.1 steps
a run, and replace used the fewest tokens. A difference of 0.1 steps a
run, at most 4 tasks of 40, no longer decides: the summary now compares
formats task by task, by sign tests, and gives a format only when it
did better than each other beyond chance (p < 0.05), the model keeping
its own otherwise. A run of the suite as made before cost about
as much and also passed 120 of 120. The 240 runs took about 2.3 million
tokens in, 85% of them read from the cache, and 110 thousand out.

The old benchmark's literal and identifier mutations came out in the
order of Python sets, which follows the hash seed of each process, so
two invocations of `editbench.py run` could pick different lines for
the same task id. The runs above under "Results", made by one command,
had the same tasks in every cell, unless that command was stopped and
started again. The generator now sorts them, and makes the same suite
every time.

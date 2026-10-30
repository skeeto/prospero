# Development

    $ cmake -B build -DCMAKE_BUILD_TYPE=Debug
    $ cmake --build build
    $ ctest --test-dir build -j8

The suites:

- `unit`: the core on a platform layer with an in-memory file system
  and a fake clock (`src/main_test.cpp`).
- `selftest`: `prospero --selftest`, the real platform layer: threads,
  the file system, and processes. On macOS, `selftest_disclaimed` runs
  it again as its own responsible process, as an app runs.
- `scenarios`: the engine end to end against a fault-injecting fake
  provider (`tests/fake_provider.py`), and for the desktop tool, a fake
  helper (`tests/fake_desktop.py`). On Linux with Xvfb, the real X11
  helper too, on displays of their own (`tests/xprobe.py`), under each
  window manager installed of openbox, xfwm4, metacity, kwin_x11, icewm
  and fluxbox (`desktop_x11_wms`; with zenity, a GTK dialog too).
- `cleanup`: the harness killed hard while its tools run, which must
  leave no process behind.
- `remote`, `includes`, `deploy`: hosts and their clients, URL includes
  against a fake web server, and deploying through a fake ssh and scp.
- `desktop_unit`, `desktop` (macOS): the desktop helper's own layer
  (`src/test/desktop_macos_test.cpp`), and the helper itself, its
  commands and frames, with its permissions treated as missing, so that
  nothing touches the screen. `tests/desktop_live_macos.py` drives
  TextEdit for real, under supervision; its header says how, and
  `tests/desktop_live_wayland.py` drives GNOME Text Editor or Kate on a
  Wayland desktop. On Linux, the `desktop_x11` scenarios also run the
  helper as if under Wayland, against no session bus and against a
  `dbus-daemon` of their own.
- `bundle` (macOS): Prospero.app's bundle identifier and signatures,
  never the real app's in a build without a signing identity.
- `sectrust` (macOS): the system's trust in https servers, through the
  HTTP layer (`src/main_edgetest.cpp`), with certificates that openssl
  makes for the test and that are never installed. With `--net`, real
  servers too, badssl.com's bad ones among them.
- `arenalint`: the sources, for one arena given to a call both as the
  place for its results and, by value, as its scratch (`f(&a, a)`),
  and for a Buf left bound to a local arena (`tests/arenalint.py`).

Debug builds, and `unit` always, check arenas for aliasing as they run
(`src/arenacheck.cpp`; `-DPROSPERO_ARENACHECK=OFF` to leave it out): an
arena allocating again over bytes another one allocated over while it
held them, a copy allocating over what a live copy of the same arena
holds, or an allocation through an arena already destroyed. Each is
reported on stderr as an "arena fault", with where the arenas were
made, and traps; the suites fail on one.

Where the toolchain has AddressSanitizer (not Windows, nor a static
build), Debug builds also run under it (`-DPROSPERO_ASAN=OFF` to leave
it out), and arena memory that no live arena holds is poisoned: a
region's fresh space, a copy's allocations once the copy ends, and what
a restore gives back. ASan then reports a read of results left in a
callee's scratch, or of anything after its arena moved on.

The Python suites need [uv][]. They also run against other machines
from this one: `--host` runs Prospero on a Windows host over ssh,
`--xp` on Windows XP, and `--ssh` connects and deploys to a real
remote host. `tests/gui.py` drives the GUI through a scripted
checklist, which fails on a stale frame, and `tests/imports.py` checks
a headless Windows engine's imports, its desktop helper's among them,
against what XP exports (CTest runs it in those builds). The scenario
`desktop_xp`, run by name with `--xp` and `PROSPERO_TEST_XP_DESKTOP=1`,
drives Notepad on XP's desktop with the real helper. Each script's header says how to run it.

`tools/showcase.py` makes the state behind the README's screenshot: a
small C project and four sessions, with subagents, a plan, a todo list
and images, recorded against the fake provider. It then opens the GUI
on that state, and with `--shot` saves the window as a PNG (`prospero
gui --quit-after S --shot FILE`, SDL builds only):

    $ uv run tools/showcase.py --prospero build/Prospero.app --work ~/src/mandel build-showcase
    $ uv run tools/showcase.py --prospero build/Prospero.app build-showcase --open --shot docs/screenshot.png
    $ sips -Z 1440 docs/screenshot.png

`--theme dark` takes the same shot in dark colors, as
`docs/screenshot-dark.png`, which the itch.io page shows.

## Evals

`prospero eval` measures models on a suite of tasks: how often each
passes, and what it took, so that a model's edit format and the
harness's heuristics can be chosen from data. A suite is a directory of
task files, `NAME.ini`, with settings for all of them in `suite.ini`:

    prompt   = "Change wrold to world in greet.txt."
    fixture  = base           # copied as the working directory
    patch    = typo.patch     # a mutation, in the edit tool's patch format
    expect   = base           # these files, byte for byte, afterward
    check    = sh "$PROSPERO_EVAL_SUITE/test.sh"   # and exit 0
    timeout  = 600            # seconds, not counting the endpoint's queue
    steps    = 100            # model requests at most
    tools    = read edit grep glob

`setup` runs a command before the agent starts, `prompt_file` reads the
prompt from a file (a task's own `prompt` or `prompt_file` wins over
`suite.ini`'s), `role` names a configured role, and `check_timeout`
limits setup and check commands. Each run works on its own copy in a
temporary directory, so no repository is ever touched: a fixture may
hold a `.git` directory, but not a `.git` file (a worktree's or a
submodule's), which would have the copy share another repository. The
session is unattended, so asks are denied, gets the project's
instruction files but not the user's own, and never moves to a
fallback model; commands run without the variables that endpoints'
keys come from.

    prospero eval -m MODEL,... [-e ENDPOINT] [-f FORMAT,...|all] [-n N]
                  [-j N] [-t TASK,...] [-o FILE] [-c FILE] SUITE...
    prospero eval --report [-c FILE] FILE...

Every model runs every task in each edit format, `-n` times, up to `-j`
runs at once. A model is `MODEL` at the `-e` endpoint, or
`ENDPOINT/MODEL` with a configured endpoint. Model requests wait their
turn at each endpoint as any session's do (its `max_concurrent`,
`max_models`, and a model's `max_concurrent`), and runs go model by
model, so a llama.cpp router that keeps one model loaded swaps rarely.
Each run appends a JSON line to the results file (`-o`, by default
`eval.jsonl`): pass or fail and why, steps and tool calls, failed edits
(alike in every format) and tool errors by kind (edit misses, invalid
calls), guard denials, what the harness repaired, nudges, retries,
tokens with cache reads, the run's own time and its time in the queue,
and the diff it left. A run already in the file is skipped, so an eval
stopped part way goes on where it stopped. A run that measured no
model, because the model never answered (a key refused, an endpoint
down), is recorded as an error and made again next time; after three
such runs of a model in a row (or `-j`, if more), its runs not yet
started are left out, and the eval exits 1. The summary, printed after
the runs and by `--report`, is a table by model and format, the failed
runs, and for each model the edit format to give it, with the numbers
that decide it: formats are compared on the runs each made of the same
tasks, task by task, and one is recommended only if it did better than
every other beyond chance (sign tests, p < 0.05), on the pass rate,
else failed edits, steps or tokens. Otherwise the model keeps its own.

`tests/bench/editbench` is the edit-format benchmark as a suite: 40
one-line bugs in real code, to fix with the file tools alone. For a
llama.cpp router on this machine, and a model at Anthropic:

    $ prospero eval -e http://localhost/ -m MODEL1,MODEL2 -f all -n 2 \
          -o local.jsonl tests/bench/editbench
    $ prospero eval -m anthropic/claude-haiku-5-5 -f all -t t00-operator,t09-delete \
          -o claude.jsonl tests/bench/editbench

[uv]: https://docs.astral.sh/uv/

# Prospero: a desktop coding harness for local and remote LLMs

Prospero is a coding harness: a desktop program in which a language
model, the agent, works on code through tools. It runs the turn loop,
the tools (read, write, edit, shell, grep, glob, a todo list,
background jobs, subagents, and control of the
desktop), and the processes they start, and
shows the conversation as it streams. It speaks the OpenAI Chat
Completions API, so it works with llama.cpp, vLLM, LiteLLM, and hosted
providers with a compatible API, and Anthropic's Messages API, with
extended thinking and prompt caching (`[endpoint NAME] kind =
anthropic`). It is built
first for models served on your own machines, whose servers are slow
to start, run out of context, and stall: the harness, not the agent,
retries, compacts, queues requests at a busy server, and keeps a turn
from getting stuck.

It is one executable. The engine runs in the same process as the
window, and needs no daemon or server. The same executable runs
headless, on another machine over ssh, or in the background after its
window closes. Each session is an append-only log, from which it
recovers after a crash. Undo and rewind put back the files the harness
changed.

## Build

CMake 3.25 or later and a C++20 compiler with `#embed` (GCC 15 or
later, or a recent Clang):

    $ cmake -B build
    $ cmake --build build

The default build type is Release. Debug builds trap on undefined
behavior (UBSan), failed assertions included. Dependencies are fetched
at configure time, each pinned by URL and SHA-256: Dear ImGui (docking
branch), md4c, FreeType and SDL3 for the GUI, mbedTLS with
cpp-httplib for HTTP on macOS and Linux, and Wuffs's single-file
release for decoding images everywhere. To build offline, or with a
modified dependency, put its source under `deps/` (`deps/imgui`,
`deps/md4c`, `deps/freetype`, `deps/sdl3`, `deps/mbedtls`,
`deps/cpp-httplib`, and `deps/wuffs` holding `wuffs-v0.4.c`), and it
is used instead.

On Windows, build the same way from a [w64devkit][] shell, with CMake
and Ninja on the PATH:

    $ cmake -B build -G Ninja
    $ cmake --build build

The Windows GUI is its own Win32 layer over Direct3D 11, and HTTP goes
through WinHTTP, so it uses the system's certificates and proxies. The
toolchain files in `cmake/` cross-compile for Windows with Mingw-w64
(`Toolchain-Mingw64.cmake`, `Toolchain-Mingw32.cmake`), for Windows on
Linux with w64devkit's own GCC under Wine, as releases are built
(`Toolchain-W64devkit-Wine.cmake`), and for x86-64 Linux
(`Toolchain-Linux-x86_64.cmake`).

`-DPROSPERO_GUI=OFF` builds the headless engine alone, with no GUI
dependencies. On Windows it is CRT-free and imports only kernel32,
advapi32 and winhttp. The i686 build of it is the Windows XP engine:

    $ cmake -B build-xp -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw32.cmake \
            -DPROSPERO_GUI=OFF
    $ cmake --build build-xp

On macOS the build also wraps the executable as `Prospero.app` in the
build directory. There Prospero asks the system whether to trust each
server's certificate, as Safari does, so a root that a user or an
administrator added, such as a corporate proxy's, is trusted. On Linux, SDL3 loads X11, Wayland and GL at run time,
so the GUI build still runs headless where no display libraries exist.
With `-DPROSPERO_GUI=OFF`, `-DPROSPERO_STATIC=ON` links the headless
engine fully statically, as `tools/engines.py` does for the Linux
engines it builds.

## Releases

Each release on GitHub carries eight files:

- `prospero-VERSION.tar.gz`: the source, with every dependency unpacked
  under `deps/`, so that it builds with no network:

      $ tar xzf prospero-VERSION.tar.gz
      $ cmake -S prospero-VERSION -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      $ cmake --build build

- `prospero-VERSION-windows-x86_64.exe`: the Windows program, the GUI
  with its engine and desktop helper, all there is to install.
- `prospero-engine-VERSION-windows-x86_64.exe` and
  `prospero-engine-VERSION-windows-i686.exe`: headless engines for
  Windows, the i686 one for XP, each its own desktop helper too.
- `prospero-engine-VERSION-linux-x86_64` and
  `prospero-engine-VERSION-linux-aarch64`: headless engines for Linux,
  linked statically.
- `prospero-desktop-VERSION-linux-x86_64` and
  `prospero-desktop-VERSION-linux-aarch64`: the desktop helper for
  Linux (Desktop control). It loads X11 at run time, so it links the C
  library dynamically, and needs glibc 2.34 or later.

The engines and helpers are named as `prospero deploy` expects to find
them, in an `engines` directory next to the executable, which puts each
helper beside its engine on the remote machine. To use a helper on the
machine itself, save it next to `prospero` as `prospero-desktop`. There
is no macOS build; build it from source.

The release workflow (`.github/workflows/release.yml`) builds every
program from the source tarball alone. The Windows programs are built
on Linux by [w64devkit][]'s GCC under Wine, the same programs it builds
on Windows, stripped and with no timestamp, then signed, with an Authenticode signature (Azure Artifact Signing), which
Windows shows in the file's Properties, under Digital Signatures.
`tools/dist.py` makes
the source tarball from any commit, the same commit always giving the
same tar file.

GitHub records each file's SHA-256 digest, shown on the release page
next to the file. To check the files downloaded into the current
directory against them all at once:

    $ gh release view vVERSION -R skeeto/prospero --json assets \
          --jq '.assets[] | .digest[7:] + "  " + .name' |
          sha256sum -c --ignore-missing

### The engines zip

`tools/engines.py --docker --zip` builds every engine and helper into
one file, `prospero-engines-VERSION.zip`, with Docker, on any machine
it runs on: the Linux and Windows programs in an image of cross
compilers that `tools/toolchain/Dockerfile` builds from sources pinned
by SHA-256 (GCC 16 and binutils as [w64devkit][] has them, musl with
its security patches, mingw-w64, and Debian 10's glibc 2.28), and the
macOS engine on the Mac itself. All are optimized for size:

- the Linux engines link musl statically, so they run with any C
  library, or none;
- the Linux desktop helpers need glibc 2.28 or later;
- the Windows engines are the same CRT-free programs as above;
- the macOS engine, for Apple Silicon, needs macOS 11 or later.

The same sources build the same zip, byte for byte. Its `engines`
directory goes next to the program, or beside `prospero.ini` (see
Settings), which suits a macOS app in /Applications, whose signed
bundle stays as it is. Its README.txt lists the files.

## Usage

    prospero [gui [options]]
    prospero [gui] DIR [options]
    prospero run [options] PROMPT
    prospero run [options] --resume SID [PROMPT]
    prospero --headless [options]
    prospero serve [options]
    prospero attach [--reconnect] [options]
    prospero hosts [--state DIR]
    prospero connect NAME | --ssh DEST | --local [options]
    prospero deploy NAME|DEST [--force] [options]
    prospero eval [options] SUITE...
    prospero mcp list | auth NAME | logout NAME [options]

With no arguments, Prospero opens its window: sessions in a sidebar,
subagents under their parents; the transcript, with markdown, code
highlighting and selection; tool calls with Undo, and Rewind to any
message that began a turn; the todo list and background jobs; and the
prompt. The model's reasoning folds to one line that counts it as it
streams, which a click opens; `[ui] thinking = open` shows it whole,
and `hide` leaves it out of the transcript (the session log keeps it,
and the model still gets it back). Tool rows fold by kind and outcome,
or all of them with `[ui] tools = fold` or `open`, and a session's
Tool output and Reasoning buttons (a wrench and screwdriver, a light
bulb, above the transcript) open everything for that session alone. Cmd/Ctrl+F finds text in the transcript, folded rows included,
and a code block's Copy button copies its code exactly. A bar above the
transcript shows how full the context is. A file tool that goes outside the session's working directory,
or the roots the configuration adds, asks first, and an answer not
given in time denies it. The settings window edits the configuration
file in place, changing only the lines it sets, with a page for each
section. Endpoints, models, roles and remotes are added there, and
deleted, with the comment just above them, when your own file rather
than an included one defines them. Closing the window
while turns run asks whether to stop them, or to keep running in the
background, with a tray icon to bring the window back. One window
runs per state directory: launching Prospero again on the same one,
even while its window is in the background, brings that window
forward and exits, as does opening the app again on macOS. Under
Wayland a window already open may only ask for attention, as the
compositor decides focus. A different `--statedir` (or
`XDG_STATE_HOME`) is a separate instance. The View menu
shows and hides the panes and zooms the text for the run (Ctrl+=,
Ctrl+- and Ctrl+0, Cmd on macOS, or the wheel with the key held);
`[ui] font_size` (17 pixels by default) sets its size for good, and
View's Reset layout puts every pane back in its place. The window
opens where it was last, fitted to the displays there are; the first
time, 1700 by 900 at the display's scale, or maximized on a display
too small for that. The first run shows Help in front, with Settings
and Hosts when the configuration names no model and no remote host,
and none of its endpoints lists a model either; close them once read, as the View menu shows each again. Help → Help
shows a page on each topic, from getting started to troubleshooting,
built in from `assets/help`. Its search field (Ctrl+F while Help has the
focus) lists the sections that have every word typed, headings first,
and a click goes to that section.

`prospero DIR`, such as `prospero .` in a terminal, opens a new
session in that directory, in Build mode with the default model,
ready for a prompt. With no window open yet, the window goes on
detached and the shell has its prompt back, as with `code .`, its
errors going to `gui.log` in the state directory; the session has the
terminal's environment. With a window open already, the session starts
there, and that window comes forward: the session then has that
window's environment, not the terminal's. On macOS, opening a folder
with Prospero (`open -a Prospero DIR`, or dropping it on the Dock
icon) does the same. A folder dropped on the window fills the New
session dialog with it.

The sidebar is a tree: groups, then hosts, then sessions with their
subagents, and archived sessions folded away at the end. A session's
context menu, and the Session menu, rename it, move it to a group,
fork it, compact it now, switch its model, export it (Markdown or the
raw log), archive or delete it (with its subagents). The transcript's
context menu forks at a message: before a prompt, which then waits in
the fork's prompt box, or after a turn. A side chat, from that menu,
the Session menu or `/side`, is a fork made even while the agent
works, nested under its session, the selection quoted in its box; its
agent is told to leave files alone. The prompt box keeps a draft
per session, with the engine, has the session's earlier prompts on Up
and Down, opens the draft in an editor with Ctrl+G (`[ui] editor`,
else `$VISUAL` or `$EDITOR`), and takes `/compact`, `/model`,
`/rename`, `/fork`, `/side`, `/export`, `/plan`, `/background` and a skill's
`/NAME`. Run in background (Ctrl+B, Cmd+B on macOS) moves a running
command to a job, or a subagent at work on by itself, so that the
agent goes on and can be talked to; it is told when either ends, a
subagent's report coming as a notice that wakes the parent. An ask may be answered "always",
with the button set apart from Allow, which allows that directory, for
reads or for writes too, for the rest of the session and its subagents,
even after it is opened again. Each session has a scratch directory in the system's
temporary directory, which tools may use freely, as they may the rest
of it unless `[guard] temp = ask`. While you look elsewhere (another
app, the window minimized, another session shown), a desktop
notification tells of an ask waiting for you and of a session that
stopped by itself, and a click on it brings you there (`[ui] notify`).

Import session, in the Session menu and a host's menu, makes a new
session from a raw log exported here or on another machine, on the
host picked, in a working directory and with a model picked there. It
keeps the conversation (prompts, replies, tool calls and their results,
notes, compactions and the todo list) and drops everything that refers
to the old machine: allowed directories and grants, snapshots and
rewinds, spilled output files, images, subagents and jobs. The new
session is titled "(imported)" and remembers the session it came from.

Images go with a message, for a model that takes them: drop PNG,
JPEG, GIF, WebP or BMP files on the window, paste one (or a copied
image file), or pick them with the Image button. They wait as
thumbnails above the prompt box until sent, at most ten to a message.
A PNG or JPEG goes as it is; a GIF (its first frame), WebP or BMP goes
as a PNG, and so does a large image, shrunk to fit. Images are decoded
by [Wuffs][], which is memory-safe against hostile files.
The session keeps them, so they go back to the model on every request,
show in the transcript and come with a fork, but not with an export.
`[model] vision = false` turns them off for a model that has none.

The agent can look at image files on its own, too: `read` shows a PNG,
JPEG, GIF, WebP or BMP file to a model that takes images, as an image,
scaled down to the model's limits as a screenshot is (below). Images
read leave the conversation along with screenshots (`[desktop]
images`), each replaced by a line that says to read the file again. A
model with `vision = false` is told only the image's kind and size.

The New session dialog's working directory may be picked with Browse,
in the platform's folder dialog, for a directory on this computer. A
leading `~` there (`~`, `~/src`, and `~\src` on Windows) is the home
directory of the host the session runs on, `HOME`, else `USERPROFILE`
on Windows; `~user` is not supported. Paths in the home directory show
as `~/...` in the sidebar and among the recent directories.

Typing a path there drops down the folders it may go on to, listed on
the session's host (a remote one through its engine). The text up to
the last `/` (or `\` on Windows) names the folder listed, and the rest
picks among its folders: each word, in any case, must be in the name.
A name typed exactly comes first, then those that begin with the first
word, then the rest. Dot folders show once the text begins with a dot.
Up and Down pick, Tab or a click takes the pick (or the first), as
does Enter once one is picked, and the field then holds that folder,
ready to list its own. Esc closes the list, and Enter otherwise
creates the session.

A session needs no project: No folder, in the New and Import session
dialogs, gives it a folder of its own instead, `sessions/SID/work` in
the state directory, for general help with the machine. The agent works
there freely, a fork gets a copy of it, and it is deleted with the
session. Such a session has only your own instructions and skills,
none of a project's, and shows as "No folder" in the sidebar. Session
→ New session, no folder (Ctrl+Shift+N, Cmd+Shift+N on macOS) makes one
at once, with the default model and no dialog. A new session's prompt
box has the keyboard, for its first message.

A message sent while a turn runs waits, dimmed, at the end of the
transcript until the agent takes it in at its next step (or, too late for this turn, runs as
the next prompt), and its Withdraw button puts it back in the draft.

Plan mode has the agent explore and write a plan before it changes
anything: the New session dialog's Plan mode box, `/plan` (and `/plan
off`) or the mode list above the transcript (Build, Plan or Bypass) turns it on, and the agent may ask to
turn it on itself. While planning, its file tools may write only the
session's plan file, `plan.md` in the session's directory, the desktop
tool may only look, and subagents plan too. When it is done it asks for
approval, showing the plan: Approve plan lets it go ahead, and Keep
planning sends it a note. Edit plan opens the plan file in the editor
(as Ctrl+G does a draft), on this computer, to revise it by hand
first: the agent carries out the plan as the file has it when
approved, told that you revised it. The model may be changed while
the plan waits. The Plan pane shows the plan file, which stays after
approval, rendered as the transcript renders Markdown, its text
selectable and copied as text or as Markdown. Its View in editor opens
the file in the editor at any time (while a plan waits, it is that
plan's Edit plan; otherwise the agent is not told), and Reveal shows it
in Finder, Explorer or the file manager; both only for a plan on this
computer.

When a decision is yours, the agent may ask (its `ask_user` tool),
most often while planning: one card shows up to four questions, each
with a few options, the recommended one marked and picked, and a field
for an answer in your own words, then a note, Submit answers and Skip.
A skip, or nobody there to answer, has the agent go with its
recommendations. The questions wait with no countdown, and a
notification tells of them (`[ui] notify` `allow`). Only the sessions
you start yourself, attended, get the tool: never subagents, roles, a
`tools` list or command-line runs. Over the protocol, an ask of kind
`question` carries `questions`, and `answer {decision:"allow",
answers:[{pick:[...], text?}, ...]}` answers it, one entry per question
(cap `question`).

Bypass mode, in red in the same mode list, is for an agent left to
work alone somewhere it can do no harm: nothing is asked, attended or
not, so its file tools read and write outside the working directory, a
patch may change many files there, and the desktop tool acts, running
programs too. Only the agent's own asks about plan mode and its
questions remain, and
writes under the state directory (but the plan file) are refused, as
are paths that name nothing. Only you turn it on, from the list, never
a new session, the agent or the configuration. It stays through
reopening and restarts, subagents follow it, and asks waiting when it
is picked are allowed; a fork or an import starts in Build, and an
approved plan always goes to Build.

Sessions may talk to each other: the agent lists the other sessions on
its host, with how they relate to it (parent, child, sibling), and
sends one a message, which it reads at its next step, or which starts a
turn if it is idle. A message is marked as another session's, never as
the user's words or permission. A chain of messages with no prompt from
the user in between stops waking sessions after eight hops, and a turn
may send at most twenty.

`prospero run` runs one turn with no window and exits when it ends,
printing its records as JSON lines, or readable text with `--pretty`.
Nobody answers its asks, so they are denied. It exits 0 when the turn
is done, 1 on error, 3 at the step budget (`[tools] turn_budget`, model
requests, none by default), 130 when cancelled, and 2 for usage
errors. Options: `-C DIR` (working directory), `-e ENDPOINT`,
`-m MODEL` (or `ENDPOINT/MODEL`), `-r ROLE`, `-c FILE` (configuration)
and `--state DIR`.

A session that names no model gets the configuration's `model`, else
the first `[model]` section, else the first model its endpoint listed
when last asked. A `model` of `ENDPOINT/MODEL`, where ENDPOINT is a
configured endpoint's name, names its endpoint too, and a configured
`endpoint` naming another is ignored, with a warning. Otherwise a
session that names no endpoint gets the configuration's `endpoint`,
else the first endpoint that listed the model when last asked, else
the first `[endpoint]` section with a URL. The New
session dialog asks every endpoint for its listing (`/v1/models`, which
loads nothing) and offers what they serve as `ENDPOINT/MODEL`.

`prospero --headless` is the engine alone, speaking the protocol (one
JSON object per line, `src/events.cpp`) on standard input and output:
commands in, records out. The GUI can run its engine this way, as a
child process, with `--engine=subprocess`.

`prospero serve` is the engine as a background host, behind a socket
that only this user may use (a named pipe on Windows). `prospero
attach` bridges standard input and output to the host of a state
directory, starting one if none runs; this is what runs at the other
end of ssh. `prospero hosts` lists the hosts running, and `prospero
connect` speaks the protocol to a host: a `[remote NAME]`, any ssh
destination, or this machine's. A host exits after ten minutes with no
client and nothing running.

`prospero deploy` puts an engine on another machine, as below, and
`prospero eval` runs a suite of tasks to compare models (Evals).

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

## Configuration

Prospero reads one INI file, `$XDG_CONFIG_HOME/prospero/prospero.ini`,
which defaults to `~/.config/prospero/prospero.ini`. On Windows, home
is `%HOME%`, else `%USERPROFILE%`, so that Prospero started from
Explorer finds the same files as from a shell that sets HOME, and
`%APPDATA%\prospero\prospero.ini` is used only while there is none
under home. On first run it writes the file
from its built-in copy ([assets/prospero.ini](assets/prospero.ini)),
which documents every section and key: `[endpoint NAME]`,
`[model NAME]`, `[role NAME]`, `[guard]`, `[tools]`, `[instructions]`,
`[skills]`, `[ui]`, `[desktop]`, `[host]` and `[remote NAME]`. Changes take effect while it
runs. An unknown key or a bad value is reported in the diagnostics,
and a bad value falls back to its default: nothing in the file keeps
Prospero from starting. The settings window edits the file a line at a
time, and its Open in editor opens the whole file in your editor, read
again once the editor exits.

The agent can change the file too, through Prospero's own skill (see
Skills). A write or edit of the file, or of a local file it includes,
asks every time, with no "always", even inside an allowed directory
(Bypass mode aside); once written, the configuration is read again at
once, the agent is told of any problem in what it wrote, and the
window takes up the change.

API keys never go in the file: `key = env:NAME` reads one from the
environment when it is needed, and `key = file:PATH` from a file
holding just the key, for an engine whose environment you cannot set,
such as one on a remote host or in WSL (its own configuration names a
file there). Above the first section, `include = ` adds a shared file,
such as a team's endpoints and models, whose settings this file can
override. An include may be a path or an http or https URL, which is
fetched at startup, cached, and refreshed in the background, and may
be pinned to a SHA-256. Nothing in an included file can make Prospero
run a command: its plaintext secrets and `file:` secrets, `[remote]`
and `[desktop]` settings, `[tools] path_prepend` and `path_append`, a
`[tools] shell` other than a keyword, and `[ui] editor` and `terminal`
are ignored. A server whose certificate is not trusted fails the turn
at once, saying why, rather than being retried.

Prospero started from Spotlight, the Finder, the Start menu or a desktop
menu has a shorter PATH than a shell gives it. `[tools] path_prepend`
and `path_append` add directories, separated by `;`, before and after
it: for the agent's commands, its shell and ripgrep, and for what the
GUI runs itself, the editor and ssh. A change applies to the next
command, and for the editor and ssh once saved in the settings window
(or at the next start). A remote engine goes by its own machine's
configuration.

    [tools]
    path_prepend = ~/.local/bin; /opt/homebrew/bin

An endpoint speaks Anthropic's Messages API with `kind = anthropic`:

    [endpoint anthropic]
    kind = anthropic
    url = https://api.anthropic.com/
    key = env:ANTHROPIC_API_KEY

Its listing gives each model's context and output limits. Reasoning
goes back to the model verbatim with its signature, and the tools, the
system prompt and the conversation carry cache breakpoints, so cache
reads grow turn over turn. A `[model NAME]` section sets `thinking`
(adaptive by default), `thinking_display`, `effort` and `max_tokens`.

For an OpenAI-compatible server, `template.NAME = VALUE` in a
`[model NAME]` section is an argument of the model's chat template,
sent in `chat_template_kwargs` (as llama.cpp and vLLM take it): `true`
and `false` as booleans, numbers as numbers, and anything else, or
anything in quotes, as a string. A model is often run several ways,
such as thinking more or less, and `like = OTHER` makes a variant: a
model of its own name that takes OTHER's settings but those it sets
itself, and is sent to the server as OTHER (or as its own `id`), so
discovery, context limits and the places on a router are OTHER's:

    [model large]
    template.thinking = true

    [model large-quick]
    like = large
    template.reasoning_effort = 25

    [model large-deep]
    like = large
    template.reasoning_effort = 100

A variant is chosen as any model is, for a session, a role, a
fallback or a compactor.

Sessions, their logs and the cache of URL includes live in the state
directory: `$XDG_STATE_HOME/prospero`, else `~/.local/state/prospero`.
On Windows, `%LOCALAPPDATA%\prospero`, where it used to be, is used
while there is none under home.

At the start of a session the agent is given its instructions:
`AGENTS.md` next to the default configuration file (such as
`~/.config/prospero/AGENTS.md`), even when `-c` names another, then
the first of `AGENTS.md` or `CLAUDE.md` (`[instructions] files`) in
each directory from the root of the repository down to the working
directory, and more below it as the agent works there.

Memory is the agent's own, as Claude Code's auto memory is: for each
project, below `memory` in the state directory, a `MEMORY.md` index of
one line per memory, given to each new session in the project, and a
file for each, which the agent saves and forgets with its memory tool,
on its own judgment and when asked. A repository's worktrees share
one. To remember something for every project on this machine, the
agent adds it to the user's own `AGENTS.md`, asking first.
`[memory] enabled = false` turns it off.

Skills are the same as in Claude Code and other harnesses: a directory
holding `SKILL.md`, whose frontmatter gives its `name` and
`description`, with any files it refers to beside it, perhaps below
folders that group skills, as Cursor allows. They are found in
`.claude/skills`, `.agents/skills`, `.cursor/skills` and
`.codex/skills` in each directory from the working directory up to the
root of the repository, then in `[skills] paths`, then in each
`--skillsdir DIR` given to `prospero`, `run`, `serve` or `attach` (as
many as wanted), then in `~/.config/prospero/skills`, then in the same
four under `~`; the nearest of a name wins. The user's own follow the
default configuration file, even when `-c` names another. The agent is told each
skill's name and description, loads one with its skill tool, which
also lets it read the skill's directory, and hears of skills added or
changed as it works. `/NAME args` in the prompt box uses a skill
directly, and one with `disable-model-invocation: true` is for that
alone. `[skills] enabled = false` turns them off.

One skill is built in: `prospero`, Prospero's Help and every setting,
served by the engine itself, so that the agent can answer questions
about Prospero and change its settings. Its replies link Help topics
(`help:NAME#SECTION`), settings (`settings:KIND/NAME#KEY`) and panes
(`view:PANE`), which a click in the window opens. A skill of the
user's own named `prospero` replaces it, never a project's, and eval
sessions do without it.

## Desktop control

The desktop tool lets the agent see the screen and use the mouse and
keyboard: screenshots of the screen or of one window, clicks, drags,
scrolling, keys and Unicode text, the list of windows, and starting,
waiting for and closing programs. Every session is offered it where
it could work, where its helper is found and there is a screen (on
Linux, an X11 or Wayland display), and every action, screenshots
included, asks you first, denied if you do not answer in time. Your own
configuration may keep it to sessions with a role that lists `desktop`
in its tools, or turn it off with `offer = none`:

    [desktop]
    offer = roles
    ask = true

    [role gui]
    description = Builds programs and tries them on the screen
    tools = default desktop

A role's `tools` are read left to right: `*` is every tool, `default`
those of a role that names none (every tool but desktop), and `-NAME`
takes one away, as in `* -shell`. The New session dialog's Role starts
a session in a role, as `-r ROLE` does on the command line.

An ask may instead allow its action's class for the rest of the session
and its subagents ("Allow input for this session"): `observe` for
screenshots, the list of windows and waiting; `input` for clicks, the
mouse, scrolling, keys and text, which includes `observe`; and
`windows` for focusing and closing them. Starting a program is asked
about every time. `ask = false` asks about nothing at all.

The work is done by a helper, which the harness starts for a turn and
talks to over pipes. On Windows, and in a Linux build from source, it
is `prospero` itself, run as `prospero desktop`. Elsewhere it is a
program of its own, `prospero-desktop`, next to `prospero`: on macOS
so that the one program the system lets watch the screen keeps its
identity however often Prospero is rebuilt, and beside the static Linux
engines of a release because it loads the X11 and portal libraries,
which a static program cannot. So far it exists for Windows (XP and
later), macOS (15.2 and later) and Linux, on X11 or Wayland. Run
alone, it takes the same commands on its command line (`prospero
desktop shot screen > s.png`, or `prospero-desktop shot screen`;
`list`, `click WINDOW X Y` and so on; `-h`). An engine that `prospero
deploy` put on another machine uses the helper deployed beside it, or
itself (Remote and background hosts).

On macOS the helper sits in `Prospero.app/Contents/MacOS`, and works in
points, which it captures at the display's full resolution. macOS asks
your permission for it twice, in System Settings > Privacy & Security:
Screen Recording, for screenshots (quit and reopen Prospero after
granting it), and Accessibility, for input and for raising, closing and
moving windows. Both are granted to Prospero.app when you start it as
an app, or to the terminal you start `prospero` from. A call that lacks
one fails at once and says which to grant to what;
`prospero-desktop access` lists them, and `access -ask` asks for those
missing. Build with `-DPROSPERO_SIGN_IDENTITY=NAME`, a code-signing
identity from your keychain, even a self-signed one, so that the
grants survive rebuilds.

Grants also name the bundle identifier, `com.nullprogram.prospero`,
which only a signed build takes. Any other build is signed ad hoc as
`com.nullprogram.prospero.dev` (its helper as `....dev.desktop`), so
that a test build never claims the real app's grants, nor leaves a
requirement on them that refuses the real app. `-DPROSPERO_BUNDLE_ID=ID`
chooses another; the helper's is that plus `.desktop`.

On Linux the helper drives the X11 display that `DISPLAY` names, in
pixels, loading libX11, libXext and libXtst when it runs. It works with
a window manager through EWMH, or without one. A program the tool runs
(a path, a name in `PATH`, or a desktop entry such as
`org.gnome.TextEditor`) is started by your systemd user manager, as a
transient service in `app.slice` with the helper's environment, so that
it outlives the turn and Prospero, as an application you start does.
Without a user manager it ends with Prospero, and the tool says so.
This holds under Wayland too.

In a Wayland session (`WAYLAND_DISPLAY` set) it goes through the
desktop's portal instead, in the compositor's logical pixels: the
RemoteDesktop portal, with a screen cast of the monitors, for
screenshots (read through PipeWire) and input (through libei, or the
portal's own calls without it); each library is loaded when it runs.
The desktop asks once, in a dialog (on GNOME, turn on "Allow Remote
Interaction" and press Share), and the approval is kept: a restore
token in the state directory lets later runs start without asking
(`prospero-desktop access -ask` shows the dialog and waits). Wayland
has no common way for one program to list, raise or close another's
windows, so that depends on the desktop: KDE Plasma's KWin runs small
scripts for it; GNOME needs the [Window Calls][] extension; elsewhere,
or on GNOME without it, only X11 programs (under Xwayland) can be named,
and the rest of the screen is reached through `screen` and `active`.
Wayland gives keys only for the characters of the keyboard's layout, so
text with others is refused before anything is typed.
`PROSPERO_DESKTOP_SESSION=x11` or `wayland` overrides the choice.

[Window Calls]: https://extensions.gnome.org/extension/4724/window-calls/

Screenshots go to the model as images, scaled down to its limits
(`[model] image_edge` and `image_pixels`: by default 1568 pixels on a
side and 1.15 megapixels for kind anthropic, else 1280 and 1 megapixel),
and only the latest few stay in the conversation, images read
included (`[desktop] images`).
Older ones leave in steps, `images_slack` at a time (by default 3, with
up to 4 more before a step), each replaced by a line naming its call,
so that the prompt cache keeps hitting between steps. A model gives points in pixels of the image it saw, or, with
`[model] coordinates = norm1000`, in thousandths of its width and
height, as Qwen-VL and Gemini do; the harness maps them back to the
screen. A model that takes no images is marked `vision = false`.

## MCP servers

MCP servers give the agent more tools, offered beside the built-in
ones as `mcp__SERVER__TOOL`. Each is an `[mcp NAME]` section: a
`command` that speaks MCP over its standard input and output, or the
`url` of a server over HTTP (Streamable HTTP; not the old SSE
transport).

    [mcp stars]
    command = uv run tools/starsmcp.py
    cwd = ~/src/StarsVM
    startup_timeout = 120

    [mcp docs]
    url = https://mcp.example.com/mcp

A command is split into words with `'...'` or `"..."` quoting, and
runs without a shell. Begin it with `'`, since a value beginning with
`"` is quoted whole. It runs in the session's folder by default, is
started for the sessions that use it, and stops a minute after the
last one has gone. Its standard error is logged in the state
directory's `mcp/logs`. `tools` keeps only some of a server's tools,
`trust = yes` runs them without asking, and a role with a tools list
names them as `mcp__NAME` or `mcp__NAME__TOOL`. Otherwise each call
asks first, with an "always" for all of that server's tools. An
included file cannot set `[mcp]`.

A project's `.mcp.json`, in Claude Code's format, with `${VAR}` and
`${VAR:-default}` expanded, is read once as a session starts, from its
folder up to the repository's root. Its servers start only once you
allow them, for the session or always for that project until their
definitions change. A call to one may also trust its server in that
project, from then on, under the same condition. Your own section of
the same name wins.

A server over HTTP may take a bearer `token` (as `env:NAME`) or
`header.NAME` values, or want you to sign in with OAuth. Settings, on
its page, has Sign in, which opens the browser and takes its return on
a loopback port; `prospero mcp auth NAME` does the same without a
window, printing the address to visit. Prospero discovers the
authorization server, registers itself as a client unless `client_id`
is set, and uses PKCE. It keeps the tokens in the state directory's
`mcp/auth`, mode 0600, and refreshes them when the server refuses
one. `prospero mcp logout NAME`, or Sign out, forgets them.

## Web search

`web_search` sends a query to a search service and returns titles,
links, dates and excerpts; `web_fetch` reads one https page as text,
with scripts, navigation and hidden elements dropped and links made
absolute. Neither is offered until `[web]` turns it on:

    [web]
    search = searxng
    url = http://localhost:8888/

`search` is `searxng` (your own instance, with `json` in its
`search.formats`), `exa` (Exa's keyless MCP endpoint, or with `key`),
or `anthropic` (a side request to an Anthropic model with Anthropic's
web search tool: `model = ENDPOINT/MODEL`, by default
`claude-haiku-5-5` on the first endpoint of kind `anthropic`).
`fetch = no` leaves out `web_fetch`, `results` caps a search's
results (8), and `searches` a session tree's searches (200). An
included file cannot set `[web]`.

Every search and page asks first, showing the query and its service
or the whole address, with "always" for every search or every page of
a site; `ask = no` asks about none. `web_fetch` reads only addresses
that already appeared in the conversation (the user's messages,
files, tool output, results and pages), never one the agent composed,
and never a local address: IP addresses, `localhost`, dotless names,
`.local` and the like. It follows redirects within a site, reports
those to another, and keeps a long page's whole text in the session's
`out` folder to read in parts. What both tools return is set between
lines marked with a random word, after a line saying that it is
information, never instructions, and characters people cannot see but
models read are removed. A role without the web tools, say one that
delegates to a `tools = web_search web_fetch` role, keeps an agent off
the web.

## Remote and background hosts

A host quits as when a client asks it to, closing its endpoint and
removing its registration, on SIGTERM, SIGINT or SIGHUP, or on Windows
Ctrl+C or Ctrl+Break at its console.

A `[remote NAME]` section names a machine reached over ssh, and the
window's Hosts panel adds one and connects to it. Prospero there runs
as `prospero attach`, started over `ssh -T`, which starts a host that
outlives the ssh session. A link that breaks is taken up again from
where it was, and the host keeps a client's place for three hours. A
window closed with "Keep running in background" makes its own process
such a host, and keeps the state directory, so a later launch brings
that window back. On Windows, a WSL distro is a remote too, reached
through `wsl.exe -d DISTRO --cd ~ --exec /bin/sh -c ...` instead of
ssh: every installed distro is listed (`wsl.exe -l -q`) as a remote of
its own name, Docker Desktop's left out, and a section with `wsl =
DISTRO` in place of `ssh` names one. Deploying into a distro sends the
engine on `cat`'s input, as it has no scp, and records it as
`wsl:DISTRO`. A window on a state directory that `prospero serve`
hosts lists the sessions the host holds and attaches to them.

A remote machine needs an engine built for its platform, which
`prospero deploy` uploads with scp to `~/.prospero` there, as
`engine-PROTO-VERSION-OS-ARCH`, verifies it by its SHA-256, and
records where it went, for connecting. With `deploy = auto` in its
section, connecting deploys one when the machine has none; with the
default, `ask`, a failed connection offers Deploy engine in the Hosts
pane. Engines come
from the `engines` directory next to the executable, or when it has
none for the platform, the one beside the default `prospero.ini`, named
`prospero-engine-VERSION-OS-ARCH`, or are the executable itself when it
is built for that platform. `tools/engines.py` builds them: macOS
arm64, Windows x86-64 and i686, and Linux aarch64 and x86-64 (static,
in the Docker image with `--docker`, or else on a Linux host given
with `--linux HOST`, else left out).
A remote whose engine is older than this program shows the operations
it lacks disabled, and a host's menu has Update engine, which deploys
this version's engine there and reconnects to it once nothing runs.
Its sessions' model picker lists the models of the remote's own
configuration. `list = connected` leaves a remote out of the sidebar
until it is connected.

The agent's task tool may start a subagent on a remote, or on Windows
in a WSL distro, by its name (`host`), with `cwd` there. The parent's
engine links to it as the window would, with `new {task}`, so this
works with no window too: the subagent is a session there, under that
host's configuration, tools and asks, in Plan mode if its parent plans
and else Build, with that host's role and model (the call's, the
role's, the parent's ENDPOINT/MODEL if it has that endpoint, else its
default). In a distro it starts in the parent's directory as the
distro sees it, under `/mnt`; over ssh in its home. It writes its
report in its log (`task.report`), and the parent's call ends with it.
The window reaches the subagent by the same name, nests it under its
parent, and answers its asks; with no window to show it, they are
denied at their deadline. `subagents = no` leaves a remote out.
Deploys to one destination take turns, so the window and an engine
never upload over each other.

A desktop helper there for the platform,
`prospero-desktop-VERSION-OS-ARCH`, goes too, as
`desktop-PROTO-VERSION-OS-ARCH` beside the engine, where the engine
looks for it first; with the executable itself as the engine, its own
`prospero-desktop` goes. An engine that is its own helper, a Windows
one or this executable built from source on Linux, gets none. `tools/engines.py` builds the Linux helpers beside their
engines. The engine works without a helper, so one
that cannot be put there, or does not run there, is only a warning.

## Windows notes

The GUI build's `prospero.exe` is a GUI program, so at an interactive
cmd or PowerShell prompt the prompt returns at once, without waiting
for a command such as `prospero deploy`: its messages still show in
that console, after the prompt, but its exit status is lost. Pipe or
redirect its output to wait for it: `prospero deploy pi | more`, or in
PowerShell `prospero deploy pi | Out-Host`. Over ssh and in scripts it
works as usual, and it serves as the headless engine there too. The headless
build (`-DPROSPERO_GUI=OFF`), which is the XP engine and the engine
deployed to Windows machines, is a console program, and works as usual
at a prompt as well.

The agent's shell is a POSIX `sh` when there is one: `sh.exe` or
busybox-w32's `busybox.exe` next to `prospero.exe`, else on the PATH.
Without either, commands run in PowerShell: PowerShell 7 (`pwsh.exe` on
the PATH), else the Windows PowerShell 5.1 that comes with Windows, and
`cmd.exe` only as a last resort. The shell tool's description tells the
agent which it has. `[tools] shell` picks one instead (`sh`,
`powershell`, `cmd`, or an executable by its path or its name on the
PATH, `[tools] path_prepend` included) for new sessions; a
session keeps the shell it began with. PowerShell takes a second or more
to start, so each command is that much slower than in busybox.
Each tool runs in a job object, so that its whole process tree dies
with it, or with Prospero.

Windows XP runs only the headless engine (the i686 build): `prospero
run`, or a host reached over ssh. Its WinHTTP has no TLS 1.2, so
endpoints must be plain http, as on a local network. Before Windows 8,
a process in a job cannot put its children in jobs of their own, so
under an ssh server that runs sessions in a job, each tool breaks away
into its own job where the server allows it, and otherwise runs without
one, with a warning.

## Development

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

## License

Prospero is free and unencumbered software released into the public
domain. See [UNLICENSE](UNLICENSE).

Its programs include third-party code under its own licenses: Dear
ImGui (MIT), FreeType (FTL), md4c (MIT), SDL3 (zlib), cpp-httplib
(MIT), Mbed TLS (Apache-2.0), and Wuffs (Apache-2.0 or MIT, at your
option), which every program with the engine includes.

[uv]: https://docs.astral.sh/uv/
[Wuffs]: https://github.com/google/wuffs
[w64devkit]: https://github.com/skeeto/w64devkit

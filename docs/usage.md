# Usage

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
subagents under their parents; the transcript, with markdown (your
messages too, each line break kept as typed), code highlighting and
selection; tool calls with Undo, and Rewind to any
message that began a turn; the todo list and background jobs; and the
prompt. The model's reasoning folds to one line that counts its words as
it streams (`[ui] count = characters` counts those instead), which a
click opens; `[ui] thinking = open` shows it whole, and `hide` leaves it
out of the transcript (the session log keeps it, and the model still
gets it back). Tool rows fold by kind and outcome, or all of
them with `[ui] tools = fold` or `open`. Above the transcript, a session's
Shell output button (the Bash logo) folds every shell command's output,
short ones too, and its Tool output and Reasoning buttons (a wrench and
screwdriver, a light bulb) open everything, each for that session alone. Cmd/Ctrl+F finds text
in what the transcript shows, not in folded rows, and a code block's Copy button
copies its code exactly. A bar above the transcript shows how full the
context is. A file tool that goes outside the session's working directory,
or the roots the configuration adds, asks first, and an answer not given
in time denies it. The settings window edits the configuration file in
place, changing only the lines it sets, with a page for each section.
Endpoints, models, roles and remotes are added there, and deleted, with
the comment just above them, when your own file rather than an included
one defines them. Closing the window while turns run asks whether to stop
them, or to keep running in the background, with a tray icon to bring the
window back. One window runs per state directory: launching Prospero again
on the same one, even while its window is in the background, brings that
window forward and exits, as does opening the app again on macOS. Under
Wayland a window already open may only ask for attention, as the
compositor decides focus. A different `--statedir` (or `XDG_STATE_HOME`)
is a separate instance. `--session SID` opens the window on that session.
The View menu shows and hides the panes and zooms
the text for the run (Ctrl+=, Ctrl+- and Ctrl+0, Cmd on macOS, or the
wheel with the key held); `[ui] font_size` (17 pixels by default) sets its
size for good. Chinese, Japanese and Korean come from the system's fonts,
which the built-in ones lack: those fontconfig, CoreText or Windows' display
language picks, or the files `[ui] fonts` names. View → Layout, View's first item, arranges the panes as
Standard, Compact (no right column) or Focus (one session), or as a layout
saved under a name of its own, kept with the window's layout in the state
directory; Ctrl+1 to Ctrl+9 (Cmd on macOS) pick them in that order. The
window opens where it was last, fitted to the displays there are; the
first time, 1700 by 900 at the display's scale, or maximized on a display
too small for that. The first run shows Help in front, with Settings and
Hosts when the configuration names no model and no remote host, and none
of its endpoints lists a model either; close them once read, as the View
menu shows each again. Settings also opens with Ctrl+, (Cmd+,), from
the Session menu (and the application menu on macOS), and from the end
of the model and shell lists, at the session's endpoint and the shells. Help → Help shows a page on each topic, from
getting started to troubleshooting, built in from `assets/help`. Its
search field (Ctrl+F while Help has the focus) lists the sections that
have every word typed, headings first, and a click goes to that section.

Launched from a terminal, `prospero` goes on detached, as gvim does,
and the shell has its prompt back once the window is up; should the
window fail to start, its errors print there first, with exit status 1.
Its messages then go to `gui.log` in the state directory. `-f`
(`--foreground`) keeps it in the terminal, with its messages there and
Ctrl+C to stop it. A launch whose standard error is not a terminal, as
from a script or a desktop launcher, stays as it is. On Windows, a
console prompt gives a GUI program no standard handles: launched so,
with its parent at a console, it detaches the same way, which matters
for w64devkit's `sh`, the one console shell that waits for GUI
programs. A shell in mintty (MSYS2, Cygwin) passes pipes, as a script
does, so there it stays.

`prospero DIR`, such as `prospero .` in a terminal, opens a new
session in that directory, in Build mode with the default model,
ready for a prompt. With no window open yet, the new window has it,
and the session has the terminal's environment. With a window open
already, the session starts there, and that window comes forward: the
session then has that window's environment, not the terminal's. On macOS, opening a folder
with Prospero (`open -a Prospero DIR`, or dropping it on the Dock
icon) does the same. A folder dropped on the window fills the New
session dialog with it.

The sidebar is a tree: groups, then hosts, then sessions with their
subagents, and archived sessions folded away at the end. A session's
context menu, and the Session menu, rename it, move it to a group,
fork it, compact it now, switch its model, export it (Markdown or the
raw log), archive it, or move it to the Trash (with its subagents),
which keeps it `[sessions] trash_days` (30) before removing it, unless
deleted permanently or emptied first. Dragging a session onto a group,
onto Archived, or below the list moves it there too, on its own host,
and onto the strip at the sidebar's foot, to the Trash. The transcript's
context menu forks at a message: before a prompt, which then waits in
the fork's prompt box, or after a turn. A side chat, from that menu,
the Session menu or `/side`, is a fork made even while the agent
works, nested under its session, the selection quoted in its box; its
agent is told to leave files alone. A worktree session, from the New
session dialog's New worktree, that menu, or `/worktree`, works in a
git worktree Prospero makes in `.prospero/worktrees`, on a branch of
its own, and removes once its sessions are archived or in the Trash,
if clean. One
started from a session begins fresh, nested under it, and reports its
branch back to it with `send_message` when asked. The prompt box keeps a draft
per session, with the engine, has the session's earlier prompts on Up
and Down, opens the draft in an editor with Ctrl+G (`[ui] editor`,
else `$VISUAL` or `$EDITOR`), and takes `/compact`, `/model`,
`/rename`, `/fork`, `/side`, `/worktree`, `/export`, `/plan`, `/background` and a skill's
`/NAME`. Run in background (Ctrl+B, Cmd+B on macOS) moves a running
command to a job, or a subagent at work on by itself, so that the
agent goes on and can be talked to; it is told when either ends, a
subagent's report coming as a notice that wakes the parent. Stop, on a
running command's row, kills that command alone, and the agent goes on
with its output so far. An ask may be answered "always",
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
image file), or pick them with the Attach button. They wait as
thumbnails above the prompt box until sent, at most ten to a message.
A PNG or JPEG goes as it is; a GIF (its first frame), WebP or BMP goes
as a PNG, and so does a large image, shrunk to fit. Images are decoded
by [Wuffs][], which is memory-safe against hostile files.
The session keeps them, so they go back to the model on every request,
show in the transcript and come with a fork, but not with an export.
`[model] vision = false` turns them off for a model that has none.

Any other file attached the same way (the Attach button, a paper clip,
picks any file) goes as text, as `read` shows it: numbered lines, cut
at read's limits with a note to read on. The session keeps a copy in
its folder (`files/N-NAME`, at most 16 MiB), which the agent may read
without asking, and the text is put before the message on every
request, so it needs no vision. A file with a NUL byte in its first
8 KiB is binary, and is refused, as `read` refuses it. The transcript
notes the files under their message, folded.

A file or folder may be mentioned in the prompt: type `@` at the start of
a word (the text's start, or after a space or `(`), and a picker of the
session's files and folders opens, ranked as you type, a subsequence of
the path matching and the file's own name counting for more. Up and Down
move the pick, and Tab or a plain Enter puts `@path` in the message
without sending it; Shift+Enter and Ctrl+Enter still start a new line,
and Esc closes the picker and leaves the message alone, a second Esc then
cancelling the turn as it always did. A path holding a space is quoted,
`@"my file.txt" `, and a folder's pick puts `@src/ui/` in the message
and keeps the picker open inside it. `@` is already how an instruction
file imports another, `@docs/a.md`, so the two read alike. `[ui]
mentions = false` turns the picker off, and `@` is then plain text.

Picking a file also attaches its text to the next message, as the Attach
button does, so the agent has the contents as well as the path. A file
that cannot be attached (binary, over 16 MiB) still gets its token, with
a dimmed chip that says why, and the mention still works: the agent reads
the file itself. With ten files already attached the box has no room for
another chip: the token goes in, no chip is added, and a note says ten
files are already attached, the mention still working.

An `@path` the message no longer holds when it is sent takes what it
attached with it: the check is made when the message is sent, not as the
text changes. A path typed by hand and dismissed with Esc stays a plain
pointer, with no contents. A session on another machine lists one folder
at a time, through its engine, and its engine reads a picked file there
as the message goes; an engine too old for that gives the pick a dimmed,
path-only chip. A path that cannot be written as a token, one holding `"`
or `@`, is refused with a note.

The picker lists other sessions too, once a word is typed: those on the
session's host that are neither archived, in the Trash, nor subagents,
matched by title or id and ranked with the files, each marked with a
discussion icon and its id. A pick puts the title in the message,
`@"Fix the parser" ` (the id where no token can hold the title, or
another session has it too), and attaches nothing: when the message is
sent, each session it mentions goes with it by id, and the agent is told
that id before the message, so `send_message` reaches the session even
after a rename. A token typed by hand counts as a pick does. The
transcript shows a folded "Mentioned:" line under the message.

The agent can look at image files on its own, too: `read` shows a PNG,
JPEG, GIF, WebP or BMP file to a model that takes images, as an image,
scaled down to the model's limits as a screenshot is ([Desktop control](desktop.md)). Images
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
turn it on itself. The mode may change at any time, mid-turn too: the
turn goes on, each call after the change held to the new mode, and the
agent told at its next step; running subagents follow. While planning, its file tools may write only the
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
recommendations. In Plan mode the questions wait with no countdown;
otherwise they are skipped after `[guard] question_timeout` (300 s),
a countdown that starts over whenever you work on the card. A
notification tells of them (`[ui] notify` `allow`). Only the sessions
you start yourself, attended, get the tool: never subagents, roles, a
`tools` list or command-line runs. Over the protocol, an ask of kind
`question` carries `questions`, and `answer {decision:"allow",
answers:[{pick:[...], text?}, ...]}` answers it, one entry per question
(cap `question`). `ask.touch {sid, req}` starts its countdown over, and
an `ask.deadline {req, deadline}` record gives a changed deadline, 0
for none (cap `question.deadline`). It starts any other ask's countdown
over too, from its own timeout, as you write a note to deny it with
(cap `ask.deadline`).

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
approved plan always goes to Build. The agent cannot ask to plan in
Bypass: `enter_plan_mode` is refused, so only you switch a Bypass
session to Plan.

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

`prospero deploy` puts an engine on another machine ([Remote and
background hosts](remote.md)), and `prospero eval` runs a suite of
tasks to compare models ([Evals](development.md#evals)).

[Wuffs]: https://github.com/google/wuffs

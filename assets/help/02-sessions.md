# Sessions and modes

## New sessions

Session → New session... (Ctrl+N, Cmd+N on macOS), or New session
here... in a host's menu in the sidebar, opens the New session dialog:

- **Host**, when more than one is ready: where the session runs.
  With Docker or Podman running, "Docker: a new container" makes a
  container for the session, and the dialog asks for its image,
  workspace and network in place of the directory (see
  [Hosts](help:hosts)).
- **Working directory**: the agent's project. Type it, pick one from
  Recent, or use Browse… for the system's folder dialog (on this
  computer only). A leading `~` is the home directory of the host the
  session runs on; `~user` is not supported. The directory must exist.
- **No folder**: no project. The session gets a folder of its own in
  the state directory, deleted with it, and shows as "No folder" in
  the sidebar. It has only your own instructions and skills, none of a
  project's.
- **New worktree**: the session works in a new git worktree of the
  folder's repository, on a branch of its own (see Worktree sessions,
  below). Its name is suggested, and may be changed. The line below it
  says what the branch starts from, and how many uncommitted changes
  stay behind in the folder.
- **Model**: the models the endpoints list, as `ENDPOINT/MODEL`: the
  default first, then each endpoint's, its `[model]` sections first.
  An endpoint that cannot be reached shows why, greyed out. Type in
  the field at the top to filter the list, or type a model it lacks,
  `MODEL` or `ENDPOINT/MODEL`, and press Enter. The pick holds for
  later sessions while the endpoint still lists it.
- **Role**, when the configuration has roles: start in one, with its
  instructions, model and tools (see [Subagents](help:jobs)). A
  session with a role cannot use Plan mode.
- **Plan mode**: start in Plan mode (below).

Once the session is made, the prompt box has the keyboard, so its first
message may be typed at once.

Session → New session, no folder (Ctrl+Shift+N, Cmd+Shift+N on macOS)
skips the dialog: a No folder session on this computer, with the
default model, its prompt box ready. Type `/plan` there for Plan mode.

A folder dropped on the window opens the dialog with that folder
filled in. `prospero DIR` in a terminal starts a session in DIR at
once.

## The sidebar's order

The sidebar lists sessions by recent activity: the one with the latest
message sent, turn begun or turn ended comes first, a session's
subagents counting as its own. Opening, renaming, moving or archiving a
session doesn't count, so reading an old session leaves it where it
is. Groups keep their own order, and subagents stay oldest first under
their parents. The sort button at the top of the sidebar switches to
Date created, newest first, and back, and the window remembers the
choice. A session's tooltip says when it was last active.

While the pointer is over the sidebar, rows hold still: a session that
ends its turn meanwhile moves once the pointer leaves, never from under
a click. A session from before Prospero kept this time starts at when
it was made, until its next turn.

Every heading folds by a click, and so does a session with subagents,
by its dot. A folded one's dot has a ring around it; an open one has a
thin rail down from its dot, beside what it holds: click anywhere along
the rail to fold it, which brings its heading back into view if it had
scrolled away.

The pointer on a session's row shows its actions at the right:
**Archive** (Unarchive under Archived) and **Move to Trash**, or in the
Trash, **Restore** and **Delete Permanently**. The grip at the far right
says the row drags: drag it, or the row anywhere, as below. The row
lifts out as a card that follows the pointer and says where it would
go.

## Going to a session

Session → Go to... (Ctrl+K, Cmd+K on macOS) opens a palette over the
window, in any layout, Focus too, where the sidebar is hidden. Type
words from a session's title, group, folder or host, or a piece of its
id: every word must match, and title matches rank first. Up and Down
move the pick, Enter shows it with its prompt box ready, and Esc, a
click outside, or Ctrl+K again closes the palette.

What you type finds more than sessions, each kind under its own
heading after the sessions: layouts and panes ("focus layout", "show
jobs"), settings, as the Settings pane's search finds them, and Help's
sections. Enter applies the layout, shows the pane, opens Settings at
the setting, or Help at the section.

With nothing typed, it lists the sessions viewed in this window, the
latest first, then the rest by recent activity, leaving out the one
shown: Ctrl+K then Enter goes back to the previous session. Subagents
and archived sessions are listed only when typed words match them, and
sessions in the Trash never are.

## Talking to the agent

Enter sends the prompt. The transcript shows your messages as
Markdown, as it does the agent's: code fences, `inline code`, lists and
emphasis render, while each line break stays as you typed it and HTML
tags show as text. The agent gets exactly what you typed. With the
mouse over a message, its header shows when it was sent, or when the
agent finished it, in local time, with the date if not today. The buttons
beside the prompt box are icons,
named in their tooltips: Send (a paper plane), Attach (a paper clip),
Open working folder (a folder) and Terminal, which shows the Terminal
pane with the session's shell in its folder, as Ctrl+` does (for a
session on another host, which has no terminal tabs yet, it opens an
external terminal instead, as Open in external terminal does). While the agent
works, Send becomes Steer (a curved arrow), and Stop (a square) shows
below it. The status line above the box begins with the session's
mode, model and shell, each a list to switch it. What you send while it works waits at
the end of the transcript, dimmed and marked "Next step", until the
agent reads it at its next step (or marked "Queued", for a turn of its
own, as it is when sent too late for this turn), and its Withdraw
button takes it back into the box. Stop, or Esc, cancels the turn and
kills any command it runs.

The line above the prompt box says what the session is doing, and
for how long. While a llama.cpp server reads a long prompt, a bar
shows how far it has got, with about how long the rest will take once
the first batch gives its pace. Tokens cost more the further into the
context they are, and the estimate allows for that as more batches
come. After one long prompt, the next on the same model, in any
session, has an estimate from the start.

Once a turn ends, the agent's last message of it says how long the turn
worked: the time in requests the model answered, in tool calls and
compacting. Waiting for you (an ask, a question, a plan to approve), for
the endpoint's line, and between attempts is left out, and subagents
that ran at once count once. The Info button (the circled i) adds up
the session's work: the time, how much of the model's was before its
first tokens (reading the prompt), the tools' and the subagents', the
tokens sent, read from the cache and received, the requests, and the
output's rate after the first token. Once a session was opened, its
tooltip in the sidebar gives its time too.

Attach adds files to the message: drop them on the window, paste them,
or pick them with the paper clip. Images go to a model that takes
them, as pictures. Any other file goes as text, as `read` shows it:
numbered lines, the first 2000 or so, then a note to read on. The
session keeps a copy in its own folder, so the agent can read the rest
without asking. A PDF goes as its text, extracted, a `--- page N ---`
line starting each page; the session keeps the PDF beside it, and a
model that takes images also gets the pictures on the pages it is shown,
as the message's own (at most ten images to a message). An encrypted
PDF is refused, and so is any other binary file (one with a NUL byte).
The transcript shows a folded "Attached:" line under the message.

On an Anthropic endpoint the agent's `read` shows it a PDF whole, its
text and an image of each page. Other models, and a PDF too long to
send whole, get its text, extracted, a `--- page N ---` line starting
each page, read on 2000 lines at a time; a model that takes images
also sees the pictures on the pages it reads, a scan's pages included.
An encrypted PDF is refused.

`@` in the prompt mentions a file or folder instead: a picked file's text is
attached too, and the path stays in the message, so the agent can read the
file or list the folder itself (see [Keyboard and mouse](help:keys)). A
word after `@` lists other sessions too, by title: picked, the agent is
given that session's id, to message it with `send_message`.

A long command or subagent keeps the agent from reading what you
send until it ends. Run in background, on its row, or Ctrl+B (also in
the Session menu and as `/background`) lets it go on by itself while
the agent goes on with you; the agent is told when it ends. See
[Jobs, todo and subagents](help:jobs).

A command's row shows the command in full, with a Copy button, above
its output. Stop, at the right of a running command's row, kills that
command alone: the agent gets what it printed so far, is told you
stopped it, and goes on. The row says "stopped".

A message starting with a command runs it instead of going to the
agent (Tab completes the name):

- `/compact [focus]` summarizes older messages now, keeping what the
  focus names;
- `/model [NAME]` switches the model, or picks one from a list;
- `/shell [NAME]` switches the shell, or picks one from a list;
- `/rename TITLE` sets the title;
- `/fork [TURN]` forks the session, or only its first turns;
- `/side` starts a side chat (below);
- `/worktree [NAME]` starts a worktree session (below), named NAME,
  else through its dialog;
- `/export [raw]` saves the transcript as Markdown, or its raw log;
- `/plan` and `/plan off` turn Plan mode on and off;
- `/background` moves the running command or subagents to the
  background, as Ctrl+B does;
- `/NAME [args]` uses the skill NAME (see
  [Instructions and skills](help:instructions)).

A change to the session, a model switched, a mode, a new title, shows
in the transcript as a line between messages, kept with the session;
the agent does not see it. What a command or an editor came to shows
over the foot of the transcript: an error until you type or close it,
anything else for a few seconds.

## Modes

The mode list on the Prompt's status line, beside the model and the
shell, has three modes:

- **Build**: the agent works on the task, changing files as it goes.
- **Plan**: the agent explores and writes a plan for you to approve
  before it changes anything, shown amber; the list then also shows
  the plan. See [Plan mode](help:plan).
- **Bypass**, in red: the agent reads, writes and acts anywhere
  without asking. See [Permissions and asks](help:permissions) before
  using it.

Only you turn on Bypass, from this list. A subagent has its parent's
mode. Plan mode turns on or off only while the session is idle or
waiting on an ask.

## The branch and its changes

In a git repository, the session bar shows the branch the session's
folder is on, cut in the middle if it is long (the commit, if
detached), and in amber anything under way, such as a rebase or a
merge, with its step. It is read from the repository's files: nothing
runs for it.

Click the branch for the Changes pane, and again to hide it. It lists
what `git status` does, conflicts first: each file changed, staged or
not, with the lines added and deleted, then new files not yet added.
Above them are the branch, its upstream, and the commits ahead (↑)
and behind (↓) it.

git runs only while the pane shows: as the agent's tools finish, as
the window takes the focus back, and on Refresh (the circular arrow), less often for a slow
repository, without taking git's locks or changing its index. It is
`[tools] git`, else the git on PATH (see
[Configuration](help:config)). A session on another host has its
host's git run there.

## The session's menu

Right-click a session in the sidebar, or open the Session menu, for:

- **Rename...**. A session is titled from its first prompt until you
  rename it.
- **Move to group**: an existing group, New group..., or Remove from
  group. Groups head the sidebar; a group's own menu renames,
  archives, ungroups, reorders or moves it to the Trash. Or drag the
  session, with its subagents, onto a group's heading, onto Archived,
  onto another session to join it there, or below the list to take it
  out of its group. While you drag, a strip at the foot of the sidebar
  offers Archive, New group... (named as you drop it) and Move to Trash,
  however long the list. Hold the drag just above it, or just under the
  pane's top, to scroll the list; an arrow over the strip says there is
  more below. Escape cancels a drag. A session never moves
  to another host, and dropping it never changes the sidebar's order.
- **Fork**: a new session with a copy of this conversation, titled
  "(fork)", which starts in Build mode. A No folder session's fork gets
  a copy of its folder.
- **Fork into another folder...**: a fork that works in a folder you
  pick, on the same host, while this session stays where it was. Its
  agent is told of the move, and that paths earlier in the conversation
  meant the old folder. The new folder's instruction files, memory and
  skills apply. Edits made before the fork can't be rewound from the
  fork, and "always allow" answers for paths in the old folder are
  dropped. Only between turns.
- **Start a side chat**: a fork you can make while the agent works
  (below).
- **Start a worktree session...**: a fresh session, nested under this
  one, working in a worktree of its own (below).
- **Switch model**: a configured model, or another typed in.
- **Switch shell**: a `[shell]` section of your configuration, or
  `bash` or `zsh` (not on Windows), `sh`, `powershell` or `cmd` where found, as the picker beside the model's
  offers. Commands run there from then on; the agent is told at its
  next request, once, and again after a compaction. A subagent starts
  in its parent's shell. If the shell's program is gone when the
  session opens again, commands run in the default shell, and the
  agent is told.
- **Export**: Markdown..., with or without reasoning and tool calls, to
  a file or the clipboard; or Raw log..., the session's log as it is.
- **Archive**: move it under Archived, at the end of the sidebar.
  Unarchive brings it back.
- **Move to Trash**: it goes, with its subagents, under the Trash
  heading at the end of the sidebar (below). A toast's Undo brings it
  back at once.
- **Delete Permanently...**: remove it, with its subagents, for good,
  once you confirm, without the Trash. Where the Trash is off, this is
  **Delete...**.
- **Copy session ID**.
- **Open working folder**: the folder the session works in, in Finder
  or Explorer, as the folder button beside the prompt box opens it (in
  a container, only a folder here mounted at `/work`). **Open session records**: its folder in the state
  directory, with its log, plan and command output.
- **Open in external terminal**: a terminal window in the folder the
  session works in, apart from the Terminal pane, running the
  session's shell (a POSIX sh as a login shell), or where that is
  `/bin/sh` or bash on Linux and macOS, the terminal's own. Its
  `[shell]` section's `env.NAME`, `path_prepend` and `path_append`
  apply, as to the agent's commands, and a WSL shell opens in its
  `distro` (but not on macOS, where Terminal keeps its own
  environment). On a remote it is a shell there, over
  ssh (`ssh -t`, so it may ask for a password) or WSL, and in a
  container a shell inside it by its runtime (`docker exec -it`), as
  the agent's user there. The terminal is
  `[ui] terminal`, else Terminal on macOS, Windows Terminal (or a
  console) on Windows, and elsewhere `$TERMINAL` or the first of the
  usual ones found.
- **New terminal tab** (Ctrl+Shift+`): another shell in its folder, in
  a tab of the Terminal pane.
- **Close**: stop its background jobs and subagents, deny what it asks
  from then on, and let it go from memory in this window. Opening it
  again reads it back.

Most of these wait until the turn ends.

Right-click in the transcript to Copy Message, or to fork there: Fork
before this prompt puts that prompt in the fork's box to edit and
send, and Fork after this turn keeps the conversation up to that
point.

## Side chats

A side chat asks about a session without disturbing it, even while its
agent works: Start a side chat in the transcript's right-click menu,
in the Session menu, or `/side`. It is a fork of the whole
conversation so far, titled "(side chat)", shown under its session in
the sidebar, and the up arrow on its bar goes back to it. From the transcript, the text
selected is quoted (`> `) in its box, ready for your question below
it. It waits for you: nothing is sent until you send it.

The side chat's agent is told that it is a side chat, and that the
main agent may still be working, so that it changes no files unless
you ask. It has every tool all the same. Calls the main agent had
under way are closed in the copy as still running there, and its
background jobs run on there alone.

## Worktree sessions

A worktree session works in a git worktree that Prospero makes for it,
on a branch of its own, so that it can change and commit files while
other sessions work in the folder. Start one from a folder with New
worktree in the New session dialog, or from a session with Start a
worktree session... in the transcript's right-click menu or the Session
menu, or `/worktree [NAME]`. The second starts fresh, with none of
the session's conversation, on the session's model, nested under it,
made even while its agent works; the selection, if any, is quoted in
its box, and its first prompt gives its title. Its agent knows which
session it came from: once its work is done, ask it to report to that
session, which can then review and merge its branch.

- **Where**: `.prospero/worktrees/NAME` in the repository's main
  checkout, on branch `prospero/NAME` (`[worktree] branch_prefix`),
  at the same subdirectory as the folder. A `.gitignore` there keeps
  the worktrees out of git status and searches.
- **From**: the folder's HEAD, or the session's, as the session is
  made. Uncommitted changes stay behind.
- **Making it**: git checks it out in the background; the session is
  there at once, and its first turn waits, "making the worktree".
- **The agent** is told where it works and on which branch, to commit
  there, and to leave the main checkout alone. One started from a
  session is told that session's id, and to report its branch and
  commits there with `send_message` when done or asked. Sessions in the main
  checkout ask before changing files in a worktree.
- **Archived, in the Trash or deleted**: once every session working in
  a worktree is,
  it is removed if git calls it clean, and its branch deleted only if
  merged into the branch it was cut from. A worktree with uncommitted
  changes is kept, and a note says so. On Windows, files in use (as by
  a program still running there) stay behind in its folder, which must
  be deleted before the worktree can be made there again. Unarchiving
  or restoring the session, or starting a turn in it, checks the branch
  out again at the same place.

## The Trash

A session moved to the Trash, by its menu, its group's, its row's
trash icon, or a drag onto the strip at the foot of the sidebar, is kept under the Trash heading
for `[sessions] trash_days` (30 by default), then removed for good.
Each row says how long it has left. It does not open there: its menu's
**Restore** puts it back where it was, as does dragging it out onto a
group, Archived or the list. **Delete Permanently...** removes it now,
and **Empty Trash...**, on the Trash heading's menu or the Session
menu, removes all of it, once you confirm. A host's engine keeps its
own Trash, by its own configuration, and empties what is due when it
starts and every hour. `trash_days = 0` turns the Trash off: deleting
is then at once, once confirmed, and what was in the Trash goes.

A container made for sessions keeps its Trash here instead: its last
session moved there stops it, restoring one starts it again, and its
sessions removed for good remove the container too.

Removed, a session's log and files are deleted from the state
directory, which is not a secure erasure: the disk, backups, and the
model's provider may still hold copies, and a worktree's unmerged
branch is kept. Delete Permanently only skips the wait in the Trash.

Bring the work back as with any branch, such as with `git merge` in
the main checkout.

## Undo and rewind

A tool row that changed a file has an Undo button (a curved arrow) at
its right, which puts back the file as it was before that call, and
every file changed after it. Files that commands changed are not
tracked, and cannot be put back this way.

Each of your prompts has Rewind to here (a clock). It takes the
conversation back to just before that prompt, in the same session,
and puts the prompt back in the box to edit and send again. The turns
from there on leave the conversation, so the agent no longer sees
them, though the session's log keeps them. The confirmation also
offers to put back the files the agent's file tools wrote or edited
from that turn on; a file changed since by something else is left
alone, and the card says so, with Put back anyway... to overwrite it.

A card marks the rewind, with Undo rewind, which brings the turns back
and the files as they were before the rewind. It is offered until the
next message is sent, also after a restart. Rewinding works only while
the session is idle (no turn, subagent or job running), and not to a
prompt before the latest compaction, which summarized it: there the
clock puts back files alone, where any changed. Subagents started in
the turns taken out stay in the sidebar.

## Import

Session → Import session... makes a new session from a raw log exported
here or on another machine, on the host, in the directory and with the
model you pick. It keeps the conversation, and drops what refers to
the old machine: allowed directories, snapshots, output files, images,
subagents and jobs. It is titled "(imported)" and starts in Build
mode.

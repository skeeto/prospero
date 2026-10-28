# Sessions and modes

## New sessions

Session → New session... (Ctrl+N, Cmd+N on macOS), or New session
here... in a host's menu in the sidebar, opens the New session dialog:

- **Host**, when more than one is ready: where the session runs.
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

## Talking to the agent

Enter sends the prompt. The buttons beside the prompt box are icons,
named in their tooltips: Send (a paper plane), and Attach (a paper
clip). While the agent works, Send becomes Steer (a curved arrow), and
Stop (a square) shows below it. What you send while it works waits at
the end of the transcript, dimmed and marked "Next step", until the
agent reads it at its next step (or marked "Queued", for a turn of its
own, as it is when sent too late for this turn), and its Withdraw
button takes it back into the box. Stop, or Esc, cancels the turn and
kills any command it runs.

Attach adds files to the message: drop them on the window, paste them,
or pick them with the paper clip. Images go to a model that takes
them, as pictures. Any other file goes as text, as `read` shows it:
numbered lines, the first 2000 or so, then a note to read on. The
session keeps a copy in its own folder, so the agent can read the rest
without asking. A binary file (one with a NUL byte) is refused. The
transcript shows a folded "Attached:" line under the message.

A long command or subagent keeps the agent from reading what you
send until it ends. Run in background, on its row, or Ctrl+B (also in
the Session menu and as `/background`) lets it go on by itself while
the agent goes on with you; the agent is told when it ends. See
[Jobs, todo and subagents](help:jobs).

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

The mode list above the transcript, beside the model and the shell,
has three modes:

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
  archives, ungroups, reorders or deletes it.
- **Fork**: a new session with a copy of this conversation, titled
  "(fork)", which starts in Build mode. A No folder session's fork gets
  a copy of its folder.
- **Start a side chat**: a fork you can make while the agent works
  (below).
- **Start a worktree session...**: the same, working in a worktree of
  its own (below).
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
- **Delete...**: remove it, with its subagents, for good.
- **Copy session ID**.
- **Open working folder**: the folder the session works in, in Finder
  or Explorer, as the folder button above the transcript opens it. **Open session records**: its folder in the state
  directory, with its log, plan and command output.
- **Open in terminal**: a terminal window in the folder the session
  works in, as the terminal button above the transcript and the Info
  popup's open one, running the
  session's shell (a POSIX sh as a login shell), or where that is
  `/bin/sh` or bash on Linux and macOS, the terminal's own. On a remote it is a shell there, over
  ssh (`ssh -t`, so it may ask for a password) or WSL. The terminal is
  `[ui] terminal`, else Terminal on macOS, Windows Terminal (or a
  console) on Windows, and elsewhere `$TERMINAL` or the first of the
  usual ones found.
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
the sidebar, and its bar links back. From the transcript, the text
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
menu, or `/worktree [NAME]`. The second is a side chat in a worktree:
a copy of the whole conversation, made even while the agent works,
nested under its session, its first prompt giving its title.

- **Where**: `.prospero/worktrees/NAME` in the repository's main
  checkout, on branch `prospero/NAME` (`[worktree] branch_prefix`),
  at the same subdirectory as the folder. A `.gitignore` there keeps
  the worktrees out of git status and searches.
- **From**: the folder's HEAD, or the session's, as the session is
  made. Uncommitted changes stay behind.
- **Making it**: git checks it out in the background; the session is
  there at once, and its first turn waits, "making the worktree".
- **The agent** is told where it works and on which branch, to commit
  there, and to leave the main checkout alone. Sessions in the main
  checkout ask before changing files in a worktree.
- **Archived or deleted**: once every session working in a worktree is,
  it is removed if git calls it clean, and its branch deleted only if
  merged into the branch it was cut from. A worktree with uncommitted
  changes is kept, and a note says so. On Windows, files in use (as by
  a program still running there) stay behind in its folder, which must
  be deleted before the worktree can be made there again. Unarchiving
  the session, or starting a turn in it, checks the branch out again
  at the same place.

Bring the work back as with any branch, such as with `git merge` in
the main checkout.

## Undo and rewind

A tool row that changed a file has an Undo button (a curved arrow) at
its right, which puts back the file as it was before that call, and
every file changed after it. A message that began a turn has Rewind to
here (a clock), which puts back every file the agent's file tools
wrote or edited from that turn on. Both ask first. Files that commands changed are not tracked, and cannot be
put back this way.

## Import

Session → Import session... makes a new session from a raw log exported
here or on another machine, on the host, in the directory and with the
model you pick. It keeps the conversation, and drops what refers to
the old machine: allowed directories, snapshots, output files, images,
subagents and jobs. It is titled "(imported)" and starts in Build
mode.

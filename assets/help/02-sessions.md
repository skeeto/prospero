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
- **Model**: empty for the configured one.
- **Role**, when the configuration has roles: start in one, with its
  instructions, model and tools (see [Subagents](help:jobs)). A
  session with a role cannot use Plan mode.
- **Plan mode**: start in Plan mode (below).

A folder dropped on the window opens the dialog with that folder
filled in. `prospero DIR` in a terminal starts a session in DIR at
once.

## Talking to the agent

Enter sends the prompt. While the agent works, the Send button becomes
Steer: what you send then waits above the box, marked "next step",
until the agent reads it at its next step (or marked "queued", for a
turn of its own), and its Withdraw button takes it back into the box. Stop, or Esc, cancels the turn and kills
any command it runs.

A message starting with a command runs it instead of going to the
agent (Tab completes the name):

- `/compact [focus]` summarizes older messages now, keeping what the
  focus names;
- `/model [NAME]` switches the model, or picks one from a list;
- `/rename TITLE` sets the title;
- `/fork [TURN]` forks the session, or only its first turns;
- `/side` starts a side chat (below);
- `/export [raw]` saves the transcript as Markdown, or its raw log;
- `/plan` and `/plan off` turn Plan mode on and off;
- `/NAME [args]` uses the skill NAME (see
  [Instructions and skills](help:instructions)).

## Modes

The mode list below Send has three modes:

- **Build**: the agent works on the task, changing files as it goes.
- **Plan**: the agent explores and writes a plan for you to approve
  before it changes anything. See [Plan mode](help:plan).
- **Bypass**, in red: the agent reads, writes and acts anywhere
  without asking. See [Permissions and asks](help:permissions) before
  using it.

Only you turn on Bypass, from this list. A subagent has its parent's
mode. Plan mode turns on or off only while the session is idle or
waiting on an ask.

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
- **Compact**: summarize the conversation now.
- **Switch model**: a configured model, or another typed in.
- **Export**: Markdown..., with or without reasoning and tool calls, to
  a file or the clipboard; or Raw log..., the session's log as it is.
- **Archive**: move it under Archived, at the end of the sidebar.
  Unarchive brings it back.
- **Delete...**: remove it, with its subagents, for good.
- **Copy session id**, **Reveal folder** (the session's folder in the
  state directory) and **Close**.

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

## Undo and rewind

A tool row that changed a file has an Undo button, which puts back the
file as it was before that call, and every file changed after it. A
message that began a turn has Rewind to here, which puts back every
file the agent's file tools wrote or edited from that turn on. Both
ask first. Files that commands changed are not tracked, and cannot be
put back this way.

## Import

Session → Import session... makes a new session from a raw log exported
here or on another machine, on the host, in the directory and with the
model you pick. It keeps the conversation, and drops what refers to
the old machine: allowed directories, snapshots, output files, images,
subagents and jobs. It is titled "(imported)" and starts in Build
mode.

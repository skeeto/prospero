# Permissions and asks

The working directory is the agent's to read and write. Its file tools
(read, write, edit, grep and glob) go anywhere in it without asking,
and in the directories that `[guard] roots` adds. A path through a
symbolic link counts as where the link leads.

Anywhere else, reading as well as writing, the tool asks you first.
Shell commands do not ask: the agent is told to stay inside the
working directory, to reach outside it only with the file tools, so
that you are asked, and not to reach a denied path another way. But
nothing holds a command to it, and files that commands change cannot
be undone. After a denial the agent is told to go on without the path
and, if it cannot finish, to ask you for it when it ends its turn.

## Answering an ask

An ask shows as a card in the transcript, "Allow TOOL PATH?", with a
countdown: it is denied unless you answer within `[guard] ask_timeout`
seconds (60 by default). **Keep waiting**, at the card's top right,
drops the countdown for that one ask, which then waits until you answer
it. Set to 0, every ask waits until you answer. Either way that holds
while a window has the session open; with none, asks are denied at
once. Its buttons:

- **Allow** lets this one call go ahead.
- **Always allow DIR (read)**, or **(write)**, set apart from Allow,
  allows every read, or every read and write, in that directory from
  now on, for this session and its subagents, even after it is opened
  again.
- **Deny**: the agent reads why, and tries something else.
- **Deny with note...** denies, telling the agent what to do instead.

A message you send while an ask waits does not answer it: it waits
below the card, marked "Next step", and the agent reads it once you
answer, along with the outcome. To refuse and say what to do instead,
use **Deny with note...**. **Stop** ends the turn, ask and all.

A write or edit of Prospero's own configuration (its file, and the
local files it includes) asks every time, even inside the working
directory, the roots or a directory you always allowed, and offers no
Always: the file chooses what runs and where the agent may go. Only
Bypass mode writes it unasked, or a write grant on the file itself in
the [Permissions pane](help:permissions#the-permissions-pane). See
[Configuration](help:config#the-agent-changes-it).

## The Permissions pane

The shield at the top right of the Transcript, or **Permissions...**
in the Session menu, shows the [Permissions](view:permissions) pane:
what the agent may reach without asking, in this session and its
subagents. Grant ahead what you expect it to need, or take back what
it no longer does. The agent is told of each change before its next
step. These rules hold the file tools, not shell commands, which the
agent is told to keep to them too.

Under **Folders and files**, the workspace comes first: the session's
folder, the roots and the temporary directories, which you cannot
change here. Then each path granted or denied, with three boxes:

- **Read**: read it, and everything in it, without asking.
- **Write**: write there too.
- **Deny**: the file tools ask there every time, with no Always, even
  inside the session's folder or a grant; grep and glob leave it out of
  their results; and Bypass mode refuses it. A grant inside a denied
  folder opens that part of it again: the deepest rule wins.

Grants made by Always show here too, and can be taken back. Add a path
in the field at the end, typed with completion. A folder granted for
writes that holds Prospero's configuration still asks for it; grant
the file itself for writes to let the agent change it unasked, each
write reloading it.

The other sections grant the desktop tool's classes (Input covers
Observe; starting programs always asks), each MCP server's tools, web
searches and the pages of a site, and container images for subagents,
as far as the session offers them.

Changes reach the session and its subagents open in this window or
process. One not open keeps what it had until you change it in its
own pane. A subagent in a worktree of its own gets a grant or deny in
its parent's checkout at the same place in its worktree, and a deny
holds in both.

## Attended and unattended

A session is attended while a window has it open. One nobody is
watching cannot ask: its asks are denied at once. That is so for
`prospero run`, for evals, and for a window closed with "Keep running
in background", until a window opens the session again.

## Temporary directories

Each session has a scratch directory of its own in the system's
temporary directory, for the agent's scratch files. The file tools may
use it, and the rest of the temporary directory, without asking,
unless `[guard] temp = ask`.

## Roots

`[guard] roots` lists more directories where the file tools go freely,
separated by `;`, with `~` for the home directory:

    [guard]
    roots = ~/src/shared; /tmp/scratch

## Bypass

Bypass mode, picked in red from the mode list on the Prompt's status line, is for an
agent left to work alone somewhere it can do no harm. Nothing is
asked, attended or not: the file tools read and write outside the
working directory, the desktop tool acts, starting programs too, and
the web tools search and read pages ([Web search](help:web)).
Asks already waiting when you pick it are allowed.

Even in Bypass, the agent's questions and plan approvals remain (it
cannot ask to plan, so that it neither waits on you nor leaves Bypass),
and the harness still refuses:

- writes under the state directory, other than the plan file;
- paths denied in the [Permissions pane](help:permissions#the-permissions-pane);
- a path that names nothing (one with a NUL byte);
- a web page whose address never appeared in the conversation, or a
  local one.

Only you turn Bypass on, never a new session, the agent or the
configuration. It stays through reopening and restarts, and subagents
follow it. A fork or an import starts in Build, and so does an
approved plan.

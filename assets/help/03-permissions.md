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
seconds (30 by default). Its buttons:

- **Allow** lets this one call go ahead.
- **Always allow DIR (read)**, or **(write)**, set apart from Allow,
  allows every read, or every read and write, in that directory from
  now on, for this session and its subagents, even after it is opened
  again.
- **Deny**: the agent reads why, and tries something else.
- **Deny with note...** denies, telling the agent what to do instead.

Sending a message while an ask waits denies it, and the agent reads
your message.

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

Bypass mode, picked in red from the mode list below Send, is for an
agent left to work alone somewhere it can do no harm. Nothing is
asked, attended or not: the file tools read and write outside the
working directory, and the desktop tool acts, starting programs too.
Asks already waiting when you pick it are allowed.

Even in Bypass, the agent's own asks about plan mode remain, and the
harness still refuses:

- writes under the state directory, other than the plan file;
- a path that names nothing (one with a NUL byte).

Only you turn Bypass on, never a new session, the agent or the
configuration. It stays through reopening and restarts, and subagents
follow it. A fork or an import starts in Build, and so does an
approved plan.

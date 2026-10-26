# Jobs, todo and subagents

## The todo list

For a longer task the agent keeps a todo list, which the Todo pane
shows, with how many items are done. If a turn ends with items left
undone, the agent is reminded of them.

## Background jobs

A shell command still running at its timeout (two minutes, unless the
agent asks for another, up to ten) is not killed: it moves to the
background as a job, and the agent goes on. The agent may also start a
command in the background at once. It reads a job's output while it
runs, and is told when it ends.

You need not wait for the timeout: Run in background, on the running
command's row, or Ctrl+B, moves it to the background as a job now,
and the agent goes on, so that you can talk to it while the command
runs. The agent is told that you moved it, and is told when it ends,
as for any job. Only you can do this; the agent cannot. With 16 jobs
running already in the session, it is refused.

The Jobs pane lists the session's jobs, with their state, run time and
output size. Kill stops a job and what it started, and the agent is
told.

- `[tools] job_timeout`: seconds a job may run, unless the agent asks
  for another limit (1800 by default, at most `job_timeout_max`, 7200).
- `[tools] auto_resume`: when a job ends while the session is idle,
  the session starts a turn by itself to report it, at most this many
  times an hour (5 by default; 0 for never).

Closing a session stops its jobs, and so does quitting.

## Subagents

The agent may hand a task to a subagent, a session of its own with its
own conversation, which reports back when done. Several may run at
once. Each runs in a role:

- `general`, every tool, the default;
- `explore`, which finds and reads code to answer a question, and
  changes nothing;
- any `[role NAME]` in the configuration.

A role's `tools` are read left to right: `*` is every tool, `default`
those a role has when it names none (every tool but desktop), and
`-NAME` takes one away. So `tools = default desktop` adds the desktop
tool, and `tools = * -shell` gives every tool but the shell. The New
session dialog's Role starts a session in a role, too.

A subagent shows under its parent in the sidebar, folded away by its
arrow. In the parent's transcript, its task row has Open subagent (or
Answer the subagent, when it asks something), and the subagent's own
transcript links back to its parent. A subagent has its parent's mode
and allowed directories, and cancelling the parent cancels it.

### Subagents in the background

The parent waits for its subagents to report. Run in background, on a
task row at work, moves that subagent to the background, and Ctrl+B
every subagent the turn waits for: the parent's call ends at once,
saying so, and the parent goes on while the subagent works. When the
subagent is done, its report (or how it failed, or that it was
cancelled) comes to the parent as a notice, which starts a turn of the
parent's if it is idle, even if it was closed meanwhile. The task row
says the subagent is still at work, and the subagent's own session
bar that it reports in the background.

Cancelling the parent leaves a subagent in the background at work, as
it does jobs: stop it from its own session. Quitting or restarting
Prospero stops it as it stops any turn, and then its parent is not
told.

### Subagents on other hosts

A task may run on one of your [remote hosts](help:hosts) instead: the
agent's task tool names it as `host`, and on Windows that includes
each WSL distro, as when the agent works in Windows but needs Linux.
The subagent is a session there, under that host's own configuration,
tools and permissions, and only its report comes back. Its role and
model are that host's, and it starts in Plan mode if its parent plans,
else in Build. In a WSL distro it works in its parent's folder as the
distro sees it (`C:\work` is `/mnt/c/work`); over ssh, in its home
directory, sharing no files with this machine.

Its parent's engine reaches the host itself, as its shell could run
`ssh` or `wsl.exe`, so this needs no window. The window reaches it
too, by the same name, to show it under its parent, marked "on NAME",
and to answer its asks. After Disconnect, the window leaves that host
alone, and the task row says that the subagent's asks are denied;
Open subagent connects again. `subagents = no` in a `[remote NAME]`
section keeps the agent off that host.

A subagent may start subagents of its own, but theirs cannot. It is
told to wrap up after `[tools] task_budget` model requests (200 by
default), and stopped at half as many again.

## Sessions talking to each other

The agent can list the other sessions on its host and send one a
message, which it reads at its next step, or which starts a turn if it
is idle. Such a message shows as from that session, never as yours,
and never counts as your permission.

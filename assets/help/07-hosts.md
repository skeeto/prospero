# Hosts and remotes

A host is an engine, the part of Prospero that runs sessions and their
tools. The window has one of its own, in the same process. Others are
listed in the Hosts pane (View → Hosts) and head their sessions in the
sidebar.

A session runs on its host: its tools, its commands and its files are
there, and so is its configuration. The window only shows it.

## Background hosts

Closing the window while turns run asks what to do: Stop the sessions
and quit, or Keep running in background. In the background this
process keeps running its sessions, unattended (their asks are
denied), with a tray icon whose menu opens the window again, stops all
sessions, or quits. Launching Prospero again brings the window back.

`prospero serve` runs an engine as a background host from a terminal.
A host with no client and nothing running exits after `[host]
idle_exit` seconds (600 by default). The Hosts pane lists those on
this computer, under This computer, each with a Stop button; Look
again looks for new ones.

## Remote hosts

A remote host is a machine reached over ssh. Add one with Add
remote... under Remotes in Settings, or in the Hosts pane: a name, an
ssh destination (`user@host`, or a Host from `~/.ssh/config`), and
optionally where `prospero` is there. A port other than 22 goes after
the host, `user@host:2222` (an IPv6 address in brackets,
`[::1]:2222`), and Prospero passes it to ssh and scp as their own
option; or set `Port` in `~/.ssh/config`. It writes a `[remote NAME]`
section to your configuration, whose page in Settings shows how the
host is, with Connect and Deploy engine, as the Hosts pane does.

Connect, in the Hosts pane or the host's menu in the sidebar, runs
`ssh` and `prospero attach` there, which starts a host that outlives
the connection. ssh runs in batch mode, so it cannot ask for a
password: use a key, or an ssh agent. A connect that fails says why
in a toast, at the foot of the Sessions or Hosts pane, where it began,
with Deploy engine when the machine has no engine. A connection that
breaks is taken up again, and the host keeps your place for three
hours. Meanwhile its line says when it tries again; hover over it, or
over the host's heading in the sidebar, for why it went down, or why a
connect not begun by a click (as at launch) failed.

## WSL distros

On Windows, each installed WSL distro is a remote host by itself, named
after it ("Debian (WSL)"), reached through `wsl.exe` rather than ssh:
nothing to set up in the distro but an engine, which deploying puts
there as for any Linux machine (with `cat`, as a distro has no scp).
The list follows distros as they are installed or unregistered, and
Refresh in the Sessions pane or Look again in the Hosts pane lists
them anew. Docker Desktop's and Podman's own distros are left out. A
distro that isn't running yet starts as it's connected to, which may
take a minute or two as WSL itself starts: its host says "starting
WSL's DISTRO" meanwhile. An unconfigured distro's Settings button in the
Hosts pane gives it a `[remote NAME]` section with `wsl = DISTRO`, to
change its settings.
A section with `wsl = DISTRO` in place of `ssh` names one yourself,
under another name, and `prospero connect NAME` and `prospero deploy
NAME` take a distro's name too. The engine runs in the distro's home
directory; Windows' drives are under `/mnt`, as `/mnt/c`, and a Windows
path typed for a new session's folder there, as `C:\Users`, becomes
the distro's, `/mnt/c/Users`. To have only the commands of a session
on Windows run in a distro, give it a `[shell]` with `program = wsl`
instead (Configuration).

## A container, by a command

A `[remote NAME]` with `exec` in place of `ssh` reaches a machine by a
command that runs another inside it: `exec = docker exec -i fuzzbox`
reaches a running container, and `podman exec -i`, `incus exec NAME
--` or `kubectl exec -i POD --` work alike. The command runs here with
no shell, its words split at blanks, and `'...'` or `"..."` keep one
together. Add remote... takes one when its choice says
command. The machine needs `/bin/sh`; the engine runs in its home
directory, and deploying puts one there with `cat`, as into a WSL
distro. A container with no network still works: a remote with no
models of its own makes its model calls through this computer (below).
A missing container fails at once, saying so, and isn't tried again.

## Containers made for sessions

With Docker or Podman installed, Sessions lists it below the remotes,
with no configuration: dimmed, saying why, while its daemon isn't
running, and Look again (its menu, or Refresh) asks again. Its + opens
New session for a container of the session's own:

- **Image**: any Linux image, pulled first if it isn't here. Those
  `[container] images` lists come first, then the images here.
- **Workspace**: the agent works in `/work`, which is empty, a copy of
  a folder here (what the agent changes stays in the container), or a
  folder here mounted there (the agent changes it as it goes).
- **Network access**: off by default (`[container] network`), so the
  container reaches no network. Its model calls and web searches still
  go through this computer, which relays them.

Each container is a host of its own in the sidebar, its image and name
as its label; the engine is deployed into it, as into any remote, and
its sessions and logs are kept here, in the state directory, mounted at
`/prospero/state`. Its last session moved to the Trash stops
its container, and restoring one starts it again. Its menu's Stop
container stops it while keeping its sessions, and Connect starts it;
Delete container..., also in the Hosts pane's Containers, removes it
and its sessions for good, as emptying the Trash or the Trash's purge
after `trash_days` does. A mounted folder is never removed.

A session's Open working folder opens the mounted folder here, where
its folder is in it; with a copy or an empty workspace its files are
only in the container. Open in external terminal, and a Terminal pane
tab (the terminal button beside the prompt box), open a shell inside
the container, in the session's folder, while it runs.

Whatever user the image names, the engine and the agent run as root in
the container, which is you, not root, on this computer with Docker
Desktop, Podman, or rootless Docker. A rootful Docker on Linux would
make that root on the folders it shares with this computer, so there
they run as your own user and group instead: files left in `/out` or
a mounted folder are yours, though the agent cannot install packages
in the container.

`[container]` in your own file chooses the runtime (`docker`, `podman`,
or a path), `cpus`, `memory` and further `args` for each new container,
and `list = never` hides it all.

## Containers for subagents

With a runtime running, an agent may also start a subagent in a new
container of an image (the task tool's image), for work best kept away
from this computer: untrusted code, fuzzing, crashes. It has no network
unless the agent asks for it, and works in `/work`, empty or with a
copy of a folder in the workspace. It shares no files with this
computer but `/out`, a folder in the session's scratch directory, whose
place its report gives; the container is removed when the subagent
ends. An image `[container] images` lists runs without asking; any
other, or one with network access, asks you first, and Always allows
that image for the rest of the session. With no network, the subagent
works in Bypass mode, as it can reach nothing of yours; with network,
it asks as any session. While it works, the sidebar shows its container
under Docker or Podman, and its task's Open subagent shows it there, to
watch it or answer it. Once it ends, its transcript is kept here, as a
subagent of its parent, and goes when the parent is deleted.
`[container] subagents = no` turns this off. The Community Edition needs a Linux engine for it,
which the Professional Edition bundles.

## Sessions on a remote

A host's menu in the sidebar also has New session here... and Import
session here.... A remote's sessions use the models of its own
configuration, or this computer's when it has none (below). With
`list = connected` in its section, a remote is left out of the sidebar
until connected.

A Terminal pane tab for a session on another host, a remote, a
container or a background host, runs its shell there, by that host's
engine, in the session's folder. While the connection is broken the
tab says Reconnecting, keeps its shell, and shows what it printed
meanwhile once the connection is back; typing meanwhile is lost. A
host that was disconnected, or that started over, ends its tabs. A
host whose engine is older than this window says so: deploy an engine
(below) to have terminal tabs there. Closing the window to run in the
background hangs up this window's terminal tabs.

The Files pane shows a file on another host, a remote, a container or
a WSL distro, as that host's engine reads it there, a part at a time:
Open file... (Ctrl+O) with such a session shown asks for a path there,
completed and checked there, and a link to a file in such a session's
transcript, or the path in its read, edit or write row, opens the file
there, its tooltip saying on which host. While the connection is
broken, a file not yet read there says its host is not connected, and
is read once it is back. One shown is checked there, and read again
when it changes, as a file on this computer is ([Files](help:sessions#files)).
An engine older than this window says it is
too old to show its files: update it (below). A file on a share or
mount there that stops answering does not stop the rest: after ten
to twenty seconds Open file says "The file system is not responding", other
files there still open, and that file, or anything under it, is
refused at once until the share answers again; then open it again.

Open in editor on such a file opens a Terminal tab of its session
there, where that host's engine runs the editor on it, at the line in
view, as on this computer ([Files](help:sessions#files)): the tab
closes when the editor exits with 0, and Files reads the file again
there. The editor is `[ui] editor` when it runs in a terminal (`vim`,
`nvim`, `nano`, `hx`, `emacs -nw`): as written on a host of the same
kind as this computer (Windows, or not), and on the other kind by its
name alone, as `nvim` for `C:\Tools\nvim.exe`; `[ui] editor_line`
goes only to a host of the same kind. Otherwise, and never from this
computer's own `$VISUAL` or `$EDITOR`, the host's own editor runs: on
Linux and macOS the one its login shell's `$VISUAL` or `$EDITOR`
names, else `vi` (busybox's `vi`, as in Alpine, at the top, without
the line); on Windows `%VISUAL%` or `%EDITOR%`, else `vi`, found on its
PATH. An image with no editor at all ends the tab with code 127, saying
so. A file in a container's folder mounted here, or on Windows in a WSL
distro, still opens here when the editor has a window of its own (VS
Code, Notepad++), through this computer's path to it. A Windows host
refuses a path holding `%`, `"` or a control character, as on Windows
here. An engine older than this window says to update it (below), and
a Windows host before Windows 10 version 1809 has no terminals.
Closing the window, or running it in the background, hangs the editor
up and loses its unsaved edits; a broken connection only shows
Reconnecting, the editor running on.

## A remote with no models of its own

A remote needs no configuration. One whose configuration has no
`[endpoint]`, or that has no file at all, uses the models of the
computer connected to it. The window offers each remote it connects to
its endpoints' and models' settings, never their URLs or keys, and
makes the remote's model calls itself, with its own keys, sending the
responses back. The Hosts pane says so under the remote, and the model
picker of its sessions lists this computer's models. Each request
travels as its difference from the session's last one, compressed.

While a call runs this way, the Prompt's status line says "via this computer",
or the name of the computer relaying it. With no computer connected
that offers the session's endpoint, its turn pauses ("paused: no relay",
in amber) and goes on once one connects, for at most that endpoint's
`slow_retry_budget` (two hours unless set), and then ends. A response
cut off by a lost connection goes on from what was shown, as after a
dropped connection to an endpoint.

Web search on such a remote runs on the computer relaying it, with
that computer's `[web]` settings, unless the remote's own
configuration sets `[web] search`; `none` there turns it off. Reading
web pages follows the remote's own `[web]`, so it is off until set
there. A subagent that an agent here starts on the remote runs on its
parent's models the same way, with no window needed. Guard, tools and
shell settings are the remote's own, its built-in defaults if it has
no file.

Such a remote also has your own skills here ([Skills](help:instructions#skills)):
those of `[skills] paths` in your own file, `--skillsdir`, `skills`
next to your configuration and the four folders under `~`, but never a
project's, nor the built-in `prospero`, which the remote has of its
own. Its sessions list them below its own skills, and the Info list
says "from" this computer. Each use copies the skill's whole folder
into the session's folder there, where its scripts run (past 2 MB,
`SKILL.md` alone). A skill for you alone (`disable-model-invocation`,
`[skills] hidden`) goes there only for your own /NAME, which fetches
its copy first, and never to an agent's subagent. `[skills] share =
no` in your own file shares none, and names share only those. A
subagent's container with no network gets none.

An `[endpoint]` in the remote's own configuration makes it independent:
its sessions then use only its own models.

## Subagents there

The agent may start a subagent on a remote host, or in a WSL distro,
which works there and reports back: see
[Subagents on other hosts](help:jobs). Its engine deploys there as
connecting does, waiting for any deploy already under way.

## Deploying an engine

The remote machine needs Prospero built for it. `prospero deploy NAME`
uploads an engine with scp to `~/.prospero` there, and records where it
went, for connecting. Engines come from an `engines` directory next to
the executable, or from one beside `prospero.ini` (Settings shows where)
when the first has none for the platform, or are the executable itself
when it is built for that platform. The Community Edition carries no
others: the engines zip that `tools/engines.py --zip` builds unpacks
into either place. The Professional Edition, from itch.io, has every
engine built in, so it needs neither. Connecting deploys one by itself
when the machine has none, or one of another protocol (`deploy = auto`,
the default), as for a WSL distro the first time. A deploy that fails
says why in a toast, at the foot of the Sessions or Hosts pane, where it
began. With `deploy = ask` in the remote's section, the connection fails
instead, saying why, and its toast offers Deploy engine.

Deploy engine, in the Hosts pane under a remote that is not connected
and in its menu in the sidebar, deploys this version's engine there and
then connects; `prospero deploy NAME` deploys from a terminal. A remote
whose section names its engine with `path` has none to deploy.

A remote whose engine is older than this program (an earlier version,
or the same one lacking something this program knows) shows what it
cannot do disabled. The Hosts pane says so under it, and Update engine
there and in its menu deploys this version's engine and starts it once
nothing runs there. Unless its section says `deploy = ask`, connecting
to an older engine updates it by itself, once nothing runs there.

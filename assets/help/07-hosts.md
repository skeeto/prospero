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

A remote host is a machine reached over ssh. Add one in the Hosts
pane, under Add a remote host: a name, an ssh destination
(`user@host`, or a Host from `~/.ssh/config`), and optionally where
`prospero` is there. Add remote writes a `[remote NAME]` section to
your configuration.

Connect, in the Hosts pane or the host's menu in the sidebar, runs
`ssh` and `prospero attach` there, which starts a host that outlives
the connection. ssh runs in batch mode, so it cannot ask for a
password: use a key, or an ssh agent. A connection that breaks is
taken up again, and the host keeps your place for three hours.

## WSL distros

On Windows, each installed WSL distro is a remote host by itself, named
after it ("Debian (WSL)"), reached through `wsl.exe` rather than ssh:
nothing to set up in the distro but an engine, which deploying puts
there as for any Linux machine (with `cat`, as a distro has no scp).
Look again in the Hosts pane lists them anew. Docker Desktop's own
distros are left out. An unconfigured distro's Settings button in the
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

## Sessions on a remote

A host's menu in the sidebar also has New session here... and Import
session here.... A remote's sessions use the models of its own
configuration, or this computer's when it has none (below). With
`list = connected` in its section, a remote is left out of the sidebar
until connected.

## A remote with no models of its own

A remote needs no configuration. One whose configuration has no
`[endpoint]`, or that has no file at all, uses the models of the
computer connected to it. The window offers each remote it connects to
its endpoints' and models' settings, never their URLs or keys, and
makes the remote's model calls itself, with its own keys, sending the
responses back. The Hosts pane says so under the remote, and the model
picker of its sessions lists this computer's models. Each request
travels as its difference from the session's last one, compressed.

While a call runs this way, the session bar says "via this computer",
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
instead, saying why.

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

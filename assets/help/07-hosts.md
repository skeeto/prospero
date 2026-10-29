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
configuration. With `list = connected` in its section, a remote is
left out of the sidebar until connected.

## Subagents there

The agent may start a subagent on a remote host, or in a WSL distro,
which works there and reports back: see
[Subagents on other hosts](help:jobs). Its engine deploys there as
connecting does, waiting for any deploy already under way.

## Deploying an engine

The remote machine needs Prospero built for it. `prospero deploy NAME`
uploads an engine with scp to `~/.prospero` there, and records where
it went, for connecting. Engines come from an `engines` directory next
to the executable, or from one beside `prospero.ini` (Settings shows
where) when the first has none for the platform, as releases name them, or are the executable itself when it
is built for that platform. The engines zip (`tools/engines.py --zip`)
unpacks into either place. The itch.io build of Prospero has every
engine built in, so it needs neither. With `deploy = auto` in the
remote's section, connecting deploys one when the machine has none.
Otherwise (`deploy = ask`, the default) the connection fails, saying
why.

Deploy engine, in the Hosts pane under a remote that is not connected
and in its menu in the sidebar, deploys this version's engine there and
then connects; `prospero deploy NAME` deploys from a terminal. A remote
whose section names its engine with `path` has none to deploy.

A remote whose engine is older than this program (an earlier version,
or the same one lacking something this program knows) shows what it
cannot do disabled. The Hosts pane says so under it, and Update engine
there and in its menu deploys this version's engine and starts it once
nothing runs there. With `deploy = auto`, connecting to an older engine
updates it by itself.

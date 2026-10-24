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

A host's menu in the sidebar also has New session here... and Import
session here.... A remote's sessions use the models of its own
configuration. With `list = connected` in its section, a remote is
left out of the sidebar until connected.

## Deploying an engine

The remote machine needs Prospero built for it. `prospero deploy NAME`
uploads an engine with scp to `~/.prospero` there, and records where
it went, for connecting. Engines come from an `engines` directory next
to the executable, as releases name them, or are the executable itself
when it is built for that platform. With `deploy = auto` in the
remote's section, connecting deploys one when the machine has none.
Otherwise (`deploy = ask`, the default) the connection fails, and the
Hosts pane offers Deploy engine under the reason, which deploys one and
connects; `prospero deploy NAME` does the same from a terminal.

A remote whose engine is older than this program shows what it cannot
do disabled, and its menu has Update engine, which deploys this
version's engine and starts it once nothing runs there.

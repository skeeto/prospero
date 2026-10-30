# Remote and background hosts

A host quits as when a client asks it to, closing its endpoint and
removing its registration, on SIGTERM, SIGINT or SIGHUP, or on Windows
Ctrl+C or Ctrl+Break at its console.

A `[remote NAME]` section names a machine reached over ssh, and the
window's Hosts panel adds one and connects to it. Prospero there runs
as `prospero attach`, started over `ssh -T`, which starts a host that
outlives the ssh session. A link that breaks is taken up again from
where it was, and the host keeps a client's place for three hours. A
window closed with "Keep running in background" makes its own process
such a host, and keeps the state directory, so a later launch brings
that window back. On Windows, a WSL distro is a remote too, reached
through `wsl.exe -d DISTRO --cd ~ --exec /bin/sh -c ...` instead of
ssh: every installed distro is listed (`wsl.exe -l -q`) as a remote of
its own name, Docker Desktop's left out, and a section with `wsl =
DISTRO` in place of `ssh` names one. Deploying into a distro sends the
engine on `cat`'s input, as it has no scp, and records it as
`wsl:DISTRO`. A window on a state directory that `prospero serve`
hosts lists the sessions the host holds and attaches to them.

A remote machine needs an engine built for its platform, which
`prospero deploy` uploads with scp to `~/.prospero` there, as
`engine-PROTO-VERSION-OS-ARCH`, verifies it by its SHA-256, and
records where it went, for connecting. Connecting deploys one by
itself when the machine has none, or one of another protocol (`deploy
= auto`, the default); with `deploy = ask` in its section, a failed
connection offers Deploy engine in the Hosts pane instead. A deploy
that fails says why in a toast, in the Sessions or Hosts pane, where
it began. Engines come
from the `engines` directory next to the executable, or when it has
none for the platform, the one beside the default `prospero.ini`, named
`prospero-engine-VERSION-OS-ARCH`, or are the executable itself when it
is built for that platform. An itch.io build carries every engine
built in, used when neither directory has one: it extracts it into the
state directory's `deploy/engines`, and deploys it from there. `tools/engines.py` builds them: macOS
arm64, Windows x86-64 and i686, and Linux aarch64 and x86-64 (static,
in the Docker image with `--docker`, or else on a Linux host given
with `--linux HOST`, else left out).
A remote whose engine is older than this program shows the operations
it lacks disabled, and a host's menu has Update engine, which deploys
this version's engine there and reconnects to it once nothing runs.
Its sessions' model picker lists the models of the remote's own
configuration. `list = connected` leaves a remote out of the sidebar
until it is connected.

The agent's task tool may start a subagent on a remote, or on Windows
in a WSL distro, by its name (`host`), with `cwd` there. The parent's
engine links to it as the window would, with `new {task}`, so this
works with no window too: the subagent is a session there, under that
host's configuration, tools and asks, in Plan mode if its parent plans
and else Build, with that host's role and model (the call's, the
role's, the parent's ENDPOINT/MODEL if it has that endpoint, else its
default). In a distro it starts in the parent's directory as the
distro sees it, under `/mnt`; over ssh in its home. It writes its
report in its log (`task.report`), and the parent's call ends with it.
The window reaches the subagent by the same name, nests it under its
parent, and answers its asks; with no window to show it, they are
denied at their deadline. `subagents = no` leaves a remote out.
Deploys to one destination take turns, so the window and an engine
never upload over each other.

A desktop helper there for the platform,
`prospero-desktop-VERSION-OS-ARCH`, goes too, as
`desktop-PROTO-VERSION-OS-ARCH` beside the engine, where the engine
looks for it first; with the executable itself as the engine, its own
`prospero-desktop` goes. An engine that is its own helper, a Windows
one or this executable built from source on Linux, gets none. `tools/engines.py` builds the Linux helpers beside their
engines. The engine works without a helper, so one
that cannot be put there, or does not run there, is only a warning.

An engine whose own configuration has no `[endpoint]` with a real URL
is thin: it uses the models of the clients connected to it. A client
that knows the `relay` cap sends `relay.offer` with its offer, the
settings of its endpoints (kind, timeouts, retry budget, the model ids
it knows) and `[model]` sections, its defaults, whether it searches,
and its machine name, but never a URL, key or `max_*` setting. The
thin engine keeps up to eight offers, a gone client's too, as layers
of its configuration under the user's file, each endpoint at
`relay://NAME` (`relay+anthropic://NAME` for kind anthropic). A model
call to one of them goes back as `relay` records, part by part as the
host makes room, to a client that offers that endpoint with that kind
and model: the session's last, else the newest. The client's own
engine makes it (`relay.call`), admitted in line with its own
sessions, at the API's paths alone, and answers with `relay.data`. A
turn's body goes as a delta against the session's last one to the
same client, deflated when that is smaller. With no client to relay,
the session waits in state `relay` for at most the endpoint's
`slow_retry_budget`; a lost client's call is sent again if nothing
showed yet, and otherwise the turn goes on from what showed. A
hostlink offers its parent's models only for the subagents it
started, by a token sent in `new {task}`. With `[web] search` unset, a
thin engine's `web_search` goes the same way, as a call to the path
`search`, which the client's engine runs with its own `[web]`. A thin
engine offers nothing, so relays never chain, and `prospero connect`
does not relay.

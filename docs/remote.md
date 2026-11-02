# Remote and background hosts

A host quits as when a client asks it to, closing its endpoint and
removing its registration, on SIGTERM, SIGINT or SIGHUP, or on Windows
Ctrl+C or Ctrl+Break at its console.

A `[remote NAME]` section names a machine reached over ssh, and the
window's Settings adds one (Add remote..., also in the Hosts panel),
and its Hosts panel connects to it. Prospero there runs
as `prospero attach`, started over `ssh -T`, which starts a host that
outlives the ssh session. A link that breaks is taken up again from
where it was, and the host keeps a client's place for three hours. A
window closed with "Keep running in background" makes its own process
such a host, and keeps the state directory, so a later launch brings
that window back. On Windows, a WSL distro is a remote too, reached
through `wsl.exe -d DISTRO --cd ~ --exec /bin/sh -c ...` instead of
ssh: every installed distro is listed (`wsl.exe -l -q`) as a remote of
its own name, Docker Desktop's and Podman's left out, and listed
again as the registry key that WSL lists them in changes. A section
with `wsl = DISTRO` in place of `ssh` names one. A distro that is not
running starts when connected to, silent meanwhile: the link is kept
for up to two minutes while `wsl.exe -l --running -q` does not list
it, polled every two seconds, and its host must answer within the
usual 20 s of silence once it is listed. Deploying into a distro sends the
engine on `cat`'s input, as it has no scp, and records it as
`wsl:DISTRO`. A section with `exec` in place of `ssh` reaches the
machine by a command that runs another inside it, `docker exec -i NAME`
say, split into words as an MCP server's command is (no shell), then
`/bin/sh -c 'cd; ...'`; it deploys over `cat` as into WSL, records it
as `exec:COMMAND`, and fails for good on a container runtime's "No such
container", "is not running" or "executable file not found". A window
on a state directory that `prospero serve` hosts lists the sessions the
host holds and attaches to them.

The window also makes containers for sessions (src/container.cpp),
with Docker or Podman, found on PATH or where their macOS installers
put them, and asked `version` at start: no configuration is needed.
Each is `prospero-NAME` (NAME `p-` and eight hex digits), labelled
`com.nullprogram.prospero=1` and `...owner=` a hash of the state
directory, so a test's state never sees another's. It is created with
`--init --entrypoint sleep IMAGE 2147483647`, `--network none` unless
asked, `/work` as its directory (a host folder bound there, else a
volume of its own, as Podman won't start in a missing working
directory, a folder copied into it with `cp` before it starts), and its
state directory here,
`STATE/containers/NAME/state`, bound at `/prospero/state` with a tmpfs
over its `run` so that the host's socket never lands on a shared
mount. `container.ini` beside it keeps the image, workspace, network
and the sessions last listed, for the Trash while it is stopped. The
window reaches it as the exec remote `ctr:NAME`, `RUNTIME exec -i
prospero-NAME`, deployed as any exec remote is, its model calls
relayed, and starts it first when it is stopped. Its Trash is the
window's: the last session there stops it (`stop -t 5`), restoring one
starts it, and Delete, emptying the Trash or the purge after the main
host's `trash_days` runs `rm -f -v` and removes its directory. Making,
starting and stopping run on threads of their own, their output in
`STATE/containers/run.err` when one fails.

An agent's subagent may run in a container of its own too: the task
tool's `image` (with `copy`, a folder copied into `/work`, and
`network`), offered while the engine's runtime answers and a Linux
engine is here (src/hostlink.cpp). The runtime is found when the first
session that offers task opens, on a thread, and cached in
`STATE/containers/runtime` for the next start. An image not in
`[container] images`, or network access, asks the user first, with an
"always" for that image in the session's tree. Each container is
`prospero-t-XXXXXXXX`, labelled `...task=1` too and not among the
window's own containers, with `-v OUT:/out`, OUT a folder in the
parent's scratch directory, which the report names when the subagent
left files there. The engine makes it on a thread while the link waits
in connecting, reaches it as an exec link, deploying its engine over
`cat`, and removes it (`rm -f -v`) once its subagent ends and the link
closes, or at once when the call is cancelled first. Quitting waits
for that. With no network, its subagent starts in Bypass mode (`new`'s
`task.bypass`). The parent's `task.host` record names the container
(`ctr`), and a window whose parent runs in its engine reaches it as
the host `ctr:t-XXXXXXXX`, by exec from its `container.ini`, while it
is there: listed under the runtime, offered for no new session, let go
once its link goes down. Before the container's directory is removed,
its sessions are copied to `STATE/sessions`, their first record without
`remote`, so that they list as subagents of the parent here, and a
`relist` record tells clients to list them. An engine's `t-`
containers left by a crash are removed when the runtime is found next
(their sessions kept too), as is any container of its owner named
`prospero-t-`. `[container] subagents = no` turns it off.

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

The agent's task tool may start a subagent on a remote (one reached by
`exec` included, as `via = exec`), or on Windows in a WSL distro, by its name (`host`), with `cwd` there. The parent's
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
it knows) and `[model]` sections (each flattened through `like`, with
the id it sends), its defaults, whether it searches,
and its machine name, but never a URL, key or `max_*` setting. The
thin engine keeps up to eight offers, a gone client's too, as layers
of its configuration under the user's file, each endpoint at
`relay://NAME` (`relay+anthropic://NAME` for kind anthropic). A model
call to one of them goes back as `relay` records, part by part as the
host makes room, to a client that offers that endpoint with that kind:
the session's last, else the newest whose listing has the model, else
the newest (a server may take names it does not list). The client's own
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

# Troubleshooting

## Where to look

- **The menu bar** shows the latest error at its right end, in red
  when the engine itself has a problem.
- **The Log pane** (View → Log) shows the window's recent messages.
  Select text there to copy it, or Copy all for all of them.
- **`gui.log`** in the state directory (`~/.local/state/prospero`, see
  [Configuration](help:config)) keeps the same messages across runs.
  A window launched from a terminal, which goes on detached, writes its
  errors there too; `prospero -f` keeps it in the terminal instead.
- **Settings → Diagnostics** lists each problem in the configuration
  files, with where it is, and copies them with Copy all.
- **The transcript** notes each retry of a failed request, and the
  error a turn ended with.
- **Session → Open session records** opens a session's folder in the
  state directory, with its log.

## Common problems

**"no model: give one, or set model in the config".** No model is
configured, no `[model]` section either, the New session dialog named
none, and the endpoint has listed none. Open the dialog's Model list:
an endpoint that cannot be reached says why there. Check its `url`,
or set `model` on the Settings pane's Defaults page. See
[Getting started](help:start).

**"no usable endpoint: give a URL or a configured name".** No
`[endpoint]` section has a `url`, or the one named does not exist.
Add one under Endpoints in the Settings pane.

**The endpoint does not answer.** The harness retries: ten quick
retries, then the model's `fallback` models if any, then slow retries
minutes apart (`[endpoint] slow_retry`) for up to two hours
(`slow_retry_budget`), before the turn ends with an error. The status
line in the Prompt pane says when the next retry is, and Stop ends the
turn.
Check the endpoint's `url` in the settings, and that the server is
running. A server whose certificate is not trusted fails at once,
saying why.

**"paused: no relay".** The session is on a remote with no endpoint of
its own, and no computer connected to it offers the session's
endpoint ([Hosts and remotes](help:hosts)). Connect to the remote from
a window whose configuration has that endpoint, and the turn goes on. It
waits at most the endpoint's `slow_retry_budget`.

**A remote host fails to connect.** The Hosts pane, and the host's
row in the sidebar, show why: the last line ssh printed, also kept in
`remote/NAME.log` in the state directory. ssh runs in batch mode, so
it never asks for a password or to accept a new host key: try `ssh
HOST` in a terminal first, and use a key or an ssh agent. A machine
with no engine needs one deployed: Deploy engine in the Hosts pane,
`prospero deploy NAME`, or `deploy = auto`. See
[Hosts and remotes](help:hosts).

**A program is not found when started from the Finder, Dock or Start
menu.** Prospero started that way has a shorter PATH than a terminal
gives it, and no `$VISUAL` or `$EDITOR`. On macOS it asks your login
shell for its environment, so check that your profile sets PATH, runs
without a terminal, and takes under five seconds. Add directories with
`[tools] path_prepend` or `path_append`, and set `[ui] editor`, with a
full path if need be. See [Configuration](help:config).

**"The file system is not responding" or "The file system is busy:
try again".** A file, folder or completion on a share or mount took
over ten seconds without an answer, so Prospero gave up on it: on
another host its engine, and for a file on this computer shown in
Files, or checked by Open file, the window's own file readers, which
keep the window drawing meanwhile. Paths under it are refused at once
until it answers again. When it is on a network share or mount, the
whole share is refused that way, so other files there fail at once
instead of each waiting. Busy means the file readers were all
occupied, usually waiting on such a path, so the request was not
started in time. Check the share or network drive, then try again
(Retry, in Files). A reader stuck that way can also make that engine
slow to exit.

**An ask was denied before you answered.** Asks are denied when their
time runs out (`[guard] ask_timeout`, 60 s; 0 for no limit), and at
once in a session nobody is watching. See [Permissions and asks](help:permissions).

**Launching Prospero again only brings the old window forward.** One
window runs per state directory, even while it runs in the
background. Use `--statedir DIR` for a separate instance.

**The panes are in a mess.** View → Layout → Standard puts them back,
or pick a layout you saved.

# Troubleshooting

## Where to look

- **The menu bar** shows the latest error at its right end, in red
  when the engine itself has a problem.
- **The Log pane** (View → Log) shows the window's recent messages.
- **`gui.log`** in the state directory (`~/.local/state/prospero`, see
  [Configuration](help:config)) keeps the same messages across runs.
  A window launched from a terminal with `prospero DIR`, which goes on
  detached, writes its errors there too.
- **Settings → Diagnostics** lists each problem in the configuration
  files, with where it is.
- **The transcript** notes each retry of a failed request, and the
  error a turn ended with.
- **The Metrics pane** (View → Metrics) shows the window's own
  workings, frames and wakes, for when the window itself misbehaves.
- **Session → Reveal folder** opens a session's folder in the state
  directory, with its log.

## Common problems

**"no model: give one, or set model in the config".** No model is
configured and the New session dialog named none. Set `model` on the
Settings pane's Defaults page, or type one in the dialog. See
[Getting started](help:start).

**The endpoint does not answer.** The harness retries: ten quick
retries, then the model's `fallback` models if any, then slow retries
minutes apart (`[endpoint] slow_retry`) for up to two hours
(`slow_retry_budget`), before the turn ends with an error. The status
line in the Prompt pane says when the next retry is, and Stop ends the
turn.
Check the endpoint's `url` in the settings, and that the server is
running. A server whose certificate is not trusted fails at once,
saying why.

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
gives it, and no `$VISUAL` or `$EDITOR`. Add directories with
`[tools] path_prepend` or `path_append`, and set `[ui] editor`, with a
full path if need be. See [Configuration](help:config).

**An ask was denied before you answered.** Asks are denied when their
time runs out (`[guard] ask_timeout`), and at once in a session nobody
is watching. See [Permissions and asks](help:permissions).

**Launching Prospero again only brings the old window forward.** One
window runs per state directory, even while it runs in the
background. Use `--statedir DIR` for a separate instance.

**The panes are in a mess.** View → Reset layout puts them back.

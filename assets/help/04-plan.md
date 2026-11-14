# Plan mode

In Plan mode the agent explores and writes a plan before it changes
anything, and you approve it first. Turn it on with the New session
dialog's Plan mode box, with Plan in the mode list on the Prompt's
status line, or with
`/plan` (`/plan off` turns it off). The agent may also ask to plan
first: Plan first agrees, and Go ahead without lets it work as before.
In Bypass mode it cannot: the agent is told to go on instead, so it
neither stops to ask nor leaves Bypass once a plan is approved.

The mode may change at any time, mid-turn too ("Oops, I want to be
planning"). The turn goes on: its next tool call is held to the new
mode, so a write after Plan is picked is refused, and the agent is
told of the change at its next step. Subagents running then follow.

## While planning

- The file tools may write only the session's plan file, `plan.md` in
  the session's folder in the state directory, and scratch files in
  the session's own temporary folder (notes, test inputs), both
  without asking. Your project's files and the system's shared
  temporary folders stay unchanged.
- The agent is told to run only commands that change nothing.
- The desktop tool may only look.
- Subagents plan too.

The mode list shows Plan in amber, and lists Show the plan, which opens
the Plan pane. A subagent's mode is its parent's: its Prompt shows an
amber Plan mode button instead, which opens the Plan pane.

## Approving

When the plan is ready, the agent asks for approval with a card that
shows it:

- **Approve plan** ends Plan mode: the session goes to Build, and the
  agent carries out the plan as the file has it then.
- **Keep planning…** keeps Plan mode on, and sends the agent a note of
  what to change.
- **Edit plan** opens the plan file in your editor, to revise it by hand
  first. Approve plan waits until the editor is closed and the plan is
  read again; the agent is told you revised it when you approve a changed
  plan.

For a plan on this computer, Edit plan uses `[ui] editor`, else `$VISUAL`
or `$EDITOR`, as opening a draft does. For a genuinely remote plan it
instead requires `[remote NAME] editor`, a remote-aware command on this
computer configured separately from `viewer`. Without it, remote Edit plan
is unavailable: there is no fallback to `viewer`, `[ui] editor`, `$VISUAL`,
`$EDITOR`, or the native Terminal-tab editor.

The remote `editor` command itself must wait until editing is finished;
Prospero does not infer or add a wait option. For example,
`editor = ghostty -e nvim scp://{ssh}/{path}` keeps Neovim's terminal open
until you exit it. An editor that talks to a daemon must use its own
documented wait protocol. Prospero gives the command the remote path, not a
local copy, and does no generic writeback.

Approve plan stays held throughout the remote editor process and the
explicit remote reread that follows **every** exit status or **Cancel**.
A nonzero exit gives a warning but is still followed by the read; Cancel
also picks up changes already saved. Only a complete remote result, used
for both the card and Plan pane, releases that hold and permits approval.
Replies belonging to another session, ask or older edit operation are
ignored. A spawn failure is recoverable: fix the command and choose Edit
plan again. A failed or lost read leaves approval held with the reason and
**Retry reread**; after
fixing the cause and reconnecting if needed, use that action rather than
approving stale text.

A plan approval has no countdown: it waits for you. The model may be
switched from the Prompt's status line while it waits.

## Questions from the agent

When a decision is yours, such as an approach, the scope or a
trade-off, the agent may ask, most often while planning. A card shows
up to four questions at once, each with a few options and the one the
agent recommends marked **Recommended** and already picked:

- Click an option to pick it; a question marked with checkboxes takes
  several.
- **Other** takes an answer in your own words: typing in it picks it.
- The note below the questions goes to the agent with your answers.
- **Submit answers** sends them, once every question has one.
- **Skip** answers none: the agent goes with its recommendations, or
  with your note instead, if you wrote one.

A message you send from the prompt box while they wait does not skip
them: it waits below the card, marked "Next step", and the agent reads
it with your answers. To skip the questions in favor of a message,
write it in the note and click **Skip**. A notification tells of the questions while you look elsewhere (`[ui]
notify`). In Plan mode they wait for you with no countdown, as you
mean to review a plan soon. Otherwise they are skipped after `[guard]
question_timeout` (300 seconds, or no limit at 0), with the agent
going with its recommendations, and the countdown starts over whenever
you pick an option or type in the card. **Keep waiting**, at the card's
top right, drops the countdown for those questions. While they wait, the model, the shell and
the mode may change, and Bypass mode leaves them for you. Only the
sessions you start yourself get questions: never subagents, roles, or
runs from the command line, where the agent decides for itself and
says what it assumed.

## The Plan pane

The Plan pane (View → Plan) shows the plan file, which stays after
approval, rendered as the transcript renders Markdown. Select its
text, and right-click to copy it as text or as Markdown.

- **Edit plan** is the waiting approval flow above. On this computer it
  uses the normal local plan editor; on a remote it requires that remote's
  own `editor` command and refreshes the plan before approval.
- **View in editor** opens the file at any time. While a local plan waits,
  this is the card's Edit plan; otherwise the agent is not told. A remote
  plan instead uses its configured `viewer`, which is detached even while
  approval waits: it holds no plan
  approval, tells the agent nothing about edits, and has no Cancel or
  exit-driven refresh. It remains a separate action where supported.
- **Reveal** shows the file in Finder, File Explorer or the file manager.
  It is unchanged and only works for a file on this computer.

For a plan on this computer, the editor is `[ui] editor`, else `$VISUAL`
or `$EDITOR` (see [Configuration](help:config)). A remote uses independent
commands in this window's own configuration, for example:

```ini
[remote test1]
ssh = user@host
viewer = ghostty -e fresh ssh://{ssh}{path}
editor = ghostty -e nvim scp://{ssh}/{path}
```

The `editor` template has exactly the `viewer` template's placeholders,
quoting and direct-argv rules. Both take `{path}`, `{ssh}`, `{wsl}`,
`{exec}`, `{remote}`, `{line}` and `{col}`, run without an implicit shell,
and need a terminal wrapper for a terminal editor; see
[Hosts and remotes](help:hosts#viewing-remote-files-in-a-local-editor).
Included files cannot supply either remote command. An invalid configured
`viewer` reports its error rather than falling back; an unset Files viewer
may use the native Terminal-tab route, but remote Edit plan never does.
Neither command falls back to `[ui] editor`.

A complete remote reread synchronizes nonempty and empty plan text alike;
the usual empty-plan approval safeguard still applies. Final approval
keeps the engine's existing final raw-plan hash behavior, with no hash
fields added to this flow. Existing mounted-container and Windows-WSL path
mappings are unchanged.

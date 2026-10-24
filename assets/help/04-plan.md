# Plan mode

In Plan mode the agent explores and writes a plan before it changes
anything, and you approve it first. Turn it on with the New session
dialog's Plan mode box, with Plan in the mode list below Send, or with
`/plan` (`/plan off` turns it off). The agent may also ask to plan
first: Plan first agrees, and Go ahead without lets it work as before.

## While planning

- The file tools may write only the session's plan file, `plan.md` in
  the session's folder in the state directory, which they do without
  asking.
- The agent is told to run only commands that change nothing.
- The desktop tool may only look.
- Subagents plan too.

The session bar shows an amber Plan mode button, which opens the Plan
pane.

## Approving

When the plan is ready, the agent asks for approval with a card that
shows it:

- **Approve plan** ends Plan mode: the session goes to Build, and the
  agent carries out the plan as the file has it then.
- **Keep planning…** keeps Plan mode on, and sends the agent a note of
  what to change.
- **Edit plan** opens the plan file in your editor, to revise it by
  hand first. Approve plan waits until the editor is closed, and the
  agent is told you revised the plan. This works only for a session on
  this computer.

A plan approval has no countdown: it waits for you. The model may be
switched from the session bar while it waits.

## The Plan pane

The Plan pane (View → Plan) shows the plan file, which stays after
approval, rendered as the transcript renders Markdown. Select its
text, and right-click to copy it as text or as Markdown.

- **View in editor** opens the file in your editor at any time. While
  a plan waits, this is the card's Edit plan; otherwise the agent is
  not told.
- **Reveal** shows the file in Finder, File Explorer or the file
  manager.

Both are only for a plan on this computer. The editor is `[ui] editor`,
else `$VISUAL` or `$EDITOR` (see [Configuration](help:config)).

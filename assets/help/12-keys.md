# Keyboard and mouse

On macOS, Cmd takes the place of Ctrl in every shortcut on this page,
except Ctrl+G in the prompt box, which is Ctrl there too.

## Anywhere

- **Ctrl+N**: New session. Once it's made, the prompt box has the
  keyboard.
- **Ctrl+Shift+N**: New session, no folder, with the default model:
  no dialog, straight to its prompt box.
- **Ctrl+F**: find in the transcript. In Help or Settings, while it has
  the focus, its search field instead.
- **Ctrl+Shift+T**: open, or fold again, all of the session's tool
  output (the Tool output button above the transcript).
- **Ctrl+Shift+R**: show, or fold again, all of its reasoning (the
  Reasoning button).
- **Ctrl+=** and **Ctrl+-** zoom the text in and out, and **Ctrl+0**
  resets it; so does the mouse wheel with Ctrl held. The zoom lasts
  until Prospero exits: `[ui] font_size` sets the size for good.
- **Ctrl+Q** quits, on Linux. macOS has Cmd+Q, and Windows Alt+F4.
- **Ctrl+Tab**, held, switches between panes.

## The prompt box

- **Enter** sends; while the agent works, it steers.
- **Shift+Enter** starts a new line.
- **Esc** cancels the running turn (from the Sessions, Transcript and
  Prompt panes, while no dialog or menu is open). In a field of the
  agent's questions, Esc first leaves the field, keeping what you
  typed, and Enter submits the answers once each question has one.
- **Ctrl+B** moves what the turn waits for to the background, a
  running command or its subagents, as Run in background does (from
  the same panes); the agent goes on, and you may talk to it. See
  [Jobs, todo and subagents](help:jobs).
- **Up**, on an empty box, recalls the session's earlier prompts,
  newest first; **Up** and **Down** go back and forth, and **Esc**
  returns to the empty draft.
- **Tab** completes a `/` command or skill name, and Up and Down pick
  one from the list that shows as you type it.
- **Ctrl+G** opens the draft in your editor (`[ui] editor`); the draft
  becomes what you save, once the editor exits.
- **Ctrl+V** pastes text, or attaches a copied image.
- Right-click for Undo, Redo, Cut, Copy, Paste, Select all and Open in
  editor.

## The transcript and documents

The transcript, the Plan pane and these Help pages select alike:

- drag to select, past the top or bottom edge to scroll;
- double-click selects a word, triple-click a block, Shift-click
  extends the selection;
- **Ctrl+A** selects all, **Ctrl+C** copies the text, and
  **Ctrl+Shift+C** copies it as Markdown;
- right-click for Copy, Copy as Markdown and Select All, and in the
  transcript Copy Message, the forks and Start a side chat (which quotes
  the selection in the side chat's box).

A code block's Copy button, at its corner while the mouse is over it,
copies its code exactly. A click on a tool row's or reasoning's header
opens or folds it.

## Finding

In the find bar, **Enter** or **Ctrl+G** goes to the next match, and
**Shift+Enter** or **Ctrl+Shift+G** to the previous. Folded tool output
and reasoning are searched too, and a folded row opens to show a
match. **Esc** closes the bar.

## Dialogs

**Enter** takes a dialog's default button, such as Create, and **Esc**
cancels it.

## Dropping files

- A folder dropped on the window opens the New session dialog with it.
- An image file dropped on the window is attached to the draft (PNG,
  JPEG, GIF, WebP or BMP, at most ten to a message). The agent can also
  look at image files itself: `read` shows them to a model that takes
  images.

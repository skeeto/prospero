# Keyboard and mouse

On macOS, Cmd takes the place of Ctrl in every shortcut on this page,
except Ctrl+G in the prompt box and in the Files pane, which is Ctrl
there too.

## Anywhere

- **Ctrl+N**: New session. Once it's made, the prompt box has the
  keyboard.
- **Ctrl+K**: Go to, the palette: find a session by words from its
  title, group, folder or host and show it, or a layout, pane,
  setting or Help section. With nothing typed it
  lists the sessions viewed before, so Ctrl+K then Enter goes back to
  the previous one. Up, Down, Page Up and Page Down move the pick, and
  Esc or Ctrl+K again closes it.
- **Ctrl+O**: Open file: a path on the session's host, shown in the Files
  pane. Tab completes it, and `:N` or `:A-B` after it goes to that line
  or those lines, marked.
- **Ctrl+G**, while the Files pane has the focus: go to a line of its
  file. Type `N`, `N:C` for a column too, or `A-B` for lines A to B,
  and Enter goes there and bands them as a link's lines are; past the
  end it goes to the last line, saying how many there are (or, for a
  file shown only in part, that only its start is shown). In its find
  bar, Ctrl+G is the next match instead (on macOS Cmd+G is, and Ctrl+G
  still goes to a line). Edit → Go to line... does the same.
- **Ctrl+W**, while the Files pane has the focus: close its front tab.
  Elsewhere on macOS, Cmd+W closes the window, as before.
- **Ctrl+Shift+N**: New session, no folder, with the default model:
  no dialog, straight to its prompt box.
- **Ctrl+F**: find in the transcript. In Help or Settings, while it has
  the focus, its search field instead, and in the Files pane its own
  find bar, over the file shown.
- **Ctrl+,**: the Settings pane, its search field taking the keyboard.
  On macOS, the Prospero menu's Settings... does the same.
- **Alt+Up**: from a subagent or side chat, up to the session it
  came from (the up arrow above its transcript). On macOS, Option+Up,
  or Cmd+Up as in Finder.
- **Ctrl+Shift+S**: fold, or show again, all of the session's shell
  output (the Shell output button above the transcript, the Bash
  logo), short output too, leaving its other tool rows as they were.
- **Ctrl+Shift+T**: open, or fold again, all of the session's tool
  output (the Tool output button above the transcript, a wrench and
  screwdriver).
- **Ctrl+Shift+R**: show, or fold again, all of its reasoning (the
  Reasoning button, a light bulb). Each of these three buttons is
  filled while it is on, and its tooltip says whether it is.
- **Ctrl+1** to **Ctrl+9**: a layout, as View → Layout lists them:
  Standard, Compact, Focus, then those you saved.
- **Ctrl+=** and **Ctrl+-** zoom the text in and out, and **Ctrl+0**
  resets it; so does the mouse wheel with Ctrl held. The zoom lasts
  until Prospero exits: `[ui] font_size` sets the size for good.
- **Ctrl+Q** quits, on Linux. macOS has Cmd+Q, and Windows Alt+F4.
- **Ctrl+Tab**, held, switches between panes.

## The prompt box

- **Enter** sends; while the agent works, it steers.
- **Shift+Enter** starts a new line.
- **Esc** closes what is open first: a dialog or menu, the `/` or `@`
  list, the find bar, keyboard navigation's highlight. Then it leaves
  the prompt box, keeping what you typed; while the agent works, the
  transcript says so: "Esc again stops the turn". With nothing left to
  close, in the Transcript or Prompt pane, Esc cancels the running
  turn. So from the box, it takes two. In a field of the agent's
  questions, Esc first leaves the field, keeping what you typed, and
  Enter submits the answers once each question has one.
- **Ctrl+B** moves what the turn waits for to the background, a
  running command or its subagents, as Run in background does (from
  the same panes); the agent goes on, and you may talk to it. See
  [Jobs, todo and subagents](help:jobs).
- **Up**, on an empty box, recalls the session's earlier prompts,
  newest first; **Up** and **Down** go back and forth, and **Esc**
  returns to the empty draft.
- **Tab** completes a `/` command or skill name, and Up and Down pick
  one from the list that shows as you type it.
- **@** at the start of a word, after a space or `(`, opens a list of
  the session's files and folders, and once a word is typed, other
  sessions, ranked as you type. **Tab** or a
  plain **Enter** puts the pick in the message without sending it,
  **Up** and **Down** move the pick, and **Esc** closes the list before
  it reaches the turn.
- **Ctrl+G** opens the draft in your editor (`[ui] editor`); the draft
  becomes what you save, once the editor exits.
- **Ctrl+V** pastes text, or attaches a copied image. **Ctrl+Shift+V**
  does the same.
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
copies its code exactly, and shows a check mark for a moment. A click on a tool row's or reasoning's header
opens or folds it. So does a click on the bar at its left (a tool
row's colored bar, reasoning's gray one), from anywhere along it,
which, folding, scrolls back to its header if that was above the view.
A click on a tool row's file path, though, opens that file in the
Files pane.

## Finding

In the find bar, **Enter**, **Ctrl+G** or **F3** goes to the next match,
and **Shift+Enter**, **Ctrl+Shift+G** or **Shift+F3** to the previous.
Only what shows is searched, not folded tool output or reasoning:
Ctrl+Shift+T or Ctrl+Shift+R opens them all to search them too. The
search is in the text as shown, not its Markdown, ignoring case and how
spaces and line breaks fall, so text copied from the transcript finds
itself. **Esc** closes the bar.

The Files pane has a find bar of its own: **Ctrl+F** while the pane has
the focus, or Edit → Find in file.... It searches the file's text, or a
Markdown file as rendered, ignoring case and how spaces, tabs and line
breaks fall, so code copied from it finds itself; a tab pasted into the
field is dropped, so type a space for it. It steps with the same keys,
and F3 and Shift+F3 work from the text too, as on macOS do Cmd+G and
Cmd+Shift+G. Matches are marked, the current one stronger, and the text
scrolls only to bring one into view. **Esc** closes it, in the bar or
the text. It stays with the pane, searching each file shown from the
top of the view.

## Dialogs

**Enter** takes a dialog's default button, such as Create, and **Esc**
cancels it.

## Dropping files

- While files are dragged over the window, the place they would go is
  outlined, and a card says what a drop there does: Open in Files,
  Attach, Fill in the path, or New session; or, in amber, why they
  would not be attached. On macOS the card knows what a drag holds: it
  counts the files and folders, a folder goes to New session, and
  text is refused ("Only files can be dropped here"); where nothing in
  the drag would be taken, the drop is refused and the drag springs
  back. On Linux and Windows the card knows only where the
  drag is, not what it holds: it counts no files, and a folder dragged
  shows where files would go, until the drop says otherwise. So does
  text on Linux; Windows refuses dragged text at once, with no outline
  or card.
- A folder dropped on the window, anywhere, opens the New session
  dialog with it.
- While Open file asks for a path, a file dropped anywhere on the
  window, its title bar included, fills the path in, to add a `:N` to.
  It takes one: others dropped with it are not attached. While another
  dialog is up, a dropped file is attached.
- Otherwise a file dropped on the Files pane opens there, in a tab of
  the session shown, and is not attached: an image (PNG, JPEG, GIF,
  WebP or BMP, by its name's extension) as the image, anything else as
  text.
- A file dropped anywhere else on the window is attached to the
  draft, at most ten to a message. Files dropped together are attached
  together or not at all: when the message has no room for them all,
  none are, and the note says why: how many more fit, or that ten is
  the most. Text dropped on the
  window is not attached: only files can be dropped there. An image (PNG, JPEG, GIF, WebP or
  BMP) goes as a picture, to a model that takes images; any other file
  goes as text, as `read` shows it, and a binary file is refused. The agent can also
  look at image files itself: `read` shows them to a model that takes
  images.

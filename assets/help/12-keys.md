# Keyboard and mouse

On macOS, <kbd>Cmd</kbd> takes the place of <kbd>Ctrl</kbd> in every shortcut on this page,
except <kbd>Ctrl+G</kbd> in the prompt box and in the Files pane, which is <kbd>Ctrl</kbd>
there too.

## Anywhere

- <kbd>Ctrl+N</kbd>: New session. Once it's made, the prompt box has the
  keyboard.
- <kbd>Ctrl+K</kbd>: Go to, the palette: find a session by words from its
  title, group, folder or host and show it, or a layout, pane,
  setting or Help section. With nothing typed it
  lists the sessions viewed before, so <kbd>Ctrl+K</kbd> then <kbd>Enter</kbd> goes back to
  the previous one. <kbd>Up</kbd>, <kbd>Down</kbd>, <kbd>Page Up</kbd> and <kbd>Page Down</kbd> move the pick, and
  <kbd>Esc</kbd> or <kbd>Ctrl+K</kbd> again closes it.
- <kbd>Ctrl+O</kbd>: Open file: a path on the session's host, shown in the Files
  pane. <kbd>Tab</kbd> completes it, and `:N` or `:A-B` after it goes to that line
  or those lines, marked.
- <kbd>Ctrl+G</kbd>, while the Files pane has the focus: go to a line of its
  file. Type `N`, `N:C` for a column too, or `A-B` for lines A to B,
  and <kbd>Enter</kbd> goes there and bands them as a link's lines are; past the
  end it goes to the last line, saying how many there are (or, for a
  file shown only in part, that only its start is shown). In its find
  bar, <kbd>Ctrl+G</kbd> is the next match instead (on macOS <kbd>Cmd+G</kbd> is, and <kbd>Ctrl+G</kbd>
  still goes to a line). Edit → Go to line... does the same.
- <kbd>Ctrl+W</kbd>, while the Files pane has the focus: close its front tab.
  Elsewhere on macOS, <kbd>Cmd+W</kbd> closes the window, as before.
- <kbd>Ctrl+Shift+N</kbd>: New session, no folder, with the default model:
  no dialog, straight to its prompt box.
- <kbd>Ctrl+F</kbd>: find in the transcript. In Help or Settings, while it has
  the focus, its search field instead, and in the Files pane its own
  find bar, over the file shown.
- <kbd>F1</kbd>: Help, these pages; on macOS, <kbd>Cmd+?</kbd>. While
  the Terminal has the keyboard, F1 is its program's.
- <kbd>Ctrl+,</kbd>: the Settings pane, its search field taking the keyboard.
  On macOS, the Prospero menu's Settings... does the same.
- <kbd>Alt+Up</kbd>: from a subagent or side chat, up to the session it
  came from (the up arrow above its transcript). On macOS, <kbd>Option+Up</kbd>,
  or <kbd>Cmd+Up</kbd> as in Finder.
- <kbd>Alt+Left</kbd> and <kbd>Alt+Right</kbd>: Back and Forward, as in a browser,
  through the window's jumps: following a link, Open in Files, a
  session picked in the sidebar or the palette, up to a parent, a pane
  brought forward by a menu or a key, a Help topic or Settings page
  picked from its list. Back returns to the session, pane and place a
  jump left: the transcript where it was (or following its end, if it
  was), the file at its line. Picking a tab, scrolling, finding and
  layouts are not jumps. The mouse's side buttons and a keyboard's
  browser keys do the same, as do View → Back and View → Forward. On
  macOS, <kbd>Cmd+[</kbd> and <kbd>Cmd+]</kbd>. While the Terminal has the keyboard, <kbd>Alt+Left</kbd>
  and <kbd>Alt+Right</kbd> are its shell's. The history is the window's, across
  sessions, and goes when Prospero quits.
- <kbd>Ctrl+Shift+S</kbd>: fold, or show again, all of the session's shell
  output (the Shell output button above the transcript, the Bash
  logo), short output too, leaving its other tool rows as they were.
- <kbd>Ctrl+Shift+T</kbd>: open, or fold again, all of the session's tool
  output (the Tool output button above the transcript, a wrench and
  screwdriver).
- <kbd>Ctrl+Shift+R</kbd>: show, or fold again, all of its reasoning (the
  Reasoning button, a light bulb). Each of these three buttons is
  filled while it is on, and its tooltip says whether it is.
- <kbd>Ctrl+Shift+L</kbd>, while a Terminal tab has the keyboard: Ask
  about this, which attaches the tab's last command, or the text
  selected in it, to the session's prompt
  ([The agent and your terminals](help:sessions#the-agent-and-your-terminals)).
- <kbd>Ctrl</kbd>+click on a path, `path:line`, web address or link in a
  Terminal tab: open it, a file in the Files pane
  ([Opening files from a tab](help:sessions#opening-files-from-a-tab)).
- <kbd>Ctrl+1</kbd> to <kbd>Ctrl+9</kbd>: a layout, as View → Layout lists them:
  Standard, Compact, Focus, then those you saved.
- <kbd>Ctrl+=</kbd> and <kbd>Ctrl+-</kbd> zoom the text in and out, and <kbd>Ctrl+0</kbd>
  resets it; so does the mouse wheel with <kbd>Ctrl</kbd> held. The zoom lasts
  until Prospero exits: `[ui] font_size` sets the size for good.
- <kbd>Ctrl+Q</kbd> quits, on Linux. macOS has <kbd>Cmd+Q</kbd>, and Windows <kbd>Alt+F4</kbd>.
- <kbd>Ctrl+Tab</kbd>, held, switches between panes.

## The prompt box

- <kbd>Enter</kbd> sends; while the agent works, it steers.
- <kbd>Shift+Enter</kbd> starts a new line.
- <kbd>Esc</kbd> closes what is open first: a dialog or menu, the `/` or `@`
  list, the find bar, keyboard navigation's highlight. Then it leaves
  the prompt box, keeping what you typed; while the agent works, the
  transcript says so: "Esc again stops the turn". With nothing left to
  close, in the Transcript or Prompt pane, <kbd>Esc</kbd> cancels the running
  turn. So from the box, it takes two. In a field of the agent's
  questions, <kbd>Esc</kbd> first leaves the field, keeping what you typed, and
  <kbd>Enter</kbd> submits the answers once each question has one.
- <kbd>Ctrl+B</kbd> moves what the turn waits for to the background, a
  running command or its subagents, as Run in background does (from
  the same panes); the agent goes on, and you may talk to it. See
  [Jobs, todo and subagents](help:jobs).
- <kbd>Up</kbd>, on an empty box, recalls the session's earlier prompts,
  newest first; <kbd>Up</kbd> and <kbd>Down</kbd> go back and forth, and <kbd>Esc</kbd>
  returns to the empty draft.
- <kbd>Tab</kbd> completes a `/` command or skill name, and <kbd>Up</kbd> and <kbd>Down</kbd> pick
  one from the list that shows as you type it.
- <kbd>@</kbd> at the start of a word, after a space or `(`, opens a list of
  the session's files and folders, and once a word is typed, other
  sessions, ranked as you type. <kbd>Tab</kbd> or a
  plain <kbd>Enter</kbd> puts the pick in the message without sending it,
  <kbd>Up</kbd> and <kbd>Down</kbd> move the pick, and <kbd>Esc</kbd> closes the list before
  it reaches the turn. The files of a folder on this computer are found
  in the background, the folder's own entries listed meanwhile. A
  search held up by a share that stops answering is given up, and the
  next one, a minute later, leaves that folder out.
- <kbd>Ctrl+G</kbd> opens the draft in your editor (`[ui] editor`), which
  must open a window of its own; the draft becomes what you save, once
  the editor exits. A terminal `[ui] editor`, such as `vim`, is
  refused here, though the Files pane's Open in editor runs one in a
  Terminal tab; on macOS one named only by `$VISUAL` or `$EDITOR` gives
  way to the default text editor.
- <kbd>Ctrl+V</kbd> pastes text, or attaches a copied image. <kbd>Ctrl+Shift+V</kbd>
  does the same.
- Right-click for Undo, Redo, Cut, Copy, Paste, Select all and Open in
  editor.

## The transcript and documents

The transcript, the Plan pane and these Help pages select alike:

- drag to select, past the top or bottom edge to scroll;
- double-click selects a word, triple-click a block, Shift-click
  extends the selection;
- <kbd>Ctrl+A</kbd> selects all, <kbd>Ctrl+C</kbd> copies the text, and
  <kbd>Ctrl+Shift+C</kbd> copies it as Markdown;
- right-click for Copy, Copy as Markdown and Select All, and in the
  transcript Copy Message, the forks and Start a side chat (which quotes
  the selection in the side chat's box).

A code block's Copy button, at its corner while the mouse is over it,
copies its code exactly, and shows a check mark for a moment. Beside
it, a shell's block has a Run button
([The agent and your terminals](help:sessions#the-agent-and-your-terminals)),
as does Run in Terminal on the right-click menu. A click on a tool row's or reasoning's header
opens or folds it. So does a click on the bar at its left (a tool
row's colored bar, reasoning's gray one), from anywhere along it,
which, folding, scrolls back to its header if that was above the view.
A click on a tool row's file path, though, opens that file in the
Files pane.

## Finding

In the find bar, <kbd>Enter</kbd>, <kbd>Ctrl+G</kbd> or <kbd>F3</kbd> goes to the next match,
and <kbd>Shift+Enter</kbd>, <kbd>Ctrl+Shift+G</kbd> or <kbd>Shift+F3</kbd> to the previous.
Only what shows is searched, not folded tool output or reasoning:
<kbd>Ctrl+Shift+T</kbd> or <kbd>Ctrl+Shift+R</kbd> opens them all to search them too. The
search is in the text as shown, not its Markdown, ignoring case and how
spaces and line breaks fall, so text copied from the transcript finds
itself. <kbd>Esc</kbd> closes the bar.

The Files pane has a find bar of its own: <kbd>Ctrl+F</kbd> while the pane has
the focus, or Edit → Find in file.... It searches the file's text, or a
Markdown file as rendered, ignoring case and how spaces, tabs and line
breaks fall, so code copied from it finds itself; a tab pasted into the
field is dropped, so type a space for it. It steps with the same keys,
and <kbd>F3</kbd> and <kbd>Shift+F3</kbd> work from the text too, as on macOS do <kbd>Cmd+G</kbd> and
<kbd>Cmd+Shift+G</kbd>. Matches are marked, the current one stronger, and the text
scrolls only to bring one into view. <kbd>Esc</kbd> closes it, in the bar or
the text. It stays with the pane, searching each file shown from the
top of the view.

## Dialogs

<kbd>Enter</kbd> takes a dialog's default button, such as Create, and <kbd>Esc</kbd>
cancels it.

## Dropping files

- While files are dragged over the window, the place they would go is
  outlined, and a card says what a drop there does: Open in Files,
  Attach, Fill in the path, or New session; or, in amber, why they
  would not be attached. On macOS and Windows the card knows what a
  drag holds: it counts the files and folders, a folder goes to New
  session, and text, or anything else not files, is refused ("Only
  files can be dropped here"); where nothing in the drag would be
  taken, the drop is refused and the drag springs back. On Windows a
  folder is told from a file only on this computer's own disks: a drag
  with anything on a network drive, a UNC path, or a CD or DVD, which
  is never asked while dragged, shows only the place, as on Linux, and
  is never refused, so a folder there still starts its New session
  when dropped. On Linux the card shows only the place, not
  what the drag holds: it counts no files, and a folder or text dragged
  shows where files would go, until the drop says otherwise.
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

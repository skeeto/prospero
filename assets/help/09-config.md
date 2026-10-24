# Configuration

Prospero reads one INI file:

- `$XDG_CONFIG_HOME/prospero/prospero.ini`, which defaults to
  `~/.config/prospero/prospero.ini`;
- on Windows, home is `%HOME%`, else `%USERPROFILE%`, and
  `%APPDATA%\prospero\prospero.ini` is used only while there is none
  under home.

`-c FILE` names another. On the first run, Prospero writes the file
from its built-in copy, whose comments document every section and
key: read it there for the full list. Changes take effect while
Prospero runs.

The Settings pane (View → Settings) edits the file in place, a page
for each section, changing only the lines it sets and keeping your
comments. Its top line says which file it edits. Diagnostics, at the
end of its list, shows each problem found in the files, with where it
is; a bad value falls back to its default.

The state directory holds the sessions, the window's layout and
`gui.log`: `$XDG_STATE_HOME/prospero`, else
`~/.local/state/prospero`. On Windows `%LOCALAPPDATA%\prospero` is
used while there is none under home. `--statedir DIR` names another,
for a separate instance.

## Includes

Above the first section, `include = FILE` adds a shared file, such as
a team's endpoints and models, whose settings yours may override
below it. An include may be an http or https URL, fetched at startup,
kept, refreshed in the background, and optionally pinned to a
SHA-256.

The Settings pane never edits an included file. An included file
cannot choose what runs, so these are ignored in one:

- plaintext secrets (a `key` not written as `env:NAME`);
- `[remote]` and `[desktop]` settings;
- `[tools] path_prepend` and `path_append`;
- a `[tools] shell` other than `auto`, `sh`, `powershell` or `cmd`;
- `[ui] editor`.

## Settings worth knowing

- **API keys** never go in the file: `key = env:NAME` reads one from
  the environment variable NAME.
- **`[tools] path_prepend` and `path_append`**: directories, separated
  by `;`, added before and after PATH, for the agent's commands, its
  shell and ripgrep, and for the editor and ssh. Prospero started from
  the Finder, the Dock, the Start menu or a desktop menu has a shorter
  PATH than a shell gives it:

      [tools]
      path_prepend = ~/.local/bin; /opt/homebrew/bin

- **`[tools] shell`**: the agent's shell for new sessions: `auto` (a
  POSIX `sh`; on Windows, `sh.exe` or `busybox.exe` next to Prospero
  or on PATH, else PowerShell, then `cmd`), `sh`, `powershell`, `cmd`,
  or a program by its path or name. A session keeps the shell it began
  with.
- **`[ui] editor`**: the command that opens a draft (Ctrl+G) or a plan
  in an editor, given the file as one more word. It must wait until
  you are done and open a window of its own, as `code --wait`,
  `subl -w` or `gvim -f` do. Empty, it is `$VISUAL`, else `$EDITOR`,
  else on macOS the default text editor, which you then quit. Started
  from the Finder or a desktop menu, Prospero has no `$VISUAL` or
  `$EDITOR`: set this, with a full path if need be.
- **`[ui] font_size`**: the text's size in pixels, 8 to 40 (15 by
  default). The View menu's zoom scales it until Prospero exits.
- **`[ui] theme`**: `system`, `dark` or `light`, also in View → Theme.
- **`[ui] thinking`** and **`[ui] tools`**: whether reasoning and tool
  output show folded or open in the transcript.
- **`[guard]`**: see [Permissions and asks](help:permissions).
- **`[instructions]`** and **`[skills]`**: see
  [Instructions and skills](help:instructions).
- **`[remote NAME]`** and **`[host]`**: see
  [Hosts and remotes](help:hosts).
- **`[desktop]`**: see [Desktop control](help:desktop).

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

The Settings pane ([View → Settings](view:settings)) edits the file in
place, a page for each section, changing only the lines it sets and
keeping your comments. Its top line says which file it edits.
[Diagnostics](settings:diagnostics), at the end of its list, shows each
problem found in the files, with where it is; a bad value falls back to
its default.

The search field above the pages (Ctrl+F, or Cmd+F on macOS, while
Settings has the focus) finds a setting by name: keys that match come
first, then those whose section's name matches, then those whose
description does. Every word typed must match. Pick a result, or press
Enter for the first, to go to that setting on its page. Escape, or
the X at the field's end, empties it.

**Open in editor**, beside Reload, opens the file itself in your
editor (`[ui] editor`, else `$VISUAL` or `$EDITOR`), to edit it by
hand. A missing file is first created: from the built-in copy, as on
the first run, or empty for one named by `-c`. Once the editor exits, Settings reads the file again.
Cancel stops the editor and keeps the file as it left it.

The state directory holds the sessions, the window's layout and
`gui.log`: `$XDG_STATE_HOME/prospero`, else
`~/.local/state/prospero`. On Windows `%LOCALAPPDATA%\prospero` is
used while there is none under home. `--statedir DIR` names another,
for a separate instance.

## The agent changes it

Ask the agent about Prospero, or to change a setting, and it reads
Prospero's own skill ([Skills](help:instructions#skills)): this Help,
and every setting with its type, default and meaning. Its answers link
what they name, a Help topic, a setting or a pane, and a click there
goes to it.

It changes the file with its edit and write tools. Each change asks
you first, with no Always, even where its writes need no asking, but
not in Bypass mode ([Permissions](help:permissions)). Once written,
Prospero reads the configuration again at once: the agent is told of
any problem in what it wrote, and Settings and the window show the
change. A session on a remote host changes that host's file.

## Includes

Above the first section, `include = FILE` adds a shared file, such as
a team's endpoints and models, whose settings yours may override
below it. An include may be an http or https URL, fetched at startup,
kept, refreshed in the background, and optionally pinned to a
SHA-256.

The Settings pane never edits an included file. An included file
cannot choose what runs, so these are ignored in one:

- plaintext secrets (a `key` not written as `env:NAME`), and secrets
  read from files (`file:PATH`);
- `[remote]`, `[shell]`, `[mcp]`, `[desktop]` and `[web]` settings;
- `[tools] path_prepend`, `path_append`, `shells` and `git`;
- a `[tools] shell` other than `auto`, `sh`, `bash`, `powershell` or
  `cmd`;
- `[ui] editor` and `[ui] terminal`.

## Settings worth knowing

- **API keys** never go in the file: `key = env:NAME` reads one from
  the environment variable NAME, and `key = file:PATH` from a file
  holding just the key (PATH absolute or under `~`). A file suits an
  engine whose environment you cannot set, such as one on a remote
  host or in WSL, where the host's own configuration names a file
  there. Keep it private (`chmod 600`).
- **[`[tools] path_prepend`](settings:tools#path_prepend) and
  [`path_append`](settings:tools#path_append)**: directories, separated
  by `;`, added before and after PATH, for the agent's commands, its
  shell and ripgrep, and for the editor and ssh. Prospero started from
  the Finder, the Dock, the Start menu or a desktop menu has a shorter
  PATH than a shell gives it:

      [tools]
      path_prepend = ~/.local/bin; /opt/homebrew/bin

- **[`[tools] shell`](settings:tools#shell)**: the agent's shell for new sessions: `auto` (on
  Linux and macOS `bash` on PATH, which models expect, else `/bin/sh`;
  on macOS a Homebrew or MacPorts bash ahead of the system's own bash
  3.2, whose limits the agent is told;
  on Windows, `sh.exe` or `busybox.exe` next to Prospero or on PATH,
  else PowerShell, then `cmd`), `sh` (`/bin/sh`, even where that is
  bash), `bash` (not a keyword on Windows, where it is a program),
  `powershell`, `cmd`, a `[shell]` section's name, or a program by its
  path or name. A
  session keeps the shell it began with, until you switch it (see
  Sessions and modes).
- **`[shell NAME]`**: a shell you name, for `[tools] shell`, `shells`
  and the shell picker. `program` is a path, a name on PATH, or `sh`,
  `powershell` or `cmd`, and defaults to NAME. `login = true` starts
  it as a login shell (`sh -l`, PowerShell with your profile, cmd with
  its AutoRun). On Windows, MSYS2's and Git's profile then keep the
  working directory and Prospero's PATH, after their own directories
  (`env.MSYS2_PATH_TYPE = minimal` for MSYS2's own default, which
  drops it). `env.NAME` sets a variable for its commands (a value,
  or `env:VAR` from Prospero's own environment). `path_prepend` and
  `path_append` add directories around `[tools]`'s, and `description`
  tells the agent what the shell is for:

      [shell msys2]
      program = C:/msys64/usr/bin/bash.exe
      login = true
      env.MSYSTEM = UCRT64
      description = MSYS2 UCRT64, with pacman to install packages

  On Windows, `program = wsl` runs commands in WSL, in the `sh` of
  the distro that `distro` names (by default the default one), in the
  working directory as WSL sees it (`/mnt/c/...`). Its `path_prepend`
  and `path_append` are Linux directories, `~` for the distro's home,
  and its `env.NAME` reach Linux through `WSLENV`. A command stopped or
  killed ends its Linux processes too. To run a whole session inside a
  distro, its files and tools Linux's, connect to the distro as a host
  instead (Hosts):

      [shell debian]
      program = wsl
      distro = Debian
      path_prepend = ~/.local/bin
      description = Debian, with apt to install packages

- **[`[tools] shells`](settings:tools#shells)**: shells the agent may pick per command, by name,
  separated by blanks. The shell tool then lists them and takes one by
  name. A name not found is left out, with a warning.
- **[`[tools] git`](settings:tools#git)**: the git that the Changes pane runs (see
  [Sessions and modes](help:sessions)), by its path or its name on
  PATH (with `path_prepend` and `path_append`); empty for `git`. On
  macOS, `/usr/bin/git` without the Command Line Tools counts as
  missing, rather than asking to install them. The branch on the
  session bar needs no git. A remote host's engine uses its own
  configuration's.
- **[`[ui] editor`](settings:ui#editor)**: the command that opens a draft (Ctrl+G) or a plan
  in an editor, given the file as one more word. It must wait until
  you are done and open a window of its own, as `code --wait`,
  `subl -w` or `gvim -f` do. Empty, it is `$VISUAL`, else `$EDITOR`,
  else on macOS the default text editor, which you then quit. Started
  from the Finder or a desktop menu, Prospero has no `$VISUAL` or
  `$EDITOR`: set this, with a full path if need be.
- **[`[ui] terminal`](settings:ui#terminal)**: the terminal that Open in terminal opens: on
  macOS an application's name (`iTerm`, `Ghostty`), elsewhere a program
  (`wt.exe`, `konsole`, `kitty`, `wezterm`), whose options for its
  folder and command Prospero knows; another gets `-e`.
- **[`[ui] notify`](settings:ui#notify)**: desktop notifications while the window is in the
  background, minimized, or showing another session: `allow` when a
  permission ask waits for your answer, `done` when a session stops by
  itself (finished, failed, or out of budget, not stopped by you), or
  `none`. Both by default. Clicking one brings the window to that
  session, and one that no longer matters goes away by itself. A window
  kept running in the background still tells of sessions done. Where
  notifications are off or unavailable, the window asks for attention
  instead (a flashing taskbar button, a bouncing Dock icon). On macOS
  only Prospero.app can show them, after you allow it once.
- **[`[ui] font_size`](settings:ui#font_size)**: the text's size in
  pixels, 8 to 40 (17 by default). The View menu's zoom scales it until Prospero exits.
- **[`[ui] theme`](settings:ui#theme)**: `system`, `dark` or `light`,
  also in View → Theme.
- **`[ui] thinking`** and **`[ui] tools`**: whether reasoning and tool
  output show folded or open in the transcript.
- **`[guard]`**: see [Permissions and asks](help:permissions).
- **`[instructions]`** and **`[skills]`**: see
  [Instructions and skills](help:instructions).
- **`[remote NAME]`** and **`[host]`**: see
  [Hosts and remotes](help:hosts).
- **`[desktop]`**: see [Desktop control](help:desktop).
- **`[web]`**: see [Web search](help:web).

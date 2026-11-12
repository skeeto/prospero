# Configuration

Prospero reads one INI file:

- `$XDG_CONFIG_HOME/prospero/prospero.ini`, which defaults to
  `~/.config/prospero/prospero.ini`;
- on Windows, home is `%HOME%`, else `%USERPROFILE%`, and
  `%APPDATA%\prospero\prospero.ini` is used only while there is none
  under home.

`-c FILE` names another. Until a setting is first saved, there is no
file, which is the same as an empty one: nothing is configured. The
first save, from Settings or by the agent, writes the file from its
built-in copy, whose comments document every section and key: read it
there for the full list. Changes take effect while Prospero runs.

The Settings pane ([View → Settings](view:settings), or Ctrl+, and
Cmd+, on macOS) edits the file in place, a page for each section,
changing only the lines it sets and keeping your comments. The model
and shell lists on the Prompt's status line end with Endpoints and models...
and Shells..., which show their pages. Its top line says which file it edits.
[Diagnostics](settings:diagnostics), at the end of its list, shows each
problem found in the files, with where it is; a bad value falls back to
its default. Select text there to copy it, or Copy all for every one,
a line each, to paste into a bug report or an editor.

The search field above the pages (Ctrl+F, or Cmd+F on macOS, while
Settings has the focus) finds a setting by name: keys that match come
first, then those whose section's name matches, then those whose
description does. Every word typed must match. Pick a result, or press
Enter for the first, to go to that setting on its page. Escape, or
the X at the field's end, empties it.

**Open in editor**, beside Reload, opens the file itself in your
editor (`[ui] editor`, else `$VISUAL` or `$EDITOR`), to edit it by
hand. A missing file is first created: from the built-in copy, as by
a first save, or empty for one named by `-c`. Once the editor exits, Settings reads the file again.
Cancel stops the editor and keeps the file as it left it.

The state directory holds the sessions, those in the Trash too, the
window's layout and `gui.log`: `$XDG_STATE_HOME/prospero`, else
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
below it. A relative path is from your file's directory, and `~/` is
your home directory. An include may be an http or https URL, fetched at startup,
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

## Color themes

A `[theme NAME]` section is a color theme of your own, which
`[ui] theme = NAME` or View → Theme picks:

```ini
[ui]
theme = dusk

[theme dusk]
base = dark
panel = #1e2030
text = #c8d3f5
link = #82aaff
```

Colors are `#rgb` or `#rrggbb`. A theme starts from the built-in theme
its `base` names, `dark` (the default) or `light`, so it sets only the
colors it changes. Hover and pressed shades of buttons, fields and
scrollbars follow their color unless set too. The Settings pane lists every color of
a theme with a swatch, the base's color as the hint, and a change there
recolors the window at once. `system`, `dark` and `light` name the
built-in themes and cannot be reused.

A theme is an ordinary section, so it may come from an include. The
[Gruvbox](https://github.com/skeeto/prospero/blob/master/assets/themes/gruvbox.ini)
theme, by Arthur Cnops (accnops) after Pavel Pertsev's palette, lists
every color there is to set:

```ini
include = themes/gruvbox.ini

[ui]
theme = gruvbox
```

The [monocream](https://github.com/skeeto/prospero/blob/master/assets/themes/monocream.ini)
theme, by Stian Gudmundsen Høiland (stianhoiland), lists every color there
is to set as well: a minimal, monochrome-ish, amber, no-blue dark theme
inspired by the plan9's sam color palette--but dark!

## Session titles

After a session's first turn, its model is asked for a title, on the
conversation as it stands: a server with a prompt cache already holds
nearly all of it, so the request costs little more than the title it
writes. It runs only while the session is idle, and a prompt sent
meanwhile goes at once, the title asked for again after that turn.
Its tokens count in the session's usage.

A model of its own, small and fast, may title sessions instead, from
the first prompt and the reply, with `titler` in the model's section,
as `MODEL` or `ENDPOINT/MODEL`; `titler = none` keeps the first prompt
as the title:

```ini
[model claude-opus-5-5]
titler = claude-haiku-5-5

[model local-model]
titler = none
```

## Approving web includes' urls

A file included by URL can change at any time, so a url it gives an
endpoint or MCP server gets nothing of yours until you approve it:
an environment variable a value takes (`env:NAME`, from either file),
a `key`, `token`, `client_secret` or `header.NAME` your own file adds,
or your `trust = yes` for the server. Until then Prospero does not
contact that url at all.

A session about to use such an endpoint or server asks once, listing
each url and what would go there. Allowing it holds for good, for
every session, and answers every other session waiting on the same
thing. Should the include change the url, it asks again. A session
nobody watches (`prospero run`) cannot ask, and says so. The section's
page in Settings also has Approve, and `prospero approve` lists what
waits, `prospero approve URL` approving it, also for an engine on
another host. Approvals are kept by the url's SHA-256 in the state
directory's `approvals.json`. Bypass mode does not approve them.

A url from a local include, or from your own file, needs no approval.

## Signing in with ChatGPT

A ChatGPT subscription can run Prospero's sessions, billed to the plan
rather than by the token. In Settings, at Endpoints, choose Add
endpoint..., name it, and pick **chatgpt: ChatGPT, signed in to your
account**: its URL is filled in. On its page, **Sign in** opens the
browser at ChatGPT's sign-in; once you sign in there, the browser comes
back to Prospero, and the page says who is signed in and on which plan.
Then pick its models, such as `gpt-5-codex`, as any others.

The browser returns to this computer at `localhost:1455` (else 1457),
as for OpenAI's Codex CLI, so sign in on the computer that runs the
engine; `prospero endpoint auth NAME` does the same from a terminal.
The tokens are kept in the state directory, readable only by you, and
refreshed as they expire. **Sign out** forgets them. A remote host that
relays its model calls through this computer uses this sign-in, and
needs none of its own. When the plan's usage limit is reached, the turn
ends saying when it resets.

`kind = responses` is the same API with an OpenAI API key instead,
billed by the token.

## Signing in with GitHub Copilot

A GitHub Copilot plan, Copilot Free included, can run sessions too.
Add an endpoint and pick **copilot: GitHub Copilot, signed in to your
account**. On its page, **Sign in** shows a code and opens GitHub's
page, where you enter it and authorize Prospero; the page then says who
is signed in and on which plan. The code can be entered in a browser on
any computer, and `prospero endpoint auth NAME` prints it in a
terminal, so this works for an engine over ssh as well.

GitHub gives each app its own models, and Prospero's own app gets only
the older ones, such as `gpt-4o`, `gpt-4o-mini` and `gpt-4.1`, not the
newer models your plan offers on github.com. The endpoint's
`client_id` setting signs in as another OAuth app instead, at your own
risk, and `integration_id` names the product the requests are for (the
`Copilot-Integration-Id` header), when Copilot says a model "is not
available for integrator". If the account has no Copilot, the sign-in
fails with a link to sign up for Copilot Free.

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
  shell and ripgrep, and for the editor and ssh. On macOS, Prospero
  started from the Finder, the Dock or Spotlight takes its environment
  from your login shell, as a terminal would have it (a profile can
  tell by `PROSPERO_RESOLVING_ENVIRONMENT=1`; one taking over five
  seconds is stopped). Started from the Start menu or a desktop menu,
  it has a shorter PATH than a shell gives it:

      [tools]
      path_prepend = ~/.local/bin; /opt/homebrew/bin

- **[`[tools] shell`](settings:tools#shell)**: the agent's shell for new sessions: `auto` (on
  Linux and macOS `bash` on PATH, which models expect, else `/bin/sh`;
  on macOS a Homebrew or MacPorts bash ahead of the system's own bash
  3.2, whose limits the agent is told;
  on Windows, `sh.exe` or `busybox.exe` next to Prospero or on PATH,
  else PowerShell, then `cmd`), `sh` (`/bin/sh`, even where that is
  bash), `bash` (not a keyword on Windows, where it is a program),
  `zsh` (likewise; the agent is told it is not bash, though it is
  never auto's choice), `powershell`, `cmd`, a `[shell]` section's name, or a program by its
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
  drops it). w64devkit's sh, which finds `~/.profile` by HOME alone,
  gets HOME as w64devkit's own window sets it, `%USERPROFILE%`, when
  Prospero has none, as when started from the Start menu (a home set
  in `w64devkit.ini` needs `env.HOME`). `env.NAME` sets a variable for
  its commands, and Open in external terminal's (a value, or `env:VAR` from
  Prospero's own environment). `path_prepend` and
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
- **[`[ui] terminal`](settings:ui#terminal)**: the terminal that Open in external terminal opens: on
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
- **[`[ui] mentions`](settings:ui#mentions)**: `false` turns off the
  `@` picker of files, folders and sessions in the compose box, leaving
  `@` as plain text. On by default.
- **[`[ui] font_size`](settings:ui#font_size)**: the text's size in
  pixels, 8 to 40 (17 by default). The View menu's zoom scales it until Prospero exits.
- **[`[ui] theme`](settings:ui#theme)**: `system`, `dark` or `light`,
  or the name of a theme of your own (see [Color themes](help:config#color-themes)),
  also in View → Theme.
- **`[ui] thinking`** and **`[ui] tools`**: whether reasoning and tool
  output show folded or open in the transcript.
- **`[ui] count`**: whether folded reasoning's line counts `words` (the
  default) or `characters` as it streams.
- **`[guard]`**: see [Permissions and asks](help:permissions).
- **`[instructions]`** and **`[skills]`**: see
  [Instructions and skills](help:instructions).
- **`[remote NAME]`** and **`[host]`**: see
  [Hosts and remotes](help:hosts).
- **`[desktop]`**: see [Desktop control](help:desktop).
- **`[web]`**: see [Web search](help:web).

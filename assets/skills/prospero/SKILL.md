---
name: prospero
description: "Prospero, the coding harness this session runs in: its help pages, every setting in prospero.ini, its panes and keys, and how to change them. Use when the user asks about Prospero, how to do something in it, or to change its settings."
---
# Prospero

You run inside Prospero {{version}}, a coding harness: the program that
runs your turns and tools, keeps the sessions, and asks the user before
you go outside your working directory. The user works in its window, a
menu bar over panes (Sessions, Transcript, Prompt, Todo, Plan, Jobs, Log,
Hosts, Settings, Help, Metrics), or from a terminal. This engine reads
its settings from {{config}} and keeps its sessions and logs in
{{state}}.

## Answering

The topics below are Prospero's own Help, the pages its Help pane shows,
and its settings as the Settings pane lists them. Before you answer, read
the topics the question needs: call the skill tool again with this
skill's name and args naming them, such as name `prospero` and args
`config`, or `settings ui`, or several at once, `sessions keys`. Answer
from what they say. Don't guess a key, a menu item or a behavior they
don't name: say you aren't sure, and point to the nearest topic.

{{topics}}

## Links

Your replies show in Prospero's window as Markdown, where these links
open its parts. Link the pane, setting or topic you name, in the
window's own words, and only one that exists:

- `[Configuration](help:config)` opens a Help topic, and
  `[Includes](help:config#includes)` one of its sections, by heading.
- `[Settings → Interface → editor](settings:ui#editor)` opens the
  Settings pane at a setting: `settings:KIND#KEY` in a section without a
  name, such as `[ui]` or `[tools]`; `settings:KIND/NAME#KEY` in a named
  one, such as `settings:model/qwen3-coder#effort` for `[model
  qwen3-coder]`; `settings:defaults#KEY` for a key above the first
  section; `settings:KIND` or `settings:KIND/NAME` for a page; and
  `settings:diagnostics` for the problems found in the files. The
  `settings` topic names each page and gives its link.
- `[View → Hosts](view:hosts)` shows a pane: `sessions`, `transcript`,
  `prompt`, `todo`, `plan`, `changes`, `jobs`, `log`, `hosts`,
  `settings`, `help` or `metrics`.
- A web link, `https://...`, opens in the user's browser.

## Changing settings

Change Prospero's settings only when the user asks you to. Edit
{{config}} with your write and edit tools, not the shell: Prospero asks
the user before each change to it, even where other writes need no ask
(except in Bypass mode), then reads the file again at once and says in
the tool's result whether it found problems there.

- Read the file first. Change only the lines the change needs, and keep
  its comments, blank lines and order. Its comments document each key
  with a commented-out line, such as `# editor = code --wait`: uncomment
  it, or put the key under its section's header, adding the header if
  the file has none.
- A section starts at a header, `[kind]` or `[kind name]`, and a setting
  is `key = value`. A `#` starts a comment only with blanks around it.
  Quote a value, `"..."`, to keep blanks at its ends.
- Never write an API key into the file. Write `key = env:NAME` and tell
  the user to set the environment variable NAME, or `key = file:PATH`
  for a file holding just the key.
- Leave an included file (`include = ...`) alone unless the user says
  otherwise: their own file overrides it below the include line.
- Endpoints, models and `[guard]` apply from every session's next
  request, and `[ui]` at once in the window. Most else, such as tools,
  shells, roles and MCP servers, applies to sessions started after the
  change: say so when the user needs a new session.
- A session on another host runs that host's engine, which reads that
  host's file; `[ui]` belongs to the window, on its own machine.
- Afterwards, say what changed, and link the setting.

The user's own instructions for every session go in {{agents}}, and
their own skills in {{skills}}, as NAME/SKILL.md.

# Instructions and skills

## Instruction files

When a session starts, the agent is given instructions from these
files, in order:

1. `AGENTS.md` next to the default configuration file, such as
   `~/.config/prospero/AGENTS.md`: your own, for every session, even
   when `-c` names another configuration file.
2. In each directory from the root of the repository (the nearest one
   holding `.git`) down to the working directory, the first of
   `AGENTS.md` or `CLAUDE.md` found there.
3. Later, the same for directories below the working directory, as the
   agent works there.

`[instructions] files` changes the names looked for:

    [instructions]
    files = AGENTS.md

A No folder session gets only your own file, none of a project's.

The session bar's Info button lists the instruction files the agent
was given, and Copy what the agent was given copies their text.

## Memory

Instruction files are yours; memory is the agent's. Each project has
a directory of memory in the state directory, local to this machine:
`memory/KEY`, where KEY is the project's root (the main checkout, for
a worktree) with each character but letters, digits, `.`, `_` and `-`
made `-`. It holds `MEMORY.md`, an index of one line per memory, and a
file for each memory, in Claude Code's format.

The agent saves what a later session in the project would need, such
as how to build and test it or what you have told it you prefer, on
its own judgment and when you ask it to remember something. It
forgets a memory that proves wrong, or when you ask. It writes them
with its memory tool, which never asks. A new session in the project
is given the index, and reads a memory's file when its line bears on
the task. A running session is told when another session, or you,
changes the index.

To have the agent remember something for every project on this
machine, ask it to remember it "on this machine": it adds it to your
own `AGENTS.md`, asking you first, as for any file outside the
project.

The Info button lists the session's memories, with Reveal to open
the index. You may edit or delete the files yourself. Only top-level
sessions in a folder have memory, not subagents or No folder
sessions.

    [memory]
    path = ~/notes/prospero-memory

`path` keeps the projects' directories elsewhere, and
`enabled = false` turns memory off.

## Skills

A skill is a directory holding `SKILL.md`, whose frontmatter gives its
`name` and `description`, with any files it refers to beside it, as in
Claude Code and other harnesses. The agent is told each skill's name
and description, and reads one through its skill tool when a task
calls for it.

Skills are looked for in:

1. `.claude/skills`, `.agents/skills`, `.cursor/skills` and
   `.codex/skills` in each directory from the working directory up to
   the root of the repository;
2. `[skills] paths`, separated by `;`;
3. each `--skillsdir DIR` given when starting Prospero, in order;
4. `skills` next to the default configuration file, such as
   `~/.config/prospero/skills`, even when `-c` names another file;
5. `~/.claude/skills`, `~/.agents/skills`, `~/.cursor/skills`, then
   `~/.codex/skills`.

The first skill of a name wins. A skill may sit below folders that
group skills, as in `.cursor/skills/frontend/lint/SKILL.md`, as Cursor
allows.

Type `/NAME args` in the prompt box to use a skill yourself: its
`SKILL.md` goes to the agent with the args. A skill with
`disable-model-invocation: true` in its frontmatter is for that alone,
and is not offered to the agent. The Info button lists the session's
skills. `[skills] enabled = false` turns skills off.

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

## Skills

A skill is a directory holding `SKILL.md`, whose frontmatter gives its
`name` and `description`, with any files it refers to beside it, as in
Claude Code and other harnesses. The agent is told each skill's name
and description, and reads one through its skill tool when a task
calls for it.

Skills are looked for in:

1. `.claude/skills` and `.agents/skills` in each directory from the
   working directory up to the root of the repository;
2. `[skills] paths`, separated by `;`;
3. each `--skillsdir DIR` given when starting Prospero, in order;
4. `skills` next to the default configuration file, such as
   `~/.config/prospero/skills`, even when `-c` names another file;
5. `~/.claude/skills`, then `~/.agents/skills`.

The first skill of a name wins.

Type `/NAME args` in the prompt box to use a skill yourself: its
`SKILL.md` goes to the agent with the args. A skill with
`disable-model-invocation: true` in its frontmatter is for that alone,
and is not offered to the agent. The Info button lists the session's
skills. `[skills] enabled = false` turns skills off.

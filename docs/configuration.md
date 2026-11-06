# Configuration

Prospero reads one INI file, `$XDG_CONFIG_HOME/prospero/prospero.ini`,
which defaults to `~/.config/prospero/prospero.ini`. On Windows, home
is `%HOME%`, else `%USERPROFILE%`, so that Prospero started from
Explorer finds the same files as from a shell that sets HOME, and
`%APPDATA%\prospero\prospero.ini` is used only while there is none
under home. With no file, nothing is configured, and none is written
until a setting is first saved, which writes the file from its
built-in copy ([assets/prospero.ini](../assets/prospero.ini)). The copy
sets nothing, and documents every section and key: `[endpoint NAME]`,
`[model NAME]`, `[role NAME]`, `[guard]`, `[tools]`, `[instructions]`,
`[skills]`, `[sessions]`, `[ui]`, `[desktop]`, `[host]`, `[remote NAME]`
and `[container]`. Changes take effect while it
runs. An unknown key or a bad value is reported in the diagnostics,
and a bad value falls back to its default: nothing in the file keeps
Prospero from starting. The settings window edits the file a line at a
time, and its Open in editor opens the whole file in your editor, read
again once the editor exits.

The agent can change the file too, through Prospero's own skill (see
Skills). A write or edit of the file, or of a local file it includes,
asks every time, with no "always", even inside an allowed directory
(Bypass mode aside); once written, the configuration is read again at
once, the agent is told of any problem in what it wrote, and the
window takes up the change.

API keys never go in the file: `key = env:NAME` reads one from the
environment when it is needed, and `key = file:PATH` from a file
holding just the key, for an engine whose environment you cannot set,
such as one on a remote host or in WSL (its own configuration names a
file there). Above the first section, `include = ` adds a shared file,
such as a team's endpoints and models, whose settings this file can
override. An include may be a path or an http or https URL, which is
fetched at startup, cached, and refreshed in the background, and may
be pinned to a SHA-256. Nothing in an included file can make Prospero
run a command, or let the agent act without asking: its plaintext
secrets and `file:` secrets, `[remote]`, `[container]` and `[desktop]`
settings, an
`[mcp]` section's `command`, `cwd`, `env.NAME` and `trust = yes`,
`[web] ask = no`, `[tools] path_prepend` and `path_append`, a
`[tools] shell` other than a keyword, and `[ui] editor` and `terminal`
are ignored. It may define an MCP server by its url, and `[web]`, but
cannot change a server your own file gives the `url` or `command` of,
nor a `[web]` whose `search` or `url` your file sets. Your file may add
to an included section instead, such as `key = file:PATH` for an
included endpoint, or a `token` for an included MCP server.
Its `env:NAME` values are read from your environment, as yours are.

A file included by URL can change at any time, so a url it gives an
`[endpoint]` or `[mcp]` section gets nothing of yours until you approve
it for that url, as a cookie goes only to its site: any `env:NAME` the
section's `key`, `token`, `client_secret` or `header.NAME` takes, from
either file, a secret or header your own file adds there, and an MCP
server's `trust = yes`. Until then Prospero does not contact the url.
A session about to use it asks once, listing each url and what would
go there; allowing holds for good, for every session, and answers the
other sessions waiting on it. The section's page in Settings has
Approve, and `prospero approve` lists what waits (`prospero approve
URL` approves it). An include that changes the url needs approving
again. Approvals are kept by the url's SHA-256 in the state
directory's `approvals.json`. A url from a local include or your own
file needs none, and Bypass mode does not approve them. A server whose certificate is not trusted fails the turn
at once, saying why, rather than being retried.

On macOS, Prospero started from Spotlight, the Finder or the Dock gets
launchd's environment, with a PATH of only the system's directories, so
it takes its login shell's environment instead: once, at start, `$SHELL
-l -i -c` printing `env`, as VS Code does, with
`PROSPERO_RESOLVING_ENVIRONMENT=1` so a profile can tell. A profile that
takes over five seconds is stopped, and the window keeps what it had.
Started from the Start menu or a desktop menu elsewhere, it has a
shorter PATH than a shell gives it. `[tools] path_prepend`
and `path_append` add directories, separated by `;`, before and after
it: for the agent's commands, its shell and ripgrep, and for what the
GUI runs itself, the editor and ssh. A change applies to the next
command, and for the editor and ssh once saved in the settings window
(or at the next start). A remote engine goes by its own machine's
configuration.

    [tools]
    path_prepend = ~/.local/bin; /opt/homebrew/bin

An endpoint's `url` is its base: chat requests go to
`URL/v1/chat/completions`, and its listing is `URL/v1/models`, with or
without the `/v1` in `url`. For a server or proxy with no `/v1` in its
paths, give the whole URL of its chat requests instead, and none is
added: with `url = https://proxy.example.com/auto/chat/completions`,
the listing is `https://proxy.example.com/auto/models`.

An endpoint speaks Anthropic's Messages API with `kind = anthropic`
(whose whole URL ends in `/messages` instead):

    [endpoint anthropic]
    kind = anthropic
    url = https://api.anthropic.com/
    key = env:ANTHROPIC_API_KEY

Its listing gives each model's context and output limits. Reasoning
goes back to the model verbatim with its signature, and the tools, the
system prompt and the conversation carry cache breakpoints, so cache
reads grow turn over turn. A `[model NAME]` section sets `thinking`
(adaptive by default), `thinking_display`, `effort` and `max_tokens`.

`kind = responses` speaks OpenAI's Responses API (whose whole URL ends
in `/responses`), with its key, at OpenAI or at a server that serves
it, as llama-server does:

    [endpoint openai]
    kind = responses
    url = https://api.openai.com/v1
    key = env:OPENAI_API_KEY

Nothing is stored at the server: each request carries the whole
conversation, its reasoning going back encrypted as the server gave
it, so the prompt cache keeps hitting, with the session as its key.
`[model NAME] effort` sets the reasoning effort (`none` through
`xhigh`), and `max_tokens` the output's limit.

`kind = chatgpt` is the same API at ChatGPT's Codex backend, billed to
a ChatGPT subscription rather than by the token. It takes no key: you
sign in once with your ChatGPT account, on the endpoint's page in
Settings (Sign in), or with `prospero endpoint auth NAME`, which
prints the page to visit. The browser comes back to this computer at
`localhost:1455` (else 1457), as for OpenAI's own Codex CLI, so sign
in on the computer that runs the engine. The tokens are kept in the
state directory's `endpoints/auth`, readable only by you, refreshed as
they expire, and forgotten by Sign out or `prospero endpoint logout
NAME`. Its URL is filled in for you:

    [endpoint chatgpt]
    kind = chatgpt
    url = https://chatgpt.com/backend-api/codex/responses

The models are those your plan includes, such as `gpt-5-codex`. A
usage limit reached ends the turn, saying when it resets, rather than
waiting hours in retries, or moves to a `fallback` if the model has
one. In Settings, adding an endpoint asks its kind first, and fills in
the kind's URL; changing the kind later moves a URL that is still
another kind's own, but keeps one you typed.

`kind = copilot` is GitHub Copilot, billed to your Copilot plan
(Copilot Free included). It takes no key either: Sign in, or
`prospero endpoint auth NAME`, shows a code to enter at GitHub's page,
github.com/login/device, in any browser on any computer, so it suits
an engine over ssh as well. The token is kept as ChatGPT's is, needs no
refreshing, and is forgotten the same ways. Its URL is filled in:

    [endpoint copilot]
    kind = copilot
    url = https://api.githubcopilot.com

While the URL is that one, requests go to the API your account's
sign-in names (such as `api.individual.githubcopilot.com`); a URL you
typed is used as it is. Copilot serves each model by one of three
APIs, and Prospero speaks to each in the one its listing names:
Anthropic's Messages API for Claude, the Responses API for newer GPT
models, Chat Completions for the rest. Each request says whether you
or the agent began it (a tool's results, compaction, a subagent), as
Copilot bills only the first as a premium request.

GitHub gives each app that signs in its own set of models. Prospero
signs in as its own GitHub app, which GitHub gives only its older
models (such as `gpt-4o`, `gpt-4o-mini` and `gpt-4.1`, as
`gpt-41-copilot`), not those of your plan's model picker. `client_id`
in the endpoint's section signs in as another OAuth app instead, at
your own risk: GitHub's terms are for you to weigh, and a token for
another app is kept apart, so changing it asks for a new sign-in. For
kind chatgpt it likewise replaces the Codex CLI's client.

Copilot may refuse a model with "not available for integrator", as it
takes requests for the product (the integration) of the app signed in
as, or of the request's `Copilot-Integration-Id`. `integration_id` in
the endpoint's section sends that header, naming the product, likewise
at your own risk. Prospero sends none of its own.

For an OpenAI-compatible server, `template.NAME = VALUE` in a
`[model NAME]` section is an argument of the model's chat template,
sent in `chat_template_kwargs` (as llama.cpp and vLLM take it): `true`
and `false` as booleans, numbers as numbers, and anything else, or
anything in quotes, as a string. A model is often run several ways,
such as thinking more or less, and `like = OTHER` makes a variant: a
model of its own name that takes OTHER's settings but those it sets
itself, and is sent to the server as OTHER (or as its own `id`), so
discovery, context limits and the places on a router are OTHER's:

    [model large]
    template.thinking = true

    [model large-quick]
    like = large
    template.reasoning_effort = 25

    [model large-deep]
    like = large
    template.reasoning_effort = 100

A variant is chosen as any model is, for a session, a role, a
fallback or a compactor.

A `[model NAME]` section applies wherever an endpoint lists NAME. Where
two endpoints list the same model, `[model ENDPOINT/MODEL]` sets it on
one of them only, each of its settings winning over `[model MODEL]`'s:

    [model large]
    template.thinking = true

    [model gpu/large]
    context = 32768

A variant of `large` on `gpu` takes the `gpu/large` settings too. A
variant with `like = gpu/large` is `gpu`'s alone: the model picker
offers it there only, and it takes those settings wherever it is used.
ENDPOINT must be a configured endpoint with a URL: otherwise the name
is a model's own, as for `Qwen/Qwen3-8B`.

A default `model` that names no endpoint, with no `endpoint` set, goes
to the first endpoint whose listing has it. When others list it too, a
new session's note says which it went to; `model = ENDPOINT/MODEL`
chooses.

Sessions, their logs and the cache of URL includes live in the state
directory: `$XDG_STATE_HOME/prospero`, else `~/.local/state/prospero`.
On Windows, `%LOCALAPPDATA%\prospero`, where it used to be, is used
while there is none under home.

At the start of a session the agent is given its instructions:
`AGENTS.md` next to the default configuration file (such as
`~/.config/prospero/AGENTS.md`), even when `-c` names another, then
the first of `AGENTS.md` or `CLAUDE.md` (`[instructions] files`) in
each directory from the root of the repository down to the working
directory, and more below it as the agent works there.

Memory is the agent's own, as Claude Code's auto memory is: for each
project, below `memory` in the state directory, a `MEMORY.md` index of
one line per memory, given to each new session in the project, and a
file for each, which the agent saves and forgets with its memory tool,
on its own judgment and when asked. A repository's worktrees share
one. To remember something for every project on this machine, the
agent adds it to the user's own `AGENTS.md`, asking first.
`[memory] enabled = false` turns it off.

Skills are the same as in Claude Code and other harnesses: a directory
holding `SKILL.md`, whose frontmatter gives its `name` and
`description`, with any files it refers to beside it, perhaps below
folders that group skills, as Cursor allows. They are found in
`.claude/skills`, `.agents/skills`, `.cursor/skills` and
`.codex/skills` in each directory from the working directory up to the
root of the repository, then in `[skills] paths`, then in each
`--skillsdir DIR` given to `prospero`, `run`, `serve` or `attach` (as
many as wanted), then in `~/.config/prospero/skills`, then in the same
four under `~`; the nearest of a name wins. The user's own follow the
default configuration file, even when `-c` names another. The agent is told each
skill's name and description, loads one with its skill tool, which
also lets it read the skill's directory, and hears of skills added or
changed as it works. `/NAME args` in the prompt box uses a skill
directly, and one with `disable-model-invocation: true` is for that
alone. `[skills] enabled = false` turns them off. Without editing a
`SKILL.md`, `[skills] hidden = NAME…` makes skills the user's alone in
the same way, and `[skills] disabled = NAME…` leaves them out
altogether, from the agent and from `/NAME`; each takes names
separated by blanks, and covers every skill of a name, so none further
away takes its place. Either may name `prospero`.

One skill is built in: `prospero`, Prospero's Help and every setting,
served by the engine itself, so that the agent can answer questions
about Prospero and change its settings. Its replies link Help topics
(`help:NAME#SECTION`), settings (`settings:KIND/NAME#KEY`) and panes
(`view:PANE`), which a click in the window opens. A skill of the
user's own named `prospero` replaces it, never a project's, and eval
sessions do without it.

## MCP servers

MCP servers give the agent more tools, offered beside the built-in
ones as `mcp__SERVER__TOOL`. Each is an `[mcp NAME]` section: a
`command` that speaks MCP over its standard input and output, or the
`url` of a server over HTTP (Streamable HTTP; not the old SSE
transport).

    [mcp stars]
    command = uv run tools/starsmcp.py
    cwd = ~/src/StarsVM
    startup_timeout = 120

    [mcp docs]
    url = https://mcp.example.com/mcp

A command is split into words with `'...'` or `"..."` quoting, and
runs without a shell. Begin it with `'`, since a value beginning with
`"` is quoted whole. It runs in the session's folder by default, is
started for the sessions that use it, and stops a minute after the
last one has gone. Its standard error is logged in the state
directory's `mcp/logs`. `tools` keeps only some of a server's tools,
`trust = yes` runs them without asking, and a role with a tools list
names them as `mcp__NAME` or `mcp__NAME__TOOL`. Otherwise each call
asks first, with an "always" for all of that server's tools, which for
a server over HTTP holds only while its url stays the same. An included
file may define a server by its `url`, but its `command`, `cwd`,
`env.NAME` and `trust = yes` are ignored, as is anything it sets for a
server your own file gives the `url` or `command` of. Your file may add
to an included server, a `token = file:PATH` say, or `trust = yes`,
which for a url from a web include wait for your approval (see
Includes).

A project's `.mcp.json`, in Claude Code's format, with `${VAR}` and
`${VAR:-default}` expanded, is read once as a session starts, from its
folder up to the repository's root. Its servers start only once you
allow them, for the session or always for that project until their
definitions change. A call to one may also trust its server in that
project, from then on, under the same condition. Your own section of
the same name wins.

A server over HTTP may take a bearer `token` (as `env:NAME`) or
`header.NAME` values, or want you to sign in with OAuth. Settings, on
its page, has Sign in, which opens the browser and takes its return on
a loopback port; `prospero mcp auth NAME` does the same without a
window, printing the address to visit. Prospero discovers the
authorization server, registers itself as a client unless `client_id`
is set (as "Prospero", or as `client_name`), and uses PKCE. The browser comes back to
`http://127.0.0.1:PORT/callback`, at a port the system picks, unless
`redirect_uri` names one (`http://127.0.0.1:PORT/PATH` or
`localhost`): a client registered beforehand with one fixed address,
as some servers require, needs it. It keeps the tokens in the state directory's
`mcp/auth`, mode 0600, and refreshes them when the server refuses
one. `prospero mcp logout NAME`, or Sign out, forgets them.

## Web search

`web_search` sends a query to a search service and returns titles,
links, dates and excerpts; `web_fetch` reads one https page as text,
with scripts, navigation and hidden elements dropped and links made
absolute. Neither is offered until `[web]` turns it on:

    [web]
    search = searxng
    url = http://localhost:8888/

`search` is `searxng` (your own instance, with `json` in its
`search.formats`), `exa` (Exa's keyless MCP endpoint, or with `key`),
or `anthropic` (a side request to an Anthropic model with Anthropic's
web search tool: `model = ENDPOINT/MODEL`, by default
`claude-haiku-5-5` on the first endpoint of kind `anthropic`).
`fetch = no` leaves out `web_fetch`, `results` caps a search's
results (8), and `searches` a session tree's searches (200). An
included file may set `[web]`, but not `ask = no`, and not at all
when your own file sets its `search` or `url`; your file may add a
`key` to an included `[web]`.

Every search and page asks first, showing the query and its service
or the whole address, with "always" for every search or every page of
a site; `ask = no` asks about none. `web_fetch` reads only addresses
that already appeared in the conversation (the user's messages,
files, tool output, results and pages), never one the agent composed,
and never a local address: IP addresses, `localhost`, dotless names,
`.local` and the like. It follows redirects within a site, reports
those to another, and keeps a long page's whole text in the session's
`out` folder to read in parts. What both tools return is set between
lines marked with a random word, after a line saying that it is
information, never instructions, and characters people cannot see but
models read are removed. A role without the web tools, say one that
delegates to a `tools = web_search web_fetch` role, keeps an agent off
the web.

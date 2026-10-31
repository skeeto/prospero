# MCP servers

An MCP server gives the agent more tools: a program that speaks the
Model Context Protocol over its standard input and output, or a
service reached over HTTP. Its tools are offered beside the built-in
ones, each named `mcp__SERVER__TOOL`, and a session's first request
waits for its servers to start ("starting MCP servers"), at most each
one's `startup_timeout`.

## Configuring one

Each is a section, `[mcp NAME]`, with either a `command` or a `url`:

    [mcp stars]
    command = uv run tools/starsmcp.py
    cwd = ~/src/StarsVM
    startup_timeout = 120

    [mcp docs]
    url = https://mcp.example.com/mcp

A command is split into words at blanks, with `'...'` or `"..."`
quoting, and runs without a shell. Begin it with `'`, never `"`, which
would quote the whole value. It runs in the session's folder unless
`cwd` says otherwise, with `env.NAME` adding to its environment. Its
standard error goes to a log in the state directory, whose last lines
explain a failure. A program starts for the sessions that use it and
stops a minute after the last one has gone; one that crashes starts
again at the next call.

`tools` keeps only some of a server's tools, `enabled = no` leaves it
out, and `timeout` limits each call. A role with a tools list gets MCP
tools only by naming them: `mcp__NAME` for all of a server's, or
`mcp__NAME__TOOL`. Subagents get their parent's.

An included file may give a server by its `url`, but not its
`command`, `cwd` or `env.NAME`, which run programs, nor `trust = yes`,
nor change a server your own file gives the `url` or `command` of. Your
file may add to an included server, such as its `token`, but your
`trust = yes` counts only for a server you define yourself.

## Asks

A call asks you first, as a write does, showing the server, the tool
and its arguments, unless the section says `trust = yes` or the
session is in Bypass mode. "Always allow SERVER" allows every tool of
that server for the rest of the session and its subagents, or, for a
server over HTTP, until its url changes. A project's server also offers "Trust SERVER in this project", which
allows its tools in later sessions too, until the `.mcp.json` changes
its definition. In Plan mode, only tools
the server marks read-only may run. See
[Permissions and asks](help:permissions).

## A project's servers

A `.mcp.json` in the session's folder, or in a folder above it up to
the repository's root, names servers as Claude Code's does:

    {"mcpServers": {
      "stars": {"command": "uv", "args": ["run", "tools/starsmcp.py"]},
      "web": {"type": "http", "url": "https://example.com/mcp",
              "headers": {"Authorization": "Bearer ${WEB_TOKEN}"}}}}

`${VAR}` and `${VAR:-default}` take values from the environment. A
program runs in the folder of the `.mcp.json` that names it. A
section of your own by the same name wins.

Since such a file comes with the project, a session asks before it
starts any of them: "Start the MCP servers of PATH?", listing each
one's command or URL. Allow covers the session, and "Always for this
project" covers the project until a server's definition changes.
Starting a server is not using it: its calls still ask, as above.
A session nobody is watching (`prospero run`) does not start them, and
says so. A session reads the file once, as it starts.

## Signing in

A server over HTTP may want a token. `token = env:NAME` sends one from
the environment, and `header.NAME` sends other headers. A server that
wants you to sign in fails, saying so, until you do: in Settings, on
the server's page, **Sign in** opens the browser at the server's own
sign-in, which then comes back to Prospero. Without a window,
`prospero mcp auth NAME` prints the address to visit in a browser on
the same machine.

The tokens are kept in the state directory, readable only by you, sent
only to that server's URL, and refreshed as they expire. **Sign out**
(or `prospero mcp logout NAME`) forgets them. A server that does not
register clients itself needs `client_id`, and perhaps
`client_secret`, from its provider; `scope` asks for particular
scopes. Prospero registers under the name "Prospero" unless
`client_name` says otherwise, for a server that admits clients by
name. If that client was registered with one address for the
browser to come back to, set it as `redirect_uri`, such as
`http://127.0.0.1:19876/callback`: Prospero then listens on that port
and path rather than on one the system picks.

The page also shows whether a server is running and with how many
tools, or why it failed; **Restart** starts it again.

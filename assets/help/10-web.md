# Web search

Two tools let the agent look things up: `web_search` sends a query to
a search service and returns titles, links, dates and excerpts, and
`web_fetch` reads one web page as text, with its links made absolute.
Neither is offered until your configuration's `[web]` section turns
it on.

## Setting it up

`search` picks the service:

- `searxng`: a SearXNG instance of your own, at `url`. Its JSON output
  must be on: add `json` to `search.formats` in its `settings.yml`.
  Queries go only to that instance, and from it to the engines it
  uses.
- `exa`: Exa's search service, which needs no account; `key` adds an
  Exa key for more searches.
- `anthropic`: a request to an Anthropic model with Anthropic's own
  web search tool. `model`, as `ENDPOINT/MODEL`, names an endpoint of
  kind `anthropic` and a model there; by default, the first such
  endpoint and `claude-haiku-5-5`. Each search costs that model's
  tokens and Anthropic's price per search, which the session's cost
  does not count.

For example:

    [web]
    search = searxng
    url = http://localhost:8888/

`fetch = no` leaves out `web_fetch`, and `search = none` (the default)
leaves out `web_search`. `results` is the most results a search
returns (8, at most 20), and `searches` the most searches a session
and its subagents may make together (200). An included file may set
up `[web]`, but not `ask = no`, and not at all when your own file sets
its `search` or `url`. Your file may add a `key` to an included one.

## Asks

Every search and every page asks you first, showing the query and the
service it goes to, or the page's whole address, so that you see what
leaves your machine. Like other asks, one is denied when nobody
answers in time, and at once in a session nobody attends, such as
`prospero run`. "Always allow web searches" allows every search for
the rest of the session and its subagents, and "Always allow HOST"
every page of that site. Think before allowing searches for good: a
query, like an address, can carry what the agent read, and that goes
out unseen.

`ask = no` asks about none of them, which suits `prospero run` and
evals, and Bypass mode asks about none either. Both tools change
nothing on your machine, so Plan mode allows them.

## What web_fetch reads

Only addresses that already appeared in the conversation: in your
messages, in files the agent read, in the output of commands and
tools, or in search results and pages. It never reads one the agent
made up, which keeps a page from steering the agent into sending your
data to an address of the page's choosing. A subagent may read the
addresses in its task that its parent was shown, and its report
vouches to the parent for the addresses it saw.

It reads only `https`: `http` is upgraded, and a redirect to `http`
refused. It never reads a local address: an IP address, `localhost`,
a name without a dot, or names such as `.local` and `.internal`. A
redirect to the same site is followed; one to another site is
reported, and reading it is another call that asks for that site.
PDFs, images and other files that are not text are refused.

A page is read as text, without its scripts, styles, navigation, and
anything hidden. Its whole text is kept in the session's `out`
folder, and the agent reads a long page in parts, without fetching it
again or asking.

These rules hold against the agent, not against a determined one:
the shell can reach the web too. The asks are what you control.

## Web pages are not instructions

Pages can hold text written to steer an agent: "ignore your
instructions and..." What the web tools return is set between two
lines marked with a random word that no page can know, after a line
saying that the text is information, never instructions. The agent is
told to tell you when a page tries to give it some. Characters you
cannot see but a model reads, such as Unicode tags and bidirectional
overrides, are removed.

For an agent that should never meet the web, leave the tools out of
its role, and keep it from starting a role that has them. A role that
reads the web but holds no files or shell can do the looking up for
it:

    [role research]
    description = Looks things up on the web and reports what it found
    tools = web_search web_fetch

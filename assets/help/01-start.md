# Getting started

**Close any pane you do not need, this one too: the View menu shows each again, and Reset layout puts them all back.**

The field above these topics searches every page: type words, and the
sections that have them all take the topics' place, headings that
match first. Click one to go there, or press Enter for the first.
Escape, or the X, brings the topics back.

Buttons drawn as icons say what they do in their tooltips: hold the
mouse over one to see its name.

Prospero is a coding harness. A language model, the agent, works on
code through tools: it reads, writes and edits files, runs commands,
searches, keeps a todo list, starts background jobs and subagents. The
harness is this program. It runs the turn loop and the tools, retries
a server that fails, compacts a conversation grown too long, and asks
you before the agent goes outside its working directory.

Each session is a conversation with the agent in one working
directory. Sessions are kept in the state directory, so they survive
quitting, and a crash.

## First-time setup

The agent needs an endpoint, a server of models, and a model there.
Prospero speaks the OpenAI Chat Completions API (llama.cpp, vLLM,
LiteLLM and others) and Anthropic's Messages API.

On its first run Prospero writes its configuration file from a
built-in copy that documents every setting. That copy has one
endpoint, `local`, at `http://localhost/`, and no model: a new session
gets the first model that endpoint lists. When the endpoint answers
with a model, the first run needs no settings at all.

1. Open the Settings pane (View → Settings). The line at its top says
   which file its edits go to, usually
   `~/.config/prospero/prospero.ini` (see
   [Configuration](help:config)).
2. Under Endpoints, pick `local` and edit its url, or use Add
   endpoint... for another server. An API key is never written in the
   file: set `key` to `env:NAME`, and Prospero reads it from the
   environment variable NAME, or to `file:PATH`, a file holding the
   key.
3. Optionally, on the Defaults page, set `model` to the endpoint's
   name and the model's, as `ENDPOINT/MODEL`, such as
   `local/qwen3-coder`. Without it, a new session gets the first
   `[model]` section, else the first model the endpoint lists. A model
   without its endpoint goes to Defaults' `endpoint`, else to an
   endpoint that listed it, else to the first endpoint with a url. The
   New session dialog lists every endpoint's models, to pick another
   for one session.

Changes apply while Prospero runs. A mistake is reported in the
settings' Diagnostics page, and a bad value falls back to its default.

To run the agent on another machine, add it in the Hosts pane; see
[Hosts and remotes](help:hosts).

## Starting a session

Session → New session... (Ctrl+N, Cmd+N on macOS) asks for the working
directory and, optionally, a model from those the endpoints list. The
prompt box at the bottom then has the keyboard: type a prompt and press
Enter. Shift+Enter starts a new line.

From a terminal, `prospero DIR`, such as `prospero .`, opens a new
session in that directory, in Build mode with the default model. If
a window is open already, the session starts there.

## More

- [Sessions and modes](help:sessions): forks, rewinds, exports, and
  the Build, Plan and Bypass modes.
- [Permissions and asks](help:permissions): what the agent may do
  without asking.
- [Keyboard and mouse](help:keys) and
  [Troubleshooting](help:trouble).

The full list of settings, with what each does, is in the
configuration file itself, as Prospero first wrote it.

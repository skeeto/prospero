# Prospero: a desktop coding harness for local and remote LLMs

Prospero is a coding harness: a desktop program in which a language
model, the agent, works on code through tools. It runs the turn loop,
the tools (read, write, edit, shell, grep, glob, a todo list,
background jobs, and subagents), and the processes they start, and
shows the conversation as it streams. It speaks the OpenAI Chat
Completions API, so it works with llama.cpp, vLLM, LiteLLM, and hosted
providers with a compatible API, Anthropic's among them. It is built
first for models served on your own machines, whose servers are slow
to start, run out of context, and stall: the harness, not the agent,
retries, compacts, queues requests at a busy server, and keeps a turn
from getting stuck.

It is one executable. The engine runs in the same process as the
window, and needs no daemon or server. The same executable runs
headless, on another machine over ssh, or in the background after its
window closes. Each session is an append-only log, from which it
recovers after a crash. Undo and rewind put back the files the harness
changed.

## Build

CMake 3.25 or later and a C++20 compiler with `#embed` (GCC 15 or
later, or a recent Clang):

    $ cmake -B build
    $ cmake --build build

The default build type is Release. Debug builds trap on undefined
behavior (UBSan), failed assertions included. Dependencies are fetched
at configure time, each pinned by URL and SHA-256: Dear ImGui (docking
branch), md4c, FreeType and SDL3 for the GUI, and mbedTLS with
cpp-httplib for HTTP on macOS and Linux. To build offline, or with a
modified dependency, put its source under `deps/` (`deps/imgui`,
`deps/md4c`, `deps/freetype`, `deps/sdl3`, `deps/mbedtls`,
`deps/cpp-httplib`), and it is used instead.

On Windows, build the same way from a [w64devkit][] shell, with CMake
and Ninja on the PATH:

    $ cmake -B build -G Ninja
    $ cmake --build build

The Windows GUI is its own Win32 layer over Direct3D 11, and HTTP goes
through WinHTTP, so it uses the system's certificates and proxies. The
toolchain files in `cmake/` cross-compile for Windows with Mingw-w64
(`Toolchain-Mingw64.cmake`, `Toolchain-Mingw32.cmake`), for Windows on
Linux with w64devkit's own GCC under Wine, as releases are built
(`Toolchain-W64devkit-Wine.cmake`), and for x86-64 Linux
(`Toolchain-Linux-x86_64.cmake`).

`-DPROSPERO_GUI=OFF` builds the headless engine alone, with no GUI
dependencies. On Windows it is CRT-free and imports only kernel32,
advapi32 and winhttp. The i686 build of it is the Windows XP engine:

    $ cmake -B build-xp -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw32.cmake \
            -DPROSPERO_GUI=OFF
    $ cmake --build build-xp

On macOS the build also wraps the executable as `Prospero.app` in the
build directory. On Linux, SDL3 loads X11, Wayland and GL at run time,
so the GUI build still runs headless where no display libraries exist.
With `-DPROSPERO_GUI=OFF`, `-DPROSPERO_STATIC=ON` links the headless
engine fully statically, as `tools/engines.py` does for the Linux
engines it builds.

## Releases

Each release on GitHub carries six files:

- `prospero-VERSION.tar.gz`: the source, with every dependency unpacked
  under `deps/`, so that it builds with no network:

      $ tar xzf prospero-VERSION.tar.gz
      $ cmake -S prospero-VERSION -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      $ cmake --build build

- `prospero-VERSION-windows-x86_64.exe`: the Windows program, the GUI
  with its engine.
- `prospero-engine-VERSION-windows-x86_64.exe` and
  `prospero-engine-VERSION-windows-i686.exe`: headless engines for
  Windows, the i686 one for XP.
- `prospero-engine-VERSION-linux-x86_64` and
  `prospero-engine-VERSION-linux-aarch64`: headless engines for Linux,
  linked statically.

The engines are named as `prospero deploy` expects to find them, in an
`engines` directory next to the executable. There is no macOS build;
build it from source.

The release workflow (`.github/workflows/release.yml`) builds every
program from the source tarball alone. The Windows programs are built
on Linux by [w64devkit][]'s GCC under Wine, the same programs it builds
on Windows, stripped and with no timestamp, then signed with an
Authenticode signature (Azure Artifact Signing), which Windows shows in
the file's Properties, under Digital Signatures. `tools/dist.py` makes
the source tarball from any commit, the same commit always giving the
same tar file.

GitHub records each file's SHA-256 digest, shown on the release page
next to the file. To check the files downloaded into the current
directory against them all at once:

    $ gh release view vVERSION -R skeeto/prospero --json assets \
          --jq '.assets[] | .digest[7:] + "  " + .name' |
          sha256sum -c --ignore-missing

## Usage

    prospero [gui [options]]
    prospero run [options] PROMPT
    prospero run [options] --resume SID [PROMPT]
    prospero --headless [options]
    prospero serve [options]
    prospero attach [--reconnect] [options]
    prospero hosts [--state DIR]
    prospero connect NAME | --ssh DEST | --local [options]
    prospero deploy NAME|DEST [--force] [options]

With no arguments, Prospero opens its window: sessions in a sidebar,
subagents under their parents; the transcript, with markdown, code
highlighting and selection; tool calls with Undo, and Rewind to any
message that began a turn; the todo list and background jobs; and the
prompt. A file tool that goes outside the session's working directory,
or the roots the configuration adds, asks first, and an answer not
given in time denies it. The settings window edits the configuration
file in place, changing only the lines it sets. Closing the window
while turns run asks whether to stop them, or to keep running in the
background, with a tray icon to bring the window back.

`prospero run` runs one turn with no window and exits when it ends,
printing its records as JSON lines, or readable text with `--pretty`.
Nobody answers its asks, so they are denied. It exits 0 when the turn
is done, 1 on error, 3 at the step budget, 130 when cancelled, and 2
for usage errors. Options: `-C DIR` (working directory), `-e ENDPOINT`,
`-m MODEL`, `-r ROLE`, `-c FILE` (configuration) and `--state DIR`.

`prospero --headless` is the engine alone, speaking the protocol (one
JSON object per line, `src/events.cpp`) on standard input and output:
commands in, records out. The GUI can run its engine this way, as a
child process, with `--engine=subprocess`.

`prospero serve` is the engine as a background host, behind a socket
that only this user may use (a named pipe on Windows). `prospero
attach` bridges standard input and output to the host of a state
directory, starting one if none runs; this is what runs at the other
end of ssh. `prospero hosts` lists the hosts running, and `prospero
connect` speaks the protocol to a host: a `[remote NAME]`, any ssh
destination, or this machine's. A host exits after ten minutes with no
client and nothing running.

`prospero deploy` puts an engine on another machine, as below.

## Configuration

Prospero reads one INI file, `$XDG_CONFIG_HOME/prospero/prospero.ini`,
which defaults to `~/.config/prospero/prospero.ini`. On Windows,
`%XDG_CONFIG_HOME%` and `%HOME%\.config` are tried first, then
`%APPDATA%\prospero\prospero.ini`. On first run it writes the file
from its built-in copy ([assets/prospero.ini](assets/prospero.ini)),
which documents every section and key: `[endpoint NAME]`,
`[model NAME]`, `[role NAME]`, `[guard]`, `[tools]`, `[instructions]`,
`[ui]`, `[host]` and `[remote NAME]`. Changes take effect while it
runs. An unknown key or a bad value is reported in the diagnostics,
and a bad value falls back to its default: nothing in the file keeps
Prospero from starting.

API keys never go in the file: `key = env:NAME` reads one from the
environment when it is needed. Above the first section, `include = `
adds a shared file, such as a team's endpoints and models, whose
settings this file can override. An include may be a path or an http
or https URL, which is fetched at startup, cached, and refreshed in the
background, and may be pinned to a SHA-256. Nothing in an included file
can make Prospero run a command, and its plaintext secrets are ignored.

Sessions, their logs and the cache of URL includes live in the state
directory: `$XDG_STATE_HOME/prospero`, else `~/.local/state/prospero`,
or `%LOCALAPPDATA%\prospero` on Windows.

At the start of a session the agent is given its instructions:
`AGENTS.md` next to the default configuration file (such as
`~/.config/prospero/AGENTS.md`), even when `-c` names another, then
the first of `AGENTS.md` or `CLAUDE.md` (`[instructions] files`) in
each directory from the root of the repository down to the working
directory, and more below it as the agent works there.

## Remote and background hosts

A `[remote NAME]` section names a machine reached over ssh, and the
window's Hosts panel adds one and connects to it. Prospero there runs
as `prospero attach`, started over `ssh -T`, which starts a host that
outlives the ssh session. A link that breaks is taken up again from
where it was, and the host keeps a client's place for three hours. A
window closed with "Keep running in background" makes its own process
such a host, and a later window lists its sessions and attaches to
them.

A remote machine needs an engine built for its platform, which
`prospero deploy` uploads with scp to `~/.prospero` there, as
`engine-PROTO-VERSION-OS-ARCH`, verifies it by its SHA-256, and
records where it went, for connecting. With `deploy = auto` in its
section, connecting deploys one when the machine has none. Engines come
from the `engines` directory next to the executable, named
`prospero-engine-VERSION-OS-ARCH`, or are the executable itself when it
is built for that platform. `tools/engines.py` builds them: macOS
arm64, Windows x86-64 and i686, and Linux aarch64 and x86-64 (static,
built on a Linux host given with `--linux HOST`, else left out).

## Windows notes

The GUI build's `prospero.exe` is a GUI program, so at an interactive
cmd or PowerShell prompt it prints nothing and the prompt returns at
once. Pipe or redirect its output instead: `prospero --version | more`,
or `prospero --version >out.txt`. Over ssh and in scripts it works as
usual, and it serves as the headless engine there too. The headless
build (`-DPROSPERO_GUI=OFF`), which is the XP engine and the engine
deployed to Windows machines, is a console program, and works as usual
at a prompt as well.

The agent's shell is a POSIX `sh`: `sh.exe` or busybox-w32's
`busybox.exe` next to `prospero.exe`, else on the PATH. Without either,
commands run in `cmd.exe`, and the shell tool's description says so.
Each tool runs in a job object, so that its whole process tree dies
with it, or with Prospero.

Windows XP runs only the headless engine (the i686 build): `prospero
run`, or a host reached over ssh. Its WinHTTP has no TLS 1.2, so
endpoints must be plain http, as on a local network. Before Windows 8,
a process in a job cannot put its children in jobs of their own, so
under an ssh server that runs sessions in a job, each tool breaks away
into its own job where the server allows it, and otherwise runs without
one, with a warning.

## Development

    $ cmake -B build -DCMAKE_BUILD_TYPE=Debug
    $ cmake --build build
    $ ctest --test-dir build -j8

The suites:

- `unit`: the core on a platform layer with an in-memory file system
  and a fake clock (`src/main_test.cpp`).
- `selftest`: `prospero --selftest`, the real platform layer: threads,
  the file system, and processes. On macOS, `selftest_disclaimed` runs
  it again as its own responsible process, as an app runs.
- `scenarios`: the engine end to end against a fault-injecting fake
  provider (`tests/fake_provider.py`).
- `cleanup`: the harness killed hard while its tools run, which must
  leave no process behind.
- `remote`, `includes`, `deploy`: hosts and their clients, URL includes
  against a fake web server, and deploying through a fake ssh and scp.

The Python suites need [uv][]. They also run against other machines
from this one: `--host` runs Prospero on a Windows host over ssh,
`--xp` on Windows XP, and `--ssh` connects and deploys to a real
remote host. `tests/gui.py` drives the GUI through a scripted
checklist, which fails on a stale frame, and `tests/imports.py` checks
a headless Windows engine's imports against what XP exports (CTest runs
it in those builds). Each script's header says how to run it.

## License

Prospero is free and unencumbered software released into the public
domain. See [UNLICENSE](UNLICENSE).

[uv]: https://docs.astral.sh/uv/
[w64devkit]: https://github.com/skeeto/w64devkit

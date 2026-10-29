# Windows notes

The GUI build's `prospero.exe` is a GUI program, so at an interactive
cmd or PowerShell prompt the prompt returns at once, without waiting
for a command such as `prospero deploy`: its messages still show in
that console, after the prompt, but its exit status is lost. Pipe or
redirect its output to wait for it: `prospero deploy pi | more`, or in
PowerShell `prospero deploy pi | Out-Host`. Over ssh and in scripts it
works as usual, and it serves as the headless engine there too. The headless
build (`-DPROSPERO_GUI=OFF`), which is the XP engine and the engine
deployed to Windows machines, is a console program, and works as usual
at a prompt as well.

The agent's shell is a POSIX `sh` when there is one: `sh.exe` or
busybox-w32's `busybox.exe` next to `prospero.exe`, else on the PATH.
Without either, commands run in PowerShell: PowerShell 7 (`pwsh.exe` on
the PATH), else the Windows PowerShell 5.1 that comes with Windows, and
`cmd.exe` only as a last resort. The shell tool's description tells the
agent which it has. `[tools] shell` picks one instead (`sh`,
`powershell`, `cmd`, or an executable by its path or its name on the
PATH, `[tools] path_prepend` included) for new sessions; a
session keeps the shell it began with. PowerShell takes a second or more
to start, so each command is that much slower than in busybox.
Each tool runs in a job object, so that its whole process tree dies
with it, or with Prospero.

Windows XP runs only the headless engine (the i686 build): `prospero
run`, or a host reached over ssh. Its WinHTTP has no TLS 1.2, so
endpoints must be plain http, as on a local network. Before Windows 8,
a process in a job cannot put its children in jobs of their own, so
under an ssh server that runs sessions in a job, each tool breaks away
into its own job where the server allows it, and otherwise runs without
one, with a warning.

# Desktop control

The desktop tool lets the agent see the screen and use the mouse and
keyboard: screenshots of the screen or of one window, clicks, drags,
scrolling, keys and Unicode text, the list of windows, and starting,
waiting for and closing programs. Every session is offered it where
it could work, where its helper is found and there is a screen (on
Linux, an X11 or Wayland display), and every action, screenshots
included, asks you first, denied if you do not answer in time. Your own
configuration may keep it to sessions with a role that lists `desktop`
in its tools, or turn it off with `offer = none`:

    [desktop]
    offer = roles
    ask = true

    [role gui]
    description = Builds programs and tries them on the screen
    tools = default desktop

A role's `tools` are read left to right: `*` is every tool, `default`
those of a role that names none (every tool but desktop), and `-NAME`
takes one away, as in `* -shell`. The New session dialog's Role starts
a session in a role, as `-r ROLE` does on the command line.

An ask may instead allow its action's class for the rest of the session
and its subagents ("Allow input for this session"): `observe` for
screenshots, the list of windows and waiting; `input` for clicks, the
mouse, scrolling, keys and text, which includes `observe`; and
`windows` for focusing and closing them. Starting a program is asked
about every time. `ask = false` asks about nothing at all.

The work is done by a helper, which the harness starts for a turn and
talks to over pipes. On Windows, and in a Linux build from source, it
is `prospero` itself, run as `prospero desktop`. Elsewhere it is a
program of its own, `prospero-desktop`, next to `prospero`: on macOS
so that the one program the system lets watch the screen keeps its
identity however often Prospero is rebuilt, and beside the static Linux
engines of a release because it loads the X11 and portal libraries,
which a static program cannot. So far it exists for Windows (XP and
later), macOS (15.2 and later) and Linux, on X11 or Wayland. Run
alone, it takes the same commands on its command line (`prospero
desktop shot screen > s.png`, or `prospero-desktop shot screen`;
`list`, `click WINDOW X Y` and so on; `-h`). An engine that `prospero
deploy` put on another machine uses the helper deployed beside it, or
itself ([Remote and background hosts](remote.md)).

On macOS the helper sits in `Prospero.app/Contents/MacOS`, and works in
points, which it captures at the display's full resolution. macOS asks
your permission for it twice, in System Settings > Privacy & Security:
Screen Recording, for screenshots (quit and reopen Prospero after
granting it), and Accessibility, for input and for raising, closing and
moving windows. Both are granted to Prospero.app when you start it as
an app, or to the terminal you start `prospero` from. A call that lacks
one fails at once and says which to grant to what;
`prospero-desktop access` lists them, and `access -ask` asks for those
missing. Build with `-DPROSPERO_SIGN_IDENTITY=NAME`, a code-signing
identity from your keychain, even a self-signed one, so that the
grants survive rebuilds.

Grants also name the bundle identifier, `com.nullprogram.prospero`,
which only a signed build takes. Any other build is signed ad hoc as
`com.nullprogram.prospero.dev` (its helper as `....dev.desktop`), so
that a test build never claims the real app's grants, nor leaves a
requirement on them that refuses the real app. `-DPROSPERO_BUNDLE_ID=ID`
chooses another; the helper's is that plus `.desktop`.

On Linux the helper drives the X11 display that `DISPLAY` names, in
pixels, loading libX11, libXext and libXtst when it runs. It works with
a window manager through EWMH, or without one. A program the tool runs
(a path, a name in `PATH`, or a desktop entry such as
`org.gnome.TextEditor`) is started by your systemd user manager, as a
transient service in `app.slice` with the helper's environment, so that
it outlives the turn and Prospero, as an application you start does.
Without a user manager it ends with Prospero, and the tool says so.
This holds under Wayland too.

In a Wayland session (`WAYLAND_DISPLAY` set) it goes through the
desktop's portal instead, in the compositor's logical pixels: the
RemoteDesktop portal, with a screen cast of the monitors, for
screenshots (read through PipeWire) and input (through libei, or the
portal's own calls without it); each library is loaded when it runs.
The desktop asks once, in a dialog (on GNOME, turn on "Allow Remote
Interaction" and press Share), and the approval is kept: a restore
token in the state directory lets later runs start without asking
(`prospero-desktop access -ask` shows the dialog and waits). Wayland
has no common way for one program to list, raise or close another's
windows, so that depends on the desktop: KDE Plasma's KWin runs small
scripts for it; GNOME needs the [Window Calls][] extension; elsewhere,
or on GNOME without it, only X11 programs (under Xwayland) can be named,
and the rest of the screen is reached through `screen` and `active`.
Wayland gives keys only for the characters of the keyboard's layout, so
text with others is refused before anything is typed.
`PROSPERO_DESKTOP_SESSION=x11` or `wayland` overrides the choice.

[Window Calls]: https://extensions.gnome.org/extension/4724/window-calls/

Screenshots go to the model as images, scaled down to its limits
(`[model] image_edge` and `image_pixels`: by default 1568 pixels on a
side and 1.15 megapixels for kind anthropic, else 1280 and 1 megapixel),
and only the latest few stay in the conversation, images read
included (`[desktop] images`).
Older ones leave in steps, `images_slack` at a time (by default 3, with
up to 4 more before a step), each replaced by a line naming its call,
so that the prompt cache keeps hitting between steps. A model gives points in pixels of the image it saw, or, with
`[model] coordinates = norm1000`, in thousandths of its width and
height, as Qwen-VL and Gemini do; the harness maps them back to the
screen. A model that takes no images is marked `vision = false`.

[Window Calls]: https://extensions.gnome.org/extension/4724/window-calls/

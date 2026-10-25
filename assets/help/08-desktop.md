# Desktop control

The desktop tool lets the agent see the screen and use the mouse and
keyboard: screenshots of the screen or of one window, clicks, drags,
scrolling, keys and text, the list of windows, and starting, waiting
for and closing programs. Screenshots go to the model as images.

Every session is offered it where it could work: where its helper is
found and there is a screen, on Linux an X11 or Wayland display (not,
say, over ssh). `offer = roles` keeps it to sessions whose role lists
`desktop` in its tools, and `offer = none` turns it off:

    [desktop]
    offer = roles

    [role gui]
    description = Builds programs and tries them on the screen
    tools = default desktop

An included file cannot change `[desktop]`.

## Asks

Every action asks you first, screenshots included, and is denied if
you do not answer within `[desktop] ask_timeout` seconds (60 by
default). Instead of Allow, an ask may allow its action's class for
the rest of the session and its subagents, with "Allow CLASS for this
session":

- `observe`: screenshots, the list of windows, and waiting;
- `input`: clicks, the mouse, scrolling, keys and text, which includes
  `observe`;
- `windows`: focusing and closing windows.

Starting a program asks every time. `ask = false` asks about nothing at
all, and so does Bypass mode. In Plan mode the tool may only look. A
session nobody is watching cannot ask, so its desktop actions are
denied.

## The helper

The work is done by a helper, which the harness starts for a turn: on
Windows, and in a Linux build from source, `prospero` itself, and
elsewhere a program of its own, `prospero-desktop`, next to
`prospero`. Run alone, it takes the same commands on its command line
(`prospero desktop -h`, or `prospero-desktop -h`).

- **macOS** (15.2 and later): the helper is in
  `Prospero.app/Contents/MacOS`. macOS asks for two permissions, in
  System Settings → Privacy & Security: Screen Recording, for
  screenshots (quit and reopen Prospero after granting it), and
  Accessibility, for input and for windows. `prospero-desktop access`
  lists what is missing, and `access -ask` asks for it.
- **Windows**: XP and later, with nothing else to install.
- **Linux on X11**: the display that `DISPLAY` names.
- **Linux on Wayland**: through the desktop's portal, which asks once
  in a dialog, and remembers the answer. Listing, raising and closing
  other programs' windows depends on the desktop: KDE Plasma does it,
  GNOME needs the Window Calls extension. `PROSPERO_DESKTOP_SESSION=x11`
  or `wayland` overrides which is used.

A session on a remote host uses the helper deployed beside its engine
there.

Only the latest few screenshots, and image files the agent read, stay
in the conversation (`[desktop] images`, 3 by default). A model that takes no images is marked
`vision = false` in its `[model]` section.

Prospero engines
================

Headless Prospero engines for other machines, which Prospero puts there
over ssh and runs ("prospero deploy", or Deploy engine in the Hosts
pane). Put this zip's engines directory in either place:

  beside prospero.ini, in Prospero's configuration directory (Settings
  shows the file): ~/.config/prospero on Linux and macOS, wherever
  Prospero.app is installed

  next to the Prospero program itself, as when you unpack this zip next
  to Prospero.exe (this place comes first)

The engines' version must match the program's.

engines/
  prospero-engine-VERSION-linux-x86_64        Linux, static
  prospero-engine-VERSION-linux-aarch64
  prospero-desktop-VERSION-linux-x86_64       Linux desktop helpers (Desktop
  prospero-desktop-VERSION-linux-aarch64      control), glibc @GLIBC@ or later
  prospero-engine-VERSION-windows-x86_64.exe  64-bit Windows, x86-64 (and
                                              Arm, under emulation)
  prospero-engine-VERSION-windows-i686.exe    Windows XP and later, x86
  prospero-engine-VERSION-macos-arm64         macOS 11 and later, Apple
                                              Silicon
  *.sha256                                    each one's SHA-256, which
                                              deploy checks

A Windows engine is its own desktop helper. A macOS engine gets none:
Desktop control there needs privacy permissions that belong to
Prospero.app. An engine started over ssh on macOS also can't read
~/Desktop, ~/Documents or ~/Downloads unless Remote Login has full disk
access (System Settings, General, Sharing, Remote Login).

LICENSES/ holds the licenses of everything linked into these programs.

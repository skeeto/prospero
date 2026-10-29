# Building

CMake 3.25 or later and a C++20 compiler with `#embed`, on macOS, Windows
([w64devkit][]), or Linux:

    $ cmake -B build
    $ cmake --build build

Dependencies are fetched at configure time, pinned by URL and SHA-256:
Dear ImGui (docking branch), md4c, FreeType and SDL3 for the GUI, mbedTLS
with cpp-httplib for HTTP on macOS and Linux, and Wuffs's single-file
release for decoding images everywhere. To build offline, or with a
modified dependency, put its source under `deps/` (`deps/imgui`,
`deps/md4c`, `deps/freetype`, `deps/sdl3`, `deps/mbedtls`,
`deps/cpp-httplib`, and `deps/wuffs` holding `wuffs-v0.4.c`).

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
build directory. There Prospero asks the system whether to trust each
server's certificate, as Safari does, so a root that a user or an
administrator added, such as a corporate proxy's, is trusted. On Linux,
SDL3 loads X11, Wayland and GL at run time, so the GUI build still runs
headless where no display libraries exist. With `-DPROSPERO_GUI=OFF`,
`-DPROSPERO_STATIC=ON` links the headless engine fully statically, as
`tools/engines.py` does for the Linux engines it builds.

The GUI's developer tools, the Metrics pane for now, are in Debug
builds only, unless `-DPROSPERO_DEVTOOLS=ON` adds them to another, such
as one for measuring performance.

A source build is the Community Edition, as the GitHub releases are:
its window title says so, and an empty transcript asks those who use
Prospero professionally to buy the [itch.io][itch] build. That build,
`-DPROSPERO_EDITION=itch`, is plain Prospero, and needs
`-DPROSPERO_ENGINES_ZIP=PATH`, the engines zip (below), which it
carries built in, so that deploy needs no engines directory.

## Releases

Each release on GitHub carries six files:

- `prospero-VERSION.tar.gz`: the source, with every dependency unpacked
  under `deps/`, so that it builds with no network:

      $ tar xzf prospero-VERSION.tar.gz
      $ cmake -S prospero-VERSION -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      $ cmake --build build

- `prospero-VERSION-windows-x86_64.zip`: the Windows program,
  `Prospero.exe`, the GUI with its engine and desktop helper, all there
  is to install. Unpack it over an older `Prospero.exe`, of either
  edition, to upgrade.
- `prospero-VERSION-macos-arm64.dmg`: the macOS app, for Apple
  silicon and macOS 13 or later, signed and notarized. Drag
  `Prospero.app` to `/Applications`.
- `prospero-VERSION-linux-x86_64.tar.gz` and
  `prospero-VERSION-linux-aarch64.tar.gz`: the Linux program, the same,
  in a directory with a `.desktop` file, its icon, and a README.txt. It
  is built in an Ubuntu 22.04 container, with everything but glibc
  linked statically, so it needs glibc 2.34 or later (Ubuntu 22.04,
  Debian 12, Fedora 35, RHEL 10), and shows its window through Wayland
  or X11, which it loads at run time.
- `prospero-engines-VERSION.zip`: headless engines for other machines,
  which `prospero deploy` puts there (see below): Windows x86-64 and
  i686 (for XP), each its own desktop helper; Linux x86-64 and aarch64,
  linked statically, with their desktop helpers ([Desktop
  control](desktop.md)), which link the C library dynamically, since
  they load X11 at run time; and macOS arm64, signed. Its `engines`
  directory goes next to the program, or beside `prospero.ini` (see
  Settings), which suits a macOS app in /Applications. Its README.txt
  lists the files and what each needs.

The engines and helpers are named as `prospero deploy` expects to find
them, and it puts each helper beside its engine on the remote machine.
To use a helper on the machine itself, save it next to `prospero` as
`prospero-desktop`.

Windows builds are made on Linux by [w64devkit][] in Wine, and signed
with Azure Artifact Signing before the engines are packed, by
`tools/engines.py --pack`. The macOS app is built and tested on a
macOS runner, then signed with a Developer ID certificate and notarized
by Apple.

### The engines zip

`tools/engines.py --docker --zip` builds every engine and helper into
one file, `prospero-engines-VERSION.zip`, with Docker, on any machine
it runs on: the Linux and Windows programs in an image of cross
compilers that `tools/toolchain/Dockerfile` builds from sources pinned
by SHA-256 (GCC 16 and binutils as [w64devkit][] has them, musl with
its security patches, mingw-w64, and Debian 10's glibc 2.28), and the
macOS engine on the Mac itself. All are optimized for size:

- the Linux engines link musl statically, so they run with any C
  library, or none;
- the Linux desktop helpers need glibc 2.28 or later;
- the Windows engines are the same CRT-free programs as above;
- the macOS engine, for Apple Silicon, needs macOS 11 or later.

The same sources build the same zip, byte for byte, laid out as a
release's.

### itch.io builds

`tools/itch.py` builds the [itch.io][itch] edition, for macOS and
Windows, on a Mac with Docker, from a source tarball of a commit: the
engines as above, the Windows ones signed with `aas-sign` and the macOS
one with the Developer ID, packed, then embedded in `Prospero.app`
(signed, tested, on a notarized disk image) and in the Windows program
(built in the toolchain image, signed with `aas-sign`, as `Prospero.exe`
in a zip). Without
`aas-sign`, the Developer ID in the keychain, or notary credentials
(`--notary-profile`, from `xcrun notarytool store-credentials`), it
builds nothing. `--push` uploads the two with [butler][], which needs
an itch.io API key, from `butler login` or in `BUTLER_API_KEY`; without
one, it builds nothing either.

[w64devkit]: https://github.com/skeeto/w64devkit
[itch]: https://skeeto.itch.io/prospero
[butler]: https://itch.io/docs/butler/

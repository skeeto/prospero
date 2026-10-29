Prospero for Linux
==================

Run ./prospero. Nothing needs installing: it needs glibc 2.34 or later
(Ubuntu 22.04, Debian 12, Fedora 35, RHEL 10, or later), and it shows
its window through Wayland or X11, with OpenGL, which a desktop has.

For a menu entry, copy prospero.desktop to ~/.local/share/applications,
and set its Exec to this prospero's full path, and its Icon to
prospero.png's.

Desktop control is built in. For engines to deploy on other machines,
unpack prospero-engines-VERSION.zip from the same release here, next to
prospero, so that its engines directory sits beside it.

LICENSES/ holds the licenses of everything linked into this program.
Documentation: https://github.com/skeeto/prospero

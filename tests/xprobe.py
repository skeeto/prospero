#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""X11 fixtures for the desktop helper's tests (scenarios.py desktop_x11),
through libX11 with ctypes, so that nothing needs installing beyond the
X client library and a server such as Xvfb.

  xprobe.py window LOG [TITLE]
                         a window, "Prospero probe" or TITLE, that draws
                         what is typed into it and appends each key,
                         button and close request to LOG as a JSON line;
                         it exits when asked to close (WM_DELETE_WINDOW)
  xprobe.py wm LOG       a stand-in window manager that speaks just
                         enough EWMH for the helper: the client lists,
                         _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW and
                         _NET_MOVERESIZE_WINDOW, with frame extents of
                         FRAME (left, right, top, bottom), though it
                         draws no frames; it logs what it is asked, and
                         never activates a window whose title starts
                         with "Stubborn", as a manager may refuse
  xprobe.py keys         the keycodes that have keysyms, as JSON

DISPLAY names the display.
"""

import ctypes
import json
import os
import sys
from ctypes import (POINTER, Structure, Union, byref, c_char_p, c_int, c_long,
                    c_ubyte, c_uint, c_ulong, c_void_p)

FRAME = (2, 2, 20, 2)

KeyPress, KeyRelease, ButtonPress, ButtonRelease = 2, 3, 4, 5
Expose, DestroyNotify, UnmapNotify, MapNotify = 12, 17, 18, 19
ClientMessage, MappingNotify = 33, 34
XA_ATOM, XA_CARDINAL, XA_STRING, XA_WINDOW = 4, 6, 31, 33


class XKeyEvent(Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("window", c_ulong), ("root", c_ulong),
                ("subwindow", c_ulong), ("time", c_ulong), ("x", c_int),
                ("y", c_int), ("x_root", c_int), ("y_root", c_int),
                ("state", c_uint), ("keycode", c_uint), ("same_screen", c_int)]


class XButtonEvent(Structure):
    _fields_ = XKeyEvent._fields_[:-2] + [("button", c_uint), ("same_screen", c_int)]


class XClientMessageEvent(Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("window", c_ulong),
                ("message_type", c_ulong), ("format", c_int), ("l", c_long * 5)]


class XStructEvent(Structure):  # Map, Unmap, Destroy: the event window, then the window
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int),
                ("display", c_void_p), ("event", c_ulong), ("window", c_ulong),
                ("override_redirect", c_int)]


class XEvent(Union):
    _fields_ = [("type", c_int), ("xkey", XKeyEvent), ("xbutton", XButtonEvent),
                ("xclient", XClientMessageEvent), ("xstruct", XStructEvent),
                ("pad", c_long * 24)]


class XWindowAttributes(Structure):
    _fields_ = [("x", c_int), ("y", c_int), ("width", c_int), ("height", c_int),
                ("border_width", c_int), ("depth", c_int), ("visual", c_void_p),
                ("root", c_ulong), ("cls", c_int), ("bit_gravity", c_int),
                ("win_gravity", c_int), ("backing_store", c_int),
                ("backing_planes", c_ulong), ("backing_pixel", c_ulong),
                ("save_under", c_int), ("colormap", c_ulong),
                ("map_installed", c_int), ("map_state", c_int),
                ("all_event_masks", c_long), ("your_event_mask", c_long),
                ("do_not_propagate_mask", c_long), ("override_redirect", c_int),
                ("screen", c_void_p)]


def xlib():
    x = ctypes.CDLL("libX11.so.6")
    x.XOpenDisplay.restype = c_void_p
    x.XOpenDisplay.argtypes = [c_char_p]
    sigs = {
        "XDefaultScreen": (c_int, [c_void_p]),
        "XRootWindow": (c_ulong, [c_void_p, c_int]),
        "XBlackPixel": (c_ulong, [c_void_p, c_int]),
        "XWhitePixel": (c_ulong, [c_void_p, c_int]),
        "XDefaultGC": (c_void_p, [c_void_p, c_int]),
        "XCreateSimpleWindow": (c_ulong, [c_void_p, c_ulong, c_int, c_int, c_uint,
                                          c_uint, c_uint, c_ulong, c_ulong]),
        "XInternAtom": (c_ulong, [c_void_p, c_char_p, c_int]),
        "XChangeProperty": (c_int, [c_void_p, c_ulong, c_ulong, c_ulong, c_int,
                                    c_int, c_void_p, c_int]),
        "XDeleteProperty": (c_int, [c_void_p, c_ulong, c_ulong]),
        "XSelectInput": (c_int, [c_void_p, c_ulong, c_long]),
        "XSetWMProtocols": (c_int, [c_void_p, c_ulong, POINTER(c_ulong), c_int]),
        "XMapWindow": (c_int, [c_void_p, c_ulong]),
        "XNextEvent": (c_int, [c_void_p, POINTER(XEvent)]),
        "XFlush": (c_int, [c_void_p]),
        "XSync": (c_int, [c_void_p, c_int]),
        "XLookupString": (c_int, [POINTER(XKeyEvent), c_char_p, c_int,
                                  POINTER(c_ulong), c_void_p]),
        "XRefreshKeyboardMapping": (c_int, [c_void_p]),
        "XClearWindow": (c_int, [c_void_p, c_ulong]),
        "XDrawString": (c_int, [c_void_p, c_ulong, c_void_p, c_int, c_int,
                                c_char_p, c_int]),
        "XFillRectangle": (c_int, [c_void_p, c_ulong, c_void_p, c_int, c_int,
                                   c_uint, c_uint]),
        "XSendEvent": (c_int, [c_void_p, c_ulong, c_int, c_long, POINTER(XEvent)]),
        "XRaiseWindow": (c_int, [c_void_p, c_ulong]),
        "XSetInputFocus": (c_int, [c_void_p, c_ulong, c_int, c_ulong]),
        "XMoveResizeWindow": (c_int, [c_void_p, c_ulong, c_int, c_int, c_uint, c_uint]),
        "XMoveWindow": (c_int, [c_void_p, c_ulong, c_int, c_int]),
        "XGetWindowAttributes": (c_int, [c_void_p, c_ulong, POINTER(XWindowAttributes)]),
        "XDisplayKeycodes": (c_int, [c_void_p, POINTER(c_int), POINTER(c_int)]),
        "XGetKeyboardMapping": (POINTER(c_ulong), [c_void_p, c_ubyte, c_int,
                                                   POINTER(c_int)]),
        "XFree": (c_int, [c_void_p]),
        "XFetchName": (c_int, [c_void_p, c_ulong, POINTER(c_char_p)]),
        "XSetErrorHandler": (c_void_p, [c_void_p]),
    }
    for name, (res, args) in sigs.items():
        f = getattr(x, name)
        f.restype = res
        f.argtypes = args
    return x


X = xlib()
ERRHANDLER = ctypes.CFUNCTYPE(c_int, c_void_p, c_void_p)(lambda d, e: 0)


def connect():
    dpy = X.XOpenDisplay(None)
    if not dpy:
        sys.exit(f"xprobe: cannot open display {os.environ.get('DISPLAY')!r}")
    X.XSetErrorHandler(ERRHANDLER)  # a window gone meanwhile is no reason to die
    scr = X.XDefaultScreen(dpy)
    return dpy, scr, X.XRootWindow(dpy, scr)


def atom(dpy, name):
    return X.XInternAtom(dpy, name.encode(), 0)


def setlongs(dpy, w, prop, type_, values):
    arr = (c_ulong * max(len(values), 1))(*values)
    X.XChangeProperty(dpy, w, prop, type_, 32, 0, arr, len(values))


def logger(path):
    def log(**kw):
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(kw, ensure_ascii=False) + "\n")
    return log


def keysym_char(sym):
    """The character a keysym types, as clients see it, or None."""
    if sym in (0xff0d, 0xff8d):
        return "\n"
    if sym == 0xff09:
        return "\t"
    if 0x20 <= sym <= 0x7e or 0xa0 <= sym <= 0xff:
        return chr(sym)
    if sym & 0xff000000 == 0x01000000:
        return chr(sym & 0xffffff)
    return None


def window(path, name="Prospero probe"):
    log = logger(path)
    dpy, scr, root = connect()
    w = X.XCreateSimpleWindow(dpy, root, 40, 40, 480, 200, 0,
                              X.XBlackPixel(dpy, scr), X.XWhitePixel(dpy, scr))
    title = name.encode()
    utf8 = atom(dpy, "UTF8_STRING")
    X.XChangeProperty(dpy, w, atom(dpy, "_NET_WM_NAME"), utf8, 8, 0, title, len(title))
    X.XChangeProperty(dpy, w, 39, XA_STRING, 8, 0, title, len(title))  # WM_NAME
    cls = b"xprobe\0XProbe\0"
    X.XChangeProperty(dpy, w, 67, XA_STRING, 8, 0, cls, len(cls))  # WM_CLASS
    setlongs(dpy, w, atom(dpy, "_NET_WM_PID"), XA_CARDINAL, [os.getpid()])
    protocols = atom(dpy, "WM_PROTOCOLS")
    delete = atom(dpy, "WM_DELETE_WINDOW")
    X.XSetWMProtocols(dpy, w, byref(c_ulong(delete)), 1)
    mask = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 15) | (1 << 17)  # keys, buttons, expose, structure
    X.XSelectInput(dpy, w, mask)
    X.XMapWindow(dpy, w)
    X.XFlush(dpy)
    gc = X.XDefaultGC(dpy, scr)
    text = ""

    def draw():
        X.XClearWindow(dpy, w)
        y = 20
        for line in text.split("\n"):
            data = line.encode("latin-1", "replace")
            X.XDrawString(dpy, w, gc, 10, y, data, len(data))
            y += 16
        X.XFlush(dpy)

    ev = XEvent()
    buf = ctypes.create_string_buffer(32)
    while True:
        X.XNextEvent(dpy, byref(ev))
        if ev.type == KeyPress:
            sym = c_ulong()
            X.XLookupString(byref(ev.xkey), buf, 32, byref(sym), None)
            ch = keysym_char(sym.value)
            log(key=sym.value, keycode=ev.xkey.keycode, state=ev.xkey.state, char=ch)
            if ch is not None:
                text += ch
                log(text=text)
                draw()
        elif ev.type in (ButtonPress, ButtonRelease):
            b = ev.xbutton
            log(button=b.button, press=ev.type == ButtonPress, x=b.x, y=b.y)
        elif ev.type == MappingNotify:
            X.XRefreshKeyboardMapping(byref(ev))
        elif ev.type == Expose:
            draw()
        elif ev.type == ClientMessage:
            c = ev.xclient
            if c.message_type == protocols and c.l[0] == delete:
                log(close=True, text=text)
                return


def wm(path):
    """Answers the EWMH messages the helper sends, and keeps the lists."""
    log = logger(path)
    dpy, scr, root = connect()
    a = {n: atom(dpy, n) for n in (
        "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST",
        "_NET_CLIENT_LIST_STACKING", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW",
        "_NET_MOVERESIZE_WINDOW", "_NET_FRAME_EXTENTS", "_NET_WM_NAME",
        "UTF8_STRING", "WM_STATE", "WM_PROTOCOLS", "WM_DELETE_WINDOW")}
    check = X.XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0)
    for w in (root, check):
        setlongs(dpy, w, a["_NET_SUPPORTING_WM_CHECK"], XA_WINDOW, [check])
    name = b"xprobe-wm"
    X.XChangeProperty(dpy, check, a["_NET_WM_NAME"], a["UTF8_STRING"], 8, 0, name, len(name))
    setlongs(dpy, root, a["_NET_SUPPORTED"], XA_ATOM, [a[n] for n in (
        "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST",
        "_NET_CLIENT_LIST_STACKING", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW",
        "_NET_MOVERESIZE_WINDOW", "_NET_FRAME_EXTENTS")])
    X.XSelectInput(dpy, root, 1 << 19)  # SubstructureNotify
    clients = []  # bottom first

    def stubborn(w):
        name = c_char_p()
        if not X.XFetchName(dpy, w, byref(name)) or not name.value:
            return False
        refuse = name.value.startswith(b"Stubborn")
        X.XFree(name)
        return refuse

    def publish(active=None):
        setlongs(dpy, root, a["_NET_CLIENT_LIST"], XA_WINDOW, sorted(clients))
        setlongs(dpy, root, a["_NET_CLIENT_LIST_STACKING"], XA_WINDOW, clients)
        if active is not None:
            setlongs(dpy, root, a["_NET_ACTIVE_WINDOW"], XA_WINDOW, [active])
        X.XFlush(dpy)

    publish(0)
    X.XSync(dpy, 0)
    log(ready=True)
    ev = XEvent()
    attrs = XWindowAttributes()
    while True:
        X.XNextEvent(dpy, byref(ev))
        if ev.type == MapNotify:
            w = ev.xstruct.window
            if ev.xstruct.override_redirect or w == check or w in clients:
                continue
            clients.append(w)
            setlongs(dpy, w, a["WM_STATE"], a["WM_STATE"], [1, 0])  # NormalState
            setlongs(dpy, w, a["_NET_FRAME_EXTENTS"], XA_CARDINAL, list(FRAME))
            publish()
            log(manage=w)
        elif ev.type in (UnmapNotify, DestroyNotify):
            w = ev.xstruct.window
            if w in clients:
                clients.remove(w)
                publish(0)
                log(unmanage=w)
        elif ev.type == ClientMessage:
            c = ev.xclient
            w = c.window
            if c.message_type == a["_NET_ACTIVE_WINDOW"] and w in clients and stubborn(w):
                log(refused=w)
            elif c.message_type == a["_NET_ACTIVE_WINDOW"] and w in clients:
                clients.remove(w)
                clients.append(w)
                X.XRaiseWindow(dpy, w)
                X.XSetInputFocus(dpy, w, 2, 0)  # RevertToParent
                publish(w)
                log(active=w, source=c.l[0])
            elif c.message_type == a["_NET_CLOSE_WINDOW"] and w in clients:
                d = XEvent()
                d.xclient.type = ClientMessage
                d.xclient.window = w
                d.xclient.message_type = a["WM_PROTOCOLS"]
                d.xclient.format = 32
                d.xclient.l[0] = a["WM_DELETE_WINDOW"]
                X.XSendEvent(dpy, w, 0, 0, byref(d))
                X.XFlush(dpy)
                log(close=w, source=c.l[1])
            elif c.message_type == a["_NET_MOVERESIZE_WINDOW"] and w in clients:
                flags = c.l[0]
                X.XGetWindowAttributes(dpy, w, byref(attrs))
                x = c.l[1] if flags & (1 << 8) else attrs.x
                y = c.l[2] if flags & (1 << 9) else attrs.y
                cw = c.l[3] if flags & (1 << 10) else attrs.width
                ch = c.l[4] if flags & (1 << 11) else attrs.height
                X.XMoveResizeWindow(dpy, w, x, y, cw, ch)
                X.XFlush(dpy)
                log(move=w, gravity=flags & 0xff, rect=[x, y, cw, ch])


def keys():
    dpy, _, _ = connect()
    lo, hi, per = c_int(), c_int(), c_int()
    X.XDisplayKeycodes(dpy, byref(lo), byref(hi))
    syms = X.XGetKeyboardMapping(dpy, lo.value, hi.value - lo.value + 1, byref(per))
    out = {}
    for k in range(lo.value, hi.value + 1):
        row = [syms[(k - lo.value) * per.value + i] for i in range(per.value)]
        if any(row):
            out[k] = row
    X.XFree(syms)
    print(json.dumps(out))


def main():
    if len(sys.argv) in (3, 4) and sys.argv[1] == "window":
        window(*sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == "wm":
        wm(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == "keys":
        keys()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

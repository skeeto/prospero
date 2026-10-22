#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""What an application shows through Accessibility, as JSON, for the
live desktop test (tests/desktop_live_macos.py) to check results by
itself rather than trust the helper's replies.

  axprobe.py PID

Prints one JSON object: whether this process is trusted for
Accessibility, the frontmost application's pid, and for each of PID's
windows its title, whether it is main and focused, the values of its
text areas, and its sheets' buttons. It only reads: it sets no
attribute and performs no action. Accessibility is charged to the
responsible process, so the live test runs this under Prospero.app's
reaper, as it runs the helper, where Prospero.app's grant applies.
"""

import ctypes
import json
import sys

UTF8 = 0x08000100  # kCFStringEncodingUTF8
DEPTH = 8          # how deep to look for text areas
WIDTH = 200        # children looked at per element

_ax = _cf = None


def lib():
    global _ax, _cf
    if _ax is None:
        _ax = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/"
                                      "ApplicationServices.framework/ApplicationServices")
        _cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/"
                                      "CoreFoundation.framework/CoreFoundation")
        p = ctypes.c_void_p
        for name, res, args in (
                ("AXIsProcessTrusted", ctypes.c_bool, []),
                ("AXUIElementCreateApplication", p, [ctypes.c_int]),
                ("AXUIElementCreateSystemWide", p, []),
                ("AXUIElementCopyAttributeValue", ctypes.c_int32, [p, p, ctypes.POINTER(p)]),
                ("AXUIElementGetPid", ctypes.c_int32, [p, ctypes.POINTER(ctypes.c_int)]),
                ("AXUIElementSetMessagingTimeout", ctypes.c_int32, [p, ctypes.c_float])):
            f = getattr(_ax, name)
            f.restype, f.argtypes = res, args
        for name, res, args in (
                ("CFStringCreateWithCString", p, [p, ctypes.c_char_p, ctypes.c_uint32]),
                ("CFStringGetLength", ctypes.c_long, [p]),
                ("CFStringGetMaximumSizeForEncoding", ctypes.c_long, [ctypes.c_long, ctypes.c_uint32]),
                ("CFStringGetCString", ctypes.c_bool, [p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]),
                ("CFGetTypeID", ctypes.c_ulong, [p]),
                ("CFStringGetTypeID", ctypes.c_ulong, []),
                ("CFArrayGetTypeID", ctypes.c_ulong, []),
                ("CFBooleanGetTypeID", ctypes.c_ulong, []),
                ("CFBooleanGetValue", ctypes.c_bool, [p]),
                ("CFArrayGetCount", ctypes.c_long, [p]),
                ("CFArrayGetValueAtIndex", p, [p, ctypes.c_long]),
                ("CFEqual", ctypes.c_bool, [p, p])):
            f = getattr(_cf, name)
            f.restype, f.argtypes = res, args
    return _ax, _cf


# A one-shot process: what Core Foundation returns is never released
_names = {}


def cfname(s):
    if s not in _names:
        _names[s] = lib()[1].CFStringCreateWithCString(None, s.encode(), UTF8)
    return _names[s]


def attr(e, name):
    """An attribute's value: a str, bool, list of elements, an element
    (a pointer), or None if it cannot be read."""
    ax, cf = lib()
    v = ctypes.c_void_p()
    if ax.AXUIElementCopyAttributeValue(e, cfname(name), ctypes.byref(v)) or not v.value:
        return None
    v = v.value
    t = cf.CFGetTypeID(v)
    if t == cf.CFStringGetTypeID():
        n = cf.CFStringGetMaximumSizeForEncoding(cf.CFStringGetLength(v), UTF8) + 1
        buf = ctypes.create_string_buffer(n)
        return buf.value.decode("utf-8", "replace") if cf.CFStringGetCString(v, buf, n, UTF8) else None
    if t == cf.CFBooleanGetTypeID():
        return bool(cf.CFBooleanGetValue(v))
    if t == cf.CFArrayGetTypeID():
        n = min(cf.CFArrayGetCount(v), WIDTH)
        return [cf.CFArrayGetValueAtIndex(v, i) for i in range(n)]
    return v


def texts(e, depth=0):
    """The values of the text areas under an element."""
    if attr(e, "AXRole") == "AXTextArea":
        v = attr(e, "AXValue")
        return [v if isinstance(v, str) else ""]
    found = []
    kids = attr(e, "AXChildren")
    if depth < DEPTH and isinstance(kids, list):
        for k in kids:
            if attr(k, "AXRole") != "AXSheet":
                found += texts(k, depth + 1)
    return found


def buttons(e, depth=0):
    found = []
    kids = attr(e, "AXChildren")
    if depth < DEPTH and isinstance(kids, list):
        for k in kids:
            if attr(k, "AXRole") == "AXButton":
                found.append(attr(k, "AXTitle") or "")
            else:
                found += buttons(k, depth + 1)
    return found


def probe(pid):
    ax, cf = lib()
    r = {"trusted": bool(ax.AXIsProcessTrusted()), "frontmost": 0, "windows": []}
    if not r["trusted"]:
        return r
    sys_ = ax.AXUIElementCreateSystemWide()
    ax.AXUIElementSetMessagingTimeout(sys_, 2.0)
    front = attr(sys_, "AXFocusedApplication")
    if isinstance(front, int):
        p = ctypes.c_int()
        if not ax.AXUIElementGetPid(front, ctypes.byref(p)):
            r["frontmost"] = p.value
    app = ax.AXUIElementCreateApplication(pid)
    focused = attr(app, "AXFocusedWindow")
    wins = attr(app, "AXWindows")
    for w in wins if isinstance(wins, list) else []:
        kids = attr(w, "AXChildren")
        sheets = [k for k in kids if attr(k, "AXRole") == "AXSheet"] \
            if isinstance(kids, list) else []
        r["windows"].append({
            "title": attr(w, "AXTitle"),
            "main": attr(w, "AXMain"),
            "focused": attr(w, "AXFocused"),
            "focused_window": isinstance(focused, int) and bool(cf.CFEqual(w, focused)),
            "texts": texts(w),
            "sheets": [buttons(s) for s in sheets],
        })
    return r


def main():
    if len(sys.argv) != 2 or not sys.argv[1].isdigit():
        print(__doc__.splitlines()[0], file=sys.stderr)
        return 2
    print(json.dumps(probe(int(sys.argv[1]))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

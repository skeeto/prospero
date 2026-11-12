#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Prospero's window as another process sees it through Accessibility,
as Voice Control does: the GUI checklist's outside check (gui.py --mode
a11y, on macOS), which runs it once its prompt, focused, holds TEXT.

  axapp.py [--app Prospero.app] PID TEXT

It runs itself (--probe) under Prospero.app's reaper, as
tests/desktop_live_macos.py runs tests/axprobe.py, so that macOS charges
its Accessibility to Prospero.app, where it is granted, and checks that
the probe answers to that reaper before letting it ask anything. The
probe then, of the GUI with pid PID:

1. dumps its windows' tree of elements (role, subrole, name), as JSON;
2. finds the focused element, by AXFocused, and requires the prompt:
   an AXTextArea named Prompt, whose AXValue is TEXT;
3. finds the AXButton named Send, requires AXPress among its actions,
   and performs it;
4. requires the prompt to empty within five seconds, as the press sent
   it: so the press reached the app;
5. edits the prompt as Voice Control does, in UTF-16 units, past an
   emoji (two units) and CJK (one each): sets AXValue, then
   AXSelectedTextRange and AXSelectedText, reading AXValue back at once
   each time; reads AXStringForRange, AXBoundsForRange and
   AXRangeForPosition back through the screen; then undoes once, by the
   prompt's context menu (AXShowMenu, then Undo's AXPress), which must
   take back the last edit alone; the menu, as AppKit's, is told
   opened (AXMenuOpened) and closed (AXMenuClosed) to an observer;
6. works a menu of the window's menu bar as Voice Control does, by
   what it is told: presses the AXMenuBarItem Edit, waits for
   AXMenuOpened, requires that menu to be an AXMenu under Edit, which
   is AXSelected, its items where it is, then presses Select all by
   name, waits for AXMenuClosed, and opens Edit again to find Copy,
   disabled before the press, enabled now, as the press selected the
   transcript; then closes it.

The application's own AXFocusedUIElement is only reported: the
checklist's window is never key, which that asks for.

Without Prospero.app, or its Accessibility grant, it says so and exits
2 (skipped); 1 if a check failed, 0 if all passed. It acts on nothing but
the prompt, Send, the prompt's Undo and the Edit menu's Select all, and
touches no other application.

  axapp.py --probe PID TEXT

The probe alone: it prints "pid N" and asks nothing until it reads "go".
"""

import argparse
import ctypes
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
UTF8 = 0x08000100  # kCFStringEncodingUTF8
DEPTH = 24         # how deep the dump goes
MAXNODES = 4000    # and how many elements it takes in

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
                ("AXUIElementCopyAttributeValue", ctypes.c_int32, [p, p, ctypes.POINTER(p)]),
                ("AXUIElementCopyActionNames", ctypes.c_int32, [p, ctypes.POINTER(p)]),
                ("AXUIElementPerformAction", ctypes.c_int32, [p, p]),
                ("AXUIElementSetMessagingTimeout", ctypes.c_int32, [p, ctypes.c_float]),
                ("AXUIElementSetAttributeValue", ctypes.c_int32, [p, p, p]),
                ("AXUIElementIsAttributeSettable", ctypes.c_int32,
                 [p, p, ctypes.POINTER(ctypes.c_bool)]),
                ("AXUIElementCopyParameterizedAttributeValue", ctypes.c_int32,
                 [p, p, p, ctypes.POINTER(p)]),
                ("AXObserverCreate", ctypes.c_int32, [ctypes.c_int, p, ctypes.POINTER(p)]),
                ("AXObserverAddNotification", ctypes.c_int32, [p, p, p, p]),
                ("AXObserverGetRunLoopSource", p, [p]),
                ("AXValueCreate", p, [ctypes.c_uint32, p]),
                ("AXValueGetType", ctypes.c_uint32, [p]),
                ("AXValueGetTypeID", ctypes.c_ulong, []),
                ("AXValueGetValue", ctypes.c_bool, [p, ctypes.c_uint32, p])):
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
                ("CFNumberGetTypeID", ctypes.c_ulong, []),
                ("CFNumberGetValue", ctypes.c_bool, [p, ctypes.c_int, p]),
                ("CFArrayGetCount", ctypes.c_long, [p]),
                ("CFArrayGetValueAtIndex", p, [p, ctypes.c_long]),
                ("CFEqual", ctypes.c_bool, [p, p]),
                ("CFRetain", p, [p]),
                ("CFRunLoopGetCurrent", p, []),
                ("CFRunLoopAddSource", None, [p, p, p]),
                ("CFRunLoopRunInMode", ctypes.c_int32, [p, ctypes.c_double, ctypes.c_bool])):
            f = getattr(_cf, name)
            f.restype, f.argtypes = res, args
    return _ax, _cf


# A one-shot process: what Core Foundation returns is never released
_names = {}


def cfname(s):
    if s not in _names:
        _names[s] = lib()[1].CFStringCreateWithCString(None, s.encode(), UTF8)
    return _names[s]


class CFRange(ctypes.Structure):
    _fields_ = [("loc", ctypes.c_long), ("len", ctypes.c_long)]


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class CGSize(ctypes.Structure):
    _fields_ = [("w", ctypes.c_double), ("h", ctypes.c_double)]


class CGRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("w", ctypes.c_double), ("h", ctypes.c_double)]


AXV_POINT, AXV_SIZE, AXV_RECT, AXV_RANGE = 1, 2, 3, 4  # kAXValueType*
AXV_STRUCTS = {AXV_POINT: CGPoint, AXV_SIZE: CGSize, AXV_RECT: CGRect, AXV_RANGE: CFRange}


def axvalue(kind, *fields):
    """An AXValue of a range (loc, len), point (x, y) or rect."""
    st = AXV_STRUCTS[kind](*fields)
    return lib()[0].AXValueCreate(kind, ctypes.byref(st))


def cfvalue(v):
    """A str, bool, int, list of elements, a tuple of a range, point or
    rect, or the element (a pointer)."""
    ax, cf = lib()
    t = cf.CFGetTypeID(v)
    if t == ax.AXValueGetTypeID():
        kind = ax.AXValueGetType(v)
        st = AXV_STRUCTS.get(kind)
        if st is None:
            return v
        out = st()
        ax.AXValueGetValue(v, kind, ctypes.byref(out))
        return tuple(getattr(out, f) for f, _ in st._fields_)
    if t == cf.CFStringGetTypeID():
        n = cf.CFStringGetMaximumSizeForEncoding(cf.CFStringGetLength(v), UTF8) + 1
        buf = ctypes.create_string_buffer(n)
        return buf.value.decode("utf-8", "replace") if cf.CFStringGetCString(v, buf, n, UTF8) else None
    if t == cf.CFBooleanGetTypeID():
        return bool(cf.CFBooleanGetValue(v))
    if t == cf.CFNumberGetTypeID():
        n = ctypes.c_longlong()
        return n.value if cf.CFNumberGetValue(v, 4, ctypes.byref(n)) else None  # SInt64
    if t == cf.CFArrayGetTypeID():
        return [cf.CFArrayGetValueAtIndex(v, i) for i in range(cf.CFArrayGetCount(v))]
    return v


def attr(e, name, err=None):
    """An attribute's value, or None, its AXError then appended to err."""
    ax, _ = lib()
    v = ctypes.c_void_p()
    rc = ax.AXUIElementCopyAttributeValue(e, cfname(name), ctypes.byref(v))
    if rc or not v.value:
        if err is not None:
            err.append(rc)
        return None
    return cfvalue(v.value)


def param(e, name, arg, err=None):
    """A parameterized attribute's value, or None."""
    ax, _ = lib()
    v = ctypes.c_void_p()
    rc = ax.AXUIElementCopyParameterizedAttributeValue(e, cfname(name), arg, ctypes.byref(v))
    if rc or not v.value:
        if err is not None:
            err.append(rc)
        return None
    return cfvalue(v.value)


def setattr_(e, name, value):
    """Set an attribute: its AXError, 0 if set."""
    return lib()[0].AXUIElementSetAttributeValue(e, cfname(name), value)


def settable(e, name):
    b = ctypes.c_bool()
    rc = lib()[0].AXUIElementIsAttributeSettable(e, cfname(name), ctypes.byref(b))
    return not rc and b.value


def cfstr(s):
    return lib()[1].CFStringCreateWithCString(None, s.encode(), UTF8)


def actions(e):
    ax, _ = lib()
    v = ctypes.c_void_p()
    if ax.AXUIElementCopyActionNames(e, ctypes.byref(v)) or not v.value:
        return []
    return [cfvalue(a) for a in cfvalue(v.value)]


def name(e):
    return attr(e, "AXDescription") or attr(e, "AXTitle") or ""


OBSERVED = ("AXMenuOpened", "AXMenuClosed")
_callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                             ctypes.c_void_p)


class Observer:
    """The app's notifications, as a tool such as Voice Control gets
    them: (name, element) in order, the run loop pumped while waiting."""

    def __init__(self, app, pid):
        ax, cf = lib()
        self.events = []
        self.err = None

        def heard(_obs, el, note, _ref):
            self.events.append((cfvalue(note), cf.CFRetain(el) if el else None))
        self.cb = _callback(heard)  # kept: called for as long as it lives
        obs = ctypes.c_void_p()
        rc = ax.AXObserverCreate(pid, self.cb, ctypes.byref(obs))
        if rc or not obs.value:
            self.err = f"AXObserverCreate: AXError {rc}"
            return
        self.obs = obs.value
        for n in OBSERVED:
            rc = ax.AXObserverAddNotification(self.obs, app, cfname(n), None)
            if rc:
                self.err = f"AXObserverAddNotification {n}: AXError {rc}"
                return
        self.mode = ctypes.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode").value
        cf.CFRunLoopAddSource(cf.CFRunLoopGetCurrent(),
                              ax.AXObserverGetRunLoopSource(self.obs), self.mode)

    def wait(self, note, secs=5):
        """The element of the first NOTE told since the last wait, or None."""
        deadline = time.monotonic() + secs
        while not self.err:
            for i, (n, el) in enumerate(self.events):
                if n == note:
                    del self.events[:i + 1]
                    return el
            if time.monotonic() >= deadline:
                return None
            lib()[1].CFRunLoopRunInMode(self.mode, 0.1, False)
        return None

    def clear(self):
        if not self.err:
            lib()[1].CFRunLoopRunInMode(self.mode, 0.05, False)
        self.events.clear()


def walk(app):
    """Every element under the app's windows, depth first, front first
    among siblings as AppKit lists them: (depth, element)."""
    found = []
    wins = attr(app, "AXWindows")
    stack = [(0, w) for w in reversed(wins if isinstance(wins, list) else [])]
    while stack and len(found) < MAXNODES:
        depth, e = stack.pop()
        found.append((depth, e))
        kids = attr(e, "AXChildren") if depth < DEPTH else None
        for k in reversed(kids if isinstance(kids, list) else []):
            stack.append((depth + 1, k))
    return found


def probe(pid, text):
    ax, cf = lib()
    r = {"trusted": bool(ax.AXIsProcessTrusted()), "checks": []}
    if not r["trusted"]:
        return r

    def check(ok, what, detail=None):
        r["checks"].append([bool(ok), what, None if ok else detail])
        return ok

    app = ax.AXUIElementCreateApplication(pid)
    ax.AXUIElementSetMessagingTimeout(app, 5.0)
    obs = Observer(app, pid)
    check(not obs.err, "an observer of the app's menus", obs.err)
    nodes = walk(app)
    r["tree"] = [[d, attr(e, "AXRole"), attr(e, "AXSubrole"), name(e)] for d, e in nodes]
    check(len(nodes) > 10, f"the windows' tree has {len(nodes)} elements")

    err = []
    app_focus = attr(app, "AXFocusedUIElement", err)
    r["app_focused"] = ([attr(app_focus, "AXRole"), name(app_focus)]
                        if isinstance(app_focus, int) else f"AXError {err}")

    focused = [e for _, e in nodes if attr(e, "AXFocused") is True]
    roles = [[attr(e, "AXRole"), name(e)] for e in focused]
    prompt = [e for e in focused if attr(e, "AXRole") == "AXTextArea" and name(e) == "Prompt"]
    check(len(prompt) == 1, "the focused element is the prompt, an AXTextArea", roles)
    if not prompt:
        return r
    value = attr(prompt[0], "AXValue")
    check(value == text, f"the prompt holds {text!r}", value)

    send = [e for _, e in nodes if attr(e, "AXRole") == "AXButton" and name(e) == "Send"]
    check(len(send) == 1, "one AXButton is named Send", len(send))
    if not send:
        return r
    acts = actions(send[0])
    check("AXPress" in acts, "Send offers AXPress", acts)
    rc = ax.AXUIElementPerformAction(send[0], cfname("AXPress"))
    check(rc == 0, "AXPress on Send", f"AXError {rc}")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        value = attr(prompt[0], "AXValue")
        if value == "":
            break
        time.sleep(0.1)
    check(value == "", "the prompt emptied: the press reached the app, which sent it", value)
    if value == "":
        edit(app, prompt[0], check, obs)
    menus(app, check, obs)
    return r


def waitvalue(e, want, secs=5):
    deadline = time.monotonic() + secs
    while True:
        got = attr(e, "AXValue")
        if got == want or time.monotonic() >= deadline:
            return got
        time.sleep(0.1)


def edit(app, prompt, check, obs):
    """Step 5: the prompt's text, set and read as Voice Control does."""
    text = "a\U0001F600 \u65e5\u672c tail"  # 11 UTF-16 units
    for a in ("AXValue", "AXSelectedTextRange", "AXSelectedText"):
        check(settable(prompt, a), f"the prompt's {a} is settable")
    rc = setattr_(prompt, "AXValue", cfstr(text))
    check(rc == 0, "AXValue set", f"AXError {rc}")
    got = attr(prompt, "AXValue")
    check(got == text, "AXValue reads back at once", got)
    got = attr(prompt, "AXNumberOfCharacters")
    check(got == 11, "AXNumberOfCharacters counts UTF-16 units", got)
    got = waitvalue(prompt, text)  # a frame, to be ImGui's
    rc = setattr_(prompt, "AXSelectedTextRange", axvalue(AXV_RANGE, 7, 4))
    check(rc == 0, "AXSelectedTextRange set past the emoji and CJK", f"AXError {rc}")
    got = attr(prompt, "AXSelectedText")
    check(got == "tail", "AXSelectedText is what the range covers", got)
    rc = setattr_(prompt, "AXSelectedText", cfstr("TAIL!"))
    check(rc == 0, "AXSelectedText set", f"AXError {rc}")
    after = "a\U0001F600 \u65e5\u672c TAIL!"
    got = attr(prompt, "AXValue")
    check(got == after, "AXValue reads back the replacement at once", got)
    got = attr(prompt, "AXSelectedTextRange")
    check(got == (12, 0), "the caret is after it", got)
    for rng, want in (((1, 2), "\U0001F600"), ((4, 2), "\u65e5\u672c")):
        got = param(prompt, "AXStringForRange", axvalue(AXV_RANGE, *rng))
        check(got == want, f"AXStringForRange {rng}", got)
    # Where the text is drawn is known once ImGui has drawn it: until the
    # next frame, the geometry is of the text before the edit
    got = waitvalue(prompt, after)
    check(got == after, "ImGui took the edits", got)
    time.sleep(0.5)
    err = []
    bounds = param(prompt, "AXBoundsForRange", axvalue(AXV_RANGE, 4, 1), err)
    frame = None
    pos, size = attr(prompt, "AXPosition"), attr(prompt, "AXSize")
    if isinstance(pos, tuple) and isinstance(size, tuple):
        frame = pos + size
    inside = (isinstance(bounds, tuple) and frame is not None and bounds[2] > 0 and bounds[3] > 0
              and bounds[0] >= frame[0] - 1 and bounds[0] + bounds[2] <= frame[0] + frame[2] + 1
              and bounds[1] >= frame[1] - 1 and bounds[1] + bounds[3] <= frame[1] + frame[3] + 1)
    check(inside, "AXBoundsForRange of a CJK character lies in the prompt",
          [bounds, frame, err])
    if isinstance(bounds, tuple):
        mid = axvalue(AXV_POINT, bounds[0] + bounds[2] / 2, bounds[1] + bounds[3] / 2)
        got = param(prompt, "AXRangeForPosition", mid)
        check(got == (4, 1), "AXRangeForPosition at its middle finds it", got)

    # One undo takes back the last edit alone: through the prompt's menu,
    # as a tool presses it
    obs.clear()
    rc = lib()[0].AXUIElementPerformAction(prompt, cfname("AXShowMenu"))
    check(rc == 0, "AXShowMenu on the prompt", f"AXError {rc}")
    menu = obs.wait("AXMenuOpened")
    check(menu and attr(menu, "AXRole") == "AXMenu", "the prompt's menu is told opened, an AXMenu",
          menu and attr(menu, "AXRole"))
    undo = []
    deadline = time.monotonic() + 5
    while not undo and time.monotonic() < deadline:
        undo = [e for _, e in walk(app)
                if attr(e, "AXRole") == "AXMenuItem" and name(e) == "Undo"]
        if not undo:
            time.sleep(0.1)
    if not check(undo, "the prompt's menu has Undo", None):
        return
    rc = lib()[0].AXUIElementPerformAction(undo[0], cfname("AXPress"))
    check(rc == 0, "AXPress on Undo", f"AXError {rc}")
    check(obs.wait("AXMenuClosed"), "the prompt's menu is told closed")
    got = waitvalue(prompt, text)
    check(got == text, "one undo took back the replacement alone", got)
    setattr_(prompt, "AXValue", cfstr(""))


def menuitems(menu):
    """A menu's items by name: (element, its frame or None)."""
    items = {}
    for e in attr(menu, "AXChildren") or []:
        if attr(e, "AXRole") == "AXMenuItem":
            pos, size = attr(e, "AXPosition"), attr(e, "AXSize")
            ok = isinstance(pos, tuple) and isinstance(size, tuple)
            items[name(e)] = (e, pos + size if ok else None)
    return items


def within(inner, outer):
    return (inner is not None and outer is not None and inner[2] > 0 and inner[3] > 0
            and inner[0] >= outer[0] - 1 and inner[0] + inner[2] <= outer[0] + outer[2] + 1
            and inner[1] >= outer[1] - 1 and inner[1] + inner[3] <= outer[1] + outer[3] + 1)


def menus(app, check, obs):
    """Step 6: the menu bar's Edit menu, as Voice Control's "Click Edit",
    then "Click Select all": it looks in a menu once told it opened."""
    ax, cf = lib()
    bar = [e for _, e in walk(app) if attr(e, "AXRole") == "AXMenuBarItem" and name(e) == "Edit"]
    if not check(len(bar) == 1, "one AXMenuBarItem is named Edit", len(bar)):
        return
    edit = bar[0]
    check("AXPress" in actions(edit), "Edit offers AXPress", actions(edit))
    check(attr(edit, "AXSelected") is False, "Edit is not selected while closed",
          attr(edit, "AXSelected"))

    def opened():
        obs.clear()
        rc = ax.AXUIElementPerformAction(edit, cfname("AXPress"))
        check(rc == 0, "AXPress on Edit", f"AXError {rc}")
        menu = obs.wait("AXMenuOpened")
        check(menu, "Edit's menu is told opened (AXMenuOpened)")
        return menu

    menu = opened()
    if not menu:
        return
    check(attr(menu, "AXRole") == "AXMenu", "the menu told opened is an AXMenu",
          attr(menu, "AXRole"))
    up = attr(menu, "AXParent")
    check(isinstance(up, int) and cf.CFEqual(up, edit), "the menu hangs from Edit",
          isinstance(up, int) and [attr(up, "AXRole"), name(up)])
    check(attr(edit, "AXSelected") is True, "Edit is selected while its menu is open",
          attr(edit, "AXSelected"))
    pos, size = attr(menu, "AXPosition"), attr(menu, "AXSize")
    frame = pos + size if isinstance(pos, tuple) and isinstance(size, tuple) else None
    items = menuitems(menu)
    check("Select all" in items and "Copy" in items, "the menu lists Copy and Select all",
          sorted(items))
    out = [n for n, (_, f) in items.items() if not within(f, frame)]
    check(items and not out, "each item lies in the menu's frame", [out, frame])
    if "Select all" not in items:
        return
    sel = items["Select all"][0]
    check("AXPress" in actions(sel), "Select all offers AXPress", actions(sel))
    # Copy follows the transcript's selection, which the checklist's
    # click on the transcript cleared: disabled now, or its enabling
    # below would prove nothing of the press
    copied = attr(items["Copy"][0], "AXEnabled") if "Copy" in items else None
    check(copied is False, "Copy is disabled before Select all", copied)

    rc = ax.AXUIElementPerformAction(sel, cfname("AXPress"))
    check(rc == 0, "AXPress on Select all", f"AXError {rc}")
    check(obs.wait("AXMenuClosed"), "the menu is told closed once its item is pressed")
    check(attr(edit, "AXSelected") is False, "Edit is not selected once it closed",
          attr(edit, "AXSelected"))
    kids = attr(edit, "AXChildren") or []
    check(not kids, "Edit holds no menu once it closed", len(kids))

    # Its press reached the app: the transcript is selected, so Copy is
    # enabled now
    menu = opened()
    if not menu:
        return
    items = menuitems(menu)
    got = attr(items["Copy"][0], "AXEnabled") if "Copy" in items else None
    check(got is True, "Copy is enabled after Select all", [copied, got])
    rc = ax.AXUIElementPerformAction(edit, cfname("AXPress"))
    check(rc == 0, "AXPress on Edit again, to close it", f"AXError {rc}")
    check(obs.wait("AXMenuClosed"), "the menu is told closed by Edit's second press")


def run(app, pid, text, say=print):
    """The probe of pid under app's reaper: ("ok"|"fail"|"skipped",
    message, its result or None)."""
    if sys.platform != "darwin":
        return "skipped", "macOS only", None
    sys.path.insert(0, HERE)
    import disclaim
    import pids
    from desktop_live_macos import Reaped, Stop

    exe = os.path.join(app, "Contents", "MacOS", "prospero")
    if not os.path.isfile(exe):
        return "skipped", f"no Prospero.app at {app} (its reaper runs the probe)", None
    if not disclaim.available():
        return "skipped", "no responsibility functions here", None
    r = Reaped(app, ["--probe", str(pids.checked(pid)), text],
               prog=[sys.executable, "-I", os.path.abspath(__file__)])
    try:
        try:
            line = r.readline(30)
        except Stop as e:
            return "fail", f"the probe did not start: {e.args[0][0]}", None
        got = pids.parse(line.removeprefix("pid "))
        if not line.startswith("pid ") or len(got) != 1:
            return "fail", f"the probe said {line!r}", None
        try:
            say(r.vouch(got[0], "the Accessibility probe"))
        except Stop as e:
            return "fail", e.args[0][0], None
        r.inp.write(b"go\n")
        r.inp.close()
        r.inp = None
        try:
            out = r.readall(60).decode("utf-8", "replace")
        except Stop as e:
            return "fail", f"the probe did not finish: {e.args[0][0]}", None
    finally:
        r.close()
    try:
        p = json.loads(out)
    except ValueError:
        return "fail", f"the probe's output is no JSON: {out[:300]!r}", None
    if not p.get("trusted"):
        return ("skipped", f"Prospero.app ({app}) lacks Accessibility: grant it in System "
                "Settings > Privacy & Security > Accessibility", p)
    bad = [c for c in p["checks"] if not c[0]]
    return ("fail" if bad else "ok"), (f"{len(bad)} of {len(p['checks'])} checks failed"
                                       if bad else f"{len(p['checks'])} checks passed"), p


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--app", default=os.path.join(HERE, "..", "build-signed", "Prospero.app"),
                    help="the Prospero.app whose reaper runs the probe")
    ap.add_argument("--probe", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("pid", type=int)
    ap.add_argument("text")
    args = ap.parse_args()
    if args.probe:
        print(f"pid {os.getpid()}", flush=True)
        if sys.stdin.readline().strip() != "go":
            return 3  # not let go on: nothing asked
        print(json.dumps(probe(args.pid, args.text)))
        return 0
    status, msg, p = run(os.path.realpath(args.app), args.pid, args.text)
    for ok, what, detail in (p or {}).get("checks", []):
        print(("ok    " if ok else "FAIL  ") + what + ("" if ok else f": {detail!r}"))
    print(f"{status}: {msg}")
    return {"ok": 0, "fail": 1}.get(status, 2)


if __name__ == "__main__":
    sys.exit(main())

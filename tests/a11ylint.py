#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Controls the accessibility tree can't name or press, as the sources show.

ImGui labels most widgets, and the accessibility glue (a11y_imgui.cpp)
names them from their labels. What it can't see through is a control
drawn by hand: an InvisibleButton under drawn text or icons, an item
added bare with ItemAdd, or a click read straight from the mouse
(IsItemClicked, IsMouseClicked, IsMouseReleased and the like), which an
assistive tool's press never sets, or a context menu ImGui opens on a
right click alone (BeginPopupContextItem and the like). Each such call
in src/ must have, on its line or within a few lines around it:
  - axname, axrole or axkey, naming it or saying what it is (axrole
    with AX_NONE hides a surface the keyboard reaches another way), or
  - uiclicked, which a tool's press also sets, or uirclicked for a
    context menu, which a tool's "show menu" also opens, or
  - a comment "a11y:" saying why it needs neither.
Each call without one is printed, as FILE:LINE: and the call, and any
fails the check. The matcher first checks itself on snippets.

  uv run tests/a11ylint.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))

# The accessibility code itself, and ImGui's own patch
SKIP = {"a11y.cpp", "a11y_imgui.cpp", "ime.cpp", "imgui_a11y.h"}

CALL = re.compile(r"\b(InvisibleButton|IsItemClicked|IsMouseClicked|IsMouseDoubleClicked|"
                  r"IsMouseReleased|ItemAdd|ButtonBehavior|BeginPopupContextItem|"
                  r"BeginPopupContextWindow|BeginPopupContextVoid|OpenPopupOnItemClick)\s*\(")
ANSWER = re.compile(r"\b(axname|axrole|axkey|axmenu|uiclicked|uirclicked)\s*\(|a11y:")
BEFORE = 3  # lines around a call that may answer it
AFTER = 4


def code(line):
    """The line without its // comment or the insides of its strings."""
    out = []
    i = 0
    quote = None
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
                out.append(c)
            i += 1
            continue
        if c in "\"'":
            quote = c
            out.append(c)
        elif line.startswith("//", i):
            break
        else:
            out.append(c)
        i += 1
    return "".join(out)


def check(lines):
    """(line number, call) for each call with no answer near it."""
    bad = []
    for i, line in enumerate(lines):
        m = CALL.search(code(line))
        if not m:
            continue
        lo = max(0, i - BEFORE)
        hi = min(len(lines), i + AFTER + 1)
        if not any(ANSWER.search(l) for l in lines[lo:hi]):
            bad.append((i + 1, m.group(1)))
    return bad


def selftest():
    cases = [
        ('ImGui::InvisibleButton("##x", sz);', 1),
        ('ImGui::InvisibleButton("##x", sz);\naxname(g, "Close");', 0),
        ('if (ImGui::IsItemClicked(0)) {', 1),
        ('if (uiclicked()) {', 0),
        ('// a11y: the keyboard has Ctrl+W for this\nif (ImGui::IsMouseClicked(0)) {', 0),
        ('// IsMouseClicked(0) in a comment', 0),
        ('log("InvisibleButton(")', 0),
        ('ItemAdd(bb, id);\n\n\n\n\n\naxname(g, "far");', 1),
        ('axrole(g, AX_NONE);\nImGui::InvisibleButton("##grip", sz);', 0),
    ]
    ok = True
    for text, want in cases:
        got = len(check(text.split("\n")))
        if got != want:
            print(f"a11ylint: selftest: {text!r}: {got} flagged, want {want}")
            ok = False
    return ok


def main():
    if not selftest():
        return 1
    found = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".cpp") or name in SKIP:
            continue
        path = os.path.join(SRC, name)
        with open(path, encoding="utf-8") as f:
            lines = f.read().split("\n")
        for n, call in check(lines):
            print(f"src/{name}:{n}: {call} with no axname, axrole, uiclicked or a11y: comment")
            found += 1
    if found:
        print(f"a11ylint: {found} unanswered")
        return 1
    print("a11ylint: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

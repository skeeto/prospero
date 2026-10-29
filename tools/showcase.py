#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Stage the README's screenshot: sessions worth showing, in a state
directory of their own, then the GUI over them.

  uv run tools/showcase.py --prospero build/Prospero.app DIR [--work W]
  uv run tools/showcase.py --prospero build/Prospero.app DIR --open [--shot FILE]

DIR gets a small project (W, by default DIR/work/mandel, a C program
that renders the Mandelbrot set to a BMP file, in a git repository),
whose path the sidebar shows under each session, so for the README
it is ~/src/mandel; and DIR gets a configuration, and
a state directory whose sessions a scripted model (the "showcase"
scenario, added to tests/fake_provider.py's at run time) made through
the real engine: its tools really read, edit, build and run the
project. The session in front planned in plan mode (a plan approved
here), keeps a todo list, ran two subagents, and read the renders before
and after its change, which the transcript shows as images. Others
round out the sidebar, one with subagents of its own.

--open serves the same model again and opens the window on DIR's state,
on the session in front, which keeps the window's size and layout from
one opening to the next: arranged once, the screenshot can be taken
again from the same state. --shot FILE then has the GUI save its window
as a PNG after --wait seconds (SDL builds only), and quit. The PNG is
uncompressed at the display's scale; for the README, scale it to 1728
pixels wide, which also compresses it (macOS: sips -Z 1728 FILE). Making DIR anew needs
a C compiler, git, and an empty or missing DIR.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tests"))
import fake_provider as F  # noqa: E402

MODEL = "qwen3-coder"  # as the window shows it
FAKE = "fake:showcase"

# The project, as it is before the sessions change it

MANDEL_C = r"""// mandel: render the Mandelbrot set as a 24-bit BMP on standard output
//   $ ./mandel [WIDTH HEIGHT] >out.bmp
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAXITER 512

static unsigned char const palette[16][3] = {
    { 66,  30,  15}, { 25,   7,  26}, {  9,   1,  47}, {  4,   4,  73},
    {  0,   7, 100}, { 12,  44, 138}, { 24,  82, 177}, { 57, 125, 209},
    {134, 181, 229}, {211, 236, 248}, {241, 233, 191}, {248, 201,  95},
    {255, 170,   0}, {204, 128,   0}, {153,  87,   0}, {106,  52,   3},
};

static void put32(unsigned char *p, unsigned v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

// Color one point, as BMP stores it: blue, green, red
static void color(double cr, double ci, unsigned char *bgr)
{
    double zr = 0, zi = 0;
    int n = 0;
    for (; n < MAXITER && zr*zr + zi*zi <= 4; n++) {
        double t = zr*zr - zi*zi + cr;
        zi = 2*zr*zi + ci;
        zr = t;
    }
    if (n == MAXITER) {
        bgr[0] = bgr[1] = bgr[2] = 0;
        return;
    }
    unsigned char const *c = palette[n % 16];
    bgr[0] = c[2];
    bgr[1] = c[1];
    bgr[2] = c[0];
}

int main(int argc, char **argv)
{
    int w = argc > 2 ? atoi(argv[1]) : 960;
    int h = argc > 2 ? atoi(argv[2]) : 600;
    int stride = (3*w + 3) & ~3;
    unsigned char hdr[54] = {'B', 'M'};
    put32(hdr +  2, 54 + stride*h);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, sizeof(hdr), 1, stdout);

    // The seahorse valley, rows bottom up as BMP has them
    unsigned char *row = calloc(stride, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double cr = -0.7453 + 0.006*(x - w/2.0)/h;
            double ci =  0.1127 + 0.006*(y - h/2.0)/h;
            color(cr, ci, row + 3*x);
        }
        fwrite(row, stride, 1, stdout);
    }
    free(row);
    return 0;
}
"""

MAKEFILE = """CC     = cc
CFLAGS = -O2 -Wall -Wextra
LDLIBS = -lm

mandel: mandel.c
\t$(CC) $(CFLAGS) -o $@ mandel.c $(LDLIBS)

render: mandel
\t./mandel >renders/latest.bmp

clean:
\trm -f mandel
"""

README = """# mandel

Renders the Mandelbrot set's seahorse valley as a BMP image.

    $ make
    $ ./mandel 960 600 >out.bmp

Renders of each version are kept in `renders/`.
"""

# The change the session in front makes: the normalized iteration count
# for a continuous shade, and a cosine palette in place of the table

SMOOTH_OLD_TEST = "for (; n < MAXITER && zr*zr + zi*zi <= 4; n++) {"
SMOOTH_NEW_TEST = ("// A large escape radius keeps the smooth count accurate\n"
                   "    for (; n < MAXITER && zr*zr + zi*zi <= 256*256; n++) {")
SMOOTH_OLD_COLOR = """    unsigned char const *c = palette[n % 16];
    bgr[0] = c[2];
    bgr[1] = c[1];
    bgr[2] = c[0];"""
SMOOTH_NEW_COLOR = """    // Normalized iteration count: continuous across escape boundaries
    double mu = n + 1 - log2(log(zr*zr + zi*zi) / 2);
    double t  = sqrt(mu / MAXITER);
    for (int k = 0; k < 3; k++) {
        double phase = (double[]){0.00, 0.15, 0.35}[k];
        double v = 0.5 + 0.5*cos(6.2831853*(5*t + phase));
        bgr[2-k] = (unsigned char)(255*v);
    }"""
SMOOTH_OLD_PALETTE = MANDEL_C[MANDEL_C.index("static unsigned char const palette"):
                              MANDEL_C.index("static void put32")]

PLAN = """# 🎨 Smooth coloring for mandel

## 🔍 What's wrong

`color()` picks a palette entry by the raw iteration count, `n % 16`, so
neighboring pixels one iteration apart jump between unrelated colors:
the hard bands in `renders/banded.bmp`.

## 🛠️ Steps

1. 📐 **Escape radius**: test against 256² instead of 4, so the count
   below is accurate to well under one shade.
2. 📝 **Normalized count**: μ = n + 1 − log₂(log |z|), continuous where
   `n` steps.
3. 🌈 **Palette**: replace the 16-entry table with a cosine gradient
   sampled at √(μ / MAXITER), which spreads the colors over the zoom.
4. 👀 **Render** `renders/smooth.bmp` and compare it with the banded one.
5. ⏰ **Check the cost**: `log2` and `cos` once a pixel, outside the loop.

## ✅ Done when

- No visible bands at the default view
- Render time within 10% of before
- `-Wall -Wextra` stays quiet ✨
"""

SUMMARY = """## 🌈 Smooth coloring is in

The bands are gone: shades follow the **normalized iteration count**.

| | Before | After |
|---|---|---|
| Coloring | `palette[n % 16]` | cosine gradient at √(μ/512) |
| Escape radius | 2 | 256 |
| Render, 960×600 | 0.21 s | 0.22 s ⏰ |

Next, a `-p` flag to pick among palettes? 🎨
"""

# Each conversation by a phrase of its first prompt, and its step: how
# many responses it has had so far

MAIN = "show hard bands"
SURVEY = "Survey how mandel.c colors"
RENDERS = "Look at renders/banded.bmp"
OFFBY = "the escape test"
PORT = "build on Windows"
FLAGS = "Check the Makefile's flags"
POSIX = "Find what mandel.c needs from POSIX"
PROFILE = "Profile the inner loop"
KNOWN = (MAIN, SURVEY, RENDERS, OFFBY, PORT, FLAGS, POSIX, PROFILE)


def turn(req):
    """The phrase that began the conversation, and its step."""
    msgs = req.get("messages", [])
    for m in msgs:
        c = m.get("content") or ""
        if isinstance(c, list):
            c = "".join(p.get("text", "") for p in c if p.get("type") == "text")
        if m.get("role") == "user":
            for k in KNOWN:
                if k in c:
                    return k, sum(1 for x in msgs if x.get("role") == "assistant")
    return "", 0


def tokens(req):
    """A prompt's tokens as a real model's: about 4 bytes a token of its
    text, and about 1,500 an image, not its base64."""
    n = 0
    for m in req.get("messages", []):
        c = m.get("content")
        if isinstance(c, list):
            for p in c:
                n += 1500 if p.get("type") == "image_url" else len(json.dumps(p)) // 4
        else:
            n += len(json.dumps(m)) // 4
    return n + len(json.dumps(req.get("tools", []))) // 4


def fin(model, req, reason, said=""):
    # Counted as a real conversation's, so the context bar fills
    n = tokens(req) + 3000
    return F.finish(model, reason, prompt=n, completion=60 + len(said)//4, cache_n=n//2)


def say(model, req, s, thought=None):
    yield F.S.head()
    if thought:
        yield from F.think(model, thought, 4)
    for i in range(0, len(s), 40):
        yield from F.text(model, s[i:i+40])
        yield F.S.sleep(0.005)
    yield from fin(model, req, "stop", s)


def do(model, n, req, items, thought=None, said=None):
    """Calls [(name, args)] in one response, after a thought or words."""
    yield F.S.head()
    if thought:
        yield from F.think(model, thought, 3)
    if said:
        yield from F.text(model, said)
    for i, (name, args) in enumerate(items):
        yield from F.call(model, i, f"call_{n}_{i}", name, args, 2)
    yield from fin(model, req, "tool_calls")


def todo(done, doing, items):
    return [{"content": c, "status": "done" if i < done else
             "in_progress" if i < done + doing else "pending"}
            for i, c in enumerate(items)]


TODO = ["Raise the escape radius", "Compute the normalized count",
        "Replace the palette with a cosine gradient",
        "Render and compare", "Check the render time"]


def sc_showcase(model, n, req):
    k, s = turn(req)
    if k == MAIN:
        if s == 0:
            yield from do(model, n, req, [
                ("task", {"description": "Survey the coloring code",
                          "prompt": "Survey how mandel.c colors each pixel: where the "
                                    "palette is picked, and what the escape test keeps."}),
                ("task", {"description": "Look at the banding",
                          "prompt": "Look at renders/banded.bmp and say where the bands "
                                    "are worst."})],
                thought="Banding means the color is a step function of the iteration "
                        "count. Before planning, I want the coloring code and a look at "
                        "the render itself; two subagents can do both at once.",
                said="I'll look at the code and the render side by side.\n")
        elif s == 1:
            yield from do(model, n, req, [("read", {"path": "renders/banded.bmp"})],
                          thought="The survey says palette[n % 16] with an escape "
                                  "radius of 2. Let me see the bands myself.")
        elif s == 2:
            yield from do(model, n, req, [("write", {"path": F.plan_path(req), "content": PLAN})],
                          thought="Clear: the classic fix is the normalized iteration "
                                  "count with a larger bailout, and a continuous palette.")
        elif s == 3:
            yield from do(model, n, req, [("exit_plan_mode", {})])
        elif s == 4:
            yield from do(model, n, req, [("todo_write", {"todos": todo(0, 1, TODO)})],
                          said="Approved. Working through it:\n")
        elif s == 5:
            yield from do(model, n, req, [("edit", {"path": "mandel.c",
                                                    "old_string": SMOOTH_OLD_TEST,
                                                    "new_string": SMOOTH_NEW_TEST})])
        elif s == 6:
            yield from do(model, n, req, [("todo_write", {"todos": todo(1, 1, TODO)})])
        elif s == 7:
            yield from do(model, n, req, [
                ("edit", {"path": "mandel.c", "old_string": SMOOTH_OLD_COLOR,
                          "new_string": SMOOTH_NEW_COLOR}),
                ("edit", {"path": "mandel.c", "old_string": SMOOTH_OLD_PALETTE,
                          "new_string": ""})])
        elif s == 8:
            yield from do(model, n, req, [("todo_write", {"todos": todo(3, 1, TODO)})])
        elif s == 9:
            yield from do(model, n, req, [("shell", {
                "command": "make && time ./mandel >renders/smooth.bmp"})])
        elif s == 10:
            yield from do(model, n, req, [("todo_write", {"todos": todo(5, 0, TODO)})],
                          said="Built clean, 0.22 s against 0.21 s before.\n")
        elif s == 11:
            yield from do(model, n, req, [("read", {"path": "renders/smooth.bmp"})],
                          thought="Last, the render itself, to see that the bands are "
                                  "gone. A palette flag is a separate change; I'll ask.")
        else:
            yield from say(model, req, SUMMARY)
    elif k == SURVEY:
        if s == 0:
            yield from do(model, n, req, [("read", {"path": "mandel.c"})])
        else:
            yield from say(model, req,
                           "`color()` iterates while |z|² ≤ 4 (radius 2) and keeps only "
                           "`n`. The color is `palette[n % 16]`, a 16-entry table, so "
                           "the shade changes only when `n` does. Interior points are "
                           "black. `math.h` is included but unused.")
    elif k == RENDERS:
        if s == 0:
            yield from do(model, n, req, [("read", {"path": "renders/banded.bmp"})])
        else:
            yield from say(model, req,
                           "Hard concentric bands everywhere outside the set, worst along "
                           "the spirals of the seahorse tails, where the count changes "
                           "every few pixels. Colors repeat every 16 bands.")
    elif k == OFFBY:
        if s == 0:
            yield from do(model, n, req, [("grep", {"pattern": "MAXITER"})],
                          thought="An off-by-one in the loop bound would color the "
                                  "last iteration as interior.")
        elif s == 1:
            yield from do(model, n, req, [("read", {"path": "mandel.c", "offset": 20,
                                                    "limit": 20})])
        else:
            yield from say(model, req,
                           "No off-by-one: the loop stops at `MAXITER` iterations, and only "
                           "points that never escaped get `n == MAXITER`, so a point that "
                           "escapes on the last step is still colored. Nothing to change.")
    elif k == PORT:
        if s == 0:
            yield from do(model, n, req, [
                ("task", {"description": "Check the compiler flags",
                          "prompt": "Check the Makefile's flags for what MSVC and MinGW "
                                    "would make of them."}),
                ("task", {"description": "Find POSIX-only calls",
                          "prompt": "Find what mandel.c needs from POSIX, if anything."})])
        else:
            yield from say(model, req,
                           "It builds on Windows as is with w64devkit or any MinGW: the "
                           "flags are portable and `mandel.c` is plain C11. One catch: "
                           "standard output is in **text mode** there, so each `\\n` byte "
                           "in the image becomes `\\r\\n`. The fix is `_setmode(1, "
                           "_O_BINARY)` under `#ifdef _WIN32`. Want me to add it?")
    elif k == FLAGS:
        if s == 0:
            yield from do(model, n, req, [("read", {"path": "Makefile"})])
        else:
            yield from say(model, req,
                           "`-O2 -Wall -Wextra` and `-lm` work with MinGW as they are. "
                           "MSVC would need `/O2 /W4` and no `-lm`, but `cc` is not MSVC.")
    elif k == POSIX:
        if s == 0:
            yield from do(model, n, req, [("grep", {"pattern": "#include"})])
        else:
            yield from say(model, req,
                           "Nothing from POSIX: only `math.h`, `stdio.h` and `stdlib.h`. "
                           "Binary output on stdout is the one platform difference.")
    elif k == PROFILE:
        if s == 0:
            yield from do(model, n, req, [("shell", {
                "command": "make -s && time ./mandel 1920 1200 >/dev/null"})],
                thought="Time a large render first.")
        else:
            yield from say(model, req,
                           "About a second for 1920×1200. Nearly all of it is points "
                           "inside the set running all 512 iterations. Checking the main "
                           "cardioid and the period-2 bulb up front would skip most of "
                           "those, but this view is mostly outside them. **Not worth it "
                           "here.**")
    else:
        yield from say(model, req, "Done.")


F.SCENARIOS["showcase"] = sc_showcase


# The engine, headless, through its JSON lines

class Engine:
    def __init__(self, prospero, ini, state, work):
        self.p = subprocess.Popen([prospero, "--headless", "-c", ini, "--state", state],
                                  cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, encoding="utf-8")
        self.events = []
        self.cv = threading.Condition()
        self.id = 0
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        for line in self.p.stdout:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            with self.cv:
                self.events.append(e)
                self.cv.notify_all()
            # Allow whatever is asked, but the plan, which the driver
            # approves itself
            if e.get("ev") == "ask" and e.get("kind") != "plan":
                self.send("answer", sid=e["sid"], req=e["req"], decision="allow")

    def send(self, cmd, **fields):
        with self.cv:
            self.id += 1
            obj = {"cmd": cmd, "id": self.id, **fields}
            self.p.stdin.write(json.dumps(obj) + "\n")
            self.p.stdin.flush()
            return self.id

    def wait(self, pred, timeout=120):
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                for e in self.events:
                    if pred(e):
                        return e
                left = deadline - time.monotonic()
                if left <= 0:
                    sys.exit(f"showcase: timed out; last events: {self.events[-5:]}")
                self.cv.wait(left)

    def call(self, cmd, **fields):
        i = self.send(cmd, **fields)
        return self.wait(lambda e: e.get("ev") == "reply" and e.get("id") == i)

    def session(self, cwd, title, prompt, plan=False, draft=None):
        r = self.call("new", cwd=cwd, endpoint="local", model=MODEL, attended=1,
                      plan=1 if plan else 0)
        sid = r.get("sid")
        if not sid:
            sys.exit(f"showcase: new: {r}")
        self.call("rename", sid=sid, title=title)
        self.send("prompt", sid=sid, text=prompt)
        if plan:
            ask = self.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid and
                            e.get("kind") == "plan")
            time.sleep(1)
            self.send("answer", sid=sid, req=ask["req"], decision="allow")
        self.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid, 300)
        if draft:
            self.call("draft", sid=sid, text=draft)
        print(f"showcase: {title}: {sid}", flush=True)
        return sid

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def config(url):
    return ("# The showcase's configuration (tools/showcase.py)\n"
            "endpoint = local\n"
            f"model = {MODEL}\n"
            "\n"
            "# Tool rows open, for the renders' thumbnails\n"
            "[ui]\n"
            "tools = open\n"
            "\n"
            "[endpoint local]\n"
            f"url = {url}\n"
            "\n"
            f"[model {MODEL}]\n"
            f"id = {FAKE}\n")


def git(work, *args):
    subprocess.run(["git", "-C", work, *args], check=True, capture_output=True,
                   env=dict(os.environ, GIT_AUTHOR_NAME="Showcase",
                            GIT_AUTHOR_EMAIL="showcase@example.com",
                            GIT_COMMITTER_NAME="Showcase",
                            GIT_COMMITTER_EMAIL="showcase@example.com"))


def project(work):
    os.makedirs(os.path.join(work, "renders"))
    for name, text in (("mandel.c", MANDEL_C), ("Makefile", MAKEFILE),
                       ("README.md", README), (".gitignore", "mandel\n")):
        with open(os.path.join(work, name), "w") as f:
            f.write(text)
    subprocess.run(["make", "-s"], cwd=work, check=True)
    with open(os.path.join(work, "renders", "banded.bmp"), "wb") as f:
        subprocess.run(["./mandel"], cwd=work, stdout=f, check=True)
    git(work, "init", "-q", "-b", "main")
    git(work, "add", ".")
    git(work, "commit", "-q", "-m", "Render the seahorse valley")
    git(work, "checkout", "-q", "-b", "smooth-coloring")


def executable(prospero):
    if prospero.endswith(".app"):
        return os.path.join(prospero, "Contents", "MacOS", "prospero")
    return prospero


def make(args, prospero, d):
    if os.path.exists(d) and os.listdir(d):
        sys.exit(f"showcase: {d} is not empty")
    work = args.work or os.path.join(d, "work", "mandel")
    if os.path.exists(work):
        sys.exit(f"showcase: {work} exists")
    state = os.path.join(d, "state")
    ini = os.path.join(d, "prospero.ini")
    os.makedirs(d, exist_ok=True)
    project(work)
    serve()
    srv, url = F.start()
    with open(ini, "w") as f:
        f.write(config(url + "/v1"))
    e = Engine(prospero, ini, state, work)
    try:
        e.session(work, "Profile the inner loop",
                  "Profile the inner loop of mandel.c. Is it worth optimizing?")
        e.session(work, "Off-by-one in the escape test?",
                  "Is there an off-by-one in the escape test? The last iteration "
                  "looks wrong to me.")
        e.session(work, "Build on Windows",
                  "What would it take to build on Windows? Check it two ways at once.")
        sid = e.session(work, "Smooth coloring",
                        "The renders in renders/ show hard bands between iteration "
                        "counts. Plan smooth coloring for mandel.c, then do it.", plan=True,
                        draft="Yes, add -p with a few named palettes: fire, ocean, and "
                              "a grayscale one for printing.")
        with open(os.path.join(d, "showcase.json"), "w") as f:
            json.dump({"work": work, "sid": sid}, f)
    finally:
        e.close()
        srv.shutdown()


def serve():
    """The scripted model, listed with a context of 128K."""
    F.STATE.register(FAKE, meta={"n_ctx": 131072, "n_ctx_train": 262144},
                     status={"value": "loaded", "args": ["llama-server", "--ctx-size",
                                                         "131072", "--parallel", "4"]})


def open_(args, prospero, d):
    state = os.path.join(d, "state")
    ini = os.path.join(d, "prospero.ini")
    if not os.path.isdir(state):
        sys.exit(f"showcase: no state in {d}: make it first")
    serve()
    srv, url = F.start()
    with open(ini, "w") as f:
        f.write(config(url + "/v1"))
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "config"))
    with open(os.path.join(d, "showcase.json")) as f:
        info = json.load(f)
    cmd = [prospero, "gui", "-c", ini, "--statedir", state, "--session", info["sid"]]
    layout = os.path.join(state, "imgui.ini")
    if not os.path.exists(layout):
        # A first run, which shows Help in front: once, briefly, for the
        # layout file, then without Help
        subprocess.run(cmd + ["--quit-after", "5"], cwd=info["work"], env=env,
                       stdin=subprocess.DEVNULL, timeout=60)
        with open(layout) as f:
            text = f.read()
        # Taller than the first size, for more of the transcript, at the
        # top left of the display, where the window is fitted to it
        text = text.replace("\nHelp=1\n", "\nHelp=0\n")
        text = re.sub(r"\[Prospero\]\[Window\]\n(?:[A-Za-z]+=[^\n]*\n)*\n?", "", text)
        text += ("\n[Prospero][Window]\nPos=0,66\n"
                 f"Size={args.size}\nMaximized=0\nFullscreen=0\n")
        with open(layout, "w") as f:
            f.write(text)
    if args.shot:
        # The GUI saves the frame itself, with no need for screen recording
        shot = os.path.abspath(args.shot)
        cmd += ["--quit-after", str(round(args.wait)), "--shot", shot]
    p = subprocess.Popen(cmd, cwd=info["work"], env=env, stdin=subprocess.DEVNULL)
    print(f"showcase: window {p.pid}; close it to end", flush=True)
    try:
        p.wait()
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
        srv.shutdown()
    if args.shot:
        if not os.path.exists(shot):
            sys.exit("showcase: no screenshot")
        print(f"showcase: {args.shot}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prospero", required=True, help="the program, or Prospero.app")
    ap.add_argument("--open", action="store_true", help="open the window on DIR")
    ap.add_argument("--shot", help="with --open: save the window to this PNG, and quit")
    ap.add_argument("--wait", type=int, default=8, help="seconds before --shot")
    ap.add_argument("--size", default="1714,1045",
                    help="the window's first size, in points (default: 1714,1045)")
    ap.add_argument("--work", help="where to make the project (default: DIR/work/mandel)")
    ap.add_argument("dir")
    args = ap.parse_args()
    prospero = os.path.abspath(executable(args.prospero))
    d = os.path.abspath(args.dir)
    if args.open:
        open_(args, prospero, d)
    else:
        make(args, prospero, d)


if __name__ == "__main__":
    main()

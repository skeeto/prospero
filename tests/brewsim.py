#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""brew's concurrent download display, without brew or the network.

Writes what Homebrew 7's DownloadQueue (download_queue.rb) writes to a
terminal while it fetches in parallel: a heading, then every 50 ms a
frame inside a synchronized update (CSI ?2026h ... CSI ?2026l): the
finished lines, each kept, then the pending ones (as many as the
concurrency or the height allows), a spinner, bar and sizes on each,
every piece flushed on its own as brew does, and the cursor moved back
up to the first pending line.

Each line's name carries the time since the start and the frame's
number, so a terminal that holds a frame too long, or skips some, shows
it as a jump. The summary at the end counts the frames written.

  uv run tests/brewsim.py                 # 24 downloads, 8 at a time
  uv run tests/brewsim.py -n 40 -j 12     # more of both
  uv run tests/brewsim.py --no-sync       # without mode 2026, to compare
  uv run tests/brewsim.py --log f.txt     # each frame's time, for later

Run it in the terminal under test; nothing is downloaded or written
but the log.
"""

import argparse
import os
import random
import shutil
import sys
import time

SPINNER = "⠋⠙⠚⠞⠖⠦⠴⠲⠳⠓"
TICK = 0.05  # brew's resolution.wait while on a TTY


def out(s):
    """One print-and-flush, as stdout_print_and_flush: its own write."""
    data = s.encode()
    while data:
        n = os.write(1, data)
        data = data[n:]


def size(n):
    """Formatter.disk_usage_readable_size_unit, as brew formats it."""
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:5.1f}{unit:>2}"
        n /= 1000


class Download:
    def __init__(self, i, rng, speed):
        self.name = f"Bottle sim{i:02d}"
        self.total = int(rng.lognormvariate(15, 1.2))  # ~3 MB, a long tail
        self.rate = rng.uniform(0.4e6, 4e6) * speed     # bytes per second
        self.start = None  # when its worker took it
        self.done = None

    def fetched(self, now):
        if self.start is None:
            return None
        if self.done is not None:
            return self.total
        return min(self.total, int((now - self.start) * self.rate))


def message(d, now, t0, frame, width, namemax, spin):
    """status + message_with_progress, its name carrying the clock."""
    if d.done is not None:
        status = "\033[32m✔︎\033[0m"
    else:
        status = f"\033[34m{spin}\033[0m"
    name = f"{d.name} [{now - t0:5.2f}s #{frame}]"
    avail = width - 3
    got = d.fetched(now)
    if got is None or width <= 0:
        return status, name[:avail]
    total = size(got) if d.done is not None else size(d.total)
    phase = "Downloaded" if d.done is not None else "Downloading"
    progress = f" {phase:<11} {size(got)}/{total}"
    barlen = max(4, avail - len(progress) - namemax - 1)
    if d.done is None:
        used = round(min(1.0, got / max(1, d.total)) * barlen)
        progress = f" {'#' * used}{' ' * (barlen - used)}{progress}"
    room = avail - len(progress)
    if room <= 0:
        return status, name[:avail]
    return status, name[:room].ljust(room) + progress


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("-n", type=int, default=24, help="downloads (24)")
    p.add_argument("-j", type=int, default=8, help="at a time (8)")
    p.add_argument("--speed", type=float, default=1.0, help="rate scale (1)")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--no-sync", action="store_true", help="leave out mode 2026")
    p.add_argument("--log", help="append each frame's time to this file")
    a = p.parse_args()
    if not os.isatty(1):
        sys.exit("brewsim: stdout is not a terminal")

    rng = random.Random(a.seed)
    downloads = [Download(i + 1, rng, a.speed) for i in range(a.n)]
    namemax = max(len(f"{d.name} [00.00s #0000]") for d in downloads)
    bsu = "" if a.no_sync else "\033[?2026h"
    esu = "" if a.no_sync else "\033[?2026l"
    log = open(a.log, "a") if a.log else None

    out("\033[34m==>\033[0m \033[1mFetching downloads for: "
        f"{', '.join(f'sim{i + 1:02d}' for i in range(min(a.n, 6)))}"
        f"{', ...' if a.n > 6 else ''}\033[0m\n")
    t0 = time.monotonic()
    queue = list(downloads)
    working = []
    remaining = list(downloads)
    pending_lines = 0
    frame = 0
    frame_width = shutil.get_terminal_size().columns
    out("\033[?25l")
    try:
        while remaining:
            now = time.monotonic()
            # The pool: finished downloads free their workers
            for d in working:
                if d.done is None and d.fetched(now) >= d.total:
                    d.done = now
            working = [d for d in working if d.done is None]
            while queue and len(working) < a.j:
                d = queue.pop(0)
                d.start = now
                working.append(d)

            frame += 1
            spin = SPINNER[frame % len(SPINNER)]
            cols, rows = shutil.get_terminal_size()
            maxlines = min(a.j, rows)
            out(bsu)
            if cols != frame_width:
                frame_width = cols
                out("\033[J")
            finished = [d for d in remaining if d.done is not None]
            remaining = [d for d in remaining if d.done is None]
            for d in finished:
                pending_lines -= 1
                s, m = message(d, now, t0, frame, cols, namemax, spin)
                out(f"{s} {m}\n")
                out("\033[K")
            pending_lines = 0
            for i, d in enumerate(remaining):
                if pending_lines >= maxlines:
                    break
                last = i == maxlines - 1 or i == len(remaining) - 1
                s, m = message(d, now, t0, frame, cols, namemax, spin)
                out(f"{s} {m}{'' if last else chr(10)}")
                pending_lines += 1
                out("\033[K")
            if pending_lines - 1 == 0:
                out("\033[0G")
            elif pending_lines > 0:
                out(f"\033[{pending_lines - 1}F")
            out(esu)
            if log:
                log.write(f"{frame} {time.monotonic() - t0:.3f}\n")
            if remaining:
                time.sleep(TICK)
    except KeyboardInterrupt:
        if pending_lines > 0:
            out(f"\033[{pending_lines - 1}B")
        out("\n")
    finally:
        out(esu)
        out("\033[?25h")
        if log:
            log.close()
    took = time.monotonic() - t0
    print(f"brewsim: {a.n} downloads in {took:.2f} s, {frame} frames, "
          f"{frame / max(took, 1e-9):.1f} per second")


if __name__ == "__main__":
    main()

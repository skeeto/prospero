#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Make the source release: one tarball that builds offline.

  uv run tools/dist.py [-o FILE] [--cache DIR] [COMMIT]

prospero-VERSION.tar.gz (or FILE) holds COMMIT's tree (default HEAD), as
git archive exports it, under prospero-VERSION/, and beside it every
dependency that COMMIT's cmake/Fetch*.cmake pins, unpacked under
deps/DIR, the directory fetch_content_bundled names for it. The build
takes a dependency from there instead of downloading it
(cmake/FetchContentBundled.cmake), so with all of them present it needs
no network:

  tar xzf prospero-VERSION.tar.gz
  cmake -S prospero-VERSION -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON

Each dependency comes from a file in --cache DIR with the pinned SHA-256,
if one is there, or else from its pinned URLs, tried in order, a few
times each, and saved into DIR when given. Either way it must match the
pin. It is unpacked as FetchContent unpacks it: a lone top-level
directory is stripped.

The tarball is deterministic: entries in path order, directories before
what they hold, every one owned by root and dated COMMIT's commit time,
files mode 644 or 755 (755 when the source had any execute bit),
directories 755, then gzipped (level 9) with no name or time. The same
commit and the same zlib make the same bytes. Like git archive's, it
starts with a pax global header naming the commit (git
get-tar-commit-id). Uncommitted changes are left out, since it comes
from COMMIT alone.
"""

import argparse
import gzip
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def say(msg):
    print(f"dist: {msg}", file=sys.stderr, flush=True)


def git(*args):
    p = subprocess.run(["git", "-C", ROOT, *args], capture_output=True)
    if p.returncode:
        sys.exit(f"dist: git {args[0]}: {p.stderr.decode().strip()}")
    return p.stdout


def version(commit):
    text = git("show", f"{commit}:CMakeLists.txt").decode()
    m = re.search(r"project\(\s*\w+\s+VERSION\s+([0-9.]+)", text)
    if not m:
        sys.exit("dist: no project VERSION in CMakeLists.txt")
    return m.group(1)


def pins(commit):
    """[(name, dir, [url, ...], sha256)] from the commit's cmake/Fetch*.cmake."""
    r = []
    names = git("ls-tree", "--name-only", commit, "cmake/").decode().split()
    for path in sorted(n for n in names if re.fullmatch(r"cmake/Fetch\w*\.cmake", n)):
        text = re.sub(r"#[^\n]*", "", git("show", f"{commit}:{path}").decode())
        dirs = dict(re.findall(r"fetch_content_bundled\(\s*([\w-]+)\s+([\w-]+)\s*\)", text))
        for m in re.finditer(r"FetchContent_Declare\(\s*([\w-]+)([^)]*)\)", text):
            name, words = m.group(1), m.group(2).split()
            if name not in dirs:
                sys.exit(f"dist: {path}: {name} has no fetch_content_bundled, "
                         "so deps/ could not supply it")
            urls, digest = [], None
            for i, w in enumerate(words):
                if w == "URL":
                    for u in words[i+1:]:
                        if re.fullmatch(r"[A-Z][A-Z0-9_]*", u):
                            break
                        urls.append(u)
                elif w == "URL_HASH" and i+1 < len(words):
                    h = re.fullmatch(r"SHA256=([0-9a-f]{64})", words[i+1])
                    digest = h.group(1) if h else None
            if not urls or not digest:
                sys.exit(f"dist: {path}: {name} needs URL and URL_HASH SHA256=")
            r.append((name, dirs[name], urls, digest))
    if not r:
        sys.exit("dist: no FetchContent_Declare in cmake/Fetch*.cmake")
    return r


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def index(cache):
    """{sha256: path} of the files in the cache directory."""
    r = {}
    if cache and os.path.isdir(cache):
        for f in sorted(os.listdir(cache)):
            p = os.path.join(cache, f)
            if os.path.isfile(p) and not f.endswith(".tmp"):
                with open(p, "rb") as fp:
                    r.setdefault(sha256(fp.read()), p)
    return r


def fetch(dep, cache, cached):
    """The dependency's tarball, checked against its pin."""
    name, d, urls, digest = dep
    if digest in cached:
        with open(cached[digest], "rb") as fp:
            data = fp.read()
        if sha256(data) == digest:
            say(f"{name}: {cached[digest]}")
            return data
    for attempt in range(3):
        if attempt:
            time.sleep(10*attempt)
        for url in urls:
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "prospero-dist"})
                with urllib.request.urlopen(req, timeout=120) as resp:
                    data = resp.read()
            except OSError as e:
                say(f"{name}: {url}: {e}")
                continue
            if sha256(data) != digest:
                sys.exit(f"dist: {name}: {url} has SHA-256 {sha256(data)}, "
                         f"not the pinned {digest}")
            say(f"{name}: {url}")
            if cache:
                os.makedirs(cache, exist_ok=True)
                base = url.rstrip("/").rsplit("/", 1)[-1]
                p = os.path.join(cache, f"{d}-{base}")
                with open(p + ".tmp", "wb") as fp:
                    fp.write(data)
                os.replace(p + ".tmp", p)
            return data
    sys.exit(f"dist: {name}: no URL worked")


def normal(path):
    """A member path, checked to stay where it is put."""
    parts = [p for p in path.split("/") if p not in ("", ".")]
    if not parts or ".." in parts:
        raise ValueError(f"bad path in archive: {path!r}")
    return parts


def members(tar):
    """(path parts, member, data) for each entry; directories have no data."""
    r = []
    for m in tar:
        if m.isdir():
            r.append((normal(m.name), m, None))
        elif m.isfile():
            r.append((normal(m.name), m, tar.extractfile(m).read()))
        elif m.issym():
            r.append((normal(m.name), m, None))
        else:
            raise ValueError(f"{m.name}: unsupported entry type {m.type!r}")
    return r


def add(entries, parts, m, data):
    """Record an entry; directories are implied by their contents too."""
    for i in range(1, len(parts)):
        entries.setdefault(tuple(parts[:i]), ("dir", None))
    key = tuple(parts)
    if m.isdir():
        entries.setdefault(key, ("dir", None))
    elif key in entries:
        raise ValueError(f"{'/'.join(parts)}: two entries")
    elif m.issym():
        entries[key] = ("sym", m.linkname)
    else:
        entries[key] = ("file", (data, bool(m.mode & 0o111)))


def build(commit, cache):
    commit = git("rev-parse", "--verify", f"{commit}^{{commit}}").decode().strip()
    ver = version(commit)
    mtime = int(git("log", "-1", "--format=%ct", commit).decode())
    entries = {(): ("dir", None)}

    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", commit))) as tar:
        for parts, m, data in members(tar):
            if parts[0] == "deps":
                sys.exit("dist: the commit has a deps/ of its own")
            add(entries, parts, m, data)

    cached = index(cache)
    for dep in pins(commit):
        data = fetch(dep, cache, cached)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tar:
            got = members(tar)
        tops = {parts[0] for parts, _, _ in got}
        strip = len(tops) == 1 and all(len(p) > 1 or m.isdir() for p, m, _ in got)
        for parts, m, data in got:
            if strip:
                parts = parts[1:]
            if parts:
                add(entries, ["deps", dep[1]] + parts, m, data)

    prefix = f"prospero-{ver}"
    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=out,
                       mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": commit}) as tar:
            for key in sorted(entries):
                kind, value = entries[key]
                info = tarfile.TarInfo("/".join((prefix,) + key))
                info.mtime = mtime
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                if kind == "dir":
                    info.type, info.mode = tarfile.DIRTYPE, 0o755
                    tar.addfile(info)
                elif kind == "sym":
                    info.type, info.mode, info.linkname = tarfile.SYMTYPE, 0o777, value
                    tar.addfile(info)
                else:
                    data, exe = value
                    info.mode, info.size = (0o755 if exe else 0o644), len(data)
                    tar.addfile(info, io.BytesIO(data))
    return ver, out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", dest="out", help="output (default: prospero-VERSION.tar.gz)")
    ap.add_argument("--cache", help="dependency tarballs: looked for here by "
                                    "SHA-256, and downloads saved here")
    ap.add_argument("commit", nargs="?", default="HEAD")
    args = ap.parse_args()
    try:
        ver, data = build(args.commit, args.cache)
    except (ValueError, tarfile.TarError) as e:
        sys.exit(f"dist: {e}")
    out = args.out or f"prospero-{ver}.tar.gz"
    with open(out + ".tmp", "wb") as f:
        f.write(data)
    os.replace(out + ".tmp", out)
    say(f"{out}: {len(data)} bytes, sha256 {sha256(data)}")


if __name__ == "__main__":
    main()

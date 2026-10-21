#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Build the headless engines that "harness deploy" puts on remote machines.

  uv run tools/engines.py [--out DIR] [--linux HOST] [PLATFORM ...]

Each engine is a Release build with HARNESS_GUI=OFF, named
harness-engine-VERSION-OS-ARCH[.exe] in DIR (default: build/engines, where
build/harness looks for them) with a .sha256 file beside it, as sha256sum
writes it:

  macos-arm64     here, on an Apple Silicon Mac (macOS 11 and later)
  windows-x86_64  here, with mingw-w64 (Homebrew's): CRT-free
  windows-i686    the same, the XP engine (cmake/Toolchain-Mingw32.cmake),
                  its imports checked against XP SP3 by tests/imports.py
  linux-aarch64   on HOST over ssh (--linux, an aarch64 Linux machine with
  linux-x86_64    the x86_64-linux-gnu cross compiler): static, so that
                  any C library will do, and checked to run (x86_64 under
                  the host's QEMU binfmt, if it has one). Names resolve
                  through /etc/hosts and DNS, which glibc has built in;
                  mDNS (.local) needs the machine's NSS module, which a
                  static program loads only where glibc's version matches

The Linux builds get the source as an archive of the working tree (what
git tracks and what it would add, so uncommitted changes too) through
scp, into --remote-dir on HOST. --deps DIR names a directory of
FetchContent sources (mbedtls-src, cpp-httplib-src) for the builds here
and on HOST; without it CMake downloads them.
"""

import argparse
import hashlib
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATFORMS = ["macos-arm64", "linux-aarch64", "linux-x86_64",
             "windows-x86_64", "windows-i686"]


def say(msg):
    print(f"engines: {msg}", file=sys.stderr, flush=True)


def run(argv, **kw):
    say(" ".join(shlex.quote(a) for a in argv))
    subprocess.run(argv, check=True, **kw)


def version():
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        m = re.search(r"project\(\s*\w+\s+VERSION\s+([0-9.]+)", f.read())
    if not m:
        sys.exit("engines: no project VERSION in CMakeLists.txt")
    return m.group(1)


def depsargs(deps):
    """FetchContent overrides for the POSIX headless build's sources."""
    if not deps:
        return []
    return [f"-DFETCHCONTENT_SOURCE_DIR_MBEDTLS={deps}/mbedtls-src",
            f"-DFETCHCONTENT_SOURCE_DIR_CPP-HTTPLIB={deps}/cpp-httplib-src"]


def publish(out, ver, plat, path):
    """Copy an engine into out under its deployed name, with its hash."""
    name = f"harness-engine-{ver}-{plat}" + (".exe" if plat.startswith("windows") else "")
    dst = os.path.join(out, name)
    tmp = dst + ".tmp"
    shutil.copyfile(path, tmp)
    os.chmod(tmp, 0o755)
    os.replace(tmp, dst)
    with open(dst, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    with open(dst + ".sha256", "w") as f:
        f.write(f"{digest}  {name}\n")
    say(f"{name}: {os.path.getsize(dst)} bytes, sha256 {digest}")
    return dst


def cmakebuild(builddir, extra):
    run(["cmake", "-S", ROOT, "-B", builddir, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DHARNESS_GUI=OFF"] + extra)
    run(["cmake", "--build", builddir, "--target", "harness"])


def macos(args, ver):
    b = os.path.join(args.work, "macos-arm64")
    cmakebuild(b, ["-DCMAKE_OSX_ARCHITECTURES=arm64",
                   "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"] + depsargs(args.deps))
    exe = os.path.join(b, "harness")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    if out.strip() != f"harness {ver}":
        sys.exit(f"engines: {exe} --version says {out!r}")
    publish(args.out, ver, "macos-arm64", exe)


def windows(args, ver, arch):
    plat = f"windows-{arch}"
    tc = "Toolchain-Mingw64.cmake" if arch == "x86_64" else "Toolchain-Mingw32.cmake"
    b = os.path.join(args.work, plat)
    cmakebuild(b, [f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/{tc}",
                   "-DCMAKE_EXE_LINKER_FLAGS=-s"])
    exe = os.path.join(b, "harness.exe")
    run(["uv", "run", os.path.join(ROOT, "tests", "imports.py"), exe])
    publish(args.out, ver, plat, exe)


def sources():
    """The working tree's files: tracked, and untracked but not ignored."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard"], capture_output=True, check=True).stdout
    names = sorted({n for n in out.decode().split("\0") if n})
    return [n for n in names if os.path.isfile(os.path.join(ROOT, n))]


def linux(args, ver, plats):
    host, rdir = args.linux, args.remote_dir
    tarpath = os.path.join(args.work, "linux-src.tar")
    os.makedirs(args.work, exist_ok=True)
    with tarfile.open(tarpath, "w", format=tarfile.PAX_FORMAT) as tar:
        for n in sources():
            tar.add(os.path.join(ROOT, n), arcname=f"src/{n}", recursive=False)
        if args.deps:
            for d, name in (("mbedtls-src", "mbedtls"), ("cpp-httplib-src", "cpp-httplib")):
                tar.add(os.path.join(args.deps, d), arcname=f"src/deps/{name}")
    run(["ssh", host, f"rm -rf {rdir}/src && mkdir -p {rdir}"])
    run(["scp", "-q", tarpath, f"{host}:{rdir}/linux-src.tar"])
    builds = {
        "linux-aarch64": [],
        "linux-x86_64": [f"-DCMAKE_TOOLCHAIN_FILE={rdir}/src/cmake/Toolchain-Linux-x86_64.cmake"],
    }
    script = [f"set -e", f"cd {rdir}", "tar xf linux-src.tar"]
    for plat in plats:
        b = f"build-{plat}"
        flags = " ".join(builds[plat])
        script += [
            f"cmake -S src -B {b} -G Ninja -DCMAKE_BUILD_TYPE=Release -DHARNESS_GUI=OFF "
            f"-DHARNESS_STATIC=ON -DCMAKE_EXE_LINKER_FLAGS=-s {flags} >{b}.log 2>&1 "
            f"|| {{ tail -30 {b}.log; exit 1; }}",
            f"cmake --build {b} --target harness >>{b}.log 2>&1 "
            f"|| {{ tail -30 {b}.log; exit 1; }}",
            f"grep -i 'warning' {b}.log || true",
            f"file {b}/harness",
            f"{b}/harness --version",
        ]
    say(f"building {', '.join(plats)} on {host}")
    p = subprocess.run(["ssh", host, "sh -s"], input="\n".join(script) + "\n", text=True)
    if p.returncode != 0:
        sys.exit(f"engines: the build on {host} failed")
    for plat in plats:
        got = os.path.join(args.work, f"{plat}.bin")
        run(["scp", "-q", f"{host}:{rdir}/build-{plat}/harness", got])
        publish(args.out, ver, plat, got)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "engines"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build-engines"),
                    help="build directories here (default: build-engines)")
    ap.add_argument("--linux", default="rat.local",
                    help="ssh host for the Linux builds (default: rat.local)")
    ap.add_argument("--remote-dir", default="/tmp/harness-engines",
                    help="its build directory (default: /tmp/harness-engines)")
    ap.add_argument("--deps", help="FetchContent sources: mbedtls-src, cpp-httplib-src")
    ap.add_argument("platforms", nargs="*", help=" ".join(PLATFORMS))
    args = ap.parse_args()
    plats = args.platforms or PLATFORMS
    for p in plats:
        if p not in PLATFORMS:
            sys.exit(f"engines: unknown platform {p}: one of {' '.join(PLATFORMS)}")
    if args.deps:
        args.deps = os.path.abspath(args.deps)
    os.makedirs(args.out, exist_ok=True)
    ver = version()
    if "macos-arm64" in plats:
        macos(args, ver)
    for arch in ("x86_64", "i686"):
        if f"windows-{arch}" in plats:
            windows(args, ver, arch)
    lin = [p for p in plats if p.startswith("linux-")]
    if lin:
        linux(args, ver, lin)
    say(f"done: {args.out}")


if __name__ == "__main__":
    main()

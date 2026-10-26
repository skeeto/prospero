#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Build the engines and desktop helpers that "prospero deploy" puts remotely.

  uv run tools/engines.py [--out DIR] [--linux HOST] [PLATFORM ...]
  uv run tools/engines.py --docker [--zip] [--out DIR] [PLATFORM ...]

With --docker, as for a release, the Linux and Windows builds run in
the toolchain image (tools/toolchain/Dockerfile, built first, with
Docker): GCC 16 cross compilers, static musl for the Linux engines,
glibc 2.28 for their desktop helpers, and mingw-w64 as w64devkit has
it for Windows, all optimized for size. The macOS engine is built here.
--zip then packs them all as prospero-engines-VERSION.zip in DIR, to
unpack next to the program (tools/toolchain/README.txt).

Without it:

Each engine is a Release build with PROSPERO_GUI=OFF, named
prospero-engine-VERSION-OS-ARCH[.exe] in DIR (default: build/engines, where
build/prospero looks for them) with a .sha256 file beside it, as sha256sum
writes it:

  macos-arm64     here, on an Apple Silicon Mac (macOS 11 and later)
  windows-x86_64  here, with mingw-w64 (Homebrew's): CRT-free
  windows-i686    the same, the XP engine (cmake/Toolchain-Mingw32.cmake),
                  its imports checked against XP SP3 by tests/imports.py
  linux-aarch64   on HOST over ssh (--linux or $PROSPERO_LINUX_HOST, an
  linux-x86_64    aarch64 Linux machine with the x86_64-linux-gnu cross
                  compiler): static, so that any C library will do, and
                  checked to run (x86_64 under the host's QEMU binfmt, if
                  it has one). Names resolve through /etc/hosts and DNS,
                  which glibc has built in; mDNS (.local) needs the
                  machine's NSS module, which a static program loads
                  only where glibc's version matches

Beside each Linux engine goes its desktop helper,
prospero-desktop-VERSION-OS-ARCH, which deploy puts beside the engine
there: linked dynamically, since it loads the X11 libraries at run
time, and run once (--version). A Windows engine is its own helper
("prospero desktop"). macOS gets none: its helper's privacy grants
belong to Prospero.app, inside which it lives.

With no PLATFORM it builds them all, except that without a HOST it
leaves out the Linux ones, and says so.

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
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = "prospero-toolchain"
# FetchContent sources: as --deps holds them, and as deps/ names them
DEPS = [("mbedtls-src", "mbedtls"), ("cpp-httplib-src", "cpp-httplib"),
        ("wuffs-src", "wuffs")]
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


def publish(out, ver, plat, path, kind="engine"):
    """Copy an engine, or a desktop helper (kind "desktop"), into out
    under its release name, with its hash."""
    name = f"prospero-{kind}-{ver}-{plat}" + (".exe" if plat.startswith("windows") else "")
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


def cmakebuild(builddir, extra, targets=("prospero",)):
    run(["cmake", "-S", ROOT, "-B", builddir, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPROSPERO_GUI=OFF"] + extra)
    run(["cmake", "--build", builddir, "--target", *targets])


def macos(args, ver):
    b = os.path.join(args.work, "macos-arm64")
    size = ["-DCMAKE_C_FLAGS_RELEASE=-Os -DNDEBUG", "-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG",
            "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-dead_strip"] if args.docker else []
    cmakebuild(b, ["-DCMAKE_OSX_ARCHITECTURES=arm64",
                   "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"] + size + depsargs(args.deps))
    exe = os.path.join(b, "prospero")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    if out.strip() != f"prospero {ver}":
        sys.exit(f"engines: {exe} --version says {out!r}")
    publish(args.out, ver, "macos-arm64", exe)


def windows(args, ver, arch):
    plat = f"windows-{arch}"
    tc = "Toolchain-Mingw64.cmake" if arch == "x86_64" else "Toolchain-Mingw32.cmake"
    b = os.path.join(args.work, plat)
    # Its own desktop helper, "prospero desktop"
    cmakebuild(b, [f"-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/{tc}",
                   "-DCMAKE_EXE_LINKER_FLAGS=-s"])
    exe = os.path.join(b, "prospero.exe")
    run(["uv", "run", os.path.join(ROOT, "tests", "imports.py"), exe])
    publish(args.out, ver, plat, exe)


def sources():
    """The working tree's files: tracked, and untracked but not ignored."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard"], capture_output=True, check=True).stdout
    names = sorted({n for n in out.decode().split("\0") if n})
    return [n for n in names if os.path.isfile(os.path.join(ROOT, n))]


def stage(dst, deps):
    """A copy of the working tree's sources at dst, with the FetchContent
    sources from deps (as --deps names them) in dst/deps, if given."""
    if os.path.exists(dst):
        shutil.rmtree(dst)
    for n in sources():
        os.makedirs(os.path.join(dst, os.path.dirname(n)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, n), os.path.join(dst, n))
    if deps:
        for d, name in DEPS:
            if os.path.isdir(os.path.join(deps, d)):
                shutil.copytree(os.path.join(deps, d), os.path.join(dst, "deps", name))


def docker(args, ver, plats):
    """The Linux and Windows builds, in the toolchain image."""
    d = os.path.join(args.work, "docker")
    stage(os.path.join(d, "src"), args.deps)
    if os.path.exists(os.path.join(d, "out")):
        shutil.rmtree(os.path.join(d, "out"))
    run(["docker", "build", "-q", "-t", IMAGE, os.path.join(ROOT, "tools", "toolchain")],
        stdout=subprocess.DEVNULL)
    run(["docker", "run", "--rm", "-v", f"{os.path.abspath(d)}:/w", IMAGE,
         "sh", "/w/src/tools/toolchain/build.sh", "/w/src", "/w/out", *plats])
    for plat in plats:
        publish(args.out, ver, plat, os.path.join(d, "out", f"{plat}-engine.bin"))
        if plat.startswith("linux-"):
            publish(args.out, ver, plat, os.path.join(d, "out", f"{plat}-desktop.bin"),
                    "desktop")


def pack(args, ver, plats):
    """prospero-engines-VERSION.zip: the engines and helpers with their
    hashes under engines/, as deploy looks for them beside the program,
    and the licenses of what is linked into them. Sorted, with fixed
    times, so that the same files make the same zip."""
    top = f"prospero-engines-{ver}"
    files = []
    for name in sorted(os.listdir(args.out)):
        m = re.match(rf"prospero-(engine|desktop)-{re.escape(ver)}-([a-z0-9_]+-[a-z0-9_]+)", name)
        if m and m.group(2) in plats:
            files.append((f"{top}/engines/{name}", os.path.join(args.out, name)))
    lic = os.path.join(args.work, "docker", "out", "LICENSES")
    for name in sorted(os.listdir(lic)):
        files.append((f"{top}/LICENSES/{name}", os.path.join(lic, name)))
    files.append((f"{top}/README.txt", os.path.join(ROOT, "tools", "toolchain", "README.txt")))
    dst = os.path.join(args.out, f"{top}.zip")
    with zipfile.ZipFile(dst + ".tmp", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, path in sorted(files):
            info = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix, for the mode bits
            exe = "/engines/" in arc and not arc.endswith(".sha256")
            info.external_attr = (0o100755 if exe else 0o100644) << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())
    os.replace(dst + ".tmp", dst)
    with open(dst, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    say(f"{top}.zip: {os.path.getsize(dst)} bytes, sha256 {digest}")


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
            f"cmake -S src -B {b} -G Ninja -DCMAKE_BUILD_TYPE=Release -DPROSPERO_GUI=OFF "
            f"-DPROSPERO_STATIC=ON -DCMAKE_EXE_LINKER_FLAGS=-s {flags} >{b}.log 2>&1 "
            f"|| {{ tail -30 {b}.log; exit 1; }}",
            f"cmake --build {b} --target prospero prospero-desktop >>{b}.log 2>&1 "
            f"|| {{ tail -30 {b}.log; exit 1; }}",
            f"grep -i 'warning' {b}.log || true",
            f"file {b}/prospero {b}/prospero-desktop",
            f"{b}/prospero --version",
            # Linked dynamically, so a cross build needs its C library
            f"QEMU_LD_PREFIX=/usr/x86_64-linux-gnu {b}/prospero-desktop --version"
            if plat == "linux-x86_64" else f"{b}/prospero-desktop --version",
        ]
    say(f"building {', '.join(plats)} on {host}")
    p = subprocess.run(["ssh", host, "sh -s"], input="\n".join(script) + "\n", text=True)
    if p.returncode != 0:
        sys.exit(f"engines: the build on {host} failed")
    for plat in plats:
        for prog, kind in (("prospero", "engine"), ("prospero-desktop", "desktop")):
            got = os.path.join(args.work, f"{plat}-{kind}.bin")
            run(["scp", "-q", f"{host}:{rdir}/build-{plat}/{prog}", got])
            publish(args.out, ver, plat, got, kind)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "engines"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build-engines"),
                    help="build directories here (default: build-engines)")
    ap.add_argument("--linux", default=os.environ.get("PROSPERO_LINUX_HOST"),
                    help="ssh host for the Linux builds "
                         "(default: $PROSPERO_LINUX_HOST)")
    ap.add_argument("--remote-dir", default="/tmp/prospero-engines",
                    help="its build directory (default: /tmp/prospero-engines)")
    ap.add_argument("--deps", help="FetchContent sources: mbedtls-src, cpp-httplib-src "
                                   "(and wuffs-src, for --docker)")
    ap.add_argument("--docker", action="store_true",
                    help="build the Linux and Windows engines in the toolchain "
                         "image (tools/toolchain), as releases are")
    ap.add_argument("--zip", action="store_true",
                    help="then pack them as prospero-engines-VERSION.zip in --out "
                         "(needs --docker)")
    ap.add_argument("platforms", nargs="*", help=" ".join(PLATFORMS))
    args = ap.parse_args()
    plats = args.platforms or PLATFORMS
    for p in plats:
        if p not in PLATFORMS:
            sys.exit(f"engines: unknown platform {p}: one of {' '.join(PLATFORMS)}")
    if args.zip and not args.docker:
        sys.exit("engines: --zip needs --docker")
    lin = [p for p in plats if p.startswith("linux-")]
    if args.docker:
        if args.deps:
            args.deps = os.path.abspath(args.deps)
        os.makedirs(args.out, exist_ok=True)
        ver = version()
        if "macos-arm64" in plats:
            macos(args, ver)
        docker(args, ver, [p for p in plats if p != "macos-arm64"])
        if args.zip:
            pack(args, ver, plats)
        say(f"done: {args.out}")
        return
    if lin and not args.linux:
        if args.platforms:
            sys.exit("engines: the Linux builds need --linux HOST or $PROSPERO_LINUX_HOST")
        say("no --linux HOST or $PROSPERO_LINUX_HOST: leaving out the Linux builds")
        lin = []
    if args.deps:
        args.deps = os.path.abspath(args.deps)
    os.makedirs(args.out, exist_ok=True)
    ver = version()
    if "macos-arm64" in plats:
        macos(args, ver)
    for arch in ("x86_64", "i686"):
        if f"windows-{arch}" in plats:
            windows(args, ver, arch)
    if lin:
        linux(args, ver, lin)
    say(f"done: {args.out}")


if __name__ == "__main__":
    main()

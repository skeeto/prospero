#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Build the itch.io edition of Prospero, for macOS, Windows and Linux.

  uv run tools/itch.py [--commit REV] [--out DIR] [--notary-profile NAME]
                       [--only PLATFORM]... [--push]

The itch.io builds are plain "Prospero" (not the Community Edition of
a source build), with the engines zip embedded, so that deploy needs no
engines directory (PROSPERO_EDITION=itch, CMakeLists.txt). Built here,
on this Mac, from a source tarball of REV (tools/dist.py, default HEAD,
whose tracked files must be committed), as the GitHub release is:

  1. the engines, as tools/engines.py --docker builds them, the Windows
     ones signed with aas-sign, the macOS one with the Developer ID,
     then packed (engines.py --pack) as prospero-engines-VERSION.zip;
  2. Prospero.app with that zip embedded, signed with the Developer ID,
     tested, on a disk image that cmake/notarize.cmake signs, notarizes
     and staples (with --notary-profile, a profile from "xcrun notarytool
     store-credentials", or the PROSPERO_NOTARY_KEY variables);
  3. the Windows program with it embedded, built in the toolchain image
     (tools/toolchain), signed with aas-sign, as Prospero.exe in a zip
     (tools/winzip.py), as the GitHub release has it;
  4. the Linux GUI with it embedded, for x86_64 and aarch64, built in
     the toolchain image by its glibc 2.28 cross compilers, so that it
     runs on glibc 2.28 and later (RHEL 8 and its rebuilds among them,
     which the GitHub release's 2.34 leaves out), then checked, tested
     (x86_64's through Docker's emulation here) and packed by
     tools/linuxgui's build.sh, as the GitHub release's are.

Everything that signs must work: no aas-sign, no Developer ID identity
in the keychain, or no notary credentials, and nothing is built. The display is kept
awake while it builds: a locked screen hides the notary profile.

DIR (default: build-itch) gets prospero-VERSION-macos-arm64.dmg,
prospero-VERSION-windows-x86_64.zip,
prospero-VERSION-linux-x86_64.tar.gz,
prospero-VERSION-linux-aarch64.tar.gz and the engines zip, with .sha256
files. --push then uploads the dmg, the Windows zip, kept a zip, and
the Linux tarballs with butler, to the channels mac, windows, linux
and linux-arm64 of skeeto/prospero, at the version; without it, the
commands that would are printed. It needs an itch.io API key, from
"butler login" or in BUTLER_API_KEY, checked before the build.

--only builds just the platforms named (macos, windows, linux-x86_64,
linux-aarch64; repeated for more), for a test, never pushed, into
build-itch-test unless --out says otherwise, apart from the release's
files of the same names. Without macos or windows, nothing signs, so
nothing about signing is checked: the engines zip it embeds is then
unsigned, and is not put in DIR.
"""

import argparse
import glob
import hashlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDENTITY = "Developer ID Application: Christopher Wellons (A7AR282GVM)"
BUNDLE_ID = "com.nullprogram.prospero"
SIGNER = "CN=Christopher Wellons"
GAME = "skeeto/prospero"
IMAGE = "prospero-toolchain"
PLATFORMS = ["macos", "windows", "linux-x86_64", "linux-aarch64"]
# notarytool keeps a profile where it can be read only while the Mac is
# unlocked, and says a locked one is not there
LOCKED = ("itch: \"No Keychain password item found\" also means the screen is "
          "locked: unlock it before storing the profile again")


def say(msg):
    print(f"itch: {msg}", file=sys.stderr, flush=True)


def run(argv, **kw):
    say(" ".join(shlex.quote(str(a)) for a in argv))
    subprocess.run(argv, check=True, **kw)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def checks(args):
    """What signing needs, before anything is built."""
    if not shutil.which("docker"):
        sys.exit("itch: no docker, for the engines, Windows and Linux builds")
    if args.push and args.only:
        sys.exit("itch: --only builds are for tests, never pushed")
    if args.signs:
        if not shutil.which("aas-sign"):
            sys.exit("itch: no aas-sign on PATH, to sign the Windows programs")
        signchecks(args)
    if args.push:
        if not shutil.which("butler"):
            sys.exit("itch: --push needs butler (https://itch.io/docs/butler/)")
        creds = os.path.expanduser("~/Library/Application Support/itch/butler_creds")
        if not os.environ.get("BUTLER_API_KEY") and not os.path.exists(creds):
            sys.exit("itch: --push needs an itch.io API key: \"butler login\", "
                     "or BUTLER_API_KEY")
    if args.commit == "HEAD":
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain",
                                "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout
        if dirty.strip():
            sys.exit("itch: uncommitted changes, which a build of HEAD would leave out")


def signchecks(args):
    """The Developer ID and the notary's credentials."""
    ids = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                         capture_output=True, text=True).stdout
    if f'"{args.identity}"' not in ids:
        sys.exit(f"itch: no codesigning identity \"{args.identity}\" in the keychain")
    if not args.notary_profile and not os.environ.get("PROSPERO_NOTARY_KEY"):
        sys.exit("itch: no notary credentials: --notary-profile NAME, or "
                 "PROSPERO_NOTARY_KEY, PROSPERO_NOTARY_KEY_ID and PROSPERO_NOTARY_ISSUER")
    if args.notary_profile:
        r = subprocess.run(["xcrun", "notarytool", "history", "-p", args.notary_profile],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"itch: notary profile {args.notary_profile}: "
                     f"{(r.stdout + r.stderr).strip()}\n{LOCKED}")


def source(args):
    """The tarball of the commit, unpacked: its root and version."""
    tgz = os.path.join(args.work, "src.tar.gz")
    run(["uv", "run", os.path.join(ROOT, "tools", "dist.py"), "--cache",
         os.path.join(args.work, "deps"), "-o", tgz, args.commit])
    dst = os.path.join(args.work, "src")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    with tarfile.open(tgz) as tar:
        tar.extractall(dst, filter="data")
    (top,) = os.listdir(dst)
    src = os.path.join(dst, top)
    with open(os.path.join(src, "CMakeLists.txt")) as f:
        m = re.search(r"project\(\s*\w+\s+VERSION\s+([0-9.]+)", f.read())
    return src, m.group(1)


def pecheck(src, *argv):
    run(["uv", "run", os.path.join(src, "tools", "pecheck.py"), *argv])


def aassign(src, exes):
    """Sign with aas-sign, then check each signature, and that the signer
    changed nothing else (pecheck --unsigned)."""
    unsigned = os.path.join(os.path.dirname(exes[0]), "unsigned")
    if os.path.exists(unsigned):
        shutil.rmtree(unsigned)
    os.makedirs(unsigned)
    for exe in exes:
        shutil.copy2(exe, unsigned)
    run(["aas-sign", "sign", *exes])
    pecheck(src, "--signed", "--unsigned", unsigned, "--signer", SIGNER, *exes)


def engines(args, src, ver):
    """The engines zip, its Windows and macOS engines signed."""
    out = os.path.join(args.work, "engines")
    if os.path.exists(out):
        shutil.rmtree(out)
    # Its build directories too: each run's source is a fresh tarball,
    # whose directory is named for the version, which a cache from an
    # earlier one would not match
    work = os.path.join(args.work, "engines-work")
    if os.path.exists(work):
        shutil.rmtree(work)
    run(["uv", "run", os.path.join(src, "tools", "engines.py"), "--docker",
         "--out", out, "--work", work])
    if args.signs:
        aassign(src, sorted(glob.glob(os.path.join(out, f"prospero-engine-{ver}-windows-*.exe"))))
        mac = os.path.join(out, f"prospero-engine-{ver}-macos-arm64")
        run(["codesign", "--force", "--options", "runtime", "--timestamp",
             "--identifier", f"{BUNDLE_ID}.engine", "--sign", args.identity, mac])
        run(["codesign", "--verify", "--strict", mac])
    else:
        say("the engines are unsigned: a test build")
    run(["uv", "run", os.path.join(src, "tools", "engines.py"), "--pack", "--out", out,
         "--licenses", os.path.join(work, "docker", "out", "LICENSES")])
    return os.path.join(out, f"prospero-engines-{ver}.zip")


def macos(args, src, ver, zip):
    """The disk image, signed, notarized and stapled."""
    b = os.path.join(args.work, "macos")
    if os.path.exists(b):
        shutil.rmtree(b)
    run(["cmake", "-S", src, "-B", b, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DPROSPERO_EDITION=itch",
         f"-DPROSPERO_ENGINES_ZIP={zip}", f"-DPROSPERO_SIGN_IDENTITY={args.identity}",
         f"-DPROSPERO_BUNDLE_ID={BUNDLE_ID}",
         f"-DPROSPERO_NOTARY_PROFILE={args.notary_profile or ''}"])
    run(["cmake", "--build", b])
    run(["ctest", "--test-dir", b, "-R", "unit|selftest", "--output-on-failure"])
    app = os.path.join(b, "Prospero.app")
    exe = os.path.join(app, "Contents", "MacOS", "prospero")
    said = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    if said != f"prospero {ver}":
        sys.exit(f"itch: {exe} --version says {said!r}")
    run(["codesign", "--verify", "--deep", "--strict", app])
    run(["cmake", "--build", b, "--target", "package"])
    dmg = os.path.join(b, f"prospero-{ver}-macos-arm64.dmg")
    run(["xcrun", "stapler", "validate", dmg])
    return dmg


def windows(args, src, ver, zip):
    """The Windows program, in the toolchain image (engines.py built it
    from this source), then signed here, and zipped."""
    w = os.path.join(args.work, "windows")
    if os.path.exists(w):
        shutil.rmtree(w)
    os.makedirs(w)
    shutil.copy2(zip, os.path.join(w, "engines.zip"))
    script = (
        "set -e\n"
        "cmake -S /src -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release"
        " -DCMAKE_TOOLCHAIN_FILE=/src/cmake/Toolchain-Mingw64.cmake"
        " -DCMAKE_EXE_LINKER_FLAGS=-s -DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        " -DPROSPERO_EDITION=itch -DPROSPERO_ENGINES_ZIP=/w/engines.zip >/w/build.log 2>&1"
        " || { tail -30 /w/build.log; exit 1; }\n"
        "cmake --build /tmp/b --target prospero >>/w/build.log 2>&1"
        " || { tail -30 /w/build.log; exit 1; }\n"
        "if grep 'warning:' /w/build.log; then exit 1; fi\n"
        "cp /tmp/b/prospero.exe /w/\n"
    )
    run(["docker", "run", "--rm", "-v", f"{os.path.abspath(src)}:/src:ro",
         "-v", f"{os.path.abspath(w)}:/w", IMAGE, "sh", "-c", script])
    exe = os.path.join(w, f"prospero-{ver}-windows-x86_64.exe")
    os.replace(os.path.join(w, "prospero.exe"), exe)
    pecheck(src, "--machine", "x86_64", "--subsystem", "gui", exe)
    aassign(src, [exe])
    zip = os.path.join(w, f"prospero-{ver}-windows-x86_64.zip")
    run(["uv", "run", os.path.join(src, "tools", "winzip.py"), exe, zip])
    return zip


def linux(args, src, ver, zip, arch):
    """The Linux GUI for arch, in the toolchain image (engines.py built
    it), against glibc 2.28."""
    w = os.path.join(args.work, f"linux-{arch}")
    if os.path.exists(w):
        shutil.rmtree(w)
    os.makedirs(w)
    shutil.copy2(zip, os.path.join(w, "engines.zip"))
    env = {"LINUXGUI_ARCH": arch, "LINUXGUI_GLIBC": "2.28"}
    # The image is this Mac's architecture. The other's programs, the
    # selftest's own children among them, run through Docker's emulation
    # (binfmt_misc), given the sysroot's loader and libraries at their
    # paths, as its multiarch packages would put them. Its spawn timing
    # is the emulator's, which the selftest would measure.
    setup = ""
    host = platform.machine().replace("arm64", "aarch64")
    if arch != host:
        sysroot = f"/opt/glibc/{arch}-linux-gnu"
        setup = (f"for d in lib64 lib/ld-linux-aarch64.so.1 lib/{arch}-linux-gnu "
                 f"usr/lib/{arch}-linux-gnu; do "
                 f"[ -e /$d ] || [ ! -e {sysroot}/$d ] || ln -s {sysroot}/$d /$d; done && ")
        env["PROSPERO_SELFTEST_NOBENCH"] = "1"
    build = shlex.join(["sh", "/src/tools/linuxgui/build.sh", "/src", "/tmp/b", "/w/out",
                        "-DCMAKE_TOOLCHAIN_FILE=/src/cmake/Toolchain-Linux-cross.cmake",
                        f"-DPROSPERO_TARGET={arch}-linux-gnu",
                        "-DPROSPERO_EDITION=itch", "-DPROSPERO_ENGINES_ZIP=/w/engines.zip"])
    run(["docker", "run", "--rm", *[a for k, v in env.items() for a in ("-e", f"{k}={v}")],
         "-v", f"{os.path.abspath(src)}:/src:ro", "-v", f"{os.path.abspath(w)}:/w",
         IMAGE, "sh", "-c", setup + build])
    return os.path.join(w, "out", f"prospero-{ver}-linux-{arch}.tar.gz")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--commit", default="HEAD", help="what to build (default: HEAD)")
    ap.add_argument("--out", help="where it puts them (default: build-itch, "
                                  "or build-itch-test with --only)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build-itch-work"),
                    help="where it builds (default: build-itch-work)")
    ap.add_argument("--identity", default=IDENTITY, help="the Developer ID")
    ap.add_argument("--notary-profile", default=os.environ.get("PROSPERO_NOTARY_PROFILE"),
                    help="notarytool keychain profile (default: $PROSPERO_NOTARY_PROFILE)")
    ap.add_argument("--push", action="store_true", help="upload with butler")
    ap.add_argument("--only", action="append", choices=PLATFORMS, metavar="PLATFORM",
                    help=f"build just this one, for a test: {', '.join(PLATFORMS)}")
    args = ap.parse_args()
    # A test's own, never over the release's of the same version
    args.out = os.path.abspath(args.out or os.path.join(
        ROOT, "build-itch-test" if args.only else "build-itch"))
    args.work = os.path.abspath(args.work)
    plats = args.only or PLATFORMS
    args.signs = "macos" in plats or "windows" in plats

    checks(args)
    # The display kept awake until this exits, so that the screen does not
    # lock, which hides the notary profile minutes in (LOCKED)
    if shutil.which("caffeinate"):
        subprocess.Popen(["caffeinate", "-d", "-i", "-w", str(os.getpid())])
    os.makedirs(args.work, exist_ok=True)
    src, ver = source(args)
    zip = engines(args, src, ver)
    built = {}
    if "macos" in plats:
        built["macos"] = macos(args, src, ver, zip)
    if "windows" in plats:
        built["windows"] = windows(args, src, ver, zip)
    for arch in ("x86_64", "aarch64"):
        if f"linux-{arch}" in plats:
            built[f"linux-{arch}"] = linux(args, src, ver, zip, arch)

    # An unsigned engines zip stays in the work directory, apart from
    # the release's signed one
    os.makedirs(args.out, exist_ok=True)
    made = []
    for path in (*built.values(), *([zip] if args.signs else [])):
        dst = os.path.join(args.out, os.path.basename(path))
        shutil.copyfile(path, dst)
        name = os.path.basename(dst)
        with open(dst + ".sha256", "w") as f:
            f.write(f"{sha256(dst)}  {name}\n")
        made.append(dst)
        say(f"{name}: {os.path.getsize(dst)} bytes, sha256 {sha256(dst)}")
    if args.only:
        say(f"done: {args.out}")
        return

    # The Windows zip as a zip: butler unpacks a pushed .zip, or a
    # directory of just one, into its files, unless --no-auto-unzip
    winpush = os.path.join(args.work, "push-windows")
    if os.path.exists(winpush):
        shutil.rmtree(winpush)
    os.makedirs(winpush)
    shutil.copy2(made[1], winpush)
    pushes = [["butler", "push", made[0], f"{GAME}:mac", "--userversion", ver],
              ["butler", "push", "--no-auto-unzip", winpush, f"{GAME}:windows",
               "--userversion", ver],
              ["butler", "push", made[2], f"{GAME}:linux", "--userversion", ver],
              ["butler", "push", made[3], f"{GAME}:linux-arm64", "--userversion", ver]]
    for argv in pushes:
        if args.push:
            run(argv)
        else:
            say("to upload: " + " ".join(shlex.quote(a) for a in argv))
    say(f"done: {args.out}")


if __name__ == "__main__":
    main()

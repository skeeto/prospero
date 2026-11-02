#!/bin/sh
# Install what the Linux GUI builds with (build.sh) into Ubuntu 22.04,
# as root: the release's linux-gui job runs it in its container, and
# the Dockerfile here bakes it into an image for tools/itch.py.
#
#   sh tools/linuxgui/install.sh
#
# The GUI links libstdc++ and libgcc statically, its dependencies are
# static, and SDL loads X11, Wayland and GL at run time, so the only
# floor is this system's glibc. SDL builds its X11 and Wayland video,
# and its D-Bus (the portal's file dialogs and theme, fcitx), against
# these headers, and needs pkg-config, without which it quietly leaves
# out Wayland. Clang comes from apt.llvm.org (for #embed), its
# key checked, and CMake from Kitware (Ubuntu's 3.22 is too old),
# pinned by SHA-256, into /opt, linked from /usr/local/bin.
set -eu

CMAKE_VERSION=4.4.5
# apt.llvm.org's signing key
LLVM_KEY=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421

arch=$(uname -m)
case $arch in
x86_64)  cmake_sha256=ed85135c5a5135b45e50423cb406dea04ca3338da0b8ef3c9ede963559680be0 ;;
aarch64) cmake_sha256=301d5063ab259577c1b57ce5490b1aab9ea5c56dc4c822f73499553dab94925f ;;
*) echo "install.sh: no CMake pinned for $arch" >&2; exit 1 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg \
    ninja-build binutils pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxss-dev libxtst-dev libwayland-dev \
    libxkbcommon-dev wayland-protocols libdecor-0-dev libegl-dev \
    libgl-dev libdbus-1-dev
k=/usr/share/keyrings/llvm.gpg
curl -fsSL --retry 3 https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o "$k"
if [ "$(gpg --show-keys --with-colons "$k" | awk -F: '$1 == "fpr" {print $10; exit}')" != "$LLVM_KEY" ]; then
    echo "install.sh: apt.llvm.org's key is not $LLVM_KEY" >&2
    exit 1
fi
echo "deb [signed-by=$k] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main" \
    >/etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y --no-install-recommends clang-20

c=cmake-$CMAKE_VERSION-linux-$arch
curl -fsSL --retry 3 -o "/tmp/$c.tar.gz" \
    "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/$c.tar.gz"
echo "$cmake_sha256  /tmp/$c.tar.gz" | sha256sum -c
tar xzf "/tmp/$c.tar.gz" -C /opt
rm "/tmp/$c.tar.gz"
for t in cmake ctest cpack; do
    ln -sf "/opt/$c/bin/$t" "/usr/local/bin/$t"
done

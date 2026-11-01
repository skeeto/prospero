# The image that builds the itch.io edition's Linux GUI (tools/itch.py),
# for each architecture:
#
#   docker build --platform linux/amd64 -t prospero-linuxgui:x86_64 tools/linuxgui
#   docker build --platform linux/arm64 -t prospero-linuxgui:aarch64 tools/linuxgui
#
# The release's linux-gui job, which builds the Community Edition's,
# runs install.sh in the same Ubuntu 22.04, pinned by digest, so both
# editions build with the same tools and have the same glibc floor.
# Then build.sh builds, checks and packs, with the source mounted.
FROM ubuntu:22.04@sha256:e0f80c829509ef4df9e76bd8944eb7b8ec25cbb379b112a7f9448b2c77a77c9b
COPY install.sh /tmp/install.sh
RUN sh /tmp/install.sh && rm -rf /tmp/install.sh /var/lib/apt/lists/*

# SPDX-License-Identifier: GPL-2.0+
# Copyright (C) 2026 Richard Gibbs
#
# Cross-build environment for AmigaOne X5000 (Varisys Cyrus P5020/P5040) U-Boot.
#
# Target: mainline U-Boot v2021.04, board/varisys/cyrus, mpc85xx / e500mc.
# Toolchain: powerpc-linux-gnu- (Debian bullseye => GCC 10, contemporary
# with the 2021.04 tree). U-Boot is freestanding (-ffreestanding), so the glibc
# target prefix is fine; nothing links against glibc.
#
# Build:  docker build -t x5000-uboot-builder docker-x5000/
# Use (mount the source tree at /src; out-of-tree build keeps the source clean):
#   docker run --rm -v <u-boot tree>:/src -w /src x5000-uboot-builder bash -lc \
#     "make O=/tmp/obj Cyrus_P5020_SDCARD_defconfig && make O=/tmp/obj -j\$(nproc)"
# See docs/building.md for the full image build (tools-x5000/build_spl_img.sh).
FROM debian:bullseye-slim

ENV DEBIAN_FRONTEND=noninteractive

# Bullseye is no longer on the live Debian mirrors: use the archive, so the
# image stays rebuildable.
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' > /etc/apt/sources.list \
    && printf 'Acquire::Check-Valid-Until "false";\n' > /etc/apt/apt.conf.d/99archive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-powerpc-linux-gnu \
        make \
        bison \
        flex \
        bc \
        libssl-dev \
        python3 \
        python3-dev \
        python3-setuptools \
        swig \
        device-tree-compiler \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# U-Boot picks these up automatically; override on the command line if needed.
ENV CROSS_COMPILE=powerpc-linux-gnu-
ENV ARCH=powerpc

WORKDIR /src
CMD ["bash"]

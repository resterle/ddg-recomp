#!/bin/sh
# Tools for build-release.sh on Ubuntu 22.04 (the GitHub runner, or a container): LLVM 23 from
# apt.llvm.org (clang, lld and its static libraries: the program carries clang/LLVM, and the static
# Python from python-build-standalone is LLVM 20 bitcode) and what SDL3 needs.
set -e
SUDO=$([ "$(id -u)" = 0 ] || echo sudo)
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg make cmake ninja-build python3 python3-pip \
    git xz-utils zstd file binutils \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
    libgles-dev libdrm-dev libgbm-dev libudev-dev libdbus-1-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libzstd-dev zlib1g-dev >/dev/null
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor | $SUDO tee /usr/share/keyrings/llvm.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-23 main" |
    $SUDO tee /etc/apt/sources.list.d/llvm.list >/dev/null
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends clang-23 lld-23 llvm-23-dev libclang-23-dev libpolly-23-dev >/dev/null
echo "tools: $(clang-23 --version | head -1)"

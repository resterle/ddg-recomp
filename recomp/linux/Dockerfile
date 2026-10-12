# Build environment for aarch64 Linux handhelds (ArkOS and similar, glibc >= 2.28).
# Debian buster keeps the glibc requirement low; clang 16 from apt.llvm.org; SDL3 from source
# with the KMSDRM video driver (EGL/GLES and libdrm/libgbm are loaded at run time from the device).
FROM --platform=linux/arm64 debian:buster-slim

RUN printf '%s\n' "deb http://archive.debian.org/debian buster main" \
        "deb http://archive.debian.org/debian buster-updates main" \
        "deb http://archive.debian.org/debian-security buster/updates main" > /etc/apt/sources.list && \
    apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg build-essential pkg-config \
        python3 make patchelf \
        libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libudev-dev libasound2-dev && \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    echo "deb https://apt.llvm.org/buster/ llvm-toolchain-buster-16 main" > /etc/apt/sources.list.d/llvm.list && \
    apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends clang-16 lld-16 && \
    rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-aarch64.tar.gz | \
    tar xz -C /opt && ln -s /opt/cmake-3.28.6-linux-aarch64/bin/cmake /usr/local/bin/cmake

ARG SDL_VERSION=3.4.16
RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz | tar xz -C /tmp && \
    cmake -S /tmp/SDL3-${SDL_VERSION} -B /tmp/sdl-build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-16 -DCMAKE_INSTALL_PREFIX=/opt/sdl3 \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_KMSDRM=ON -DSDL_KMSDRM_SHARED=ON -DSDL_OPENGLES=ON -DSDL_OPENGL=OFF -DSDL_VULKAN=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_GPU=OFF -DSDL_RENDER_GPU=OFF \
        -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
        -DSDL_SNDIO=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=ON -DSDL_LIBUDEV=ON && \
    cmake --build /tmp/sdl-build -j8 && cmake --install /tmp/sdl-build && rm -rf /tmp/sdl-build /tmp/SDL3-*

# profile-guided optimisation (linux/pgo.sh): profile runtime and llvm-profdata
RUN apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends llvm-16 libclang-rt-16-dev && \
    rm -rf /var/lib/apt/lists/*

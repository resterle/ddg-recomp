# Build environment for the x86-64 Linux desktop version and for generating the game code
# (docker-build.sh). Ubuntu 22.04 keeps the glibc requirement at 2.35. SDL3 is built from source
# with X11, Wayland, Vulkan, OpenGL and ALSA/PulseAudio/PipeWire; their libraries are loaded at
# run time, so the bundled libSDL3.so.0 runs whatever the desktop has.
FROM --platform=linux/amd64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl build-essential pkg-config cmake ninja-build make zip \
        clang-15 lld-15 python3 python3-venv \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
        libgles-dev libdrm-dev libgbm-dev libudev-dev libdbus-1-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev && \
    rm -rf /var/lib/apt/lists/* && \
    ln -s /usr/bin/clang-15 /usr/local/bin/clang && ln -s /usr/bin/clang++-15 /usr/local/bin/clang++ && \
    ln -s /usr/bin/ld.lld-15 /usr/local/bin/ld.lld

# the translator's only dependency (recomp/requirements.txt)
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir capstone==5.0.9

ARG SDL_VERSION=3.4.16
RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz | tar xz -C /tmp && \
    cmake -S /tmp/SDL3-${SDL_VERSION} -B /tmp/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_INSTALL_PREFIX=/opt/sdl3 \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_LIBDECOR=ON -DSDL_KMSDRM=ON -DSDL_VULKAN=ON \
        -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_GPU=ON \
        -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF && \
    cmake --build /tmp/sdl-build && cmake --install /tmp/sdl-build && rm -rf /tmp/sdl-build /tmp/SDL3-*

ENV PKG_CONFIG_PATH=/opt/sdl3/lib/pkgconfig CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld

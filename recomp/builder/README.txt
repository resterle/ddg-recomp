ddg-recomp builder - Densha de GO! FINAL (PC), recompiled for Linux handhelds and desktops

This folder builds the game from your own perfect.exe. It contains no part of the game: the
exe's code is translated and compiled here, on your computer, which takes a few minutes. Nothing
else needs to be installed (no Docker, Python or compiler).

You need
  The PC version of Densha de GO! FINAL: the cddata folder and an unprotected perfect.exe. The
  perfect.exe on the CD is protected by SafeDisc and cannot be used; a copy patched by
  DDGModernizer works.

Building
  Linux / macOS:  ./ddg-builder handheld /path/to/perfect.exe
                  ./ddg-builder desktop  /path/to/perfect.exe
  Windows:        drag perfect.exe onto ddg-builder.bat (builds the handheld package), or in a
                  command prompt: ddg-builder.bat handheld C:\path\to\perfect.exe

  handheld  R36H/R36S and other ArkOS-style handhelds -> ddg-recomp-arm64.zip
            Copy the zip's contents into the SD card's "ports" folder, put cddata and perfect.exe
            into ports/ddg-recomp/gamedata/ and restart EmulationStation.
  desktop   x86-64 Linux (Vulkan)                     -> ddg-recomp-linux-x64.tar.gz
            Unpack and run: ddg-recomp/perfect_recomp "/path/to/the game folder"

  The package is written next to this file (--out <folder> for another place). The first build
  also prepares the target's C library (kept in cache/ for later builds).

The package contains code translated from your perfect.exe: do not share it.

Contents (and their licences)
  python/    portable CPython (python-build-standalone; PSF licence)
  zig/       zig toolchain with clang and lld (MIT; LLVM: Apache 2.0 with LLVM exception)
  recomp/    the translator (ddg-recomp) and capstone (BSD) in recomp/pylib
  targets/   per target the project's own code, compiled, and SDL3 (zlib licence)
Project page and the manual build: https://github.com/resterle/ddg-recomp

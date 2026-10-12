ddg-recomp: Densha de GO! FINAL (recompiled) - Linux desktop (x86-64, Vulkan)

You need your own copy of the PC game (perfect.exe and the cddata folder).

Start:   ./perfect_recomp "<folder with perfect.exe and cddata>"
         The settings window (game folder, resolution, aspect, draw distance, fullscreen) opens
         first; hold Shift at start or pass --settings to get it back, --play to skip it.

Settings and saves: ~/.local/share/TAITO/DengoFinal/ (graphics.ini, written by the settings
window). Other graphics.ini keys:
  drawdistance = 0.90  scenery distance (0.9 is the PC original)
  frameskip = 0        skip drawing up to n frames in a row when the PC falls behind
  maxfps = 0           draw at most n frames per second (30, 20, 15; 0 = 60)
  texformat = 1        32-bit textures (0 = paletted, as the original)

Graphics: SDL GPU on Vulkan (any current Mesa or NVIDIA driver). RECOMP_GPU_DEBUG=1 turns on
SDL's GPU debug mode (validation layers when installed). libs/libSDL3.so.0 is the SDL3 of the
build machine; delete it to use the system's.

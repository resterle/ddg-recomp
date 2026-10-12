ddg-recomp: Densha de GO! FINAL (recompiled) for ArkOS handhelds (R36H/R36S, RK3326, aarch64)

Install
  1. Copy everything in this package into the "ports" folder of the SD card
     (EASYROMS/ports on ArkOS; /roms2/ports when you use a second card).
  2. Copy from your PC installation of Densha de GO! FINAL into ports/ddg-recomp/gamedata/:
       - the folder  cddata   (about 2.6 GB)
       - perfect.exe         (the unprotected executable; the SafeDisc-protected one from the
                              CD does not work, a copy patched by DDGModernizer does)
  3. Restart EmulationStation (or the device). "Densha de GO! FINAL" appears under Ports.
     Updating from a version that used the folder ports/densha: the first start moves its
     gamedata, settings and saves into ports/ddg-recomp; delete ports/densha afterwards.

Controls (keyboard keys in brackets)
  D-pad / left stick   menu, master controller          [arrow keys]
  A / Start            confirm                          [Return]
  B / Select           back                             [Escape]
  X                    [Space]
  L1 / R1              [Page Up / Page Down]
  L2 / R2              [Home / End]
  Start + Select       quit
  R3 (right stick)     screenshot (ddg-recomp/snapshot/)

Files
  ddg-recomp/conf/graphics.ini   settings (draw distance, aspect, texture format, ...)
  ddg-recomp/conf/shaders.bin    compiled shaders (made on the first run, renewed when the driver changes)
  ddg-recomp/conf/AppData/       save data
  ddg-recomp/log.txt             log of the last run (send this along with bug reports)

Renderer: OpenGL ES 3.0 (Mali-G31). Sound through ALSA.

Speed settings (ddg-recomp/conf/graphics.ini)
  frameskip = 3        skip drawing up to 3 frames in a row when the device falls behind, so the
                       game keeps full speed (0 = draw every frame, slow motion when too slow)
  drawdistance = 0.40  scenery distance (the PC original used 0.9; higher costs speed)
  standins = 1         new scenery textures that are not uploaded yet are drawn blurred (or, with
                       transparency, not at all) for a few frames; 0 = wait (short freezes)
  maxfps = 0           draw at most this many frames per second (30, 20 or 15; 0 = 60). The game
                       still runs at full speed. On the R36H, driving with drawdistance = 0.90
                       runs at about 60 (with vsync = 0); maxfps = 30 saves heat and battery
  texformat = 1        32-bit textures (0 = paletted: less memory but much slower on this GPU)
  batching = 1         sort and merge model draws (far fewer draw calls)
  fastalpha = 1        skip blending where it cannot change a pixel (harder edges on foliage)
  mipmaps = 1          smoother, faster distant textures
  vsync = 1            0 = don't wait for the display refresh (may tear)
  threaded = 1         draw on a second CPU core while the game runs on, and hand finished frames to
                       a third thread that waits for the display (0 = old single-core path)
  tex16 = 0            1 = opaque textures in 16-bit colour (faster loading, slight banding)
  fps = 0              1 = frames-per-second counter in the top-left corner (frames shown)
  texarrays = 0        1 = textures in texture arrays (fewer draw calls, but slower on the Mali-G31)
  scrwidth / scrheight 640 x 480; 320 x 240 renders a quarter of the pixels (blurrier)

Troubleshooting
  Wrong-looking surfaces: try fastalpha = 0, then mipmaps = 0, then batching = 0, one at a time,
  and send ddg-recomp/log.txt plus an R3 screenshot (ddg-recomp/snapshot/).
  Crashes: log.txt ends with a crash report (signal, backtrace); send the whole file.
  log.txt has a timing report every 10 s (ticks/s should be 60) and any GL errors.

# Recompiled game: translated C (build/game) + host runtime + ported PC layer (../port).
# Linux: SDL3 from pkg-config, or SDL_PREFIX=<install dir>. RENDERER=gl uses the OpenGL ES 3.0
# backend (GraphicGL.cpp) instead of SDL GPU, e.g. for Mali handhelds without Vulkan.
ifeq ($(origin CC),default)
CC         := clang
endif
ifeq ($(origin CXX),default)
CXX        := clang++
endif
RENDERER   ?= gpu
ifeq ($(shell uname -s),Darwin)
SDL_PREFIX ?= $(shell brew --prefix sdl3 2>/dev/null)
endif
ifneq ($(SDL_PREFIX),)
SDL_CFLAGS := -I$(SDL_PREFIX)/include
SDL_LIBS   := -L$(SDL_PREFIX)/lib -lSDL3 -Wl,-rpath,$(SDL_PREFIX)/lib
else
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS   := $(shell pkg-config --libs sdl3)
endif
OPT        ?= -O2
# Fixed address of the emulated 4 GB. macOS refuses fixed allocations below 0x7000000000 and puts
# malloc's regions at random places in 0x7000000000-0x8000000000 before main runs (sometimes over a
# base there), so it goes to 1 TB; Linux kernels with 39-bit address spaces (common on ARM
# handhelds) end at 0x8000000000.
ifeq ($(shell uname -s),Darwin)
MEMBASE    ?= 0x10000000000
else
MEMBASE    ?= 0x400000000
endif
DEFS       := -DRT_MEM_BASE=$(MEMBASE)
GFX        := $(if $(filter gl,$(RENDERER)),GraphicGL,Graphic)
GFXDEFS    := $(if $(filter gl,$(RENDERER)),-DRECOMP_GL,)
# objects per renderer/target (native.o differs); OBJ=build/obj-<name> for cross builds
OBJ        ?= build/obj$(if $(filter gl,$(RENDERER)),-gl,)
TARGET     ?= perfect_recomp$(if $(filter gl,$(RENDERER)),_gl,)
# ThinLTO for the translated code (cross-file inlining of small functions); LTO=0 to disable
LTO        ?= 1
LTOFLAGS   := $(if $(filter 1,$(LTO)),-flto=thin,)
# profile-guided optimisation (linux/pgo.sh): -fprofile-generate / -fprofile-use=<file>
PGOFLAGS   ?=
# CPU tuning for all objects (e.g. -mcpu=cortex-a35 for RK3326 handhelds)
TUNE       ?=

GAME_SRC := $(wildcard build/game/*.c)
GAME_OBJ := $(patsubst build/game/%.c,$(OBJ)/game/%.o,$(GAME_SRC))
HOST_OBJ := $(OBJ)/host/runtime.o $(OBJ)/host/win32.o $(OBJ)/host/native.o $(OBJ)/host/launcher.o $(OBJ)/host/overlay.o $(OBJ)/host/d3dx.o $(OBJ)/host/dsound.o $(OBJ)/host/tasks.o
PORT_SRC := $(GFX) Texture TexturePack Gs Dma Vertex
# RECOMP_JIT=1: the program without the game code. On the first start with an exe it translates
# it with the built-in Python (host/pyembed.cpp), compiles the C with the built-in clang/LLVM and
# runs it with LLVM's JIT, caching both (host/jit.cpp; prototype). LLVM_CONFIG: the LLVM to build
# against; PY_PREFIX: a python-build-standalone "full" distribution (static libpython);
# CAPSTONE_WHEEL: capstone for the target.
RECOMP_JIT ?= 0
ifeq ($(RECOMP_JIT),1)
LLVM_CONFIG ?= llvm-config
PY_PREFIX  ?= build/kit-cache/unpacked/pbs-full/python
CAPSTONE_WHEEL ?= $(firstword $(wildcard build/kit-cache/capstone-linux-x64/*.whl))
DEFS       += -DRECOMP_JIT
GAME_OBJ   :=
HOST_OBJ   += $(OBJ)/host/jit.o $(OBJ)/host/rt_header.o $(OBJ)/host/pyembed.o $(OBJ)/py/pyconfig.o
# libpython and what it needs: $(OBJ)/py/link.txt (host/mkpyruntime.py), read when linking
PY_LIBS    = $$(cat $(OBJ)/py/link.txt)
LLVM_SYSLIBS ?=
ifneq ($(LLVM_PREFIX),)
# a static LLVM/clang install (cmake install of the libraries only, without llvm-config): all its
# archives in one group, the linker takes what is used
JIT_CXXFLAGS := -I$(LLVM_PREFIX)/include -std=c++17 -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -fno-exceptions
# exported for the game code (the JIT finds the host by dlsym): only the runtime's interface, so
# that the linker can drop the unused parts of LLVM and clang. LLVM_SYSLIBS: what the LLVM build
# needs besides (llvm-config --link-static --system-libs, e.g. -lz -lzstd)
LLVM_ARCHIVES := $(filter-out %/libclang.a,$(wildcard $(LLVM_PREFIX)/lib/libclang*.a $(LLVM_PREFIX)/lib/libLLVM*.a))
ifeq ($(shell uname -s),Darwin)
JIT_LIBS   = $(PY_LIBS) $(LLVM_ARCHIVES) $(LLVM_SYSLIBS) -Wl,-exported_symbols_list,host/jit-exports-macos.txt -Wl,-dead_strip -lpthread -lm
else
JIT_EXPORTS := -Wl,--export-dynamic-symbol=N_* -Wl,--export-dynamic-symbol=R -Wl,--export-dynamic-symbol=RT_* -Wl,--export-dynamic-symbol=rt_*
JIT_LIBS   = $(PY_LIBS) -Wl,--start-group $(LLVM_ARCHIVES) -Wl,--end-group $(LLVM_SYSLIBS) $(JIT_EXPORTS) -Wl,--gc-sections -ldl -lpthread -lm
endif
else
JIT_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
JIT_LIBS   = $(PY_LIBS) $(shell $(LLVM_CONFIG) --ldflags) -lclang-cpp $(shell $(LLVM_CONFIG) --libs) -rdynamic -ldl -lpthread -lm
endif
endif
PORT_OBJ := $(patsubst %,$(OBJ)/port/%.o,$(PORT_SRC))

$(TARGET): $(GAME_OBJ) $(HOST_OBJ) $(PORT_OBJ) $(if $(filter 1,$(RECOMP_JIT)),$(OBJ)/py/link.txt)
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) $(LTOFLAGS) -o $@ $(filter %.o,$^) $(SDL_LIBS) $(JIT_LIBS) $(LDEXTRA)

$(OBJ)/game/%.o: build/game/%.c recomp_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(PGOFLAGS) $(TUNE) $(OPT) $(DEFS) $(LTOFLAGS) -g0 -w -fno-strict-aliasing -MMD -MP -I. -Ibuild/game -c $< -o $@

$(OBJ)/host/%.o: host/%.c host/runtime.h recomp_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(PGOFLAGS) $(TUNE) -O2 $(DEFS) -g -Wall -fno-strict-aliasing -MMD -MP -I. -c $< -o $@

$(OBJ)/host/native.o: host/native.cpp host/runtime.h recomp_rt.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 $(DEFS) $(GFXDEFS) -g -Wall -MMD -MP -I. $(SDL_CFLAGS) -c $< -o $@

$(OBJ)/host/tasks.o: host/tasks.cpp host/runtime.h recomp_rt.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 $(DEFS) -g -Wall -MMD -MP -I. -c $< -o $@

$(OBJ)/host/d3dx.o: host/d3dx.cpp host/runtime.h recomp_rt.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 $(DEFS) -g -Wall -MMD -MP -I. -c $< -o $@

$(OBJ)/host/dsound.o: host/dsound.cpp host/runtime.h recomp_rt.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 $(DEFS) -g -Wall -MMD -MP -I. $(SDL_CFLAGS) -c $< -o $@

$(OBJ)/host/launcher.o: host/launcher.cpp host/launcher.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 -g -Wall -MMD -MP $(SDL_CFLAGS) -c $< -o $@

$(OBJ)/host/overlay.o: host/overlay.cpp host/overlay.h host/launcher.h
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 -g -Wall -MMD -MP $(SDL_CFLAGS) -c $< -o $@

$(OBJ)/host/jit.o: host/jit.cpp recomp_rt.h
	@mkdir -p $(dir $@)
	$(CXX) $(TUNE) -O2 $(DEFS) -g -Wall $(JIT_CXXFLAGS) -c $< -o $@

# recomp_rt.h as a string for jit.cpp (the translated code's header)
$(OBJ)/host/rt_header.c: recomp_rt.h
	@mkdir -p $(dir $@)
	python3 -c "import sys,json; print('const char rt_jit_header[] = ' + json.dumps(open(sys.argv[1]).read()) + ';')" $< > $@

$(OBJ)/host/rt_header.o: $(OBJ)/host/rt_header.c
	$(CC) -c $< -o $@

# the translator runtime (stdlib subset, translator, capstone) and Python's module table
$(OBJ)/py/runtime.bin $(OBJ)/py/pyconfig.c $(OBJ)/py/link.txt: host/mkpyruntime.py $(wildcard *.py data/*) build/discovered.txt
	$(PY_PREFIX)/install/bin/python3 host/mkpyruntime.py $(PY_PREFIX) $(CAPSTONE_WHEEL) $(OBJ)/py

$(OBJ)/py/pyconfig.o: $(OBJ)/py/pyconfig.c
	$(CC) -O2 -I$(PY_PREFIX)/install/include/python3.12 -I$(PY_PREFIX)/install/include/python3.12/internal -DPy_BUILD_CORE -c $< -o $@

$(OBJ)/host/pyembed.o: host/pyembed.cpp $(OBJ)/py/runtime.bin
	@mkdir -p $(dir $@)
	$(CXX) $(TUNE) -std=c++17 -O2 -g -Wall -I$(PY_PREFIX)/install/include/python3.12 -DRT_PY_RUNTIME='"$(abspath $(OBJ)/py/runtime.bin)"' -c $< -o $@

$(OBJ)/port/%.o: ../port/src/unb/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PGOFLAGS) $(TUNE) -std=c++17 -O2 -g -w -MMD -MP $(SDL_CFLAGS) -c $< -o $@

# header dependencies (generated by -MMD)
-include $(GAME_OBJ:.o=.d) $(HOST_OBJ:.o=.d) $(PORT_OBJ:.o=.d)

# host and port objects only (builder/make-kit.sh: linked with the game code on the user's machine)
hostobjs: $(HOST_OBJ) $(PORT_OBJ)

clean:
	rm -rf build/obj build/obj-* perfect_recomp perfect_recomp_gl
.PHONY: clean hostobjs

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -g
SDL_PREFIX ?= $(shell brew --prefix sdl3 2>/dev/null)
SDL      := $(if $(shell command -v pkg-config),$(shell pkg-config --cflags --libs sdl3),-I$(SDL_PREFIX)/include -L$(SDL_PREFIX)/lib -lSDL3 -Wl,-rpath,$(SDL_PREFIX)/lib)

UNB_SRC := src/unb/Graphic.cpp src/unb/Texture.cpp src/unb/Gs.cpp src/unb/Dma.cpp src/unb/Vertex.cpp
UNB_HDR := src/unb/Graphic.h src/unb/GraphicShaders.h src/unb/Texture.h src/unb/Gs.h src/unb/Dma.h src/unb/Vertex.h demo/PacketBuilder.h

all: graphic_demo gs_demo dma_demo model_demo txm2png cull_test

graphic_demo: demo/main.cpp $(UNB_SRC) $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -o $@ demo/main.cpp $(UNB_SRC) $(SDL)

gs_demo: demo/gs_demo.cpp $(UNB_SRC) $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -o $@ demo/gs_demo.cpp $(UNB_SRC) $(SDL)

dma_demo: demo/dma_demo.cpp $(UNB_SRC) $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -o $@ demo/dma_demo.cpp $(UNB_SRC) $(SDL)

model_demo: demo/model_demo.cpp $(UNB_SRC) $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -o $@ demo/model_demo.cpp $(UNB_SRC) $(SDL)

txm2png: tools/txm2png.cpp src/unb/Texture.cpp $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -DTXM_TOOL -o $@ tools/txm2png.cpp src/unb/Texture.cpp src/unb/Graphic.cpp $(SDL)

cull_test: tools/cull_test.cpp $(UNB_SRC) $(UNB_HDR)
	$(CXX) $(CXXFLAGS) -o $@ tools/cull_test.cpp $(UNB_SRC) $(SDL)

clean:
	rm -rf graphic_demo gs_demo dma_demo model_demo txm2png cull_test *.dSYM
.PHONY: all clean

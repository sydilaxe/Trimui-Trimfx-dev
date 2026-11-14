# TrimFX build - TrimUI Brick Pro (and Brick / Brick Hammer), MustardOS
#   make              cross-compile with zig (aarch64, glibc 2.28, against the device's SDL2) -> build/trimui/trimfx
#   make package      build/TrimFX-mustardos.muxapp (copy into ARCHIVE/, install from the Archive Manager)
#   make desktop      a build for this computer (SDL2 dev files): TFX_WINDOWED=1 build/desktop/trimfx
#   make test         the engine tests (every algorithm; ASan/UBSan)
#   make shots        desktop screenshots of every page -> build/shots/
#   make wavs         a WAV of every algorithm on a test signal -> build/wavs/
#   make install SD=/path/to/card
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG      ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
ZTARGET  := aarch64-linux-gnu.2.28
CPU      := cortex_a53
APP      := trimfx
NAME     := TrimFX
SDL_INC  := third_party/SDL2-2.30.9-headers
VERSION  ?= $(shell sed -n 's/^\#define TFX_VERSION "\([^"]*\)".*/\1/p' src/main.c)
DSP      := src/engine.c src/tone.c src/modu.c src/delay.c src/verb.c src/bloom.c src/midiverb2.c src/master/master.c
SRC      := src/main.c $(DSP) src/brick_input.c src/perflog.c src/ringleds.c src/gfx.c
HDR      := $(wildcard src/*.h src/master/*.h)
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation \
            -ffast-math -DTFX_VERSION='"$(VERSION)"'
HOST_CC  ?= cc

DEV_CC   = $(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)
DEV_OBJ  := $(patsubst src/%.c,build/trimui/%.o,$(SRC))

all: build/trimui/$(APP)

build/trimui/$(APP): $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0
	$(DEV_CC) -o $@ $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -Wl,--allow-shlib-undefined -s

build/trimui/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(DEV_CC) $(CFLAGS) -I$(SDL_INC) -c -o $@ $<

# a stand-in for the device's libSDL2-2.0.so.0, to link against (the firmware has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py $(SRC)
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c $(SRC)
	$(ZIG) cc -target $(ZTARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# --- desktop
HOST_OBJ := $(patsubst src/%.c,build/desktop/%.o,$(SRC))
desktop: build/desktop/$(APP)
build/desktop/$(APP): $(HOST_OBJ)
	$(HOST_CC) -o $@ $(HOST_OBJ) $(shell sdl2-config --libs) -lm -lpthread
build/desktop/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) $(shell sdl2-config --cflags | sed 's|-I\([^ ]*\)/SDL2|-I\1|') -c -o $@ $<

# --- tests
build/test_engine: tests/test_engine.c $(DSP) $(HDR)
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o $@ tests/test_engine.c $(DSP) -lm
build/bench_engine: tests/test_engine.c $(DSP) $(HDR)
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O2 -ffast-math -o $@ tests/test_engine.c $(DSP) -lm
test: build/test_engine
	build/test_engine
wavs: build/bench_engine
	@mkdir -p build/wavs
	build/bench_engine -w build/wavs
shots: build/desktop/$(APP)
	@mkdir -p build/shots
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TFX_SHOT=build/shots/trimfx TFX_DIR=build build/desktop/$(APP)

# --- the MustardOS package: <Name>/ with the binary, launch.sh, mux_launch.sh, wake_screen.sh, the glyph
package: build/$(NAME)-mustardos.muxapp
build/$(NAME)-mustardos.muxapp: build/trimui/$(APP) $(shell find Apps/$(NAME) mustardos -type f) LICENSE README.md
	rm -rf build/mux $@
	mkdir -p build/mux/$(NAME)
	cp -r Apps/$(NAME)/. build/mux/$(NAME)/
	cp -r mustardos/. build/mux/$(NAME)/
	rm -f build/mux/$(NAME)/state.txt build/mux/$(NAME)/log.txt
	cp build/trimui/$(APP) build/mux/$(NAME)/$(APP)
	cp LICENSE build/mux/$(NAME)/LICENSE.txt
	cp src/master/LICENSE-SP1Master.txt build/mux/$(NAME)/
	cp README.md build/mux/$(NAME)/README.md
	echo $(VERSION) > build/mux/$(NAME)/VERSION
	chmod +x build/mux/$(NAME)/launch.sh build/mux/$(NAME)/mux_launch.sh build/mux/$(NAME)/wake_screen.sh build/mux/$(NAME)/$(APP)
	cd build/mux && zip -qr ../$(NAME)-mustardos.muxapp $(NAME)
	@echo "$@: copy it into ARCHIVE/ on the MustardOS card and install it from the Archive Manager"

install: package
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/card" && exit 1)
	@test -d "$(SD)/MUOS" || (echo "$(SD)/MUOS not found - is this the MustardOS card?" && exit 1)
	mkdir -p "$(SD)/MUOS/application/$(NAME)" && cp -r build/mux/$(NAME)/. "$(SD)/MUOS/application/$(NAME)/"
	sync

version:
	@echo $(VERSION)
clean:
	rm -rf build
.PHONY: all desktop test wavs shots package install version clean

# TrimFX build - TrimUI Brick Pro (and Brick / Brick Hammer), MustardOS
#   make              cross-compile with zig (aarch64, glibc 2.28, against the device's SDL2) -> build/trimui/trimfx
#   make package      build/TrimFX-mustardos.muxapp (copy into ARCHIVE/, install from the Archive Manager)
#   make desktop      a build for this computer (SDL2 dev files): TFX_WINDOWED=1 build/desktop/trimfx
#   make test         the engine tests (every algorithm; ASan/UBSan)
#   make shots        desktop screenshots of every page -> build/shots/
#   make wavs         a WAV of every algorithm on a test signal -> build/wavs/
#   make update-test  install a fake release over a test app folder, the way the app updates itself
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
# lib/brickio: USB audio and MIDI for the TrimUI apps, shared (see lib/brickio/README.md)
BRICKIO  := lib/brickio/usbaudio.c lib/brickio/aring.c lib/brickio/duplex.c lib/brickio/midi.c
SRC      := src/main.c $(DSP) src/brick_input.c src/perflog.c src/ringleds.c src/gfx.c src/net.c src/update.c $(BRICKIO)
HDR      := $(wildcard src/*.h src/master/*.h lib/brickio/*.h)
CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation \
            -ffast-math -DTFX_VERSION='"$(VERSION)"' -Ilib/brickio
HOST_CC  ?= cc
.DEFAULT_GOAL := all

# --- Mbed TLS (Apache-2.0) for the updater's HTTPS, from a pinned release, not committed; each toolchain
# builds its own libmbedtls.a from it with tools/mbedtls.mk (as in Conduit, Columbia Street, RadioScan, Jaffedrone)
MBED_VER := 3.6.7
MBED_SHA := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBED_DIR := third_party/mbedtls
MBED_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBED_VER)/mbedtls-$(MBED_VER).tar.bz2
MBED_SRC := $(MBED_DIR)/.v$(MBED_VER)
$(MBED_SRC):
	rm -rf $(MBED_DIR) && mkdir -p $(MBED_DIR)
	curl -sSL -o $(MBED_DIR).tar.bz2 $(MBED_URL) || wget -qO $(MBED_DIR).tar.bz2 $(MBED_URL)
	echo "$(MBED_SHA)  $(MBED_DIR).tar.bz2" | sha256sum -c --quiet || (rm -f $(MBED_DIR).tar.bz2; false)
	tar -xjf $(MBED_DIR).tar.bz2 -C $(MBED_DIR) --strip-components=1
	rm $(MBED_DIR).tar.bz2
	touch $@

build/zig/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/zig MBED_DIR=$(MBED_DIR) \
		CC="$(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)" AR="$(ZIG) ar" MBED_CFLAGS=-O2

build/host/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/host MBED_DIR=$(MBED_DIR) CC="$(HOST_CC)" AR=ar MBED_CFLAGS=-O2

DEV_CC   = $(ZIG) cc -target $(ZTARGET) -mcpu=$(CPU)
DEV_OBJ  := $(patsubst %.c,build/trimui/%.o,$(SRC))

all: build/trimui/$(APP)

# the TrimUI build (TFX_TRIMUI: it checks GitHub for updates by default)
build/trimui/$(APP): $(DEV_OBJ) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	$(DEV_CC) -o $@ $(DEV_OBJ) build/zig/libmbedtls.a build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -ldl -Wl,--allow-shlib-undefined -s

build/trimui/%.o: %.c $(HDR) $(MBED_SRC)
	@mkdir -p $(dir $@)
	$(DEV_CC) $(CFLAGS) -DTFX_TRIMUI -I$(SDL_INC) -I$(MBED_DIR)/include -c -o $@ $<

# a stand-in for the device's libSDL2-2.0.so.0, to link against (the firmware has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py $(SRC)
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c $(SRC)
	$(ZIG) cc -target $(ZTARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# --- desktop
HOST_OBJ := $(patsubst %.c,build/desktop/%.o,$(SRC))
desktop: build/desktop/$(APP)
build/desktop/$(APP): $(HOST_OBJ) build/host/libmbedtls.a
	$(HOST_CC) -o $@ $(HOST_OBJ) build/host/libmbedtls.a $(shell sdl2-config --libs) -lm -lpthread -ldl
build/desktop/%.o: %.c $(HDR) $(MBED_SRC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) $(shell sdl2-config --cflags | sed 's|-I\([^ ]*\)/SDL2|-I\1|') -I$(MBED_DIR)/include -c -o $@ $<

# --- tests
build/test_engine: tests/test_engine.c $(DSP) $(HDR)
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o $@ tests/test_engine.c $(DSP) -lm
build/bench_engine: tests/test_engine.c $(DSP) $(HDR)
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O2 -ffast-math -o $@ tests/test_engine.c $(DSP) -lm
build/test_aring: lib/brickio/tests/test_aring.c lib/brickio/aring.c lib/brickio/aring.h
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ lib/brickio/tests/test_aring.c lib/brickio/aring.c -lm
build/test_duplex: lib/brickio/tests/test_duplex.c lib/brickio/duplex.c lib/brickio/duplex.h
	@mkdir -p build
	$(HOST_CC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ lib/brickio/tests/test_duplex.c lib/brickio/duplex.c -ldl -lpthread
test: build/test_engine build/test_aring build/test_duplex
	build/test_engine
	build/test_aring
	build/test_duplex

# the host updater: what the app does on A, from the command line (src/main_update.c)
build/trimfx_update: src/net.c src/update.c src/main_update.c src/net.h src/update.h build/host/libmbedtls.a
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -DTFX_VERSION='"$(VERSION)"' -I$(MBED_DIR)/include -o $@ \
		src/net.c src/update.c src/main_update.c build/host/libmbedtls.a -lpthread
update-test: build/trimfx_update
	sh tools/update_test.sh
wavs: build/bench_engine
	@mkdir -p build/wavs
	build/bench_engine -w build/wavs
shots: build/desktop/$(APP)
	@mkdir -p build/shots
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TFX_SHOT=build/shots/trimfx TFX_DIR=build build/desktop/$(APP)

# --- the MustardOS package: <Name>/ with the binary, launch.sh, mux_launch.sh, wake_screen.sh, the glyph
package: build/$(NAME)-mustardos.muxapp
build/$(NAME)-mustardos.muxapp: build/trimui/$(APP) $(shell find Apps/$(NAME) mustardos -type f) LICENSE README.md $(MBED_SRC)
	rm -rf build/mux $@
	mkdir -p build/mux/$(NAME)
	cp -r Apps/$(NAME)/. build/mux/$(NAME)/
	cp -r mustardos/. build/mux/$(NAME)/
	rm -f build/mux/$(NAME)/state.txt build/mux/$(NAME)/log.txt build/mux/$(NAME)/midimap.txt build/mux/$(NAME)/audio.txt build/mux/$(NAME)/presets/slot*.txt
	cp build/trimui/$(APP) build/mux/$(NAME)/$(APP)
	cp LICENSE build/mux/$(NAME)/LICENSE.txt
	cp src/master/LICENSE-SP1Master.txt build/mux/$(NAME)/
	cp $(MBED_DIR)/LICENSE build/mux/$(NAME)/LICENSE-mbedtls.txt # the binary carries Mbed TLS (the updater)
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
.PHONY: all desktop test update-test wavs shots package install version clean

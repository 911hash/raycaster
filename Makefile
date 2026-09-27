# Makefile for the terminal raycaster.
#
#   make            build ./raycaster
#   make run        build and play
#   make demo       build and watch the autopilot
#   make test       build and run the built-in self test
#   make bench      build and render 500 frames off screen
#   make dist       package a shareable archive into ./dist
#   make web        build the browser version into ./web (needs emcc)
#   make webtest    run the self test on the wasm build under node
#   make dist-web   package the browser version into ./dist
#   make clean      remove build products
#
# Only libc, the POSIX layer and libm are needed: no ncurses, no assets.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -pedantic
LDLIBS  ?= -lm
TARGET   = raycaster

all: $(TARGET)

$(TARGET): raycaster.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

demo: $(TARGET)
	./$(TARGET) --demo

test: $(TARGET)
	./$(TARGET) --selftest

bench: $(TARGET)
	./$(TARGET) --headless --screen 120x40 --frames 500 --demo --seed 1 | head -12

# Package a shareable source archive (tar.gz for Linux/macOS, zip for Windows
# / chat apps). Both unpack to a folder the receiver just runs ./play.sh in.
VERSION = 1.0
dist: $(TARGET)
	rm -rf dist/raycaster-$(VERSION)
	mkdir -p dist/raycaster-$(VERSION)
	cp raycaster.c Makefile README.md play.sh dist/raycaster-$(VERSION)/
	tar -C dist -czf dist/raycaster-$(VERSION)-src.tar.gz raycaster-$(VERSION)
	cd dist && zip -q -r raycaster-$(VERSION)-src.zip raycaster-$(VERSION)
	rm -rf dist/raycaster-$(VERSION)
	@echo "created:"
	@echo "  dist/raycaster-$(VERSION)-src.tar.gz"
	@echo "  dist/raycaster-$(VERSION)-src.zip"

# --------------------------------------------------------------- web build --
# Needs Emscripten (https://emscripten.org), e.g. the emsdk:
#   git clone https://github.com/emscripten-core/emsdk.git && cd emsdk
#   ./emsdk install latest && ./emsdk activate latest && source emsdk_env.sh
EMCC     ?= emcc
# In CI the emsdk prepends its own directory to PATH, where `node` resolves to
# the SDK's node *directory* (not an executable); use the interpreter the SDK
# exports via EMSDK_NODE when present, plain `node` otherwise.
NODE     ?= $(if $(EMSDK_NODE),$(EMSDK_NODE),node)
WEBFLAGS  = -std=c99 -Wall -Wextra -pedantic \
	-sWASM=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
	-sEXPORTED_FUNCTIONS=_main,_web_key,_web_resize,_web_rgba,_web_text_ptr,_web_text_count,_web_cols,_web_rows

# Single file (wasm inlined), web-only loader: open web/index.html and play.
web: raycaster.c web/index.html
	$(EMCC) -O3 $(WEBFLAGS) -sENVIRONMENT=web -sSINGLE_FILE=1 \
		-o web/raycaster.js raycaster.c

# Verify the wasm build: the same self test, under node (22 checks).
webtest: raycaster.c
	mkdir -p build
	$(EMCC) -O2 $(WEBFLAGS) -o build/webtest.js raycaster.c
	$(NODE) build/webtest.js --selftest

dist-web: web
	rm -rf dist/raycaster-$(VERSION)-web
	mkdir -p dist/raycaster-$(VERSION)-web
	cp web/index.html web/raycaster.js dist/raycaster-$(VERSION)-web/
	cd dist && zip -q -r raycaster-$(VERSION)-web.zip raycaster-$(VERSION)-web
	rm -rf dist/raycaster-$(VERSION)-web
	@echo "created:"
	@echo "  dist/raycaster-$(VERSION)-web.zip"

clean:
	rm -f $(TARGET)
	rm -rf build

.PHONY: all run demo test bench dist web webtest dist-web clean

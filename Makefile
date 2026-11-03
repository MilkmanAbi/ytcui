# ═══════════════════════════════════════════════════════════════════════════════
# ytcui v5 — Makefile wrapper around CMake
#
# Primary build system is CMake (CMakeLists.txt).  This wrapper exists so that
# OIS (which calls `make` and expects `./ytcui`) continues to work unchanged.
# ═══════════════════════════════════════════════════════════════════════════════

UNAME_S := $(shell uname -s)
BUILD_DIR ?= build
PREFIX    ?= /usr/local

# Detect a Ninja installation; fall back to the default make generator.
NINJA := $(shell command -v ninja 2>/dev/null)
ifneq ($(NINJA),)
    CMAKE_GENERATOR := -G Ninja
else
    CMAKE_GENERATOR :=
endif

# On macOS, steer CMake toward Homebrew/MacPorts OpenSSL and ncurses when they
# are available (they are keg-only and not on the default search path).
CMAKE_EXTRA :=
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(BREW_PREFIX),)
        CMAKE_EXTRA += -DOPENSSL_ROOT_DIR=$(shell brew --prefix openssl 2>/dev/null)
        CMAKE_EXTRA += -DCURSES_INCLUDE_PATH=$(shell brew --prefix ncurses 2>/dev/null)/include
        CMAKE_EXTRA += -DCURSES_LIBRARY=$(shell brew --prefix ncurses 2>/dev/null)/lib/libncursesw.dylib
    endif
endif

VERSION := $(shell cat VERSION 2>/dev/null || echo "5.0.0")

.PHONY: all clean install uninstall version tests

all: $(BUILD_DIR)/ytcui
	cp $(BUILD_DIR)/ytcui ./ytcui
	@echo "Build complete: ./ytcui (v$(VERSION))"

$(BUILD_DIR)/ytcui: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

$(BUILD_DIR)/CMakeCache.txt:
	cmake -B $(BUILD_DIR) $(CMAKE_GENERATOR) \
	      -DCMAKE_BUILD_TYPE=Release $(CMAKE_EXTRA)

clean:
	rm -rf $(BUILD_DIR) ytcui
	@echo "Clean complete"

tests: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 ytcui $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/ytcui
	install -m 644 VERSION $(DESTDIR)$(PREFIX)/share/ytcui/
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/ytcui"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ytcui
	rm -rf $(DESTDIR)$(PREFIX)/share/ytcui
	@echo "Uninstalled"

version:
	@echo "$(VERSION)"

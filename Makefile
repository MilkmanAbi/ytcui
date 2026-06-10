# ═══════════════════════════════════════════════════════════════════════════════
# ytcui — Cross-platform Makefile (Linux, macOS, FreeBSD)
# ═══════════════════════════════════════════════════════════════════════════════

# ─── OS Detection ───────────────────────────────────────────────────────────────
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    OS_TYPE := macos
else ifeq ($(UNAME_S),FreeBSD)
    OS_TYPE := freebsd
else ifeq ($(UNAME_S),DragonFly)
    OS_TYPE := dragonfly
else ifeq ($(UNAME_S),NetBSD)
    OS_TYPE := netbsd
else ifeq ($(UNAME_S),OpenBSD)
    OS_TYPE := openbsd
else
    OS_TYPE := linux
endif

# ─── Compiler Selection ─────────────────────────────────────────────────────────
# Prefer g++ on Linux, clang++ on macOS/BSD (unless CXX is set)
ifeq ($(origin CXX),default)
    ifeq ($(OS_TYPE),macos)
        CXX := clang++
    else ifeq ($(OS_TYPE),freebsd)
        CXX := clang++
    else
        CXX := g++
    endif
endif

# ─── Base Flags ─────────────────────────────────────────────────────────────────
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -Iinclude

# Required for wide-char functions (ncursesw)
CXXFLAGS += -D_XOPEN_SOURCE_EXTENDED

# ─── Platform-specific defines ──────────────────────────────────────────────────
ifeq ($(OS_TYPE),macos)
    CXXFLAGS += -DYTUI_MACOS
else ifeq ($(OS_TYPE),freebsd)
    CXXFLAGS += -DYTUI_FREEBSD
else ifeq ($(OS_TYPE),dragonfly)
    CXXFLAGS += -DYTUI_FREEBSD
else
    CXXFLAGS += -DYTUI_LINUX
endif

# ─── ncurses Detection ──────────────────────────────────────────────────────────
# CRITICAL: Must use ncursesw (wide-char ncurses) for proper UTF-8 rendering.

ifeq ($(OS_TYPE),macos)
    # macOS: Homebrew ncurses (not linked by default)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo "/opt/homebrew")
    NCURSES_PREFIX := $(BREW_PREFIX)/opt/ncurses
    
    ifneq ($(wildcard $(NCURSES_PREFIX)/lib/libncursesw.*),)
        NCURSES_CFLAGS := -I$(NCURSES_PREFIX)/include
        NCURSES_LIBS := -L$(NCURSES_PREFIX)/lib -lncursesw
    else
        # Fallback to Apple's system ncurses. This is 5.7 (from 2008): it lacks
        # 256-colour / extended-pair support and mishandles high colour pairs,
        # which degrades (and historically crashed) coloured thumbnails. The
        # code now guards against this at runtime, but Homebrew ncurses is
        # strongly recommended. Install with:  brew install ncurses
        NCURSES_CFLAGS :=
        NCURSES_LIBS := -lncurses
        SYSTEM_NCURSES_WARNING := 1
    endif
else ifeq ($(OS_TYPE),freebsd)
    # FreeBSD: ncurses is in base system
    NCURSES_CFLAGS :=
    NCURSES_LIBS := -lncursesw
else
    # Linux: Use pkg-config
    NCURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo "-lncursesw")
    NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null || echo "")
endif

CXXFLAGS += $(NCURSES_CFLAGS)
LDFLAGS = $(NCURSES_LIBS) -lpthread $(YTCUIDL_LIBS) $(SIXEL_LIBS)

# ─── Backend Selection ──────────────────────────────────────────────────────────
# BACKEND=ytcuidl  (default) — built-in InnerTube client, no yt-dlp dependency
# BACKEND=ytdlp              — shell out to yt-dlp (legacy)
# install.sh writes the user's choice here so it survives the OIS sudo re-exec.
-include .ytcui-build.conf
BACKEND ?= ytcuidl

ifeq ($(BACKEND),ytcuidl)
    CXXFLAGS += -DUSE_YTCUIDL
    # ytcui-dl requires libcurl, openssl. We need the CFLAGS too, not just LIBS:
    # on multiarch distros curl/curl.h lives under e.g. /usr/include/<triplet>
    # and is not on the default search path, so omitting cflags breaks the build.
    YTCUIDL_CFLAGS := $(shell pkg-config --cflags libcurl openssl 2>/dev/null)
    YTCUIDL_LIBS   := $(shell pkg-config --libs libcurl openssl 2>/dev/null || echo "-lcurl -lssl -lcrypto")
    CXXFLAGS += $(YTCUIDL_CFLAGS)
else
    YTCUIDL_LIBS :=
endif

# ─── Optional: in-process SIXEL via libsixel ────────────────────────────────────
# SIXEL=libsixel  — link libsixel and encode sixel thumbnails in-process (one
#                   fewer subprocess per thumbnail). Optional; the default sixel
#                   path shells out to chafa (already a runtime dependency), so
#                   no extra build/runtime dependency unless you opt in.
SIXEL ?= chafa
ifeq ($(SIXEL),libsixel)
    SIXEL_CFLAGS := $(shell pkg-config --cflags libsixel 2>/dev/null)
    SIXEL_LIBS   := $(shell pkg-config --libs libsixel 2>/dev/null || echo "-lsixel")
    CXXFLAGS += -DUSE_LIBSIXEL $(SIXEL_CFLAGS)
else
    SIXEL_LIBS :=
endif


SRC_DIR = src
INC_DIR = include
OBJ_DIR = build
BIN_DIR = .

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
TARGET = $(BIN_DIR)/ytcui

VERSION := $(shell cat VERSION 2>/dev/null || echo "2.5.0")
# Make the runtime version match the VERSION file (was hardcoded in types.h).
CXXFLAGS += -DYTUI_VERSION='"$(VERSION)"'

# ─── Targets ────────────────────────────────────────────────────────────────────
.PHONY: all clean install uninstall version info

all: info $(TARGET)

info:
	@echo "Building ytcui v$(VERSION) for $(OS_TYPE) using $(CXX) [backend=$(BACKEND) sixel=$(SIXEL)]"
ifeq ($(SYSTEM_NCURSES_WARNING),1)
	@echo "  WARNING: linking Apple system ncurses (5.7). Coloured thumbnails are"
	@echo "           limited/unsafe on it. Recommended: brew install ncurses"
endif

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@ (v$(VERSION))"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
	@echo "Clean complete"

version:
	@echo "$(VERSION)"

PREFIX ?= /usr/local

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/ytcui
	install -m 644 VERSION $(DESTDIR)$(PREFIX)/share/ytcui/
	install -m 755 update.sh $(DESTDIR)$(PREFIX)/share/ytcui/
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/ytcui"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ytcui
	rm -rf $(DESTDIR)$(PREFIX)/share/ytcui
	@echo "Uninstalled"

# ─── Dependencies ───────────────────────────────────────────────────────────────
$(OBJ_DIR)/app.o:     $(SRC_DIR)/app.cpp     $(INC_DIR)/app.h $(INC_DIR)/types.h $(INC_DIR)/tui.h $(INC_DIR)/youtube.h $(INC_DIR)/player.h $(INC_DIR)/input.h $(INC_DIR)/config.h $(INC_DIR)/library.h $(INC_DIR)/log.h $(INC_DIR)/thumbs.h $(INC_DIR)/auth.h $(INC_DIR)/theme.h
$(OBJ_DIR)/config.o:  $(SRC_DIR)/config.cpp  $(INC_DIR)/config.h $(INC_DIR)/theme.h
$(OBJ_DIR)/input.o:   $(SRC_DIR)/input.cpp   $(INC_DIR)/input.h $(INC_DIR)/types.h
$(OBJ_DIR)/main.o:    $(SRC_DIR)/main.cpp    $(INC_DIR)/app.h $(INC_DIR)/log.h $(INC_DIR)/player.h $(INC_DIR)/youtube.h $(INC_DIR)/types.h $(INC_DIR)/theme.h
$(OBJ_DIR)/player.o:  $(SRC_DIR)/player.cpp  $(INC_DIR)/player.h $(INC_DIR)/compat.h $(INC_DIR)/types.h $(INC_DIR)/log.h
$(OBJ_DIR)/tui.o:     $(SRC_DIR)/tui.cpp     $(INC_DIR)/tui.h $(INC_DIR)/types.h $(INC_DIR)/library.h $(INC_DIR)/thumbs.h $(INC_DIR)/theme.h
$(OBJ_DIR)/youtube.o: $(SRC_DIR)/youtube.cpp $(INC_DIR)/youtube.h $(INC_DIR)/types.h $(INC_DIR)/log.h

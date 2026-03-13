#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# ytcui installer — Linux, macOS, FreeBSD, and other BSDs
# ═══════════════════════════════════════════════════════════════════════════════
set -e

INSTALL_PREFIX="/usr/local"
DATA_DIR="/usr/local/share/ytcui"
BINARY_NAME="ytcui"

# ─── Colors ─────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    BOLD='\033[1m'; CYAN='\033[36m'; GREEN='\033[32m'
    YELLOW='\033[33m'; RED='\033[31m'; DIM='\033[2m'; RESET='\033[0m'
else
    BOLD=''; CYAN=''; GREEN=''; YELLOW=''; RED=''; DIM=''; RESET=''
fi

ok()   { echo -e "  ${GREEN}✓${RESET} $*"; }
warn() { echo -e "  ${YELLOW}!${RESET} $*"; }
die()  { echo -e "\n  ${RED}✗ Fatal:${RESET} $*\n"; exit 1; }
info() { echo -e "  ${DIM}→${RESET} $*"; }
hdr()  { echo -e "\n${BOLD}$*${RESET}\n"; }

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${RESET}"
echo -e "${BOLD}${CYAN}  ytcui — YouTube Terminal UI Installer${RESET}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${RESET}"
echo ""

# ─── OS / arch detection ────────────────────────────────────────────────────────
OS_TYPE="unknown"
case "$(uname -s)" in
    Linux*)    OS_TYPE="linux"     ;;
    Darwin*)   OS_TYPE="macos"     ;;
    FreeBSD*)  OS_TYPE="freebsd"   ;;
    NetBSD*)   OS_TYPE="netbsd"    ;;
    OpenBSD*)  OS_TYPE="openbsd"   ;;
    DragonFly*) OS_TYPE="dragonfly" ;;
esac

ARCH="$(uname -m)"
ok "OS: ${BOLD}$OS_TYPE${RESET}  arch: ${BOLD}$ARCH${RESET}"

# ═══════════════════════════════════════════════════════════════════════════════
# Clipboard tool installation
#
# macOS:  pbcopy is a system binary (/usr/bin/pbcopy), ships with every macOS
#         since 10.3. Nothing to install.
#
# Linux:  No distro ships clipboard CLI tools by default. We detect the session
#         type (Wayland vs X11) and install the right tool:
#           Wayland  →  wl-clipboard  (provides wl-copy + wl-paste)
#           X11      →  xclip         (most compatible)
#         We install both when we can't tell, so it just works after reboot/login.
#
# FreeBSD: Same as Linux X11 — xclip is available in ports/pkg.
#
# The runtime code (app.cpp) tries wl-copy → xclip → xsel in order, so having
# any one of them is enough.
# ═══════════════════════════════════════════════════════════════════════════════

install_clipboard_linux() {
    local PM="$1"

    # Detect session type. WAYLAND_DISPLAY / XDG_SESSION_TYPE are set by the
    # login manager. We check both because not all compositors set both.
    local SESSION="x11"
    if [[ "${WAYLAND_DISPLAY:-}" != "" || "${XDG_SESSION_TYPE:-}" == "wayland" ]]; then
        SESSION="wayland"
    fi

    info "Display session: $SESSION"

    # Already have something? Skip.
    if command -v wl-copy &>/dev/null || command -v xclip &>/dev/null || command -v xsel &>/dev/null; then
        ok "Clipboard tool already installed ($(command -v wl-copy 2>/dev/null || command -v xclip 2>/dev/null || command -v xsel 2>/dev/null))"
        return 0
    fi

    info "Installing clipboard tool..."

    case "$PM" in
        apt)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo apt-get install -y wl-clipboard || \
                sudo apt-get install -y xclip || \
                warn "Could not install clipboard tool"
            else
                sudo apt-get install -y xclip || \
                sudo apt-get install -y xsel  || \
                warn "Could not install clipboard tool"
            fi
            ;;
        pacman)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo pacman -S --noconfirm wl-clipboard || \
                sudo pacman -S --noconfirm xclip
            else
                sudo pacman -S --noconfirm xclip || \
                sudo pacman -S --noconfirm xsel
            fi
            ;;
        dnf)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo dnf install -y wl-clipboard || sudo dnf install -y xclip
            else
                sudo dnf install -y xclip || sudo dnf install -y xsel
            fi
            ;;
        yum)
            sudo yum install -y xclip || \
            sudo yum install -y xsel  || \
            warn "xclip/xsel not in repos — try: sudo yum install epel-release first"
            ;;
        zypper)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo zypper install -y wl-clipboard || sudo zypper install -y xclip
            else
                sudo zypper install -y xclip || sudo zypper install -y xsel
            fi
            ;;
        apk)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo apk add --no-cache wl-clipboard || sudo apk add --no-cache xclip
            else
                sudo apk add --no-cache xclip
            fi
            ;;
        emerge)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo emerge gui-apps/wl-clipboard || sudo emerge x11-misc/xclip
            else
                sudo emerge x11-misc/xclip
            fi
            ;;
        xbps)
            if [[ "$SESSION" == "wayland" ]]; then
                sudo xbps-install -y wl-clipboard || sudo xbps-install -y xclip
            else
                sudo xbps-install -y xclip
            fi
            ;;
        *)
            warn "Unknown package manager — install wl-clipboard (Wayland) or xclip (X11) manually"
            return 0
            ;;
    esac

    if command -v wl-copy &>/dev/null; then
        ok "Clipboard: wl-copy (wl-clipboard)"
    elif command -v xclip &>/dev/null; then
        ok "Clipboard: xclip"
    elif command -v xsel &>/dev/null; then
        ok "Clipboard: xsel"
    else
        warn "Clipboard tool could not be installed. URL copy will not work."
        warn "Install manually: sudo apt install xclip  (or wl-clipboard on Wayland)"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# macOS
# ═══════════════════════════════════════════════════════════════════════════════
install_macos() {
    hdr "Installing on macOS..."

    # pbcopy ships with macOS — no clipboard install needed at all
    ok "Clipboard: pbcopy (built-in, no install needed)"

    # Xcode CLI tools
    if ! xcode-select -p &>/dev/null; then
        warn "Xcode Command Line Tools not found"
        info "Installing Xcode Command Line Tools..."
        xcode-select --install 2>/dev/null || true
        echo ""
        echo "  Complete the installation dialog that appeared, then press Enter..."
        read -r
        if ! xcode-select -p &>/dev/null; then
            die "Xcode Command Line Tools installation failed"
        fi
    fi
    ok "Xcode Command Line Tools"

    # Homebrew
    if ! command -v brew &>/dev/null; then
        warn "Homebrew not found"
        info "Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        if [[ -f "/opt/homebrew/bin/brew" ]]; then
            eval "$(/opt/homebrew/bin/brew shellenv)"
        elif [[ -f "/usr/local/bin/brew" ]]; then
            eval "$(/usr/local/bin/brew shellenv)"
        fi
        if ! command -v brew &>/dev/null; then
            die "Homebrew installation failed. Install from https://brew.sh then re-run."
        fi
        ok "Homebrew installed"
        echo ""
        warn "Add Homebrew to your PATH permanently — add to ~/.zshrc:"
        if [[ "$ARCH" == "arm64" ]]; then
            echo '    eval "$(/opt/homebrew/bin/brew shellenv)"'
        else
            echo '    eval "$(/usr/local/bin/brew shellenv)"'
        fi
        echo ""
    else
        ok "Homebrew: $(brew --version | head -1)"
    fi

    info "Updating Homebrew..."
    brew update 2>/dev/null || warn "brew update failed (continuing)"

    info "Installing dependencies..."
    local DEPS="mpv yt-dlp ncurses curl chafa"
    for pkg in $DEPS; do
        if brew list "$pkg" &>/dev/null 2>&1; then
            ok "$pkg (already installed)"
        else
            info "Installing $pkg..."
            brew install "$pkg" || warn "Failed to install $pkg"
        fi
    done

    # Export ncurses paths so the Makefile can find Homebrew's ncurses
    BREW_PREFIX="$(brew --prefix)"
    export PKG_CONFIG_PATH="$BREW_PREFIX/opt/ncurses/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export LDFLAGS="-L$BREW_PREFIX/opt/ncurses/lib ${LDFLAGS:-}"
    export CPPFLAGS="-I$BREW_PREFIX/opt/ncurses/include ${CPPFLAGS:-}"

    ok "macOS dependencies ready"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Linux
# ═══════════════════════════════════════════════════════════════════════════════
install_linux() {
    hdr "Installing on Linux..."

    # Detect package manager
    local PM="unknown"
    if   command -v apt-get      &>/dev/null; then PM="apt"
    elif command -v pacman       &>/dev/null; then PM="pacman"
    elif command -v dnf          &>/dev/null; then PM="dnf"
    elif command -v yum          &>/dev/null; then PM="yum"
    elif command -v zypper       &>/dev/null; then PM="zypper"
    elif command -v apk          &>/dev/null; then PM="apk"
    elif command -v emerge       &>/dev/null; then PM="emerge"
    elif command -v xbps-install &>/dev/null; then PM="xbps"
    fi
    ok "Package manager: ${BOLD}$PM${RESET}"
    echo ""

    info "Updating package index..."
    case "$PM" in
        apt)    sudo apt-get update -qq ;;
        pacman) sudo pacman -Sy --noconfirm ;;
        dnf)    sudo dnf check-update -q || true ;;
        yum)    sudo yum check-update -q || true ;;
        zypper) sudo zypper refresh ;;
        apk)    sudo apk update ;;
        *)      true ;;
    esac

    info "Installing build dependencies and core tools..."
    case "$PM" in
        apt)
            sudo apt-get install -y \
                g++ make libncursesw5-dev libncurses-dev \
                mpv python3 python3-pip curl chafa git
            ;;
        pacman)
            sudo pacman -S --noconfirm \
                gcc make ncurses mpv python python-pip curl chafa git
            ;;
        dnf)
            sudo dnf install -y \
                gcc-c++ make ncurses-devel mpv python3 python3-pip curl chafa git
            ;;
        yum)
            sudo yum install -y epel-release 2>/dev/null || true
            sudo yum install -y \
                gcc-c++ make ncurses-devel mpv python3 python3-pip curl git
            warn "chafa not in yum repos (optional — thumbnails will be disabled)"
            ;;
        zypper)
            sudo zypper install -y \
                gcc-c++ make ncurses-devel mpv python3 python3-pip curl chafa git
            ;;
        apk)
            sudo apk add --no-cache \
                g++ make ncurses-dev mpv python3 py3-pip curl chafa git
            ;;
        emerge)
            sudo emerge \
                sys-devel/gcc sys-devel/make sys-libs/ncurses \
                media-video/mpv dev-lang/python net-misc/curl \
                media-gfx/chafa dev-vcs/git
            ;;
        xbps)
            sudo xbps-install -y \
                gcc make ncurses-devel mpv python3 python3-pip curl chafa git
            ;;
        *)
            die "Unknown package manager. Install manually: g++, make, ncurses-dev, mpv, python3, pip, curl — then re-run."
            ;;
    esac
    ok "Build dependencies installed"

    # yt-dlp (pip-installed to get a recent version, distro packages lag behind)
    echo ""
    info "Installing yt-dlp..."
    local PIP=""
    command -v pip3 &>/dev/null && PIP="pip3"
    command -v pip  &>/dev/null && [[ -z "$PIP" ]] && PIP="pip"

    if [[ -z "$PIP" ]]; then
        if command -v pipx &>/dev/null; then
            pipx install yt-dlp || warn "yt-dlp via pipx failed"
        else
            die "pip not found — cannot install yt-dlp"
        fi
    else
        $PIP install --upgrade yt-dlp --break-system-packages 2>/dev/null || \
        $PIP install --upgrade yt-dlp 2>/dev/null || \
        $PIP install --upgrade --user yt-dlp || \
        warn "yt-dlp install failed"
    fi

    if command -v yt-dlp &>/dev/null; then
        ok "yt-dlp $(yt-dlp --version 2>/dev/null)"
    else
        warn "yt-dlp not in PATH yet (may need new shell session)"
    fi

    # Clipboard tools
    echo ""
    install_clipboard_linux "$PM"
}

# ═══════════════════════════════════════════════════════════════════════════════
# FreeBSD
# ═══════════════════════════════════════════════════════════════════════════════
install_freebsd() {
    hdr "Installing on FreeBSD..."

    local SUDO=""
    [[ $EUID -ne 0 ]] && SUDO="sudo"

    info "Installing dependencies via pkg..."
    $SUDO pkg update || warn "pkg update failed"
    $SUDO pkg install -y gmake gcc ncurses mpv curl chafa xclip || \
        warn "Some packages failed to install"

    # wl-clipboard if Wayland is available
    if [[ "${WAYLAND_DISPLAY:-}" != "" || "${XDG_SESSION_TYPE:-}" == "wayland" ]]; then
        $SUDO pkg install -y wl-clipboard 2>/dev/null || true
    fi

    if command -v wl-copy &>/dev/null; then
        ok "Clipboard: wl-copy"
    elif command -v xclip &>/dev/null; then
        ok "Clipboard: xclip"
    else
        warn "No clipboard tool found — URL copy will not work"
    fi

    if ! command -v yt-dlp &>/dev/null; then
        info "Installing yt-dlp via pip..."
        $SUDO pip install --break-system-packages yt-dlp 2>/dev/null || \
        pip install --user yt-dlp || \
        warn "yt-dlp installation failed"
    else
        ok "yt-dlp $(yt-dlp --version 2>/dev/null)"
    fi

    export MAKE="gmake"
    ok "FreeBSD dependencies ready"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Run the right installer
# ═══════════════════════════════════════════════════════════════════════════════
case "$OS_TYPE" in
    macos)    install_macos ;;
    linux)    install_linux ;;
    freebsd)  install_freebsd ;;
    netbsd|openbsd|dragonfly)
        warn "BSD variant '$OS_TYPE' — attempting generic install"
        warn "Ensure these are installed: c++ compiler, make/gmake, ncurses, mpv, yt-dlp, curl, xclip or wl-clipboard"
        ;;
    *)
        die "Unsupported OS: $(uname -s)"
        ;;
esac

echo ""

# ═══════════════════════════════════════════════════════════════════════════════
# Build
# ═══════════════════════════════════════════════════════════════════════════════
hdr "Building ytcui..."

MAKE_CMD="${MAKE:-make}"
$MAKE_CMD clean 2>/dev/null || true
$MAKE_CMD || die "Build failed — see errors above"
ok "Build complete"
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
# Install files
# ═══════════════════════════════════════════════════════════════════════════════
hdr "Installing..."

if [[ -w "$INSTALL_PREFIX/bin" ]]; then
    cp "$BINARY_NAME" "$INSTALL_PREFIX/bin/$BINARY_NAME"
    chmod 755 "$INSTALL_PREFIX/bin/$BINARY_NAME"
else
    sudo cp "$BINARY_NAME" "$INSTALL_PREFIX/bin/$BINARY_NAME"
    sudo chmod 755 "$INSTALL_PREFIX/bin/$BINARY_NAME"
fi
ok "Binary → $INSTALL_PREFIX/bin/$BINARY_NAME"

if [[ -w "$(dirname "$DATA_DIR")" ]] 2>/dev/null; then
    mkdir -p "$DATA_DIR"
    cp update.sh "$DATA_DIR/update.sh"
    cp VERSION   "$DATA_DIR/VERSION"
    chmod 755 "$DATA_DIR/update.sh"
else
    sudo mkdir -p "$DATA_DIR"
    sudo cp update.sh "$DATA_DIR/update.sh"
    sudo cp VERSION   "$DATA_DIR/VERSION"
    sudo chmod 755 "$DATA_DIR/update.sh"
fi
ok "Data → $DATA_DIR/"

# ═══════════════════════════════════════════════════════════════════════════════
# Done
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}${GREEN}══════════════════════════════════════════${RESET}"
echo -e "${BOLD}${GREEN}  ✓ ytcui installed successfully!${RESET}"
echo -e "${BOLD}${GREEN}══════════════════════════════════════════${RESET}"
echo ""

if ! command -v "$BINARY_NAME" &>/dev/null; then
    warn "ytcui not in PATH yet. Add to your shell config:"
    echo '    export PATH="$PATH:/usr/local/bin"'
    echo ""
fi

echo "  ytcui                           start"
echo "  ytcui --diag                    check everything is working"
echo "  ytcui -t dracula                dracula theme"
echo "  ytcui --upgrade                 upgrade to latest version"
echo "  ytcui --help                    help"
echo ""

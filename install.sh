#!/bin/bash
# ytcui installer — Linux, macOS, FreeBSD
set -e

INSTALL_PREFIX="/usr/local"
DATA_DIR="/usr/local/share/ytcui"
BINARY_NAME="ytcui"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/ytcui"

# Colors (only if real terminal)
if [[ -t 1 ]]; then
    BOLD='\033[1m'; GREEN='\033[32m'; YELLOW='\033[33m'
    RED='\033[31m'; DIM='\033[2m'; RESET='\033[0m'
else
    BOLD=''; GREEN=''; YELLOW=''; RED=''; DIM=''; RESET=''
fi

ok()   { echo -e "  ${GREEN}✓${RESET}  $*"; }
warn() { echo -e "  ${YELLOW}!${RESET}  $*"; }
die()  { echo -e "\n  ${RED}✗${RESET}  $*\n"; exit 1; }
info() { echo -e "  ${DIM}·${RESET}  $*"; }
ask()  { echo -ne "  ${BOLD}$*${RESET} "; }
sep()  { echo ""; }

# ─── Header ──────────────────────────────────────────────────────────────────

echo ""
echo -e "  ${BOLD}ytcui${RESET} — YouTube Terminal UI"
echo -e "  ${DIM}──────────────────────────────${RESET}"
echo ""

# ─── OS / arch detection ─────────────────────────────────────────────────────

OS_TYPE="unknown"
case "$(uname -s)" in
    Linux*)     OS_TYPE="linux"     ;;
    Darwin*)    OS_TYPE="macos"     ;;
    FreeBSD*)   OS_TYPE="freebsd"   ;;
    NetBSD*)    OS_TYPE="netbsd"    ;;
    OpenBSD*)   OS_TYPE="openbsd"   ;;
    DragonFly*) OS_TYPE="dragonfly" ;;
esac
ARCH="$(uname -m)"
ok "System: $OS_TYPE / $ARCH"

# ─── Interactive setup ───────────────────────────────────────────────────────

BACKEND="ytcuidl"
SHOW_THUMBNAILS="yes"
CHOSEN_THEME="default"

if [[ -t 0 ]]; then
    sep
    echo -e "  ${BOLD}Setup${RESET}"
    echo -e "  ${DIM}──────────────────────────────${RESET}"
    sep

    # Backend
    echo -e "  Backend — how ytcui fetches YouTube streams:"
    sep
    echo -e "    ${BOLD}1)${RESET} ytcui-dl  ${DIM}(recommended)${RESET}"
    echo -e "         Built-in, no Python needed. Near-instant playback."
    echo -e "         Connects directly to YouTube's mobile API."
    sep
    echo -e "    ${BOLD}2)${RESET} yt-dlp    ${DIM}(stable)${RESET}"
    echo -e "         Python-based extractor. 2–5s startup per video."
    echo -e "         Battle-tested, works with every edge case."
    sep
    ask "Choice [1/2, default=1]:"
    read -r _b
    case "$_b" in
        2|yt-dlp|ytdlp) BACKEND="ytdlp";   ok "Backend: yt-dlp" ;;
        *)               BACKEND="ytcuidl"; ok "Backend: ytcui-dl" ;;
    esac
    sep

    # Thumbnails
    echo -e "  Thumbnails — show video thumbnails in the sidebar?"
    echo -e "    ${DIM}(chafa is installed either way)${RESET}"
    sep
    ask "Show thumbnails? [Y/n, default=Y]:"
    read -r _t
    case "$_t" in
        n|N|no|No|NO) SHOW_THUMBNAILS="no";  ok "Thumbnails: off" ;;
        *)             SHOW_THUMBNAILS="yes"; ok "Thumbnails: on"  ;;
    esac
    sep

    # Theme picker
    echo -e "  Theme — pick a colour scheme:"
    sep
    echo -e "    ${BOLD} 1)${RESET} default    The classic. Clean terminal colours, no fuss."
    echo -e "    ${BOLD} 2)${RESET} dracula    Deep purples and crimson accents. Creature of the night."
    echo -e "    ${BOLD} 3)${RESET} nord       Arctic blues and soft greys. Nordic winter calm."
    echo -e "    ${BOLD} 4)${RESET} tokyo      Neon city rain at midnight. Tokyo Night vibes."
    echo -e "    ${BOLD} 5)${RESET} gruvbox    Warm wood and amber. Retro terminal cosiness."
    echo -e "    ${BOLD} 6)${RESET} monokai    Vivid syntax colours. The classic dev palette."
    echo -e "    ${BOLD} 7)${RESET} solarized  Precision-tuned tones. Easy on the eyes all day."
    echo -e "    ${BOLD} 8)${RESET} pink       Soft sakura blossoms and blush petals at dawn."
    echo -e "    ${BOLD} 9)${RESET} purple     Wisteria and lavender fields in the late afternoon."
    echo -e "    ${BOLD}10)${RESET} blue       Powder sky, periwinkle haze, and summer sea glass."
    echo -e "    ${BOLD}11)${RESET} green      Morning sage, honeydew, and botanical softness."
    echo -e "    ${BOLD}12)${RESET} mint       Cool spearmint foam and pale jade on a spring day."
    echo -e "    ${BOLD}13)${RESET} ocean      Pale turquoise coves and seafoam on still water."
    echo -e "    ${BOLD}14)${RESET} coral      Warm peach, apricot, and sun-kissed sandy blush."
    echo -e "    ${BOLD}15)${RESET} amber      Champagne fields, soft gold, and cornsilk warmth."
    echo -e "    ${BOLD}16)${RESET} red        Dusty rose, linen, and the blush of a gentle sunset."
    echo -e "    ${BOLD}17)${RESET} slate      Cool steel mist and powder blue-grey at dusk."
    echo -e "    ${BOLD}18)${RESET} grayscale  No colour. Just shape, light, and shadow."
    sep
    echo -e "    ${DIM}Not sure? Pick anything — you can change it anytime with: ytcui -t <name>${RESET}"
    sep
    ask "Choice [1-18, default=1]:"
    read -r _theme
    case "$_theme" in
        2)  CHOSEN_THEME="dracula"   ;;
        3)  CHOSEN_THEME="nord"      ;;
        4)  CHOSEN_THEME="tokyo"     ;;
        5)  CHOSEN_THEME="gruvbox"   ;;
        6)  CHOSEN_THEME="monokai"   ;;
        7)  CHOSEN_THEME="solarized" ;;
        8)  CHOSEN_THEME="pink"      ;;
        9)  CHOSEN_THEME="purple"    ;;
        10) CHOSEN_THEME="blue"      ;;
        11) CHOSEN_THEME="green"     ;;
        12) CHOSEN_THEME="mint"      ;;
        13) CHOSEN_THEME="ocean"     ;;
        14) CHOSEN_THEME="coral"     ;;
        15) CHOSEN_THEME="amber"     ;;
        16) CHOSEN_THEME="red"       ;;
        17) CHOSEN_THEME="slate"     ;;
        18) CHOSEN_THEME="grayscale" ;;
        *)  CHOSEN_THEME="default"   ;;
    esac
    ok "Theme: $CHOSEN_THEME"
    sep
else
    info "Non-interactive — using defaults: backend=ytcui-dl, thumbnails=on, theme=default"
fi

# ─── Write thumbnail preference to config now (before build) ─────────────────
# The runtime reads this from config.json on startup.

write_thumb_config() {
    mkdir -p "$CONFIG_DIR"
    local cf="$CONFIG_DIR/config.json"
    local thumb_val="true"
    [[ "$SHOW_THUMBNAILS" == "no" ]] && thumb_val="false"
    if [[ -f "$cf" ]]; then
        if command -v python3 &>/dev/null; then
            python3 -c "
import json
with open('$cf') as f:
    d = json.load(f)
d['show_thumbnails'] = $thumb_val
d['theme'] = '$CHOSEN_THEME'
with open('$cf','w') as f:
    json.dump(d, f, indent=2)
" 2>/dev/null || true
        fi
    else
        cat > "$cf" << JSONEOF
{
  "show_thumbnails": $thumb_val,
  "theme": "$CHOSEN_THEME"
}
JSONEOF
    fi
}
write_thumb_config

# ─── Clipboard helper (Linux) ────────────────────────────────────────────────

install_clipboard_linux() {
    local PM="$1"
    local SESSION="x11"
    [[ "${WAYLAND_DISPLAY:-}" != "" || "${XDG_SESSION_TYPE:-}" == "wayland" ]] && SESSION="wayland"

    command -v wl-copy &>/dev/null && { ok "Clipboard: wl-copy"; return; }
    command -v xclip   &>/dev/null && { ok "Clipboard: xclip";   return; }
    command -v xsel    &>/dev/null && { ok "Clipboard: xsel";    return; }

    info "Installing clipboard tool..."
    case "$PM" in
        apt)
            [[ "$SESSION" == "wayland" ]] && sudo apt-get install -y wl-clipboard 2>/dev/null || \
                sudo apt-get install -y xclip 2>/dev/null || warn "Clipboard install failed"
            ;;
        pacman)
            [[ "$SESSION" == "wayland" ]] && sudo pacman -S --noconfirm wl-clipboard || \
                sudo pacman -S --noconfirm xclip
            ;;
        dnf)
            [[ "$SESSION" == "wayland" ]] && sudo dnf install -y wl-clipboard || \
                sudo dnf install -y xclip
            ;;
        yum)    sudo yum install -y xclip 2>/dev/null || warn "xclip not available" ;;
        zypper) sudo zypper install -y xclip 2>/dev/null || warn "xclip not available" ;;
        apk)    sudo apk add --no-cache xclip 2>/dev/null || warn "xclip not available" ;;
        emerge) sudo emerge x11-misc/xclip ;;
        xbps)   sudo xbps-install -y xclip ;;
        *)      warn "Unknown PM — install xclip or wl-clipboard manually" ;;
    esac
    command -v wl-copy &>/dev/null && ok "Clipboard: wl-copy" && return
    command -v xclip   &>/dev/null && ok "Clipboard: xclip"   && return
    warn "No clipboard tool found — URL copy won't work"
}

# ─── Installers ──────────────────────────────────────────────────────────────

install_linux() {
    local PM="unknown"
    command -v apt-get      &>/dev/null && PM="apt"
    command -v pacman       &>/dev/null && PM="pacman"
    command -v dnf          &>/dev/null && PM="dnf"
    command -v yum          &>/dev/null && PM="yum"
    command -v zypper       &>/dev/null && PM="zypper"
    command -v apk          &>/dev/null && PM="apk"
    command -v emerge       &>/dev/null && PM="emerge"
    command -v xbps-install &>/dev/null && PM="xbps"
    ok "Package manager: $PM"
    sep

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

    info "Installing dependencies..."
    case "$PM" in
        apt)
            PKGS="g++ make libncursesw5-dev libncurses-dev mpv curl chafa git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS libcurl4-openssl-dev libssl-dev"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 python3-pip"
            sudo apt-get install -y $PKGS
            ;;
        pacman)
            PKGS="gcc make ncurses mpv curl chafa git"
            [[ "$BACKEND" == "ytdlp" ]] && PKGS="$PKGS python python-pip"
            sudo pacman -S --noconfirm $PKGS
            ;;
        dnf)
            PKGS="gcc-c++ make ncurses-devel mpv curl chafa git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS libcurl-devel openssl-devel"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 python3-pip"
            sudo dnf install -y $PKGS
            ;;
        yum)
            sudo yum install -y epel-release 2>/dev/null || true
            PKGS="gcc-c++ make ncurses-devel mpv curl git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS libcurl-devel openssl-devel"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 python3-pip"
            sudo yum install -y $PKGS
            warn "chafa: install manually from https://hpjansson.org/chafa/ if not available"
            ;;
        zypper)
            PKGS="gcc-c++ make ncurses-devel mpv curl chafa git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS libcurl-devel libopenssl-devel"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 python3-pip"
            sudo zypper install -y $PKGS
            ;;
        apk)
            PKGS="g++ make ncurses-dev mpv curl chafa git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS curl-dev openssl-dev"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 py3-pip"
            sudo apk add --no-cache $PKGS
            ;;
        emerge)
            sudo emerge sys-devel/gcc sys-devel/make sys-libs/ncurses \
                media-video/mpv net-misc/curl media-gfx/chafa dev-vcs/git
            [[ "$BACKEND" == "ytcuidl" ]] && sudo emerge dev-libs/openssl
            [[ "$BACKEND" == "ytdlp"   ]] && sudo emerge dev-lang/python
            ;;
        xbps)
            PKGS="gcc make ncurses-devel mpv curl chafa git"
            [[ "$BACKEND" == "ytcuidl" ]] && PKGS="$PKGS libcurl-devel openssl-devel"
            [[ "$BACKEND" == "ytdlp"   ]] && PKGS="$PKGS python3 python3-pip"
            sudo xbps-install -y $PKGS
            ;;
        *)
            die "Unknown package manager. Install g++, make, ncurses-dev, mpv, curl, chafa manually."
            ;;
    esac
    ok "Dependencies installed"

    if [[ "$BACKEND" == "ytdlp" ]]; then
        sep
        info "Installing yt-dlp..."
        local PIP=""
        command -v pip3 &>/dev/null && PIP="pip3"
        command -v pip  &>/dev/null && [[ -z "$PIP" ]] && PIP="pip"
        if [[ -n "$PIP" ]]; then
            $PIP install --upgrade yt-dlp --break-system-packages 2>/dev/null || \
            $PIP install --upgrade yt-dlp 2>/dev/null || \
            $PIP install --upgrade --user yt-dlp || warn "yt-dlp install failed"
        elif command -v pipx &>/dev/null; then
            pipx install yt-dlp || warn "yt-dlp via pipx failed"
        else
            die "pip not found — cannot install yt-dlp"
        fi
        command -v yt-dlp &>/dev/null && ok "yt-dlp $(yt-dlp --version 2>/dev/null)" \
            || warn "yt-dlp not in PATH (may need new shell)"
    fi

    sep
    install_clipboard_linux "$PM"
}

install_macos() {
    ok "Clipboard: pbcopy (built-in)"

    if ! xcode-select -p &>/dev/null; then
        info "Installing Xcode Command Line Tools..."
        xcode-select --install 2>/dev/null || true
        echo ""; echo "  Complete the dialog, then press Enter..."
        read -r
        xcode-select -p &>/dev/null || die "Xcode CLT install failed"
    fi
    ok "Xcode Command Line Tools"

    if ! command -v brew &>/dev/null; then
        info "Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        [[ -f "/opt/homebrew/bin/brew" ]] && eval "$(/opt/homebrew/bin/brew shellenv)"
        [[ -f "/usr/local/bin/brew"    ]] && eval "$(/usr/local/bin/brew shellenv)"
        command -v brew &>/dev/null || die "Homebrew install failed"
        ok "Homebrew installed"
    else
        ok "Homebrew: $(brew --version | head -1)"
    fi

    brew update 2>/dev/null || true

    info "Installing dependencies..."
    local DEPS="mpv ncurses curl chafa"
    [[ "$BACKEND" == "ytdlp" ]] && DEPS="$DEPS yt-dlp"
    for pkg in $DEPS; do
        brew list "$pkg" &>/dev/null || brew install "$pkg" || warn "Failed: $pkg"
    done
    ok "Dependencies installed"

    BREW_PREFIX="$(brew --prefix)"
    export PKG_CONFIG_PATH="$BREW_PREFIX/opt/ncurses/lib/pkgconfig:$BREW_PREFIX/opt/curl/lib/pkgconfig:$BREW_PREFIX/opt/openssl/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export LDFLAGS="-L$BREW_PREFIX/opt/ncurses/lib -L$BREW_PREFIX/opt/curl/lib -L$BREW_PREFIX/opt/openssl/lib ${LDFLAGS:-}"
    export CPPFLAGS="-I$BREW_PREFIX/opt/ncurses/include -I$BREW_PREFIX/opt/curl/include -I$BREW_PREFIX/opt/openssl/include ${CPPFLAGS:-}"
}

install_freebsd() {
    local SUDO=""; [[ $EUID -ne 0 ]] && SUDO="sudo"
    info "Installing via pkg..."
    $SUDO pkg update || true
    PKGS="gmake gcc ncurses mpv curl chafa"
    [[ "$BACKEND" == "ytdlp" ]] && PKGS="$PKGS python3 py39-pip"
    $SUDO pkg install -y $PKGS || warn "Some packages failed"
    [[ "${WAYLAND_DISPLAY:-}" != "" ]] && $SUDO pkg install -y wl-clipboard 2>/dev/null || true
    command -v xclip &>/dev/null || $SUDO pkg install -y xclip 2>/dev/null || true
    ok "FreeBSD dependencies ready"
    export MAKE="gmake"
}

# ─── Run installer ────────────────────────────────────────────────────────────

sep
echo -e "  ${BOLD}Installing dependencies${RESET}"
echo -e "  ${DIM}──────────────────────────────${RESET}"
sep

case "$OS_TYPE" in
    linux)   install_linux ;;
    macos)   install_macos ;;
    freebsd) install_freebsd ;;
    netbsd|openbsd|dragonfly)
        warn "BSD variant '$OS_TYPE' — install g++, make, ncurses, mpv, curl, chafa manually"
        ;;
    *) die "Unsupported OS: $(uname -s)" ;;
esac

# ─── Build ────────────────────────────────────────────────────────────────────

sep
echo -e "  ${BOLD}Building${RESET}"
echo -e "  ${DIM}──────────────────────────────${RESET}"
sep

MAKE_CMD="${MAKE:-make}"
$MAKE_CMD clean 2>/dev/null || true
$MAKE_CMD BACKEND="$BACKEND" || die "Build failed — see errors above"
ok "Build complete"

# ─── Install ──────────────────────────────────────────────────────────────────

sep
echo -e "  ${BOLD}Installing${RESET}"
echo -e "  ${DIM}──────────────────────────────${RESET}"
sep

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

# ─── Done ─────────────────────────────────────────────────────────────────────

sep
echo -e "  ${GREEN}${BOLD}Done!${RESET}  ytcui is installed."
sep
echo -e "    ytcui                  start"
echo -e "    ytcui --diag           check system"
echo -e "    ytcui -t dracula       change theme"
echo -e "    ytcui --upgrade        upgrade"
echo ""

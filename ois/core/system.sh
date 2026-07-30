#!/bin/sh
# OIS v4 -- core/system.sh
# Platform, package manager, privilege, and macOS/BSD environment detection.
# ---------------------------------------------------------------------

case "$(uname -s 2>/dev/null)" in
    Linux)     OIS_OS="linux"     ;;
    Darwin)    OIS_OS="macos"     ;;
    FreeBSD)   OIS_OS="freebsd"   ;;
    NetBSD)    OIS_OS="netbsd"    ;;
    OpenBSD)   OIS_OS="openbsd"   ;;
    DragonFly) OIS_OS="dragonfly" ;;
    SunOS)     OIS_OS="illumos"   ;;
    *)         OIS_OS="unknown"   ;;
esac
[ -r /proc/version ] && grep -qi microsoft /proc/version 2>/dev/null && OIS_OS="wsl"

OIS_DISTRO="" OIS_DISTRO_VER=""
case "$OIS_OS" in linux|wsl)
    if [ -r /etc/os-release ]; then
        OIS_DISTRO="$(. /etc/os-release 2>/dev/null; printf '%s' "${ID:-}")"
        OIS_DISTRO_VER="$(. /etc/os-release 2>/dev/null; printf '%s' "${VERSION_ID:-}")"
    elif [ -f /etc/arch-release ];   then OIS_DISTRO="arch"
    elif [ -f /etc/debian_version ]; then OIS_DISTRO="debian"
    fi ;;
esac

case "$(uname -m 2>/dev/null)" in
    x86_64|amd64)  OIS_ARCH="x86_64" ;;
    aarch64|arm64) OIS_ARCH="arm64"  ;;
    armv7*|armv6*) OIS_ARCH="arm"    ;;
    riscv64)       OIS_ARCH="riscv64";;
    i386|i686)     OIS_ARCH="i386"   ;;
    *)             OIS_ARCH="$(uname -m 2>/dev/null || printf 'unknown')" ;;
esac

# -- macOS version detection -------------------------------------------
# sw_vers -productVersion on every macOS from 10.x onward.
# OIS_MACOS_VER: full dotted version e.g. "15.2.1" or "26.0"
# OIS_MACOS_NAME: codename for MacPorts pkg selection
OIS_MACOS_VER="" OIS_MACOS_NAME="" OIS_MACOS_MAJOR=""
if [ "$OIS_OS" = "macos" ] && command -v sw_vers >/dev/null 2>&1; then
    OIS_MACOS_VER="$(sw_vers -productVersion 2>/dev/null || printf '')"
    OIS_MACOS_MAJOR="${OIS_MACOS_VER%%.*}"
    # Sub-version for 10.x (e.g. 10.15 -> "15")
    _ois_macos_minor="${OIS_MACOS_VER#*.}"
    _ois_macos_minor="${_ois_macos_minor%%.*}"
    case "$OIS_MACOS_MAJOR" in
        26) OIS_MACOS_NAME="Tahoe"      ;;
        15) OIS_MACOS_NAME="Sequoia"    ;;
        14) OIS_MACOS_NAME="Sonoma"     ;;
        13) OIS_MACOS_NAME="Ventura"    ;;
        12) OIS_MACOS_NAME="Monterey"   ;;
        11) OIS_MACOS_NAME="BigSur"     ;;
        10) case "$_ois_macos_minor" in
                15) OIS_MACOS_NAME="Catalina"   ;;
                14) OIS_MACOS_NAME="Mojave"     ;;
                13) OIS_MACOS_NAME="HighSierra" ;;
                12) OIS_MACOS_NAME="Sierra"     ;;
                11) OIS_MACOS_NAME="ElCapitan"  ;;
                10) OIS_MACOS_NAME="Yosemite"   ;;
                9)  OIS_MACOS_NAME="Mavericks"  ;;
                *)  OIS_MACOS_NAME="Unknown"    ;;
            esac ;;
        *)  OIS_MACOS_NAME="Unknown" ;;
    esac
fi

# -- Homebrew prefix ---------------------------------------------------
# Intel: /usr/local  |  Apple Silicon: /opt/homebrew  |  Linux: /home/linuxbrew/.linuxbrew
# Never hardcode either. Ask brew, with fallback probing.
OIS_BREW_PREFIX=""
if command -v brew >/dev/null 2>&1; then
    OIS_BREW_PREFIX="$(brew --prefix 2>/dev/null || printf '')"
fi
# If brew not yet on PATH but exists at known locations, expose it
if [ -z "$OIS_BREW_PREFIX" ]; then
    for _bp in /opt/homebrew /usr/local /home/linuxbrew/.linuxbrew; do
        if [ -x "$_bp/bin/brew" ]; then
            OIS_BREW_PREFIX="$_bp"
            eval "$("$_bp/bin/brew" shellenv 2>/dev/null)" 2>/dev/null || true
            break
        fi
    done
fi

# -- MacPorts prefix ---------------------------------------------------
OIS_PORT_PREFIX="/opt/local"
command -v port >/dev/null 2>&1 && {
    _pp="$(port prefix 2>/dev/null)" && [ -n "$_pp" ] && OIS_PORT_PREFIX="$_pp"
}

# -- Package manager ---------------------------------------------------
# On macOS we respect the user-set OIS_PKG_MANAGER env (set by install.sh)
# but still auto-detect to break ties.
OIS_PM="unknown"
case "$OIS_OS" in
    macos)
        # Respect explicit user choice from install.sh
        case "${OIS_PKG_MANAGER:-}" in
            brew|homebrew) OIS_PM="brew"     ;;
            port|macports) OIS_PM="macports" ;;
            *)
                if   command -v brew >/dev/null 2>&1; then OIS_PM="brew"
                elif command -v port >/dev/null 2>&1; then OIS_PM="macports"
                fi ;;
        esac ;;
    freebsd|dragonfly) command -v pkg     >/dev/null 2>&1 && OIS_PM="pkg"     ;;
    netbsd)            command -v pkgin   >/dev/null 2>&1 && OIS_PM="pkgin"   ;;
    openbsd)           command -v pkg_add >/dev/null 2>&1 && OIS_PM="pkg_add" ;;
    illumos)           command -v pkg     >/dev/null 2>&1 && OIS_PM="ips"     ;;
    *)
        if   command -v apt-get      >/dev/null 2>&1; then OIS_PM="apt"
        elif command -v pacman       >/dev/null 2>&1; then OIS_PM="pacman"
        elif command -v dnf          >/dev/null 2>&1; then OIS_PM="dnf"
        elif command -v yum          >/dev/null 2>&1; then OIS_PM="yum"
        elif command -v zypper       >/dev/null 2>&1; then OIS_PM="zypper"
        elif command -v apk          >/dev/null 2>&1; then OIS_PM="apk"
        elif command -v emerge       >/dev/null 2>&1; then OIS_PM="emerge"
        elif command -v xbps-install >/dev/null 2>&1; then OIS_PM="xbps"
        elif command -v nix-env      >/dev/null 2>&1; then OIS_PM="nix"
        fi ;;
esac

# -- Privilege ---------------------------------------------------------
OIS_IS_ROOT="no"
[ "$(id -u 2>/dev/null)" = "0" ] && OIS_IS_ROOT="yes"

OIS_SUDO="none"
if   command -v doas >/dev/null 2>&1 && [ -f /etc/doas.conf ]; then OIS_SUDO="doas"
elif command -v sudo >/dev/null 2>&1; then OIS_SUDO="sudo"
elif command -v doas >/dev/null 2>&1; then OIS_SUDO="doas"
fi

if [ "$OIS_IS_ROOT" = "yes" ]; then
    ois_priv() { "$@"; }
else
    case "$OIS_SUDO" in
        sudo) ois_priv() { sudo "$@"; } ;;
        doas) ois_priv() { doas "$@"; } ;;
        *)    ois_priv() { return 1; }  ;;
    esac
fi

OIS_MAKE="make"
command -v gmake >/dev/null 2>&1 && OIS_MAKE="gmake"

OIS_IS_CI="no"
{ [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; } && OIS_IS_CI="yes"

OIS_USER="${USER:-$(id -un 2>/dev/null)}"
OIS_HOME="${HOME:-/root}"

OIS_XDG_CONFIG="${XDG_CONFIG_HOME:-$OIS_HOME/.config}"
OIS_XDG_DATA="${XDG_DATA_HOME:-$OIS_HOME/.local/share}"
OIS_XDG_CACHE="${XDG_CACHE_HOME:-$OIS_HOME/.cache}"
OIS_XDG_STATE="${XDG_STATE_HOME:-$OIS_HOME/.local/state}"

export OIS_OS OIS_DISTRO OIS_DISTRO_VER OIS_ARCH
export OIS_MACOS_VER OIS_MACOS_MAJOR OIS_MACOS_NAME
export OIS_BREW_PREFIX OIS_PORT_PREFIX
export OIS_PM OIS_IS_ROOT OIS_SUDO OIS_MAKE OIS_IS_CI
export OIS_USER OIS_HOME
export OIS_XDG_CONFIG OIS_XDG_DATA OIS_XDG_CACHE OIS_XDG_STATE

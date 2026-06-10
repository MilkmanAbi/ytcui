#!/bin/sh
# OIS core/system.sh — platform detection, pure POSIX sh
# ────────────────────────────────────────────────────────

_raw="$(uname -s 2>/dev/null)"
case "$_raw" in
    Linux)     OIS_OS="linux"     ;;
    Darwin)    OIS_OS="macos"     ;;
    FreeBSD)   OIS_OS="freebsd"   ;;
    NetBSD)    OIS_OS="netbsd"    ;;
    OpenBSD)   OIS_OS="openbsd"   ;;
    DragonFly) OIS_OS="dragonfly" ;;
    *)         OIS_OS="unknown"   ;;
esac

grep -qi microsoft /proc/version 2>/dev/null && OIS_OS="wsl"

# ── Distro ─────────────────────────────────────────────
OIS_DISTRO="" OIS_DISTRO_VER=""
if [ "$OIS_OS" = "linux" ] || [ "$OIS_OS" = "wsl" ]; then
    if [ -f /etc/os-release ]; then
        OIS_DISTRO="$(. /etc/os-release && printf '%s' "${ID:-}")"
        OIS_DISTRO_VER="$(. /etc/os-release && printf '%s' "${VERSION_ID:-}")"
    elif [ -f /etc/arch-release ];   then OIS_DISTRO="arch"
    elif [ -f /etc/debian_version ]; then OIS_DISTRO="debian"
    fi
fi

# ── Arch ───────────────────────────────────────────────
case "$(uname -m 2>/dev/null)" in
    x86_64|amd64)  OIS_ARCH="x86_64" ;;
    aarch64|arm64) OIS_ARCH="arm64"  ;;
    armv7*)        OIS_ARCH="armv7"  ;;
    i386|i686)     OIS_ARCH="i386"   ;;
    *)             OIS_ARCH="$(uname -m)" ;;
esac

# ── Package manager ────────────────────────────────────
OIS_PM="unknown"
case "$OIS_OS" in
    macos)
        command -v brew    >/dev/null 2>&1 && OIS_PM="brew"
        command -v port    >/dev/null 2>&1 && { [ "$OIS_PM" = "unknown" ] && OIS_PM="macports"; }
        ;;
    freebsd|dragonfly) command -v pkg     >/dev/null 2>&1 && OIS_PM="pkg"    ;;
    netbsd)            command -v pkgin   >/dev/null 2>&1 && OIS_PM="pkgin"  ;;
    openbsd)           command -v pkg_add >/dev/null 2>&1 && OIS_PM="pkg_add";;
    *)
        if   command -v apt-get      >/dev/null 2>&1; then OIS_PM="apt"
        elif command -v pacman       >/dev/null 2>&1; then OIS_PM="pacman"
        elif command -v dnf          >/dev/null 2>&1; then OIS_PM="dnf"
        elif command -v yum          >/dev/null 2>&1; then OIS_PM="yum"
        elif command -v zypper       >/dev/null 2>&1; then OIS_PM="zypper"
        elif command -v apk          >/dev/null 2>&1; then OIS_PM="apk"
        elif command -v emerge       >/dev/null 2>&1; then OIS_PM="emerge"
        elif command -v xbps-install >/dev/null 2>&1; then OIS_PM="xbps"
        elif command -v pkg          >/dev/null 2>&1; then OIS_PM="pkg"
        fi ;;
esac

# ── Privilege ──────────────────────────────────────────
OIS_IS_ROOT="no" ; OIS_SUDO="none"
[ "$(id -u)" -eq 0 ] && OIS_IS_ROOT="yes"
command -v sudo >/dev/null 2>&1 && OIS_SUDO="sudo"
command -v doas >/dev/null 2>&1 && OIS_SUDO="${OIS_SUDO:-doas}"

# Override ois_priv now that we know privilege situation
if [ "$OIS_IS_ROOT" = "yes" ]; then
    ois_priv() { "$@"; }
elif [ "$OIS_SUDO" = "sudo" ]; then
    ois_priv() { sudo "$@"; }
elif [ "$OIS_SUDO" = "doas" ]; then
    ois_priv() { doas "$@"; }
else
    ois_priv() { "$@"; }
fi

# ── Make ───────────────────────────────────────────────
OIS_MAKE="make"
command -v gmake >/dev/null 2>&1 && OIS_MAKE="gmake"

# ── Display ────────────────────────────────────────────
OIS_DISPLAY="none"
[ "$OIS_OS" = "macos" ] && OIS_DISPLAY="quartz"
[ -n "${WAYLAND_DISPLAY:-}" ] && OIS_DISPLAY="wayland"
[ -n "${DISPLAY:-}" ] && [ "$OIS_DISPLAY" = "none" ] && OIS_DISPLAY="x11"

# ── Runtimes ───────────────────────────────────────────
OIS_PYTHON="" OIS_PIP=""
command -v python3 >/dev/null 2>&1 && OIS_PYTHON="python3"
command -v python  >/dev/null 2>&1 && [ -z "$OIS_PYTHON" ] && OIS_PYTHON="python"
command -v pip3    >/dev/null 2>&1 && OIS_PIP="pip3"
command -v pip     >/dev/null 2>&1 && [ -z "$OIS_PIP"    ] && OIS_PIP="pip"

OIS_DOTNET=""
command -v dotnet >/dev/null 2>&1 && OIS_DOTNET="dotnet"
command -v mono   >/dev/null 2>&1 && [ -z "$OIS_DOTNET" ] && OIS_DOTNET="mono"

# ── Misc ───────────────────────────────────────────────
OIS_IS_CI="no"
{ [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; } && OIS_IS_CI="yes"
OIS_USER="${USER:-$(id -un 2>/dev/null)}"
OIS_HOME="${HOME:-/root}"
OIS_BREW_PREFIX=""
[ "$OIS_OS" = "macos" ] && command -v brew >/dev/null 2>&1 && \
    OIS_BREW_PREFIX="$(brew --prefix 2>/dev/null)"

export OIS_OS OIS_DISTRO OIS_DISTRO_VER OIS_ARCH
export OIS_PM OIS_IS_ROOT OIS_SUDO OIS_MAKE OIS_DISPLAY
export OIS_PYTHON OIS_PIP OIS_DOTNET
export OIS_IS_CI OIS_USER OIS_HOME OIS_BREW_PREFIX

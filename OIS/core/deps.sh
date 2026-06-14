#!/bin/sh
# OIS core/deps.sh — dependency installation
# Key feature: macOS asks which PM you want if none found, installs it.
# ─────────────────────────────────────────────────────────────────────

# ── Single package install via detected PM ────────────
_ois_pm_install() {
    case "$OIS_PM" in
        apt)      ois_priv apt-get install -y "$1" ;;
        pacman)   ois_priv pacman -S --noconfirm "$1" ;;
        dnf)      ois_priv dnf install -y "$1" ;;
        yum)      ois_priv yum install -y "$1" ;;
        zypper)   ois_priv zypper install -y "$1" ;;
        apk)      ois_priv apk add --no-cache "$1" ;;
        emerge)   ois_priv emerge "$1" ;;
        xbps)     ois_priv xbps-install -y "$1" ;;
        brew)     brew install "$1" ;;
        macports) ois_priv port install "$1" ;;
        pkg)      ois_priv pkg install -y "$1" ;;
        pkg_add)  ois_priv pkg_add "$1" ;;
        pkgin)    ois_priv pkgin -y install "$1" ;;
        *)        ois_warn "No package manager — install '$1' manually"; return 1 ;;
    esac
}

# ── Update package index (once per session) ───────────
_ois_pm_updated=0
_ois_pm_update() {
    [ "$_ois_pm_updated" = "1" ] && return 0
    case "$OIS_PM" in
        apt)    ois_priv apt-get update -qq 2>/dev/null || true ;;
        pacman) ois_priv pacman -Sy --noconfirm 2>/dev/null || true ;;
        dnf)    ois_priv dnf check-update -q 2>/dev/null || true ;;
        apk)    ois_priv apk update -q 2>/dev/null || true ;;
        brew)   brew update -q 2>/dev/null || true ;;
        pkg)    ois_priv pkg update 2>/dev/null || true ;;
        *)      true ;;
    esac
    _ois_pm_updated=1
}

# ── macOS: bootstrap a package manager if none found ──
_ois_macos_bootstrap_pm() {
    [ "$OIS_OS" != "macos" ] && return 0
    [ "$OIS_PM" != "unknown" ] && return 0

    printf "\n  ${_Y}No package manager found on macOS.${_R}\n\n"
    printf "  OIS can set one up for you:\n\n"
    printf "    1) Homebrew   — install automatically (recommended)\n"
    printf "    2) MacPorts   — download installer, then continue\n"
    printf "    3) Skip       — I'll install dependencies manually\n"
    printf "\n  Choice [1/2/3]: "
    read -r _pm_choice

    case "$_pm_choice" in
        2)
            _macos_ver="$(sw_vers -productVersion 2>/dev/null)"
            ois_info "Opening the MacPorts download page in your browser..."
            open "https://www.macports.org/install.php" 2>/dev/null || true
            printf "\n  Download and run the MacPorts .pkg installer for macOS ${_macos_ver}.\n"
            printf "  When done, press Enter to continue...\n"
            read -r _
            # Try to pick up the newly installed port command
            for _portbin in /opt/local/bin/port /usr/local/bin/port; do
                [ -x "$_portbin" ] && PATH="$(dirname "$_portbin"):$PATH" && break
            done
            if command -v port >/dev/null 2>&1; then
                OIS_PM="macports"
                export OIS_PM
                ois_ok "MacPorts detected"
            else
                ois_die "MacPorts not found after installation. Try opening a new terminal and re-running install.sh"
            fi
            ;;
        3)
            ois_warn "Skipping package manager — you'll need to install deps manually"
            return 0
            ;;
        *)
            ois_info "Installing Homebrew..."
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" \
                || ois_die "Homebrew installation failed. Try manually: https://brew.sh"
            # Add brew to PATH for this session (arm64 = /opt/homebrew, intel = /usr/local)
            if [ -f "/opt/homebrew/bin/brew" ]; then
                eval "$(/opt/homebrew/bin/brew shellenv)"
                OIS_BREW_PREFIX="/opt/homebrew"
            elif [ -f "/usr/local/bin/brew" ]; then
                eval "$(/usr/local/bin/brew shellenv)"
                OIS_BREW_PREFIX="/usr/local"
            fi
            OIS_PM="brew"
            export OIS_PM OIS_BREW_PREFIX
            ois_ok "Homebrew installed"
            ;;
    esac
}

# ── macOS: set PKG_CONFIG_PATH for keg-only Homebrew formulae ─────────────────
# curl, ncurses, openssl@3 install to their own Cellar prefix and are NOT
# symlinked — their .pc files are invisible to pkg-config without this.
# Called after PM is known and any bootstrap has run.
_ois_macos_pkgconfig_path() {
    [ "$OIS_OS" != "macos" ] || [ "$OIS_PM" != "brew" ] && return 0
    _bp="$(brew --prefix 2>/dev/null)" ; [ -z "$_bp" ] && return 0
    _extra=""
    for _f in curl ncurses "openssl@3" "openssl@1.1"; do
        _keg="$(brew --prefix "$_f" 2>/dev/null)/lib/pkgconfig"
        [ -d "$_keg" ] && _extra="${_extra}${_keg}:"
    done
    # MacPorts fallback paths (harmless if dirs don't exist)
    _extra="${_extra}/opt/local/lib/pkgconfig:/opt/local/share/pkgconfig"
    if [ -n "$PKG_CONFIG_PATH" ]; then
        export PKG_CONFIG_PATH="${_extra}:${PKG_CONFIG_PATH}"
    else
        export PKG_CONFIG_PATH="${_extra}"
    fi
}

# ── Xcode CLI tools (macOS build requirement) ─────────
_ois_macos_xcode() {
    [ "$OIS_OS" != "macos" ] && return 0
    xcode-select -p >/dev/null 2>&1 && return 0
    ois_info "Installing Xcode Command Line Tools..."
    xcode-select --install 2>/dev/null || true
    printf "\n  Complete the installation dialog that appeared,\n"
    printf "  then press Enter to continue...\n"
    read -r _
    xcode-select -p >/dev/null 2>&1 || ois_die "Xcode CLI Tools required — install and retry"
    ois_ok "Xcode Command Line Tools"
}

# ── Install all declared deps ──────────────────────────
ois_deps_install() {
    # macOS: ensure we have a package manager before anything else
    _ois_macos_bootstrap_pm
    _ois_macos_pkgconfig_path
    _ois_macos_xcode

    [ "$OIS_DEP_COUNT" -eq 0 ] && return 0
    _ois_pm_update

    _i=0 ; _failed=0
    while [ "$_i" -lt "$OIS_DEP_COUNT" ]; do
        eval "_name=\$OIS_DEP_${_i}_NAME"
        eval "_opt=\$OIS_DEP_${_i}_OPT"
        eval "_cmd=\$OIS_DEP_${_i}_CMD"
        eval "_desc=\$OIS_DEP_${_i}_DESC"
        eval "_pkg=\$OIS_DEP_${_i}_PKG_${OIS_PM}"
        eval "_pc=\$OIS_DEP_${_i}_PKGCONFIG"

        _check="${_cmd:-$_name}"
        # Check if already available:
        # 0. pkg-config module present  (authoritative for -dev/-devel libraries:
        #    a runtime CLI like `openssl`/`curl` existing does NOT mean the build
        #    headers are installed, so we must not rely on command -v for those)
        # 1. command exists in PATH
        # 2. brew keg-only: installed but not linked (brew list check)
        # 3. macOS system ncurses: headers present even without ncurses-config
        _already=0
        if [ -n "$_pc" ]; then
            if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$_pc" 2>/dev/null; then
                _already=1
            fi
        elif command -v "$_check" >/dev/null 2>&1; then
            _already=1
        elif [ "$OIS_PM" = "brew" ] && [ -n "$_pkg" ] && brew list "$_pkg" >/dev/null 2>&1; then
            _already=1
        elif [ "$OIS_OS" = "macos" ] && [ "$_name" = "ncurses" ]; then
            # macOS always ships ncurses via Xcode/SDK
            [ -f "$(xcrun --show-sdk-path 2>/dev/null)/usr/include/ncurses.h" ] && _already=1
            [ -f "/usr/include/ncurses.h" ] && _already=1
        fi
        if [ "$_already" = "1" ]; then
            ois_ok "$_name  (already installed)"
            _i=$(( _i + 1 )) ; continue
        fi

        if [ -z "$_pkg" ]; then
            [ "$_opt" = "yes" ] \
                && ois_warn "$_name — no package for $OIS_PM (optional, skipping)" \
                || { ois_warn "$_name — no package for '$OIS_PM', install manually"; _failed=1; }
            _i=$(( _i + 1 )) ; continue
        fi

        _lbl="$_name" ; [ -n "$_desc" ] && _lbl="$_name  ($_desc)"
        ois_info "Installing $_lbl..."
        if _ois_pm_install "$_pkg" >/dev/null 2>&1; then
            # For keg-only brew formulae, update PKG_CONFIG_PATH so the next
            # dep's pkg-config check can find the .pc file we just installed.
            if [ "$OIS_PM" = "brew" ]; then
                _kpc="$(brew --prefix "$_pkg" 2>/dev/null)/lib/pkgconfig"
                [ -d "$_kpc" ] && export PKG_CONFIG_PATH="${_kpc}:${PKG_CONFIG_PATH:-}"
            fi
            ois_ok "$_name"
        else
            # brew keg-only packages won't show up via command -v even when installed.
            # Check brew list as a fallback before declaring failure.
            if [ "$OIS_PM" = "brew" ] && brew list "$_pkg" >/dev/null 2>&1; then
                ois_ok "$_name  (keg-only, headers available)"
            elif [ "$_opt" = "yes" ]; then
                ois_warn "$_name install failed (optional — skipping)"
            else
                ois_err "$_name install FAILED"; _failed=1
            fi
        fi
        _i=$(( _i + 1 ))
    done
    return "$_failed"
}

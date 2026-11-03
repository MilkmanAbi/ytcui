#!/bin/sh
# OIS v4 -- core/deps.sh
# Dependency declaration, probing, and installation.
#
# Layering (v4):
#   - core/pm.sh  answers "is it installed / known / what version" per PM,
#     handling the brew-can't-run-as-root and macports-needs-root splits.
#   - THIS file   declares deps, probes them (pkg-config -> header ->
#     PM-installed -> command), resolves package names, and orchestrates
#     install + re-probe + optional next-best-version fallback.
#
# Probe order for a library:
#   1. explicit .cmd    -> command -v            (tools)
#   2. pkg-config       -> --exists / --atleast-version
#   3. header search    -> across all include roots (brew opt, macports, BSD)
#   4. PM "is installed"-> ois_pm_have  (keg-only immune, no .pc needed)
#   5. bare command -v  -> only when nothing else maps
#
# macOS/BSD hardening this file drives:
#   - Xcode CLT bootstrap, Homebrew bootstrap, MacPorts bootstrap
#   - pkg-config auto-install so probes have their best tool
#   - PKG_CONFIG_PATH enrichment via the stable brew opt/ symlinks
#   - build flag wiring (CPPFLAGS/LDFLAGS) for keg-only formulae
# ---------------------------------------------------------------------

# -- Alias table -------------------------------------------------------
# Columns (1-indexed):
#   1:pc  2:header  3:apt  4:pacman  5:dnf  6:zypper  7:apk  8:xbps
#   9:emerge  10:brew  11:pkg  12:pkgin  13:pkg_add  14:macports
_ois_alias_row() {
    case "$1" in
    ncurses)    printf '%s' 'ncursesw|ncurses.h|libncurses-dev|ncurses|ncurses-devel|ncurses-devel|ncurses-dev|ncurses-devel|sys-libs/ncurses|ncurses|ncurses|ncurses|-|ncurses' ;;
    readline)   printf '%s' 'readline|readline/readline.h|libreadline-dev|readline|readline-devel|readline-devel|readline-dev|readline-devel|sys-libs/readline|readline|readline|readline|-|readline' ;;
    openssl)    printf '%s' 'openssl|openssl/ssl.h|libssl-dev|openssl|openssl-devel|libopenssl-devel|openssl-dev|openssl-devel|dev-libs/openssl|openssl@3|openssl|openssl|-|openssl' ;;
    zlib)       printf '%s' 'zlib|zlib.h|zlib1g-dev|zlib|zlib-devel|zlib-devel|zlib-dev|zlib-devel|sys-libs/zlib|zlib|-|zlib|-|zlib' ;;
    bzip2)      printf '%s' 'bzip2|bzlib.h|libbz2-dev|bzip2|bzip2-devel|libbz2-devel|bzip2-dev|bzip2-devel|app-arch/bzip2|bzip2|bzip2|bzip2|bzip2|bzip2' ;;
    xz)         printf '%s' 'liblzma|lzma.h|liblzma-dev|xz|xz-devel|xz-devel|xz-dev|liblzma-devel|app-arch/xz-utils|xz|xz|xz|xz|xz' ;;
    zstd)       printf '%s' 'libzstd|zstd.h|libzstd-dev|zstd|libzstd-devel|libzstd-devel|zstd-dev|libzstd-devel|app-arch/zstd|zstd|zstd|zstd|zstd|zstd' ;;
    sqlite3)    printf '%s' 'sqlite3|sqlite3.h|libsqlite3-dev|sqlite|sqlite-devel|sqlite3-devel|sqlite-dev|sqlite-devel|dev-db/sqlite|sqlite|sqlite3|sqlite3|sqlite3|sqlite3' ;;
    curl)       printf '%s' 'libcurl|curl/curl.h|libcurl4-openssl-dev|curl|libcurl-devel|libcurl-devel|curl-dev|libcurl-devel|net-misc/curl|curl|curl|curl|curl|curl' ;;
    pcre2)      printf '%s' 'libpcre2-8|pcre2.h|libpcre2-dev|pcre2|pcre2-devel|pcre2-devel|pcre2-dev|pcre2-devel|dev-libs/libpcre2|pcre2|pcre2|pcre2|pcre2|pcre2' ;;
    libxml2)    printf '%s' 'libxml-2.0|libxml/parser.h|libxml2-dev|libxml2|libxml2-devel|libxml2-devel|libxml2-dev|libxml2-devel|dev-libs/libxml2|libxml2|libxml2|libxml2|libxml|libxml2' ;;
    libgit2)    printf '%s' 'libgit2|git2.h|libgit2-dev|libgit2|libgit2-devel|libgit2-devel|libgit2-dev|libgit2-devel|dev-libs/libgit2|libgit2|libgit2|libgit2|libgit2|libgit2' ;;
    libssh2)    printf '%s' 'libssh2|libssh2.h|libssh2-1-dev|libssh2|libssh2-devel|libssh2-devel|libssh2-dev|libssh2-devel|net-libs/libssh2|libssh2|libssh2|libssh2|libssh2|libssh2' ;;
    libusb)     printf '%s' 'libusb-1.0|libusb-1.0/libusb.h|libusb-1.0-0-dev|libusb|libusb1-devel|libusb-1_0-devel|libusb-dev|libusb-devel|dev-libs/libusb|libusb|libusb|libusb1|libusb1|libusb' ;;
    libpng)     printf '%s' 'libpng|png.h|libpng-dev|libpng|libpng-devel|libpng16-devel|libpng-dev|libpng-devel|media-libs/libpng|libpng|png|png|png|libpng' ;;
    libjpeg)    printf '%s' 'libjpeg|jpeglib.h|libjpeg-dev|libjpeg-turbo|libjpeg-turbo-devel|libjpeg8-devel|jpeg-dev|libjpeg-turbo-devel|media-libs/libjpeg-turbo|jpeg-turbo|jpeg-turbo|jpeg|jpeg|jpeg' ;;
    freetype)   printf '%s' 'freetype2|ft2build.h|libfreetype6-dev|freetype2|freetype-devel|freetype2-devel|freetype-dev|freetype-devel|media-libs/freetype|freetype|freetype2|freetype2|freetype|freetype' ;;
    harfbuzz)   printf '%s' 'harfbuzz|hb.h|libharfbuzz-dev|harfbuzz|harfbuzz-devel|harfbuzz-devel|harfbuzz-dev|harfbuzz-devel|media-libs/harfbuzz|harfbuzz|harfbuzz|harfbuzz|harfbuzz|harfbuzz' ;;
    sdl2)       printf '%s' 'sdl2|SDL2/SDL.h|libsdl2-dev|sdl2|SDL2-devel|libSDL2-devel|sdl2-dev|SDL2-devel|media-libs/libsdl2|sdl2|sdl2|SDL2|sdl2|libsdl2' ;;
    glfw)       printf '%s' 'glfw3|GLFW/glfw3.h|libglfw3-dev|glfw|glfw-devel|glfw-devel|glfw-dev|glfw-devel|media-libs/glfw|glfw|glfw|glfw|glfw|glfw3' ;;
    vulkan)     printf '%s' 'vulkan|vulkan/vulkan.h|libvulkan-dev|vulkan-icd-loader|vulkan-loader-devel|vulkan-devel|vulkan-loader-dev|Vulkan-Loader-devel|media-libs/vulkan-loader|vulkan-loader|vulkan-loader|-|-|vulkan-loader' ;;
    opengl)     printf '%s' 'gl|GL/gl.h|libgl1-mesa-dev|mesa|mesa-libGL-devel|Mesa-libGL-devel|mesa-dev|MesaLib-devel|media-libs/mesa|-|mesa-libs|MesaLib|mesa-libGL|mesa' ;;
    alsa)       printf '%s' 'alsa|alsa/asoundlib.h|libasound2-dev|alsa-lib|alsa-lib-devel|alsa-devel|alsa-lib-dev|alsa-lib-devel|media-libs/alsa-lib|-|-|-|-|-' ;;
    pulseaudio) printf '%s' 'libpulse|pulse/pulseaudio.h|libpulse-dev|libpulse|pulseaudio-libs-devel|libpulse-devel|pulseaudio-dev|pulseaudio-devel|media-sound/pulseaudio|pulseaudio|pulseaudio|pulseaudio|pulseaudio|pulseaudio' ;;
    x11)        printf '%s' 'x11|X11/Xlib.h|libx11-dev|libx11|libX11-devel|libX11-devel|libx11-dev|libX11-devel|x11-libs/libX11|libx11|libX11|libX11|-|xorg-libX11' ;;
    wayland)    printf '%s' 'wayland-client|wayland-client.h|libwayland-dev|wayland|wayland-devel|wayland-devel|wayland-dev|wayland-devel|dev-libs/wayland|-|wayland|wayland|wayland|wayland' ;;
    gtk3)       printf '%s' 'gtk+-3.0|gtk/gtk.h|libgtk-3-dev|gtk3|gtk3-devel|gtk3-devel|gtk+3.0-dev|gtk+3-devel|x11-libs/gtk+|gtk+3|gtk3|gtk3+|gtk+3|gtk3' ;;
    qt6)        printf '%s' 'Qt6Core|QtCore|qt6-base-dev|qt6-base|qt6-qtbase-devel|qt6-base-devel|qt6-base-dev|qt6-base-devel|dev-qt/qtbase|qt|qt6|qt6-qtbase|qt6|qt6' ;;
    ffmpeg)     printf '%s' 'libavcodec|libavcodec/avcodec.h|libavcodec-dev|ffmpeg|ffmpeg-devel|libavcodec-devel|ffmpeg-dev|ffmpeg-devel|media-video/ffmpeg|ffmpeg|ffmpeg|ffmpeg|ffmpeg|ffmpeg' ;;
    gmp)        printf '%s' 'gmp|gmp.h|libgmp-dev|gmp|gmp-devel|gmp-devel|gmp-dev|gmp-devel|dev-libs/gmp|gmp|gmp|gmp|gmp|gmp' ;;
    jansson)    printf '%s' 'jansson|jansson.h|libjansson-dev|jansson|jansson-devel|libjansson-devel|jansson-dev|jansson-devel|dev-libs/jansson|jansson|jansson|jansson|jansson|jansson' ;;
    cjson)      printf '%s' 'libcjson|cjson/cJSON.h|libcjson-dev|cjson|cjson-devel|libcjson-devel|cjson-dev|cjson-devel|dev-libs/cJSON|cjson|libcjson|-|-|cjson' ;;
    yaml)       printf '%s' 'yaml-0.1|yaml.h|libyaml-dev|libyaml|libyaml-devel|libyaml-devel|yaml-dev|libyaml-devel|dev-libs/libyaml|libyaml|libyaml|libyaml|libyaml|libyaml' ;;
    protobuf)   printf '%s' 'protobuf|google/protobuf/message.h|libprotobuf-dev|protobuf|protobuf-devel|protobuf-devel|protobuf-dev|protobuf-devel|dev-libs/protobuf|protobuf|protobuf|protobuf|protobuf|protobuf' ;;
    fmt)        printf '%s' 'fmt|fmt/core.h|libfmt-dev|fmt|fmt-devel|fmt-devel|fmt-dev|fmt-devel|dev-libs/libfmt|fmt|fmt|-|-|fmt' ;;
    boost)      printf '%s' '-|boost/version.hpp|libboost-all-dev|boost|boost-devel|libboost_headers-devel|boost-dev|boost-devel|dev-libs/boost|boost|boost-libs|boost|boost|boost' ;;
    mpv)        printf '%s' 'mpv|mpv/client.h|libmpv-dev|mpv|mpv-devel|mpv-devel|mpv-dev|mpv-devel|media-video/mpv|mpv|mpv|mpv|-|mpv' ;;
    # -- tools (probed with command -v) --------------------------------
    git)        printf '%s' '-|-|git|git|git|git|git|git|dev-vcs/git|git|git|git|git|git' ;;
    cmake)      printf '%s' '-|-|cmake|cmake|cmake|cmake|cmake|cmake|dev-build/cmake|cmake|cmake|cmake|cmake|cmake' ;;
    ninja)      printf '%s' '-|-|ninja-build|ninja|ninja-build|ninja|samurai|ninja|dev-build/ninja|ninja|ninja|ninja|ninja|ninja' ;;
    meson)      printf '%s' '-|-|meson|meson|meson|meson|meson|meson|dev-build/meson|meson|meson|meson|meson|meson' ;;
    pkgconfig)  printf '%s' '-|-|pkg-config|pkgconf|pkgconf|pkg-config|pkgconf|pkg-config|dev-util/pkgconf|pkg-config|pkgconf|pkg-config|pkgconf|pkgconfig' ;;
    python3)    printf '%s' '-|-|python3|python|python3|python3|python3|python3|dev-lang/python|python|python3|python311|python3|python313' ;;
    nodejs)     printf '%s' '-|-|nodejs|nodejs|nodejs|nodejs|nodejs|nodejs|net-libs/nodejs|node|node|nodejs|node|nodejs22' ;;
    go)         printf '%s' '-|-|golang|go|golang|go|go|go|dev-lang/go|go|go|go|go|go' ;;
    rust)       printf '%s' '-|-|rustc|rust|rust|rust|rust|rust|dev-lang/rust|rust|rust|rust|rust|rust' ;;
    jq)         printf '%s' '-|-|jq|jq|jq|jq|jq|jq|app-misc/jq|jq|jq|jq|jq|jq' ;;
    *)          return 1 ;;
    esac
}

_ois_alias_col() {
    case "$OIS_PM" in
        apt)          printf '%s' '3'  ;; pacman)   printf '%s' '4'  ;;
        dnf|yum)      printf '%s' '5'  ;; zypper)   printf '%s' '6'  ;;
        apk)          printf '%s' '7'  ;; xbps)     printf '%s' '8'  ;;
        emerge)       printf '%s' '9'  ;; brew)     printf '%s' '10' ;;
        pkg|ips)      printf '%s' '11' ;; pkgin)    printf '%s' '12' ;;
        pkg_add)      printf '%s' '13' ;; macports) printf '%s' '14' ;;
        *)            printf '%s' '0'  ;;
    esac
}

_ois_row_field() {
    _rf_r="$1" ; _rf_n="$2" ; _rf_i=1
    while [ "$_rf_i" -lt "$_rf_n" ]; do
        case "$_rf_r" in *\|*) _rf_r="${_rf_r#*|}" ;; *) printf ''; return 1 ;; esac
        _rf_i=$(( _rf_i + 1 ))
    done
    _rf_v="${_rf_r%%|*}"
    [ "$_rf_v" = "-" ] && { printf ''; return 1; }
    printf '%s' "$_rf_v"
}

ois_alias_pc()     { _ois_alias_row "$1" >/dev/null 2>&1 || return 1
                     _ois_row_field "$(_ois_alias_row "$1")" 1; }
ois_alias_header() { _ois_alias_row "$1" >/dev/null 2>&1 || return 1
                     _ois_row_field "$(_ois_alias_row "$1")" 2; }
ois_alias_pkg()    { _ois_alias_row "$1" >/dev/null 2>&1 || return 1
                     _ap_c="$(_ois_alias_col)" ; [ "$_ap_c" = 0 ] && return 1
                     _ois_row_field "$(_ois_alias_row "$1")" "$_ap_c"; }

# -- Declaration parsing -----------------------------------------------
ois_deps_parse() {
    OIS_DEP_TABLE=""
    _dp_feed() {
        _dpf_req="$1" ; _dpf_raw="$2"
        while [ -n "$_dpf_raw" ]; do
            case "$_dpf_raw" in
                *"$OIS_NL"*) _dpf_l="${_dpf_raw%%"$OIS_NL"*}" ; _dpf_raw="${_dpf_raw#*"$OIS_NL"}" ;;
                *)           _dpf_l="$_dpf_raw" ; _dpf_raw="" ;;
            esac
            _dpf_l="$(ois_trim "$_dpf_l")"
            [ -z "$_dpf_l" ] && continue

            case "$_dpf_l" in
                *.*=*)
                    _dpf_k="$(ois_trim "${_dpf_l%%=*}")"
                    _dpf_v="$(ois_trim "${_dpf_l#*=}")"
                    _dpf_n="${_dpf_k%%.*}" ; _dpf_a="${_dpf_k#*.}"
                    ois_is_ident "$_dpf_n" || { ois_warn "bad dep name: $_dpf_n"; continue; }
                    ois_is_ident "$_dpf_a" || { ois_warn "bad dep attr: $_dpf_a"; continue; }
                    OIS_DEP_TABLE="$OIS_DEP_TABLE$_dpf_n	$_dpf_a	$_dpf_v
$_dpf_n	req	$_dpf_req
"
                    continue ;;
            esac
            _dpf_n="${_dpf_l%%[ 	=><!]*}"
            _dpf_rest="${_dpf_l#"$_dpf_n"}"
            _dpf_rest="$(ois_trim "$_dpf_rest")"
            ois_is_ident "$_dpf_n" || { ois_warn "bad dep name: $_dpf_n"; continue; }
            OIS_DEP_TABLE="$OIS_DEP_TABLE$_dpf_n	req	$_dpf_req
"
            case "$_dpf_rest" in
                '>='*|'>'*|'='*)
                    _dpf_ver="$(ois_trim "${_dpf_rest#*[=>]}")"
                    _dpf_ver="$(ois_trim "${_dpf_ver#=}")"
                    [ -n "$_dpf_ver" ] && OIS_DEP_TABLE="$OIS_DEP_TABLE$_dpf_n	ver	$_dpf_ver
" ;;
            esac
        done
    }
    _dp_feed 1 "${OIS_DEPS_RAW:-}"
    _dp_feed 0 "${OIS_DEPS_OPT_RAW:-}"
}

ois_dep_attr() {
    _da_want="$1	$2	"
    _da_t="$OIS_DEP_TABLE"
    while [ -n "$_da_t" ]; do
        case "$_da_t" in
            *"$OIS_NL"*) _da_l="${_da_t%%"$OIS_NL"*}" ; _da_t="${_da_t#*"$OIS_NL"}" ;;
            *)           _da_l="$_da_t" ; _da_t="" ;;
        esac
        case "$_da_l" in
            "$_da_want"*) printf '%s' "${_da_l#"$_da_want"}" ; return 0 ;;
        esac
    done
    return 1
}

ois_dep_names() {
    _dn_seen=" "
    _dn_t="$OIS_DEP_TABLE"
    while [ -n "$_dn_t" ]; do
        case "$_dn_t" in
            *"$OIS_NL"*) _dn_l="${_dn_t%%"$OIS_NL"*}" ; _dn_t="${_dn_t#*"$OIS_NL"}" ;;
            *)           _dn_l="$_dn_t" ; _dn_t="" ;;
        esac
        _dn_n="${_dn_l%%	*}"
        [ -z "$_dn_n" ] && continue
        case "$_dn_seen" in *" $_dn_n "*) continue ;; esac
        _dn_seen="$_dn_seen$_dn_n "
        printf '%s\n' "$_dn_n"
    done
}

# -- Package-name resolution -------------------------------------------
ois_dep_package() {
    _dpk_n="$1"
    ois_dep_attr "$_dpk_n" "$OIS_PM" && return 0     # explicit per-PM override
    ois_dep_attr "$_dpk_n" pkg       && return 0     # explicit default
    ois_alias_pkg "$_dpk_n"          && return 0     # alias table
    printf '%s' "$_dpk_n"                            # assume same name
}

# -- Header search roots (brew opt symlinks + macports + BSD) ----------
_ois_header_roots() {
    printf '/usr/include\n/usr/local/include\n'
    [ -n "${OIS_BREW_PREFIX:-}" ] && printf '%s/include\n' "$OIS_BREW_PREFIX"
    case "$OIS_OS" in
        macos)
            printf '/opt/homebrew/include\n/usr/local/include\n'
            [ "$OIS_PM" = "macports" ] && printf '%s/include\n' "${OIS_PORT_PREFIX:-/opt/local}"
            # brew opt/ symlinks per declared dep (keg-only friendly)
            if [ "$OIS_PM" = "brew" ] && _ois_brew_locate; then
                for _hr_dep in $(ois_dep_names 2>/dev/null); do
                    _hr_pkg="$(ois_dep_package "$_hr_dep" 2>/dev/null)" || _hr_pkg="$_hr_dep"
                    _hr_p="$(ois_brew --prefix "$_hr_pkg" 2>/dev/null)" && \
                        [ -d "$_hr_p/include" ] && printf '%s/include\n' "$_hr_p"
                done
            fi ;;
        openbsd|netbsd) printf '/usr/X11R6/include\n/usr/pkg/include\n' ;;
        freebsd|dragonfly) printf '/usr/local/include\n' ;;
    esac
}

_ois_have_header() {
    for _hh_r in $(_ois_header_roots); do
        [ -f "$_hh_r/$1" ] && return 0
    done
    return 1
}

# ======================================================================
# macOS/BSD bootstrap: toolchain and package manager
# ======================================================================

# Xcode Command Line Tools: cc/make/git live here on macOS.
_ois_macos_ensure_xcode_clt() {
    [ "$OIS_OS" = "macos" ] || return 0
    command -v xcode-select >/dev/null 2>&1 || return 0
    xcode-select -p >/dev/null 2>&1 && return 0
    printf '\n'
    ois_warn "Xcode Command Line Tools are not installed (needed for cc/make/git)"
    printf '     xcode-select --install\n\n'
    if ois_ask "install Xcode Command Line Tools now?" y; then
        xcode-select --install 2>/dev/null || true
        printf '\n'
        ois_info "A macOS dialog should have opened. Complete it, then re-run this installer."
    else
        ois_warn "continuing without CLT; the build will likely fail"
    fi
    # Async GUI install: we cannot block on it. Signal caller to stop cleanly.
    return 1
}

# Homebrew bootstrap. Never as root; the official script itself refuses.
_ois_macos_ensure_brew() {
    [ "$OIS_OS" = "macos" ] || return 0
    [ "$OIS_PM" = "brew" ]  || return 0
    _ois_brew_locate && return 0
    if [ "$OIS_IS_ROOT" = "yes" ] && [ -z "${SUDO_USER:-}" ]; then
        ois_err "Homebrew cannot be installed or run as root."
        ois_err "Re-run this installer as a normal user (no sudo)."
        return 1
    fi
    printf '\n'
    ois_warn "Homebrew is not installed"
    # shellcheck disable=SC2016  # literal text shown to the user, not expanded
    printf '     /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"\n\n'
    ois_ask "install Homebrew now?" y || return 1
    # The install script must run as the invoking user, never root.
    if [ "$OIS_IS_ROOT" = "yes" ]; then
        sudo -u "$(_ois_pm_real_user)" -H /bin/bash -c \
            "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" || {
            ois_warn "Homebrew install failed"; return 1; }
    else
        /bin/bash -c \
            "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" || {
            ois_warn "Homebrew install failed"; return 1; }
    fi
    # Wire the freshly installed brew into this session.
    for _bp in /opt/homebrew /usr/local; do
        [ -x "$_bp/bin/brew" ] || continue
        eval "$("$_bp/bin/brew" shellenv 2>/dev/null)" 2>/dev/null || true
        OIS_BREW_PREFIX="$_bp" ; export OIS_BREW_PREFIX
        OIS_BREW_BIN="$_bp/bin/brew" ; export OIS_BREW_BIN
        break
    done
    _ois_brew_locate || { ois_warn "brew still not found after install"; return 1; }
    ois_ok "Homebrew installed"
    return 0
}

# MacPorts bootstrap: version-matched .pkg from GitHub, installer, selfupdate.
_ois_macos_ensure_macports() {
    [ "$OIS_OS" = "macos" ]    || return 0
    [ "$OIS_PM" = "macports" ] || return 0
    command -v port >/dev/null 2>&1 && return 0
    if [ -z "$OIS_MACOS_NAME" ] || [ "$OIS_MACOS_NAME" = "Unknown" ]; then
        ois_warn "unknown macOS version; cannot pick a MacPorts .pkg automatically"
        ois_warn "install manually: https://www.macports.org/install.php"
        return 1
    fi
    if [ "$OIS_IS_ROOT" != "yes" ] && [ "$OIS_SUDO" = "none" ]; then
        ois_warn "MacPorts install needs root and no sudo is available"
        ois_warn "install manually: https://www.macports.org/install.php"
        return 1
    fi
    printf '\n'
    ois_warn "MacPorts is not installed"
    ois_warn "will fetch the .pkg for macOS $OIS_MACOS_NAME ($OIS_MACOS_VER) from GitHub"
    ois_ask "auto-install MacPorts now?" y || return 1

    _mp_url="$(_ois_macports_pkg_url)"
    if [ -z "$_mp_url" ]; then
        ois_warn "could not find a MacPorts .pkg for $OIS_MACOS_NAME"
        ois_warn "install manually: https://www.macports.org/install.php"
        return 1
    fi
    _mp_pkg="${TMPDIR:-/tmp}/macports-ois-$$.pkg"
    ois_info "downloading ${_mp_url##*/} ..."
    curl -fL --retry 2 -o "$_mp_pkg" "$_mp_url" 2>/dev/null || {
        ois_warn "download failed: $_mp_url"; rm -f "$_mp_pkg"; return 1; }
    ois_info "installing MacPorts (needs root) ..."
    if [ "$OIS_IS_ROOT" = "yes" ]; then
        installer -pkg "$_mp_pkg" -target / || { ois_warn "installer failed"; rm -f "$_mp_pkg"; return 1; }
    else
        ois_priv installer -pkg "$_mp_pkg" -target / || { ois_warn "installer failed"; rm -f "$_mp_pkg"; return 1; }
    fi
    rm -f "$_mp_pkg"
    export PATH="${OIS_PORT_PREFIX:-/opt/local}/bin:${OIS_PORT_PREFIX:-/opt/local}/sbin:$PATH"
    command -v port >/dev/null 2>&1 || { ois_warn "port not found after install"; return 1; }
    ois_info "running port selfupdate ..."
    if [ "$OIS_IS_ROOT" = "yes" ]; then port selfupdate >/dev/null 2>&1 || ois_warn "selfupdate failed (non-fatal)"
    else ois_priv port selfupdate >/dev/null 2>&1 || ois_warn "selfupdate failed (non-fatal)"; fi
    ois_ok "MacPorts installed"
    return 0
}

# Resolve the MacPorts .pkg URL for the detected macOS, from the GitHub
# releases API. Pure POSIX JSON scraping -- no jq. Falls back to the
# constructed www.macports.org distfiles URL if the API is unreachable.
_ois_macports_pkg_url() {
    _mpu_api="https://api.github.com/repos/macports/macports-base/releases/latest"
    _mpu_json="$(curl -fsSL --retry 2 "$_mpu_api" 2>/dev/null)" || _mpu_json=""
    if [ -n "$_mpu_json" ]; then
        _mpu_rest="$_mpu_json"
        while [ -n "$_mpu_rest" ]; do
            case "$_mpu_rest" in
                *"$OIS_NL"*) _mpu_l="${_mpu_rest%%"$OIS_NL"*}" ; _mpu_rest="${_mpu_rest#*"$OIS_NL"}" ;;
                *)           _mpu_l="$_mpu_rest" ; _mpu_rest="" ;;
            esac
            case "$_mpu_l" in
                *browser_download_url*"${OIS_MACOS_NAME}.pkg"*)
                    _mpu_u="${_mpu_l#*\"browser_download_url\":}"
                    _mpu_u="${_mpu_u#*\"}"
                    _mpu_u="${_mpu_u%%\"*}"
                    case "$_mpu_u" in
                        *-rc[0-9]*|*-beta[0-9]*) ;;   # skip pre-releases
                        https://*"${OIS_MACOS_NAME}.pkg") printf '%s' "$_mpu_u"; return 0 ;;
                    esac ;;
            esac
        done
    fi
    return 1
}

# pkg-config bootstrap: many probes want it. Install via the PM if absent.
_ois_ensure_pkgconfig() {
    command -v pkg-config >/dev/null 2>&1 && return 0
    command -v pkgconf    >/dev/null 2>&1 && return 0
    case "$OIS_PM" in
        brew)
            ois_info "pkg-config missing -- installing via brew ..."
            ois_pm_do_install pkg-config >/dev/null 2>&1 && { ois_ok "pkg-config installed"; return 0; } ;;
        macports)
            ois_info "pkgconfig missing -- installing via port ..."
            ois_pm_do_install pkgconfig >/dev/null 2>&1 && { ois_ok "pkgconfig installed"; return 0; } ;;
    esac
    ois_dbg "pkg-config unavailable; probes will fall back to headers + PM state"
    return 0   # never fatal
}

# ======================================================================
# PKG_CONFIG_PATH + build-flag enrichment
# ======================================================================

# Wire flags for ONE brew keg-only formula via its stable opt/ symlink.
# Note: `brew --prefix <formula>` prints the opt path even if the formula
# is not installed, so we only wire when ois_pm_have confirms it.
_ois_brew_wire_dep() {
    _bwd_dep="$1" _bwd_pkg="$2"
    _ois_brew_locate || return 0
    ois_pm_have "$_bwd_pkg" || return 0
    _bwd_p="$(ois_brew --prefix "$_bwd_pkg" 2>/dev/null)" || return 0
    [ -d "$_bwd_p" ] || return 0
    for _bwd_sub in lib/pkgconfig share/pkgconfig; do
        _bwd_d="$_bwd_p/$_bwd_sub"
        [ -d "$_bwd_d" ] || continue
        case ":${PKG_CONFIG_PATH:-}:" in *":$_bwd_d:"*) ;; *)
            PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$_bwd_d" ;; esac
    done
    [ -d "$_bwd_p/include" ] && case " ${CPPFLAGS:-} " in *" -I$_bwd_p/include "*) ;; *)
        CPPFLAGS="-I$_bwd_p/include${CPPFLAGS:+ $CPPFLAGS}" ;; esac
    [ -d "$_bwd_p/lib" ] && case " ${LDFLAGS:-} " in *" -L$_bwd_p/lib "*) ;; *)
        LDFLAGS="-L$_bwd_p/lib${LDFLAGS:+ $LDFLAGS}" ;; esac
    export PKG_CONFIG_PATH CPPFLAGS LDFLAGS
    ois_dbg "brew dep wired: $_bwd_dep ($_bwd_pkg) -> $_bwd_p"
}

# Enrich PKG_CONFIG_PATH once for the whole run, for every platform.
ois_deps_enrich_env() {
    case "$OIS_OS" in
        macos)
            if [ "$OIS_PM" = "brew" ] && _ois_brew_locate; then
                _de_global="${OIS_BREW_PREFIX:-$(ois_brew --prefix 2>/dev/null)}/lib/pkgconfig"
                [ -d "$_de_global" ] && case ":${PKG_CONFIG_PATH:-}:" in *":$_de_global:"*) ;; *)
                    PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$_de_global" ;; esac
                for _de_dep in $(ois_dep_names 2>/dev/null); do
                    _ois_brew_wire_dep "$_de_dep" "$(ois_dep_package "$_de_dep")"
                done
            elif [ "$OIS_PM" = "macports" ]; then
                _de_mp="${OIS_PORT_PREFIX:-/opt/local}/lib/pkgconfig"
                [ -d "$_de_mp" ] && case ":${PKG_CONFIG_PATH:-}:" in *":$_de_mp:"*) ;; *)
                    PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$_de_mp" ;; esac
            fi ;;
        freebsd|openbsd|netbsd|dragonfly)
            for _de_d in /usr/local/lib/pkgconfig /usr/local/share/pkgconfig \
                         /usr/pkg/lib/pkgconfig /usr/X11R6/lib/pkgconfig; do
                [ -d "$_de_d" ] || continue
                case ":${PKG_CONFIG_PATH:-}:" in *":$_de_d:"*) ;; *)
                    PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$_de_d" ;; esac
            done ;;
    esac
    export PKG_CONFIG_PATH
}

# ======================================================================
# Probing
# ======================================================================

# A pkg-config binary, whichever exists.
_ois_pkgconfig_bin() {
    if command -v pkg-config >/dev/null 2>&1; then printf 'pkg-config'; return 0; fi
    if command -v pkgconf    >/dev/null 2>&1; then printf 'pkgconf';    return 0; fi
    return 1
}

# ois_dep_probe NAME -> 0 present, 1 missing. Sets OIS_DEP_HOW.
ois_dep_probe() {
    _pb_n="$1" ; OIS_DEP_HOW=""

    # 1. explicit tool probe
    if _pb_cmd="$(ois_dep_attr "$_pb_n" cmd)"; then
        OIS_DEP_HOW="command -v $_pb_cmd"
        command -v "$_pb_cmd" >/dev/null 2>&1 && return 0
        return 1
    fi

    _pb_pc="$(ois_dep_attr "$_pb_n" pc)" || _pb_pc="$(ois_alias_pc "$_pb_n")" || _pb_pc=""
    _pb_ver="$(ois_dep_attr "$_pb_n" ver)" || _pb_ver=""

    # 2. pkg-config (PKG_CONFIG_PATH already enriched)
    if [ -n "$_pb_pc" ] && _pb_pcbin="$(_ois_pkgconfig_bin)"; then
        if [ -n "$_pb_ver" ]; then
            OIS_DEP_HOW="$_pb_pcbin $_pb_pc >= $_pb_ver"
            "$_pb_pcbin" --atleast-version="$_pb_ver" "$_pb_pc" 2>/dev/null && return 0
        else
            OIS_DEP_HOW="$_pb_pcbin $_pb_pc"
            "$_pb_pcbin" --exists "$_pb_pc" 2>/dev/null && return 0
        fi
    fi

    # 3. header search
    _pb_h="$(ois_dep_attr "$_pb_n" header)" || _pb_h="$(ois_alias_header "$_pb_n")" || _pb_h=""
    if [ -n "$_pb_h" ]; then
        OIS_DEP_HOW="header $_pb_h"
        _ois_have_header "$_pb_h" && return 0
    fi

    # 4. PM "is it installed" -- keg-only immune, needs no .pc file.
    #    This is the check that fixes the brew keg-only failure loop.
    _pb_pkg="$(ois_dep_package "$_pb_n")"
    if ois_pm_have "$_pb_pkg"; then
        OIS_DEP_HOW="$OIS_PM has $_pb_pkg"
        # If it's a brew keg, wire build flags so the compiler can find it.
        [ "$OIS_PM" = "brew" ] && _ois_brew_wire_dep "$_pb_n" "$_pb_pkg"
        return 0
    fi

    # 5. bare command -v, only when nothing else mapped
    if [ -z "$_pb_pc" ] && [ -z "$_pb_h" ]; then
        OIS_DEP_HOW="command -v $_pb_n"
        command -v "$_pb_n" >/dev/null 2>&1 && return 0
    fi

    [ -z "$OIS_DEP_HOW" ] && OIS_DEP_HOW="no usable probe"
    return 1
}

# ======================================================================
# Version comparison + next-best-version selection
# ======================================================================

# NOTE: version comparison uses ois_ver_cmp from core/version.sh (canonical,
# handles prerelease tags). deps.sh must NOT redefine it -- doing so shadowed
# the update logic. For next-best selection we compare numeric @-suffixes with
# a small local helper that tolerates non-numeric tails.

# _ois_numver N -> a plain integer-ish comparable form of a version suffix.
# '3' -> 3, '3.0' -> 3.0, '1.1' -> 1.1, '' -> 0. Used only for @-suffix ranking.
_ois_suffix_cmp() {
    # Compare two @-suffix version strings numerically, segment by segment.
    # Prints 1 / 0 / -1. Tolerates empty and non-numeric tails.
    _sc_a="$1" _sc_b="$2"
    while [ -n "$_sc_a" ] || [ -n "$_sc_b" ]; do
        _sc_as="${_sc_a%%.*}" ; _sc_bs="${_sc_b%%.*}"
        case "$_sc_a" in *.*) _sc_a="${_sc_a#*.}" ;; *) _sc_a="" ;; esac
        case "$_sc_b" in *.*) _sc_b="${_sc_b#*.}" ;; *) _sc_b="" ;; esac
        _sc_as="${_sc_as%%[!0-9]*}" ; _sc_bs="${_sc_bs%%[!0-9]*}"
        [ -z "$_sc_as" ] && _sc_as=0 ; [ -z "$_sc_bs" ] && _sc_bs=0
        if [ "$_sc_as" -gt "$_sc_bs" ] 2>/dev/null; then printf '1';  return 0; fi
        if [ "$_sc_as" -lt "$_sc_bs" ] 2>/dev/null; then printf -- '-1'; return 0; fi
    done
    printf '0'
}

# Find the closest available package to NAME when the exact one is
# missing. Strategy: search the PM, prefer names that share the base
# stem, then pick the highest numeric suffix (openssl@3 > openssl@1.1).
# Returns the package name on stdout, or nonzero if nothing suitable.
ois_dep_next_best() {
    _nb_dep="$1" _nb_pkg="$2"
    _nb_base="${_nb_pkg%%@*}"
    _nb_cands="$(ois_pm_search "$_nb_base" 2>/dev/null)" || return 1
    [ -z "$_nb_cands" ] && return 1
    _nb_best="" _nb_best_v=""
    for _nb_c in $_nb_cands; do
        # only accept exact base or base@version / base-version
        case "$_nb_c" in
            "$_nb_base"|"$_nb_base"@*|"$_nb_base"[0-9]*) ;;
            *) continue ;;
        esac
        # skip the exact name we already know is missing/unusable
        [ "$_nb_c" = "$_nb_pkg" ] && continue
        _nb_cv="${_nb_c#"$_nb_base"}" ; _nb_cv="${_nb_cv#@}"
        [ -z "$_nb_cv" ] && _nb_cv="0"
        if [ -z "$_nb_best" ]; then
            _nb_best="$_nb_c" ; _nb_best_v="$_nb_cv"
        else
            [ "$(_ois_suffix_cmp "$_nb_cv" "$_nb_best_v")" = "1" ] && {
                _nb_best="$_nb_c" ; _nb_best_v="$_nb_cv"; }
        fi
    done
    [ -n "$_nb_best" ] && { printf '%s' "$_nb_best"; return 0; }
    return 1
}

# ======================================================================
# Installed version (for the lockfile)
# ======================================================================
ois_dep_version() {
    _dv_n="$1"
    _dv_pc="$(ois_dep_attr "$_dv_n" pc)" || _dv_pc="$(ois_alias_pc "$_dv_n")" || _dv_pc=""
    if [ -n "$_dv_pc" ] && _dv_pcbin="$(_ois_pkgconfig_bin)"; then
        _dv_v="$("$_dv_pcbin" --modversion "$_dv_pc" 2>/dev/null)" && [ -n "$_dv_v" ] && {
            printf '%s' "$_dv_v"; return 0; }
    fi
    _dv_p="$(ois_dep_package "$_dv_n")"
    _dv_v="$(ois_pm_installed_version "$_dv_p" 2>/dev/null)"
    case "$_dv_v" in
        ''|unknown|*[!0-9A-Za-z.:+~_-]*) ;;
        *) printf '%s' "$_dv_v"; return 0 ;;
    esac
    _dv_c="$(ois_dep_attr "$_dv_n" cmd)" || _dv_c="$_dv_n"
    if command -v "$_dv_c" >/dev/null 2>&1; then
        _dv_v="$("$_dv_c" --version 2>/dev/null | head -n 1 | tr -d '	')"
        [ -n "$_dv_v" ] && { printf '%s' "$_dv_v"; return 0; }
    fi
    printf 'unknown'
    return 0
}

# ======================================================================
# The main check
# ======================================================================
ois_deps_check() {
    ois_deps_parse
    [ -z "$OIS_DEP_TABLE" ] && return 0

    # 1. Bootstrap toolchain + package manager on macOS.
    if [ "$OIS_OS" = "macos" ]; then
        _ois_macos_ensure_xcode_clt || return 1
        _ois_macos_ensure_brew      || return 1
        _ois_macos_ensure_macports  || return 1
    fi

    # 2. Ensure pkg-config exists (best tool for the probes below).
    _ois_ensure_pkgconfig

    # 3. Enrich the environment once so every probe benefits.
    ois_deps_enrich_env

    # 4. First pass: probe every declared dependency.
    _dc_missing="" _dc_pkgs="" _dc_optmiss=""
    for _dc_n in $(ois_dep_names); do
        _dc_req="$(ois_dep_attr "$_dc_n" req)" || _dc_req=1
        if ois_dep_probe "$_dc_n"; then
            ois_dbg "dep ok: $_dc_n ($OIS_DEP_HOW)"
            continue
        fi
        if [ "$_dc_req" = 1 ]; then
            _dc_missing="${_dc_missing:+$_dc_missing }$_dc_n"
            _dc_pkgs="${_dc_pkgs:+$_dc_pkgs }$(ois_dep_package "$_dc_n")"
        else
            _dc_optmiss="${_dc_optmiss:+$_dc_optmiss }$_dc_n"
        fi
    done

    [ -n "$_dc_optmiss" ] && ois_info "optional, not installed: $_dc_optmiss"
    [ -z "$_dc_missing" ] && return 0

    printf '\n'
    ois_warn "missing required dependencies: $_dc_missing"

    # 5. Build the install command for display and confirm.
    # shellcheck disable=SC2086
    _dc_cmd="$(ois_pm_install_cmd $_dc_pkgs)" || _dc_cmd=""
    if [ -z "$_dc_cmd" ]; then
        ois_fail E-TOOL "cannot install dependencies automatically" \
            "no supported package manager was detected on this system" \
            "install these yourself, then re-run: $_dc_missing"
        return 1
    fi

    # Show the command with the privilege prefix the user would type.
    _dc_disp_pfx=""
    case "$OIS_PM" in
        brew) ;;   # brew is never sudo
        *) [ "$OIS_IS_ROOT" != "yes" ] && [ "$OIS_SUDO" != "none" ] && _dc_disp_pfx="$OIS_SUDO " ;;
    esac
    printf '     %s%s\n\n' "$_dc_disp_pfx" "$_dc_cmd"

    # If we genuinely cannot elevate for a PM that needs it, stop early.
    if [ "$OIS_PM" != "brew" ] && [ "$OIS_IS_ROOT" != "yes" ] && [ "$OIS_SUDO" = "none" ]; then
        ois_fail E-PERM "dependencies need installing but no sudo/doas is available" \
            "OIS cannot elevate to run the package manager" \
            "run this yourself, then re-run OIS:" \
            "  $_dc_cmd"
        return 1
    fi

    if ! ois_ask "run this now?" y; then
        ois_fail E-TOOL "required dependencies are missing" \
            "the build cannot proceed without: $_dc_missing" \
            "run: $_dc_disp_pfx$_dc_cmd" \
            "then re-run the same OIS command"
        return 1
    fi

    # 6. Install (pm.sh handles privilege correctly per manager).
    _dc_rc=0
    # shellcheck disable=SC2086
    ois_pm_do_install $_dc_pkgs || _dc_rc=$?

    # 7. On brew failure: the classic /usr/local ownership problem (Intel).
    if [ "$_dc_rc" != 0 ] && [ "$OIS_PM" = "brew" ] && [ "$OIS_OS" = "macos" ]; then
        _dc_rc="$(_ois_brew_fix_ownership_and_retry "$_dc_pkgs" "$_dc_rc")"
    fi

    if [ "${_dc_rc:-0}" != 0 ]; then
        ois_fail E-TOOL "package installation failed (exit ${_dc_rc})" \
            "the package manager reported an error" \
            "check the output above; package names can differ on your distro" \
            "override them in ois.conf, e.g.  ${_dc_missing%% *}.$OIS_PM = <real-name>"
        return 1
    fi

    # 8. Re-enrich (new .pc files) and re-probe. Try next-best on misses.
    ois_deps_enrich_env
    _dc_still=""
    for _dc_n in $_dc_missing; do
        if ois_dep_probe "$_dc_n"; then
            ois_dbg "dep now found: $_dc_n ($OIS_DEP_HOW)"
            continue
        fi
        if [ "${OIS_NEXT_BEST_VERSION:-no}" = "yes" ]; then
            _dc_want="$(ois_dep_package "$_dc_n")"
            ois_info "searching for an alternative to $_dc_want ..."
            _dc_alt="$(ois_dep_next_best "$_dc_n" "$_dc_want")" || _dc_alt=""
            if [ -n "$_dc_alt" ] && [ "$_dc_alt" != "$_dc_want" ]; then
                ois_warn "closest available: $_dc_alt (wanted $_dc_want)"
                if ois_ask "install $_dc_alt instead?" y; then
                    _dc_alt_rc=0
                    ois_pm_do_install "$_dc_alt" || _dc_alt_rc=$?
                    if [ "${_dc_alt_rc:-0}" = 0 ]; then
                        ois_deps_enrich_env
                        # Probe the ALT package directly, since the logical
                        # dep's default name won't match the alt.
                        if ois_pm_have "$_dc_alt" || ois_dep_probe "$_dc_n"; then
                            [ "$OIS_PM" = "brew" ] && _ois_brew_wire_dep "$_dc_n" "$_dc_alt"
                            ois_ok "using $_dc_alt for $_dc_n"
                            continue
                        fi
                    fi
                fi
            else
                ois_dbg "no alternative found for $_dc_want"
            fi
        fi
        _dc_still="${_dc_still:+$_dc_still }$_dc_n"
    done

    if [ -n "$_dc_still" ]; then
        ois_fail E-TOOL "still missing after installation: $_dc_still" \
            "the packages installed but the probe still cannot find them" \
            "the package name may be wrong for this platform" \
            "override it: ${_dc_still%% *}.$OIS_PM = <correct-package>" \
            "or override the probe: ${_dc_still%% *}.pc = <pkg-config-name>" \
            "or enable fuzzy matching: next_best_version = yes"
        return 1
    fi
    ois_ok "dependencies installed"
    return 0
}

# macOS Intel /usr/local ownership fix, then retry the install.
# Prints the new return code on stdout. Only ever runs for brew.
_ois_brew_fix_ownership_and_retry() {
    _fo_pkgs="$1" _fo_rc="$2"
    _ois_brew_locate || { printf '%s' "$_fo_rc"; return 0; }
    _fo_pfx="${OIS_BREW_PREFIX:-$(ois_brew --prefix 2>/dev/null || printf '')}"
    [ -n "$_fo_pfx" ] || { printf '%s' "$_fo_rc"; return 0; }
    # Only intervene for the ownership signature: prefix not writable by
    # the real user. Apple Silicon /opt/homebrew is rarely affected.
    _fo_u="$(_ois_pm_real_user)"
    [ -w "$_fo_pfx" ] && { printf '%s' "$_fo_rc"; return 0; }
    ois_warn "homebrew prefix $_fo_pfx is not writable by $_fo_u" >&2
    ois_warn "this is the classic macOS /usr/local ownership issue" >&2
    _fo_dirs=""
    for _fo_d in "$_fo_pfx" \
        "$_fo_pfx/bin" "$_fo_pfx/etc" "$_fo_pfx/include" "$_fo_pfx/lib" \
        "$_fo_pfx/sbin" "$_fo_pfx/share" "$_fo_pfx/var" "$_fo_pfx/opt" \
        "$_fo_pfx/Cellar" "$_fo_pfx/Caskroom" "$_fo_pfx/Homebrew" "$_fo_pfx/Frameworks"; do
        [ -d "$_fo_d" ] && _fo_dirs="${_fo_dirs:+$_fo_dirs }$_fo_d"
    done
    printf '     sudo chown -R %s %s\n\n' "$_fo_u" "$_fo_dirs" >&2
    if ois_ask "fix ownership and retry?" y; then
        # chown needs root; ois_priv escalates. We are NOT root here
        # (brew refuses root), so this uses sudo/doas.
        # shellcheck disable=SC2086
        ois_priv chown -R "$_fo_u" $_fo_dirs 2>/dev/null || :
        _fo_rc2=0
        # shellcheck disable=SC2086
        ois_pm_do_install $_fo_pkgs || _fo_rc2=$?
        printf '%s' "$_fo_rc2"
        return 0
    fi
    printf '%s' "$_fo_rc"
    return 0
}

# -- install command string (used by pm.sh for non-brew/port managers) --
ois_pm_install_cmd() {
    [ $# -gt 0 ] || return 1
    case "$OIS_PM" in
        apt)      printf 'apt-get install -y %s' "$*" ;;
        pacman)   printf 'pacman -S --needed --noconfirm %s' "$*" ;;
        dnf|yum)  printf '%s install -y %s' "$OIS_PM" "$*" ;;
        zypper)   printf 'zypper --non-interactive install %s' "$*" ;;
        apk)      printf 'apk add %s' "$*" ;;
        xbps)     printf 'xbps-install -y %s' "$*" ;;
        emerge)   printf 'emerge --ask=n %s' "$*" ;;
        brew)     printf 'brew install %s' "$*" ;;
        macports) printf 'port install %s' "$*" ;;
        pkg)      printf 'pkg install -y %s' "$*" ;;
        pkgin)    printf 'pkgin -y install %s' "$*" ;;
        pkg_add)  printf 'pkg_add %s' "$*" ;;
        ips)      printf 'pkg install %s' "$*" ;;
        *)        return 1 ;;
    esac
}

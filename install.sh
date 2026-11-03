#!/bin/sh
# ytcui installer.
#
#     git clone https://github.com/MilkmanAbi/ytcui && cd ytcui && ./install.sh
#
# A short, friendly Q&A up front, then it hands the actual build/install to
# OIS (ois/ois.sh). Non-interactive callers (CI, --yes, no TTY) skip every
# prompt and take the defaults, so this stays scriptable.
#
# Flags are forwarded to OIS untouched:
#   --user | --system | --prefix DIR | --yes | --verbose | --json
set -eu

SELF="$(cd "$(dirname "$0")" && pwd)"
[ -f "$SELF/ois/ois.sh" ] || {
    printf 'error: %s/ois/ois.sh is missing.\n' "$SELF" >&2
    printf 'The ois/ directory must sit beside this script.\n' >&2
    exit 1
}

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    B="$(printf '\033[1m')"; DIM="$(printf '\033[2m')"; R="$(printf '\033[0m')"
    CY="$(printf '\033[36m')"; GR="$(printf '\033[32m')"; YE="$(printf '\033[33m')"
else
    B='' ; DIM='' ; R='' ; CY='' ; GR='' ; YE=''
fi

say()   { printf '%s\n' "$*"; }
head_() { printf '\n%s%s%s\n' "$B" "$*" "$R"; }

INTERACTIVE=1
for a in "$@"; do
    case "$a" in --yes|-y|--json|--noninteractive) INTERACTIVE=0 ;; esac
done
[ -t 0 ] || INTERACTIVE=0

ask() {
    _q="$1"; shift
    _n=$#
    printf '\n%s%s%s\n' "$B" "$_q" "$R" >&2
    _i=1
    for _o in "$@"; do
        if [ "$_i" = 1 ]; then printf '  %s%s)%s %s %s(default)%s\n' "$CY" "$_i" "$R" "$_o" "$DIM" "$R" >&2
        else printf '  %s%s)%s %s\n' "$CY" "$_i" "$R" "$_o" >&2; fi
        _i=$(( _i + 1 ))
    done
    while :; do
        printf '%s> %s' "$GR" "$R" >&2
        if ! IFS= read -r _ans; then _ans=""; fi
        [ -z "$_ans" ] && { echo 1; return; }
        case "$_ans" in
            *[!0-9]*) ;;
            *) if [ "$_ans" -ge 1 ] && [ "$_ans" -le "$_n" ]; then echo "$_ans"; return; fi ;;
        esac
        printf '%s  enter a number 1-%s%s\n' "$YE" "$_n" "$R" >&2
    done
}

confirm() {
    _q="$1"; _def="${2:-y}"
    if [ "$_def" = y ]; then _hint="[Y/n]"; else _hint="[y/N]"; fi
    printf '\n%s%s%s %s ' "$B" "$_q" "$R" "$_hint" >&2
    if ! IFS= read -r _a; then _a=""; fi
    [ -z "$_a" ] && _a="$_def"
    case "$_a" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

say ""
say "${B}  ytcui installer${R}"
say "${DIM}  a terminal YouTube client${R}"

BACKEND="ytcuidl"
THEME="default"
NO_HA=0
PKG_MGR=""

OS="$(uname -s 2>/dev/null || echo unknown)"

if [ "$INTERACTIVE" = 0 ]; then
    say ""
    say "${DIM}  non-interactive: defaults (ytcui-dl backend, default theme, HW accel on)${R}"
else
    if [ "$OS" = "Darwin" ]; then
        c="$(ask "Which package manager should handle dependencies?" \
                 "Homebrew" "MacPorts")"
        if [ "$c" = 2 ]; then PKG_MGR="port"; else PKG_MGR="brew"; fi
        # OIS handles the rest: it bootstraps the Xcode CLT, installs the
        # chosen package manager if missing (Homebrew via the official
        # script as your user; MacPorts via the correct version-matched
        # .pkg), wires keg-only build flags, and adds ~/.local/bin to your
        # PATH. We just record the preference for OIS to read.
        export OIS_PKG_MANAGER="$PKG_MGR"
        if [ "$PKG_MGR" = brew ] && ! command -v brew >/dev/null 2>&1; then
            say "${DIM}  Homebrew not found -- OIS will offer to install it (as you, not root).${R}"
        elif [ "$PKG_MGR" = port ] && ! command -v port >/dev/null 2>&1; then
            say "${DIM}  MacPorts not found -- OIS will fetch the correct .pkg for your macOS.${R}"
        fi
    fi

    c="$(ask "Which video backend?" \
             "ytcui-dl  [built-in, no external deps, Experimental]" \
             "yt-dlp    [mature, needs yt-dlp installed]")"
    if [ "$c" = 2 ]; then BACKEND="ytdlp"; else BACKEND="ytcuidl"; fi

    say ""
    say "${B}Pick a colour theme${R}  ${DIM}(changeable anytime in-app with Ctrl-S)${R}"
    _tn="default grayscale nord dracula solarized monokai gruvbox tokyo pink green blue purple red amber ocean mint coral slate"
    printf '  %s1)%s default    %sbalanced, warm amber accents (default)%s\n'      "$CY" "$R" "$DIM" "$R"
    printf '  %s2)%s grayscale  %sno colour, clean monochrome%s\n'                "$CY" "$R" "$DIM" "$R"
    printf '  %s3)%s nord       %scool arctic blues and frost%s\n'                "$CY" "$R" "$DIM" "$R"
    printf '  %s4)%s dracula    %sdark violet with vivid pink%s\n'                "$CY" "$R" "$DIM" "$R"
    printf '  %s5)%s solarized  %smuted, low-contrast, easy on the eyes%s\n'      "$CY" "$R" "$DIM" "$R"
    printf '  %s6)%s monokai    %sclassic editor greens and magentas%s\n'         "$CY" "$R" "$DIM" "$R"
    printf '  %s7)%s gruvbox    %sretro earthy orange and green%s\n'              "$CY" "$R" "$DIM" "$R"
    printf '  %s8)%s tokyo      %stokyo-night deep blue and purple%s\n'           "$CY" "$R" "$DIM" "$R"
    printf '  %s9)%s pink       %ssolid hot pink accents%s\n'                     "$CY" "$R" "$DIM" "$R"
    printf ' %s10)%s green      %ssolid terminal green%s\n'                       "$CY" "$R" "$DIM" "$R"
    printf ' %s11)%s blue       %ssolid electric blue%s\n'                        "$CY" "$R" "$DIM" "$R"
    printf ' %s12)%s purple     %ssolid royal purple%s\n'                         "$CY" "$R" "$DIM" "$R"
    printf ' %s13)%s red        %ssolid crimson%s\n'                              "$CY" "$R" "$DIM" "$R"
    printf ' %s14)%s amber      %ssolid warm amber%s\n'                           "$CY" "$R" "$DIM" "$R"
    printf ' %s15)%s ocean      %sdeep teal and cyan%s\n'                         "$CY" "$R" "$DIM" "$R"
    printf ' %s16)%s mint       %ssoft mint green%s\n'                            "$CY" "$R" "$DIM" "$R"
    printf ' %s17)%s coral      %swarm coral and salmon%s\n'                      "$CY" "$R" "$DIM" "$R"
    printf ' %s18)%s slate      %smuted blue-grey, understated%s\n'               "$CY" "$R" "$DIM" "$R"
    while :; do
        printf '%s> %s' "$GR" "$R"
        if ! IFS= read -r tsel; then tsel=""; fi
        [ -z "$tsel" ] && { THEME="default"; break; }
        case "$tsel" in
            *[!0-9]*) ;;
            *) if [ "$tsel" -ge 1 ] && [ "$tsel" -le 18 ]; then
                   THEME="$(echo "$_tn" | cut -d' ' -f"$tsel")"; break
               fi ;;
        esac
        printf '%s  enter a number 1-18%s\n' "$YE" "$R"
    done

    if confirm "Enable mpv hardware (GPU) video acceleration?" y; then
        NO_HA=0
    else
        NO_HA=1
    fi

    head_ "Ready to install"
    say "  backend      ${CY}${BACKEND}${R}"
    say "  theme        ${CY}${THEME}${R}"
    say "  hw accel     ${CY}$([ "$NO_HA" = 1 ] && echo off || echo on)${R}"
    [ -n "$PKG_MGR" ] && say "  pkg manager  ${CY}${PKG_MGR}${R}"
    confirm "Proceed?" y || { say "${YE}cancelled.${R}"; exit 0; }
fi

# Backend -> Makefile include (survives OIS's sudo re-exec for --system).
printf 'BACKEND = %s\n' "$BACKEND" > "$SELF/.ytcui-build.conf"

# Theme + HW accel -> config.json seed (only if absent; never clobber).
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/ytcui"
CFG="$CFG_DIR/config.json"
if [ ! -f "$CFG" ]; then
    mkdir -p "$CFG_DIR"
    {
        printf '{\n'
        printf '  "theme": "%s",\n' "$THEME"
        printf '  "no_hardware_accel": %s\n' "$([ "$NO_HA" = 1 ] && echo true || echo false)"
        printf '}\n'
    } > "$CFG"
    say "${DIM}  wrote $CFG${R}"
else
    if [ "$INTERACTIVE" = 1 ] && command -v sed >/dev/null 2>&1; then
        if grep -q '"theme"' "$CFG" 2>/dev/null; then
            tmp="$CFG.ois-tmp.$$"
            sed "s/\"theme\"[[:space:]]*:[[:space:]]*\"[^\"]*\"/\"theme\": \"$THEME\"/" "$CFG" > "$tmp" \
                && mv "$tmp" "$CFG" && say "${DIM}  updated theme in existing $CFG${R}"
        fi
    fi
fi

say ""
exec sh "$SELF/ois/ois.sh" install "$@"

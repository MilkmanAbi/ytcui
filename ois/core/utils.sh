#!/bin/sh
# OIS v2 -- core/utils.sh
# Output, privilege, atomic file operations, lexical path normalisation.
# Source first. Everything depends on this.
#
# Rules enforced here:
#   - no variables in printf format strings (SC2059)
#   - every file write is tmp-on-same-fs + rename(2)
#   - every prompt reads /dev/tty so `curl ... | sh` works
#   - privilege goes through ois_priv, never a hardcoded `sudo`
# ---------------------------------------------------------------------

# -- Character constants ----------------------------------------------
# $(printf '\n') is USELESS in a case pattern: command substitution
# strips trailing newlines, so the pattern collapses to the empty string
# and matches everything. Build the newline by appending a sentinel and
# removing it. This bit us once already -- do not "simplify" it.
OIS_TAB="$(printf '\t')"
OIS_NL="$(printf '\nx')" ; OIS_NL="${OIS_NL%x}"

# -- Colour ------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then
    C_B=$(printf '\033[1m')  ; C_D=$(printf '\033[2m')
    C_R=$(printf '\033[0m')  ; C_G=$(printf '\033[32m')
    C_Y=$(printf '\033[33m') ; C_C=$(printf '\033[36m')
    C_E=$(printf '\033[31m')
else
    C_B='' C_D='' C_R='' C_G='' C_Y='' C_C='' C_E=''
fi

OIS_QUIET="${OIS_QUIET:-0}"
OIS_VERBOSE="${OIS_VERBOSE:-0}"

ois_ok()   { [ "$OIS_QUIET" = 1 ] || printf '  %s+%s  %s\n' "$C_G" "$C_R" "$*"; }
ois_info() { [ "$OIS_QUIET" = 1 ] || printf '  %s>%s  %s\n' "$C_C" "$C_R" "$*"; }
ois_warn() { printf '  %s!%s  %s\n' "$C_Y" "$C_R" "$*" >&2; }
ois_err()  { printf '  %sx%s  %s\n' "$C_E" "$C_R" "$*" >&2; }
ois_die()  { ois_err "$*"; exit 1; }
ois_dbg()  { [ "$OIS_VERBOSE" = 1 ] && printf '  %s..%s %s\n' "$C_D" "$C_R" "$*" >&2; return 0; }

ois_hdr() {
    [ "$OIS_QUIET" = 1 ] && return 0
    printf '%s%s%s\n' "$C_B$C_C" \
        '--------------------------------------------------' "$C_R"
    printf '%s  %s%s\n' "$C_B$C_C" "$1" "$C_R"
    [ -n "${2:-}" ] && printf '%s  %s%s\n' "$C_D" "$2" "$C_R"
    printf '%s%s%s\n\n' "$C_B$C_C" \
        '--------------------------------------------------' "$C_R"
}

# -- Privilege ---------------------------------------------------------
# Real implementation is installed by system.sh once OIS_SUDO is known.
# This stub keeps utils usable when sourced standalone (tests).
ois_priv() { "$@"; }

# -- Prompting ---------------------------------------------------------
# Always reads the terminal, never stdin, so piping the installer works.
# Honours OIS_ASSUME_YES for full non-interactivity (v1 --yes only
# skipped the *first* prompt and then hung).
ois_ask() {
    _ask_q="$1" ; _ask_def="${2:-n}"
    if [ "${OIS_ASSUME_YES:-0}" = "1" ]; then
        # --yes accepts the DEFAULT, it does not blanket-answer "yes".
        # A destructive prompt that defaults to "n" (e.g. "also delete
        # config and saved data?") must stay "n" under --yes, or --yes
        # becomes a data-loss footgun. Non-destructive prompts that need
        # a yes already default to "y", so they proceed as intended.
        [ "$_ask_def" = "y" ] && return 0
        return 1
    fi
    if [ ! -r /dev/tty ]; then
        ois_dbg "no tty; assuming default '$_ask_def' for: $_ask_q"
        [ "$_ask_def" = "y" ] && return 0
        return 1
    fi
    if [ "$_ask_def" = "y" ]; then _ask_h="[Y/n]"; else _ask_h="[y/N]"; fi
    printf '  %s %s ' "$_ask_q" "$_ask_h" > /dev/tty
    read -r _ask_a < /dev/tty || _ask_a=""
    [ -z "$_ask_a" ] && _ask_a="$_ask_def"
    case "$_ask_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# -- Temp files --------------------------------------------------------
# mktemp is not in POSIX. Fall back to a pid+counter name in TMPDIR.
_ois_tmp_seq=0
ois_tmpfile() {
    if command -v mktemp >/dev/null 2>&1; then
        mktemp 2>/dev/null && return 0
    fi
    _ois_tmp_seq=$(( _ois_tmp_seq + 1 ))
    _tf="${TMPDIR:-/tmp}/ois.$$.$_ois_tmp_seq"
    ( set -C; : > "$_tf" ) 2>/dev/null || return 1
    printf '%s' "$_tf"
}

ois_tmpdir() {
    if command -v mktemp >/dev/null 2>&1; then
        mktemp -d 2>/dev/null && return 0
    fi
    _ois_tmp_seq=$(( _ois_tmp_seq + 1 ))
    _td="${TMPDIR:-/tmp}/ois.d.$$.$_ois_tmp_seq"
    mkdir "$_td" 2>/dev/null || return 1
    printf '%s' "$_td"
}

# -- Directories -------------------------------------------------------
ois_mkdir() {
    [ -d "$1" ] && return 0
    mkdir -p "$1" 2>/dev/null && return 0
    ois_priv mkdir -p "$1" 2>/dev/null && return 0
    ois_err "cannot create directory: $1"; return 1
}

# -- Atomic write ------------------------------------------------------
# Content on stdin -> $1. Staging file lives in the SAME directory as the
# destination so rename(2) cannot fail with EXDEV. Readable by all
# (0644) so system-scope state is queryable by unprivileged users.
ois_write_atomic() {
    _wa_dest="$1" ; _wa_mode="${2:-644}"
    _wa_dir="${_wa_dest%/*}" ; [ "$_wa_dir" = "$_wa_dest" ] && _wa_dir="."
    ois_mkdir "$_wa_dir" || return 1
    _wa_tmp="$_wa_dir/.ois-tmp.$$"

    if cat > "$_wa_tmp" 2>/dev/null; then
        chmod "$_wa_mode" "$_wa_tmp" 2>/dev/null || :
        mv -f "$_wa_tmp" "$_wa_dest" 2>/dev/null && return 0
        rm -f "$_wa_tmp" 2>/dev/null || :
    fi

    # Unprivileged staging failed: stage in TMPDIR, move with privilege.
    _wa_stg="$(ois_tmpfile)" || { ois_err "cannot stage write: $_wa_dest"; return 1; }
    cat > "$_wa_stg" || { rm -f "$_wa_stg"; return 1; }
    chmod "$_wa_mode" "$_wa_stg" 2>/dev/null || :
    if ois_priv cp "$_wa_stg" "$_wa_tmp" 2>/dev/null &&
       ois_priv mv -f "$_wa_tmp" "$_wa_dest" 2>/dev/null; then
        rm -f "$_wa_stg" 2>/dev/null || :
        return 0
    fi
    rm -f "$_wa_stg" 2>/dev/null || :
    ois_priv rm -f "$_wa_tmp" 2>/dev/null || :
    ois_err "cannot write: $_wa_dest"
    return 1
}

# -- Atomic install of an executable -----------------------------------
# `cp` onto a running binary fails with ETXTBSY ("Text file busy").
# rename(2) over it always succeeds: the old inode stays alive for
# processes that still have it open. This is the ONLY safe way to
# self-update, and the path taken when an app shells out to OIS.
ois_install_file() {
    _if_src="$1" ; _if_dest="$2" ; _if_mode="${3:-755}"
    [ -f "$_if_src" ] || { ois_err "source missing: $_if_src"; return 1; }
    _if_dir="${_if_dest%/*}" ; [ "$_if_dir" = "$_if_dest" ] && _if_dir="."
    ois_mkdir "$_if_dir" || return 1
    _if_tmp="$_if_dir/.ois-new.$$"

    if cp "$_if_src" "$_if_tmp" 2>/dev/null; then
        chmod "$_if_mode" "$_if_tmp" 2>/dev/null || :
        mv -f "$_if_tmp" "$_if_dest" 2>/dev/null && return 0
        rm -f "$_if_tmp" 2>/dev/null || :
    fi

    if ois_priv cp "$_if_src" "$_if_tmp" 2>/dev/null; then
        ois_priv chmod "$_if_mode" "$_if_tmp" 2>/dev/null || :
        ois_priv mv -f "$_if_tmp" "$_if_dest" 2>/dev/null && return 0
        ois_priv rm -f "$_if_tmp" 2>/dev/null || :
    fi
    ois_err "cannot install: $_if_dest"
    return 1
}

# -- Removal -----------------------------------------------------------
# rm -rf does not descend through symlinks, so a symlinked directory in
# a manifest removes the link and never the target.
ois_rm()    { rm -f  "$1" 2>/dev/null || ois_priv rm -f  "$1" 2>/dev/null || return 1; }
ois_rmtree() { rm -rf "$1" 2>/dev/null || ois_priv rm -rf "$1" 2>/dev/null || return 1; }

# -- Lexical path normalisation ---------------------------------------
# realpath(1) and readlink -f are not POSIX. Normalise lexically:
# collapse '//' and '/./', resolve '..' textually, strip trailing '/'.
# Rejects relative paths. Because removal never dereferences symlinks,
# lexical normalisation plus a prefix allowlist is a sound boundary.
ois_path_norm() {
    _pn_in="$1"
    case "$_pn_in" in /*) ;; *) return 1 ;; esac
    _pn_out=""
    _pn_rest="$_pn_in"
    while [ -n "$_pn_rest" ]; do
        _pn_rest="${_pn_rest#/}"
        case "$_pn_rest" in
            */*) _pn_seg="${_pn_rest%%/*}" ; _pn_rest="/${_pn_rest#*/}" ;;
            *)   _pn_seg="$_pn_rest"       ; _pn_rest="" ;;
        esac
        case "$_pn_seg" in
            ''|.) ;;
            ..)   _pn_out="${_pn_out%/*}" ;;
            *)    _pn_out="$_pn_out/$_pn_seg" ;;
        esac
    done
    printf '%s' "${_pn_out:-/}"
}

# True if $1 is equal to, or lies underneath, $2. Both must be normalised.
ois_path_under() {
    [ "$1" = "$2" ] && return 0
    case "$1" in "${2%/}/"*) return 0 ;; esac
    return 1
}

# -- Hashing -----------------------------------------------------------
# Covers Linux, macOS, and the BSDs. Absence is not fatal; hashes are
# recorded as '-' and `ois verify` reports them as unverifiable.
OIS_SHA_CMD=""
ois_sha256_init() {
    [ -n "$OIS_SHA_CMD" ] && return 0
    if   command -v sha256sum >/dev/null 2>&1; then OIS_SHA_CMD="sha256sum"
    elif command -v shasum    >/dev/null 2>&1; then OIS_SHA_CMD="shasum -a 256"
    elif command -v sha256    >/dev/null 2>&1; then OIS_SHA_CMD="sha256 -q"
    elif command -v openssl   >/dev/null 2>&1; then OIS_SHA_CMD="openssl-dgst"
    fi
}

ois_sha256() {
    ois_sha256_init
    [ -f "$1" ] || { printf '%s' '-'; return 0; }
    case "$OIS_SHA_CMD" in
        '')            printf '%s' '-' ;;
        openssl-dgst)  openssl dgst -sha256 "$1" 2>/dev/null | ois_last_field ;;
        'sha256 -q')   sha256 -q "$1" 2>/dev/null ;;
        *)             $OIS_SHA_CMD "$1" 2>/dev/null | ois_first_field ;;
    esac
}

ois_first_field() { read -r _ff _ 2>/dev/null; printf '%s' "${_ff:--}"; }
ois_last_field()  { read -r _lf 2>/dev/null; printf '%s' "${_lf##* }"; }

# -- String helpers (no sed, no forks) --------------------------------
ois_trim() {
    _tr="$1"
    while : ; do
        case "$_tr" in
            ' '*|"$OIS_TAB"*) _tr="${_tr#?}" ;;
            *) break ;;
        esac
    done
    while : ; do
        case "$_tr" in
            *' '|*"$OIS_TAB") _tr="${_tr%?}" ;;
            *) break ;;
        esac
    done
    printf '%s' "$_tr"
}

# Identifier guard. Everything that could ever reach a variable name,
# a filename, or a grep pattern goes through this first. v1 fed these
# straight into eval, which was a root RCE.
ois_is_ident() {
    case "$1" in
        ''|*[!A-Za-z0-9_-]*) return 1 ;;
        *) return 0 ;;
    esac
}

# Filename guard: a valid executable name. Stricter than a path (no
# slashes, so it cannot escape the bin dir) but looser than an ident
# (allows '.' and '+' for names like "foo.sh" or "clang++"). Rejects
# spaces, shell metacharacters, glob characters, and leading dashes
# (which would look like an option to the tools that receive the name).
ois_is_fname() {
    case "$1" in
        ''|-*|*[!A-Za-z0-9._+-]*) return 1 ;;
        *) return 0 ;;
    esac
}

# `-nt` is a bashism: POSIX test(1) has no such operator. find -newer IS
# POSIX. If find is unavailable we decline to judge rather than guess.
ois_newer_than() {
    command -v find >/dev/null 2>&1 || return 0
    [ -n "$(find "$1" -newer "$2" 2>/dev/null | head -n 1)" ]
}

ois_cpu_count() {
    _cc="$(getconf _NPROCESSORS_ONLN 2>/dev/null)" && [ -n "$_cc" ] && \
        { printf '%s' "$_cc"; return 0; }
    _cc="$(sysctl -n hw.ncpu 2>/dev/null)" && [ -n "$_cc" ] && \
        { printf '%s' "$_cc"; return 0; }
    printf '1'
}

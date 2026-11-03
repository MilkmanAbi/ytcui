#!/bin/sh
# OIS v2 -- core/errors.sh
# Structured failure reporting, transient-retry, and a failure journal.
#
# Philosophy: an error message must answer three questions --
#   WHAT failed, WHY it failed, and WHAT TO DO next.
# A bare "Build failed." answers none of them. Every terminal failure
# goes through ois_fail with a stable code, a cause, and remedies, and
# is journalled to $OIS_ROOT/log so `ois doctor` can show history.
#
# Error codes (stable, grep-able, documented):
#   E-NET       network unreachable / timeout
#   E-HTTP      server answered with an error (404 asset, 429 limit...)
#   E-BUILD     compilation or build-system failure
#   E-TOOL      a required tool is missing on this machine
#   E-CONF      ois.conf is invalid
#   E-STORE     store corruption or permission problem
#   E-VERIFY    checksum mismatch
#   E-LOCK      could not acquire the store lock
#   E-PERM      insufficient privilege
#   E-STATE     operation invalid in current state (not installed, etc.)
#   E-HOOK      a lifecycle hook or service operation failed
#   E-MIGRATE   a data migration failed (triggers automatic rollback)
#   E-NIX       Nix-managed system; imperative install refused
# ---------------------------------------------------------------------

# ois_fail CODE "what" ["cause"] ["remedy"]...
# Prints the block, journals one line, returns 1 (callers decide to
# exit). Remedies render as '->' lines; pass as many as are useful.
ois_fail() {
    _ef_code="$1" ; _ef_what="$2" ; shift 2
    printf '\n  %sx  %s%s  %s\n' "$C_E" "$_ef_code" "$C_R" "$_ef_what" >&2
    if [ $# -gt 0 ] && [ -n "${1:-}" ]; then
        printf '     %scause:%s %s\n' "$C_D" "$C_R" "$1" >&2
        shift
    fi
    while [ $# -gt 0 ]; do
        [ -n "$1" ] && printf '     %s->%s %s\n' "$C_C" "$C_R" "$1" >&2
        shift
    done
    printf '\n' >&2
    _ois_journal "$_ef_code" "$_ef_what"
    return 1
}

ois_fail_die() { ois_fail "$@" || exit 1; }

_ois_journal() {
    _ej_dir="$(ois_store_root 2>/dev/null)" || return 0
    [ -d "$_ej_dir" ] || return 0
    _ej_f="$_ej_dir/log"
    _ej_line="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)	$1	$2"
    if [ -w "$_ej_f" ] 2>/dev/null || [ -w "$_ej_dir" ] 2>/dev/null; then
        printf '%s\n' "$_ej_line" >> "$_ej_f" 2>/dev/null || :
        # Cap the journal at ~200 lines without forking wc: count and
        # trim only when the file grows past ~32KB.
        if [ -f "$_ej_f" ]; then
            set -- "$_ej_f"
            _ej_sz="$(wc -c < "$_ej_f" 2>/dev/null)" || _ej_sz=0
            if [ "${_ej_sz:-0}" -gt 32768 ]; then
                tail -n 100 "$_ej_f" 2>/dev/null | ois_write_atomic "$_ej_f" 644 || :
            fi
        fi
    fi
}

# -- Retry -------------------------------------------------------------
# ois_retry ATTEMPTS BASE_DELAY cmd args...
# Runs cmd; on failure sleeps BASE_DELAY, 2x, 4x... and retries.
# Return code semantics honoured:
#   rc 22 (HTTP error from ois_fetch) is DEFINITIVE -- a 404 will not
#   become a 200 by asking again -- so it is never retried.
#   Everything else is treated as transient.
ois_retry() {
    _rt_max="$1" ; _rt_delay="$2" ; shift 2
    _rt_n=1
    while : ; do
        "$@" && return 0
        _rt_rc=$?
        [ "$_rt_rc" = 22 ] && return 22
        [ "$_rt_n" -ge "$_rt_max" ] && return "$_rt_rc"
        ois_dbg "attempt $_rt_n failed (rc=$_rt_rc); retrying in ${_rt_delay}s"
        sleep "$_rt_delay"
        _rt_delay=$(( _rt_delay * 2 ))
        _rt_n=$(( _rt_n + 1 ))
    done
}

# -- Command-output capture --------------------------------------------
# Runs a command with all output captured to a log file. On failure,
# prints the last N lines, the log path, and returns the command's rc.
# This is what turns "Build failed." into an actual diagnosis.
ois_run_logged() {
    _rl_log="$1" ; _rl_label="$2" ; shift 2
    ois_mkdir "${_rl_log%/*}" || return 1
    # Capture rc on the SAME line as the command. `if cmd; then` consumes
    # cmd's status, and a later `_rl_rc=$?` reads the `if`'s result (0),
    # not cmd's -- so a failing hook looked like a success.
    "$@" > "$_rl_log" 2>&1 ; _rl_rc=$?
    [ "$_rl_rc" = 0 ] && return 0
    printf '\n  %s--- last 15 lines of %s output ---%s\n' "$C_D" "$_rl_label" "$C_R" >&2
    tail -n 15 "$_rl_log" 2>/dev/null | while IFS= read -r _rl_l; do
        printf '  %s|%s %s\n' "$C_D" "$C_R" "$_rl_l" >&2
    done
    printf '  %s--- full log: %s ---%s\n' "$C_D" "$_rl_log" "$C_R" >&2
    return "$_rl_rc"
}

# -- Missing-tool diagnosis --------------------------------------------
# Not "cmake not found" but "cmake not found; install it with <your
# actual package manager>". OIS knows the PM; use that knowledge.
ois_need_tool() {
    _nt="$1"
    command -v "$_nt" >/dev/null 2>&1 && return 0
    # Consult the alias table first: the tool's command name and its
    # package name often differ (go -> golang, pkg-config -> pkgconf).
    _nt_pkg="$_nt"
    if command -v ois_alias_pkg >/dev/null 2>&1; then
        _nt_a="$(ois_alias_pkg "$_nt" 2>/dev/null)" && [ -n "$_nt_a" ] && _nt_pkg="$_nt_a"
    fi
    _nt_hint=""
    case "$OIS_PM" in
        apt)     _nt_hint="sudo apt-get install $_nt_pkg" ;;
        pacman)  _nt_hint="sudo pacman -S $_nt_pkg" ;;
        dnf|yum) _nt_hint="sudo $OIS_PM install $_nt_pkg" ;;
        zypper)  _nt_hint="sudo zypper install $_nt_pkg" ;;
        apk)     _nt_hint="sudo apk add $_nt_pkg" ;;
        xbps)    _nt_hint="sudo xbps-install $_nt_pkg" ;;
        emerge)  _nt_hint="sudo emerge $_nt_pkg" ;;
        brew)    _nt_hint="brew install $_nt_pkg" ;;
        macports)_nt_hint="sudo port install $_nt_pkg" ;;
        pkg)     _nt_hint="sudo pkg install $_nt_pkg" ;;
        pkgin)   _nt_hint="sudo pkgin install $_nt_pkg" ;;
        pkg_add) _nt_hint="doas pkg_add $_nt_pkg" ;;
    esac
    ois_fail E-TOOL "'$_nt' is required but not installed" \
        "this build needs $_nt and it is not in PATH" \
        "${_nt_hint:+install it: $_nt_hint}" \
        "then re-run the same command"
}

# -- Interrupted-operation journal (crash safety) ----------------------
# meta 'state' walks installing -> ok. A crash (power loss, kill -9)
# leaves state=installing; `ois doctor` detects it and `ois install`
# offers a clean redo instead of a confusing half-state.
ois_state_begin() { ois_meta_set "$1" state "$2"; }
ois_state_ok()    { ois_meta_set "$1" state ok; }
ois_state_get()   { ois_meta_get "$1" state 2>/dev/null || printf 'unknown'; }

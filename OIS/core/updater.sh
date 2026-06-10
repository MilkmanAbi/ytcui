#!/bin/sh
# OIS core/updater.sh
# ────────────────────

ois_fetch_version() {
    _url="$1"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --max-time 8 "${_url}?$(date +%s)" 2>/dev/null | tr -d '[:space:]'
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- --timeout=8 "$_url" 2>/dev/null | tr -d '[:space:]'
    else
        ois_warn "curl and wget not found — cannot check for updates."
        return 1
    fi
}

# Returns 0 if A is strictly older than B
ois_version_older() {
    _a="${1%%-*}" ; _b="${2%%-*}"
    _a1="${_a%%.*}" ; _r="${_a#*.}" ; _a2="${_r%%.*}" ; _a3="${_r#*.}"
    _b1="${_b%%.*}" ; _r="${_b#*.}" ; _b2="${_r%%.*}" ; _b3="${_r#*.}"
    _a1="${_a1:-0}" ; _a2="${_a2:-0}" ; _a3="${_a3:-0}"
    _b1="${_b1:-0}" ; _b2="${_b2:-0}" ; _b3="${_b3:-0}"
    [ "$_a1" -lt "$_b1" ] 2>/dev/null && return 0
    [ "$_a1" -gt "$_b1" ] 2>/dev/null && return 1
    [ "$_a2" -lt "$_b2" ] 2>/dev/null && return 0
    [ "$_a2" -gt "$_b2" ] 2>/dev/null && return 1
    [ "$_a3" -lt "$_b3" ] 2>/dev/null && return 0
    return 1
}

# Sets OIS_REMOTE_VER / OIS_LOCAL_VER. Returns 0 if update available.
ois_update_check() {
    _url="$(ois_reg_get "$1" version_url 2>/dev/null)" || return 1
    [ -z "$_url" ] && return 1
    _local="$(ois_reg_get "$1" version 2>/dev/null)"
    _remote="$(ois_fetch_version "$_url")" || return 1
    [ -z "$_remote" ] && return 1
    OIS_REMOTE_VER="$_remote" ; OIS_LOCAL_VER="$_local"
    export OIS_REMOTE_VER OIS_LOCAL_VER
    [ "$_local" = "$_remote" ] && return 1
    ois_version_older "$_local" "$_remote"
}

ois_update_run() {
    _app="$1" ; _yes="${2:-}"

    _bin="$(ois_reg_get "$_app" binary_path)" || ois_die "$_app is not installed."
    _gh="$(ois_reg_get  "$_app" github)"       || ois_die "No GitHub repo for $_app."
    _cur="$(ois_reg_get "$_app" version)"
    _mode="$(ois_reg_get "$_app" update_mode)"
    _url="$(ois_reg_get  "$_app" version_url)"

    ois_info "Checking for updates..."
    _remote="$(ois_fetch_version "$_url")" || {
        ois_warn "Cannot reach update server. $_cur still installed and working."
        return 1
    }

    if [ "$_cur" = "$_remote" ]; then ois_ok "Already up to date  ($_cur)."; return 0; fi
    if ! ois_version_older "$_cur" "$_remote"; then
        ois_ok "Local version ($_cur) is newer than remote ($_remote)."; return 0
    fi

    printf '\n  %bUpdate available:  %s  →  %s%b\n\n' "$_Y" "$_cur" "$_remote" "$_R"

    # notify mode only applies to background checks — explicit --update always prompts
    if [ "$_mode" = "notify" ] && [ "$_yes" != "yes" ] && [ "${_explicit:-}" != "yes" ]; then
        printf '  Run:  %s --update   to install.\n\n' "$_app"; return 0
    fi
    if [ "$_yes" != "yes" ] && [ "$_mode" != "auto" ]; then
        printf '  Install update? [y/N] ' ; read -r _r
        case "$_r" in y|Y|yes) ;; *) printf '  Cancelled.\n\n'; return 0 ;; esac
    fi

    # Backup, clone, build, install
    _backup="${_bin}.ois-bak"
    ois_cp "$_bin" "$_backup" 2>/dev/null || true

    _tmp="$(mktemp -d 2>/dev/null || mktemp -d -t ois_upd)"
    trap "rm -rf '$_tmp'" EXIT INT TERM

    ois_info "Downloading $_app $_remote..."
    git clone --depth 1 "https://github.com/$_gh" "$_tmp/src" \
        || { ois_err "Git clone failed."; _ois_rollback "$_bin" "$_backup"; return 1; }

    cd "$_tmp/src" || return 1
    ois_builder_detect
    ois_builder_clean 2>/dev/null || true
    ois_builder_build || { ois_err "Build failed."; _ois_rollback "$_bin" "$_backup"; cd - >/dev/null; return 1; }

    _new="$(ois_builder_find_binary)" || {
        ois_err "Binary not found after build."
        _ois_rollback "$_bin" "$_backup"; cd - >/dev/null; return 1
    }

    ois_cp "$_new" "$_bin" && ois_chmod 755 "$_bin" || {
        ois_err "Install failed."; _ois_rollback "$_bin" "$_backup"; cd - >/dev/null; return 1
    }

    ois_reg_set "$_app" version "$_remote"
    ois_rm "$_backup"
    cd - >/dev/null
    printf '\n' ; ois_ok "Updated to $_remote!" ; printf '\n'
}

_ois_rollback() {
    [ -f "$2" ] || return 0
    ois_info "Rolling back to previous version..."
    ois_cp "$2" "$1" ; ois_rm "$2"
    ois_ok "Rolled back."
}

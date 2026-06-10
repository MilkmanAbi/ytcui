#!/bin/sh
# OIS core/registry.sh
# Per-app plain-text records. Two files per app:
#   <app>.reg      — key=value metadata
#   <app>.manifest — one installed-file path per line
# ──────────────────────────────────────────────────────

ois_reg_dir() {
    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        printf '/usr/local/share/OIS'
    else
        printf '%s/.local/share/OIS' "${OIS_HOME:-$HOME}"
    fi
}

ois_reg_file()    { printf '%s/%s.reg'      "$(ois_reg_dir)" "$1"; }
ois_mf_file()     { printf '%s/%s.manifest' "$(ois_reg_dir)" "$1"; }

ois_reg_init()    { ois_mkdir "$(ois_reg_dir)"; }

# ── Write a key=value field ────────────────────────────
ois_reg_set() {
    _rs_app="$1" _rs_key="$2" _rs_val="$3"
    _rs_file="$(ois_reg_file "$_rs_app")"
    ois_reg_init
    # Strip existing key and append new value
    _rs_existing=""
    [ -f "$_rs_file" ] && _rs_existing="$(grep -v "^${_rs_key}=" "$_rs_file" 2>/dev/null || true)"
    if [ -n "$_rs_existing" ]; then
        ois_writef "${_rs_existing}
${_rs_key}=${_rs_val}" "$_rs_file"
    else
        ois_writef "${_rs_key}=${_rs_val}" "$_rs_file"
    fi
}

# ── Read a field ───────────────────────────────────────
ois_reg_get() {
    _rg_file="$(ois_reg_file "$1")"
    [ -f "$_rg_file" ] || return 1
    grep "^${2}=" "$_rg_file" 2>/dev/null | head -1 | cut -d= -f2-
}

# ── Check registered ───────────────────────────────────
ois_reg_has() { [ -f "$(ois_reg_file "$1")" ]; }

# ── Manifest append ────────────────────────────────────
ois_mf_add()  { ois_reg_init; ois_appendf "$2" "$(ois_mf_file "$1")"; }

# ── Manifest read ──────────────────────────────────────
ois_mf_read() { _mf="$(ois_mf_file "$1")"; [ -f "$_mf" ] && cat "$_mf"; }

# ── Remove app records ─────────────────────────────────
ois_reg_remove() {
    ois_rm "$(ois_reg_file "$1")"
    ois_rm "$(ois_mf_file  "$1")"
}

# ── List all installed apps ────────────────────────────
ois_reg_list() {
    _d="$(ois_reg_dir)"
    [ -d "$_d" ] || { printf '  (none)\n'; return; }
    _found=0
    for _f in "$_d"/*.reg; do
        [ -f "$_f" ] || continue
        _n="$(basename "$_f" .reg)"
        _v="$(ois_reg_get "$_n" version      2>/dev/null || printf '?')"
        _s="$(ois_reg_get "$_n" scope        2>/dev/null || printf '?')"
        _b="$(ois_reg_get "$_n" binary_path  2>/dev/null || printf '?')"
        _d2="$(ois_reg_get "$_n" installed_at 2>/dev/null || printf '?')"
        printf "  %-20s %-10s %-8s %-35s %s\n" "$_n" "$_v" "$_s" "$_b" "$_d2"
        _found=1
    done
    [ "$_found" -eq 0 ] && printf '  (none)\n'
}

#!/bin/sh
# OIS core/utils.sh — output helpers, privilege, privileged file ops
# Source first. Everything else depends on this.
# ─────────────────────────────────────────────────────────────────────

# ── Output ────────────────────────────────────────────
if [ -t 1 ]; then
    _B='\033[1m' _D='\033[2m' _R='\033[0m'
    _G='\033[32m' _Y='\033[33m' _C='\033[36m' _RED='\033[31m'
else
    _B='' _D='' _R='' _G='' _Y='' _C='' _RED=''
fi

ois_print() { printf '%s\n' "$*"; }
ois_ok()    { printf "  ${_G}✓${_R}  %s\n" "$*"; }
ois_info()  { printf "  ${_C}→${_R}  %s\n" "$*"; }
ois_warn()  { printf "  ${_Y}!${_R}  %s\n" "$*"; }
ois_err()   { printf "  ${_RED}✗${_R}  %s\n" "$*" >&2; }
ois_die()   { ois_err "$*"; exit 1; }
ois_hdr() {
    printf "${_B}${_C}══════════════════════════════════════════${_R}\n"
    printf "${_B}${_C}  %-38s${_R}\n" "$1"
    printf "${_D}  %-38s${_R}\n" "$2"
    printf "${_B}${_C}══════════════════════════════════════════${_R}\n\n"
}
ois_div() { printf "${_B}${_C}══════════════════════════════════════════${_R}\n"; }

# ── Privilege ─────────────────────────────────────────
# ois_priv is defined after system.sh sets OIS_IS_ROOT / OIS_SUDO,
# but is used inside utils functions — so we define it here as a stub
# that gets overridden when system.sh is sourced.
ois_priv() { "$@"; }

# ── Privileged mkdir ──────────────────────────────────
ois_mkdir() {
    mkdir -p "$1" 2>/dev/null \
        || sudo mkdir -p "$1" 2>/dev/null \
        || { ois_err "Cannot create: $1"; return 1; }
}

# ── Privileged copy ───────────────────────────────────
ois_cp() {
    cp "$1" "$2" 2>/dev/null \
        || sudo cp "$1" "$2" 2>/dev/null \
        || { ois_err "Cannot copy: $1 → $2"; return 1; }
}

# ── Privileged chmod ──────────────────────────────────
ois_chmod() {
    chmod "$1" "$2" 2>/dev/null \
        || sudo chmod "$1" "$2" 2>/dev/null || true
}

# ── Privileged write ──────────────────────────────────
# Writes content to a file, using sudo if needed.
# Always sets 644 so regular users can read system-scope files.
ois_writef() {
    _wf_content="$1"
    _wf_dest="$2"
    ois_mkdir "$(dirname "$_wf_dest")"
    _wf_tmp="$(mktemp)"
    printf '%s\n' "$_wf_content" > "$_wf_tmp"
    chmod 644 "$_wf_tmp" 2>/dev/null || true
    mv "$_wf_tmp" "$_wf_dest" 2>/dev/null \
        || sudo mv "$_wf_tmp" "$_wf_dest" 2>/dev/null \
        || { rm -f "$_wf_tmp"; ois_err "Cannot write: $_wf_dest"; return 1; }
}

# ── Privileged append ─────────────────────────────────
ois_appendf() {
    _af_line="$1"
    _af_dest="$2"
    ois_mkdir "$(dirname "$_af_dest")"
    # Ensure file exists with 644 before appending
    if [ ! -f "$_af_dest" ]; then
        touch "$_af_dest" 2>/dev/null || sudo touch "$_af_dest" 2>/dev/null || true
        chmod 644 "$_af_dest" 2>/dev/null || sudo chmod 644 "$_af_dest" 2>/dev/null || true
    fi
    printf '%s\n' "$_af_line" >> "$_af_dest" 2>/dev/null \
        || printf '%s\n' "$_af_line" | sudo tee -a "$_af_dest" >/dev/null 2>&1 \
        || { ois_err "Cannot append to: $_af_dest"; return 1; }
}

# ── Privileged remove ─────────────────────────────────
ois_rm()    { rm -f  "$1" 2>/dev/null || sudo rm -f  "$1" 2>/dev/null || true; }
ois_rmdir() { rm -rf "$1" 2>/dev/null || sudo rm -rf "$1" 2>/dev/null || true; }

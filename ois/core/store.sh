#!/bin/sh
# OIS v2 -- core/store.sh
# The store is the product. install/update/uninstall are transactions
# against it; everything else reads it.
#
# Layout:
#   $OIS_ROOT/
#     lock/                     mkdir(2) is atomic on POSIX -- no flock
#     runtime/<ver>/            versioned, refcounted, NEVER overwritten
#       ois.sh core/
#       refs                    one app name per line
#     runtime/current           symlink -> <ver>
#     apps/<app>/
#       conf                    THIS app's config. per-app. never shared.
#       meta                    key=value
#       manifest                type \t path \t sha256 \t policy
#       claims                  append-only, written by the app itself
#       allow                   claim allowlist prefixes, one per line
#       history                 append-only audit log
#       prev/                   previous binary, for instant rollback
#
# v1 kept a SINGLE shared runtime/OIS.conf, so installing a second app
# overwrote the first app's identity (`alpha --ois` printed "App beta")
# and uninstalling either app rm -rf'd the shared runtime, permanently
# bricking the other. Per-app dirs plus refcounted runtimes fix both.
# ---------------------------------------------------------------------

# OIS_TAB / OIS_NL come from utils.sh.

# -- Root resolution ---------------------------------------------------
ois_store_root() {
    [ -n "${OIS_ROOT:-}" ] && { printf '%s' "$OIS_ROOT"; return 0; }
    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        printf '/usr/local/lib/ois'
    else
        printf '%s/ois' "$OIS_XDG_DATA"
    fi
}

ois_apps_dir()    { printf '%s/apps' "$(ois_store_root)"; }
ois_app_dir()     { printf '%s/apps/%s' "$(ois_store_root)" "$1"; }
ois_runtime_dir() { printf '%s/runtime' "$(ois_store_root)"; }

ois_store_init() {
    _sr="$(ois_store_root)"
    ois_mkdir "$_sr/apps" || return 1
    ois_mkdir "$_sr/runtime" || return 1
    return 0
}

# -- Locking -----------------------------------------------------------
# mkdir(2) either creates the directory or fails; there is no window.
# That makes it the only lock primitive guaranteed by POSIX without
# flock(1) or a non-portable helper.
OIS_LOCK_HELD=0

ois_lock_acquire() {
    _lk_wait="${1:-30}"
    _lk="$(ois_store_root)/lock"
    ois_mkdir "$(ois_store_root)" || return 1
    _lk_n=0
    while [ "$_lk_n" -lt "$_lk_wait" ]; do
        if mkdir "$_lk" 2>/dev/null; then
            printf '%s\n' "$$" > "$_lk/pid" 2>/dev/null || :
            OIS_LOCK_HELD=1
            return 0
        fi
        # Stale lock: holder is gone, so reclaim it.
        if [ -r "$_lk/pid" ]; then
            read -r _lk_pid < "$_lk/pid" 2>/dev/null || _lk_pid=""
            if [ -n "$_lk_pid" ] && ! kill -0 "$_lk_pid" 2>/dev/null; then
                ois_warn "clearing stale lock from pid $_lk_pid"
                rm -rf "$_lk" 2>/dev/null || ois_priv rm -rf "$_lk" 2>/dev/null || :
                continue
            fi
        fi
        [ "$_lk_n" = 0 ] && ois_info "waiting for another OIS operation to finish..."
        sleep 1
        _lk_n=$(( _lk_n + 1 ))
    done
    ois_err "could not acquire lock: $_lk"
    return 1
}

ois_lock_release() {
    [ "$OIS_LOCK_HELD" = 1 ] || return 0
    _lk="$(ois_store_root)/lock"
    rm -rf "$_lk" 2>/dev/null || ois_priv rm -rf "$_lk" 2>/dev/null || :
    OIS_LOCK_HELD=0
}

# -- App lifecycle -----------------------------------------------------
ois_app_exists() { [ -f "$(ois_app_dir "$1")/meta" ]; }

ois_app_create() {
    ois_is_ident "$1" || { ois_err "invalid app name: $1"; return 1; }
    _ac="$(ois_app_dir "$1")"
    ois_mkdir "$_ac" || return 1
    for _f in meta manifest claims allow history; do
        [ -f "$_ac/$_f" ] || : | ois_write_atomic "$_ac/$_f" 644 || return 1
    done
    # World-writable claims file: the app may run as a different user
    # than the installer and still needs to append its own paths.
    chmod 666 "$_ac/claims" 2>/dev/null || \
        ois_priv chmod 666 "$_ac/claims" 2>/dev/null || :
    return 0
}

ois_app_list() {
    _al="$(ois_apps_dir)"
    [ -d "$_al" ] || return 0
    for _ae in "$_al"/*; do
        [ -f "$_ae/meta" ] || continue
        printf '%s\n' "${_ae##*/}"
    done
}

ois_app_destroy() {
    ois_is_ident "$1" || return 1
    ois_rmtree "$(ois_app_dir "$1")"
}

# -- Metadata ----------------------------------------------------------
# Plain key=value. Keys are validated identifiers, values may not
# contain a newline. No eval anywhere: v1 did
#   eval "OIS_DEP_${slot}_PKG_${attr}=\"$val\""
# on unvalidated config input, which was a root RCE because install.sh
# is routinely run under sudo.
ois_meta_get() {
    _mg="$(ois_app_dir "$1")/meta"
    [ -r "$_mg" ] || return 1
    while IFS= read -r _mgl || [ -n "$_mgl" ]; do
        case "$_mgl" in
            "$2="*) printf '%s' "${_mgl#*=}"; return 0 ;;
        esac
    done < "$_mg"
    return 1
}

# Read key=value pairs from stdin, apply them all in ONE rewrite.
# v1 called its per-key setter 11 times during install, each doing a
# full read-filter-rewrite cycle with no lock.
ois_meta_setmany() {
    _ms_app="$1"
    _ms_file="$(ois_app_dir "$_ms_app")/meta"
    _ms_new=""
    _ms_keys=""
    while IFS= read -r _ms_l || [ -n "$_ms_l" ]; do
        [ -z "$_ms_l" ] && continue
        _ms_k="${_ms_l%%=*}"
        ois_is_ident "$_ms_k" || { ois_err "invalid meta key: $_ms_k"; return 1; }
        case "$_ms_l" in *"$OIS_NL"*) ois_err "meta value has newline"; return 1 ;; esac
        _ms_keys="$_ms_keys $_ms_k "
        _ms_new="$_ms_new$_ms_l
"
    done
    _ms_keep=""
    if [ -r "$_ms_file" ]; then
        while IFS= read -r _ms_o || [ -n "$_ms_o" ]; do
            [ -z "$_ms_o" ] && continue
            _ms_ok="${_ms_o%%=*}"
            case "$_ms_keys" in *" $_ms_ok "*) continue ;; esac
            _ms_keep="$_ms_keep$_ms_o
"
        done < "$_ms_file"
    fi
    printf '%s%s' "$_ms_keep" "$_ms_new" | ois_write_atomic "$_ms_file" 644
}

ois_meta_set() { printf '%s=%s\n' "$2" "$3" | ois_meta_setmany "$1"; }

# -- History -----------------------------------------------------------
ois_history_add() {
    _ha="$(ois_app_dir "$1")/history"
    _ha_line="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)$OIS_TAB$2$OIS_TAB${3:-}"
    if [ -w "$_ha" ] 2>/dev/null; then
        printf '%s\n' "$_ha_line" >> "$_ha"
    else
        { [ -r "$_ha" ] && cat "$_ha"; printf '%s\n' "$_ha_line"; } | \
            ois_write_atomic "$_ha" 644
    fi
}

# -- Manifest ----------------------------------------------------------
# type \t path \t sha256 \t policy \t origin
#   type   : file | dir | link
#   policy : purge (always removed) | keep (kept unless --purge) | ask
#   origin : install (OIS put it there -- must exist)
#            claim   (the app registered it -- may not exist yet)
# The origin column is what lets `ois verify` hard-fail on a missing
# installed file while only warning about a path the app announced it
# would create.
ois_manifest_file() { printf '%s/manifest' "$(ois_app_dir "$1")"; }

ois_manifest_add() {
    _ma_app="$1" ; _ma_type="$2" ; _ma_path="$3"
    _ma_policy="${4:-purge}" ; _ma_origin="${5:-install}"
    _ma_path="$(ois_path_norm "$_ma_path")" || {
        ois_err "manifest path not absolute: $3"; return 1; }
    case "$_ma_path" in
        *"$OIS_TAB"*|*"$OIS_NL"*)
            ois_err "manifest path contains tab or newline: $_ma_path"; return 1 ;;
    esac
    _ma_sha='-'
    [ "$_ma_type" = "file" ] && _ma_sha="$(ois_sha256 "$_ma_path")"

    _ma_f="$(ois_manifest_file "$_ma_app")"
    _ma_out=""
    if [ -r "$_ma_f" ]; then
        while IFS= read -r _ma_l || [ -n "$_ma_l" ]; do
            [ -z "$_ma_l" ] && continue
            _ma_lp="${_ma_l#*"$OIS_TAB"}" ; _ma_lp="${_ma_lp%%"$OIS_TAB"*}"
            [ "$_ma_lp" = "$_ma_path" ] && continue   # dedupe: v1 duplicated on reinstall
            _ma_out="$_ma_out$_ma_l
"
        done < "$_ma_f"
    fi
    printf '%s%s\t%s\t%s\t%s\t%s\n' "$_ma_out" \
        "$_ma_type" "$_ma_path" "$_ma_sha" "$_ma_policy" "$_ma_origin" | \
        ois_write_atomic "$_ma_f" 644
}

# Emits: type \t path \t sha \t policy
ois_manifest_read() {
    _mr="$(ois_manifest_file "$1")"
    [ -r "$_mr" ] && cat "$_mr"
    return 0
}

ois_manifest_owner() {
    _mo_path="$(ois_path_norm "$1")" || return 1
    for _mo_app in $(ois_app_list); do
        _mo_f="$(ois_manifest_file "$_mo_app")"
        [ -r "$_mo_f" ] || continue
        while IFS="$OIS_TAB" read -r _mo_t _mo_p _mo_s _mo_pol _mo_o || [ -n "$_mo_t" ]; do
            [ "$_mo_p" = "$_mo_path" ] && { printf '%s' "$_mo_app"; return 0; }
        done < "$_mo_f"
    done
    return 1
}

# -- Claim allowlist ---------------------------------------------------
# Without this boundary a buggy app claims "/" and uninstall deletes the
# machine. Claims are accepted only under paths the installer declared.
ois_allow_add() {
    _aa_p="$(ois_path_norm "$2")" || { ois_err "allow path not absolute: $2"; return 1; }
    _aa_f="$(ois_app_dir "$1")/allow"
    _aa_out=""
    if [ -r "$_aa_f" ]; then
        while IFS= read -r _aa_l || [ -n "$_aa_l" ]; do
            [ -z "$_aa_l" ] && continue
            [ "$_aa_l" = "$_aa_p" ] && continue
            _aa_out="$_aa_out$_aa_l
"
        done < "$_aa_f"
    fi
    printf '%s%s\n' "$_aa_out" "$_aa_p" | ois_write_atomic "$_aa_f" 644
}

ois_allow_check() {
    _ac_f="$(ois_app_dir "$1")/allow"
    [ -r "$_ac_f" ] || return 1
    _ac_p="$(ois_path_norm "$2")" || return 1
    # Never permit a claim on a filesystem root or a bare top-level dir.
    case "$_ac_p" in /|/usr|/etc|/var|/bin|/sbin|/lib|/opt|/home|/root|/tmp) return 1 ;; esac
    while IFS= read -r _ac_l || [ -n "$_ac_l" ]; do
        [ -z "$_ac_l" ] && continue
        ois_path_under "$_ac_p" "$_ac_l" && return 0
    done < "$_ac_f"
    return 1
}

# -- Claims ------------------------------------------------------------
# The app appends here itself. A single write(2) in O_APPEND mode below
# PIPE_BUF (512 bytes minimum, guaranteed by POSIX) cannot interleave,
# so concurrent app processes are safe without OIS being involved and
# without OIS needing to be in PATH.
#
#   printf 'file\t%s\tkeep\n' "$path" >> "$OIS_CLAIMS"
#
ois_claims_file() { printf '%s/claims' "$(ois_app_dir "$1")"; }

# Validate, normalise, dedupe, fold accepted claims into the manifest,
# then truncate. Rejected claims are logged, never silently dropped.
ois_claims_fold() {
    _cf_app="$1"
    _cf_f="$(ois_claims_file "$_cf_app")"
    [ -s "$_cf_f" ] || return 0
    _cf_n=0 _cf_rej=0
    while IFS="$OIS_TAB" read -r _cf_t _cf_p _cf_pol || [ -n "$_cf_t" ]; do
        [ -z "$_cf_p" ] && continue
        case "$_cf_t" in file|dir|link) ;; *) _cf_t="file" ;; esac
        case "$_cf_pol" in purge|keep|ask) ;; *) _cf_pol="keep" ;; esac
        if ois_allow_check "$_cf_app" "$_cf_p"; then
            ois_manifest_add "$_cf_app" "$_cf_t" "$_cf_p" "$_cf_pol" claim && \
                _cf_n=$(( _cf_n + 1 ))
        else
            ois_warn "rejected claim outside allowlist: $_cf_p"
            ois_history_add "$_cf_app" "claim-rejected" "$_cf_p"
            _cf_rej=$(( _cf_rej + 1 ))
        fi
    done < "$_cf_f"
    : | ois_write_atomic "$_cf_f" 666
    if [ "$_cf_n"   -gt 0 ]; then ois_dbg  "folded $_cf_n claim(s) into manifest"; fi
    if [ "$_cf_rej" -gt 0 ]; then ois_warn "$_cf_rej claim(s) rejected"; fi
    return 0
}

# -- Runtime: versioned and refcounted --------------------------------
# v1 put the shared runtime path into EVERY app's manifest, so the
# first uninstall rm -rf'd it and broke every other installed app.
# Here a runtime tree is removed only when its last referent is gone.
ois_runtime_path() { printf '%s/%s' "$(ois_runtime_dir)" "$1"; }

ois_runtime_install() {
    _ri_ver="$1" ; _ri_src="$2"
    _ri_dst="$(ois_runtime_path "$_ri_ver")"
    if [ -f "$_ri_dst/ois.sh" ]; then
        ois_dbg "runtime $_ri_ver already present"
    else
        ois_mkdir "$_ri_dst/core" || return 1
        ois_install_file "$_ri_src/ois.sh" "$_ri_dst/ois.sh" 755 || return 1
        for _ri_f in "$_ri_src"/core/*.sh; do
            [ -f "$_ri_f" ] || continue
            ois_install_file "$_ri_f" "$_ri_dst/core/${_ri_f##*/}" 644 || return 1
        done
    fi
    # Publish `current` by renaming a symlink: rename(2) over an existing
    # link is atomic, so no reader ever observes a missing runtime.
    # Record the active version in a plain file too: readlink(1) is not
    # POSIX and parsing `ls -l` output is fragile.
    printf '%s\n' "$_ri_ver" | ois_write_atomic "$(ois_runtime_dir)/.active" 644
    _ri_cur="$(ois_runtime_dir)/current"
    _ri_tmp="$(ois_runtime_dir)/.current.$$"
    _ri_done=0
    if ln -s "$_ri_ver" "$_ri_tmp" 2>/dev/null; then
        if mv -f "$_ri_tmp" "$_ri_cur" 2>/dev/null; then _ri_done=1; fi
    fi
    if [ "$_ri_done" = 0 ]; then
        rm -f "$_ri_tmp" 2>/dev/null || :
        if ois_priv ln -s "$_ri_ver" "$_ri_tmp" 2>/dev/null; then
            ois_priv mv -f "$_ri_tmp" "$_ri_cur" 2>/dev/null || :
        fi
    fi
    return 0
}

ois_runtime_ref_add() {
    _ra_f="$(ois_runtime_path "$1")/refs"
    _ra_out=""
    if [ -r "$_ra_f" ]; then
        while IFS= read -r _ra_l || [ -n "$_ra_l" ]; do
            [ -z "$_ra_l" ] && continue
            [ "$_ra_l" = "$2" ] && continue
            _ra_out="$_ra_out$_ra_l
"
        done < "$_ra_f"
    fi
    printf '%s%s\n' "$_ra_out" "$2" | ois_write_atomic "$_ra_f" 644
}

ois_runtime_ref_del() {
    _rd_f="$(ois_runtime_path "$1")/refs"
    [ -r "$_rd_f" ] || return 0
    _rd_out=""
    while IFS= read -r _rd_l || [ -n "$_rd_l" ]; do
        [ -z "$_rd_l" ] && continue
        [ "$_rd_l" = "$2" ] && continue
        _rd_out="$_rd_out$_rd_l
"
    done < "$_rd_f"
    printf '%s' "$_rd_out" | ois_write_atomic "$_rd_f" 644
}

ois_runtime_refcount() {
    _rc_f="$(ois_runtime_path "$1")/refs"
    [ -r "$_rc_f" ] || { printf '0'; return 0; }
    _rc_n=0
    while IFS= read -r _rc_l || [ -n "$_rc_l" ]; do
        [ -n "$_rc_l" ] && _rc_n=$(( _rc_n + 1 ))
    done < "$_rc_f"
    printf '%s' "$_rc_n"
}

# Remove only runtimes nothing references. Never called implicitly by
# uninstall of a single app.
ois_runtime_gc() {
    _rg_d="$(ois_runtime_dir)"
    [ -d "$_rg_d" ] || return 0
    _rg_cur=""
    if [ -r "$_rg_d/.active" ]; then read -r _rg_cur < "$_rg_d/.active" || _rg_cur=""; fi
    _rg_n=0
    for _rg_v in "$_rg_d"/*; do
        [ -d "$_rg_v" ] || continue
        _rg_name="${_rg_v##*/}"
        case "$_rg_name" in current|.active) continue ;; esac
        [ "$(ois_runtime_refcount "$_rg_name")" != "0" ] && continue
        [ "$_rg_name" = "$_rg_cur" ] && continue
        ois_rmtree "$_rg_v" && { ois_ok "removed unreferenced runtime $_rg_name"; \
            _rg_n=$(( _rg_n + 1 )); }
    done
    printf '%s' "$_rg_n" > /dev/null
    return 0
}

# -- Environment handed to the app ------------------------------------
# Written as plain KEY=value at a PREDICTABLE path:
#     <store>/apps/<name>/env
# so an app launched DIRECTLY by the user (the normal case -- OIS is not
# in the process chain) can still find its own OIS state by checking the
# two standard store roots. See docs/03-PROTOCOL.md for the 6-line
# lookup an app should use.
ois_env_write() {
    _ew_app="$1"
    _ew_f="$(ois_app_dir "$_ew_app")/env"
    {
        printf 'OIS_APP=%s\n'         "$_ew_app"
        printf 'OIS_APP_VERSION=%s\n' "$(ois_meta_get "$_ew_app" version || printf 'unknown')"
        printf 'OIS_CLAIMS=%s\n'      "$(ois_claims_file "$_ew_app")"
        printf 'OIS_CONFIG_DIR=%s\n'  "$(ois_meta_get "$_ew_app" config_dir || printf '')"
        printf 'OIS_DATA_DIR=%s\n'    "$(ois_meta_get "$_ew_app" data_dir   || printf '')"
        printf 'OIS_CACHE_DIR=%s\n'   "$(ois_meta_get "$_ew_app" cache_dir  || printf '')"
        printf 'OIS_STATE_DIR=%s\n'   "$(ois_meta_get "$_ew_app" state_dir  || printf '')"
    } | ois_write_atomic "$_ew_f" 644
}

# -- Verification ------------------------------------------------------
# Exit 0 if every manifest entry is present and unmodified.
ois_verify_app() {
    _va_app="$1" ; _va_bad=0 ; _va_miss=0 ; _va_unk=0 ; _va_pend=0
    _va_f="$(ois_manifest_file "$_va_app")"
    [ -r "$_va_f" ] || { ois_err "no manifest for $_va_app"; return 1; }
    while IFS="$OIS_TAB" read -r _va_t _va_p _va_s _va_pol _va_o || [ -n "$_va_t" ]; do
        [ -z "$_va_p" ] && continue
        if [ ! -e "$_va_p" ]; then
            # An owned directory is a claim of ownership, not an
            # assertion that it exists: the app creates its config dir
            # on first run, so absence here is normal, not corruption.
            if [ "$_va_t" = "dir" ] || [ "$_va_o" = "claim" ]; then
                _va_pend=$(( _va_pend + 1 ))
                ois_dbg "not yet created  $_va_p" ; continue
            fi
            ois_err "missing   $_va_p" ; _va_miss=$(( _va_miss + 1 )) ; continue
        fi
        [ "$_va_t" = "file" ] || { ois_dbg "ok        $_va_p"; continue; }
        [ "$_va_s" = "-" ] && { _va_unk=$(( _va_unk + 1 )); ois_dbg "unhashed  $_va_p"; continue; }
        if [ "$(ois_sha256 "$_va_p")" = "$_va_s" ]; then
            ois_dbg "ok        $_va_p"
        else
            ois_warn "modified  $_va_p" ; _va_bad=$(( _va_bad + 1 ))
        fi
    done < "$_va_f"
    if [ "$_va_unk"  -gt 0 ]; then ois_info "$_va_unk entr(ies) had no recorded hash"; fi
    if [ "$_va_pend" -gt 0 ]; then ois_info "$_va_pend owned path(s) not created yet"; fi
    if [ "$_va_miss" -gt 0 ] || [ "$_va_bad" -gt 0 ]; then return 1; fi
    return 0
}

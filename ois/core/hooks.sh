#!/bin/sh
# OIS v3 -- core/hooks.sh
# Lifecycle hooks and data migrations.
#
# Project layout:
#   ois/hooks/pre-install.sh     before anything is installed
#   ois/hooks/post-install.sh    after install completes
#   ois/hooks/pre-update.sh      after payload acquired, BEFORE swap
#   ois/hooks/post-update.sh     after swap + migrations
#   ois/hooks/pre-uninstall.sh   before removal starts
#   ois/hooks/post-uninstall.sh  after removal completes
#   ois/migrate/<version>.sh     run once when crossing <version> upward
#
# Design keystone: hooks and migrations are CAPTURED INTO THE STORE at
# install time (apps/<name>/hooks, apps/<name>/migrate). At uninstall
# the source tree no longer exists, and a prebuilt-asset update has no
# tree either -- the store copy is what makes both work. Source updates
# refresh the capture; prebuilt updates reuse it (and try to refresh it
# from the source tarball when hooks exist, non-fatally).
#
# Failure semantics:
#   pre-*   failing ABORTS the operation. That is their purpose: if
#           pre-update cannot stop your daemon, do not swap its binary.
#   post-*  failing reports E-HOOK and exits nonzero, but does not roll
#           anything back -- rolling back a completed install because a
#           notification script failed would be worse.
#   migration failing triggers AUTOMATIC BINARY ROLLBACK (update.sh).
#
# Hooks run with sh, output captured to apps/<name>/hook.log, last 15
# lines shown on failure. Environment: everything in ois_hook_env.
# ---------------------------------------------------------------------

OIS_HOOK_EVENTS="pre-install post-install pre-update post-update pre-uninstall post-uninstall"

ois_hooks_dir()   { printf '%s/hooks'   "$(ois_app_dir "$1")"; }
ois_migrate_dir() { printf '%s/migrate' "$(ois_app_dir "$1")"; }

# -- Capture -----------------------------------------------------------
# Copy ois/hooks/*.sh and ois/migrate/*.sh from a project tree into the
# store. Replaces the previous capture entirely (a hook deleted upstream
# must not linger). Migration filenames must be valid versions.
ois_hooks_capture() {
    _hc_app="$1" ; _hc_root="$2"
    _hc_hd="$(ois_hooks_dir "$_hc_app")"
    _hc_md="$(ois_migrate_dir "$_hc_app")"
    rm -rf "$_hc_hd" "$_hc_md" 2>/dev/null || :
    if [ -d "$_hc_root/ois/hooks" ]; then
        for _hc_f in "$_hc_root"/ois/hooks/*.sh; do
            [ -f "$_hc_f" ] || continue
            _hc_n="${_hc_f##*/}" ; _hc_n="${_hc_n%.sh}"
            case " $OIS_HOOK_EVENTS " in
                *" $_hc_n "*)
                    ois_mkdir "$_hc_hd" || return 1
                    ois_install_file "$_hc_f" "$_hc_hd/$_hc_n.sh" 644 || return 1 ;;
                *) ois_warn "ignoring unknown hook: ${_hc_f##*/}" ;;
            esac
        done
    fi
    if [ -d "$_hc_root/ois/migrate" ]; then
        for _hc_f in "$_hc_root"/ois/migrate/*.sh; do
            [ -f "$_hc_f" ] || continue
            _hc_v="${_hc_f##*/}" ; _hc_v="${_hc_v%.sh}"
            case "$_hc_v" in
                *[!0-9.v]*|'') ois_warn "ignoring migration with non-version name: ${_hc_f##*/}"
                               continue ;;
            esac
            ois_mkdir "$_hc_md" || return 1
            ois_install_file "$_hc_f" "$_hc_md/$_hc_v.sh" 644 || return 1
        done
    fi
    return 0
}

ois_app_has_hooks() {
    [ -d "$(ois_hooks_dir "$1")" ] && return 0
    [ -d "$(ois_migrate_dir "$1")" ] && return 0
    return 1
}

# -- Environment -------------------------------------------------------
# ois_hook_env APP EVENT OLD_VER NEW_VER  -- exports the hook contract.
ois_hook_env() {
    OIS_HOOK_APP="$1"
    OIS_EVENT="$2"
    OIS_OLD_VERSION="${3:-}"
    OIS_NEW_VERSION="${4:-}"
    OIS_APP="$1"
    OIS_APP_VERSION="$(ois_meta_get "$1" version 2>/dev/null)" || OIS_APP_VERSION=""
    OIS_BINARY="$(ois_meta_get "$1" binary 2>/dev/null)" || OIS_BINARY=""
    OIS_CONFIG_DIR="$(ois_meta_get "$1" config_dir 2>/dev/null)" || OIS_CONFIG_DIR=""
    OIS_DATA_DIR="$(ois_meta_get "$1" data_dir 2>/dev/null)" || OIS_DATA_DIR=""
    OIS_CACHE_DIR="$(ois_meta_get "$1" cache_dir 2>/dev/null)" || OIS_CACHE_DIR=""
    OIS_STATE_DIR="$(ois_meta_get "$1" state_dir 2>/dev/null)" || OIS_STATE_DIR=""
    OIS_CLAIMS="$(ois_claims_file "$1")"
    export OIS_HOOK_APP OIS_EVENT OIS_OLD_VERSION OIS_NEW_VERSION OIS_APP
    export OIS_APP_VERSION OIS_BINARY OIS_CONFIG_DIR OIS_DATA_DIR
    export OIS_CACHE_DIR OIS_STATE_DIR OIS_CLAIMS OIS_SCOPE OIS_PREFIX
}

# -- Running -----------------------------------------------------------
# ois_hook_run APP EVENT OLD NEW [SRC_DIR]
# SRC_DIR overrides the store capture (install-time: hooks come from the
# project tree, which is captured only after they succeed).
# Return: 0 ran ok or no such hook; 1 hook failed.
ois_hook_run() {
    _hr_app="$1" ; _hr_ev="$2" ; _hr_old="${3:-}" ; _hr_new="${4:-}"
    _hr_src="${5:-$(ois_hooks_dir "$_hr_app")}"
    _hr_f="$_hr_src/$_hr_ev.sh"
    [ -f "$_hr_f" ] || return 0
    ois_info "hook: $_hr_ev"
    ois_hook_env "$_hr_app" "$_hr_ev" "$_hr_old" "$_hr_new"
    _hr_log="$(ois_app_dir "$_hr_app")/hook.log"
    if ois_run_logged "$_hr_log" "hook $_hr_ev" sh "$_hr_f"; then
        return 0
    fi
    _hr_rc=$?
    ois_fail E-HOOK "hook $_hr_ev failed (exit $_hr_rc)" \
        "your ois/hooks/$_hr_ev.sh returned nonzero -- see the excerpt above" \
        "full log: $_hr_log" \
        "fix the hook, then re-run the same command"
    return 1
}

# -- Migrations --------------------------------------------------------
# List captured migration versions applicable to OLD -> NEW, ascending:
# every V with  OLD < V <= NEW.
ois_migrations_pending() {
    _mp_app="$1" ; _mp_old="$2" ; _mp_new="$3"
    _mp_d="$(ois_migrate_dir "$_mp_app")"
    [ -d "$_mp_d" ] || return 0
    _mp_list=""
    for _mp_f in "$_mp_d"/*.sh; do
        [ -f "$_mp_f" ] || continue
        _mp_v="${_mp_f##*/}" ; _mp_v="${_mp_v%.sh}"
        ois_ver_older "$_mp_old" "$_mp_v" || continue          # OLD < V
        if ois_ver_older "$_mp_new" "$_mp_v"; then continue; fi # V <= NEW
        # insertion sort by version (lists are tiny)
        if [ -z "$_mp_list" ]; then
            _mp_list="$_mp_v"
        else
            _mp_out="" ; _mp_ins=0
            for _mp_e in $_mp_list; do
                if [ "$_mp_ins" = 0 ] && ois_ver_older "$_mp_v" "$_mp_e"; then
                    _mp_out="$_mp_out $_mp_v" ; _mp_ins=1
                fi
                _mp_out="$_mp_out $_mp_e"
            done
            [ "$_mp_ins" = 0 ] && _mp_out="$_mp_out $_mp_v"
            _mp_list="${_mp_out# }"
        fi
    done
    # shellcheck disable=SC2086  # deliberate split: one version per line
    [ -n "$_mp_list" ] && printf '%s\n' $_mp_list
    return 0
}

# ois_migrations_run APP OLD NEW -> 0 all ok, 1 a migration failed
# (OIS_MIGRATION_FAILED holds the version that broke;
#  OIS_MIGRATIONS_APPLIED holds the space-separated list that ran OK
#  before it -- these have ALREADY mutated on-disk data and are NOT
#  auto-reverted, so callers must report them honestly).
ois_migrations_run() {
    _mg_app="$1" ; _mg_old="$2" ; _mg_new="$3"
    OIS_MIGRATION_FAILED=""
    OIS_MIGRATIONS_APPLIED=""
    export OIS_MIGRATION_FAILED OIS_MIGRATIONS_APPLIED
    _mg_d="$(ois_migrate_dir "$_mg_app")"
    _mg_log="$(ois_app_dir "$_mg_app")/hook.log"
    for _mg_v in $(ois_migrations_pending "$_mg_app" "$_mg_old" "$_mg_new"); do
        ois_info "migration: $_mg_v  ($_mg_old -> $_mg_new)"
        ois_hook_env "$_mg_app" "migrate" "$_mg_old" "$_mg_new"
        OIS_MIGRATION="$_mg_v" ; export OIS_MIGRATION
        if ! ois_run_logged "$_mg_log" "migration $_mg_v" sh "$_mg_d/$_mg_v.sh"; then
            OIS_MIGRATION_FAILED="$_mg_v"
            return 1
        fi
        OIS_MIGRATIONS_APPLIED="${OIS_MIGRATIONS_APPLIED:+$OIS_MIGRATIONS_APPLIED }$_mg_v"
        ois_history_add "$_mg_app" migrate "$_mg_v"
    done
    return 0
}

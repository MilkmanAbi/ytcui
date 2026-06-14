#!/bin/sh
# ═══════════════════════════════════════════════════════════════════════════════
# OIS — OneInstallSystem  v1.0.0
# Drop OIS/ into any project. Users run: sh install.sh
# Pure POSIX sh — Linux, macOS, FreeBSD, OpenBSD, NetBSD, WSL
# ═══════════════════════════════════════════════════════════════════════════════

OIS_VERSION="1.0.0"
OIS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$OIS_DIR/.." && pwd)"
export OIS_VERSION OIS_DIR PROJECT_ROOT

# ── Source core ───────────────────────────────────────
. "$OIS_DIR/core/utils.sh"
. "$OIS_DIR/core/system.sh"
. "$OIS_DIR/core/conf.sh"
. "$OIS_DIR/core/registry.sh"
. "$OIS_DIR/core/deps.sh"
. "$OIS_DIR/core/builder.sh"
. "$OIS_DIR/core/updater.sh"
. "$OIS_DIR/core/integrate.sh"

# When the macOS user-pass elevated us, reuse its resolved package manager and
# PKG_CONFIG_PATH (so the root `make` finds the keg-only Homebrew .pc files)
# instead of re-detecting / re-prompting as root.
if [ "${OIS_DEPS_DONE:-0}" = "1" ]; then
    [ -n "${OIS_PM_RESOLVED:-}" ] && OIS_PM="$OIS_PM_RESOLVED" && export OIS_PM
    [ -n "${OIS_PKG_CONFIG_PATH_RESOLVED:-}" ] && \
        PKG_CONFIG_PATH="$OIS_PKG_CONFIG_PATH_RESOLVED" && export PKG_CONFIG_PATH
    [ -n "${OIS_BREW_PREFIX_RESOLVED:-}" ] && \
        OIS_BREW_PREFIX="$OIS_BREW_PREFIX_RESOLVED" && export OIS_BREW_PREFIX
fi

# ── Sudo escalation ───────────────────────────────────
# Re-execs as root for system-scope commands. One sudo prompt. Done.
_ois_elevate() {
    [ "${OIS_SCOPE:-user}" = "system" ] || return 0
    [ "$OIS_IS_ROOT" = "yes" ]          && return 0
    [ "$OIS_SUDO" = "none" ] && \
        ois_die "System install requires sudo or doas. Use --user to install to ~/.local/bin"
    printf "\n  ${_B}Administrator privileges required.${_R}\n"
    exec $OIS_SUDO sh "$OIS_DIR/OIS.sh" "$@"
}

# macOS: elevate while carrying the user-resolved deps environment across the
# sudo boundary, so the root pass reuses the package manager we already set up
# (and skips re-running brew/port as root, which would fail).
_ois_elevate_macos() {
    [ "$OIS_SUDO" = "none" ] && \
        ois_die "System install requires sudo or doas. Use --user to install to ~/.local/bin"
    printf "\n  ${_B}Administrator privileges required${_R}  ${_D}(to install into %s)${_R}\n" "$OIS_APP_INSTALL_PATH"
    exec $OIS_SUDO env \
        OIS_DEPS_DONE="${OIS_DEPS_DONE:-1}" \
        OIS_PM_RESOLVED="${OIS_PM_RESOLVED:-}" \
        OIS_PKG_CONFIG_PATH_RESOLVED="${OIS_PKG_CONFIG_PATH_RESOLVED:-}" \
        OIS_BREW_PREFIX_RESOLVED="${OIS_BREW_PREFIX_RESOLVED:-}" \
        sh "$OIS_DIR/OIS.sh" "$@"
}

# ── Scope ─────────────────────────────────────────────
_resolve_scope() {
    _force="${1:-}"
    if   [ "$_force" = "user" ];            then OIS_SCOPE="user"
    elif [ "$_force" = "system" ];          then OIS_SCOPE="system"
    elif [ "$OIS_IS_ROOT" = "yes" ];        then OIS_SCOPE="system"
    elif [ "$OIS_SUDO" != "none" ];         then OIS_SCOPE="system"
    elif [ "${OIS_APP_REQUIRE_SUDO:-auto}" = "no" ]; then OIS_SCOPE="user"
    else
        OIS_SCOPE="user"
        ois_warn "No sudo — installing to ~/.local/bin"
    fi
    [ "$OIS_SCOPE" = "user" ] && OIS_APP_INSTALL_PATH="${OIS_HOME}/.local/bin"
    export OIS_SCOPE OIS_APP_INSTALL_PATH
}

# ── Restore project root ──────────────────────────────
# When called via hook (OIS_DIR = runtime), PROJECT_ROOT is wrong.
# Read the stored project root from registry instead.
_fix_project_root() {
    _fpr="$(ois_reg_get "${OIS_APP_NAME:-_}" project_root 2>/dev/null)" || return 0
    [ -n "$_fpr" ] && [ -d "$_fpr" ] && { PROJECT_ROOT="$_fpr"; export PROJECT_ROOT; }
}

# ═══════════════════════════════════════════════════════
# INSTALL
# ═══════════════════════════════════════════════════════
cmd_install() {
    ois_hdr "  $OIS_APP_DISPLAY" "Installing  ·  OIS v$OIS_VERSION  ·  $OIS_OS $OIS_ARCH"
    printf "  ${_D}%-16s${_R} %s\n" "System:"  "$OIS_OS${OIS_DISTRO:+ ($OIS_DISTRO $OIS_DISTRO_VER)}"
    printf "  ${_D}%-16s${_R} %s\n" "Package:" "$OIS_PM"
    printf "  ${_D}%-16s${_R} %s\n" "Scope:"   "$OIS_SCOPE"
    printf "  ${_D}%-16s${_R} %s/%s\n" "Install:" "$OIS_APP_INSTALL_PATH" "$OIS_APP_BINARY"
    printf '\n'

    # Already installed?
    if ois_reg_has "$OIS_APP_NAME"; then
        _ev="$(ois_reg_get "$OIS_APP_NAME" version)"
        ois_warn "$OIS_APP_NAME v$_ev is already installed."
        printf '\n  Reinstall? [y/N] ' ; read -r _r
        case "$_r" in y|Y|yes) _ois_remove_current ;; *)
            printf '\n  Tip: %s --update\n\n' "$OIS_APP_BINARY"; exit 0 ;; esac
        printf '\n'
    fi

    # [1/4] Deps
    if [ "${OIS_DEPS_DONE:-0}" = "1" ]; then
        # Already resolved as the real user (macOS pre-elevation pass).
        printf "${_B}[1/4] Dependencies${_R}\n"
        ois_ok "Resolved as $OIS_USER  (skipping — Homebrew/MacPorts can't run as root)"
        printf '\n'
    else
        printf "${_B}[1/4] Dependencies${_R}\n"
        ois_deps_install || ois_die "Required dependency failed — fix above and retry."
        printf '\n'
    fi

    # [2/4] Build
    printf "${_B}[2/4] Build${_R}\n"
    _fix_project_root
    cd "$PROJECT_ROOT" || ois_die "Cannot cd to project root: $PROJECT_ROOT"
    ois_builder_detect
    ois_builder_clean 2>/dev/null || true
    ois_builder_build
    _built="$(ois_builder_find_binary)" || ois_die "Binary not found after build."
    ois_ok "Binary: $_built  ($(du -sh "$_built" 2>/dev/null | cut -f1))"
    printf '\n'

    # [3/4] Install binary
    printf "${_B}[3/4] Install${_R}\n"
    ois_mkdir "$OIS_APP_INSTALL_PATH"
    _dest="$OIS_APP_INSTALL_PATH/$OIS_APP_BINARY"
    ois_cp "$_built" "$_dest" && ois_chmod 755 "$_dest"
    ois_ok "Installed      →  $_dest"

    # Update preference
    _umode="$OIS_APP_UPDATE_MODE"
    if [ "$_umode" = "ask" ]; then
        printf '\n  Enable automatic update checks on launch? [Y/n] '
        read -r _u
        case "$_u" in n|N|no) _umode="manual" ;; *) _umode="notify" ;; esac
    fi
    printf '\n'

    # [4/4] Integrate
    printf "${_B}[4/4] Integrate${_R}\n"
    _ver="unknown"
    [ -f "$PROJECT_ROOT/VERSION" ] && _ver="$(tr -d '[:space:]' < "$PROJECT_ROOT/VERSION")"

    # Build dep list for --install-info (stored as "name:cmd:optional" tokens)
    _dep_str=""
    _di=0
    while [ "$_di" -lt "$OIS_DEP_COUNT" ]; do
        eval "_dn=\$OIS_DEP_${_di}_NAME"
        eval "_dc=\$OIS_DEP_${_di}_CMD"
        eval "_do=\$OIS_DEP_${_di}_OPT"
        eval "_dpc=\$OIS_DEP_${_di}_PKGCONFIG"
        _dep_str="${_dep_str}${_dn}:${_dc:-$_dn}:${_do}:${_dpc} "
        _di=$(( _di + 1 ))
    done

    ois_reg_init
    ois_reg_set "$OIS_APP_NAME" version         "$_ver"
    ois_reg_set "$OIS_APP_NAME" binary_path     "$_dest"
    ois_reg_set "$OIS_APP_NAME" scope           "$OIS_SCOPE"
    ois_reg_set "$OIS_APP_NAME" version_url     "$OIS_APP_VERSION_URL"
    ois_reg_set "$OIS_APP_NAME" github          "$OIS_APP_GITHUB"
    ois_reg_set "$OIS_APP_NAME" update_mode     "$_umode"
    ois_reg_set "$OIS_APP_NAME" installed_at    "$(date '+%Y-%m-%d %H:%M %Z')"
    ois_reg_set "$OIS_APP_NAME" project_root    "$PROJECT_ROOT"
    ois_reg_set "$OIS_APP_NAME" installed_by    "$OIS_PM"
    ois_reg_set "$OIS_APP_NAME" additional_info "${OIS_APP_ADDITIONAL_INFO:-}"
    ois_reg_set "$OIS_APP_NAME" deps            "$_dep_str"
    # Binary is the FIRST manifest entry — always
    ois_mf_add  "$OIS_APP_NAME" "$_dest"

    ois_integrate_run "$_dest"
    printf '\n'

    # Done
    ois_div
    printf "${_B}${_G}  ✓  $OIS_APP_DISPLAY installed!${_R}\n"
    ois_div
    printf '\n'
    printf "  ${_B}%-34s${_R} %s\n" "$OIS_APP_BINARY"             "launch"
    printf "  ${_B}%-34s${_R} %s\n" "$OIS_APP_BINARY --ois"       "OIS panel"
    printf "  ${_B}%-34s${_R} %s\n" "$OIS_APP_BINARY --update"    "update"
    printf "  ${_B}%-34s${_R} %s\n" "$OIS_APP_BINARY --uninstall" "uninstall"
    printf '\n'

    # PATH reminder
    case ":${PATH}:" in *":${OIS_APP_INSTALL_PATH}:"*) ;;
        *) ois_warn "Add to your shell config:"
           printf '\n    export PATH="$PATH:%s"\n\n' "$OIS_APP_INSTALL_PATH" ;;
    esac
}

# ── Remove current install (no prompts — used internally) ──
_ois_remove_current() {
    # Read binary and hook paths from registry BEFORE wiping it
    _rc_bin="$(ois_reg_get  "$OIS_APP_NAME" binary_path 2>/dev/null)"
    _rc_hook="${OIS_APP_INSTALL_PATH}/.${OIS_APP_BINARY}-ois"
    # Also try deriving hook from binary path in case install path differs
    [ -n "$_rc_bin" ] && _rc_hook="$(dirname "$_rc_bin")/.${OIS_APP_BINARY}-ois"

    # Remove binary first
    if [ -n "$_rc_bin" ] && [ -f "$_rc_bin" ]; then
        ois_rm "$_rc_bin" && ois_ok "Removed  $_rc_bin"
    fi
    # Remove hook
    [ -f "$_rc_hook" ] && ois_rm "$_rc_hook" && ois_ok "Removed  $_rc_hook"

    # Remove manifest items (skip runtime — we may still be running from it)
    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        _rc_runtime="/usr/local/share/OIS/runtime"
    else
        _rc_runtime="${OIS_HOME}/.local/share/OIS/runtime"
    fi
    _rc_mf="$(ois_mf_file "$OIS_APP_NAME")"
    if [ -f "$_rc_mf" ]; then
        while IFS= read -r _f || [ -n "$_f" ]; do
            [ -z "$_f" ] && continue
            [ "$_f" = "$_rc_bin" ]  && continue
            [ "$_f" = "$_rc_hook" ] && continue
            case "$_f" in "$_rc_runtime"*) continue ;; esac
            [ -f "$_f" ] && ois_rm    "$_f" && ois_ok "Removed  $_f"
            [ -d "$_f" ] && ois_rmdir "$_f" && ois_ok "Removed  $_f"
        done < "$_rc_mf"
    fi

    # Wipe registry records
    ois_rm "$(ois_mf_file  "$OIS_APP_NAME")"
    ois_rm "$(ois_reg_file "$OIS_APP_NAME")"
}

# ═══════════════════════════════════════════════════════
# UNINSTALL
# ═══════════════════════════════════════════════════════
cmd_uninstall() {
    ois_hdr "  $OIS_APP_DISPLAY" "Uninstall  ·  OIS v$OIS_VERSION"
    ois_reg_has "$OIS_APP_NAME" || ois_die "$OIS_APP_NAME is not installed."

    _u="$(ois_reg_get "$OIS_APP_NAME" uninstaller 2>/dev/null)"
    if [ -n "$_u" ] && [ -f "$_u" ]; then
        sh "$_u" "$@"
    else
        # Fallback: manual removal
        ois_warn "Uninstaller not found — removing directly."
        printf '  Remove %s? [y/N] ' "$OIS_APP_NAME" ; read -r _r
        case "$_r" in y|Y|yes) ;; *) printf '  Cancelled.\n\n'; exit 0 ;; esac
        _ois_remove_current
        ois_ok "$OIS_APP_NAME removed."
    fi
}

# ═══════════════════════════════════════════════════════
# UPDATE / UPGRADE
# ═══════════════════════════════════════════════════════
cmd_update() {
    ois_hdr "  $OIS_APP_DISPLAY" "Update  ·  OIS v$OIS_VERSION"
    ois_reg_has "$OIS_APP_NAME" || ois_die "$OIS_APP_NAME is not installed."
    _explicit=yes ois_update_run "$OIS_APP_NAME" "${1:-}"
}

# ═══════════════════════════════════════════════════════
# REINSTALL
# ═══════════════════════════════════════════════════════
cmd_reinstall() {
    ois_hdr "  $OIS_APP_DISPLAY" "Reinstall  ·  OIS v$OIS_VERSION"

    # Read everything we need from the registry BEFORE removing it
    _ri_root="$(ois_reg_get  "$OIS_APP_NAME" project_root 2>/dev/null)"
    _ri_gh="$(ois_reg_get    "$OIS_APP_NAME" github       2>/dev/null)"

    if ois_reg_has "$OIS_APP_NAME"; then
        ois_info "Removing current install..."
        _ois_remove_current
        ois_ok "Removed."
    fi

    # Decide build source
    _ri_tmp=""
    if [ -n "$_ri_root" ] && [ -d "$_ri_root" ] && \
       { [ -f "$_ri_root/Makefile" ] || [ -f "$_ri_root/CMakeLists.txt" ] || \
         [ -f "$_ri_root/Cargo.toml" ] || [ -f "$_ri_root/go.mod" ] || \
         [ -f "$_ri_root/setup.py" ]   || [ -f "$_ri_root/pyproject.toml" ]; }; then
        PROJECT_ROOT="$_ri_root"
        export PROJECT_ROOT
        ois_info "Building from: $PROJECT_ROOT"
    elif [ -n "$_ri_gh" ]; then
        ois_info "Source dir gone — cloning fresh from GitHub..."
        _ri_tmp="$(mktemp -d 2>/dev/null || mktemp -d -t ois_ri)"
        git clone --depth 1 "https://github.com/$_ri_gh" "$_ri_tmp/src" \
            || ois_die "Git clone failed."
        PROJECT_ROOT="$_ri_tmp/src"
        export PROJECT_ROOT
    else
        ois_die "No source dir and no GitHub repo — cannot reinstall."
    fi

    printf '\n'
    cmd_install

    [ -n "$_ri_tmp" ] && rm -rf "$_ri_tmp" 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════
# REPAIR
# ═══════════════════════════════════════════════════════
cmd_repair() {
    ois_hdr "  $OIS_APP_DISPLAY" "Repair  ·  OIS v$OIS_VERSION"
    ois_reg_has "$OIS_APP_NAME" || ois_die "$OIS_APP_NAME is not installed."
    _dest="$(ois_reg_get "$OIS_APP_NAME" binary_path)"
    ois_info "Rebuilding binary — config and data untouched."
    _fix_project_root
    cd "$PROJECT_ROOT" || ois_die "Cannot cd to project root."
    ois_builder_detect ; ois_builder_clean 2>/dev/null || true ; ois_builder_build
    _built="$(ois_builder_find_binary)" || ois_die "Binary not found after build."
    ois_cp "$_built" "$_dest" && ois_chmod 755 "$_dest"
    ois_ok "Repaired  →  $_dest"
    printf '\n'
}

# ═══════════════════════════════════════════════════════
# --ois PANEL
# ═══════════════════════════════════════════════════════
cmd_ois_panel() {
    ois_hdr "  $OIS_APP_DISPLAY" "Managed by OIS v$OIS_VERSION"

    if ! ois_reg_has "$OIS_APP_NAME"; then
        ois_warn "$OIS_APP_NAME is not installed."
        printf '\n  Run:  sh install.sh\n\n'; return 0
    fi

    _ver="$(ois_reg_get  "$OIS_APP_NAME" version)"
    _mode="$(ois_reg_get "$OIS_APP_NAME" update_mode)"
    _date="$(ois_reg_get "$OIS_APP_NAME" installed_at)"
    _gh="$(ois_reg_get   "$OIS_APP_NAME" github)"
    _url="$(ois_reg_get  "$OIS_APP_NAME" version_url)"
    _by="$(ois_reg_get   "$OIS_APP_NAME" installed_by)"
    _info="$(ois_reg_get "$OIS_APP_NAME" additional_info)"

    # ── Version & update status ──
    printf "  ${_D}%-14s${_R} v%s\n"  "Version:"  "$_ver"
    printf "  ${_D}%-14s${_R} %s\n"   "Updates:"  "$_mode"
    printf '\n'
    if [ "$_mode" != "off" ] && [ -n "$_url" ]; then
        if ois_update_check "$OIS_APP_NAME" 2>/dev/null; then
            printf "  ${_Y}⬆  Update available:  %s  →  %s${_R}\n" \
                "$OIS_LOCAL_VER" "$OIS_REMOTE_VER"
            printf "  ${_Y}   Run: %s --update${_R}\n" "$OIS_APP_BINARY"
        else
            ois_ok "Up to date  ($OIS_REMOTE_VER)"
        fi
    fi

    # ── Package info ──
    printf '\n'
    printf "  ${_D}%s${_R} Installed on:     %s\n" "$OIS_APP_NAME" "$_date"
    printf "  ${_D}%s${_R} Source:           https://github.com/%s\n" "$OIS_APP_NAME" "$_gh"
    printf "  ${_D}%s${_R} Remote version:   %s\n" "$OIS_APP_NAME" "$_url"
    [ -n "$_info" ] && \
    printf "  ${_D}%s${_R} Info:             %s\n" "$OIS_APP_NAME" "$_info"
    printf "  ${_D}%s${_R} Installed by:     %s\n" "$OIS_APP_NAME" "$_by"

    # ── Commands ──
    printf '\n'
    printf "  ${_B}Commands:${_R}\n\n"
    printf "  %-38s %s\n" "$OIS_APP_BINARY --update"       "update to latest version"
    printf "  %-38s %s\n" "$OIS_APP_BINARY --upgrade"      "same as --update"
    printf "  %-38s %s\n" "$OIS_APP_BINARY --uninstall"    "remove cleanly"
    printf "  %-38s %s\n" "$OIS_APP_BINARY --reinstall"    "full clean reinstall from source"
    printf "  %-38s %s\n" "$OIS_APP_BINARY --install-info" "full installation details"
    printf '\n'
    printf "  ${_D}OIS v%s — https://github.com/MilkmanAbi/OneInstallSystem${_R}\n\n" "$OIS_VERSION"
}

# ═══════════════════════════════════════════════════════
# --install-info
# ═══════════════════════════════════════════════════════
cmd_install_info() {
    ois_hdr "  $OIS_APP_DISPLAY" "Installation Info  ·  OIS v$OIS_VERSION"
    if ! ois_reg_has "$OIS_APP_NAME"; then
        ois_warn "Not installed."; printf '\n  Run: sh install.sh\n\n'; return 0
    fi
    _ver="$(ois_reg_get  "$OIS_APP_NAME" version)"
    _bin="$(ois_reg_get  "$OIS_APP_NAME" binary_path)"
    _scp="$(ois_reg_get  "$OIS_APP_NAME" scope)"
    _mode="$(ois_reg_get "$OIS_APP_NAME" update_mode)"
    _date="$(ois_reg_get "$OIS_APP_NAME" installed_at)"
    _gh="$(ois_reg_get   "$OIS_APP_NAME" github)"
    _url="$(ois_reg_get  "$OIS_APP_NAME" version_url)"
    _root="$(ois_reg_get "$OIS_APP_NAME" project_root)"
    _bin_s="✓" ; [ ! -f "$_bin" ] && _bin_s="${_RED}MISSING${_R}"

    printf "  ${_B}Package${_R}\n"
    printf "  ${_D}%-20s${_R} %s\n"   "Name:"      "$OIS_APP_DISPLAY"
    printf "  ${_D}%-20s${_R} v%s\n"  "Version:"   "$_ver"
    printf "  ${_D}%-20s${_R} %s\n"   "Installed:" "$_date"
    printf '\n'
    printf "  ${_B}Location${_R}\n"
    printf "  ${_D}%-20s${_R} %b  [%b]\n" "Binary:"   "$_bin" "$_bin_s"
    printf "  ${_D}%-20s${_R} %s\n"    "Scope:"     "$_scp"
    printf "  ${_D}%-20s${_R} %s\n"    "Source:"    "${_root:-unknown}"
    printf '\n'
    printf "  ${_B}Updates${_R}\n"
    printf "  ${_D}%-20s${_R} %s\n"    "Mode:"      "$_mode"
    printf "  ${_D}%-20s${_R} %s\n"    "GitHub:"    "github.com/$_gh"
    printf "  ${_D}%-20s${_R} %s\n"    "URL:"       "$_url"
    printf '\n'
    printf "  ${_B}Update check${_R}\n"
    if [ -n "$_url" ]; then
        ois_info "Fetching..."
        if ois_update_check "$OIS_APP_NAME" 2>/dev/null; then
            printf "  ${_Y}%-20s${_R} ${_Y}%s → %s  (update available)${_R}\n" \
                "Status:" "$OIS_LOCAL_VER" "$OIS_REMOTE_VER"
        else
            printf "  ${_D}%-20s${_R} ✓ Up to date  (%s)\n" "Status:" "$_ver"
        fi
    else
        printf "  ${_D}%-20s${_R} no version_url set\n" "Status:"
    fi
    printf '\n'
    printf "  ${_B}Dependencies${_R}\n"
    _deps="$(ois_reg_get "$OIS_APP_NAME" deps 2>/dev/null)"
    if [ -n "$_deps" ]; then
        for _de in $_deps; do
            _dn="${_de%%:*}" ; _r="${_de#*:}" ; _dc="${_r%%:*}"
            _r2="${_r#*:}" ; _do="${_r2%%:*}" ; _dpc="${_r2#*:}"
            if [ -n "$_dpc" ] && [ "$_dpc" != "$_do" ] && command -v pkg-config >/dev/null 2>&1; then
                pkg-config --exists "$_dpc" 2>/dev/null \
                    && _ds="${_G}✓ installed${_R}" || _ds="${_Y}not found${_R}"
            elif command -v "${_dc:-$_dn}" >/dev/null 2>&1; then
                _ds="${_G}✓ installed${_R}"
            else
                _ds="${_Y}not found${_R}"
            fi
            _dot="" ; [ "$_do" = "yes" ] && _dot=" (optional)"
            printf "  ${_D}  %-18s${_R} %b%s\n" "$_dn" "$_ds" "$_dot"
        done
    else
        printf "  ${_D}  (none declared)${_R}\n"
    fi
    printf '\n'
    printf "  ${_B}Installed files${_R}\n"
    ois_mf_read "$OIS_APP_NAME" | while IFS= read -r _f; do
        [ -z "$_f" ] && continue
        _fs="✓" ; [ ! -e "$_f" ] && _fs="${_Y}missing${_R}"
        printf "  ${_D}%b${_R}  %s\n" "$_fs" "$_f"
    done
    printf '\n  %bOIS v%s  ·  %s %s%b\n\n' "$_D" "$OIS_VERSION" "$OIS_OS" "$OIS_ARCH" "$_R"
}

# ═══════════════════════════════════════════════════════
# STATUS / INFO / LIST / HELP
# ═══════════════════════════════════════════════════════
cmd_status() {
    ois_hdr "  $OIS_APP_DISPLAY" "Status  ·  OIS v$OIS_VERSION"
    if ! ois_reg_has "$OIS_APP_NAME"; then
        ois_warn "Not installed."; printf '\n  Run: sh install.sh\n\n'; return 0
    fi
    _ver="$(ois_reg_get "$OIS_APP_NAME" version)"
    _bin="$(ois_reg_get "$OIS_APP_NAME" binary_path)"
    _mode="$(ois_reg_get "$OIS_APP_NAME" update_mode)"
    _date="$(ois_reg_get "$OIS_APP_NAME" installed_at)"
    _bs="✓" ; [ ! -f "$_bin" ] && _bs="${_RED}missing${_R}"
    printf "  ${_D}%-18s${_R} v%s  (%s)\n" "Installed:" "$_ver" "$_date"
    printf "  ${_D}%-18s${_R} %s  [%b]\n"  "Binary:"    "$_bin" "$_bs"
    printf "  ${_D}%-18s${_R} %s\n"         "Updates:"  "$_mode"
    printf '\n'
    if [ -n "$OIS_APP_VERSION_URL" ]; then
        ois_info "Checking for updates..."
        if ois_update_check "$OIS_APP_NAME" 2>/dev/null; then
            printf "  ${_Y}Update: %s → %s${_R}\n  Run: %s --update\n" \
                "$OIS_LOCAL_VER" "$OIS_REMOTE_VER" "$OIS_APP_BINARY"
        else
            ois_ok "Up to date  ($_ver)"
        fi
    fi
    printf '\n'
}

cmd_info() {
    ois_hdr "  OIS System Info" "OIS v$OIS_VERSION"
    printf "  ${_D}%-22s${_R} %s\n" "OS:"         "$OIS_OS"
    printf "  ${_D}%-22s${_R} %s\n" "Distro:"     "${OIS_DISTRO:-n/a} ${OIS_DISTRO_VER:-}"
    printf "  ${_D}%-22s${_R} %s\n" "Arch:"       "$OIS_ARCH"
    printf "  ${_D}%-22s${_R} %s\n" "Package mgr:""$OIS_PM"
    printf "  ${_D}%-22s${_R} %s\n" "Privilege:"  "${OIS_SUDO}  (root: $OIS_IS_ROOT)"
    printf "  ${_D}%-22s${_R} %s\n" "Make:"       "$OIS_MAKE"
    printf "  ${_D}%-22s${_R} %s\n" "Python:"     "${OIS_PYTHON:-not found}"
    printf "  ${_D}%-22s${_R} %s\n" ".NET:"       "${OIS_DOTNET:-not found}"
    printf '\n'
}

cmd_list() {
    ois_hdr "  OIS — Installed Apps" "OIS v$OIS_VERSION"
    printf "  ${_D}%-20s %-10s %-8s %-35s %s${_R}\n" "App" "Version" "Scope" "Binary" "Date"
    printf '  %s\n' "──────────────────────────────────────────────────────────────────────"
    ois_reg_list
    printf '\n'
}

cmd_help() {
    printf '\n%bOIS — OneInstallSystem  v%s%b\n\n' "$_B" "$OIS_VERSION" "$_R"
    printf '  The one folder that makes any Unix app installable.\n\n'
    printf '  %bDev setup:%b  put OIS/ in your project, fill in OIS/OIS.conf\n' "$_B" "$_R"
    printf '  %bUser install:%b  sh install.sh\n\n' "$_B" "$_R"
    printf '  %bApp flags (after install):%b\n\n' "$_B" "$_R"
    printf '  %-36s %s\n' "myapp --ois"           "OIS panel"
    printf '  %-36s %s\n' "myapp --install-info"  "full install details"
    printf '  %-36s %s\n' "myapp --update"        "update to latest"
    printf '  %-36s %s\n' "myapp --upgrade"       "same as --update"
    printf '  %-36s %s\n' "myapp --uninstall"     "remove cleanly"
    printf '  %-36s %s\n' "myapp --reinstall"     "full clean reinstall"
    printf '\n  %bDirect OIS commands:%b\n\n' "$_B" "$_R"
    printf '  %-36s %s\n' "sh OIS/OIS.sh install"      "install"
    printf '  %-36s %s\n' "sh OIS/OIS.sh update"       "update"
    printf '  %-36s %s\n' "sh OIS/OIS.sh uninstall"    "uninstall"
    printf '  %-36s %s\n' "sh OIS/OIS.sh reinstall"    "reinstall"
    printf '  %-36s %s\n' "sh OIS/OIS.sh repair"       "rebuild binary only"
    printf '  %-36s %s\n' "sh OIS/OIS.sh status"       "status"
    printf '  %-36s %s\n' "sh OIS/OIS.sh install-info" "full install details"
    printf '  %-36s %s\n' "sh OIS/OIS.sh info"         "system detection"
    printf '  %-36s %s\n' "sh OIS/OIS.sh list"         "all OIS apps on system"
    printf '\n  %bFlags:%b  --user  --system  --yes  --version\n\n' "$_B" "$_R"
}

# ═══════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════
main() {
    _cmd="" _scope="" _yes=""
    for _a in "$@"; do
        case "$_a" in
            install|uninstall|update|upgrade|reinstall|repair|\
            status|info|list|help|ois|install-info) _cmd="$_a" ;;
            --user)    _scope="user"   ;;
            --system)  _scope="system" ;;
            --yes|-y)  _yes="yes"      ;;
            --version) printf 'OIS v%s\n' "$OIS_VERSION"; exit 0 ;;
            --update|--upgrade|--uninstall|--reinstall|--ois|--install-info)
                _cmd="${_a#--}" ;;
        esac
    done

    case "$_cmd" in info|list|help) ;;
        *) ois_conf_load ;;
    esac

    _resolve_scope "$_scope"

    # macOS: Homebrew REFUSES to run as root, and MacPorts deps are better
    # resolved as the invoking user too. So for a system install we run the
    # dependency phase (PM choice + brew/port installs) as the real user FIRST,
    # then elevate only for the binary copy into /usr/local/bin. We hand the
    # resolved package manager + PKG_CONFIG_PATH across the sudo boundary via
    # environment variables so the root pass doesn't redo (and re-prompt) it.
    if [ "$_cmd" = "install" ] && [ "$OIS_OS" = "macos" ] \
       && [ "$OIS_SCOPE" = "system" ] && [ "$OIS_IS_ROOT" != "yes" ]; then
        printf "${_B}[1/4] Dependencies${_R}  ${_D}(as %s — Homebrew/MacPorts must not run as root)${_R}\n" "$OIS_USER"
        ois_deps_install || ois_die "Required dependency failed — fix above and retry."
        printf '\n'
        # Pass the resolved environment into the elevated re-exec.
        OIS_DEPS_DONE=1 \
        OIS_PM_RESOLVED="$OIS_PM" \
        OIS_PKG_CONFIG_PATH_RESOLVED="${PKG_CONFIG_PATH:-}" \
        OIS_BREW_PREFIX_RESOLVED="${OIS_BREW_PREFIX:-}" \
            _ois_elevate_macos "$@"
    fi

    # Elevate for write-requiring commands
    case "$_cmd" in
        install|uninstall|update|upgrade|reinstall|repair)
            _ois_elevate "$@" ;;
    esac

    case "$_cmd" in
        install)          cmd_install        ;;
        uninstall)        cmd_uninstall      ;;
        update|upgrade)   cmd_update "$_yes" ;;
        reinstall)        cmd_reinstall      ;;
        repair)           cmd_repair         ;;
        status)           cmd_status         ;;
        info)             cmd_info           ;;
        list)             cmd_list           ;;
        help)             cmd_help           ;;
        ois)              cmd_ois_panel      ;;
        install-info)     cmd_install_info   ;;
        "")
            if ois_reg_has "${OIS_APP_NAME:-_}" 2>/dev/null; then
                cmd_status
            else
                cmd_install
            fi ;;
        *) ois_err "Unknown: $_cmd"; printf '  sh OIS/OIS.sh help\n\n'; exit 1 ;;
    esac
}

main "$@"

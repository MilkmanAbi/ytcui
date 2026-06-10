#!/bin/sh
# OIS core/conf.sh — parses OIS/OIS.conf
# ─────────────────────────────────────────

ois_conf_load() {
    _cf="${OIS_DIR}/OIS.conf"
    [ -f "$_cf" ] || ois_die "OIS.conf not found: $_cf"

    OIS_APP_NAME="" OIS_APP_DISPLAY="" OIS_APP_BINARY=""
    OIS_APP_VERSION_URL="" OIS_APP_GITHUB=""
    OIS_APP_INSTALL_PATH="/usr/local/bin"
    OIS_APP_UPDATE_MODE="ask"
    OIS_APP_BUILD_SYSTEM="auto" OIS_APP_BINARY_OUT="" OIS_APP_CUSTOM_BUILD=""
    OIS_APP_ICON="" OIS_APP_DESKTOP_CAT="Utility" OIS_APP_DESKTOP_CMT=""
    OIS_APP_REQUIRE_SUDO="auto" OIS_APP_PYTHON_ENTRY=""
    OIS_APP_ADDITIONAL_INFO=""
    OIS_DEP_COUNT=0
    _section="main"

    while IFS= read -r _line || [ -n "$_line" ]; do
        _line="${_line%%#*}"
        _line="$(printf '%s' "$_line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
        [ -z "$_line" ] && continue
        case "$_line" in
            \[deps\])          _section="deps"    ; continue ;;
            \[deps.optional\]) _section="opt"     ; continue ;;
            \[build\])         _section="build"   ; continue ;;
            \[*\])             _section="other"   ; continue ;;
        esac
        _k="${_line%%=*}" ; _v="${_line#*=}"
        _k="$(printf '%s' "$_k" | sed 's/[[:space:]]*$//')"
        _v="$(printf '%s' "$_v" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

        case "$_section" in
            main)
                case "$_k" in
                    app_name)         OIS_APP_NAME="$_v"         ;;
                    display_name)     OIS_APP_DISPLAY="$_v"      ;;
                    binary)           OIS_APP_BINARY="$_v"       ;;
                    version_url)      OIS_APP_VERSION_URL="$_v"  ;;
                    github)           OIS_APP_GITHUB="$_v"       ;;
                    install_path)     OIS_APP_INSTALL_PATH="$_v" ;;
                    update_mode)      OIS_APP_UPDATE_MODE="$_v"  ;;
                    icon)             OIS_APP_ICON="$_v"         ;;
                    desktop_category) OIS_APP_DESKTOP_CAT="$_v"  ;;
                    desktop_comment)  OIS_APP_DESKTOP_CMT="$_v"  ;;
                    require_sudo)     OIS_APP_REQUIRE_SUDO="$_v" ;;
                    python_entry)     OIS_APP_PYTHON_ENTRY="$_v" ;;
                    additional_info)  OIS_APP_ADDITIONAL_INFO="$_v" ;;
                esac ;;
            build)
                case "$_k" in
                    system)     OIS_APP_BUILD_SYSTEM="$_v"  ;;
                    binary_out) OIS_APP_BINARY_OUT="$_v"    ;;
                    custom)     OIS_APP_CUSTOM_BUILD="$_v"  ;;
                esac ;;
            deps|opt)
                _logical="${_k%%.*}" ; _attr="${_k#*.}"
                _slot="" ; _i=0
                while [ "$_i" -lt "$OIS_DEP_COUNT" ]; do
                    eval "_dn=\$OIS_DEP_${_i}_NAME"
                    [ "$_dn" = "$_logical" ] && { _slot="$_i"; break; }
                    _i=$(( _i + 1 ))
                done
                if [ -z "$_slot" ]; then
                    _slot="$OIS_DEP_COUNT"
                    eval "OIS_DEP_${_slot}_NAME=\"$_logical\""
                    eval "OIS_DEP_${_slot}_OPT=no"
                    OIS_DEP_COUNT=$(( _slot + 1 ))
                fi
                case "$_attr" in
                    cmd)       eval "OIS_DEP_${_slot}_CMD=\"$_v\""  ;;
                    desc)      eval "OIS_DEP_${_slot}_DESC=\"$_v\"" ;;
                    pkgconfig) eval "OIS_DEP_${_slot}_PKGCONFIG=\"$_v\"" ;;
                    *)         eval "OIS_DEP_${_slot}_PKG_${_attr}=\"$_v\"" ;;
                esac
                [ "$_section" = "opt" ] && eval "OIS_DEP_${_slot}_OPT=yes"
                ;;
        esac
    done < "$_cf"

    [ -z "$OIS_APP_NAME" ]    && ois_die "OIS.conf: app_name is required"
    [ -z "$OIS_APP_BINARY" ]  && OIS_APP_BINARY="$OIS_APP_NAME"
    [ -z "$OIS_APP_DISPLAY" ] && OIS_APP_DISPLAY="$OIS_APP_NAME"
    [ -z "$OIS_APP_BINARY_OUT" ] && OIS_APP_BINARY_OUT="$OIS_APP_BINARY"

    export OIS_APP_NAME OIS_APP_DISPLAY OIS_APP_BINARY OIS_APP_VERSION_URL
    export OIS_APP_GITHUB OIS_APP_INSTALL_PATH OIS_APP_UPDATE_MODE
    export OIS_APP_BUILD_SYSTEM OIS_APP_BINARY_OUT OIS_APP_CUSTOM_BUILD
    export OIS_APP_ICON OIS_APP_DESKTOP_CAT OIS_APP_DESKTOP_CMT
    export OIS_APP_REQUIRE_SUDO OIS_APP_PYTHON_ENTRY OIS_APP_ADDITIONAL_INFO
    export OIS_DEP_COUNT
}

#!/bin/sh
# OIS v2 -- core/json.sh
# Machine-readable output for scripts, CI, and other programs.
#
# Everything OIS knows is already plain key=value on disk, so --json is
# a rendering concern only. Strings are escaped properly: backslash,
# double quote, and the C0 control characters JSON forbids raw.
# ---------------------------------------------------------------------

# Escape one string for use inside JSON double quotes.
# shellcheck disable=SC1003  # '\' below is a literal backslash, intentional
ois_json_esc() {
    _je="$1" ; _je_o=""
    while [ -n "$_je" ]; do
        _je_c="${_je%"${_je#?}"}" ; _je="${_je#?}"
        case "$_je_c" in
            '\')  _je_o="$_je_o\\\\" ;;
            '"')  _je_o="$_je_o\\\"" ;;
            "$OIS_TAB") _je_o="$_je_o\\t" ;;
            *)
                # Control characters (< 0x20) must be escaped as \u00XX.
                case "$_je_c" in
                    [[:cntrl:]]) _je_o="$_je_o " ;;
                    *)           _je_o="$_je_o$_je_c" ;;
                esac ;;
        esac
    done
    printf '%s' "$_je_o"
}

ois_json_kv() {   # ois_json_kv KEY VALUE [trailing-comma?]
    printf '"%s": "%s"%s' "$(ois_json_esc "$1")" "$(ois_json_esc "$2")" "${3:-}"
}

# -- Renderers ---------------------------------------------------------
ois_json_app() {   # one app object, indented by 4
    _ja="$1"
    printf '    {\n'
    printf '      "name": "%s",\n' "$(ois_json_esc "$_ja")"
    for _ja_k in version state binary hook scope prefix runtime github \
                 update_mode installed_at latest_seen; do
        _ja_v="$(ois_meta_get "$_ja" "$_ja_k" 2>/dev/null)" || _ja_v=""
        printf '      "%s": "%s",\n' "$_ja_k" "$(ois_json_esc "$_ja_v")"
    done
    printf '      "paths": [\n'
    _ja_first=1
    while IFS="$OIS_TAB" read -r _ja_t _ja_p _ja_s _ja_pol _ja_o || [ -n "$_ja_t" ]; do
        [ -z "$_ja_p" ] && continue
        [ "$_ja_first" = 0 ] && printf ',\n'
        _ja_first=0
        if [ -e "$_ja_p" ]; then _ja_st="present"
        elif [ "$_ja_o" = "claim" ] || [ "$_ja_t" = "dir" ]; then _ja_st="pending"
        else _ja_st="missing" ; fi
        printf '        {"type": "%s", "path": "%s", "policy": "%s", "origin": "%s", "status": "%s", "sha256": "%s"}' \
            "$(ois_json_esc "$_ja_t")" "$(ois_json_esc "$_ja_p")" \
            "$(ois_json_esc "$_ja_pol")" "$(ois_json_esc "${_ja_o:-install}")" \
            "$_ja_st" "$(ois_json_esc "$_ja_s")"
    done < "$(ois_manifest_file "$_ja")"
    [ "$_ja_first" = 0 ] && printf '\n'
    printf '      ]\n    }'
}

ois_json_list() {
    printf '{\n  "ois_version": "%s",\n' "$(ois_json_esc "$OIS_VERSION")"
    printf '  "scope": "%s",\n  "root": "%s",\n' \
        "$(ois_json_esc "$OIS_SCOPE")" "$(ois_json_esc "$(ois_store_root)")"
    printf '  "os": "%s",\n  "arch": "%s",\n  "package_manager": "%s",\n' \
        "$(ois_json_esc "$OIS_OS")" "$(ois_json_esc "$OIS_ARCH")" \
        "$(ois_json_esc "$OIS_PM")"
    printf '  "apps": [\n'
    _jl_first=1
    for _jl_a in $(ois_app_list); do
        [ "$_jl_first" = 0 ] && printf ',\n'
        _jl_first=0
        ois_json_app "$_jl_a"
    done
    [ "$_jl_first" = 0 ] && printf '\n'
    printf '  ]\n}\n'
}

ois_json_info() {
    printf '{\n  "ois_version": "%s",\n  "app":\n' "$(ois_json_esc "$OIS_VERSION")"
    ois_json_app "$1"
    printf '\n}\n'
}

ois_json_check() {   # ois_json_check APP STATUS LOCAL REMOTE
    printf '{\n  %s\n  %s\n  %s\n  %s\n}\n' \
        "$(ois_json_kv app "$1" ,)" \
        "$(ois_json_kv status "$2" ,)" \
        "$(ois_json_kv local_version "${3:-}" ,)" \
        "$(ois_json_kv remote_version "${4:-}")"
}

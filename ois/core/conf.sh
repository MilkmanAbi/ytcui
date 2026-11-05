#!/bin/sh
# OIS v2 -- core/conf.sh
# Parses ois.conf without eval and without forking sed.
#
# v1 did two forks of sed PER LINE for trimming, and stored dependency
# values with
#     eval "OIS_DEP_${slot}_PKG_${attr}=\"$val\""
# on unvalidated input. A value of  x"; cmd; :"  executed cmd, as root
# whenever install.sh was run under sudo. Nothing here reaches eval:
# every destination is a fixed variable selected by a case statement.
# ---------------------------------------------------------------------

# Expand a small, closed set of tokens. Not a shell expansion -- a
# literal substitution table -- so config can never inject code.
# shellcheck disable=SC2016,SC2088
# The patterns below are LITERAL text to match in config values, not
# expansions -- that is the entire point of the substitution table.
ois_conf_expand() {
    _ce="$1"
    case "$_ce" in
        '~'|'~/'*) _ce="$OIS_HOME${_ce#\~}" ;;
    esac
    while : ; do
        case "$_ce" in
            *'$XDG_CONFIG_HOME'*) _ce="${_ce%%\$XDG_CONFIG_HOME*}$OIS_XDG_CONFIG${_ce#*\$XDG_CONFIG_HOME}" ;;
            *'$XDG_DATA_HOME'*)   _ce="${_ce%%\$XDG_DATA_HOME*}$OIS_XDG_DATA${_ce#*\$XDG_DATA_HOME}" ;;
            *'$XDG_CACHE_HOME'*)  _ce="${_ce%%\$XDG_CACHE_HOME*}$OIS_XDG_CACHE${_ce#*\$XDG_CACHE_HOME}" ;;
            *'$XDG_STATE_HOME'*)  _ce="${_ce%%\$XDG_STATE_HOME*}$OIS_XDG_STATE${_ce#*\$XDG_STATE_HOME}" ;;
            *'$HOME'*)            _ce="${_ce%%\$HOME*}$OIS_HOME${_ce#*\$HOME}" ;;
            *'$APP'*)             _ce="${_ce%%\$APP*}$OIS_APP_NAME${_ce#*\$APP}" ;;
            *) break ;;
        esac
    done
    printf '%s' "$_ce"
}

# Comment rules, chosen so free-text values survive:
#   - a full-line comment is '#' as the first non-blank character
#   - a trailing comment is '#' preceded by whitespace AND followed by
#     whitespace or end-of-line
#   - '\#' is always a literal '#'
# So "Rated #1 tool", "#ff0000" and "http://x#frag" keep their hash,
# while "binary = foo # note" drops the note. v1 used ${line%%#*},
# which ate all four.
ois_conf_decomment() {
    _cd_in="$1" ; _cd_out="" ; _cd_prev=""
    while [ -n "$_cd_in" ]; do
        _cd_c="${_cd_in%"${_cd_in#?}"}"
        _cd_in="${_cd_in#?}"
        if [ "$_cd_c" = "#" ] && [ "$_cd_prev" = "\\" ]; then
            _cd_out="${_cd_out%\\}#" ; _cd_prev="#" ; continue
        fi
        if [ "$_cd_c" = "#" ]; then
            _cd_next="${_cd_in%"${_cd_in#?}"}"
            case "$_cd_prev" in
                ''|' '|"$OIS_TAB")
                    case "$_cd_next" in
                        ''|' '|"$OIS_TAB") break ;;
                    esac ;;
            esac
        fi
        _cd_out="$_cd_out$_cd_c"
        _cd_prev="$_cd_c"
    done
    printf '%s' "$_cd_out"
}

ois_conf_load() {
    _cl_file="$1"
    [ -r "$_cl_file" ] || { ois_err "config not readable: $_cl_file"; return 1; }

    OIS_APP_NAME="" OIS_APP_DISPLAY="" OIS_APP_BINARY=""
    OIS_APP_GITHUB="" OIS_APP_PREFIX="" OIS_APP_UPDATE_MODE="notify"
    OIS_APP_DESC="" OIS_APP_REQUIRE_SUDO="auto"
    OIS_BUILD_SYSTEM="auto" OIS_BUILD_OUT="" OIS_BUILD_CUSTOM="" OIS_BUILD_JOBS="auto"
    OIS_BUILD_TARGET="" OIS_BUILD_CMAKE_OPTS="" OIS_BUILD_MAKE_OPTS=""
    OIS_OWNS_CONFIG="" OIS_OWNS_DATA="" OIS_OWNS_CACHE="" OIS_OWNS_STATE=""
    OIS_OWNS_EXTRA="" OIS_DEPS_RAW="" OIS_DEPS_OPT_RAW=""
    OIS_APP_CHANNEL="stable" OIS_SIGNING_KEY="" OIS_NEXT_BEST_VERSION="no"
    OIS_SVC_ENABLE="" OIS_SVC_ARGS="" OIS_SVC_DESC="" OIS_SVC_RESTART="on-failure"
    OIS_SVC_AFTER="network" OIS_BINARIES_RAW=""

    _cl_sec="main" _cl_n=0
    while IFS= read -r _cl_l || [ -n "$_cl_l" ]; do
        _cl_n=$(( _cl_n + 1 ))
        _cl_l="$(ois_conf_decomment "$_cl_l")"
        _cl_l="$(ois_trim "$_cl_l")"
        [ -z "$_cl_l" ] && continue

        case "$_cl_l" in
            '['*']')
                case "$_cl_l" in
                    '[deps]')          _cl_sec="deps"  ;;
                    '[deps.optional]') _cl_sec="depso" ;;
                    '[build]')         _cl_sec="build" ;;
                    '[owns]')          _cl_sec="owns"  ;;
                    '[service]')       _cl_sec="svc"   ;;
                    '[binaries]')      _cl_sec="bins"  ;;
                    *) ois_warn "$_cl_file:$_cl_n: unknown section $_cl_l"; _cl_sec="skip" ;;
                esac
                continue ;;
        esac

        case "$_cl_sec" in
            deps)  OIS_DEPS_RAW="$OIS_DEPS_RAW$_cl_l
"; continue ;;
            depso) OIS_DEPS_OPT_RAW="$OIS_DEPS_OPT_RAW$_cl_l
"; continue ;;
            skip)  continue ;;
        esac

        case "$_cl_l" in
            *=*) ;;
            *) ois_warn "$_cl_file:$_cl_n: not a key=value line"; continue ;;
        esac
        _cl_k="$(ois_trim "${_cl_l%%=*}")"
        _cl_v="$(ois_trim "${_cl_l#*=}")"
        ois_is_ident "$_cl_k" || {
            ois_err "$_cl_file:$_cl_n: invalid key '$_cl_k'"; return 1; }

        # Fixed destinations only. Nothing here is computed or evaluated.
        case "$_cl_sec:$_cl_k" in
            main:app_name)     OIS_APP_NAME="$_cl_v" ;;
            main:display_name) OIS_APP_DISPLAY="$_cl_v" ;;
            main:binary)       OIS_APP_BINARY="$_cl_v" ;;
            main:github)       OIS_APP_GITHUB="$_cl_v" ;;
            main:prefix)       OIS_APP_PREFIX="$_cl_v" ;;
            main:update_mode)  OIS_APP_UPDATE_MODE="$_cl_v" ;;
            main:description)  OIS_APP_DESC="$_cl_v" ;;
            main:require_sudo) OIS_APP_REQUIRE_SUDO="$_cl_v" ;;
            main:channel)           OIS_APP_CHANNEL="$_cl_v" ;;
            main:next_best_version) OIS_NEXT_BEST_VERSION="$_cl_v" ;;
            main:signing_key)  OIS_SIGNING_KEY="$_cl_v" ;;
            svc:enable)        OIS_SVC_ENABLE="$_cl_v" ;;
            svc:args)          OIS_SVC_ARGS="$_cl_v" ;;
            svc:description)   OIS_SVC_DESC="$_cl_v" ;;
            svc:restart)       OIS_SVC_RESTART="$_cl_v" ;;
            svc:after)         OIS_SVC_AFTER="$_cl_v" ;;
            bins:*)            OIS_BINARIES_RAW="$OIS_BINARIES_RAW$_cl_k	$_cl_v
" ;;
            build:system)      OIS_BUILD_SYSTEM="$_cl_v" ;;
            build:out)         OIS_BUILD_OUT="$_cl_v" ;;
            build:custom)      OIS_BUILD_CUSTOM="$_cl_v" ;;
            build:jobs)        OIS_BUILD_JOBS="$_cl_v" ;;
            build:target)      OIS_BUILD_TARGET="$_cl_v" ;;
            build:cmake_opts)  OIS_BUILD_CMAKE_OPTS="$_cl_v" ;;
            build:make_opts)   OIS_BUILD_MAKE_OPTS="$_cl_v" ;;
            owns:config)       OIS_OWNS_CONFIG="$_cl_v" ;;
            owns:data)         OIS_OWNS_DATA="$_cl_v" ;;
            owns:cache)        OIS_OWNS_CACHE="$_cl_v" ;;
            owns:state)        OIS_OWNS_STATE="$_cl_v" ;;
            owns:extra)        OIS_OWNS_EXTRA="$OIS_OWNS_EXTRA$_cl_v
" ;;
            *) ois_warn "$_cl_file:$_cl_n: unknown key '$_cl_k' in [$_cl_sec]" ;;
        esac
    done < "$_cl_file"

    [ -n "$OIS_APP_NAME" ] || { ois_err "$_cl_file: app_name is required"; return 1; }
    ois_is_ident "$OIS_APP_NAME" || { ois_err "invalid app_name: $OIS_APP_NAME"; return 1; }
    [ -n "$OIS_APP_BINARY" ]  || OIS_APP_BINARY="$OIS_APP_NAME"
    [ -n "$OIS_APP_DISPLAY" ] || OIS_APP_DISPLAY="$OIS_APP_NAME"
    [ -n "$OIS_BUILD_OUT" ]   || OIS_BUILD_OUT="$OIS_APP_BINARY"
    ois_is_fname "$OIS_APP_BINARY" || {
        ois_err "invalid binary name '$OIS_APP_BINARY' (no slashes, spaces, or special characters)"; return 1; }
    ois_is_fname "$OIS_BUILD_OUT" || {
        ois_err "invalid [build] out '$OIS_BUILD_OUT' (no slashes, spaces, or special characters)"; return 1; }

    # Defaults for [owns] so a config that declares nothing still gets
    # correct XDG ownership and a usable claim allowlist.
    [ -n "$OIS_OWNS_CONFIG" ] || OIS_OWNS_CONFIG="$OIS_XDG_CONFIG/$OIS_APP_NAME"
    [ -n "$OIS_OWNS_DATA" ]   || OIS_OWNS_DATA="$OIS_XDG_DATA/$OIS_APP_NAME"
    [ -n "$OIS_OWNS_CACHE" ]  || OIS_OWNS_CACHE="$OIS_XDG_CACHE/$OIS_APP_NAME"
    [ -n "$OIS_OWNS_STATE" ]  || OIS_OWNS_STATE="$OIS_XDG_STATE/$OIS_APP_NAME"

    OIS_OWNS_CONFIG="$(ois_conf_expand "$OIS_OWNS_CONFIG")"
    OIS_OWNS_DATA="$(ois_conf_expand "$OIS_OWNS_DATA")"
    OIS_OWNS_CACHE="$(ois_conf_expand "$OIS_OWNS_CACHE")"
    OIS_OWNS_STATE="$(ois_conf_expand "$OIS_OWNS_STATE")"

    # Logical dependency names, one word each, derived from [deps] and
    # [deps.optional] lines ("ncurses", "openssl >= 3.0", "foo.apt = x").
    OIS_DEP_NAMES=""
    _cl_all="$OIS_DEPS_RAW$OIS_DEPS_OPT_RAW"
    while [ -n "$_cl_all" ]; do
        case "$_cl_all" in
            *"$OIS_NL"*) _cl_dl="${_cl_all%%"$OIS_NL"*}" ; _cl_all="${_cl_all#*"$OIS_NL"}" ;;
            *)           _cl_dl="$_cl_all" ; _cl_all="" ;;
        esac
        _cl_dl="$(ois_trim "$_cl_dl")" ; [ -z "$_cl_dl" ] && continue
        _cl_dn="${_cl_dl%%[ 	=.]*}"
        ois_is_ident "$_cl_dn" || continue
        case " $OIS_DEP_NAMES " in *" $_cl_dn "*) ;; *)
            OIS_DEP_NAMES="${OIS_DEP_NAMES:+$OIS_DEP_NAMES }$_cl_dn" ;; esac
    done

    export OIS_APP_NAME OIS_APP_DISPLAY OIS_APP_BINARY OIS_APP_GITHUB
    export OIS_APP_PREFIX OIS_APP_UPDATE_MODE OIS_APP_DESC OIS_APP_REQUIRE_SUDO
    export OIS_BUILD_SYSTEM OIS_BUILD_OUT OIS_BUILD_CUSTOM OIS_BUILD_JOBS
    export OIS_BUILD_TARGET OIS_BUILD_CMAKE_OPTS OIS_BUILD_MAKE_OPTS OIS_DEP_NAMES
    export OIS_NEXT_BEST_VERSION
    export OIS_OWNS_CONFIG OIS_OWNS_DATA OIS_OWNS_CACHE OIS_OWNS_STATE
    export OIS_OWNS_EXTRA OIS_DEPS_RAW OIS_DEPS_OPT_RAW
    export OIS_APP_CHANNEL OIS_SIGNING_KEY OIS_BINARIES_RAW
    export OIS_SVC_ENABLE OIS_SVC_ARGS OIS_SVC_DESC OIS_SVC_RESTART OIS_SVC_AFTER
    return 0
}

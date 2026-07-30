#!/bin/sh
# OIS v2 -- core/version.sh
# Version comparison done properly.
#
# v1 split into exactly three fields with ${r#*.}, so a missing patch
# component DUPLICATED the minor ("1.2" parsed as 1.2.2), a v-prefix
# made every numeric test fail silently, and "%%-*" stripped prerelease
# tags so 1.0.0-rc1 compared equal to 1.0.0. Net effect: users sat on
# stale versions forever behind a green "Up to date" checkmark.
#
# Rules here (semver-compatible, superset-tolerant):
#   - a single leading 'v' or 'V' is ignored
#   - numeric fields compare numerically, any count of fields,
#     missing fields are 0  (1.2 < 1.2.1, 1.0.0 < 1.0.0.1)
#   - a prerelease suffix (-rc1, -beta) sorts BEFORE its release
#     (1.0.0-rc1 < 1.0.0), and prereleases of the same core version
#     compare as plain strings (rc1 < rc2)
#   - build metadata (+sha) is ignored, per semver
# ---------------------------------------------------------------------

# Split "$1" -> V_CORE (dotted numerics) and V_PRE (prerelease or "").
_ois_ver_split() {
    _vs="$1"
    case "$_vs" in v*|V*) _vs="${_vs#?}" ;; esac
    _vs="${_vs%%+*}"
    case "$_vs" in
        *-*) V_CORE="${_vs%%-*}" ; V_PRE="${_vs#*-}" ;;
        *)   V_CORE="$_vs"       ; V_PRE="" ;;
    esac
}

# Next numeric field from $_vf_rest -> $_vf. Non-numeric junk in a
# field is treated as 0 rather than silently poisoning the compare.
_ois_ver_field() {
    case "$_vf_rest" in
        '')  _vf=0 ;;
        *.*) _vf="${_vf_rest%%.*}" ; _vf_rest="${_vf_rest#*.}" ;;
        *)   _vf="$_vf_rest"       ; _vf_rest="" ;;
    esac
    case "$_vf" in ''|*[!0-9]*) _vf=0 ;; esac
}

# ois_ver_cmp A B -> prints -1, 0, or 1 (A<B, A=B, A>B)
ois_ver_cmp() {
    _ois_ver_split "$1" ; _vc_ac="$V_CORE" ; _vc_ap="$V_PRE"
    _ois_ver_split "$2" ; _vc_bc="$V_CORE" ; _vc_bp="$V_PRE"

    _vc_ar="$_vc_ac" ; _vc_br="$_vc_bc"
    while [ -n "$_vc_ar" ] || [ -n "$_vc_br" ]; do
        _vf_rest="$_vc_ar" ; _ois_ver_field ; _vc_a="$_vf" ; _vc_ar="$_vf_rest"
        _vf_rest="$_vc_br" ; _ois_ver_field ; _vc_b="$_vf" ; _vc_br="$_vf_rest"
        [ "$_vc_a" -lt "$_vc_b" ] && { printf -- '-1'; return 0; }
        [ "$_vc_a" -gt "$_vc_b" ] && { printf '1';     return 0; }
    done

    # Equal cores: release > prerelease; two prereleases compare as strings.
    if [ -n "$_vc_ap" ] && [ -z "$_vc_bp" ]; then printf -- '-1'; return 0; fi
    if [ -z "$_vc_ap" ] && [ -n "$_vc_bp" ]; then printf '1';     return 0; fi
    if [ "$_vc_ap" = "$_vc_bp" ]; then printf '0'; return 0; fi
    # test's \< is undefined in POSIX sh; compare byte-by-byte instead.
    _ois_str_lt "$_vc_ap" "$_vc_bp" && { printf -- '-1'; return 0; }
    printf '1'
}

# Byte-wise "A < B" using only case globbing. Good enough for
# prerelease tags (rc1 < rc2, alpha < beta); no locale surprises.
_ois_str_lt() {
    _sl_a="$1" ; _sl_b="$2"
    while [ -n "$_sl_a" ] && [ -n "$_sl_b" ]; do
        _sl_ca="${_sl_a%"${_sl_a#?}"}" ; _sl_cb="${_sl_b%"${_sl_b#?}"}"
        if [ "$_sl_ca" != "$_sl_cb" ]; then
            case "$_sl_ca$_sl_cb" in
                *[!A-Za-z0-9.]*)  # fall back to sort order for oddballs
                    [ "$(printf '%s\n%s\n' "$_sl_ca" "$_sl_cb" | sort | head -n 1)" = "$_sl_ca" ]
                    return $? ;;
            esac
            _sl_rank="0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            _sl_pa="${_sl_rank%%"$_sl_ca"*}" ; _sl_pb="${_sl_rank%%"$_sl_cb"*}"
            [ "${#_sl_pa}" -lt "${#_sl_pb}" ]
            return $?
        fi
        _sl_a="${_sl_a#?}" ; _sl_b="${_sl_b#?}"
    done
    [ -z "$_sl_a" ] && [ -n "$_sl_b" ]
}

# ois_ver_older A B -> true if A is strictly older than B
ois_ver_older() { [ "$(ois_ver_cmp "$1" "$2")" = "-1" ]; }

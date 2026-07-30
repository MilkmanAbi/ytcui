#!/bin/sh
# OIS v2 -- core/fetch.sh
# Network access, latest-release discovery, and check throttling.
#
# v1's structural flaw: it fetched a VERSION file from a BRANCH, then
# cloned that branch's TIP -- so what it verified and what it installed
# were two different commits. And it hit raw.githubusercontent.com on
# every launch in notify mode; since May 2025 GitHub rate-limits
# unauthenticated raw fetches (HTTP 429), so frequently-launched apps
# were guaranteed to start failing.
#
# v2:
#   truth   = git tags. The tag is checked AND the tag is installed.
#   query   = releases.atom feed (not API-rate-limited, no auth),
#             fallback to the REST API (60/hr anon, 5000/hr with
#             $GITHUB_TOKEN).
#   volume  = TTL cache (default 24h) + persisted backoff on 429/5xx.
#   payload = release tarball, not a git clone. git is not needed.
#
# OIS_GITHUB_BASE / OIS_GITHUB_API exist so the whole pipeline is
# testable against a local mock server.
# ---------------------------------------------------------------------

OIS_GITHUB_BASE="${OIS_GITHUB_BASE:-https://github.com}"
OIS_GITHUB_API="${OIS_GITHUB_API:-https://api.github.com}"
OIS_FETCH_TIMEOUT="${OIS_FETCH_TIMEOUT:-15}"
OIS_UPDATE_TTL="${OIS_UPDATE_TTL:-86400}"

# -- Transport ---------------------------------------------------------
OIS_HTTP=""
ois_http_init() {
    [ -n "$OIS_HTTP" ] && return 0
    if   command -v curl >/dev/null 2>&1; then OIS_HTTP="curl"
    elif command -v wget >/dev/null 2>&1; then OIS_HTTP="wget"
    else return 1
    fi
}

# ois_fetch URL DEST  -- download to a file, atomically.
# Returns 22 specifically on HTTP 4xx/5xx so callers can distinguish
# "no such asset" from "no network".
ois_fetch() {
    ois_http_init || { ois_warn "neither curl nor wget found"; return 1; }
    _fe_url="$1" ; _fe_dest="$2"
    _fe_dir="${_fe_dest%/*}" ; [ "$_fe_dir" = "$_fe_dest" ] && _fe_dir="."
    _fe_tmp="$_fe_dir/.ois-dl.$$"
    _fe_auth=""
    case "$_fe_url" in
        "$OIS_GITHUB_API"/*) [ -n "${GITHUB_TOKEN:-}" ] && _fe_auth="Authorization: Bearer $GITHUB_TOKEN" ;;
    esac
    case "$OIS_HTTP" in
        curl)
            if [ -n "$_fe_auth" ]; then
                curl -fsSL --max-time "$OIS_FETCH_TIMEOUT" -H "$_fe_auth" \
                     -o "$_fe_tmp" "$_fe_url"
            else
                curl -fsSL --max-time "$OIS_FETCH_TIMEOUT" -o "$_fe_tmp" "$_fe_url"
            fi
            _fe_rc=$? ;;
        wget)
            if [ -n "$_fe_auth" ]; then
                wget -q -T "$OIS_FETCH_TIMEOUT" --header="$_fe_auth" \
                     -O "$_fe_tmp" "$_fe_url"
            else
                wget -q -T "$OIS_FETCH_TIMEOUT" -O "$_fe_tmp" "$_fe_url"
            fi
            _fe_rc=$?
            # wget uses 8 for server errors; normalise to curl's 22.
            [ "$_fe_rc" = 8 ] && _fe_rc=22 ;;
    esac
    if [ "$_fe_rc" != 0 ]; then
        rm -f "$_fe_tmp" 2>/dev/null || :
        return "$_fe_rc"
    fi
    mv -f "$_fe_tmp" "$_fe_dest" 2>/dev/null || { rm -f "$_fe_tmp"; return 1; }
    return 0
}

# -- Epoch time --------------------------------------------------------
# %s is not in POSIX date(1) but is universal in practice. If it is
# genuinely absent we return failure and callers treat the cache as
# expired -- the safe degradation is "check", never "skip forever".
ois_epoch() {
    _ep="$(date +%s 2>/dev/null)"
    case "$_ep" in ''|*[!0-9]*) return 1 ;; esac
    printf '%s' "$_ep"
}

# -- Check throttle ----------------------------------------------------
# True if a network check is due for this app. Explicit user commands
# pass force=1 and always check. Background/notify paths respect:
#   OIS_OFFLINE=1, CI, the TTL, and a persisted backoff deadline.
ois_check_due() {
    _cd_app="$1" ; _cd_force="${2:-0}"
    [ "${OIS_OFFLINE:-0}" = 1 ] && return 1
    [ "$_cd_force" = 1 ] && return 0
    [ "$OIS_IS_CI" = "yes" ] && return 1
    _cd_now="$(ois_epoch)" || return 0
    _cd_until="$(ois_meta_get "$_cd_app" backoff_until 2>/dev/null)" || _cd_until=0
    case "$_cd_until" in ''|*[!0-9]*) _cd_until=0 ;; esac
    [ "$_cd_now" -lt "$_cd_until" ] && return 1
    _cd_last="$(ois_meta_get "$_cd_app" last_check 2>/dev/null)" || _cd_last=0
    case "$_cd_last" in ''|*[!0-9]*) _cd_last=0 ;; esac
    [ $(( _cd_now - _cd_last )) -ge "$OIS_UPDATE_TTL" ]
}

ois_check_stamp() {
    _cs_now="$(ois_epoch)" || return 0
    ois_meta_set "$1" last_check "$_cs_now"
}

# Exponential backoff, persisted: 1h, 2h, 4h ... capped at 24h.
ois_check_backoff() {
    _cb_app="$1"
    _cb_now="$(ois_epoch)" || return 0
    _cb_n="$(ois_meta_get "$_cb_app" backoff_n 2>/dev/null)" || _cb_n=0
    case "$_cb_n" in ''|*[!0-9]*) _cb_n=0 ;; esac
    _cb_secs=3600 ; _cb_i=0
    while [ "$_cb_i" -lt "$_cb_n" ] && [ "$_cb_secs" -lt 86400 ]; do
        _cb_secs=$(( _cb_secs * 2 )) ; _cb_i=$(( _cb_i + 1 ))
    done
    [ "$_cb_secs" -gt 86400 ] && _cb_secs=86400
    ois_meta_setmany "$_cb_app" <<EOF
backoff_n=$(( _cb_n + 1 ))
backoff_until=$(( _cb_now + _cb_secs ))
EOF
    ois_dbg "backing off update checks for ${_cb_secs}s"
}

ois_check_backoff_clear() {
    ois_meta_setmany "$1" <<EOF
backoff_n=0
backoff_until=0
EOF
}

# -- Channel filtering -------------------------------------------------
# stable : no prerelease suffix               (v1.2.3)
# beta   : stable plus -beta/-rc/-pre tags    (v1.3.0-rc1)
# any    : every tag                          (nightlies included)
ois_channel_accepts() {
    _ca_ch="${1:-stable}" ; _ca_tag="$2"
    case "$_ca_ch" in any|nightly) return 0 ;; esac
    _ois_ver_split "$_ca_tag"
    if [ -z "$V_PRE" ]; then return 0; fi
    case "$_ca_ch" in
        beta) case "$V_PRE" in
                  beta*|rc*|pre*|alpha*) return 0 ;;
              esac ;;
    esac
    return 1
}

# -- Latest release discovery ------------------------------------------
# Primary: GET $BASE/OWNER/REPO/releases.atom -- entries carry
#   <id>tag:github.com,2008:Repository/NNN/TAG</id>
# The feed is newest-first BY DATE, which is not newest by VERSION: a
# v1.0.9 patch tagged after v2.0.0 appears first (this was a live bug in
# v2, which took the first entry). We therefore collect every tag,
# filter by channel, and take the maximum by version comparison.
# Fallback: GET $API/repos/OWNER/REPO/releases/latest -> "tag_name"
# (stable-ish only: GitHub excludes prereleases from /latest).
# ois_latest_tag REPO [CHANNEL]. Prints the tag.
# Return: 0 ok, 1 no transport/parse/none-matching, 22 http error.
ois_latest_tag() {
    _lt_repo="$1" ; _lt_ch="${2:-stable}"
    _lt_tmp="$(ois_tmpfile)" || return 1

    if ois_fetch "$OIS_GITHUB_BASE/$_lt_repo/releases.atom" "$_lt_tmp"; then
        _lt_tag=""
        while IFS= read -r _lt_l || [ -n "$_lt_l" ]; do
            case "$_lt_l" in
                *'<id>'*'Repository/'*'</id>'*)
                    _lt_t="${_lt_l#*<id>}" ; _lt_t="${_lt_t%%</id>*}"
                    _lt_t="${_lt_t##*/}"
                    [ -z "$_lt_t" ] && continue
                    ois_channel_accepts "$_lt_ch" "$_lt_t" || continue
                    if [ -z "$_lt_tag" ] || ois_ver_older "$_lt_tag" "$_lt_t"; then
                        _lt_tag="$_lt_t"
                    fi
                    ;;
            esac
        done < "$_lt_tmp"
        rm -f "$_lt_tmp"
        [ -n "$_lt_tag" ] && { printf '%s' "$_lt_tag"; return 0; }
        _lt_tmp="$(ois_tmpfile)" || return 1
    fi

    _lt_rc=0
    ois_fetch "$OIS_GITHUB_API/repos/$_lt_repo/releases/latest" "$_lt_tmp" || _lt_rc=$?
    if [ "$_lt_rc" != 0 ]; then rm -f "$_lt_tmp"; return "$_lt_rc"; fi
    _lt_tag=""
    while IFS= read -r _lt_l || [ -n "$_lt_l" ]; do
        case "$_lt_l" in
            *'"tag_name"'*)
                _lt_t="${_lt_l#*\"tag_name\"}" ; _lt_t="${_lt_t#*\"}"
                _lt_t="${_lt_t%%\"*}"
                [ -n "$_lt_t" ] && { _lt_tag="$_lt_t"; break; }
                ;;
        esac
    done < "$_lt_tmp"
    rm -f "$_lt_tmp"
    [ -n "$_lt_tag" ] && { printf '%s' "$_lt_tag"; return 0; }
    return 1
}

# -- Payload URLs ------------------------------------------------------
ois_url_source_tarball() { printf '%s/%s/archive/refs/tags/%s.tar.gz' "$OIS_GITHUB_BASE" "$1" "$2"; }
ois_url_asset()          { printf '%s/%s/releases/download/%s/%s'     "$OIS_GITHUB_BASE" "$1" "$2" "$3"; }

# -- Signature verification --------------------------------------------
# When ois.conf pins a signing_key (a minisign/signify public key), the
# release must ship SHA256SUMS.minisig and it must verify. This closes
# the "MITM swaps both the asset and the sums" hole for UPDATES: the key
# used is the one captured in the store at install time (trust on first
# use -- stated plainly in the docs).
# ois_sig_verify SUMS_FILE SIG_FILE KEY -> 0 verified, 1 bad, 2 no tool.
ois_sig_verify() {
    _gv_sums="$1" ; _gv_sig="$2" ; _gv_key="$3"
    if command -v minisign >/dev/null 2>&1; then
        minisign -Vm "$_gv_sums" -x "$_gv_sig" -P "$_gv_key" >/dev/null 2>&1 && return 0
        return 1
    fi
    if command -v signify >/dev/null 2>&1; then
        _gv_kf="$(ois_tmpfile)" || return 2
        printf 'untrusted comment: ois signing key
%s
' "$_gv_key" > "$_gv_kf"
        if signify -V -p "$_gv_kf" -x "$_gv_sig" -m "$_gv_sums" >/dev/null 2>&1; then
            rm -f "$_gv_kf" ; return 0
        fi
        rm -f "$_gv_kf" ; return 1
    fi
    return 2
}

# -- SHA256SUMS verification -------------------------------------------
# Verify FILE against a SHA256SUMS document, matched by basename.
# Returns: 0 verified, 1 MISMATCH (fatal), 2 no entry / no hash tool
# (caller decides; default policy is verify-if-present, warn-if-not).
ois_sums_verify() {
    _sv_file="$1" ; _sv_sums="$2"
    _sv_name="${_sv_file##*/}"
    ois_sha256_init
    [ -z "$OIS_SHA_CMD" ] && return 2
    _sv_want=""
    while IFS= read -r _sv_l || [ -n "$_sv_l" ]; do
        case "$_sv_l" in
            *"$_sv_name") _sv_want="${_sv_l%% *}" ; break ;;
        esac
    done < "$_sv_sums"
    [ -z "$_sv_want" ] && return 2
    _sv_got="$(ois_sha256 "$_sv_file")"
    [ "$_sv_got" = "$_sv_want" ] && return 0
    ois_err "sha256 mismatch for $_sv_name"
    ois_err "  expected $_sv_want"
    ois_err "  got      $_sv_got"
    return 1
}

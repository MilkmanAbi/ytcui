#!/bin/sh
# OIS v4 -- core/pm.sh
# Package-manager abstraction: a uniform interface over brew, port, apt,
# pacman, dnf, zypper, apk, xbps, emerge, pkg, pkgin, pkg_add, ips.
#
# Every package manager answers the same five questions, and OIS never
# has to special-case them at the call site:
#
#   ois_pm_have    NAME   -> 0 if the package is INSTALLED right now
#   ois_pm_known   NAME   -> 0 if the PM knows a package by that name
#   ois_pm_search  QUERY  -> prints candidate package names, one per line
#   ois_pm_do_install PKG... -> installs, handling privilege correctly
#   ois_pm_installed_version NAME -> prints version or 'unknown'
#
# The hard part this module exists to solve is PRIVILEGE, which differs
# fundamentally between the two macOS managers:
#
#   * Homebrew REFUSES to run as root ("Running Homebrew as root is
#     extremely dangerous and no longer supported"). If OIS itself was
#     launched under sudo (a --system install), every brew call must be
#     dropped back to the invoking user via `sudo -u $SUDO_USER`.
#   * MacPorts REQUIRES root for install/selfupdate but NOT for queries.
#     port installed / port echo / port search run fine unprivileged.
#
# Getting this wrong is the difference between "it just works" and a wall
# of "unknown user: brew" or "must be run as root" errors -- exactly the
# failure the user hit.
# ---------------------------------------------------------------------

# -- Who should own unprivileged package-manager operations? -----------
# If OIS is running as root but was invoked via sudo, SUDO_USER names the
# real user. brew must run as THAT user, never root.
OIS_REAL_USER=""
_ois_pm_real_user() {
    [ -n "$OIS_REAL_USER" ] && { printf '%s' "$OIS_REAL_USER"; return 0; }
    if [ "$OIS_IS_ROOT" = "yes" ] && [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
        OIS_REAL_USER="$SUDO_USER"
    else
        OIS_REAL_USER="$(id -un 2>/dev/null || printf '%s' "${USER:-}")"
    fi
    printf '%s' "$OIS_REAL_USER"
}

# -- Run a command as the real (non-root) user, for brew ---------------
# When we are root-via-sudo, brew must be de-escalated. `sudo -u user -H`
# sets HOME correctly so brew finds its config. When we are already the
# unprivileged user, just run it directly. When we are genuinely root
# (not via sudo -- e.g. a root shell) there is no safe user to drop to,
# so we surface a clear error rather than let brew reject us cryptically.
ois_brew() {
    if [ "$OIS_IS_ROOT" = "yes" ]; then
        _ob_u="$(_ois_pm_real_user)"
        if [ -z "$_ob_u" ] || [ "$_ob_u" = "root" ]; then
            ois_err "Homebrew cannot run as root and no invoking user was found."
            ois_err "Run OIS without sudo for a --user install, or set SUDO_USER."
            return 90
        fi
        # -H so HOME is the target user's; brew needs it for its prefix/config.
        sudo -u "$_ob_u" -H brew "$@"
    else
        brew "$@"
    fi
}

# -- Locate the brew binary even when not on PATH ----------------------
# After a fresh install, or under sudo with a scrubbed PATH, `brew` may
# not resolve. Fall back to the known prefixes. Sets OIS_BREW_BIN.
OIS_BREW_BIN=""
_ois_brew_locate() {
    [ -n "$OIS_BREW_BIN" ] && return 0
    if command -v brew >/dev/null 2>&1; then
        OIS_BREW_BIN="$(command -v brew)"; export OIS_BREW_BIN; return 0
    fi
    for _bl in "${OIS_BREW_PREFIX:-}/bin/brew" \
               /opt/homebrew/bin/brew /usr/local/bin/brew \
               /home/linuxbrew/.linuxbrew/bin/brew; do
        [ -x "$_bl" ] && { OIS_BREW_BIN="$_bl"; export OIS_BREW_BIN; return 0; }
    done
    return 1
}

# =====================================================================
# QUERY: is a package installed right now?
# =====================================================================
ois_pm_have() {
    _ph_pkg="$1"
    case "$OIS_PM" in
        brew)
            # brew list --versions is offline, fast, and immune to keg-only
            # linking status. It's the authoritative "is it installed" check.
            ois_brew list --versions "$_ph_pkg" >/dev/null 2>&1 ;;
        macports)
            # `port -q installed NAME` prints a line ending in "(active)"
            # when an active version exists. Queries need no privilege.
            port -q installed "$_ph_pkg" 2>/dev/null | grep -q '(active)' ;;
        apt)
            dpkg-query -W -f='${Status}' "$_ph_pkg" 2>/dev/null | grep -q 'install ok installed' ;;
        pacman)
            pacman -Qq "$_ph_pkg" >/dev/null 2>&1 ;;
        dnf|yum)
            rpm -q "$_ph_pkg" >/dev/null 2>&1 ;;
        zypper)
            rpm -q "$_ph_pkg" >/dev/null 2>&1 ;;
        apk)
            apk info -e "$_ph_pkg" >/dev/null 2>&1 ;;
        xbps)
            xbps-query "$_ph_pkg" >/dev/null 2>&1 ;;
        emerge)
            # qlist -I is exact; fall back to equery, then a portage dir glob.
            if command -v qlist >/dev/null 2>&1; then
                qlist -I "$_ph_pkg" 2>/dev/null | grep -q .
            elif command -v equery >/dev/null 2>&1; then
                equery -q list "$_ph_pkg" >/dev/null 2>&1
            else
                return 1
            fi ;;
        pkg)
            # FreeBSD: pkg info -e EXACTLY tests installed; exit 0 if present.
            pkg info -e "$_ph_pkg" >/dev/null 2>&1 ;;
        pkgin)
            # NetBSD: pkgin list shows installed; grep the stem.
            pkgin -p list 2>/dev/null | cut -d';' -f1 | grep -q "^$_ph_pkg-[0-9]" ;;
        pkg_add)
            # OpenBSD: pkg_info -e "pkgspec" -- augments exit by 2 if none match.
            # Use the stem form so any version/flavor satisfies it.
            pkg_info -e "$_ph_pkg->=0" >/dev/null 2>&1 || \
            pkg_info -e "$_ph_pkg-*"   >/dev/null 2>&1 ;;
        ips)
            pkg info "$_ph_pkg" >/dev/null 2>&1 ;;
        *)  return 1 ;;
    esac
}

# =====================================================================
# QUERY: does the PM know a package by this name (installable)?
# =====================================================================
ois_pm_known() {
    _pk_pkg="$1"
    case "$OIS_PM" in
        brew)
            # `brew info` hits the local formula cache; a known formula
            # exits 0. Suppress network by not passing --json (which forces
            # an API fetch). Fall back to search if info is inconclusive.
            ois_brew info "$_pk_pkg" >/dev/null 2>&1 ;;
        macports)
            # `port echo NAME` expands a portname; empty output => unknown.
            [ -n "$(port echo "$_pk_pkg" 2>/dev/null)" ] ;;
        apt)
            apt-cache show "$_pk_pkg" >/dev/null 2>&1 ;;
        pacman)
            pacman -Si "$_pk_pkg" >/dev/null 2>&1 ;;
        dnf|yum)
            "$OIS_PM" info "$_pk_pkg" >/dev/null 2>&1 ;;
        zypper)
            zypper --non-interactive info "$_pk_pkg" >/dev/null 2>&1 ;;
        apk)
            apk info "$_pk_pkg" >/dev/null 2>&1 || \
            apk search -e "$_pk_pkg" 2>/dev/null | grep -q . ;;
        xbps)
            xbps-query -R "$_pk_pkg" >/dev/null 2>&1 ;;
        emerge)
            emerge --search --quiet "$_pk_pkg" >/dev/null 2>&1 ;;
        pkg)
            pkg rquery '%n' "$_pk_pkg" >/dev/null 2>&1 ;;
        pkgin)
            pkgin -p avail 2>/dev/null | cut -d';' -f1 | grep -q "^$_pk_pkg-[0-9]" ;;
        pkg_add)
            pkg_info -Q "$_pk_pkg" 2>/dev/null | grep -q . ;;
        ips)
            pkg list -a "$_pk_pkg" >/dev/null 2>&1 ;;
        *)  return 1 ;;
    esac
}

# =====================================================================
# SEARCH: print candidate package names, one per line
# =====================================================================
ois_pm_search() {
    _ps_q="$1"
    case "$OIS_PM" in
        brew)
            # brew search prints matches; strip section headers and blanks.
            ois_brew search "$_ps_q" 2>/dev/null | grep -v '^==>' | grep -v '^[[:space:]]*$' ;;
        macports)
            # --line gives one tab-sep record per port; field 1 is the name.
            port search --name --line "$_ps_q" 2>/dev/null | cut -f1 ;;
        apt)
            apt-cache search "$_ps_q" 2>/dev/null | cut -d' ' -f1 ;;
        pacman)
            pacman -Ssq "$_ps_q" 2>/dev/null ;;
        dnf|yum)
            "$OIS_PM" search "$_ps_q" 2>/dev/null | grep ':' | cut -d' ' -f1 | grep -v '^=' ;;
        zypper)
            zypper --non-interactive search "$_ps_q" 2>/dev/null | awk -F'|' 'NR>2{gsub(/ /,"",$2);print $2}' ;;
        apk)
            apk search "$_ps_q" 2>/dev/null ;;
        xbps)
            xbps-query -Rs "$_ps_q" 2>/dev/null | awk '{print $2}' | sed 's/-[0-9].*$//' ;;
        emerge)
            emerge --search --quiet "$_ps_q" 2>/dev/null | grep '^\*' | awk '{print $2}' ;;
        pkg)
            pkg search -q "$_ps_q" 2>/dev/null | sed 's/-[0-9].*$//' ;;
        pkgin)
            pkgin search "$_ps_q" 2>/dev/null | cut -d' ' -f1 | sed 's/-[0-9].*$//' ;;
        pkg_add)
            pkg_info -Q "$_ps_q" 2>/dev/null | sed 's/-[0-9].*$//' ;;
        ips)
            pkg list -a "*$_ps_q*" 2>/dev/null | awk 'NR>1{print $1}' ;;
        *)  return 1 ;;
    esac
}

# =====================================================================
# INSTALL: run the install with correct privilege handling
# =====================================================================
# brew:      never sudo; drop to real user if we are root-via-sudo
# macports:  needs root for install (queries do not)
# apt/etc:   need root unless already root
ois_pm_do_install() {
    [ $# -gt 0 ] || return 1
    case "$OIS_PM" in
        brew)
            # NEVER prefix brew with sudo. ois_brew de-escalates as needed.
            # HOMEBREW_NO_AUTO_UPDATE speeds up and avoids surprise upgrades.
            HOMEBREW_NO_AUTO_UPDATE=1 ois_brew install "$@" ;;
        macports)
            # -N = non-interactive, -q = quiet. Needs privilege.
            if [ "$OIS_IS_ROOT" = "yes" ]; then
                port -N -q install "$@"
            elif [ "$OIS_SUDO" != "none" ]; then
                ois_priv port -N -q install "$@"
            else
                ois_err "MacPorts install needs root and no sudo/doas is available"
                return 1
            fi ;;
        *)
            # Linux/BSD: build the command, elevate if not root.
            _pi_cmd="$(ois_pm_install_cmd "$@")" || return 1
            if [ "$OIS_IS_ROOT" = "yes" ]; then
                sh -c "$_pi_cmd"
            elif [ "$OIS_SUDO" != "none" ]; then
                ois_priv sh -c "$_pi_cmd"
            else
                ois_err "package install needs root and no sudo/doas is available"
                ois_err "run manually: $_pi_cmd"
                return 1
            fi ;;
    esac
}

# =====================================================================
# INSTALLED VERSION
# =====================================================================
ois_pm_installed_version() {
    _piv_pkg="$1"
    case "$OIS_PM" in
        brew)     ois_brew list --versions "$_piv_pkg" 2>/dev/null | cut -d' ' -f2 ;;
        macports) port -q installed "$_piv_pkg" 2>/dev/null | ois_first_field | sed 's/^@//;s/_[0-9]*$//' ;;
        apt)      dpkg-query -W -f='${Version}' "$_piv_pkg" 2>/dev/null ;;
        pacman)   pacman -Q "$_piv_pkg" 2>/dev/null | cut -d' ' -f2 ;;
        dnf|yum)  rpm -q --qf '%{VERSION}' "$_piv_pkg" 2>/dev/null ;;
        zypper)   rpm -q --qf '%{VERSION}' "$_piv_pkg" 2>/dev/null ;;
        apk)      apk version "$_piv_pkg" 2>/dev/null | tail -n +2 | awk '{print $1}' | sed "s/^$_piv_pkg-//" ;;
        xbps)     xbps-query -p pkgver "$_piv_pkg" 2>/dev/null | sed "s/^$_piv_pkg-//" ;;
        pkg)      pkg query '%v' "$_piv_pkg" 2>/dev/null ;;
        pkgin)    pkgin -p list 2>/dev/null | grep "^$_piv_pkg-" | head -n1 | cut -d';' -f1 | sed "s/^$_piv_pkg-//" ;;
        pkg_add)  pkg_info -e "$_piv_pkg-*" 2>/dev/null | head -n1 | sed "s/^$_piv_pkg-//" ;;
        *)        printf 'unknown' ;;
    esac
}

# -- selfupdate / index refresh (best-effort, before a search) ---------
# Only called when a search is about to happen and the index may be stale.
ois_pm_refresh_index() {
    case "$OIS_PM" in
        brew)     ois_brew update >/dev/null 2>&1 || : ;;
        macports) [ "$OIS_IS_ROOT" = yes ] && port selfupdate >/dev/null 2>&1 || \
                  { [ "$OIS_SUDO" != none ] && ois_priv port selfupdate >/dev/null 2>&1; } || : ;;
        apt)      { [ "$OIS_IS_ROOT" = yes ] && apt-get update >/dev/null 2>&1; } || \
                  { [ "$OIS_SUDO" != none ] && ois_priv apt-get update >/dev/null 2>&1; } || : ;;
    esac
    return 0
}

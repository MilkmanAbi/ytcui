#!/bin/sh
# OIS v2 -- OneInstallSystem
# Pure POSIX sh. Hard dependency: sh + POSIX utilities.
# ---------------------------------------------------------------------

OIS_VERSION="4.0.0"
OIS_SELF_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || exit 1

# shellcheck source=core/utils.sh
. "$OIS_SELF_DIR/core/utils.sh"
# shellcheck source=core/system.sh
. "$OIS_SELF_DIR/core/system.sh"
# shellcheck source=core/store.sh
. "$OIS_SELF_DIR/core/store.sh"
# shellcheck source=core/conf.sh
. "$OIS_SELF_DIR/core/conf.sh"
# shellcheck source=core/version.sh
. "$OIS_SELF_DIR/core/version.sh"
# shellcheck source=core/fetch.sh
. "$OIS_SELF_DIR/core/fetch.sh"
# pm.sh before deps.sh: deps probing/install calls the PM abstraction.
# pm.sh after system.sh: it reads OIS_PM, OIS_IS_ROOT, OIS_SUDO, brew prefix.
# shellcheck source=core/pm.sh
. "$OIS_SELF_DIR/core/pm.sh"
# deps.sh before errors.sh: ois_need_tool consults the alias table.
# shellcheck source=core/deps.sh
. "$OIS_SELF_DIR/core/deps.sh"
# path.sh: shell PATH management (add on install, retract on uninstall).
# shellcheck source=core/path.sh
. "$OIS_SELF_DIR/core/path.sh"
# shellcheck source=core/errors.sh
. "$OIS_SELF_DIR/core/errors.sh"
# shellcheck source=core/json.sh
. "$OIS_SELF_DIR/core/json.sh"
# shellcheck source=core/build.sh
. "$OIS_SELF_DIR/core/build.sh"
# shellcheck source=core/hooks.sh
. "$OIS_SELF_DIR/core/hooks.sh"
# shellcheck source=core/service.sh
. "$OIS_SELF_DIR/core/service.sh"
# shellcheck source=core/update.sh
. "$OIS_SELF_DIR/core/update.sh"

# Top-level traps only. Set once, expanded at signal time.
OIS_SCRATCH=""
_ois_cleanup() {
    [ -n "$OIS_SCRATCH" ] && rm -rf "$OIS_SCRATCH" 2>/dev/null
    ois_lock_release
}
trap '_ois_cleanup' EXIT
trap '_ois_cleanup; exit 130' INT
trap '_ois_cleanup; exit 143' TERM

# -- Lockfile ----------------------------------------------------------
# ois.lock records the dependency versions RESOLVED AT INSTALL TIME.
# Deliberately scoped: OIS delegates to the system package manager, and
# apt/pacman/apk cannot generally install an arbitrary older version, so
# a lockfile here CANNOT promise reproduction the way Nix or Cargo can.
# What it can do -- and what it is for -- is detect drift: commit it,
# and `ois lock --check` tells you exactly which dependency moved
# between two machines or two dates. Honest scope beats a false promise.
ois_lock_write() {
    _lw_app="$1"
    [ -n "${OIS_CONF_SRC:-}" ] || return 0
    _lw_root="${OIS_CONF_SRC%/*}" ; _lw_root="${_lw_root%/ois}"
    _lw_f="$_lw_root/ois.lock"
    [ -w "$_lw_root" ] 2>/dev/null || return 0
    ois_deps_parse
    {
        printf '# ois.lock -- dependency versions resolved at install time.\n'
        printf '# Commit this. It detects drift; it does not pin versions\n'
        printf '# (system package managers cannot install arbitrary old ones).\n'
        printf '# Regenerate: ois lock    Compare: ois lock --check\n'
        printf 'ois_version\t%s\n' "$OIS_VERSION"
        printf 'platform\t%s/%s\t%s\n' "$OIS_OS" "$OIS_ARCH" "$OIS_PM"
        for _lw_n in $(ois_dep_names); do
            _lw_v="$(ois_dep_version "$_lw_n")" || _lw_v="unknown"
            printf 'dep\t%s\t%s\t%s\n' "$_lw_n" "$(ois_dep_package "$_lw_n")" "$_lw_v"
        done
    } | ois_write_atomic "$_lw_f" 644 || return 0
    ois_dbg "wrote $_lw_f"
    return 0
}

ois_lock_check() {
    _lc_f="${1:-./ois.lock}"
    [ -r "$_lc_f" ] || { ois_fail E-STATE "no lockfile at $_lc_f" "" \
        "generate one: ois lock"; return 1; }
    ois_deps_parse
    _lc_drift=0
    while IFS="$OIS_TAB" read -r _lc_k _lc_a _lc_b _lc_c || [ -n "$_lc_k" ]; do
        case "$_lc_k" in
            dep) ;;
            platform)
                [ "$_lc_a" = "$OIS_OS/$OIS_ARCH" ] || \
                    ois_info "platform differs: locked $_lc_a, here $OIS_OS/$OIS_ARCH"
                continue ;;
            *) continue ;;
        esac
        _lc_now="$(ois_dep_version "$_lc_a")" || _lc_now="unknown"
        if [ "$_lc_now" = "$_lc_c" ]; then
            ois_dbg "same  $_lc_a $_lc_c"
        else
            ois_warn "drift: $_lc_a  locked=$_lc_c  installed=$_lc_now"
            _lc_drift=$(( _lc_drift + 1 ))
        fi
    done < "$_lc_f"
    if [ "$_lc_drift" = 0 ]; then
        ois_ok "no dependency drift against $_lc_f"
        return 0
    fi
    ois_warn "$_lc_drift dependency/dependencies differ from the lockfile"
    return 1
}

# -- Nix detection -----------------------------------------------------
# On NixOS the filesystem is managed declaratively: /usr/local/bin does
# not exist and ~/.local/bin is outside the module system, so an OIS
# install is at best invisible to the system and at worst misleading.
# Detect it, explain it, and offer a flake fragment instead of failing
# silently. Non-NixOS machines that merely have the nix package manager
# get a warning, not a refusal -- ~/.local/bin works fine there.
ois_nix_detect() {
    [ -e /etc/NIXOS ] && { printf 'nixos'; return 0; }
    [ -n "${NIX_STORE:-}" ] && { printf 'nix-shell'; return 0; }
    command -v nix-env >/dev/null 2>&1 && { printf 'nix'; return 0; }
    printf 'none'
}

# shellcheck disable=SC2016  # ${system} is Nix syntax, must stay literal
_ois_nix_flake_fragment() {
    printf '\n  A flake fragment for this project:\n\n'
    printf '    # flake.nix\n'
    printf '    {\n'
    printf '      outputs = { self, nixpkgs }: let\n'
    printf '        system = "%s-linux";\n' "$OIS_ARCH"
    printf '        pkgs = nixpkgs.legacyPackages.${system};\n'
    printf '      in {\n'
    printf '        packages.${system}.default = pkgs.stdenv.mkDerivation {\n'
    printf '          pname = "%s";\n' "${OIS_APP_NAME:-myapp}"
    printf '          version = "%s";\n' "${1:-0.1.0}"
    printf '          src = ./.;\n'
    printf '          nativeBuildInputs = with pkgs; [ %s ];\n' \
        "$(case "$OIS_BUILD_SYSTEM" in cmake) printf 'cmake' ;; meson) printf 'meson ninja' ;; cargo) printf 'cargo rustc' ;; go) printf 'go' ;; *) printf 'gnumake' ;; esac)"
    printf '          buildInputs = with pkgs; [ %s ];\n' "${OIS_DEP_NAMES:-}"
    printf '        };\n      };\n    }\n\n'
}

ois_nix_guard() {
    _ng="$(ois_nix_detect)"
    case "$_ng" in
        nixos)
            ois_fail E-NIX "this is NixOS -- OIS cannot manage software here" \
                "NixOS builds its filesystem declaratively; imperative installs into /usr/local or ~/.local are not tracked by the system and will not survive a rebuild" \
                "add this project to your configuration.nix or a flake instead" \
                "to override anyway (it will work, but Nix will not know about it): OIS_ALLOW_NIX=1"
            _ois_nix_flake_fragment "${1:-}"
            return 1 ;;
        nix-shell)
            ois_warn "running inside a nix shell -- installs land outside the store and vanish with it" ;;
        nix)
            ois_dbg "nix package manager present; user-scope install is still fine" ;;
    esac
    return 0
}

# -- Scope -------------------------------------------------------------
_resolve_scope() {
    case "${1:-}" in
        user)   OIS_SCOPE="user" ;;
        system) OIS_SCOPE="system" ;;
        *)
            if [ "$OIS_IS_ROOT" = "yes" ]; then OIS_SCOPE="system"
            else OIS_SCOPE="user"
            fi ;;
    esac
    if [ -z "${OIS_PREFIX:-}" ]; then
        if [ "$OIS_SCOPE" = "system" ]; then OIS_PREFIX="/usr/local"
        else OIS_PREFIX="$OIS_HOME/.local"
        fi
    fi
    export OIS_SCOPE OIS_PREFIX
}

_elevate_if_needed() {
    [ "$OIS_SCOPE" = "system" ] || return 0
    [ "$OIS_IS_ROOT" = "yes" ] && return 0
    [ "$OIS_SUDO" = "none" ] && ois_fail_die E-PERM \
        "system scope needs sudo or doas" \
        "neither is available on this machine" \
        "use --user to install under $OIS_HOME/.local instead"
    ois_info "escalating with $OIS_SUDO"
    ois_lock_release
    case "$OIS_SUDO" in
        sudo) exec sudo -- sh "$0" "$@" ;;
        doas) exec doas    sh "$0" "$@" ;;
    esac
}

# -- Global `ois` shim -------------------------------------------------
# One shim per prefix, store-level infrastructure -- deliberately in NO
# app's manifest (the v1 shared-runtime-in-every-manifest mistake).
# Removed by uninstall/gc only when the last app is gone.
_ois_shim_write() {
    _sw="$OIS_PREFIX/bin/ois"
    printf '#!/bin/sh\nexec sh "%s/runtime/current/ois.sh" "$@"\n' \
        "$(ois_store_root)" | ois_write_atomic "$_sw" 755 || return 1
    printf '%s\n' "$_sw" | ois_write_atomic "$(ois_store_root)/shim" 644
    ois_dbg "shim: $_sw"
}

_ois_shim_gc() {
    _sg_any=0
    for _sg_a in $(ois_app_list); do _sg_any=1; break; done
    [ "$_sg_any" = 1 ] && return 0
    _sg_f="$(ois_store_root)/shim"
    [ -r "$_sg_f" ] || return 0
    read -r _sg_p < "$_sg_f" 2>/dev/null || return 0
    [ -n "$_sg_p" ] && [ -f "$_sg_p" ] && ois_rm "$_sg_p" && \
        ois_ok "removed ois shim (no apps remain)"
    ois_rm "$_sg_f"
}

# -- Config resolution -------------------------------------------------
# True when this ois.sh is the store's installed runtime rather than a
# copy sitting inside somebody's project.
_ois_self_is_runtime() {
    case "$OIS_SELF_DIR" in
        "$(ois_runtime_dir)"/*) return 0 ;;
        *) return 1 ;;
    esac
}

# Returns 0 when a config was loaded, 1 when this directory simply has
# none. A config that EXISTS but is INVALID is fatal here and never
# falls through to the next candidate directory -- otherwise a typo in
# your ois.conf silently installs whatever unrelated ois.conf happens to
# sit in $PWD.
_load_conf_from_dir() {
    for _lc in "$1/ois/ois.conf" "$1/ois.conf"; do
        [ -r "$_lc" ] || continue
        ois_conf_load "$_lc" || ois_fail_die E-CONF \
            "$_lc is invalid" \
            "the file exists but could not be parsed -- see the message above" \
            "fix it, or delete it if this is not the project you meant to install"
        OIS_CONF_SRC="$_lc"
        return 0
    done
    return 1
}

# Synthesize a minimal config for a repo that ships none. This is what
# makes `ois install user/repo` work on ARBITRARY GitHub projects.
_synth_conf() {
    _sy_repo="$1" ; _sy_dir="$2"
    _sy_name="${_sy_repo##*/}"
    _sy_name="$(printf '%s' "$_sy_name" | tr -cd 'A-Za-z0-9_-')"
    ois_is_ident "$_sy_name" || return 1
    ois_warn "repo ships no ois.conf -- synthesizing one (app: $_sy_name)"
    printf 'app_name = %s\nbinary = %s\ngithub = %s\n' \
        "$_sy_name" "$_sy_name" "$_sy_repo" > "$_sy_dir/.ois-synth.conf"
    ois_conf_load "$_sy_dir/.ois-synth.conf" || return 1
    OIS_CONF_SRC="$_sy_dir/.ois-synth.conf"
    return 0
}

# -- install (shared tail: build tree already on disk) ------------------
_install_from_tree() {
    _it_root="$1"
    _it_root="$(cd "$_it_root" && pwd)" || {
        ois_fail E-STATE "project root vanished: $1"; return 1; }

    ois_hdr "$OIS_APP_DISPLAY" "install  ois $OIS_VERSION  $OIS_OS/$OIS_ARCH  scope=$OIS_SCOPE"

    _it_ver_hint="unknown"
    if [ -r "$_it_root/VERSION" ]; then read -r _it_ver_hint < "$_it_root/VERSION" || :; fi

    # NixOS refuses BEFORE any store mutation or prompt: the answer is
    # "not here" regardless of whether the app is already recorded.
    if [ "${OIS_ALLOW_NIX:-0}" != 1 ]; then
        ois_nix_guard "$_it_ver_hint" || return 1
    fi

    _it_bindir="$OIS_PREFIX/bin"
    _it_dest="$_it_bindir/$OIS_APP_BINARY"

    if ois_app_exists "$OIS_APP_NAME"; then
        case "$(ois_state_get "$OIS_APP_NAME")" in
            ok) ois_warn "$OIS_APP_NAME $(ois_meta_get "$OIS_APP_NAME" version) is already installed"
                # Defaults to yes: running `install` again is an explicit
                # request to reinstall, and doing so is non-destructive
                # (user data is untouched), so --yes proceeds. Contrast
                # the purge prompt, which defaults to no.
                ois_ask "reinstall?" y || { ois_info "nothing to do"; return 0; } ;;
            *)  ois_warn "previous install of $OIS_APP_NAME did not complete -- redoing cleanly" ;;
        esac
    fi

    # Record intent FIRST: a crash from here on is detectable. If this
    # is a REINSTALL over a healthy app and the build fails, the old
    # binary is still on disk and untouched -- so on failure we restore
    # state=ok rather than leaving a healthy install marked partial
    # (which doctor would then offer to delete).
    _it_had_ok=0
    [ "$(ois_state_get "$OIS_APP_NAME" 2>/dev/null)" = "ok" ] && _it_had_ok=1
    ois_app_create "$OIS_APP_NAME" || return 1
    ois_state_begin "$OIS_APP_NAME" installing
    _it_undo() {
        [ "$_it_had_ok" = 1 ] && ois_state_ok "$OIS_APP_NAME"
        return 1
    }
    ois_write_atomic "$(ois_app_dir "$OIS_APP_NAME")/conf" 644 < "$OIS_CONF_SRC" || { _it_undo; return 1; }

    ois_deps_check || { _it_undo; return 1; }
    ois_lock_write "$OIS_APP_NAME"

    if ! ois_hook_run "$OIS_APP_NAME" pre-install "" "$_it_ver_hint" \
                      "$_it_root/ois/hooks"; then
        _it_undo; return 1
    fi

    _it_log="$(ois_app_dir "$OIS_APP_NAME")/build.log"
    ois_info "building in $_it_root  (system: auto)"
    _it_cwd="$(pwd)"
    cd "$_it_root" || { _it_undo; return 1; }
    ois_build_detect || { cd "$_it_cwd" || :; _it_undo; return 1; }
    ois_build_run "$_it_log" || { cd "$_it_cwd" || :; _it_undo; return 1; }
    cd "$_it_cwd" || return 1
    _it_built="$_it_root/${OIS_BUILT#./}"
    ois_ok "built $_it_built"

    ois_install_file "$_it_built" "$_it_dest" 755 || { _it_undo; return 1; }
    ois_ok "installed $_it_dest"

    # [binaries] -- extra executables from the same build/config.
    # name = path-relative-to-build-root
    if [ -n "$OIS_BINARIES_RAW" ]; then
        _it_bins="$OIS_BINARIES_RAW"
        while [ -n "$_it_bins" ]; do
            case "$_it_bins" in
                *"$OIS_NL"*) _it_bl="${_it_bins%%"$OIS_NL"*}" ; _it_bins="${_it_bins#*"$OIS_NL"}" ;;
                *)           _it_bl="$_it_bins" ; _it_bins="" ;;
            esac
            [ -z "$_it_bl" ] && continue
            _it_bn="${_it_bl%%	*}" ; _it_bp="${_it_bl#*	}"
            ois_is_fname "$_it_bn" || { ois_warn "invalid binary name: $_it_bn"; continue; }
            # The path is relative to the build tree and must stay inside
            # it: reject absolute paths and any '..' traversal, so a
            # hostile repo cannot point [binaries] at /etc or climb out.
            case "$_it_bp" in
                /*|*..*|'') ois_warn "invalid binary path (absolute or traversal): $_it_bp"; continue ;;
            esac
            _it_bsrc=""
            for _it_c in "$_it_root/$_it_bp" "$_it_root/.ois-build/$_it_bp" \
                         "$_it_root/build/$_it_bp" "$_it_root/target/release/$_it_bp"; do
                [ -f "$_it_c" ] && [ -x "$_it_c" ] && { _it_bsrc="$_it_c"; break; }
            done
            if [ -z "$_it_bsrc" ]; then
                ois_fail E-BUILD "extra binary '$_it_bn' not found at '$_it_bp'" \
                    "[binaries] declares it but the build produced nothing executable there" \
                    "paths are relative to the project root or the build dir"
                _it_undo; return 1
            fi
            ois_install_file "$_it_bsrc" "$_it_bindir/$_it_bn" 755 || { _it_undo; return 1; }
            ois_manifest_add "$OIS_APP_NAME" file "$_it_bindir/$_it_bn" purge
            ois_ok "installed $_it_bindir/$_it_bn"
        done
    fi

    ois_runtime_install "$OIS_VERSION" "$OIS_SELF_DIR" || return 1
    ois_runtime_ref_add "$OIS_VERSION" "$OIS_APP_NAME"
    _ois_shim_write || :

    _it_hook="$_it_bindir/.$OIS_APP_BINARY-ois"
    printf '#!/bin/sh\nexec sh "%s/runtime/%s/ois.sh" --app %s "$@"\n' \
        "$(ois_store_root)" "$OIS_VERSION" "$OIS_APP_NAME" | \
        ois_write_atomic "$_it_hook" 755 || return 1
    ois_ok "hook $_it_hook"

    _it_ver="unknown"
    if [ -r "$_it_root/VERSION" ]; then read -r _it_ver < "$_it_root/VERSION" || :; fi
    [ "$_it_ver" = "unknown" ] && [ -n "${OIS_INSTALL_TAG:-}" ] && {
        _it_ver="$OIS_INSTALL_TAG"
        case "$_it_ver" in v*|V*) _it_ver="${_it_ver#?}" ;; esac
    }

    ois_meta_setmany "$OIS_APP_NAME" <<META
version=$_it_ver
binary=$_it_dest
hook=$_it_hook
scope=$OIS_SCOPE
prefix=$OIS_PREFIX
runtime=$OIS_VERSION
github=$OIS_APP_GITHUB
update_mode=$OIS_APP_UPDATE_MODE
channel=$OIS_APP_CHANNEL
signing_key=$OIS_SIGNING_KEY
config_dir=$OIS_OWNS_CONFIG
data_dir=$OIS_OWNS_DATA
cache_dir=$OIS_OWNS_CACHE
state_dir=$OIS_OWNS_STATE
installed_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)
source_root=$_it_root
META

    ois_manifest_add "$OIS_APP_NAME" file "$_it_dest" purge
    ois_manifest_add "$OIS_APP_NAME" file "$_it_hook" purge

    ois_allow_add "$OIS_APP_NAME" "$OIS_OWNS_CONFIG"
    ois_allow_add "$OIS_APP_NAME" "$OIS_OWNS_DATA"
    ois_allow_add "$OIS_APP_NAME" "$OIS_OWNS_CACHE"
    ois_allow_add "$OIS_APP_NAME" "$OIS_OWNS_STATE"
    ois_allow_add "$OIS_APP_NAME" "$_it_bindir"
    if [ -n "$OIS_OWNS_EXTRA" ]; then
        printf '%s' "$OIS_OWNS_EXTRA" | while IFS= read -r _it_x; do
            [ -n "$_it_x" ] && ois_allow_add "$OIS_APP_NAME" "$(ois_conf_expand "$_it_x")"
        done
    fi
    ois_manifest_add "$OIS_APP_NAME" dir "$OIS_OWNS_CONFIG" keep
    ois_manifest_add "$OIS_APP_NAME" dir "$OIS_OWNS_DATA"   keep
    ois_manifest_add "$OIS_APP_NAME" dir "$OIS_OWNS_CACHE"  purge
    ois_manifest_add "$OIS_APP_NAME" dir "$OIS_OWNS_STATE"  keep

    ois_hooks_capture "$OIS_APP_NAME" "$_it_root" || \
        ois_warn "could not capture hooks into the store"
    ois_env_write "$OIS_APP_NAME"

    case "$OIS_SVC_ENABLE" in
        true|yes|1)
            ois_service_install "$OIS_APP_NAME" || { _it_undo; return 1; } ;;
    esac

    ois_history_add "$OIS_APP_NAME" install "$_it_ver"
    ois_state_ok "$OIS_APP_NAME"

    ois_hook_run "$OIS_APP_NAME" post-install "" "$_it_ver" || return 1

    printf '\n'
    ois_ok "$OIS_APP_DISPLAY $_it_ver installed"
    printf '    %-30s %s\n' "$OIS_APP_BINARY"             "run it"
    printf '    %-30s %s\n' "$OIS_APP_BINARY --ois"       "manage"
    printf '    %-30s %s\n' "$OIS_APP_BINARY --update"    "update"
    printf '    %-30s %s\n' "$OIS_APP_BINARY --uninstall" "remove"
    case ":$PATH:" in
        *":$_it_bindir:"*) ;;
        *)
            # macOS especially: ~/.local/bin is not on PATH by default in
            # zsh/bash, so a perfectly-installed binary "isn't found".
            # Actively add it to the user's shell startup (user scope only;
            # system prefixes are already global). Best-effort, never fatal.
            if [ "$OIS_SCOPE" = "user" ]; then
                ois_path_ensure "$_it_bindir"
            else
                printf '\n'; ois_warn "$_it_bindir is not on PATH"
                # shellcheck disable=SC2016
                printf '    export PATH=\"$PATH:%s\"\n' "$_it_bindir"
            fi ;;
    esac
    printf '\n'
}

# -- install: local project --------------------------------------------
cmd_install_local() {
    ois_lock_acquire 30 || exit 1
    ois_store_init || exit 1
    _cl_root="${1:-}"
    if [ -n "$_cl_root" ]; then
        _load_conf_from_dir "$_cl_root" || ois_fail_die E-CONF \
            "no ois.conf under $_cl_root" \
            "looked for $_cl_root/ois/ois.conf and $_cl_root/ois.conf" \
            "create one -- only app_name is required"
    else
        # Search beside ois.sh and one level up -- that is a project
        # checkout. Fall back to the current directory ONLY when we were
        # NOT invoked through the global shim: the shim lives in the
        # store's runtime and has no project scope, so letting it pick up
        # whatever ois.conf happens to be in $PWD silently installs the
        # wrong app.
        for _cl_c in "$OIS_SELF_DIR" "$OIS_SELF_DIR/.."; do
            _load_conf_from_dir "$_cl_c" && break
        done
        if [ -z "${OIS_CONF_SRC:-}" ] && ! _ois_self_is_runtime; then
            _load_conf_from_dir "." || :
        fi
        [ -n "${OIS_CONF_SRC:-}" ] || ois_fail_die E-CONF \
            "no ois.conf found" \
            "looked beside ois.sh and one level up" \
            "cd into the project and run: sh install.sh" \
            "or name it explicitly: ois install /path/to/project" \
            "or install from GitHub: ois install user/repo"
        _cl_root="${OIS_CONF_SRC%/*}" ; _cl_root="${_cl_root%/ois}"
    fi
    _elevate_if_needed install "$@"
    _install_from_tree "$_cl_root" || exit 1
}

# -- install: straight from a GitHub repo ------------------------------
# `ois install user/repo [--tag TAG]` -- latest release tag, source
# tarball, ois.conf from the tree or synthesized. No git required.
cmd_install_remote() {
    _cr_repo="$1" ; _cr_tag="${2:-}"
    _cr_repo="${_cr_repo#https://github.com/}"
    _cr_repo="${_cr_repo#github.com/}"
    _cr_repo="${_cr_repo%.git}" ; _cr_repo="${_cr_repo%/}"
    case "$_cr_repo" in
        */*) ;;
        *) ois_fail_die E-CONF "not a repo reference: $1" "" "use the form user/repo" ;;
    esac

    ois_lock_acquire 60 || exit 1
    ois_store_init || exit 1
    ois_hdr "$_cr_repo" "remote install  ois $OIS_VERSION"

    if [ -z "$_cr_tag" ]; then
        ois_info "resolving latest release..."
        _cr_rc=0
        _cr_tag="$(ois_retry 3 2 ois_latest_tag "$_cr_repo")" || _cr_rc=$?
        if [ "$_cr_rc" != 0 ]; then
            if [ "$_cr_rc" = 22 ]; then
                ois_fail_die E-HTTP "no releases found for $_cr_repo" \
                    "the repository exists but has no published release, or is private" \
                    "publish a release/tag on GitHub, then retry" \
                    "or pass an explicit tag: ois install $_cr_repo --tag <tag>"
            fi
            ois_fail_die E-NET "cannot reach GitHub" \
                "network failure after 3 attempts" \
                "check connectivity, or set OIS_OFFLINE=1 to silence background checks" \
                "if you are rate-limited, export GITHUB_TOKEN=<token>"
        fi
    fi
    ois_ok "release: $_cr_tag"

    OIS_SCRATCH="$(ois_tmpdir)" || ois_fail_die E-STORE "no scratch space" "" \
        "check free space in ${TMPDIR:-/tmp}"
    _cr_tb="$OIS_SCRATCH/src.tar.gz"
    ois_info "downloading source tarball..."
    _cr_rc=0
    ois_retry 3 2 ois_fetch "$(ois_url_source_tarball "$_cr_repo" "$_cr_tag")" "$_cr_tb" \
        || _cr_rc=$?
    [ "$_cr_rc" != 0 ] && ois_fail_die E-HTTP "could not download $_cr_tag tarball" \
        "GitHub returned an error for archive/refs/tags/$_cr_tag.tar.gz" \
        "verify the tag exists: https://github.com/$_cr_repo/tags"

    ois_need_tool tar || exit 1
    mkdir -p "$OIS_SCRATCH/src"
    ( cd "$OIS_SCRATCH/src" && \
      { tar -xzf "$_cr_tb" 2>/dev/null || gzip -dc "$_cr_tb" | tar -xf -; } ) || \
        ois_fail_die E-STORE "source extraction failed" \
            "the downloaded tarball is corrupt or tar/gzip are limited here" \
            "retry the install; transfers are re-fetched from scratch"

    _cr_root=""
    for _cr_d in "$OIS_SCRATCH/src"/*/; do
        [ -d "$_cr_d" ] && { _cr_root="${_cr_d%/}"; break; }
    done
    [ -n "$_cr_root" ] || ois_fail_die E-STORE "empty source tarball" "" \
        "report this to the project -- their release archive has no content"

    if ! _load_conf_from_dir "$_cr_root"; then
        _synth_conf "$_cr_repo" "$_cr_root" || ois_fail_die E-CONF \
            "cannot derive an app name from $_cr_repo" "" \
            "clone the repo and add an ois.conf with app_name ="
    fi
    [ -n "$OIS_APP_GITHUB" ] || OIS_APP_GITHUB="$_cr_repo"
    OIS_INSTALL_TAG="$_cr_tag"

    _elevate_if_needed install "$_cr_repo" --tag "$_cr_tag"
    _install_from_tree "$_cr_root" || exit 1
}

# -- uninstall ---------------------------------------------------------
cmd_uninstall() {
    _un_app="$1" ; _un_purge="${2:-0}"
    ois_lock_acquire 30 || exit 1
    ois_app_exists "$_un_app" || ois_fail_die E-STATE "$_un_app is not installed" "" \
        "ois list shows what is"
    ois_hdr "$_un_app" "uninstall  ois $OIS_VERSION"

    ois_claims_fold "$_un_app"

    if ! ois_hook_run "$_un_app" pre-uninstall "$(ois_meta_get "$_un_app" version)" ""; then
        ois_err "pre-uninstall hook failed -- nothing was removed"
        return 1
    fi
    ois_service_remove "$_un_app"

    if [ "$_un_purge" != 1 ]; then
        ois_ask "also delete config and saved data?" n && _un_purge=1
    fi

    _un_mf="$(ois_manifest_file "$_un_app")"
    while IFS="$OIS_TAB" read -r _un_t _un_p _un_s _un_pol _un_o || [ -n "$_un_t" ]; do
        [ -z "$_un_p" ] && continue
        if [ "$_un_pol" = "keep" ] && [ "$_un_purge" != 1 ]; then
            if [ -e "$_un_p" ]; then ois_info "kept    $_un_p"; fi
            continue
        fi
        [ -e "$_un_p" ] || continue
        case "$_un_t" in
            dir) ois_rmtree "$_un_p" && ois_ok "removed $_un_p" ;;
            *)   ois_rm     "$_un_p" && ois_ok "removed $_un_p" ;;
        esac
    done < "$_un_mf"

    _un_rt="$(ois_meta_get "$_un_app" runtime || printf '%s' "$OIS_VERSION")"
    # Capture the bindir BEFORE the store record is destroyed, so we can
    # decide whether to retract PATH once the app's files are gone.
    _un_scope="$(ois_meta_get "$_un_app" scope || printf 'user')"
    _un_prefix="$(ois_meta_get "$_un_app" prefix || printf '')"
    # post-uninstall runs while the store record still exists, so the
    # hook can still read its own metadata.
    ois_hook_run "$_un_app" post-uninstall "$(ois_meta_get "$_un_app" version)" "" || :
    ois_runtime_ref_del "$_un_rt" "$_un_app"

    # Retract the PATH entry we added on install -- but only for user
    # scope, and only if no OTHER installed app still keeps a binary in
    # that bindir (refcounted, so uninstalling one of several tools that
    # share ~/.local/bin does not break the others). Check while the
    # store still has this app, excluding it explicitly.
    if [ "$_un_scope" = "user" ] && [ -n "$_un_prefix" ]; then
        _un_bindir="$_un_prefix/bin"
        if ! ois_path_bindir_in_use "$_un_bindir" "$_un_app"; then
            ois_path_retract "$_un_bindir"
        else
            ois_dbg "path: $_un_bindir still used by another app; keeping PATH entry"
        fi
    fi

    ois_app_destroy "$_un_app"
    ois_runtime_gc >/dev/null
    _ois_shim_gc
    printf '\n' ; ois_ok "$_un_app uninstalled" ; printf '\n'
}

# -- update / check / rollback -----------------------------------------
cmd_update() {
    _cu_app="$1" ; _cu_to="${2:-}"
    ois_lock_acquire 30 || exit 1
    ois_app_exists "$_cu_app" || ois_fail_die E-STATE "$_cu_app is not installed"
    ois_hdr "$_cu_app" "update  ois $OIS_VERSION"
    ois_claims_fold "$_cu_app"
    ois_update_run "$_cu_app" "$_cu_to"
}

cmd_check() {
    _ck_app="$1"
    ois_app_exists "$_ck_app" || ois_fail_die E-STATE "$_ck_app is not installed"
    ois_update_check "$_ck_app" 1
    _ck_rc=$?
    case $_ck_rc in
        0) _ck_s="update-available" ;;
        1) _ck_s="up-to-date" ;;
        *) _ck_s="unknown" ;;
    esac
    if [ "$OIS_JSON" = 1 ]; then
        ois_json_check "$_ck_app" "$_ck_s" "${OIS_UPD_LOCAL:-}" "${OIS_UPD_REMOTE:-}"
    else
        case $_ck_rc in
            0) printf 'update-available %s %s\n' "$OIS_UPD_LOCAL" "$OIS_UPD_REMOTE" ;;
            1) printf 'up-to-date %s\n' "$OIS_UPD_LOCAL" ;;
            *) printf 'unknown\n' ;;
        esac
    fi
    exit $_ck_rc
}

# Report how every declared dependency resolves on THIS machine.
cmd_deps() {
    if [ -n "${_arg1:-}" ] && ois_app_exists "$_arg1"; then
        ois_conf_load "$(ois_app_dir "$_arg1")/conf" || exit 1
    else
        for _cd_c in "$OIS_SELF_DIR" "$OIS_SELF_DIR/.."; do
            _load_conf_from_dir "$_cd_c" && break
        done
        if [ -z "${OIS_CONF_SRC:-}" ] && ! _ois_self_is_runtime; then
            _load_conf_from_dir "." || :
        fi
        [ -n "${OIS_CONF_SRC:-}" ] || ois_fail_die E-CONF "no ois.conf found" \
            "looked beside ois.sh and one level up" \
            "cd into the project, or name an installed app: ois deps <app>"
    fi
    ois_deps_parse
    ois_hdr "$OIS_APP_DISPLAY dependencies" "$OIS_OS/$OIS_ARCH  pm=$OIS_PM"
    [ -z "$OIS_DEP_TABLE" ] && { printf '  (none declared)\n\n'; return 0; }
    printf '  %-16s %-4s %-9s %-22s %s\n' NAME REQ STATUS "PACKAGE ($OIS_PM)" PROBE
    for _cd_n in $(ois_dep_names); do
        _cd_r="$(ois_dep_attr "$_cd_n" req)" || _cd_r=1
        [ "$_cd_r" = 1 ] && _cd_rl="yes" || _cd_rl="no"
        if ois_dep_probe "$_cd_n"; then _cd_s="present"; else _cd_s="MISSING"; fi
        printf '  %-16s %-4s %-9s %-22s %s\n' \
            "$_cd_n" "$_cd_rl" "$_cd_s" "$(ois_dep_package "$_cd_n")" "$OIS_DEP_HOW"
    done
    printf '\n'
}

# -- plan (dry run) ----------------------------------------------------
# Read-only: report exactly what an install WOULD do, touching nothing.
# The one command a cautious admin runs before letting OIS near a box.
cmd_plan() {
    _pl_arg="${1:-}"
    if [ -n "$_pl_arg" ] && [ -d "$_pl_arg" ]; then
        _load_conf_from_dir "$_pl_arg" || ois_fail_die E-CONF "no ois.conf under $_pl_arg"
        _pl_root="$_pl_arg"
    else
        for _pl_c in "$OIS_SELF_DIR" "$OIS_SELF_DIR/.."; do
            _load_conf_from_dir "$_pl_c" && break
        done
        if [ -z "${OIS_CONF_SRC:-}" ] && ! _ois_self_is_runtime; then
            _load_conf_from_dir "." || :
        fi
        [ -n "${OIS_CONF_SRC:-}" ] || ois_fail_die E-CONF "no ois.conf found" \
            "cd into the project, or: ois plan /path/to/project"
        _pl_root="${OIS_CONF_SRC%/*}" ; _pl_root="${_pl_root%/ois}"
    fi

    ois_hdr "$OIS_APP_DISPLAY" "plan (dry run)  ois $OIS_VERSION  $OIS_OS/$OIS_ARCH  scope=$OIS_SCOPE"
    _pl_bindir="$OIS_PREFIX/bin"

    printf '  %sapp%s        %s  (binary: %s)\n' "$C_B" "$C_R" "$OIS_APP_NAME" "$OIS_APP_BINARY"
    if ois_app_exists "$OIS_APP_NAME"; then
        printf '  %saction%s     reinstall over %s %s\n' "$C_B" "$C_R" "$OIS_APP_NAME" \
            "$(ois_meta_get "$OIS_APP_NAME" version 2>/dev/null || printf '?')"
    else
        printf '  %saction%s     fresh install\n' "$C_B" "$C_R"
    fi

    # build system
    _pl_cwd="$(pwd)" ; cd "$_pl_root" 2>/dev/null || cd "$_pl_cwd" || :
    _pl_bs="$OIS_BUILD_SYSTEM"
    if [ "$_pl_bs" = auto ]; then
        if   [ -f CMakeLists.txt ]; then _pl_bs="cmake (detected)"
        elif [ -f Makefile ] || [ -f makefile ] || [ -f GNUmakefile ]; then _pl_bs="make (detected)"
        elif [ -f meson.build ];  then _pl_bs="meson (detected)"
        elif [ -f Cargo.toml ];   then _pl_bs="cargo (detected)"
        elif [ -f go.mod ];       then _pl_bs="go (detected)"
        elif [ -f build.zig ];    then _pl_bs="zig (detected)"
        else _pl_bs="UNKNOWN -- no build system found"; fi
    fi
    cd "$_pl_cwd" || :
    printf '  %sbuild%s      %s\n' "$C_B" "$C_R" "$_pl_bs"

    # dependencies
    ois_deps_parse
    if [ -n "$OIS_DEP_TABLE" ]; then
        printf '  %sdeps%s\n' "$C_B" "$C_R"
        for _pl_n in $(ois_dep_names); do
            if ois_dep_probe "$_pl_n"; then _pl_st="present"
            else _pl_st="WILL INSTALL $(ois_dep_package "$_pl_n")"; fi
            printf '               %-16s %s\n' "$_pl_n" "$_pl_st"
        done
    fi

    # install destinations
    printf '  %sinstalls%s   %s\n' "$C_B" "$C_R" "$_pl_bindir/$OIS_APP_BINARY"
    if [ -n "$OIS_BINARIES_RAW" ]; then
        _pl_b="$OIS_BINARIES_RAW"
        while [ -n "$_pl_b" ]; do
            case "$_pl_b" in *"$OIS_NL"*) _pl_bl="${_pl_b%%"$OIS_NL"*}" ; _pl_b="${_pl_b#*"$OIS_NL"}" ;;
                             *) _pl_bl="$_pl_b" ; _pl_b="" ;; esac
            [ -z "$_pl_bl" ] && continue
            printf '               %s\n' "$_pl_bindir/${_pl_bl%%	*}"
        done
    fi

    # service
    case "$OIS_SVC_ENABLE" in
        true|yes|1) printf '  %sservice%s    register with %s\n' "$C_B" "$C_R" "$(ois_service_backend)" ;;
    esac

    # hooks present in the tree
    if [ -d "$_pl_root/ois/hooks" ] || [ -d "$_pl_root/ois/migrate" ]; then
        printf '  %shooks%s      ' "$C_B" "$C_R"
        for _pl_e in $OIS_HOOK_EVENTS; do
            [ -f "$_pl_root/ois/hooks/$_pl_e.sh" ] && printf '%s ' "$_pl_e"
        done
        for _pl_m in "$_pl_root"/ois/migrate/*.sh; do
            [ -f "$_pl_m" ] && { _pl_mv="${_pl_m##*/}"; printf 'migrate:%s ' "${_pl_mv%.sh}"; }
        done
        printf '\n'
    fi

    case ":$PATH:" in *":$_pl_bindir:"*) ;; *)
        printf '  %sPATH%s       %s is NOT on your PATH\n' "$C_Y" "$C_R" "$_pl_bindir" ;; esac
    printf '\n  %snothing was changed.%s  run:  ois install%s\n\n' "$C_D" "$C_R" \
        "$([ -n "$_pl_arg" ] && [ -d "$_pl_arg" ] && printf ' %s' "$_pl_arg")"
}

# -- self-update -------------------------------------------------------
# Update the OIS vendored in the CURRENT project from a git remote.
# Refuses to run against the store runtime -- this edits a project's
# ois/ directory, never the store.
cmd_self_update() {
    _su_src="${OIS_SELF_UPDATE_URL:-https://github.com/MilkmanAbi/OneInstallSystem}"
    if _ois_self_is_runtime; then
        ois_fail_die E-STATE "self-update updates a PROJECT's vendored ois/, not the store"             "run it from inside your project checkout, not via the global shim"             "cd into your project and run: sh ois/ois.sh self-update"
    fi
    _su_proj="$OIS_SELF_DIR" ; _su_proj="${_su_proj%/ois}"
    [ -f "$_su_proj/ois/ois.sh" ] || ois_fail_die E-STATE "cannot locate this project's ois/ directory"
    ois_hdr "self-update" "current ois $OIS_VERSION"
    ois_need_tool git || exit 1

    _su_tmp="$(ois_tmpdir)" || ois_fail_die E-STORE "no scratch space"
    OIS_SCRATCH="$_su_tmp"
    ois_info "fetching latest OIS from $_su_src"
    if ! ois_run_logged "$_su_tmp/clone.log" "git clone"             git clone --depth 1 "$_su_src" "$_su_tmp/OIS"; then
        ois_fail_die E-NET "could not clone $_su_src"             "check the URL and your connectivity"             "override the source: OIS_SELF_UPDATE_URL=<url> ois self-update"
    fi
    [ -f "$_su_tmp/OIS/ois/ois.sh" ] || ois_fail_die E-STATE "clone has no ois/ois.sh"
    _su_new="$(sh "$_su_tmp/OIS/ois/ois.sh" --version 2>/dev/null | cut -d' ' -f2)"
    ois_info "installed: $OIS_VERSION   available: ${_su_new:-unknown}"
    if [ "$_su_new" = "$OIS_VERSION" ]; then
        ois_ok "already on the latest OIS ($OIS_VERSION)"
        return 0
    fi
    ois_ask "replace this project's ois/ with $_su_new?" y || { ois_info "cancelled"; return 0; }

    # Replace core/ and ois.sh only. NEVER touch the user's ois.conf,
    # hooks, or migrations -- those are the project's, not OIS's.
    ois_install_file "$_su_tmp/OIS/ois/ois.sh" "$_su_proj/ois/ois.sh" 755 || return 1
    ois_mkdir "$_su_proj/ois/core" || return 1
    for _su_f in "$_su_tmp/OIS/ois/core"/*.sh; do
        [ -f "$_su_f" ] || continue
        ois_install_file "$_su_f" "$_su_proj/ois/core/${_su_f##*/}" 644 || return 1
    done
    # Remove core files that no longer exist upstream.
    for _su_old in "$_su_proj/ois/core"/*.sh; do
        [ -f "$_su_old" ] || continue
        [ -f "$_su_tmp/OIS/ois/core/${_su_old##*/}" ] || { ois_rm "$_su_old"; ois_dbg "removed stale ${_su_old##*/}"; }
    done
    printf '\n' ; ois_ok "updated vendored OIS: $OIS_VERSION -> $_su_new"
    ois_info "your ois.conf, hooks, and migrations were left untouched"
    ois_info "commit the change: git add ois/ && git commit -m 'update OIS to $_su_new'"
}

cmd_rollback() {
    _rb_a="$1"
    ois_lock_acquire 30 || exit 1
    ois_app_exists "$_rb_a" || ois_fail_die E-STATE "$_rb_a is not installed"
    ois_hdr "$_rb_a" "rollback  ois $OIS_VERSION"
    ois_rollback_run "$_rb_a"
}

# -- read-only ---------------------------------------------------------
cmd_list() {
    _ls_n=0
    printf '  %-18s %-10s %-6s %-8s %s\n' "APP" "VERSION" "STATE" "SCOPE" "BINARY"
    for _ls_a in $(ois_app_list); do
        printf '  %-18s %-10s %-6s %-8s %s\n' "$_ls_a" \
            "$(ois_meta_get "$_ls_a" version || printf '?')" \
            "$(ois_state_get "$_ls_a")" \
            "$(ois_meta_get "$_ls_a" scope   || printf '?')" \
            "$(ois_meta_get "$_ls_a" binary  || printf '?')"
        _ls_n=$(( _ls_n + 1 ))
    done
    if [ "$_ls_n" = 0 ]; then printf '  (none)\n'; fi
    printf '\n'
}

cmd_info() {
    _if_a="$1"
    ois_app_exists "$_if_a" || ois_fail_die E-STATE "$_if_a is not installed"
    ois_claims_fold "$_if_a"
    ois_hdr "$_if_a" "ois $OIS_VERSION"
    for _if_k in version state binary hook scope prefix runtime github update_mode installed_at; do
        printf '  %-14s %s\n' "$_if_k" "$(ois_meta_get "$_if_a" "$_if_k" || printf '-')"
    done
    printf '\n  owned paths\n'
    while IFS="$OIS_TAB" read -r _if_t _if_p _if_s _if_pol _if_o || [ -n "$_if_t" ]; do
        [ -z "$_if_p" ] && continue
        if [ -e "$_if_p" ]; then _if_st="ok"
        elif [ "$_if_o" = "claim" ] || [ "$_if_t" = "dir" ]; then _if_st="pending"
        else _if_st="MISSING" ; fi
        printf '    %-5s %-7s %-8s %-8s %s\n' "$_if_t" "$_if_pol" "${_if_o:-install}" "$_if_st" "$_if_p"
    done < "$(ois_manifest_file "$_if_a")"
    printf '\n'
}

cmd_why() {
    _wh="$(ois_manifest_owner "$1")" || { printf '  no OIS app owns %s\n\n' "$1"; return 1; }
    printf '  %s is owned by %s\n\n' "$1" "$_wh"
}

cmd_claim() {
    _cm_a="$1" ; _cm_p="$2" ; _cm_pol="${3:-keep}"
    ois_app_exists "$_cm_a" || ois_fail_die E-STATE "$_cm_a is not installed"
    printf 'file\t%s\t%s\n' "$_cm_p" "$_cm_pol" >> "$(ois_claims_file "$_cm_a")" \
        || ois_fail_die E-STORE "cannot append claim"
    ois_claims_fold "$_cm_a"
}

# -- doctor ------------------------------------------------------------
# The one command to run when anything feels wrong. Read-mostly; only
# repairs with --repair or per-finding consent via prompts.
cmd_doctor() {
    _dr_fix="${1:-0}" ; _dr_bad=0
    ois_hdr "ois doctor" "ois $OIS_VERSION  $OIS_OS/$OIS_ARCH"

    # transport / tooling
    if ois_http_init; then ois_ok "transport: $OIS_HTTP"
    else ois_warn "no curl or wget -- updates and remote installs unavailable"; _dr_bad=1; fi
    ois_sha256_init
    if [ -n "$OIS_SHA_CMD" ]; then ois_ok "sha256: $OIS_SHA_CMD"
    else ois_warn "no sha256 tool -- downloads cannot be verified"; fi
    if [ "$OIS_SUDO" != "none" ] || [ "$OIS_IS_ROOT" = "yes" ]; then
        ois_ok "privilege: ${OIS_SUDO}${OIS_IS_ROOT:+ (root=$OIS_IS_ROOT)}"
    else ois_info "no sudo/doas -- user scope only"; fi

    # stale lock
    _dr_lk="$(ois_store_root)/lock"
    if [ -d "$_dr_lk" ]; then
        _dr_pid="" ; [ -r "$_dr_lk/pid" ] && read -r _dr_pid < "$_dr_lk/pid"
        if [ -n "$_dr_pid" ] && kill -0 "$_dr_pid" 2>/dev/null; then
            ois_info "lock held by live pid $_dr_pid (an operation is running)"
        else
            ois_warn "stale lock (pid ${_dr_pid:-?} is gone)"; _dr_bad=1
            if [ "$_dr_fix" = 1 ] || ois_ask "clear it?" y; then
                ois_rmtree "$_dr_lk" && ois_ok "lock cleared"
            fi
        fi
    else
        ois_ok "lock: free"
    fi

    # apps
    for _dr_a in $(ois_app_list); do
        _dr_st="$(ois_state_get "$_dr_a")"
        if [ "$_dr_st" != "ok" ]; then
            ois_warn "$_dr_a: incomplete install (state=$_dr_st)"; _dr_bad=1
            if [ "$_dr_fix" = 1 ] || ois_ask "remove the partial record for $_dr_a?" y; then
                _dr_rt="$(ois_meta_get "$_dr_a" runtime 2>/dev/null)" || _dr_rt=""
                [ -n "$_dr_rt" ] && ois_runtime_ref_del "$_dr_rt" "$_dr_a"
                ois_app_destroy "$_dr_a" && ois_ok "$_dr_a partial record removed"
            fi
            continue
        fi
        _dr_bin="$(ois_meta_get "$_dr_a" binary 2>/dev/null)"
        if [ -n "$_dr_bin" ] && [ ! -f "$_dr_bin" ]; then
            ois_warn "$_dr_a: binary missing ($_dr_bin)"; _dr_bad=1
            ois_info "  -> ois update $_dr_a   will reinstall it"
        else
            if ois_verify_app "$_dr_a" >/dev/null 2>&1; then
                ois_ok "$_dr_a: healthy"
            else
                ois_warn "$_dr_a: verify reports drift -- run: ois verify $_dr_a"
                _dr_bad=1
            fi
        fi
    done

    # orphaned runtimes
    _dr_rt_d="$(ois_runtime_dir)"
    if [ -d "$_dr_rt_d" ]; then
        for _dr_v in "$_dr_rt_d"/*; do
            [ -d "$_dr_v" ] || continue
            _dr_n="${_dr_v##*/}"
            case "$_dr_n" in current|.active) continue ;; esac
            if [ "$(ois_runtime_refcount "$_dr_n")" = 0 ]; then
                ois_info "runtime $_dr_n is unreferenced -- ois gc will remove it"
            fi
        done
    fi

    # recent failures
    _dr_log="$(ois_store_root)/log"
    if [ -s "$_dr_log" ]; then
        printf '\n  recent failures:\n'
        tail -n 5 "$_dr_log" 2>/dev/null | while IFS= read -r _dr_l; do
            printf '    %s\n' "$_dr_l"
        done
    fi

    printf '\n'
    if [ "$_dr_bad" = 0 ]; then ois_ok "no problems found"
    else ois_warn "issues found -- see above"; fi
    printf '\n'
    return "$_dr_bad"
}

cmd_service() {
    _cs_app="$1" ; _cs_act="${2:-status}"
    ois_app_exists "$_cs_app" || ois_fail_die E-STATE "$_cs_app is not installed"
    ois_conf_load "$(ois_app_dir "$_cs_app")/conf" || exit 1
    case "$_cs_act" in
        start)   ois_service_start "$_cs_app" && ois_ok "started" ;;
        stop)    ois_service_stop  "$_cs_app" && ois_ok "stopped" ;;
        restart) ois_service_stop  "$_cs_app" ; ois_service_start "$_cs_app" && ois_ok "restarted" ;;
        status)  ois_service_status "$_cs_app" ;;
        enable)  ois_service_install "$_cs_app" ;;
        disable) ois_service_remove  "$_cs_app" ;;
        *) ois_fail_die E-STATE "unknown service action: $_cs_act" "" \
               "valid: start stop restart status enable disable" ;;
    esac
}

cmd_channel() {
    _cc_app="$1" ; _cc_new="${2:-}"
    ois_app_exists "$_cc_app" || ois_fail_die E-STATE "$_cc_app is not installed"
    if [ -z "$_cc_new" ]; then
        printf '%s\n' "$(ois_meta_get "$_cc_app" channel 2>/dev/null || printf 'stable')"
        return 0
    fi
    case "$_cc_new" in
        stable|beta|any|nightly) ;;
        *) ois_fail_die E-STATE "unknown channel: $_cc_new" "" \
               "valid: stable (releases only), beta (adds -rc/-beta/-alpha), any (every tag)" ;;
    esac
    ois_meta_setmany "$_cc_app" <<EOF
channel=$_cc_new
last_check=0
backoff_n=0
backoff_until=0
EOF
    ois_ok "$_cc_app is now on the $_cc_new channel"
    ois_info "check now: ois check $_cc_app"
}

cmd_hooks() {
    _ch_app="$1"
    ois_app_exists "$_ch_app" || ois_fail_die E-STATE "$_ch_app is not installed"
    ois_hdr "$_ch_app hooks" "ois $OIS_VERSION"
    _ch_hd="$(ois_hooks_dir "$_ch_app")" ; _ch_any=0
    for _ch_e in $OIS_HOOK_EVENTS; do
        if [ -f "$_ch_hd/$_ch_e.sh" ]; then
            printf '  %-16s %s\n' "$_ch_e" "captured" ; _ch_any=1
        else
            printf '  %-16s %s\n' "$_ch_e" "-"
        fi
    done
    printf '\n  migrations\n'
    _ch_md="$(ois_migrate_dir "$_ch_app")"
    if [ -d "$_ch_md" ]; then
        for _ch_m in "$_ch_md"/*.sh; do
            [ -f "$_ch_m" ] || continue
            _ch_v="${_ch_m##*/}" ; printf '    %s\n' "${_ch_v%.sh}" ; _ch_any=1
        done
    fi
    [ "$_ch_any" = 0 ] && printf '    (none)\n'
    printf '\n'
}

cmd_help() {
    printf '\nois %s -- OneInstallSystem\n\n' "$OIS_VERSION"
    printf '  ois install [PATH | USER/REPO] [--tag TAG] [--user|--system] [--prefix P] [--yes]\n'
    printf '  ois uninstall <app> [--purge] [--yes]\n'
    printf '  ois update <app> [--to TAG]      prebuilt asset if published, else build\n'
    printf '  ois check <app>                  exit 0 if an update exists\n'
    printf '  ois rollback <app>               instant, offline, no rebuild\n'
    printf '  ois list\n'
    printf '  ois info <app>\n'
    printf '  ois verify <app>                 hash every owned file, report drift\n'
    printf '  ois why <path>                   which app owns this path\n'
    printf '  ois claim <app> <path>           register a runtime-created path\n'
    printf '  ois env <app>                    print the app environment block\n'
    printf '  ois doctor [--repair]            diagnose the store and every app\n'
    printf '  ois gc                           drop unreferenced runtimes\n\n'
    printf '  --app <name>                     operate on an installed app (used by hooks)\n'
    printf '  --json                           machine-readable output (list, info, check)\n'
    printf '  --verbose --quiet --yes\n\n'
}

# -- main --------------------------------------------------------------
main() {
    _cmd="" _scope="" _app="" _purge=0 _arg1="" _arg2="" _to="" _tag="" _repair=0
    OIS_JSON="${OIS_JSON:-0}" ; _check=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --app)      _app="$2" ; shift 2 ; continue ;;
            --to)       _to="$2"  ; shift 2 ; continue ;;
            --tag)      _tag="$2" ; shift 2 ; continue ;;
            --prefix)   OIS_PREFIX="$2" ; shift 2 ; continue ;;
            --user)     _scope="user" ;;
            --system)   _scope="system" ;;
            --purge)    _purge=1 ;;
            --repair)   _repair=1 ;;
            --check)    _check=1 ;;
            --json)     OIS_JSON=1 ;;
            --yes|-y)   OIS_ASSUME_YES=1 ;;
            --verbose)  OIS_VERBOSE=1 ;;
            --quiet)    OIS_QUIET=1 ;;
            --version)  printf 'ois %s\n' "$OIS_VERSION" ; exit 0 ;;
            --help|-h)  cmd_help ; exit 0 ;;
            --*)        _cmd="${_cmd:-${1#--}}" ;;
            *)          if [ -z "$_cmd" ]; then _cmd="$1"
                        elif [ -z "$_arg1" ]; then _arg1="$1"
                        else _arg2="$1" ; fi ;;
        esac
        shift
    done
    # JSON output must never be polluted by human-facing chatter.
    [ "$OIS_JSON" = 1 ] && OIS_QUIET=1
    export OIS_ASSUME_YES OIS_VERBOSE OIS_QUIET OIS_JSON

    if [ -n "$_app" ] && [ -z "$_scope" ]; then
        OIS_SCOPE="user"
        ois_app_exists "$_app" || OIS_SCOPE="system"
        _scope="$OIS_SCOPE"
    fi
    _resolve_scope "$_scope"
    [ -n "$_app" ] && [ -z "$_arg1" ] && _arg1="$_app"

    case "${_cmd:-help}" in
        install)
            case "${_arg1:-}" in
                */*) if [ -d "$_arg1" ]; then cmd_install_local "$_arg1"
                     else cmd_install_remote "$_arg1" "$_tag"; fi ;;
                '')  cmd_install_local "" ;;
                *)   if [ -d "$_arg1" ]; then cmd_install_local "$_arg1"
                     else ois_fail_die E-CONF "not a directory or repo: $_arg1"; fi ;;
            esac ;;
        uninstall|remove)    cmd_uninstall "${_arg1:?app required}" "$_purge" ;;
        update|upgrade)      cmd_update   "${_arg1:?app required}" "$_to" ;;
        check)               cmd_check    "${_arg1:?app required}" ;;
        rollback)            cmd_rollback "${_arg1:?app required}" ;;
        list|ls)             if [ "$OIS_JSON" = 1 ]; then ois_json_list; else cmd_list; fi ;;
        info|ois|status)
            if [ "$OIS_JSON" = 1 ]; then
                ois_app_exists "${_arg1:?app required}" || exit 1
                ois_claims_fold "$_arg1" >/dev/null 2>&1
                ois_json_info "$_arg1"
            else cmd_info "${_arg1:?app required}"; fi ;;
        deps)                cmd_deps ;;
        service|svc)         cmd_service "${_arg1:?app required}" "${_arg2:-status}" ;;
        channel)             cmd_channel "${_arg1:?app required}" "${_arg2:-}" ;;
        hooks)               cmd_hooks   "${_arg1:?app required}" ;;
        plan|dry-run)        cmd_plan "${_arg1:-}" ;;
        self-update)         cmd_self_update ;;
        lock)
            for _lk_c in "$OIS_SELF_DIR" "$OIS_SELF_DIR/.."; do
                _load_conf_from_dir "$_lk_c" && break
            done
            if [ -z "${OIS_CONF_SRC:-}" ] && ! _ois_self_is_runtime; then
                _load_conf_from_dir "." || :
            fi
            [ -n "${OIS_CONF_SRC:-}" ] || ois_fail_die E-CONF "no ois.conf found"
            if [ "$_check" = 1 ]; then ois_lock_check "${_arg1:-./ois.lock}"
            else ois_lock_write "$OIS_APP_NAME" && ois_ok "wrote ois.lock"; fi ;;
        verify)              ois_verify_app "${_arg1:?app required}" && ois_ok "verified" ;;
        why|owner)           cmd_why "${_arg1:?path required}" ;;
        claim)               cmd_claim "${_arg1:?app required}" "${_arg2:?path required}" ;;
        env)                 cat "$(ois_app_dir "${_arg1:?app required}")/env" ;;
        doctor)              cmd_doctor "$_repair" ;;
        gc)                  ois_runtime_gc ; _ois_shim_gc ;;
        help)                cmd_help ;;
        *) ois_err "unknown command: $_cmd" ; cmd_help ; exit 1 ;;
    esac
}

main "$@"

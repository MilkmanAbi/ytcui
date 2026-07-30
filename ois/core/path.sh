#!/bin/sh
# OIS v4 -- core/path.sh
# Shell PATH management: add the install bindir to the user's shell
# startup files on install, and remove it on uninstall.
#
# Why this exists: macOS is uniquely hostile about PATH. A user-scope
# install lands in ~/.local/bin, which is NOT on the default PATH in
# zsh (the macOS default shell since Catalina) or bash. Homebrew's
# /opt/homebrew/bin (Apple Silicon) is likewise absent until you run
# `brew shellenv`. So a freshly installed binary "isn't found" even
# though it installed perfectly -- the single most common post-install
# confusion. OIS fixes it by writing a small, clearly-marked block to
# the right rc files, and cleanly removing it on uninstall.
#
# Rules:
#   - Every edit is delimited by OIS markers so removal is exact.
#   - System scope (/usr/local/bin) is already on PATH everywhere; we
#     never touch rc files for it.
#   - We only add when the dir is genuinely missing from PATH.
#   - We write to a temp file and mv (never sed -i -- not portable).
#   - We touch every login/interactive rc a user's shell might read,
#     because we cannot know which one is authoritative, but we make
#     the block idempotent so duplicates never accumulate.
#   - Removal is refcounted: the PATH block is only removed when no
#     other OIS-managed binary still lives in that bindir.
# ---------------------------------------------------------------------

OIS_PATH_MARK_BEGIN="# >>> ois path (managed) >>>"
OIS_PATH_MARK_END="# <<< ois path (managed) <<<"

# Candidate rc files for the invoking user. We deliberately cover the
# common shells rather than trying to detect "the" shell, because a
# user may switch shells and expect the tool to still be found.
#   zsh:   ~/.zshrc  (+ ~/.zprofile for login)
#   bash:  ~/.bashrc, ~/.bash_profile, ~/.profile
#   POSIX: ~/.profile
#   fish:  handled separately (different syntax + config path)
_ois_path_rc_files() {
    _prf_home="${OIS_HOME:-$HOME}"
    # Only files that already exist, PLUS the one canonical file per shell
    # we are willing to create if none exist. Emit existing ones first.
    for _prf in \
        "$_prf_home/.zshrc" \
        "$_prf_home/.bashrc" \
        "$_prf_home/.bash_profile" \
        "$_prf_home/.profile"
    do
        [ -f "$_prf" ] && printf '%s\n' "$_prf"
    done
}

# The canonical file to CREATE when the user has no rc file at all.
# ~/.profile is read by POSIX sh and login bash, and sourced by most
# setups; it's the safest lowest-common-denominator target.
_ois_path_default_rc() {
    printf '%s' "${OIS_HOME:-$HOME}/.profile"
}

# fish uses a different syntax and its own config dir. We handle it only
# if fish config already exists, to avoid creating fish state for a user
# who doesn't use fish.
_ois_path_fish_config() {
    _pfc_base="${XDG_CONFIG_HOME:-${OIS_HOME:-$HOME}/.config}"
    printf '%s' "$_pfc_base/fish/config.fish"
}

# True if $1 (a bindir) is already present in the live PATH.
_ois_path_on_path() {
    case ":$PATH:" in *":$1:"*) return 0 ;; *) return 1 ;; esac
}

# True if a file already contains an OIS-managed block for $2 (bindir).
_ois_path_block_present() {
    _pbp_file="$1" _pbp_dir="$2"
    [ -f "$_pbp_file" ] || return 1
    # The block is present AND references this exact dir.
    grep -qF "$OIS_PATH_MARK_BEGIN" "$_pbp_file" 2>/dev/null || return 1
    grep -qF "$_pbp_dir" "$_pbp_file" 2>/dev/null
}

# Append an OIS-managed PATH block to a POSIX/bash/zsh rc file.
# Idempotent: if a block referencing this dir already exists, no-op.
_ois_path_add_posix() {
    _pap_file="$1" _pap_dir="$2"
    _ois_path_block_present "$_pap_file" "$_pap_dir" && return 0
    # Build the new content: existing content (minus any stale OIS block
    # for a DIFFERENT dir is left alone -- we only manage our own marker
    # once) + our block. If a block exists for another dir we still add,
    # because a second managed bindir is legitimate; the guard command
    # inside makes it safe.
    {
        [ -f "$_pap_file" ] && cat "$_pap_file"
        printf '%s\n' "$OIS_PATH_MARK_BEGIN"
        printf '# Added by OIS so installed tools in this directory are found.\n'
        # Guard: only prepend if not already present, so re-sourcing is safe.
        # shellcheck disable=SC2016  # $PATH must be written literally into the rc file
        printf 'case ":$PATH:" in\n'
        printf '  *":%s:"*) ;;\n' "$_pap_dir"
        # shellcheck disable=SC2016
        printf '  *) PATH="%s:$PATH" ;;\n' "$_pap_dir"
        printf 'esac\n'
        printf 'export PATH\n'
        printf '%s\n' "$OIS_PATH_MARK_END"
    } | ois_write_atomic "$_pap_file" 644 || return 1
    ois_dbg "path: added $_pap_dir block to $_pap_file"
    return 0
}

# fish variant: uses fish_add_path, guarded, inside markers.
_ois_path_add_fish() {
    _paf_file="$1" _paf_dir="$2"
    _ois_path_block_present "$_paf_file" "$_paf_dir" && return 0
    ois_mkdir "${_paf_file%/*}" || return 1
    {
        [ -f "$_paf_file" ] && cat "$_paf_file"
        printf '%s\n' "$OIS_PATH_MARK_BEGIN"
        printf '# Added by OIS so installed tools in this directory are found.\n'
        printf 'if test -d %s\n' "$_paf_dir"
        printf '    fish_add_path %s\n' "$_paf_dir"
        printf 'end\n'
        printf '%s\n' "$OIS_PATH_MARK_END"
    } | ois_write_atomic "$_paf_file" 644 || return 1
    ois_dbg "path: added $_paf_dir block to $_paf_file (fish)"
    return 0
}

# Remove the OIS-managed block that references $2 (bindir) from $1.
# Pure shell line filter (no sed -i). Removes exactly one begin..end
# block whose body mentions the dir; leaves blocks for other dirs.
_ois_path_remove_block() {
    _prb_file="$1" _prb_dir="$2"
    [ -f "$_prb_file" ] || return 0
    _ois_path_block_present "$_prb_file" "$_prb_dir" || return 0
    _prb_tmp="$_prb_file.ois-path.$$"
    _prb_in_block=0 _prb_buf="" _prb_match=0
    # Read line by line; buffer each managed block, decide at END whether
    # it referenced our dir. Non-block lines pass straight through.
    {
        while IFS= read -r _prb_l || [ -n "$_prb_l" ]; do
            case "$_prb_l" in
                "$OIS_PATH_MARK_BEGIN")
                    _prb_in_block=1 ; _prb_buf="$_prb_l" ; _prb_match=0 ; continue ;;
                "$OIS_PATH_MARK_END")
                    _prb_buf="$_prb_buf
$_prb_l"
                    if [ "$_prb_match" = 1 ]; then
                        :   # drop the whole block (ours)
                    else
                        printf '%s\n' "$_prb_buf"   # keep (another dir's)
                    fi
                    _prb_in_block=0 ; _prb_buf="" ; continue ;;
            esac
            if [ "$_prb_in_block" = 1 ]; then
                _prb_buf="$_prb_buf
$_prb_l"
                case "$_prb_l" in *"$_prb_dir"*) _prb_match=1 ;; esac
            else
                printf '%s\n' "$_prb_l"
            fi
        done < "$_prb_file"
    } > "$_prb_tmp" 2>/dev/null || { rm -f "$_prb_tmp"; return 1; }
    # If the file is now effectively empty and we created it, remove it.
    mv -f "$_prb_tmp" "$_prb_file" 2>/dev/null || { rm -f "$_prb_tmp"; return 1; }
    ois_dbg "path: removed $_prb_dir block from $_prb_file"
    return 0
}

# -- Public: ensure a bindir is on PATH via rc files -------------------
# Called after a USER-scope install. Adds to existing rc files, and if
# none exist, creates ~/.profile. Returns 0 always (best-effort).
# Sets nothing fatal: a failure here never fails an install.
ois_path_ensure() {
    _pe_dir="$1"
    # System prefixes are already on everyone's PATH; never edit rc files.
    case "$_pe_dir" in
        /usr/local/bin|/usr/bin|/bin|/opt/homebrew/bin) return 0 ;;
    esac
    # Already on PATH in this session AND persisted somewhere? Still make
    # sure a managed block exists so NEW shells get it too. But if it's on
    # PATH because the user already configured it themselves, don't clobber.
    _pe_added=0
    _pe_any_rc=0
    for _pe_f in $(_ois_path_rc_files); do
        _pe_any_rc=1
        _ois_path_add_posix "$_pe_f" "$_pe_dir" && _pe_added=1
    done
    # fish, only if the user has a fish config already
    _pe_fish="$(_ois_path_fish_config)"
    [ -f "$_pe_fish" ] && { _ois_path_add_fish "$_pe_fish" "$_pe_dir" && _pe_added=1; }
    # No rc files at all: create the canonical default.
    if [ "$_pe_any_rc" = 0 ]; then
        _ois_path_add_posix "$(_ois_path_default_rc)" "$_pe_dir" && _pe_added=1
    fi
    if [ "$_pe_added" = 1 ]; then
        if _ois_path_on_path "$_pe_dir"; then
            ois_info "PATH already active; also added to your shell startup for new sessions"
        else
            ois_ok "added $_pe_dir to your shell startup files"
            printf '    %s\n' "open a new terminal, or run:  export PATH=\"$_pe_dir:\$PATH\""
        fi
    fi
    return 0
}

# -- Public: remove a bindir from PATH rc files ------------------------
# Called on uninstall. Refcounted by the caller: only invoked once no
# OIS-managed binary remains in the bindir. Removes the managed block
# from every rc file that has one.
ois_path_retract() {
    _pr_dir="$1"
    case "$_pr_dir" in
        /usr/local/bin|/usr/bin|/bin|/opt/homebrew/bin) return 0 ;;
    esac
    _pr_removed=0
    for _pr_f in \
        "${OIS_HOME:-$HOME}/.zshrc" \
        "${OIS_HOME:-$HOME}/.bashrc" \
        "${OIS_HOME:-$HOME}/.bash_profile" \
        "${OIS_HOME:-$HOME}/.profile" \
        "$(_ois_path_fish_config)"
    do
        if _ois_path_block_present "$_pr_f" "$_pr_dir"; then
            _ois_path_remove_block "$_pr_f" "$_pr_dir" && _pr_removed=1
        fi
    done
    [ "$_pr_removed" = 1 ] && ois_ok "removed $_pr_dir from your shell startup files"
    return 0
}

# -- Public: does the bindir still hold any OIS-managed binary? ---------
# Uses the store: scan every installed app's manifest for a 'file' entry
# whose parent dir is the bindir. If none, the bindir is ours to retract.
# Excludes the app currently being removed (pass its name as $2).
ois_path_bindir_in_use() {
    _pbu_dir="$1" _pbu_except="${2:-}"
    command -v ois_app_list >/dev/null 2>&1 || return 0   # can't tell; assume in use
    for _pbu_app in $(ois_app_list 2>/dev/null); do
        [ "$_pbu_app" = "$_pbu_except" ] && continue
        _pbu_mf="$(ois_manifest_file "$_pbu_app" 2>/dev/null)" || continue
        [ -f "$_pbu_mf" ] || continue
        while IFS="$OIS_TAB" read -r _pbu_t _pbu_p _pbu_rest || [ -n "$_pbu_t" ]; do
            [ "$_pbu_t" = file ] || continue
            case "$_pbu_p" in
                "$_pbu_dir"/*) return 0 ;;   # still in use
            esac
        done < "$_pbu_mf"
    done
    return 1   # not in use -> safe to retract
}

#!/bin/sh
# OIS core/integrate.sh — post-install integration
# ──────────────────────────────────────────────────

ois_integrate_run() {
    _bin_path="$1"

    # ── Runtime copy ─────────────────────────────────
    # Copies OIS scripts to a stable location so the source clone can be deleted.
    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        _runtime="/usr/local/share/OIS/runtime"
    else
        _runtime="${OIS_HOME}/.local/share/OIS/runtime"
    fi

    ois_mkdir "$_runtime/core"
    if [ "$OIS_DIR" != "$_runtime" ]; then
        for _f in "$OIS_DIR/OIS.sh" "$OIS_DIR/OIS.conf"; do
            [ -f "$_f" ] && ois_cp "$_f" "$_runtime/$(basename "$_f")" \
                         && ois_chmod 755 "$_runtime/$(basename "$_f")"
        done
        for _f in "$OIS_DIR/core/"*.sh; do
            [ -f "$_f" ] && ois_cp "$_f" "$_runtime/core/$(basename "$_f")" \
                         && ois_chmod 755 "$_runtime/core/$(basename "$_f")"
        done
    fi
    ois_mf_add "$OIS_APP_NAME" "$_runtime"
    ois_ok "OIS runtime    →  $_runtime"

    # ── OIS hook ─────────────────────────────────────
    # Points to the runtime copy. Source clone can be deleted safely.
    _hook="${OIS_APP_INSTALL_PATH}/.${OIS_APP_BINARY}-ois"
    ois_writef "#!/bin/sh
exec sh '${_runtime}/OIS.sh' \"\$@\"
" "$_hook"
    ois_chmod 755 "$_hook"
    ois_mf_add "$OIS_APP_NAME" "$_hook"
    ois_ok "OIS hook       →  $_hook"

    # ── Desktop / app bundle ─────────────────────────
    case "$OIS_OS" in
        linux|wsl) _ois_desktop "$_bin_path" ;;
        macos)     _ois_macos_bundle "$_bin_path" ;;
    esac

    # ── Uninstaller ───────────────────────────────────
    _ois_gen_uninstaller "$_bin_path" "$_hook" "$_runtime"
}

# ── Linux .desktop ────────────────────────────────────
_ois_desktop() {
    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        _dd="/usr/share/applications"
        _id="/usr/share/icons/hicolor/256x256/apps"
    else
        _dd="${OIS_HOME}/.local/share/applications"
        _id="${OIS_HOME}/.local/share/icons/hicolor/256x256/apps"
    fi
    ois_mkdir "$_dd" ; ois_mkdir "$_id"
    _desk="$_dd/${OIS_APP_NAME}.desktop"
    _icon_line=""
    [ -n "$OIS_APP_ICON" ] && [ -f "$OIS_APP_ICON" ] && \
        _icon_line="Icon=${OIS_APP_NAME}"
    ois_writef "[Desktop Entry]
Name=${OIS_APP_DISPLAY}
Exec=${1}
Terminal=true
Type=Application
Categories=${OIS_APP_DESKTOP_CAT:-Utility};
Comment=${OIS_APP_DESKTOP_CMT:-}
${_icon_line}" "$_desk"
    ois_mf_add "$OIS_APP_NAME" "$_desk"
    ois_ok "Desktop entry  →  $_desk"

    if [ -n "$OIS_APP_ICON" ] && [ -f "$OIS_APP_ICON" ]; then
        ois_cp "$OIS_APP_ICON" "$_id/${OIS_APP_NAME}.png"
        ois_mf_add "$OIS_APP_NAME" "$_id/${OIS_APP_NAME}.png"
        ois_ok "Icon           →  $_id/${OIS_APP_NAME}.png"
    fi
    command -v update-desktop-database >/dev/null 2>&1 && \
        update-desktop-database "$_dd" 2>/dev/null || true
}

# ── macOS minimal bundle ──────────────────────────────
_ois_macos_bundle() {
    [ -z "$OIS_APP_ICON" ] || [ ! -f "$OIS_APP_ICON" ] && {
        ois_warn "No icon set — skipping macOS app bundle"; return 0
    }
    [ "${OIS_SCOPE:-user}" = "system" ] \
        && _apps="/Applications" \
        || _apps="${OIS_HOME}/Applications"
    _bundle="$_apps/${OIS_APP_DISPLAY}.app"
    ois_mkdir "$_bundle/Contents/MacOS"
    ois_mkdir "$_bundle/Contents/Resources"
    ois_writef "#!/bin/sh
exec '${1}' \"\$@\"
" "$_bundle/Contents/MacOS/${OIS_APP_NAME}"
    ois_chmod 755 "$_bundle/Contents/MacOS/${OIS_APP_NAME}"
    _ver="$(ois_reg_get "$OIS_APP_NAME" version 2>/dev/null || printf '1.0.0')"
    ois_writef "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
  <key>CFBundleExecutable</key>        <string>${OIS_APP_NAME}</string>
  <key>CFBundleIdentifier</key>        <string>com.ois.${OIS_APP_NAME}</string>
  <key>CFBundleName</key>              <string>${OIS_APP_DISPLAY}</string>
  <key>CFBundleVersion</key>           <string>${_ver}</string>
  <key>CFBundleShortVersionString</key><string>${_ver}</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>LSUIElement</key>               <true/>
</dict></plist>" "$_bundle/Contents/Info.plist"
    ois_cp "$OIS_APP_ICON" "$_bundle/Contents/Resources/${OIS_APP_NAME}.png"
    ois_mf_add "$OIS_APP_NAME" "$_bundle"
    ois_ok "macOS bundle   →  $_bundle"
}

# ── Uninstaller generation ────────────────────────────
# All paths baked in as literal strings — no runtime lookups.
# Binary and hook removed first. Then manifest. Clean and total.
_ois_gen_uninstaller() {
    _gu_bin="$1"
    _gu_hook="$2"
    _gu_runtime="$3"

    if [ "${OIS_SCOPE:-user}" = "system" ]; then
        _gu_dir="/usr/local/share/OIS/uninstallers"
    else
        _gu_dir="${OIS_HOME}/.local/share/OIS/uninstallers"
    fi
    ois_mkdir "$_gu_dir"

    _gu_path="$_gu_dir/${OIS_APP_NAME}.sh"
    _gu_mf="$(ois_mf_file   "$OIS_APP_NAME")"
    _gu_reg="$(ois_reg_file  "$OIS_APP_NAME")"

    # Write via temp file — avoids all quoting/heredoc issues with ois_writef
    _gu_tmp="$(mktemp)"
    cat > "$_gu_tmp" << UEOF
#!/bin/sh
# OIS uninstaller for: ${OIS_APP_NAME}
# All paths are literal — no external lookups needed.

G='\033[32m' Y='\033[33m' B='\033[1m' R='\033[0m'
ok()   { printf "  \${G}ok\${R}  %s\n" "\$*"; }
warn() { printf "  \${Y}!\${R}  %s\n" "\$*"; }
prm()  { rm -f  "\$1" 2>/dev/null || sudo rm -f  "\$1" 2>/dev/null; }
prmr() { rm -rf "\$1" 2>/dev/null || sudo rm -rf "\$1" 2>/dev/null; }

printf "\n\${B}Uninstalling ${OIS_APP_NAME}...\${R}\n\n"

if [ "\$1" != "--yes" ] && [ "\$1" != "-y" ]; then
    printf "  Remove ${OIS_APP_NAME} and all its files? [y/N] "
    read -r _a
    case "\$_a" in y|Y|yes) ;; *) printf "  Cancelled.\n\n"; exit 0 ;; esac
fi

_keep="yes"
if [ "\$1" != "--purge" ]; then
    printf "\n  Keep your config and saved data? [Y/n] "
    read -r _k
    case "\$_k" in n|N|no) _keep="no" ;; esac
fi
printf "\n"

# ── Remove binary (literal path, no lookup) ──────────
if [ -f "${_gu_bin}" ]; then
    prm "${_gu_bin}" && ok "Removed  ${_gu_bin}"
fi

# ── Remove OIS hook (literal path) ───────────────────
if [ -f "${_gu_hook}" ]; then
    prm "${_gu_hook}" && ok "Removed  ${_gu_hook}"
fi

# ── Remove manifest items ─────────────────────────────
if [ -f "${_gu_mf}" ]; then
    while IFS= read -r _f || [ -n "\$_f" ]; do
        [ -z "\$_f" ] && continue
        [ "\$_f" = "${_gu_bin}" ]     && continue
        [ "\$_f" = "${_gu_hook}" ]    && continue
        if [ "\$_keep" = "yes" ]; then
            case "\$_f" in
                */.config/${OIS_APP_NAME}*)      continue ;;
                */.local/share/${OIS_APP_NAME}*) continue ;;
                */.cache/${OIS_APP_NAME}*)       continue ;;
            esac
        fi
        if   [ -f "\$_f" ]; then prm  "\$_f" && ok "Removed  \$_f"
        elif [ -d "\$_f" ]; then prmr "\$_f" && ok "Removed  \$_f"
        fi
    done < "${_gu_mf}"
fi

# ── Remove registry files and self ───────────────────
prm  "${_gu_mf}"
prm  "${_gu_reg}"
prm  "\$0"
printf "\n"
ok "${OIS_APP_NAME} uninstalled."
printf "\n"
UEOF

    # Replace 'ok' → '✓' in the output functions (heredoc can't embed it directly)
    sed -i "s/printf \"  \${G}ok\${R}/printf \"  \${G}✓\${R}/" "$_gu_tmp" 2>/dev/null || true

    ois_cp "$_gu_tmp" "$_gu_path"
    rm -f "$_gu_tmp"
    ois_chmod 755 "$_gu_path"
    ois_mf_add "$OIS_APP_NAME" "$_gu_path"
    ois_reg_set "$OIS_APP_NAME" uninstaller "$_gu_path"
    ois_ok "Uninstaller    →  $_gu_path"
}

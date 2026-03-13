#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# ytcui uninstaller — Cross-platform (Linux, macOS, BSD)
# Removes the binary, config, library, cache, and support files
# Compatible with macOS Bash 3.2+, Linux, FreeBSD/NetBSD/OpenBSD
# Usage: sudo ./uninstall.sh
# ═══════════════════════════════════════════════════════════════════════════════

set -e

# ─── Colors ─────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    BOLD='\033[1m'
    CYAN='\033[36m'
    GREEN='\033[32m'
    YELLOW='\033[33m'
    RED='\033[31m'
    NC='\033[0m'
else
    BOLD='' CYAN='' GREEN='' YELLOW='' RED='' NC=''
fi

ok()   { echo -e "  ${GREEN}✓${NC} $*"; }
warn() { echo -e "  ${YELLOW}!${NC} $*"; }
die()  { echo -e "\n  ${RED}✗ Fatal:${NC} $*"; exit 1; }
info() { echo -e "  ${CYAN}→${NC} $*"; }

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo -e "${BOLD}${CYAN}  ytcui — Uninstaller${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo ""

# ─── Remove binary ───────────────────────────────────────────────────────────────
BINARY_PATH="/usr/local/bin/ytcui"
if [[ -f "$BINARY_PATH" ]]; then
    info "Removing binary: $BINARY_PATH"
    if [[ -w "$BINARY_PATH" ]]; then
        rm -f "$BINARY_PATH" || die "Failed to remove binary"
    else
        sudo rm -f "$BINARY_PATH" || die "Failed to remove binary with sudo"
    fi
    ok "Binary removed"
else
    warn "Binary not found at $BINARY_PATH"
fi

# ─── Remove shared support files ────────────────────────────────────────────────
DATA_DIR="/usr/local/share/ytcui"
if [[ -d "$DATA_DIR" ]]; then
    info "Removing support files: $DATA_DIR"
    if [[ -w "$DATA_DIR" ]]; then
        rm -rf "$DATA_DIR" || die "Failed to remove support files"
    else
        sudo rm -rf "$DATA_DIR" || die "Failed to remove support files with sudo"
    fi
    ok "Support files removed"
else
    warn "Support files directory not found: $DATA_DIR"
fi

# ─── Remove user config / cache / library ───────────────────────────────────────
USER_DIRS=(
    "$HOME/.config/ytcui"
    "$HOME/.local/share/ytcui"
    "$HOME/.cache/ytcui"
)

for dir in "${USER_DIRS[@]}"; do
    if [[ -d "$dir" ]]; then
        info "Removing user data: $dir"
        rm -rf "$dir" || warn "Failed to remove $dir"
        ok "Removed $dir"
    else
        warn "Directory not found: $dir"
    fi
done

# ─── Verify ─────────────────────────────────────────────────────────────────────
if ! command -v ytcui >/dev/null 2>&1; then
    ok "ytcui successfully uninstalled"
else
    warn "ytcui still exists in PATH"
fi

echo ""
echo -e "${GREEN}${BOLD}══════════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}  ✓ Uninstallation complete!${NC}"
echo -e "${GREEN}${BOLD}══════════════════════════════════════════${NC}"
echo ""

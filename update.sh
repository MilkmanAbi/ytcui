#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# ytcui updater — Cross-platform (Linux, macOS, BSD)
# Works on Bash 3.2+ (macOS), Linux, BSD
# Can be invoked via: ytcui --upgrade
# ═══════════════════════════════════════════════════════════════════════════════

set -e

REPO_URL="https://github.com/MilkmanAbi/ytcui"
RAW_VERSION_URL="https://raw.githubusercontent.com/MilkmanAbi/ytcui/main/VERSION"
INSTALL_PREFIX="/usr/local"
DATA_DIR="/usr/local/share/ytcui"
BINARY_NAME="ytcui"

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

# ─── OS Detection ───────────────────────────────────────────────────────────────
OS_TYPE="unknown"
case "$(uname -s)" in
    Linux*)   OS_TYPE="linux" ;;
    Darwin*)  OS_TYPE="macos" ;;
    FreeBSD*) OS_TYPE="freebsd" ;;
    NetBSD*)  OS_TYPE="netbsd" ;;
    OpenBSD*) OS_TYPE="openbsd" ;;
    DragonFly*) OS_TYPE="dragonfly" ;;
esac

echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo -e "${BOLD}${CYAN}  ytcui — Update Checker${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo ""

info "Detected OS: ${BOLD}$OS_TYPE${NC}"

# ─── Get local version ──────────────────────────────────────────────────────────
LOCAL_VERSION_FILE="$DATA_DIR/VERSION"

if [[ -f "$LOCAL_VERSION_FILE" ]]; then
    LOCAL_VERSION=$(tr -d '[:space:]' < "$LOCAL_VERSION_FILE")
    ok "Local version: $LOCAL_VERSION"
else
    LOCAL_VERSION="0.0.0"
    warn "No local VERSION file found at $LOCAL_VERSION_FILE"
    warn "Assuming version $LOCAL_VERSION"
fi

# ─── Fetch remote version ───────────────────────────────────────────────────────
info "Checking for updates..."
if command -v curl >/dev/null 2>&1; then
    REMOTE_VERSION=$(curl -fsSL --max-time 10 \
        -H "Cache-Control: no-cache, no-store" \
        -H "Pragma: no-cache" \
        "$RAW_VERSION_URL?$(date +%s)" | tr -d '[:space:]') \
        || die "Failed to fetch remote version"
elif command -v wget >/dev/null 2>&1; then
    REMOTE_VERSION=$(wget -qO- --timeout=10 "$RAW_VERSION_URL" | tr -d '[:space:]') \
        || die "Failed to fetch remote version"
else
    die "Neither curl nor wget found"
fi

[[ -z "$REMOTE_VERSION" ]] && die "Could not parse remote version"

ok "Remote version: $REMOTE_VERSION"
echo ""

# ─── Version comparison function ────────────────────────────────────────────────
# Returns 0 if $1 < $2, 1 otherwise
version_lt() {
    IFS=. read -r L1 L2 L3 <<< "$1"
    IFS=. read -r R1 R2 R3 <<< "$2"
    L1=${L1:-0}; L2=${L2:-0}; L3=${L3:-0}
    R1=${R1:-0}; R2=${R2:-0}; R3=${R3:-0}

    if ((L1 < R1)); then return 0; fi
    if ((L1 > R1)); then return 1; fi
    if ((L2 < R2)); then return 0; fi
    if ((L2 > R2)); then return 1; fi
    if ((L3 < R3)); then return 0; fi
    return 1
}

# ─── Compare versions ───────────────────────────────────────────────────────────
if [[ "$LOCAL_VERSION" == "$REMOTE_VERSION" ]]; then
    echo -e "${GREEN}${BOLD}  You're up to date! ($LOCAL_VERSION)${NC}"
    echo ""
    exit 0
fi

if version_lt "$LOCAL_VERSION" "$REMOTE_VERSION"; then
    echo -e "${YELLOW}${BOLD}  Update available: $LOCAL_VERSION → $REMOTE_VERSION${NC}"
else
    echo -e "${CYAN}  Your version ($LOCAL_VERSION) is newer than remote ($REMOTE_VERSION)${NC}"
    echo "  You're running a development or unreleased version."
    echo ""
    exit 0
fi

# ─── Ask user to confirm update ────────────────────────────────────────────────
if [[ "$1" == "--yes" || "$1" == "-y" ]]; then
    REPLY="y"
else
    read -p "  Do you want to update? [y/N] " -n 1 -r
    echo ""
fi

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    info "Update cancelled"
    exit 0
fi

echo ""
info "Updating ytcui..."

# ─── Create temp directory ──────────────────────────────────────────────────────
TEMP_DIR=$(mktemp -d 2>/dev/null || mktemp -d -t ytcui)
trap 'rm -rf "$TEMP_DIR"' EXIT

# ─── Clone repo ─────────────────────────────────────────────────────────────────
info "Downloading latest version..."
git clone --depth 1 "$REPO_URL" "$TEMP_DIR/ytcui" || die "Git clone failed"
cd "$TEMP_DIR/ytcui" || die "Clone directory missing"

# ─── macOS brew ncurses fix ─────────────────────────────────────────────────────
if [[ "$OS_TYPE" == "macos" ]] && command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
    export PKG_CONFIG_PATH="$BREW_PREFIX/opt/ncurses/lib/pkgconfig:$PKG_CONFIG_PATH"
    export LDFLAGS="-L$BREW_PREFIX/opt/ncurses/lib $LDFLAGS"
    export CPPFLAGS="-I$BREW_PREFIX/opt/ncurses/include $CPPFLAGS"
fi

# ─── Determine make command ─────────────────────────────────────────────────────
MAKE_CMD="make"
if command -v gmake >/dev/null 2>&1; then
    MAKE_CMD="gmake"
fi

# ─── Build ──────────────────────────────────────────────────────────────────────
info "Building..."
$MAKE_CMD clean >/dev/null 2>&1 || true
$MAKE_CMD || die "Build failed"
ok "Build complete"

# ─── Install binary ─────────────────────────────────────────────────────────────
info "Installing binary..."
if [[ -w "$INSTALL_PREFIX/bin" ]]; then
    cp "$BINARY_NAME" "$INSTALL_PREFIX/bin/$BINARY_NAME" || die "Binary install failed"
    chmod 755 "$INSTALL_PREFIX/bin/$BINARY_NAME"
else
    sudo cp "$BINARY_NAME" "$INSTALL_PREFIX/bin/$BINARY_NAME" || die "Binary install failed"
    sudo chmod 755 "$INSTALL_PREFIX/bin/$BINARY_NAME"
fi
ok "Binary updated"

# ─── Update support files ───────────────────────────────────────────────────────
info "Updating support files..."
if [[ -w "$DATA_DIR" ]]; then
    cp update.sh "$DATA_DIR/update.sh" || die "Update script copy failed"
    cp VERSION "$DATA_DIR/VERSION" || die "VERSION copy failed"
    chmod 755 "$DATA_DIR/update.sh"
else
    sudo cp update.sh "$DATA_DIR/update.sh" || die "Update script copy failed"
    sudo cp VERSION "$DATA_DIR/VERSION" || die "VERSION copy failed"
    sudo chmod 755 "$DATA_DIR/update.sh"
fi
ok "Support files updated"

echo ""
echo -e "${GREEN}${BOLD}══════════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}  ✓ Updated to version $REMOTE_VERSION!${NC}"
echo -e "${GREEN}${BOLD}══════════════════════════════════════════${NC}"
echo ""
echo "  Run 'ytcui --version' to verify"
echo ""

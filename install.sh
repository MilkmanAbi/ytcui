#!/bin/sh
# ytcui installer — interactive front-end to OneInstallSystem (OIS).
# Asks a couple of setup questions, records the choices, then hands off to OIS
# which resolves dependencies, builds from source, and installs.
cd "$(dirname "$0")" || exit 1
chmod +x OIS/OIS.sh OIS/core/*.sh 2>/dev/null || true

# Non-interactive when there's no TTY (CI) or when --yes/--defaults is passed.
NONINTERACTIVE=0
for a in "$@"; do case "$a" in --yes|-y|--defaults|--no-config) NONINTERACTIVE=1 ;; esac; done
[ -t 0 ] || NONINTERACTIVE=1

BACKEND_CHOICE=ytcuidl
MODE_CHOICE=auto
GFX_CHOICE=blocks
THEME_CHOICE=default

if [ "$NONINTERACTIVE" = "0" ]; then
    printf '\n  \033[1mytcui setup\033[0m  (press Enter for the default)\n'

    printf '\n  Backend:\n'
    printf '    1) ytcui-dl  — native InnerTube client, no yt-dlp needed  [default]\n'
    printf '    2) yt-dlp    — shell out to yt-dlp (legacy)\n'
    printf '  Choice [1/2]: '
    read -r _r; case "$_r" in 2) BACKEND_CHOICE=ytdlp ;; esac

    printf '\n  Streamlined mode — a minimalist music-player UI for narrow terminals:\n'
    printf '    1) Auto      — switch automatically when the terminal is narrow  [default]\n'
    printf '    2) Off       — always use the full UI\n'
    printf '    3) Always    — always use the music-player UI\n'
    printf '  Choice [1/2/3]: '
    read -r _r; case "$_r" in 2) MODE_CHOICE=normal ;; 3) MODE_CHOICE=streamlined ;; esac

    printf '\n  Enable thumbnails (needs chafa)? [Y/n]: '
    read -r _r; case "$_r" in n|N|no) GFX_CHOICE=off ;; esac

    printf '\n  Theme — pick a colour scheme:\n'
    cat <<'THEMES'
     1) default    The classic. Clean terminal colours, no fuss.
     2) dracula    Deep purples and crimson accents. Creature of the night.
     3) nord       Arctic blues and soft greys. Nordic winter calm.
     4) tokyo      Neon city rain at midnight. Tokyo Night vibes.
     5) gruvbox    Warm wood and amber. Retro terminal cosiness.
     6) monokai    Vivid syntax colours. The classic dev palette.
     7) solarized  Precision-tuned tones. Easy on the eyes all day.
     8) pink       Soft sakura blossoms and blush petals at dawn.
     9) purple     Wisteria and lavender fields in the late afternoon.
    10) blue       Powder sky, periwinkle haze, and summer sea glass.
    11) green      Morning sage, honeydew, and botanical softness.
    12) mint       Cool spearmint foam and pale jade on a spring day.
    13) ocean      Pale turquoise coves and seafoam on still water.
    14) coral      Warm peach, apricot, and sun-kissed sandy blush.
    15) amber      Champagne fields, soft gold, and cornsilk warmth.
    16) red        Dusty rose, linen, and the blush of a gentle sunset.
    17) slate      Cool steel mist and powder blue-grey at dusk.
    18) grayscale  No colour. Just shape, light, and shadow.
    Not sure? Pick anything — you can change it anytime with: ytcui -t <name>
THEMES
    printf '  Choice [1-18]: '
    read -r _r
    case "$_r" in
        2) THEME_CHOICE=dracula ;;   3) THEME_CHOICE=nord ;;      4) THEME_CHOICE=tokyo ;;
        5) THEME_CHOICE=gruvbox ;;   6) THEME_CHOICE=monokai ;;   7) THEME_CHOICE=solarized ;;
        8) THEME_CHOICE=pink ;;      9) THEME_CHOICE=purple ;;   10) THEME_CHOICE=blue ;;
        11) THEME_CHOICE=green ;;   12) THEME_CHOICE=mint ;;     13) THEME_CHOICE=ocean ;;
        14) THEME_CHOICE=coral ;;   15) THEME_CHOICE=amber ;;    16) THEME_CHOICE=red ;;
        17) THEME_CHOICE=slate ;;   18) THEME_CHOICE=grayscale ;;
        *) THEME_CHOICE=default ;;
    esac

    [ "$BACKEND_CHOICE" = "ytdlp" ] && \
        printf '\n  \033[33mNote:\033[0m the yt-dlp backend needs yt-dlp installed (e.g. pip install yt-dlp).\n'
    printf '\n'
fi

# Backend choice → build override read by the Makefile (survives the OIS sudo
# re-exec because it lives in the project dir, not the environment).
printf 'BACKEND := %s\n' "$BACKEND_CHOICE" > .ytcui-build.conf 2>/dev/null || true

# UI mode + thumbnails → runtime config in the invoking user's config dir.
# Written now, as the real user, before OIS elevates for a system install.
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/ytcui"
CFG="$CFG_DIR/config.json"
mkdir -p "$CFG_DIR" 2>/dev/null || true
if command -v python3 >/dev/null 2>&1; then
    MODE="$MODE_CHOICE" GFX="$GFX_CHOICE" THEME="$THEME_CHOICE" CFGP="$CFG" python3 - <<'PY' 2>/dev/null || true
import json, os
p = os.environ["CFGP"]; d = {}
try:
    with open(p) as f: d = json.load(f)
except Exception:
    d = {}
d["mode"] = os.environ["MODE"]
d["graphics"] = os.environ["GFX"]
d["theme"] = os.environ["THEME"]
if os.environ["GFX"] == "off":
    d["show_thumbnails"] = False
try:
    with open(p, "w") as f: json.dump(d, f, indent=2)
except Exception:
    pass
PY
elif [ ! -f "$CFG" ]; then
    cat > "$CFG" <<EOF
{
  "mode": "$MODE_CHOICE",
  "graphics": "$GFX_CHOICE",
  "theme": "$THEME_CHOICE"
}
EOF
else
    printf '  (note: set "mode": "%s" in %s manually)\n' "$MODE_CHOICE" "$CFG"
fi

exec sh OIS/OIS.sh install "$@"

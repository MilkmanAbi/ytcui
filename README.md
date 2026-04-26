# ytcui (=^･ω･^=)

A fast, beautiful terminal YouTube client — search, play, and manage videos without leaving your shell.

Built in C++ with ncurses. Plays via **mpv**, fetches via **ytcui-dl** (built-in) or **yt-dlp**.

<img src="Pictures/ytcui.png" alt="ytcui screenshot">

---

## Install

```bash
git clone https://github.com/MilkmanAbi/ytcui.git && cd ytcui && chmod +x install.sh && ./install.sh
```

The installer walks you through everything — backend, thumbnails, theme — then builds and installs to your PATH.

### Supported Platforms

| Platform | Package Manager | Notes |
|----------|----------------|-------|
| **Linux** | apt, pacman, dnf, yum, zypper, apk, emerge, xbps | Full support |
| **macOS** | Homebrew or MacPorts | Auto-installs either if needed |
| **FreeBSD** | pkg | Native `procctl` support |
| **WSL2** | (same as Linux) | Works great |

---

## What's new in v3.0.0

**Playlists** — create named playlists, add videos from search results or history, reorder, remove, copy between playlists. Stored locally and persists across sessions.

**ytcui-dl** — a built-in InnerTube client that replaces yt-dlp entirely. Talks directly to YouTube's mobile API. Search results appear near-instantly, stream URLs are prefetched in the background while you browse. No Python, no subprocess overhead.

**Clickable titles everywhere** — click any video in History or Feed to search it up and land straight in Results with the full action menu. Pick audio, video, loop, whatever you want.

**Esc re-searches** — pressing Esc from Results re-runs your last query, so you can pick a different video without retyping anything.

**Coloured thumbnails** (EP1) — proper 256-colour rendering via ncurses colour pairs. No more monochrome silhouettes. Works on any 256-colour terminal without ghosting.

**18 themes** — eight established palettes plus ten new soft pastel colour schemes. All configurable per-element in `config.json`.

**Per-element colour customisation** — override any UI element's colour on top of any theme. Mix dracula with a custom accent, or build something entirely your own.

> Notes: ytcui's structure and build instructions changed >2.9.1, so if you want to install v3.0.0 by upgrading from an older version, it's recommended to manually uninstall first. Install v3.0.0 with the one liner install. >3.5.0, OIS will be introduced to prevent updates from breaking, Dynamically building based on remote versions make config. Please pardon the issues!!
---

## Keys

| Key | Action |
|-----|--------|
| `j` / `k` | Navigate up / down |
| `h` / `l` | Navigate left / right (tabs, menus) |
| `Tab` | Cycle panel focus |
| `Enter` | Select / open action menu |
| `Esc` | Back / re-search from Results |
| `/` | Jump to search |
| `p` | Pause / resume (global) |
| `s` | Sort & filter |
| `n` | New playlist (in Playlists tab) |
| `g` / `G` | Jump to top / bottom of results |
| `q` | Quit |

Mouse works everywhere — click tabs, result rows, action items, info panel, scroll wheel navigates.

---

## Tabs

| Tab | What's there |
|-----|-------------|
| **Library** | Subscriptions (left) + saved videos (right) |
| **Playlists** | All your playlists — open, create, manage |
| **Feed** | Recently watched + subscribed channels |
| **History** | Everything you've played — click any title to search it |
| **Results** | Your last search, always accessible while music plays |

---

## Usage

```bash
ytcui                      # launch
ytcui -t pink              # sakura theme
ytcui -t dracula           # dracula theme
ytcui --colors             # list all colour elements + config example
ytcui --debug              # enable debug logging
ytcui --debug --logdump    # full mpv output logging
ytcui --upgrade            # upgrade to latest version
ytcui --diag               # full system diagnostic
ytcui --help               # help
ytcui --version            # version
```

---

## Themes

Ten new soft pastel themes joined the classics in v3.0.0:

| Theme | Vibe |
|-------|------|
| `default` | Clean terminal colours |
| `dracula` | Deep purples and crimson. Creature of the night |
| `nord` | Arctic blues and soft greys. Nordic winter calm |
| `tokyo` | Neon city rain at midnight |
| `gruvbox` | Warm wood and amber. Retro cosiness |
| `monokai` | Vivid syntax colours. The classic dev palette |
| `solarized` | Precision-tuned tones. Easy on the eyes all day |
| `pink` | Soft sakura blossoms and blush petals at dawn |
| `purple` | Wisteria and lavender fields in the late afternoon |
| `blue` | Powder sky, periwinkle haze, summer sea glass |
| `green` | Morning sage, honeydew, and botanical softness |
| `mint` | Cool spearmint foam and pale jade on a spring day |
| `ocean` | Pale turquoise coves and seafoam on still water |
| `coral` | Warm peach, apricot, and sun-kissed sandy blush |
| `amber` | Champagne fields, soft gold, and cornsilk warmth |
| `red` | Dusty rose, linen, the blush of a gentle sunset |
| `slate` | Cool steel mist and powder blue-grey at dusk |
| `grayscale` | No colour. Just shape, light, and shadow |

Switch anytime: `ytcui -t mint`

---

## Colour customisation

Override any UI element on top of any base theme. Add a `"colors"` block to your config:

```json
{
  "theme": "dracula",
  "colors": {
    "accent": 198,
    "title":  213,
    "border": 141
  }
}
```

Run `ytcui --colors` for the full element reference and a 256-colour chart link.

**Elements:** `bg`, `search_box`, `title`, `channel`, `stats`, `selected`, `action`, `action_sel`, `status`, `border`, `header`, `accent`, `tag`, `published`, `bookmark`, `desc`

---

## Backend: ytcui-dl vs yt-dlp

ytcui ships with two backends, chosen at install time (switchable by rebuilding):

| | ytcui-dl | yt-dlp |
|---|---|---|
| **Speed** | Near-instant | 2–5s per video |
| **Dependencies** | None (built-in) | Python + yt-dlp |
| **How it works** | InnerTube API (YouTube mobile) | JS player extraction |
| **Stability** | Experimental | Battle-tested |
| **Recommended** |  Yes | If ytcui-dl breaks |

The backend is baked in at compile time. Rebuild to switch:
```bash
make BACKEND=ytcuidl   # default
make BACKEND=ytdlp
```

---

## Config

`~/.config/ytcui/config.json`

```json
{
  "theme": "pink",
  "max_results": 15,
  "show_thumbnails": true,
  "colors": {}
}
```

Data lives in:
- `~/.config/ytcui/` — config
- `~/.local/share/ytcui/` — library, history, playlists
- `~/.cache/ytcui/` — thumbnails, debug log

---

## Building manually

```bash
make BACKEND=ytcuidl   # build with ytcui-dl (default)
make BACKEND=ytdlp     # build with yt-dlp
make clean             # clean
```

**Dependencies by platform:**

| Platform | Build deps | Runtime |
|----------|-----------|---------|
| Linux | `g++`, `make`, `libncursesw-dev`, `libcurl-dev`, `libssl-dev` | `mpv`, `chafa` |
| macOS | Xcode CLT, Homebrew/MacPorts `ncurses`, `curl`, `openssl@3` | `mpv`, `chafa` |
| FreeBSD | `clang++`, `gmake`, `ncurses`, `curl`, `openssl` | `mpv`, `chafa` |

`nlohmann/json` is bundled — no separate install needed.

---

## Updating

```bash
ytcui --upgrade
```

The updater clones the latest release, detects your current backend, rebuilds, and reinstalls — all in one command. The update script lives at `/usr/local/share/ytcui/update.sh`, so you don't need the original repo folder.

---

## Troubleshooting

**UTF-8 looks garbled** — set your terminal locale:
```bash
export LANG=en_US.UTF-8   # add to ~/.bashrc or ~/.zshrc
```

**Age-restricted videos fail** — use browser auth from the action menu (Login via browser cookies).

**macOS: `command not found: brew`** — after Homebrew installs, add to `~/.zshrc`:
```bash
eval "$(/opt/homebrew/bin/brew shellenv)"   # Apple Silicon
eval "$(/usr/local/bin/brew shellenv)"      # Intel
```

**Thumbnails are missing or broken** — check `chafa` is installed: `which chafa`. Run `ytcui --diag` for a full system check.

**Debug mode** — run `ytcui --debug --logdump` to capture detailed logs to `~/.cache/ytcui/`.

---

## License

MIT

---

*ytcui values simplicity over flashiness, portability over lock-in, and clean code over clever abstractions. It's a practical terminal client that aims to stay small, readable, and reliable — something you can actually open the source of and understand.*

*Made with (=^･ω･^=) and ncurses*

---

If you found like this, please 🌟 it, makes me feel fuzzy and nice inside. ૮ ˶ᵔ ᵕ ᵔ˶ ა

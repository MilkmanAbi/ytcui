# ytcui 5.0.4 (=^･ω･^=)

A fast, beautiful terminal YouTube client — search, play, and manage videos without leaving your shell.

Built in C++17 with ncurses. Plays via mpv, fetches via **ytcui-dl** (built-in, experimental YouTube client of mine), **ytcui-we**, or the mature **yt-dlp**.

<p align="center">
  <img src="Pictures/00-search-results.png" alt="ytcui v5 search results alongside the selected video's thumbnail, channel, duration and view count" width="1100">
  <br>
  <em>ytcui running in Windows Terminal under WSL. >_<</em>
</p>

---

## Gallery

<details>
<summary><strong>Search · sorting and filters</strong></summary>

<p align="center">
  <img src="Pictures/01-sort-and-filter.png" alt="Sorting and filtering options over the search results" width="1100">
  <br>
  <em>Sort by relevance, upload date, view count or rating, and filter the type or length of results.</em>
</p>

</details>

<details>
<summary><strong>Your collection · library, playlists, feed and history</strong></summary>

<p align="center">
  <img src="Pictures/02-library.png" alt="Library with subscriptions and saved videos" width="1100">
  <br>
  <em>Library keeps channel subscriptions and saved videos together.</em>
</p>

<p align="center">
  <img src="Pictures/03-playlists.png" alt="Local playlists with their video counts" width="1100">
  <br>
  <em>Create and browse local playlists, see video counts, and assign playlist colours.</em>
</p>

<p align="center">
  <img src="Pictures/04-feed.png" alt="Feed with recently played videos and subscribed channels" width="1100">
  <br>
  <em>Feed puts recently played videos beside your subscribed channels.</em>
</p>

<p align="center">
  <img src="Pictures/05-watch-history.png" alt="Watch history showing video titles and channel names" width="1100">
  <br>
  <em>Revisit watch history; selecting an entry searches for it and returns to results.</em>
</p>

</details>

<details>
<summary><strong>Make it yours · themes, keybindings and general settings</strong></summary>

<p align="center">
  <img src="Pictures/06-settings-themes.png" alt="Settings Theme tab with the available colour themes" width="1100">
  <br>
  <em>Change themes live from Settings, without restarting.</em>
</p>

<p align="center">
  <img src="Pictures/07-settings-keybindings.png" alt="Settings Accelerators tab with configurable playback and navigation keys" width="1100">
  <br>
  <em>Rebind playback and navigation keys in the Accelerators tab.</em>
</p>

<p align="center">
  <img src="Pictures/08-settings-general.png" alt="General settings for backend, UI mode, SponsorBlock, resume and looping" width="1100">
  <br>
  <em>Choose a backend or interface mode and configure SponsorBlock, resume, looping and interface hints.</em>
</p>

</details>

<details>
<summary><strong>Playback tools · sleep timer and QR sharing</strong></summary>

<p align="center">
  <img src="Pictures/09-settings-timer.png" alt="Settings Timer tab with sleep timer and loop on end options" width="1100">
  <br>
  <em>Manage the sleep timer and loop-on-end option from Settings.</em>
</p>

<p align="center">
  <img src="Pictures/10-sleep-timer.png" alt="Sleep timer dialog accepting durations such as 30m, 1h23m and 45s" width="1100">
  <br>
  <em>Set a sleep timer using a duration such as 30m, 1h23m or 45s.</em>
</p>

<p align="center">
  <img src="Pictures/11-qr-sharing.png" alt="Share dialog with a complete QR code, video URL and copy command" width="1100">
  <br>
  <em>Scan the QR code or press c to copy the URL; m switches between cell and compact rendering.</em>
</p>

</details>

<details>
<summary><strong>Small terminals · streamlined mode</strong></summary>

<p align="center">
  <img src="Pictures/12-streamlined-menu.png" alt="Narrow streamlined menu with Search, Library, Playlists, Feed and History" width="440">
  <br>
  <em>The streamlined section menu keeps navigation available in a narrow terminal.</em>
</p>

<p align="center">
  <img src="Pictures/13-streamlined-search.png" alt="Streamlined search screen with a focused input field and keyboard hints" width="440">
  <br>
  <em>Compact search: type a query, press Enter, or Escape to return.</em>
</p>

</details>

---

## Install

```bash
git clone https://github.com/MilkmanAbi/ytcui-test-repo.git && cd ytcui-test-repo && sh install.sh
```

The installer walks you through a short setup Q&A — package manager (Homebrew or MacPorts on macOS), video backend, colour theme, and mpv hardware acceleration — then hands off to OIS which builds from source and installs to your PATH. On macOS it offers to bootstrap Homebrew or fetch the right MacPorts `.pkg` if neither is installed.

Manage your install afterwards with:

```bash
ytcui --ois            # status + update panel
ytcui --update         # update to the latest version
ytcui --reinstall      # clean rebuild from source
ytcui --uninstall      # remove cleanly
ytcui --install-info   # full install details + dependency status
```

### Supported Platforms

| Platform | Package Manager | Notes |
|----------|----------------|-------|
| **Linux** | apt, pacman, dnf, yum, zypper, apk, emerge, xbps | Full support |
| **macOS** | Homebrew or MacPorts | Auto-installs either if needed |
| **FreeBSD** | pkg | Native `procctl` support |
| **NetBSD** | pkgin | |
| **OpenBSD** | pkg_add | |
| **Illumos/OmniOS** | ips (pkg) | |
| **WSL2** | (same as Linux) | Works great |

---

## What's new in v5

### Three backends, no rebuild

**Ctrl-B** cycles through **ytcui-dl → ytcui-we → yt-dlp**. You can also choose a backend in **Ctrl-S → General**. ytcui-dl is built in; ytcui-we ships as a companion executable; yt-dlp is an optional external backend.

### Playback

**Resolution selector.** Press `r` to open the quality ladder for a resolved video. Your preferred height is saved across launches. The choices depend on the backend and streams available for that video.

**Queue.** Add videos to the queue or choose **Play next** from the action menu. `Ctrl-Q` opens the queue.

**Resume and looping.** Pick up where you left off, loop the current video, or keep a playlist going. Set your preferences in **Ctrl-S → General**.

**SponsorBlock.** Skip configured sponsor and other segment categories. Toggle it in Settings.

Pause/resume, volume and seeking still use mpv's JSON IPC socket: `Space` or `p` pauses, `+`/`-` changes volume, and `<`/`>` seeks. The now-playing strip shows elapsed time and playback progress.

### Sleep timer

Press `Ctrl-T`, then enter a duration such as `30m`, `1h23m` or `45s`. `Enter` sets the timer; `Ctrl-T` cancels it. Timer options also live in Settings.

### Share (QR + clipboard)

Open **Share** from a video's action menu to show a QR code. Scan it on your phone, or press `c` to copy the link. `m` switches between full-cell and compact QR rendering; `Esc` closes the dialog.

### In-app settings (Ctrl-S)

- **Theme** — change colours live, no restart.
- **Accelerators** — select an action and press a new key to rebind it.
- **General** — backend, UI mode, SponsorBlock, resume, looping, status bar and hints.
- **Timer** — sleep timer and loop-on-end options.
- **About** — version information.

Changes save to `config.json`. Press `?` for the shortcuts reference.

### Terminal handling

Both layouts support the same settings and dialogs. UTF-8 input, narrow windows, low-colour terminals and QR rendering have had a portability pass, including full-cell QR rendering by default on macOS Terminal.app.

---

## Keys

All playback and navigation keys are configurable via `Ctrl-S` → Accelerators, or the `"keys"` block in `config.json`.

| Key | Action |
|-----|--------|
| `j` / `k` | Navigate down / up |
| `h` / `l` | Navigate left / right (tabs, menus) |
| `Tab` | Cycle panel focus |
| `Enter` | Select / open action menu |
| `Esc` | Back / cancel |
| `/` | Jump to search (global — works from anywhere) |
| `Space` / `p` | Pause / resume playback (global) |
| `+` / `-` | Volume up / down ±5% (global) |
| `<` / `>` | Seek back / forward 10s (global) |
| `s` | Sort & filter |
| `n` | New playlist (in Playlists tab) |
| `g` / `G` | Jump to top / bottom of results |
| `q` | Quit |
| `?` | Shortcuts reference panel |
| `Ctrl-S` | In-app settings |
| `Ctrl-B` | Cycle backend: ytcui-dl → ytcui-we → yt-dlp |
| `Ctrl-Q` | Open / close the queue |
| `Ctrl-T` | Sleep timer |
| `r` | Resolution selector |

Mouse works everywhere — click tabs, result rows, action items, scroll wheel navigates.

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

## Streamlined mode

On very narrow terminals ytcui automatically switches to a dense, minimalist music-player layout that follows your theme colours:

- An iPod-style section menu mirroring the normal tabs — **Search · Library · Playlists · Feed · History** (plus Now Playing) — navigate with `j`/`k` and `Enter`.
- Each section opens a compact list (title + channel · duration); selecting an item gives a **Play video / Play audio** chooser.
- A now-playing card with album-art thumbnail, waveform, real elapsed/total time, title/artist and transport controls. `Space` pauses, `+`/`-` adjust volume, `<`/`>` seek, `Ctrl-S` opens settings, `b`/`Esc` goes back, `q` quits.

Force it either way:

```bash
ytcui --mode streamlined   # always the music-player UI
ytcui --mode normal        # always the full UI
ytcui --mode auto          # default: switch when narrow
```

---

## Usage

```bash
ytcui                      # launch
ytcui -t pink              # sakura theme
ytcui -t dracula           # dracula theme (persists to config)
ytcui --gfx sixel          # real-image thumbnails (sixel/kitty/iterm/blocks/auto/off)
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

18 themes, switchable with `ytcui -t <name>` or live in-app with `Ctrl-S`. The choice persists across launches.

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

## Backends

Switch any time with `Ctrl-B` or **Ctrl-S → General**. No rebuild needed.

| Backend | How it works | What you need |
|---------|--------------|---------------|
| **ytcui-dl** | Native C++ YouTube extraction; the default | Built in |
| **ytcui-we** | C++ resolver with player-script and session support | Companion executable and a supported JavaScript runtime |
| **yt-dlp** | External YouTube extractor | Install yt-dlp separately |

ytcui-we resolves streams through its own executable, and mpv handles playback. The companion is built and installed alongside ytcui. Run `ytcui-we --check-runtime` to check its JavaScript runtime.

Browser-cookie login is available from the action menu. YouTube changes can affect extraction; if a video fails, try another backend and keep yt-dlp up to date.

---

## Config

`~/.config/ytcui/config.json`

```json
{
  "theme": "pink",
  "max_results": 15,
  "show_thumbnails": true,
  "no_hardware_accel": false,
  "keys": {
    "search":      "/",
    "pause":       " ",
    "volume_up":   "+",
    "volume_down": "-",
    "seek_fwd":    ">",
    "seek_back":   "<",
    "quit":        "q",
    "scroll_up":   "k",
    "scroll_down": "j",
    "top":         "g",
    "bottom":      "G",
    "sort":        "s",
    "new_playlist": "n"
  },
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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
./build/ytcui

# Install ytcui, the ytcui-we companion and its player-script assets:
cmake --install build --prefix "$HOME/.local"
```

The Makefile wraps CMake, so `make`, `make clean` and `make tests` are also available. On macOS it supplies Homebrew dependency paths automatically.

**Dependencies by platform:**

| Platform | Build deps | Playback |
|----------|------------|----------|
| Linux / WSL | C++17 compiler, CMake 3.16+, ncursesw, OpenSSL, zlib, libcurl development packages | mpv |
| macOS | Xcode CLT, CMake, Homebrew/MacPorts ncurses, OpenSSL, zlib, curl | mpv |
| BSDs | C++17 compiler, CMake, ncurses, OpenSSL, zlib, curl | mpv |

On Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake pkg-config \
  libncursesw5-dev libssl-dev zlib1g-dev libcurl4-openssl-dev mpv
```

On macOS with Homebrew:

```bash
brew install cmake ninja pkg-config ncurses openssl zlib curl mpv
make
```

`chafa` is optional for thumbnails. `ffmpeg` is optional for download conversion and muxing. Install `yt-dlp` to use the external backend.

`nlohmann/json` and the QR encoder are bundled — no separate install needed. Keep the `build/vendor/ejs/` assets with a local build, or use the install command above.

To run the tests:

```bash
ctest --test-dir build --output-on-failure
```

---

## Troubleshooting

**UTF-8 looks garbled** — set your terminal locale:
```bash
export LANG=en_US.UTF-8   # add to ~/.bashrc or ~/.zshrc
```

**Age-restricted videos fail** — use browser auth from the action menu (Login via browser cookies).

**macOS: `command not found` after install** — OIS writes `~/.local/bin` to your shell rc files automatically, but you may need to open a new terminal (or run `source ~/.zshrc`) for it to take effect. If it still isn't found, check `echo $PATH` includes `~/.local/bin`, or add it manually.

**macOS: `command not found: brew`** — after Homebrew installs, add to `~/.zshrc`:
```bash
eval "$(/opt/homebrew/bin/brew shellenv)"   # Apple Silicon
eval "$(/usr/local/bin/brew shellenv)"      # Intel
```

**Thumbnails are missing or broken** — check `chafa` is installed: `which chafa`. Run `ytcui --diag` for a full system check.

**Debug mode** — run `ytcui --debug --logdump` to capture detailed logs.

**QR does not fit** — enlarge the terminal or press `m` in the share dialog for compact rendering. Press `c` to copy the link instead. Terminal.app uses full-cell rendering by default; compact blocks depend on your font and character-width settings.

---

## Acknowledgements

[yt-dlp](https://github.com/yt-dlp/yt-dlp) is the external fallback backend and a major reference for the YouTube extraction work. Playback uses [mpv](https://mpv.io/), and QR encoding uses [Nayuki’s QR Code generator](https://www.nayuki.io/page/qr-code-generator-library). Bundled components retain their own license notices.

---

## License

MIT

---

*ytcui values simplicity over flashiness, portability over lock-in, and clean code over clever abstractions. It's a practical terminal client that aims to stay small, readable, and reliable — something you can actually open the source of and understand.*

*Made with (=^･ω･^=) and ncurses*

---

## Connected Project Repositories

- [ytcui](https://github.com/MilkmanAbi/ytcui)
- [ytcui-dl](https://github.com/MilkmanAbi/ytcui-dl)
- [ytcui-we](https://github.com/MilkmanAbi/ytcui-we)
- [ytcui-termux](https://github.com/MilkmanAbi/ytcui-termux)
- [ytcui-dotmatrix](https://github.com/MilkmanAbi/ytcui-dotmatrix)
- [OIS](https://github.com/MilkmanAbi/oneinstallsystem)

> These are some of the ytcui* stuff. ytcui-dl is the library driving it all. ytcui-termux is for android, runs on termux!

---

If you like this, please 🌟 it — makes me feel fuzzy and nice inside. ૮ ˶ᵔ ᵕ ᵔ˶ ა
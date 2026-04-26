# ytcui 🐱

> **v3.0.0 with playlist features, better history tab, clickable links delayed slightly because I'm having a horrible code block, can't code well at the moment, brain tired.
> **v2.5.0** — Cross-platform release! Now runs natively on macOS and BSD!

**ytcui 3.0.0 Alpha with ytcui-dl in place of yt-dlp and using TinyRenderCache coming soon, expect speedups :) Heavy rewriting and optimising cross platform speed consistency, yayy**

A terminal YouTube client. Search, play, and manage videos without leaving your terminal.
Built in C++ with ncurses. Plays audio and video via mpv, fetches via yt-dlp.

<img src="Pictures/ytcui.png" alt="ytcui screenshot">

---

## Install

**One-liner:**

```bash
git clone https://github.com/MilkmanAbi/ytcui.git && cd ytcui && chmod +x install.sh && ./install.sh
```

The installer auto-detects your OS and package manager, handles dependencies, builds, and installs `ytcui` to your PATH.

### Supported Platforms

| Platform    | Package Manager                                  | Notes                            |
| ----------- | ------------------------------------------------ | -------------------------------- |
| **Linux**   | apt, pacman, dnf, yum, zypper, apk, emerge, xbps | Full support                     |
| **macOS**   | Homebrew                                         | Auto-installs Homebrew if needed |
| **FreeBSD** | pkg                                              | Native `procctl` support         |
| **WSL2**    | (same as Linux)                                  | Works great under WSL            |

**Then just run:**

```bash
ytcui
```

---

## Update

ytcui checks for updates automatically on startup. If an update is available, you'll see:

```
⬆ Update available: 2.0.0 → 2.1.0 (run ytcui --upgrade) (NOTE: Upgrade tool introduced in 2.0.1, if your version is < 2.0.1, please manually remove binary and reinstall, from that point, 2.0.1 and >2.0.1 will auto upgrade cleanly)
```

To upgrade:

```bash
ytcui --upgrade
```

That's it! No need to keep the repo folder — the update script is installed to `/usr/local/share/ytcui/`.

To skip the startup update check: `ytcui --no-update-check`

---

## What it does

* **Search & play** — search YouTube and play audio or video instantly
* **Library** — bookmark videos and subscribe to channels, persisted locally
* **Watch history** — last 100 items, browseable
* **Browser auth** — login with browser cookies to access age-restricted content
* **Download** — save video or audio to disk
* **UTF-8** — full CJK, emoji, and international text support
* **Mouse + keyboard** — click anything or use vim keys
* **Themes** — 8 color schemes to choose from

---

## Keys

| Key          | Action                    |
| ------------ | ------------------------- |
| `Tab`        | Cycle focus               |
| `j` / `k`    | Up / down                 |
| `h` / `l`    | Left / right              |
| `Enter`      | Select / open action menu |
| `Esc`        | Back / cancel             |
| `/`          | Focus search              |
| `p`          | Pause / resume (global)   |
| `s` or `...` | Sort & filter             |
| `q`          | Quit                      |

Mouse works too — click tabs, results, the `...` button, scroll to navigate.

---

## Usage

```bash
ytcui                     # normal (default theme)
ytcui --theme dracula     # dracula color scheme
ytcui -t nord             # nord theme
ytcui --grayscale         # minimal blue/gray (legacy)
ytcui --debug             # enable debug logging
ytcui --debug --logdump   # full mpv/yt-dlp output logging
ytcui --upgrade           # upgrade to latest version
ytcui --help              # help
ytcui --version           # show version
```

---

## Themes

| Theme       | Description                              |
| ----------- | ---------------------------------------- |
| `default`   | Vibrant terminal colors                  |
| `grayscale` | Minimal Nord-inspired blues              |
| `nord`      | Arctic, bluish palette                   |
| `dracula`   | Dark purple/pink vampire theme           |
| `solarized` | Precision colors for machines and people |
| `monokai`   | Classic code editor theme                |
| `gruvbox`   | Retro groove colors                      |
| `tokyo`     | Tokyo Night dark theme                   |

Set permanently in `~/.config/ytcui/config.json`:

```json
{
  "theme": "dracula"
}
```

---

## Action menu

Select a video and press `Enter`:

* Play video / Play audio / Play audio (loop)
* Pause / resume
* View channel
* Subscribe / unsubscribe
* Open in browser
* Bookmark / unbookmark
* Download video / audio
* Copy URL
* Login via browser cookies
* Logout

---

## Tabs

* **Library** — subscriptions (left) + saved videos (right)
* **Feed** — recently watched + subscribed channels
* **History** — everything you've played
* **Results** — your last search, always accessible while music plays

---

## Config

`~/.config/ytcui/config.json`

```json
{
  "max_results": 15,
  "theme": "default",
  "grayscale": false
}
```

Data lives in:

* `~/.config/ytcui/` — config
* `~/.local/share/ytcui/` — library, history
* `~/.cache/ytcui/` — debug log, thumbnails

---

## Customising

Colors, the koala, and status bar messages are all editable directly in the source. See [`CUSTOMISING.md`](CUSTOMISING.md) for exact file names and line numbers.

---

## Building manually

```bash
make        # build
make clean  # clean
```

**Linux:** `g++`, `make`, `libncursesw-dev`, `mpv`, `yt-dlp`
**macOS:** `clang++` (Xcode CLT), Homebrew's `ncurses`, `mpv`, `yt-dlp`
**FreeBSD:** `clang++`, `gmake`, `ncurses`, `mpv`, `yt-dlp`

Optional: `chafa` (thumbnails), `curl` (async thumbnail downloads)

`nlohmann/json` is bundled — no separate install needed.

---

## Troubleshooting

**UTF-8 looks garbled** — make sure your terminal locale is UTF-8:

```bash
export LANG=en_US.UTF-8
```

Add to `~/.bashrc` or `~/.zshrc` to make it permanent.

**Age-restricted videos fail** — use browser auth from the action menu.

**Audio keeps playing after quit** — update to v2.5.0+; the cross-platform process group fix handles this on all platforms.

**Audio glitches/weird playback** — v2.0.0+ includes stability fixes with better format selection and buffering.

**macOS: "command not found: brew"** — after installing Homebrew, add to `~/.zshrc`:

```bash
eval "$(/opt/homebrew/bin/brew shellenv)"  # Apple Silicon
# or
eval "$(/usr/local/bin/brew shellenv)"     # Intel Mac
```

**macOS: processes linger after quit** — v2.5.0 uses `kqueue(EVFILT_PROC)` for reliable child cleanup.

**Debug mode** — run with `ytcui --debug --logdump` to capture detailed logs to `~/.cache/ytcui/`.

---

## License

MIT

---

Many YouTube TUIs already exist, and many of them are more visually polished or feature-heavy. **ytcui is intentionally simplistic by design.**

Its goal is not to compete on flashiness, but on clarity and maintainability.

ytcui aims to remain readable, approachable, and easy to hack on. The codebase is written entirely in C++ and structured to be understandable rather than clever. If you open the source, you should be able to follow what it’s doing without fighting layers of abstraction or framework complexity.

Another core principle is **minimal dependency bloat**. ytcui relies only on portable, widely available tools (such as `ncurses`, `mpv`, and `yt-dlp`) instead of large frameworks or language runtimes. This keeps the install lightweight and avoids dependency hell.

Portability is a first-class goal. ytcui is designed to build and run cleanly across:

* Linux / GNU distributions
* BSD systems
* macOS

The update system is also designed to be clean and predictable — no hidden package manager conflicts, no breaking system libraries, no runtime environments to manage. Just a straightforward upgrade path that keeps the program self-contained and stable.

In short, ytcui values:

* Simplicity over flashiness
* Maintainability over complexity
* Portability over ecosystem lock-in
* Clean updates over fragile dependency chains

It’s a practical terminal client that aims to stay small, understandable, and reliable over time.

---

*Made with 🐱 and ncurses*

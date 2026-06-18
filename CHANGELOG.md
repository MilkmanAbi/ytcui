# Changelog

All notable changes to ytcui will be documented in this file.

## [3.5.6] - 2026-06-18

### Added
- **Strict mlterm hardening.** When ytcui detects it is running inside mlterm —
  or you pass `--mlterm` (also `--mono` / `--bw`, or set `"theme": "mlterm"`) —
  the UI is driven in pure black & white with no per-element colour, no
  bold/dim emphasis (mlterm renders those as reverse-video blocks, which is the
  "highlight glitching" some builds showed), plain reverse-video selection
  instead of a coloured bar, and **no thumbnails of any kind** — neither sixel
  nor chafa block art, so the bytes that garble a no-imagelib mlterm are never
  emitted. The active tab is marked with brackets (`[Results]`) so you can still
  see which menu you are on without relying on colour or bold.
- New `--mlterm` flag (treat the terminal as mlterm and harden fully, even if it
  did not self-identify) and `--mono` / `--bw` aliases for the black & white
  `mlterm` theme. `ytcui --diag` now reports a `mono-harden` line.
- `--injectconfig mlterm` (also `mono` / `bw`) saves the strict B&W mlterm theme
  as the default, for users who want black & white on every terminal.

### Changed
- **mlterm is now reliably auto-detected.** Detection also keys off the `MLTERM`
  environment variable, which mlterm always exports — it usually leaves `$TERM`
  as `xterm`/`xterm-256color`, so the old `$TERM`-only check missed stock mlterm
  setups. The same install now adapts per terminal automatically: colours and
  thumbnails on a normal terminal (WSL, xterm, etc.), strict black & white the
  moment you open mlterm, with no flags or config. The `MLTERM` signal is
  ignored inside tmux/screen, where the multiplexer is what renders.
- mlterm hardening is decided in one place (`TermCaps::mono_hardening`) and is
  **non-negotiable**: it is applied before the `force_features` escape hatch, so
  "honour my config verbatim" can never re-enable thumbnails or raster on a
  terminal that would render them as garbage.
- The debug log no longer claims "chafa missing" when thumbnails were disabled
  for some other reason (e.g. hardening); it now distinguishes "disabled" from
  an actually-absent chafa.

## [3.5.5] - 2026-06-14

### Fixed
- **No more sixel garbage in mlterm (and any non-sixel terminal).** The
  thumbnail "character soup" some terminals showed (e.g. `^[P0;1;0q"1;1;153…`)
  was a real sixel image being dumped as text: ytcui's detection hardcoded
  "sixel = yes" for any terminal identifying as mlterm, but sixel is a *build
  option* in mlterm — the MacPorts / SDL / framebuffer builds ship without it.
  Sixel support is now gated strictly on the authoritative DA1 probe (attribute
  `4` in the Primary Device Attributes reply), exactly as the VT spec and
  notcurses/libsixel require. mlterm without sixel now correctly uses block art.
- **Explicit `--gfx sixel` (and kitty/iterm) now refuse gracefully** when the
  terminal's capability probe contradicts the request, falling back to block
  art with a one-line notice instead of corrupting the screen.
- Added an authoritative clamp: if a terminal answers DA1 *without* advertising
  sixel, sixel is forced off regardless of any per-terminal heuristic.
- `ytcui --diag` now reports the sixel probe result (`DA1 seen` / `DA1
  advertised sixel(4)`) so this is debuggable at a glance.

### Added
- **Capability auto-override (safety net).** If the terminal genuinely can't do
  a feature — no colour (< 8), no chafa, or a raster protocol it never confirmed
  — ytcui now force-disables that feature even if it's enabled in config, so a
  weak or misconfigured terminal can never end up with a corrupted UI (block art
  with no colour, sixel bytes dumped as text, etc.). Forced `--gfx sixel`/`kitty`/
  `iterm` falls back to block art unless the terminal positively confirmed the
  protocol. Set `"force_features": true` in config.json to suppress the
  auto-override and honour your settings verbatim (you accept the risk). The
  override decisions are written to the debug log.

## [3.5.4] - 2026-06-11

### Fixed
- **macOS Homebrew install actually works now.** The real root cause: OIS
  re-execs itself through `sudo` for a system install, so the dependency phase
  was running as **root** — and Homebrew refuses to run as root, making every
  `brew install` fail (even though `brew install curl` works fine when you run
  it yourself). On macOS, OIS now resolves and installs dependencies as the
  invoking user *first*, then elevates with `sudo` only for the final copy into
  `/usr/local/bin`. The resolved package manager and `PKG_CONFIG_PATH` are
  carried across the privilege boundary so the build still finds everything.
- Homebrew's `curl`, `ncurses`, `openssl@3` are keg-only — their `.pc` files
  aren't on the default `PKG_CONFIG_PATH`. OIS now primes `PKG_CONFIG_PATH` with
  each keg's prefix before dependency checks and the build (and updates it live
  after each keg install), so pkg-config finds them.
- The package-manager prompt on macOS now always asks when both Homebrew and
  MacPorts are present, and is correctly skipped (not re-prompted as root) on
  the elevated pass.
- Homebrew pkg-config formula corrected to `pkgconf`; all deps gained verified
  `.macports` port names; MacPorts bootstrap opens the installer page and waits.

## [3.5.3] - 2026-06-11

### Fixed
- **Borders no longer garble on terminals where ACS line drawing fails**
  (mlterm, bobcat, PuTTY-class emulators that render boxes as literal
  `l q k x m j` letters). ytcui no longer uses ncurses ACS/VT100 alternate-
  charset drawing at all: borders are real Unicode box-drawing glyphs in UTF-8
  locales and plain ASCII `+ - |` everywhere else.
- **Non-UTF-8 locales (Latin-1, C/POSIX, legacy terminal encodings) no longer
  turn the UI into byte garbage.** ytcui now detects the locale charset
  (nl_langinfo) at startup; when it isn't UTF-8, every multibyte glyph is
  avoided: thumbnails render with `chafa --symbols ascii`, decorative symbols
  fall back to ASCII, and non-ASCII bytes in titles are stripped instead of
  being emitted raw. `ytcui --diag` now reports the detected codeset.
- **`make install` works again** (MacPorts destroot was failing): the install
  target referenced the removed `update.sh`. It now installs the binary and
  VERSION only; lifecycle management belongs to OIS or the package manager.
- **The macOS "Apple system ncurses (5.7)" warning is now accurate.** The
  Makefile probes the curses.h the compiler will actually use (via the
  preprocessor) instead of guessing from Homebrew paths, so MacPorts builds
  linking ncurses 6 are no longer told to install Homebrew. ncurses detection
  itself now tries pkg-config, then Homebrew, then MacPorts (/opt/local),
  then the system library.

## [3.5.2] - 2026-06-10

### Added
- The installer asks which **theme** to use, with the original clean one-line
  descriptions for all 18 built-ins (and a "change it anytime with ytcui -t"
  hint), and writes the choice to the config.

## [3.5.0] - 2026-06-10

### Changed
- **Streamlined mode, properly.** The narrow-terminal music-player UI was
  reworked to be dense, neat, and fully theme-coloured:
  - The menu now mirrors the normal-mode sections — Search, Library, Playlists,
    Feed, History (plus Now Playing) — each with an icon, navigated with j/k and
    Enter, and you can switch between them cleanly.
  - Each section opens a dense list (title + dim channel · duration, LIVE for
    streams) with the same cute empty-state messages as the full UI, and the
    cute status line is shown throughout.
  - Selecting an item opens a small **Play video / Play audio** chooser.
  - The now-playing card keeps the album art, waveform, time and transport row,
    and the **fake volume slider was removed**.

### Added
- **Interactive installer.** `sh install.sh` now asks a couple of setup
  questions before handing off to OIS: backend (ytcui-dl native / yt-dlp),
  streamlined mode (auto / off / always), and thumbnails on/off. The backend
  choice is recorded as a build override (so it survives the privilege
  escalation), and the UI choices are written to the config. Piping input or
  passing `--yes`/`--defaults` keeps it non-interactive for scripts/CI.

## [3.4.0] - 2026-06-10

### Added
- **Streamlined mode — a minimalist music-player UI for very narrow terminals.**
  When the terminal is reliably very narrow (width detected and below ~48 cols),
  ytcui automatically switches to a compact, iPod-style layout:
  - A centered scrolling menu (Search / Library / History, plus Now Playing when
    something is playing) navigated with j/k and Enter.
  - Search and a compact results list (title + channel + duration).
  - A now-playing card: a small chafa album-art thumbnail, a stylized waveform,
    elapsed/total time (LIVE for livestreams), bold title, artist, transport
    glyphs, and a volume bar.
  Detection is conservative: the default is always the full ("normal") UI, and
  the switch only happens when a sane, narrow width is reported — terminals that
  can't report a size stay in normal mode. Override with the `mode` config key
  or `--mode auto|normal|streamlined`.
- BSD: OIS now installs and uses `gmake` for the (GNU-syntax) Makefile, instead
  of falling back to the system `bmake` and failing the build.

### Changed
- New `--mode` flag and `mode` config key (auto/normal/streamlined).

## [3.3.0] - 2026-06-10

### Added
- **OneInstallSystem (OIS) is now the installer.** `sh install.sh` builds from
  source and installs ytcui across Linux/macOS/BSD with one command, resolving
  dependencies per package manager (apt, pacman, dnf, yum, zypper, apk, emerge,
  xbps, brew, macports, pkg). Dependency package names are the *development*
  packages and are verified with pkg-config, so a runtime `curl`/`openssl` CLI
  is never mistaken for the build headers. The binary gained lifecycle flags
  routed to OIS: `ytcui --update`, `--uninstall`, `--reinstall`, `--install-info`,
  `--ois`. Install registers a manifest and generates a clean uninstaller;
  `--uninstall` removes everything it added. Verified end-to-end (install +
  uninstall) on Debian/Ubuntu.
  - OIS itself was extended with a `name.pkgconfig = <module>` dependency check
    for correct detection of `-dev`/`-devel` libraries.
  - The old `install.sh`/`update.sh`/`uninstaller.sh` were replaced by OIS.

### Fixed
- **Terminal robustness:**
  - `KEY_RESIZE` is now handled explicitly (clear + full redraw), so resizing
    the terminal no longer leaves stale cells, and the resize event is no longer
    fed to the key handler as a stray keypress.
  - All displayed text is stripped of layout-breaking code points (C0/C1
    controls, BiDi overrides/isolates, zero-width and soft-hyphen characters)
    that can appear in YouTube titles and scramble a line.
- **Removed the koala mascot from the Library tab.** It straddled the shared
  panel border and overdrew it; gone now.

## [3.2.0] - 2026-06-09

### Added
- **Real terminal detection and capability layer (`TermCaps`).** ytcui now
  identifies the terminal and adapts, instead of sniffing `$TERM`. At startup
  (before ncurses) it sends a batched query handshake and parses the replies:
  - `XTVERSION` (`CSI > 0 q`) identifies XTerm/WezTerm/Contour/iTerm2/mintty,
  - `XTGETTCAP TN` identifies kitty and mlterm,
  - `DA2`/`DA3` help with Alacritty/VTE/foot/Terminology,
  - `DA1` (`CSI c`) is the anchor and reports sixel support (attribute 4),
  - `CSI 16 t` reports the cell-pixel size for correct image scaling.
  Everything is timeout-bounded, so minimal emulators that don't answer (e.g.
  uterm/libtsm) degrade to a safe baseline instead of hanging. Environment
  signals (`$TERM_PROGRAM`, `$KITTY_WINDOW_ID`, `$COLORTERM`, ...) and live
  terminfo flags (`RGB`, `bce`, `ccc`, `colors`) refine the result.
- Capabilities now drive feature enable/disable:
  - **Mouse** (SGR 1006) is only enabled where supported, so the Linux console
    no longer turns mouse movement into garbage keypresses.
  - **Graphics** auto-detection (kitty/sixel/iterm) is grounded in the DA1/
    XTVERSION queries, not guesses, and stays disabled inside tmux/screen.
  - **Colour tier** (truecolor / 256 / 16 / 8) is detected and capped sanely
    (e.g. multiplexers without `Tc` cap to 256).
- `--diag` prints the full terminal report: identity, colour tier, ccc/bce,
  native-block support, mouse, graphics protocols, cell size, keyboard protocol.

### Fixed
- **Selection highlighting is now terminal-robust.** The selected row/tab/menu
  item previously used `A_REVERSE` over a foreground-only colour pair with a
  default background, which renders inconsistently across terminals and only
  covered the text width. It now uses a dedicated foreground/background colour
  pair, painted full width with explicit spaces (never relying on `clrtoeol`/
  back-colour-erase, which differ across terminals). Falls back to reverse video
  only on sub-8-colour terminals. The bar's text colour auto-contrasts (black or
  white) against the theme's selection colour for legibility.

## [3.1.1] - 2026-06-09

### Fixed
- **Crash on macOS after a search ("malloc: pointer being freed was not
  allocated").** Root cause was a heap use-after-free at teardown: detached
  prefetch/update threads captured the InnertubeClient singleton and a
  thread_local curl handle, then outlived the singleton and
  `curl_global_cleanup()` at exit. Reproduced and verified fixed under
  AddressSanitizer. Fixes:
  - Prefetch now runs on a single owned worker thread, joined in
    `InnertubeClient::shutdown()` (also de-dupes prefetch storms).
  - `UrlCache::get()` returns a copy under the lock instead of a pointer into
    the map that escaped the lock (an independent use-after-free).
  - The update-check thread is joinable and joined before exit; `main()` shuts
    the backend down before static curl teardown.
  - Signal handler uses `_exit()` (async-signal-safe; skips unsafe teardown).
- **Coloured thumbnails unsafe on Apple's system ncurses 5.7.** 5.7 lacks
  extended colour pairs and mishandles pairs > 255. `init_thumb_colors()` now
  detects ncurses >= 6.1 and otherwise clamps/scales the thumbnail palette to
  what the library can safely allocate. The Makefile warns and recommends
  `brew install ncurses` when it falls back to system ncurses.

### Added
- **Optional pixel-perfect thumbnails (Sixel / Kitty / iTerm2), experimental.**
  Thumbnail mode `blocks | sixel | kitty | iterm | auto | off` via config key
  `"graphics"` and CLI `--gfx <mode>`.
  - **Default is `blocks`** (block art), the universal path that works on every
    terminal. Raster is never auto-enabled, so it cannot corrupt a working TUI.
  - `auto` also renders block art but logs which raster protocol your terminal
    could try.
  - Raster modes are explicit opt-in and EXPERIMENTAL: thumbnails are encoded by
    piping through chafa, which cannot probe the terminal cell-pixel size through
    a pipe. ytcui now queries the cell size itself (`CSI 16 t`) and passes it to
    the encoder, and the `make SIXEL=libsixel` build uses it for pixel-exact
    sizing. Placement may still need per-terminal tuning; if it looks wrong,
    stay on `blocks`.
  - Raster is auto-disabled inside tmux/screen.
  - Per-terminal detection covers kitty/Ghostty/WezTerm/Konsole (kitty), iTerm2
    (iterm), mlterm/foot/yaft/contour/st/Windows Terminal/VS Code (sixel);
    Apple Terminal/Alacritty/plain VTE stay on block art.
- `--diag` now reports `$TERM`, `$TERM_PROGRAM`, `$COLORTERM`, multiplexer
  status, the detected protocol (with reason) and the sixel DA result.

### Changed
- Makefile passes libcurl/openssl CFLAGS (not just LIBS), fixing source builds
  on multiarch distros where `curl/curl.h` is under a triplet include dir.
- Added `packaging/aur/` with working `ytcui-git` and versioned `ytcui`
  PKGBUILDs. The old `ytcui-bin` 404 was caused by referencing an untagged
  `v1.0.0`; tag releases from `VERSION` to avoid it.

## [2.9.1] - 2026-04-26

### Fixed
- **Thumbnails now actually render.** chafa emits ANSI escape sequences that
  ncurses `printw()` cannot handle — they were rendering as raw garbage or
  invisible. Fixed by recording the thumbnail region during the draw pass and
  calling `render_at()` **after** `ncurses refresh()`, writing chafa output
  directly to stdout via ANSI cursor positioning (`\033[row;colH`). Completely
  bypasses ncurses for thumbnail I/O.
- **Thumbnail download fallback.** If the primary URL returns a tiny/invalid
  response (common with ytcui-dl API thumbnail URLs on older videos), the
  downloader now retries with `hqdefault.jpg` automatically.
- **chafa format auto-detection.** Detects kitty → sixel → symbols+truecolor
  → symbols+256 based on `$TERM`, `$COLORTERM`, `$KITTY_WINDOW_ID`.
- **`thumbs_available` now logs clearly** when chafa is missing rather than
  silently setting itself to false and showing nothing.
- chafa is now a required dependency. The install script installs it as a
  required package (not optional) on all supported package managers.

## [2.8.0] - 2026-03-23

### 🐛 Critical Bug Fixes

#### macOS: Terminal freeze when closing mpv window via GUI

- **FIXED: TUI becomes completely unresponsive after closing mpv video window**
  - Root cause: The complex `compat.h` watchdog/death-pipe process management
    introduced in 2.5.0 for cross-platform parent death detection was interfering
    with terminal state on macOS. When mpv's Cocoa window was closed, the forked
    watchdog process and pipe handling corrupted the terminal, causing ncurses
    `getch()` to block forever.
  - Fix: Completely rewrote `player.cpp` to use simple process management:
    - Removed all `compat.h` dependency from player
    - Removed death pipes and watchdog process forking
    - Removed complex sync pipe handshaking for setpgid()
    - Now uses simple `fork()` + `setpgid(0,0)` + `usleep(10ms)`
    - Linux still gets `prctl(PR_SET_PDEATHSIG)` via direct `#ifdef`
    - macOS/BSD just rely on process group kill for cleanup
  - Kept all the good 2.6.x features: stream URL pre-resolution for fast video
    start, volume control, hardware accel toggle, cache options.

---

## [2.6.2] - 2026-03-11

### 🐛 Critical Bug Fixes

#### macOS Compatibility (10.x and up — all versions)

- **FIXED: `EVFILT_PROC` / `NOTE_EXIT` permission failure on macOS < 11 (Big Sur)**
  - Root cause: kqueue watching a grandparent PID from a grandchild process silently fails
    with `EPERM` on macOS 10.x (Catalina and below). macOS 11 relaxed this same-UID rule.
  - Fix: Removed kqueue `EVFILT_PROC` watchdog on macOS entirely. Now uses the
    pipe-based watchdog (already used for generic BSDs) for **all** macOS versions.
    The pipe approach requires zero OS permissions and is reliable on macOS 10.x+.
  - Added `macos_major_version()` via `sysctlbyname("kern.osproductversion")` for
    runtime version detection used in `--diag` output.

- **FIXED: URL copy silently failing on all macOS versions**
  - Root cause: `wl-copy` (Wayland) and `xclip` (X11) were being called on macOS where
    neither exists. The copy action appeared to succeed but nothing was written.
  - Fix: macOS now uses `pbcopy` (available since macOS 10.3). Linux/BSD still use
    `wl-copy` → `xclip` → `xsel` fallback chain.
  - Also switched from `echo -n` (shell-dependent) to `printf '%s'` (POSIX portable).

- **FIXED: "Open in browser" silently failing on all macOS versions**
  - Root cause: `open_in_browser()` in `app.cpp` unconditionally called `xdg-open`
    which does not exist on macOS. The `play_xdg()` function in `player.cpp` had the
    correct `#if defined(YTUI_MACOS)` guard but `open_in_browser()` did not.
  - Fix: Added proper platform guard — `open` on macOS, `xdg-open` on Linux/BSD.

- **FIXED: `setpgid()` race condition causing intermittent playback failures**
  - Root cause: After `fork()`, parent waited for child's `setpgid(0,0)` using
    `usleep(10000)` — a 10ms timing gamble. On slower macOS (pre-11), scheduling
    variance meant `kill(-pgid, ...)` fired before the new process group existed,
    sending signals to the wrong group or failing silently.
  - Fix: Replaced with a sync pipe. Child writes 1 byte after `setpgid()` completes.
    Parent polls up to 500ms (1ms steps with `WNOHANG` child-death checks).
    Fully deterministic, zero race. Applied to `play_piped`, `play_direct`, `play_xdg`.

### ✨ New Features

#### `--diag` — Full system diagnostic
- New flag that exits immediately (no TUI) and prints a comprehensive diagnostic:
  - Platform and compat layer details (pdeathsig strategy, pipe2 emulation, watchdog type)
  - macOS version with explicit warning if < 11
  - All binary dependencies with version strings (yt-dlp, mpv, curl, chafa, clipboard tools)
  - Config and log file paths with existence checks
  - Contents of `config.json` if present
  - Live yt-dlp search test (`ytsearch1:test`)
  - mpv `--version` output
  - Clipboard tool availability check with warning if none found
  - `pipe_cloexec()` self-test
  - Pipe watchdog fork test (macOS)
  - Last 20 lines of `debug.log`
- Run this first when anything is broken: `ytcui --diag`

#### `--injectconfig` — Write config from CLI
- Write any config key directly from the command line without editing JSON or opening TUI
- Supports: `max_results`, `theme`, `grayscale`, `sort_by`, `filter_type`, `filter_dur`,
  `ytdlp_path`, `mpv_path`
- Examples:
  ```
  ytcui --injectconfig max_results=25
  ytcui --injectconfig theme=dracula
  ytcui --injectconfig max_results=20 theme=nord grayscale=false
  ```

#### `--debug` / `--logdump` improvements
- Debug log now includes compat platform dump and macOS version on startup
- Dependency check errors now suggest `--diag` for deeper investigation

### 🔧 Internal Changes

- `compat.h`: Added `dump_platform_info()` used by `--diag`
- `app.h`: Added `copy_to_clipboard()` declaration (split from `execute_action`)
- Error messages on missing `yt-dlp`/`mpv` now suggest `ytcui --diag`

---

## [2.5.0] - 2025-02-27

### 🎨 New Features

#### Cross-Platform Support
- **macOS support**: Full native support for macOS (Intel and Apple Silicon)
- **FreeBSD support**: Native support using `procctl(PROC_PDEATHSIG_CTL)`
- **BSD support**: Generic BSD support with watchdog-based process management

### 🔧 Major Improvements

#### Process Management Overhaul
- **New `compat.h` abstraction layer**: Unified cross-platform process management
- **Platform-specific parent death detection**:
  - Linux: `prctl(PR_SET_PDEATHSIG, SIGKILL)` — kernel signals on parent death
  - FreeBSD: `procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig)` — same concept
  - macOS: `kqueue` with `EVFILT_PROC` + `NOTE_EXIT` — watches parent PID
  - Generic BSD: Pipe-based watchdog — detects EOF when parent dies
- **Watchdog process**: Forks a lightweight watchdog in same process group that kills all children when parent dies (macOS/generic BSD)
- **Portable `pipe_cloexec()`**: Replaces Linux-only `pipe2(O_CLOEXEC)` with portable `pipe()` + `fcntl(FD_CLOEXEC)` on macOS

#### Installer Improvements
- **macOS Homebrew support**: Automatically installs Homebrew if missing, then uses it for dependencies
- **Xcode CLT detection**: Prompts for Xcode Command Line Tools installation on macOS
- **FreeBSD pkg support**: Uses native `pkg` package manager
- **Improved OS detection**: Detects Linux, macOS, FreeBSD, NetBSD, OpenBSD, DragonFly

#### Build System
- **Cross-platform Makefile**: Detects OS and selects appropriate compiler/flags
- **Homebrew ncurses handling**: Properly finds Homebrew's keg-only ncurses on macOS
- **Compiler selection**: Uses `clang++` on macOS/FreeBSD, `g++` on Linux (configurable)
- **Platform defines**: Sets `YTUI_LINUX`, `YTUI_MACOS`, or `YTUI_FREEBSD` at compile time

#### Update Script
- **macOS-compatible `mktemp`**: Uses portable temp directory creation
- **Homebrew PATH setup**: Exports proper environment for builds on macOS
- **`gmake` detection**: Uses GNU make on FreeBSD when available

### ⚠️ Breaking Changes

- Removed direct `#include <sys/prctl.h>` from `player.h` (now in `compat.h`)
- Process group management refactored (internal, no user-facing changes)

### 📝 Technical Notes

#### How Parent Death Detection Works

**Linux/FreeBSD (native):**
```
fork() → child calls set_pdeathsig(SIGKILL) → kernel sends SIGKILL when parent dies
```

**macOS (kqueue watchdog):**
```
fork() → child forks watchdog → watchdog monitors parent via kqueue(EVFILT_PROC)
       → parent dies → kqueue returns → watchdog calls kill(0, SIGTERM) on process group
```

**Generic BSD (pipe watchdog):**
```
parent creates pipe → fork() → child forks watchdog holding read end
→ parent holds write end (auto-closes on death)
→ watchdog blocks on read() → EOF when parent dies → kills process group
```

---

## [2.0.0] - 2025-02-18

### 🎨 New Features

#### Theme System
- Added 8 color themes: `default`, `grayscale`, `nord`, `dracula`, `solarized`, `monokai`, `gruvbox`, `tokyo`
- Themes can be set via CLI (`-t dracula`) or config file
- `--grayscale` still works for backwards compatibility

#### Auto-Update System
- **Auto-check on startup**: ytcui checks GitHub for new versions (2s timeout, won't slow startup)
- **`--upgrade` flag**: Run `ytcui --upgrade` to update to the latest version
- **Self-updating**: Update script is installed to `/usr/local/share/ytcui/` and updates itself
- **`--no-update-check`**: Skip the startup version check if desired
- Update script and VERSION file persist even if you delete the repo folder

#### Enhanced CLI
- Beautiful colored `--help` output
- `--version` / `-v` flag
- `--theme` / `-t` flag for theme selection
- `--debug` / `-d` for debug logging
- `--logdump` for full mpv/yt-dlp output capture
- `--upgrade` for easy updates
- `--no-update-check` to skip version checking

### 🔧 Stability Improvements

#### Audio Playback Fixes
- Fixed random audio glitches on different systems
- Explicit audio format selection: prefers m4a (AAC), fallback to webm (opus)
- Increased audio buffer from 1s to 2s
- Added demuxer cache for smoother streaming
- Added audio pitch correction for stable playback

#### Video Playback Fixes
- Reasonable default window size (854x480)
- Max size constrained to 70% of screen
- Min size of 640x360
- Video quality capped at 1080p for performance
- Larger demuxer buffer (100MB) for stability

#### Process Management
- Longer grace period for process cleanup (300ms)
- Better zombie process prevention
- Improved signal handling

### 📝 Other Changes

- Renamed internal state from `grayscale` to `theme`
- `config.json` now supports `"theme": "dracula"` instead of just `"grayscale": true`
- Debug logs now go to `~/.cache/ytcui/debug.log`
- mpv logs (with `--logdump`) go to `~/.cache/ytcui/mpv.log`
- Updated README with new features and troubleshooting

### 🐛 Bug Fixes

- Fixed "River Flows in You" and similar tracks failing on some systems
- Fixed mpv window opening at full screen size
- Fixed minor memory issues in UTF-8 input handling

---

## [1.x.x] - Previous Versions

See git history for older changes.

---

### Legend

- 🎨 New features
- 🔧 Improvements  
- 📝 Documentation
- 🐛 Bug fixes
- ⚠️ Breaking changes

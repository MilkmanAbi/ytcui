# Changelog

All notable changes to ytcui will be documented in this file.
Yes, all of them. Even the ones that are just me being a fucking dumbass.

## [3.2.0] - 2026-06-09

### Added

Real terminal detection and capability layer (TermCaps). ytcui now actually identifies the terminal instead of reading tea leaves from $TERM. At startup (before ncurses) it sends a batched query handshake and parses whatever comes back:

- XTVERSION (CSI > 0 q) for XTerm/WezTerm/Contour/iTerm2/mintty
- XTGETTCAP TN for kitty and mlterm
- DA2/DA3 for Alacritty/VTE/foot/Terminology
- DA1 (CSI c) is the anchor and reports sixel support (attribute 4)
- CSI 16 t reports cell-pixel size for correct image scaling

Everything is timeout-bounded so minimal emulators that don't answer (uterm/libtsm, you know who you are) degrade to a safe baseline instead of hanging forever. Environment signals ($TERM_PROGRAM, $KITTY_WINDOW_ID, $COLORTERM, etc.) and live terminfo flags (RGB, bce, ccc, colors) refine the result.

Capabilities now actually drive feature enable/disable:

- Mouse (SGR 1006) is only enabled where supported, so the Linux console no longer turns mouse movement into garbage keypresses. You're welcome.
- Graphics auto-detection (kitty/sixel/iterm) is grounded in DA1/XTVERSION queries now, not guesses, and stays disabled inside tmux/screen.
- Colour tier (truecolor / 256 / 16 / 8) is detected and capped sanely. Multiplexers without Tc cap to 256.

`--diag` prints the full terminal report: identity, colour tier, ccc/bce, native-block support, mouse, graphics protocols, cell size, keyboard protocol.

### Fixed

Selection highlighting is now terminal-robust. The selected row/tab/menu item used to use A_REVERSE over a foreground-only colour pair with a default background, which renders inconsistently across terminals and only covered the text width. Lovely. It now uses a dedicated foreground/background colour pair, painted full width with explicit spaces (never relying on clrtoeol/back-colour-erase, which differ across terminals because of course they do). Falls back to reverse video only on sub-8-colour terminals. The bar's text colour auto-contrasts (black or white) against the theme's selection colour so you can actually read it.


## [3.1.1] - 2026-06-09

### Fixed

Crash on macOS after a search ("malloc: pointer being freed was not allocated"). Root cause: heap use-after-free at teardown. Detached prefetch/update threads captured the InnertubeClient singleton and a thread_local curl handle, then outlived the singleton and curl_global_cleanup() at exit. Reproduced and verified fixed under AddressSanitizer. Here's what got done:

- Prefetch now runs on a single owned worker thread, joined in InnertubeClient::shutdown(). Also de-dupes prefetch storms as a bonus.
- UrlCache::get() returns a copy under the lock instead of a pointer into the map that escaped the lock. That was a separate use-after-free. There were two.
- The update-check thread is joinable and joined before exit. main() shuts the backend down before static curl teardown.
- Signal handler uses _exit() (async-signal-safe, skips unsafe teardown).

Coloured thumbnails unsafe on Apple's system ncurses 5.7. 5.7 lacks extended colour pairs and mishandles pairs > 255. init_thumb_colors() now detects ncurses >= 6.1 and otherwise clamps/scales the thumbnail palette to what the library can safely allocate. The Makefile warns and recommends `brew install ncurses` when it falls back to system ncurses.

### Added

Optional pixel-perfect thumbnails (Sixel / Kitty / iTerm2), experimental. Thumbnail mode `blocks | sixel | kitty | iterm | auto | off` via config key "graphics" and CLI `--gfx <mode>`.

Default is `blocks` (block art). The universal path. Works on every terminal. Raster is never auto-enabled so it cannot corrupt a working TUI.

`auto` also renders block art but logs which raster protocol your terminal could theoretically try.

Raster modes are explicit opt-in and EXPERIMENTAL. Thumbnails are encoded by piping through chafa, which cannot probe the terminal cell-pixel size through a pipe. ytcui now queries the cell size itself (CSI 16 t) and passes it to the encoder. The `make SIXEL=libsixel` build uses it for pixel-exact sizing. Placement may still need per-terminal tuning. If it looks wrong, stay on blocks.

Raster is auto-disabled inside tmux/screen.

Per-terminal detection covers kitty/Ghostty/WezTerm/Konsole (kitty protocol), iTerm2 (iterm), mlterm/foot/yaft/contour/st/Windows Terminal/VS Code (sixel). Apple Terminal/Alacritty/plain VTE stay on block art.

`--diag` now reports $TERM, $TERM_PROGRAM, $COLORTERM, multiplexer status, detected protocol with reason, and the sixel DA result.

### Changed

- Makefile passes libcurl/openssl CFLAGS (not just LIBS), fixing source builds on multiarch distros where curl/curl.h is under a triplet include dir.
- Added packaging/aur/ with working ytcui-git and versioned ytcui PKGBUILDs. The old ytcui-bin 404 was caused by referencing an untagged v1.0.0. Tag releases from VERSION. Don't do that again.


## [2.9.1] - 2026-04-26

### Fixed

Thumbnails now actually render. chafa emits ANSI escape sequences that ncurses printw() cannot handle. They were rendering as raw garbage or invisible. Fixed by recording the thumbnail region during the draw pass and calling render_at() after ncurses refresh(), writing chafa output directly to stdout via ANSI cursor positioning (\033[row;colH). Completely bypasses ncurses for thumbnail I/O. It is what it is.

Thumbnail download fallback. If the primary URL returns a tiny/invalid response (common with ytcui-dl API thumbnail URLs on older videos), the downloader now retries with hqdefault.jpg automatically.

chafa format auto-detection. Detects kitty -> sixel -> symbols+truecolor -> symbols+256 based on $TERM, $COLORTERM, $KITTY_WINDOW_ID.

thumbs_available now logs clearly when chafa is missing rather than silently setting itself to false and showing nothing. Silent failures are for cowards.

chafa is now a required dependency. The install script installs it as a required package on all supported package managers.


## [2.8.0] - 2026-03-23

### Fixed

macOS: Terminal freeze when closing mpv window via GUI.

TUI becomes completely unresponsive after closing mpv video window. Root cause: the complex compat.h watchdog/death-pipe process management introduced in 2.5.0 for cross-platform parent death detection was interfering with terminal state on macOS. When mpv's Cocoa window was closed, the forked watchdog process and pipe handling corrupted the terminal, causing ncurses getch() to block forever.

Fix: completely rewrote player.cpp to use simple process management.

- Removed all compat.h dependency from player
- Removed death pipes and watchdog process forking
- Removed complex sync pipe handshaking for setpgid()
- Now uses simple fork() + setpgid(0,0) + usleep(10ms)
- Linux still gets prctl(PR_SET_PDEATHSIG) via direct #ifdef
- macOS/BSD rely on process group kill for cleanup

Kept all the good 2.6.x features: stream URL pre-resolution for fast video start, volume control, hardware accel toggle, cache options.


## [2.6.2] - 2026-03-11

### Fixed

macOS compatibility (10.x and up, all versions).

EVFILT_PROC / NOTE_EXIT permission failure on macOS < 11 (Big Sur). Root cause: kqueue watching a grandparent PID from a grandchild process silently fails with EPERM on macOS 10.x (Catalina and below). macOS 11 relaxed the same-UID rule. Fix: removed kqueue EVFILT_PROC watchdog on macOS entirely. Now uses the pipe-based watchdog (already used for generic BSDs) for all macOS versions. The pipe approach requires zero OS permissions and is reliable on macOS 10.x+. Also added macos_major_version() via sysctlbyname("kern.osproductversion") for runtime version detection used in --diag output.

URL copy silently failing on all macOS versions. Root cause: wl-copy (Wayland) and xclip (X11) were being called on macOS where neither exists. The copy action appeared to succeed but nothing was written. Fix: macOS now uses pbcopy (available since macOS 10.3). Linux/BSD still use wl-copy -> xclip -> xsel fallback chain. Also switched from echo -n (shell-dependent) to printf '%s' (POSIX portable).

"Open in browser" silently failing on all macOS versions. Root cause: open_in_browser() in app.cpp unconditionally called xdg-open which does not exist on macOS. The play_xdg() function in player.cpp had the correct #if defined(YTUI_MACOS) guard but open_in_browser() did not. Fix: added proper platform guard. open on macOS, xdg-open on Linux/BSD.

setpgid() race condition causing intermittent playback failures. Root cause: after fork(), parent waited for child's setpgid(0,0) using usleep(10000), a 10ms timing gamble. On slower macOS (pre-11), scheduling variance meant kill(-pgid, ...) fired before the new process group existed, sending signals to the wrong group or failing silently. Fix: replaced with a sync pipe. Child writes 1 byte after setpgid() completes. Parent polls up to 500ms (1ms steps with WNOHANG child-death checks). Fully deterministic, zero race. Applied to play_piped, play_direct, play_xdg.

### Added

`--diag`: full system diagnostic. New flag that exits immediately (no TUI) and prints a comprehensive diagnostic report. Run this first when anything is broken.

- Platform and compat layer details (pdeathsig strategy, pipe2 emulation, watchdog type)
- macOS version with explicit warning if < 11
- All binary dependencies with version strings (yt-dlp, mpv, curl, chafa, clipboard tools)
- Config and log file paths with existence checks
- Contents of config.json if present
- Live yt-dlp search test (ytsearch1:test)
- mpv --version output
- Clipboard tool availability check with warning if none found
- pipe_cloexec() self-test
- Pipe watchdog fork test (macOS)
- Last 20 lines of debug.log

`--injectconfig`: write config from CLI. Write any config key directly from the command line without editing JSON or opening the TUI.

Supports: max_results, theme, grayscale, sort_by, filter_type, filter_dur, ytdlp_path, mpv_path.

```
ytcui --injectconfig max_results=25
ytcui --injectconfig theme=dracula
ytcui --injectconfig max_results=20 theme=nord grayscale=false
```

--debug / --logdump improvements. Debug log now includes compat platform dump and macOS version on startup. Dependency check errors now suggest --diag for deeper investigation.

### Changed

- compat.h: added dump_platform_info() used by --diag
- app.h: added copy_to_clipboard() declaration (split from execute_action)
- Error messages on missing yt-dlp/mpv now suggest ytcui --diag


## [2.5.0] - 2025-02-27

### Added

Cross-platform support. macOS (Intel and Apple Silicon), FreeBSD (native procctl), generic BSD (watchdog-based).

New compat.h abstraction layer for unified cross-platform process management. Platform-specific parent death detection:

- Linux: prctl(PR_SET_PDEATHSIG, SIGKILL) -- kernel handles it
- FreeBSD: procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig) -- same idea
- macOS: kqueue with EVFILT_PROC + NOTE_EXIT -- watches parent PID
- Generic BSD: pipe-based watchdog -- detects EOF when parent dies

Watchdog process forks a lightweight watchdog in same process group that kills all children when parent dies (macOS/generic BSD).

Portable pipe_cloexec(): replaces Linux-only pipe2(O_CLOEXEC) with portable pipe() + fcntl(FD_CLOEXEC) on macOS.

Installer improvements:

- macOS Homebrew support: automatically installs Homebrew if missing
- Xcode CLT detection: prompts for Xcode Command Line Tools on macOS
- FreeBSD pkg support: uses native pkg package manager
- Improved OS detection: Linux, macOS, FreeBSD, NetBSD, OpenBSD, DragonFly

Cross-platform Makefile. Detects OS, selects compiler, handles Homebrew's keg-only ncurses, sets YTUI_LINUX / YTUI_MACOS / YTUI_FREEBSD at compile time.

Update script improvements. macOS-compatible mktemp, Homebrew PATH setup, gmake detection on FreeBSD.

### Breaking Changes

- Removed direct #include <sys/prctl.h> from player.h (now in compat.h)
- Process group management refactored internally

### Notes

How parent death detection works, since someone will ask:

Linux/FreeBSD (native):
```
fork() -> child calls set_pdeathsig(SIGKILL) -> kernel sends SIGKILL when parent dies
```

macOS (kqueue watchdog):
```
fork() -> child forks watchdog -> watchdog monitors parent via kqueue(EVFILT_PROC)
-> parent dies -> kqueue returns -> watchdog calls kill(0, SIGTERM) on process group
```

Generic BSD (pipe watchdog):
```
parent creates pipe -> fork() -> child forks watchdog holding read end
-> parent holds write end (auto-closes on death)
-> watchdog blocks on read() -> EOF when parent dies -> kills process group
```


## [2.0.0] - 2025-02-18

### Added

Theme system. 8 colour themes: default, grayscale, nord, dracula, solarized, monokai, gruvbox, tokyo. Set via CLI (-t dracula) or config file. --grayscale still works for backwards compatibility.

Auto-update system. Checks GitHub for new versions at startup (2s timeout, won't slow startup). --upgrade to update. --no-update-check to skip. Update script and VERSION file persist even if you delete the repo folder.

Enhanced CLI:

- Coloured --help output
- --version / -v
- --theme / -t
- --debug / -d for debug logging
- --logdump for full mpv/yt-dlp output capture
- --upgrade for easy updates
- --no-update-check to skip version checking

### Fixed

Audio playback. Random audio glitches on different systems. Explicit audio format selection now: prefers m4a (AAC), fallback to webm (opus). Buffer increased from 1s to 2s. Demuxer cache added. Audio pitch correction added.

Video playback. Reasonable default window size (854x480). Max size constrained to 70% of screen. Min size 640x360. Quality capped at 1080p. Larger demuxer buffer (100MB).

Process management. Longer grace period for cleanup (300ms). Better zombie process prevention. Improved signal handling.

Fixed "River Flows in You" and similar tracks failing on some systems. Don't ask.

### Changed

- Renamed internal state from grayscale to theme
- config.json now supports "theme": "dracula" instead of just "grayscale": true
- Debug logs go to ~/.cache/ytcui/debug.log
- mpv logs (with --logdump) go to ~/.cache/ytcui/mpv.log


## [1.x.x] - Previous versions

See git history. It was a simpler time.

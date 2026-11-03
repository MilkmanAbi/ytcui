#pragma once

#include <string>
#include <map>
#include <functional>
#include "theme.h"
#include "types.h"

namespace ytui {

struct Config {
    std::string ytdlp_path  = "yt-dlp";
    std::string mpv_path    = "mpv";
    int  max_results        = 15;
    bool prefer_audio       = false;
    bool no_hardware_accel  = false;   // seed from installer; --no-ha overrides
    bool grayscale          = false;   // legacy
    bool show_thumbnails    = true;

    // Thumbnail rendering mode:
    //   "blocks" — Unicode block art via ncurses colour pairs (DEFAULT;
    //              universal, the safe path that works on every terminal)
    //   "sixel" / "kitty" / "iterm" — real raster images. EXPERIMENTAL and
    //              opt-in: thumbnails are encoded by piping through chafa, which
    //              cannot probe the terminal's cell-pixel size through a pipe,
    //              so sizing/placement may be off on some terminals. Only use
    //              if it looks right on yours.
    //   "auto"   — detect the terminal but still render block art (raster is
    //              never auto-enabled, so it can't break a working TUI); the log
    //              hints which raster mode your terminal could try.
    //   "off"    — no thumbnails
    std::string graphics    = "blocks";

    std::string theme_name  = "default";

    // UI mode: auto (switch to the streamlined music-player layout when the
    // terminal is very narrow), normal (always full UI), streamlined (always).
    std::string mode        = "auto";

    std::string sort_by     = "";

    // Capability auto-override (default ON): if the terminal can't actually do
    // a feature — no colour, no chafa, no sixel/kitty/iterm — ytcui disables it
    // even when enabled, so a weak terminal never gets a corrupted UI. Set
    // force_features = true in config.json to suppress this and honour your
    // settings verbatim (you accept the risk).
    bool force_features     = false;
    std::string filter_type = "";
    std::string filter_dur  = "";

    // Per-element colour overrides applied on top of any base theme.
    // Stored under "colors": {} in config.json.
    // Keys: bg, search_box, title, channel, stats, selected, action,
    //       action_sel, status, border, header, accent, tag,
    //       published, bookmark, desc
    // Values: 256-colour index (0-255) or -1 for terminal default.
    std::map<std::string, int> custom_colors;

    // Configurable keybindings: action name -> key character.
    // Defaults are set in load(). Users override in config.json under "keys".
    // Action names: pause, volume_up, volume_down, seek_fwd, seek_back,
    //               quit, search, scroll_up, scroll_down, select, back
    struct KeyBindings {
        int pause      = ' ';    // space
        int volume_up  = '+';
        int volume_down = '-';
        int seek_fwd   = '>';
        int seek_back  = '<';
        int quit       = 'q';
        int search     = '/';
        int scroll_up  = 'k';
        int scroll_down = 'j';
        int select     = '\n';
        int back       = 27;     // escape
        int top        = 'g';
        int bottom     = 'G';
        int sort       = 's';
        int new_playlist = 'n';
        int timer      = 0x14;   // Ctrl-T
    } keys;

    // Human-readable name for a key code (inverse of the installer's parser).
    // e.g. ' ' -> "Space", 27 -> "Esc", 'a' -> "a". Used by the settings UI.
    static std::string key_display(int k);
    // Config-file form of a key code ("space", "x") — the inverse of the
    // installer/loader's parse_key_name. Used by save() to persist rebinds.
    static std::string key_config_name(int k);

    Theme get_theme() const {
        if (grayscale) return Theme::Grayscale;
        return string_to_theme(theme_name);
    }

    // Final ThemeColors = base theme + any custom_colors overrides on top.
    ThemeColors resolve_colors() const {
        ThemeColors tc = get_theme_colors(get_theme());
        for (const auto& [key, val] : custom_colors)
            apply_custom_color(tc, key, val);
        return tc;
    }

    // ── v5 additions ─────────────────────────────────────────────────────────

    // Which extractor to use. Runtime-switchable (Ctrl-B or Settings → General).
    BackendKind backend = BackendKind::YtcuiDl;
    // Fall back to the other backend automatically if the primary fails.
    bool backend_auto_failover = true;

    // Remember where you stopped; offer to resume on next play.
    bool resume_playback = true;

    // Loop the current item. Off by default — videos play start to end.
    // With loop on the item never ends, so the queue waits.
    bool loop_video = false;

    // When true and in streamlined mode, always loop (overrides loop_video).
    bool streamlined_loop = true;

    // What happens when a video ends and the queue is empty.
    //   "off"  — stop, as v4 did
    //   "list" — play the next item in whatever list is visible
    std::string autoplay = "list";

    // Advance to next item in playlist when current ends (requires autoplay="list").
    bool loop_playlist = true;

    // Arm a sleep timer at launch, in seconds. 0 = none.
    int sleep_seconds = 0;

    // SponsorBlock. On by default; off means no request is made at all.
    // The lookup uses the hash-prefix endpoint so only 4 hex chars of
    // sha256(videoID) leave the machine — ~128 videos per bucket.
    bool sponsorblock = true;
    std::string sponsorblock_categories =
        "sponsor,selfpromo,interaction,intro,outro,music_offtopic";

    // Path to the ytcui-we session file (cookie path, not a credential).
    std::string cookies_file;

    void load();
    void save();
    static std::string config_dir();
};

} // namespace ytui

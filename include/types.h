#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "theme.h"

namespace ytui {

#ifdef YTUI_VERSION
constexpr const char* VERSION = YTUI_VERSION;
#else
constexpr const char* VERSION = "5.0.0";
#endif

struct Video {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string duration;
    int duration_seconds = 0;
    std::string view_count;
    int64_t     view_count_raw = 0;
    std::string upload_date;
    std::string thumbnail_url;
    std::string url;
    std::string description;
    bool is_live = false;

    bool empty() const { return id.empty(); }
};

// Which extraction backend to use. Switchable at runtime (no restart).
enum class BackendKind { YtcuiDl, YtDlp, YtcuiWe };

// Stream URLs returned by a backend — either direct URLs or the yt-dlp pass-through mode.
struct Streams {
    std::string video_url;
    std::string audio_url;
    std::string hls_url;
    std::string user_agent;
    bool        muxed = false;
    bool        is_live = false;
    std::string label;
    int         start_seconds = 0;

    // yt-dlp mode: hand the page URL to mpv's ytdl hook instead of extracting URLs.
    bool        use_ytdl = false;
    std::string page_url;
    std::string ytdl_path;
    std::string ytdl_format;

    bool ok() const {
        if (use_ytdl) return !page_url.empty();
        return !hls_url.empty() || !video_url.empty() || !audio_url.empty();
    }
};

enum class PlayMode { Video, Audio, AudioLoop };

enum class Panel {
    Search, Tabs, Results, Actions,
    BrowserPick, SortMenu, SavePrompt,
    // Playlist panels
    PlaylistList,     // Navigating the list of playlists
    PlaylistView,     // Viewing videos inside a playlist
    PlaylistActions,  // Action menu on a playlist video
    PlaylistPick,     // "Add to playlist" popup
    NewPlaylist,      // Name-entry dialog for new playlist
    Shortcuts,        // Keybinding reference popup (? to open)
    Settings,         // In-app settings UI (Ctrl-S): live theme + accelerators
    // v5 additions
    ShareMenu,        // Share overlay: QR code + clipboard copy
    TimerPrompt,      // Sleep timer text input
    QueueView,        // Play queue panel
    ResMenu,          // Resolution selector popup ('r' key)
};

enum class Tab { Library, Playlists, Feed, History, Results };

// Sub-screens of the narrow-terminal "Streamlined" music-player UI.
//   Menu    - iPod-style section picker (Search/Library/Playlists/Feed/History)
//   Search  - query entry
//   Browse  - a dense list (search results, saved, feed, history, or playlists)
//   Actions - play-video / play-audio chooser for the highlighted item
//   Playing - now-playing card
enum class StreamScreen { Menu, Search, Browse, Actions, Playing, MoreActions };

enum class Action {
    PlayVideo, PlayAudio, PlayAudioLoop,
    PauseToggle,
    ViewChannel, OpenInBrowser,
    ToggleBookmark, SubscribeChannel,
    SaveToLibrary,
    AddToPlaylist,
    CopyURL, LoginBrowser, Logout,
    // Playlist-internal actions
    RemoveFromPlaylist,
    MoveUp, MoveDown,
    // v5 additions
    Share,          // open share menu (QR + clipboard)
    AddToQueue,     // add to play queue
    PlayNext,       // jump queue position
    LoopToggle,     // toggle loop on/off
};

struct ActionItem { Action action; std::string label; };

struct AppState {
    std::string search_query;

    std::vector<Video> results;
    int selected_result = 0;
    int results_scroll  = 0;

    std::vector<ActionItem> actions;
    int selected_action  = 0;
    bool actions_visible = false;

    Tab   active_tab = Tab::Feed;
    Panel focus      = Panel::Search;

    std::string status_message;
    bool running = true;

    int term_w = 0, term_h = 0;

    bool is_playing = false;
    bool is_paused  = false;
    std::string now_playing;
    std::string now_playing_id;  // video ID of current track (empty if unknown)
    PlayMode play_mode = PlayMode::Video;

    // Playback progress (updated from mpv IPC every render cycle)
    double playback_pos  = 0.0;   // seconds elapsed
    double playback_dur  = 0.0;   // total duration in seconds
    int    playback_vol  = 80;    // current volume %

    // Set by a waveform click in streamlined mode (see input.cpp's
    // stream_handle); app.cpp reads this once on the "__SEEK_TO__" status
    // message and issues the actual mpv seek.
    double seek_to_secs = 0.0;

    bool thumbs_available = false;
    // Resolved thumbnail graphics protocol (cast of Thumbnails::Gfx). Set once
    // by App at startup. 0=None,1=Blocks,2=Sixel,3=Kitty,4=Iterm. Defaults to
    // Blocks so behaviour is unchanged unless a raster mode is selected.
    int gfx_mode = 1;

    bool show_shortcuts = false;   // ? key opens the shortcuts reference panel

    // In-app settings UI (Ctrl-S). Live theme switching + key rebinding,
    // no restart required. settings_tab: 0=Theme, 1=Accelerators.
    bool show_settings   = false;
    int  settings_tab    = 0;
    int  settings_sel    = 0;   // cursor within the active tab
    bool settings_capturing = false;  // true while waiting for a key to bind
    std::string settings_toast;       // transient confirmation line
    // Snapshot the App fills before rendering the settings UI, so the TUI
    // stays decoupled from Config. Theme names + current accelerator labels.
    std::vector<std::string> settings_theme_names;   // 18 theme names
    std::vector<std::string> settings_accel_labels;  // "Pause / resume" ...
    std::vector<std::string> settings_accel_keys;    // "Space", "^Q" ...
    Theme theme    = Theme::Default;
    bool grayscale = false;  // legacy compat
    ThemeColors resolved_colors;  // final colors after custom overrides — set by App

    bool logged_in = false;
    std::string auth_browser;

    std::vector<std::string> browser_choices;
    int browser_pick_idx = 0;

    int sort_col = 0;
    int sort_row = 0;

    int save_prompt_idx = 0;

    // ── Home panel (History/Feed/Library) navigation ─────────────────────────
    int home_selected_idx = 0;    // selected row in the active home tab
    std::string home_search_query; // title queued for search from home panel

    // ── Playlist state ───────────────────────────────────────────────────────
    int selected_playlist      = 0;  // index in playlists list
    int playlist_scroll        = 0;  // scroll offset in playlists list
    int playlist_video_idx     = 0;  // selected video inside current playlist
    int playlist_video_scroll  = 0;  // scroll offset inside playlist view
    int playlist_video_count   = 0;  // size of current playlist (set by app render loop)
    std::string current_playlist_id;  // id of playlist being viewed

    // Playlist picker popup
    std::vector<std::string> playlist_names;  // "Create new…" + existing names
    int playlist_pick_idx = 0;
    std::string new_playlist_name;  // being typed in NewPlaylist dialog

    // ── Streamlined mode (auto-engaged on very narrow terminals) ─────────────
    int   ui_mode        = 0;   // 0 = normal, 1 = streamlined
    int   stream_screen  = 0;   // cast of StreamScreen
    int   stream_menu_sel = 0;  // selected row in the iPod-style menu
    int   stream_action_sel = 0;// 0 = Play video, 1 = Play audio
    std::string stream_section; // current Browse section label (header + empty text)
    bool  stream_on_playlists = false;  // Browse is showing playlist names
    Video stream_now;           // track shown on the now-playing screen

    // ── v5 additions ─────────────────────────────────────────────────────────

    // Backend indicator (for status line and settings)
    BackendKind active_backend = BackendKind::YtcuiDl;

    // Loop toggle — off by default; toggled with Ctrl-R
    bool loop_enabled = false;

    // Sleep timer display — "23:45 left" or "" when not set; updated each frame
    std::string timer_status;

    // Share overlay
    bool show_share = false;
    std::string share_url;       // URL shown in the QR + clipboard
    int share_mode = 0;           // 0=automatic, 1=font-independent cells, 2=half-blocks
    Panel share_return_focus = Panel::Results;
    std::string share_copy_status;

    // Timer prompt (Ctrl-T)
    bool show_timer_prompt = false;
    std::string timer_prompt_input;

    // Queue panel (Ctrl-Q)
    bool show_queue = false;
    int  queue_sel  = 0;
    std::vector<Video> queue_items;   // snapshot from App::queue_ for rendering

    // Bottom status/now-playing bar visibility (mirrors config_.show_status_bar)
    bool show_status_bar = true;
    // Hint bar visibility — j/k hints etc. (mirrors config_.show_hints)
    bool show_hints = true;

    // SponsorBlock status line ("2 segments · 1:47 skippable" or "")
    std::string sponsorblock_summary;
    bool sponsorblock_enabled = true;   // mirrors config_.sponsorblock for the settings UI

    // Mirrors of config fields for the settings General tab
    std::string config_mode        = "auto";  // config_.mode ("auto"/"normal"/"streamlined")
    bool resume_playback           = true;    // mirrors config_.resume_playback
    bool loop_playlist             = true;    // mirrors config_.loop_playlist
    bool streamlined_loop          = true;    // mirrors config_.streamlined_loop

    // Watch progress (0..1) for the selected result, for a pip in the row
    double selected_progress = 0.0;

    // Resolution selector popup ('r' key while playing video)
    bool show_res_menu = false;
    int  res_sel = 0;
    int  res_active_height = 0; // config_.preferred_height when menu was opened
    std::vector<std::string> res_labels;   // "1080p60", "720p", etc.
    std::vector<int>         res_heights;  // corresponding height values (0 = best)
    // Set by input.cpp when user picks a resolution; app.cpp reads and clears it.
    int pending_res_height = -1;
    // Set by input.cpp when 'r' is pressed; app.cpp opens the popup and clears it.
    bool res_menu_requested = false;
};

// The streamlined section menu, shared by the renderer and input handler so the
// selected index always maps to the same action. Mirrors the normal-mode tabs,
// plus Search; "Now Playing" only appears while something is playing.
inline std::vector<std::string> stream_menu_items(const AppState& s) {
    std::vector<std::string> v = {"Search", "Library", "Playlists", "Feed", "History"};
    if (s.is_playing) v.push_back("Now Playing");
    return v;
}

// 7 rainbow colors as 256-color ANSI indices, used with THUMB_COLOR_BASE
// for playlist tag dots. Index 0-6: red, orange, yellow, green, blue, purple, pink.
static const int kPlaylistTagColors[7] = {196, 208, 226, 46, 27, 93, 200};
// Glyph markers for mono mode (one per tag index)
static const char* const kPlaylistTagGlyphs[7] = {"*", "+", "~", "#", "@", "%", "&"};

namespace Color {
    constexpr int BG         = 1;
    constexpr int SEARCH_BOX = 2;
    constexpr int TITLE      = 3;
    constexpr int CHANNEL    = 4;
    constexpr int STATS      = 5;
    constexpr int SELECTED   = 6;
    constexpr int ACTION     = 7;
    constexpr int ACTION_SEL = 8;
    constexpr int STATUS     = 9;
    constexpr int BORDER     = 10;
    constexpr int HEADER     = 11;
    constexpr int ACCENT     = 12;
    constexpr int TAG        = 13;
    constexpr int PUBLISHED  = 14;
    constexpr int BOOKMARK   = 15;
    constexpr int DESC       = 16;
    constexpr int QR = 18;
    constexpr int SELECTED_BAR = 17;  // full-width selection bar (fg on bg)
}

} // namespace ytui

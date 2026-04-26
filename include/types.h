#pragma once

#include <string>
#include <vector>
#include "theme.h"

namespace ytui {

constexpr const char* VERSION = "3.0.0";

struct Video {
    std::string id;
    std::string title;
    std::string channel;
    std::string channel_id;
    std::string duration;
    int duration_seconds = 0;
    std::string view_count;
    std::string upload_date;
    std::string thumbnail_url;
    std::string url;
    std::string description;
    bool is_live = false;
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
};

enum class Tab { Library, Playlists, Feed, History, Results };

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
    PlayMode play_mode = PlayMode::Video;

    bool thumbs_available = false;

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
    std::string current_playlist_id;  // id of playlist being viewed

    // Playlist picker popup
    std::vector<std::string> playlist_names;  // "Create new…" + existing names
    int playlist_pick_idx = 0;
    std::string new_playlist_name;  // being typed in NewPlaylist dialog
};

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
}

} // namespace ytui

#pragma once

#include <string>
#include <vector>
#include "theme.h"

namespace ytui {

// Version
constexpr const char* VERSION = "2.6.2";

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
    BrowserPick, SortMenu,
    SavePrompt,
};

enum class Tab { Library, Feed, History, Results };

enum class Action {
    PlayVideo, PlayAudio, PlayAudioLoop,
    PauseToggle,
    ViewChannel, OpenInBrowser,
    ToggleBookmark, SubscribeChannel,
    SaveToLibrary,
    CopyURL, LoginBrowser, Logout,
};

struct ActionItem { Action action; std::string label; };

struct AppState {
    std::string search_query;

    std::vector<Video> results;
    int selected_result = 0;
    int results_scroll = 0;

    std::vector<ActionItem> actions;
    int selected_action = 0;
    bool actions_visible = false;

    Tab active_tab = Tab::Feed;

    Panel focus = Panel::Search;
    std::string status_message;
    bool running = true;

    int term_w = 0, term_h = 0;

    bool is_playing = false;
    bool is_paused = false;
    std::string now_playing;
    PlayMode play_mode = PlayMode::Video;

    bool thumbs_available = false;
    
    // Theme (replaces grayscale bool)
    Theme theme = Theme::Default;
    bool grayscale = false;  // Legacy compat, maps to Theme::Grayscale

    bool logged_in = false;
    std::string auth_browser;

    std::vector<std::string> browser_choices;
    int browser_pick_idx = 0;

    int sort_col = 0;
    int sort_row = 0;

    int save_prompt_idx = 0;
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

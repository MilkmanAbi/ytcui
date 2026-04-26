#include "app.h"
#include "log.h"
#include "thumbs.h"
#include "auth.h"
#include "theme.h"
#include "compat.h"
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace ytui {

static inline void shell(const std::string& cmd) {
    int r = system(cmd.c_str()); (void)r;
}

static const char* greetings[] = {
    "Ready to listen? (^.^)", "What's on your mind? :3",
    "Anything good today? (~_~)", "Waiting for your search... (._. )",
    "Type something cool! (>w<)", "Your ears await~ (*^-^*)",
    "Search the vibes (=^.^=)", "Let's find some tunes! \\(^o^)/",
    "Music time? ( *`w`)~", "What are we listening to? :D",
    "Hit me with a search! (^_~)", "The stage is yours~ (*_*)",
    "Discover something new? (o_o)", "Feed your ears! (>_<)b",
    "Another day, another banger? (^w^)", "The queue is empty... for now :3",
    "What shall we play? ('v')", "Your terminal jukebox awaits! \\m/",
    "Drop a search, get a vibe (._.)~", "Nothing playing... yet! (^_^)v",
    "Tuned in and ready! (*>_<*)", "Awaiting orders, captain~ (>_>)7",
    "Bored? Search something! \\(=w=)/",
};
static const int NUM_GREETINGS = sizeof(greetings) / sizeof(greetings[0]);
static const char* rgreet() { return greetings[rand() % NUM_GREETINGS]; }

App::App(Theme theme) {
    srand(time(nullptr));
    config_.load();

    // Priority: CLI flag > config file > default
    if (theme != Theme::Default) {
        // CLI flag wins — store it back into config so resolve_colors() uses it
        state_.theme = theme;
        config_.theme_name = theme_to_string(theme);
    } else if (config_.theme_name != "default") {
        state_.theme = config_.get_theme();
    } else {
        state_.theme = Theme::Default;
    }

    if (config_.grayscale && theme == Theme::Default)
        state_.theme = Theme::Grayscale;

    state_.grayscale = (state_.theme == Theme::Grayscale);
    config_.theme_name = theme_to_string(state_.theme); // keep in sync

    // Resolve final colors: base theme + any per-element custom_colors from config
    state_.resolved_colors = config_.resolve_colors();

    library_.load();
    state_.thumbs_available = config_.show_thumbnails && Thumbnails::renderer_available();
    if (config_.show_thumbnails && !Thumbnails::renderer_available()) {
        Log::write("WARNING: chafa not found — thumbnails disabled");
    } else if (!config_.show_thumbnails) {
        Log::write("Thumbnails disabled by config");
    }
    state_.logged_in = Auth::is_logged_in();
    state_.auth_browser = Auth::get_configured_browser();
    Log::write("Thumbs: %s | Auth: %s | Theme: %s",
        state_.thumbs_available ? "yes (chafa)" : "no (chafa missing!)",
        state_.logged_in ? state_.auth_browser.c_str() : "none",
        theme_to_string(state_.theme).c_str());
}

App::~App() { force_cleanup(); }
void App::force_cleanup() { player_.stop(); tui_.shutdown(); }
void App::set_player_options(const PlayerOptions& opts) { player_.set_options(opts); }

void App::build_actions() {
    state_.actions.clear();
    if (state_.results.empty() || state_.selected_result >= (int)state_.results.size()) return;
    const auto& v = state_.results[state_.selected_result];

    state_.actions.push_back({Action::PlayVideo,     "Play video"});
    state_.actions.push_back({Action::PlayAudio,     "Play audio"});
    state_.actions.push_back({Action::PlayAudioLoop, "Play audio (loop)"});

    if (state_.is_playing) {
        state_.actions.push_back({Action::PauseToggle,
            state_.is_paused ? "Resume playback" : "Pause playback"});
    }

    state_.actions.push_back({Action::ViewChannel,   "View channel"});

    if (!v.channel_id.empty()) {
        bool sub = library_.is_subscribed(v.channel_id);
        state_.actions.push_back({Action::SubscribeChannel,
            sub ? "Unsubscribe from channel" : "Subscribe to channel"});
    }

    state_.actions.push_back({Action::OpenInBrowser, "Open in browser"});

    bool bm = library_.is_bookmarked(v.id);
    state_.actions.push_back({Action::ToggleBookmark,
        bm ? "Remove bookmark" : "Toggle bookmark"});

    state_.actions.push_back({Action::AddToPlaylist, "Add to playlist..."});
    state_.actions.push_back({Action::SaveToLibrary, "Save to library..."});
    state_.actions.push_back({Action::CopyURL,       "Copy URL"});

    if (state_.logged_in)
        state_.actions.push_back({Action::Logout, "Logout (" + state_.auth_browser + ")"});
    else
        state_.actions.push_back({Action::LoginBrowser, "Login via browser cookies"});
}

void App::build_playlist_actions() {
    state_.actions.clear();
    const Playlist* pl = library_.get_playlist(state_.current_playlist_id);
    if (!pl || state_.playlist_video_idx >= (int)pl->videos.size()) return;

    state_.actions.push_back({Action::PlayVideo,     "Play video"});
    state_.actions.push_back({Action::PlayAudio,     "Play audio"});
    state_.actions.push_back({Action::PlayAudioLoop, "Play audio (loop)"});

    if (state_.is_playing)
        state_.actions.push_back({Action::PauseToggle,
            state_.is_paused ? "Resume playback" : "Pause playback"});

    if (state_.playlist_video_idx > 0)
        state_.actions.push_back({Action::MoveUp,   "Move up"});
    if (state_.playlist_video_idx < (int)pl->videos.size() - 1)
        state_.actions.push_back({Action::MoveDown, "Move down"});

    state_.actions.push_back({Action::RemoveFromPlaylist, "Remove from playlist"});
    state_.actions.push_back({Action::AddToPlaylist,      "Copy to another playlist..."});
    state_.actions.push_back({Action::OpenInBrowser,      "Open in browser"});
    state_.actions.push_back({Action::CopyURL,            "Copy URL"});
}

void App::refresh_playlist_names() {
    state_.playlist_names.clear();
    state_.playlist_names.push_back("+ Create new playlist");
    for (const auto& pl : library_.playlists())
        state_.playlist_names.push_back(pl.name + " (" + std::to_string(pl.videos.size()) + ")");
}

void App::show_playlist_picker() {
    refresh_playlist_names();
    state_.playlist_pick_idx = 0;
    state_.focus = Panel::PlaylistPick;
}

void App::enter_playlist(const std::string& playlist_id) {
    state_.current_playlist_id = playlist_id;
    state_.playlist_video_idx    = 0;
    state_.playlist_video_scroll = 0;
    state_.focus = Panel::PlaylistView;
    state_.actions_visible = false;

    // Prefetch thumbnails for playlist videos
    if (state_.thumbs_available) {
        const Playlist* pl = library_.get_playlist(playlist_id);
        if (pl) {
            std::vector<std::pair<std::string, std::string>> items;
            for (const auto& v : pl->videos)
                if (!v.id.empty() && !v.thumbnail_url.empty())
                    items.push_back({v.id, v.thumbnail_url});
            Thumbnails::download_batch(items);
        }
    }
}

int App::run() {
    if (!tui_.init()) return 1;
    state_.status_message = rgreet();

    while (state_.running) {
        tui_.get_dimensions(state_.term_w, state_.term_h);
        state_.is_playing  = player_.is_playing();
        state_.is_paused   = player_.is_paused();
        state_.now_playing = state_.is_playing ? player_.now_playing() : "";

        // ── Clamp playlist indices before render ──────────────────────────────
        if (!state_.current_playlist_id.empty()) {
            const Playlist* pl = library_.get_playlist(state_.current_playlist_id);
            if (pl && !pl->videos.empty()) {
                int n = (int)pl->videos.size();
                state_.playlist_video_idx = std::clamp(state_.playlist_video_idx, 0, n - 1);
                int max_vis = std::max(1, state_.term_h - 4 - 7 - 2);
                if (state_.playlist_video_idx >= state_.playlist_video_scroll + max_vis)
                    state_.playlist_video_scroll = state_.playlist_video_idx - max_vis + 1;
                if (state_.playlist_video_idx < state_.playlist_video_scroll)
                    state_.playlist_video_scroll = state_.playlist_video_idx;
            }
        }
        if (state_.active_tab == Tab::Playlists) {
            int n_pl = (int)library_.playlists().size();
            if (n_pl > 0)
                state_.selected_playlist = std::clamp(state_.selected_playlist, 0, n_pl - 1);
        }

        // ── Clamp home tab selection ──────────────────────────────────────────
        {
            int home_max = 0;
            if (state_.active_tab == Tab::History)
                home_max = (int)library_.history().size() - 1;
            else if (state_.active_tab == Tab::Feed)
                home_max = (int)library_.history().size() - 1;
            else if (state_.active_tab == Tab::Library)
                home_max = (int)library_.saved_videos().size() - 1;
            if (home_max >= 0)
                state_.home_selected_idx = std::clamp(state_.home_selected_idx, 0, home_max);
            else
                state_.home_selected_idx = 0;
        }

        tui_.render(state_, &library_);

        int ch = getch();
        bool should_search = input_.handle(ch, state_);

        // ── Status message dispatch ───────────────────────────────────────────

        if (state_.status_message == "__PAUSE_TOGGLE__") {
            player_.toggle_pause();
            state_.is_paused = player_.is_paused();
            state_.status_message = state_.is_paused ? "Paused (*-_-*)" : "Resumed (^_^)b";
        }

        // ── Home tab item clicked / Enter'd → search that title ───────────────
        if (state_.status_message == "__HOME_SEARCH__") {
            std::string title;
            int idx = state_.home_selected_idx;

            if (state_.active_tab == Tab::History || state_.active_tab == Tab::Feed) {
                auto hist = library_.history();
                // History is stored oldest-first; display is newest-first
                int rev_idx = (int)hist.size() - 1 - idx;
                if (rev_idx >= 0 && rev_idx < (int)hist.size())
                    title = hist[rev_idx].title;
            } else if (state_.active_tab == Tab::Library) {
                auto saved = library_.saved_videos();
                if (idx < (int)saved.size())
                    title = saved[idx].title;
            }

            if (!title.empty()) {
                state_.search_query = title;
                state_.status_message = "Searching: " + title + " (~_~)";
                tui_.render(state_, &library_);
                do_search();
                state_.active_tab = Tab::Results;
                state_.focus = Panel::Results;
                state_.selected_result = 0;
                state_.results_scroll  = 0;
            } else {
                state_.status_message = rgreet();
            }
        }

        // ── Esc from Results → re-run the current search ──────────────────────
        if (state_.status_message == "__RESEARCH__") {
            if (!state_.search_query.empty()) {
                state_.status_message = "Searching: " + state_.search_query + " (~_~)";
                tui_.render(state_, &library_);
                do_search();
                state_.active_tab = Tab::Results;
                state_.focus = Panel::Results;
                state_.selected_result = 0;
                state_.results_scroll  = 0;
            } else {
                state_.status_message = rgreet();
            }
        }

        if (state_.status_message == "__BROWSER_PICKED__") {
            if (state_.browser_pick_idx >= 0 &&
                state_.browser_pick_idx < (int)state_.browser_choices.size()) {
                std::string b = state_.browser_choices[state_.browser_pick_idx];
                Auth::set_browser(b);
                state_.logged_in = true;
                state_.auth_browser = b;
                state_.status_message = "Logged in via " + b + " (^_^)b";
            } else {
                state_.status_message = rgreet();
            }
            state_.browser_choices.clear();
            state_.focus = Panel::Actions;
        }

        if (state_.status_message == "__SORT_APPLIED__") {
            apply_sort_filter();
            state_.status_message = rgreet();
        }

        if (state_.status_message == "__SAVE_PICKED__") {
            do_save(state_.save_prompt_idx);
            state_.focus = Panel::Actions;
        }

        // Open a playlist (from playlist list)
        if (state_.status_message == "__OPEN_PLAYLIST__") {
            const auto& pls = library_.playlists();
            if (state_.selected_playlist >= 0 && state_.selected_playlist < (int)pls.size())
                enter_playlist(pls[state_.selected_playlist].id);
            state_.status_message = rgreet();
        }

        // Playlist picker — user selected a playlist or "Create new"
        if (state_.status_message == "__PLAYLIST_PICKED__") {
            const auto& pls = library_.playlists();
            int pick = state_.playlist_pick_idx;
            if (pick == 0) {
                // "Create new playlist" selected
                state_.new_playlist_name.clear();
                state_.focus = Panel::NewPlaylist;
                state_.status_message = rgreet();
            } else if (pick > 0 && pick <= (int)pls.size()) {
                // Add to existing playlist
                const std::string& pl_id = pls[pick - 1].id;
                std::string vid, title, channel, ch_id, thumb;
                int dur = 0;

                if (!state_.current_playlist_id.empty() &&
                    (state_.focus == Panel::PlaylistPick)) {
                    // Copying from another playlist
                    const Playlist* src = library_.get_playlist(state_.current_playlist_id);
                    if (src && state_.playlist_video_idx < (int)src->videos.size()) {
                        const auto& v = src->videos[state_.playlist_video_idx];
                        vid = v.id; title = v.title; channel = v.channel;
                        ch_id = v.channel_id; thumb = v.thumbnail_url; dur = v.duration_seconds;
                    }
                } else if (!state_.results.empty() &&
                           state_.selected_result < (int)state_.results.size()) {
                    const auto& v = state_.results[state_.selected_result];
                    vid = v.id; title = v.title; channel = v.channel;
                    ch_id = v.channel_id; thumb = v.thumbnail_url; dur = v.duration_seconds;
                }

                if (!vid.empty()) {
                    if (library_.add_to_playlist(pl_id, vid, title, channel, ch_id, thumb, dur))
                        state_.status_message = "Added to " + pls[pick - 1].name + " (^_^)b";
                    else
                        state_.status_message = "Already in that playlist (._.)";
                }
                state_.focus = Panel::Actions;
            } else {
                state_.status_message = rgreet();
                state_.focus = Panel::Actions;
            }
        }

        // Create new playlist
        if (state_.status_message == "__CREATE_PLAYLIST__") {
            if (!state_.new_playlist_name.empty()) {
                std::string pl_id = library_.create_playlist(state_.new_playlist_name);
                state_.status_message = "Created \"" + state_.new_playlist_name + "\" (^_^)";

                // Auto-add current video if we got here from a picker
                std::string vid, title, channel, ch_id, thumb;
                int dur = 0;
                if (!state_.results.empty() &&
                    state_.selected_result < (int)state_.results.size()) {
                    const auto& v = state_.results[state_.selected_result];
                    vid = v.id; title = v.title; channel = v.channel;
                    ch_id = v.channel_id; thumb = v.thumbnail_url; dur = v.duration_seconds;
                    if (!vid.empty()) library_.add_to_playlist(pl_id, vid, title, channel, ch_id, thumb, dur);
                }

                state_.new_playlist_name.clear();
                state_.focus = Panel::Actions;
            } else {
                state_.status_message = rgreet();
                state_.focus = Panel::PlaylistList;
            }
        }

        // Playlist action menu executed
        if (state_.status_message == "__PLAYLIST_ACTION__") {
            if (state_.selected_action >= 0 && state_.selected_action < (int)state_.actions.size())
                execute_playlist_action(state_.actions[state_.selected_action].action);
            if (state_.status_message == "__PLAYLIST_ACTION__")
                state_.status_message = rgreet();
        }

        // Standard action menu executed
        if (state_.status_message == "__EXEC_ACTION__") {
            if (state_.selected_action >= 0 && state_.selected_action < (int)state_.actions.size())
                execute_action(state_.actions[state_.selected_action].action);
            if (state_.status_message == "__EXEC_ACTION__")
                state_.status_message = rgreet();
        }

        // Rebuild action lists dynamically
        if (state_.actions_visible &&
            state_.focus != Panel::BrowserPick &&
            state_.focus != Panel::SavePrompt  &&
            state_.focus != Panel::PlaylistPick &&
            state_.focus != Panel::NewPlaylist) {
            int prev = state_.selected_action;
            if (state_.focus == Panel::PlaylistActions)
                build_playlist_actions();
            else if (!state_.results.empty())
                build_actions();
            state_.selected_action = std::min(prev, std::max(0, (int)state_.actions.size() - 1));
        }

        if (should_search && !state_.search_query.empty())
            do_search();
    }

    tui_.shutdown();
    return 0;
}

void App::do_search() {
    state_.status_message = "Searching... (>_<)";
    state_.results.clear();
    state_.selected_result = 0;
    state_.results_scroll = 0;
    state_.actions_visible = false;
    state_.selected_action = 0;
    tui_.render(state_, &library_);

    std::string cookies = Auth::ytdlp_cookie_args();
    auto results = youtube_.search(state_.search_query, config_.max_results, cookies);

    if (results.empty()) {
        state_.status_message = "No results found (T_T)";
    } else {
        state_.results = std::move(results);
        state_.status_message = std::to_string(state_.results.size()) + " results";
        state_.active_tab = Tab::Results;
        state_.focus = Panel::Results;
        prefetch_thumbnails();
    }
}

void App::prefetch_thumbnails() {
    if (!state_.thumbs_available) return;
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& v : state_.results)
        if (!v.id.empty() && !v.thumbnail_url.empty())
            items.push_back({v.id, v.thumbnail_url});
    Thumbnails::download_batch(items);
}

void App::show_browser_picker() {
    auto browsers = Auth::detect_browsers();
    if (browsers.empty()) { state_.status_message = "No browsers found (>_<)"; return; }
    state_.browser_choices = browsers;
    state_.browser_pick_idx = 0;
    state_.focus = Panel::BrowserPick;
}

void App::apply_sort_filter() {
    const char* sort_keys[] = {"relevance", "date", "view_count", "rating"};
    const char* filter_keys[] = {"", "video", "channel", "playlist", "short", "long"};

    if (state_.sort_col == 0 && state_.sort_row < 4)
        config_.sort_by = sort_keys[state_.sort_row];
    if (state_.sort_col == 1 && state_.sort_row < 6) {
        if (state_.sort_row <= 3)
            config_.filter_type = filter_keys[state_.sort_row];
        else
            config_.filter_dur = filter_keys[state_.sort_row];
    }
    config_.save();
    Log::write("Sort: %s, Filter type: %s, dur: %s",
        config_.sort_by.c_str(), config_.filter_type.c_str(), config_.filter_dur.c_str());
}

void App::do_save(int choice) {
    if (state_.results.empty()) return;
    const auto& v = state_.results[state_.selected_result];
    std::string cookies = Auth::ytdlp_cookie_args();

    if (!library_.is_bookmarked(v.id))
        library_.toggle_bookmark(v.id, v.title, v.channel, v.channel_id);

    std::string url = "https://www.youtube.com/watch?v=" + v.id;

    if (choice == 0) {
        state_.status_message = "Bookmarked! (*^_^*)";
    } else if (choice == 1) {
        std::string dir = std::string(getenv("HOME")) + "/Videos/ytcui";
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            std::string mc = "mkdir -p '" + dir + "'";
            int r = system(mc.c_str()); (void)r;
            std::string output_tmpl = dir + "/%(title)s.%(ext)s";
            if (cookies.empty()) {
                execlp("yt-dlp", "yt-dlp", "-o", output_tmpl.c_str(), url.c_str(), nullptr);
            } else {
                std::string flag, browser;
                std::istringstream iss(cookies);
                iss >> flag >> browser;
                execlp("yt-dlp", "yt-dlp", flag.c_str(), browser.c_str(),
                       "-o", output_tmpl.c_str(), url.c_str(), nullptr);
            }
            _exit(127);
        }
        state_.status_message = "Downloading video... (>w<)b";
    } else if (choice == 2) {
        std::string dir = std::string(getenv("HOME")) + "/Music/ytcui";
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            std::string mc = "mkdir -p '" + dir + "'";
            int r = system(mc.c_str()); (void)r;
            std::string output_tmpl = dir + "/%(title)s.%(ext)s";
            if (cookies.empty()) {
                execlp("yt-dlp", "yt-dlp", "-x", "--audio-format", "mp3",
                       "-o", output_tmpl.c_str(), url.c_str(), nullptr);
            } else {
                std::string flag, browser;
                std::istringstream iss(cookies);
                iss >> flag >> browser;
                execlp("yt-dlp", "yt-dlp", flag.c_str(), browser.c_str(),
                       "-x", "--audio-format", "mp3",
                       "-o", output_tmpl.c_str(), url.c_str(), nullptr);
            }
            _exit(127);
        }
        state_.status_message = "Downloading audio... (>w<)b";
    }
}

void App::execute_action(Action action) {
    if (state_.results.empty()) return;
    const auto& v = state_.results[state_.selected_result];

    switch (action) {
        case Action::PlayVideo: {
            state_.status_message = "Loading video... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::Video);
            state_.status_message = "Playing: " + v.title;
            return;
        }
        case Action::PlayAudio: {
            state_.status_message = "Loading audio... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::Audio);
            state_.status_message = "Playing audio: " + v.title;
            return;
        }
        case Action::PlayAudioLoop: {
            state_.status_message = "Loading loop... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::AudioLoop);
            state_.status_message = "Looping: " + v.title;
            return;
        }
        case Action::PauseToggle: {
            player_.toggle_pause();
            state_.is_paused = player_.is_paused();
            state_.status_message = state_.is_paused
                ? "Paused (*-_-*)" : "Resumed (^_^)b";
            return;
        }
        case Action::OpenInBrowser:
            open_in_browser("https://www.youtube.com/watch?v=" + v.id);
            state_.status_message = "Opened in browser (^_^)";
            break;
        case Action::ViewChannel:
            if (!v.channel_id.empty()) {
                open_in_browser("https://www.youtube.com/channel/" + v.channel_id);
                state_.status_message = "Opened channel (^_^)";
            } else state_.status_message = "No channel ID (._.)";
            break;
        case Action::SubscribeChannel:
            if (!v.channel_id.empty()) {
                bool was = library_.is_subscribed(v.channel_id);
                library_.toggle_subscribe(v.channel_id, v.channel);
                state_.status_message = was
                    ? "Unsubscribed from " + v.channel + " (T_T)/~"
                    : "Subscribed to " + v.channel + " (^o^)/";
            }
            return;
        case Action::ToggleBookmark: {
            bool was = library_.is_bookmarked(v.id);
            library_.toggle_bookmark(v.id, v.title, v.channel, v.channel_id);
            state_.status_message = was ? "Removed bookmark (._.)/" : "Bookmarked! (*^_^*)";
            return;
        }
        case Action::AddToPlaylist:
            show_playlist_picker();
            return;
        case Action::SaveToLibrary:
            state_.save_prompt_idx = 0;
            state_.focus = Panel::SavePrompt;
            return;
        case Action::CopyURL: {
            std::string url = "https://www.youtube.com/watch?v=" + v.id;
            copy_to_clipboard(url);
            state_.status_message = "URL copied! (^_~)b";
            return;
        }
        case Action::LoginBrowser:
            show_browser_picker();
            return;
        case Action::Logout:
            Auth::clear_browser();
            state_.logged_in = false;
            state_.auth_browser.clear();
            state_.status_message = "Logged out (~_~)/";
            return;
        default:
            break;  // Playlist-only actions are handled by execute_playlist_action
    }

    state_.focus = Panel::Results;
    state_.actions_visible = false;
}

// ─── Playlist action executor ──────────────────────────────────────────────────
void App::execute_playlist_action(Action action) {
    const Playlist* pl = library_.get_playlist(state_.current_playlist_id);
    if (!pl || state_.playlist_video_idx >= (int)pl->videos.size()) return;
    const auto& v = pl->videos[state_.playlist_video_idx];

    switch (action) {
        case Action::PlayVideo: {
            state_.status_message = "Loading video... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id,
                                    v.thumbnail_url, v.duration_seconds);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::Video);
            state_.status_message = "Playing: " + v.title;
            return;
        }
        case Action::PlayAudio: {
            state_.status_message = "Loading audio... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id,
                                    v.thumbnail_url, v.duration_seconds);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::Audio);
            state_.status_message = "Playing audio: " + v.title;
            return;
        }
        case Action::PlayAudioLoop: {
            state_.status_message = "Loading loop... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id,
                                    v.thumbnail_url, v.duration_seconds);
            player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, PlayMode::AudioLoop);
            state_.status_message = "Looping: " + v.title;
            return;
        }
        case Action::PauseToggle:
            player_.toggle_pause();
            state_.is_paused = player_.is_paused();
            state_.status_message = state_.is_paused ? "Paused (*-_-*)" : "Resumed (^_^)b";
            return;
        case Action::MoveUp:
            if (library_.move_up_in_playlist(state_.current_playlist_id, state_.playlist_video_idx)) {
                state_.playlist_video_idx--;
                state_.status_message = "Moved up (^_^)";
            }
            return;
        case Action::MoveDown:
            if (library_.move_down_in_playlist(state_.current_playlist_id, state_.playlist_video_idx)) {
                state_.playlist_video_idx++;
                state_.status_message = "Moved down (^_^)";
            }
            return;
        case Action::RemoveFromPlaylist: {
            if (library_.remove_from_playlist(state_.current_playlist_id, v.id)) {
                const Playlist* updated = library_.get_playlist(state_.current_playlist_id);
                if (updated)
                    state_.playlist_video_idx = std::clamp(state_.playlist_video_idx,
                                                           0, std::max(0, (int)updated->videos.size() - 1));
                state_.status_message = "Removed from playlist (._.)";
                state_.focus = Panel::PlaylistView;
                state_.actions_visible = false;
            }
            return;
        }
        case Action::AddToPlaylist:
            show_playlist_picker();
            return;
        case Action::OpenInBrowser:
            open_in_browser("https://www.youtube.com/watch?v=" + v.id);
            state_.status_message = "Opened in browser (^_^)";
            break;
        case Action::CopyURL: {
            copy_to_clipboard("https://www.youtube.com/watch?v=" + v.id);
            state_.status_message = "URL copied! (^_~)b";
            return;
        }
        default: break;
    }
}

// ─── Platform-correct browser open ────────────────────────────────────────────
// BUG FIX: original code always used xdg-open, which doesn't exist on macOS.
// macOS uses 'open'. Linux/BSD use 'xdg-open'.

void App::open_in_browser(const std::string& url) {
    Log::write("open_in_browser: %s", url.c_str());
#if defined(YTUI_MACOS)
    shell("open '" + url + "' >/dev/null 2>&1 &");
#else
    shell("xdg-open '" + url + "' >/dev/null 2>&1 &");
#endif
}

// ─── Clipboard copy ────────────────────────────────────────────────────────────
// Uses fork+pipe+execvp directly — no shell(), no system(), no escaping.
// shell()/system() are unreliable for clipboard tools inside ncurses because
// they inherit a mangled terminal environment. Piping raw bytes via execvp
// works correctly regardless of URL content (ampersands, quotes, etc.)
//
// Tool priority:
//   macOS:          pbcopy   (built into macOS since 10.3, always present)
//   Linux Wayland:  wl-copy  (from wl-clipboard package)
//   Linux/BSD X11:  xclip    (xclip package)
//   Linux/BSD X11:  xsel     (xsel package, fallback)
//
// We try tools in order and stop at the first success, so even on Wayland if
// wl-copy isn't installed we fall through to xclip/xsel gracefully.

static bool pipe_to_cmd(const std::string& text, const char* const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char* const*)argv);
        _exit(127);  // execvp failed (tool not found)
    }

    close(pipefd[0]);
    ssize_t written = write(pipefd[1], text.data(), text.size());
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);

    // Success = we wrote all bytes AND the tool exited 0
    return written == (ssize_t)text.size()
        && WIFEXITED(status)
        && WEXITSTATUS(status) == 0;
}

void App::copy_to_clipboard(const std::string& text) {
    Log::write("copy_to_clipboard: %s", text.c_str());
    bool ok = false;

#if defined(YTUI_MACOS)
    // pbcopy ships with macOS itself — always available, no install needed
    { const char* a[] = {"pbcopy", nullptr};
      ok = pipe_to_cmd(text, a);
      Log::write("clipboard: pbcopy %s", ok ? "ok" : "failed"); }
#else
    // Try wl-copy first (Wayland). Works even if WAYLAND_DISPLAY isn't set
    // in our env — the tool itself knows how to find the compositor socket.
    if (!ok) {
        const char* a[] = {"wl-copy", nullptr};
        ok = pipe_to_cmd(text, a);
        Log::write("clipboard: wl-copy %s", ok ? "ok" : "not found/failed");
    }
    // xclip (X11)
    if (!ok) {
        const char* a[] = {"xclip", "-selection", "clipboard", nullptr};
        ok = pipe_to_cmd(text, a);
        Log::write("clipboard: xclip %s", ok ? "ok" : "not found/failed");
    }
    // xsel (X11, fallback)
    if (!ok) {
        const char* a[] = {"xsel", "--clipboard", "--input", nullptr};
        ok = pipe_to_cmd(text, a);
        Log::write("clipboard: xsel %s", ok ? "ok" : "not found/failed");
    }
    if (!ok)
        Log::write("clipboard: all tools failed — run 'ytcui --diag' for help");
#endif
}

} // namespace ytui

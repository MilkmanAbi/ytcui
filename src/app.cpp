#include "app.h"
#include "log.h"
#include "thumbs.h"
#include "termcaps.h"
#include "auth.h"
#include "theme.h"
#include "compat.h"
#include "core/url.h"
#include "core/share.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace ytui {

static const char* const kThemeNames[] = {
    "default","grayscale","nord","dracula","solarized","monokai","gruvbox",
    "tokyo","pink","green","blue","purple","red","amber","ocean","mint",
    "coral","slate"
};
static const int kThemeCount = (int)(sizeof(kThemeNames)/sizeof(kThemeNames[0]));

// Rebindable accelerators shown on the Accelerators tab. The pointer-to-member
// lets us read and write the live Config::KeyBindings without a giant switch.
struct AccelRow { const char* label; int Config::KeyBindings::* field; };
static const AccelRow kAccelRows[] = {
    {"Pause / resume",   &Config::KeyBindings::pause},
    {"Volume up",        &Config::KeyBindings::volume_up},
    {"Volume down",      &Config::KeyBindings::volume_down},
    {"Seek forward",     &Config::KeyBindings::seek_fwd},
    {"Seek backward",    &Config::KeyBindings::seek_back},
    {"Search",           &Config::KeyBindings::search},
    {"Scroll up",        &Config::KeyBindings::scroll_up},
    {"Scroll down",      &Config::KeyBindings::scroll_down},
    {"Jump to top",      &Config::KeyBindings::top},
    {"Jump to bottom",   &Config::KeyBindings::bottom},
    {"Sort menu",        &Config::KeyBindings::sort},
    {"New playlist",     &Config::KeyBindings::new_playlist},
    {"Quit",             &Config::KeyBindings::quit},
    {"Sleep timer",      &Config::KeyBindings::timer},
};
static const int kAccelCount = (int)(sizeof(kAccelRows)/sizeof(kAccelRows[0]));


static inline void shell(const std::string& cmd) {
    int r = system(cmd.c_str()); (void)r;
}

static Video entry_to_video(const VideoEntry& e) {
    Video v;
    v.id = e.id; v.title = e.title; v.channel = e.channel;
    v.channel_id = e.channel_id; v.thumbnail_url = e.thumbnail_url;
    v.duration_seconds = e.duration_seconds;
    return v;
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

    // Point the input handler at the live keybindings so configured / rebound
    // keys actually take effect. config_.keys stays valid for the App's life.
    input_.set_keybindings(&config_.keys);

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

    // Persist the theme choice: next launch remembers it without --theme.
    // Only writes if the theme actually differs from what's on disk, so
    // launching without --theme doesn't clobber a saved choice.
    if (theme != Theme::Default) {
        config_.save();
    }

    // Resolve final colors: base theme + any per-element custom_colors from config
    state_.resolved_colors = config_.resolve_colors();

    library_.load();
    state_.thumbs_available = config_.show_thumbnails && Thumbnails::renderer_available();
    if (config_.show_thumbnails && !Thumbnails::renderer_available()) {
        Log::write("WARNING: chafa not found — thumbnails disabled");
    } else if (!config_.show_thumbnails) {
        Log::write("Thumbnails disabled by config");
    }

    // Resolve thumbnail graphics protocol (see resolve_graphics()).
    resolve_graphics();
    // Force-disable anything the terminal can't actually do (pre-ncurses pass).
    apply_capability_overrides(/*authoritative=*/false);
    state_.logged_in = Auth::is_logged_in();
    state_.auth_browser = Auth::get_configured_browser();
    Log::write("Thumbs: %s | Auth: %s | Theme: %s",
        state_.thumbs_available ? "yes (chafa)"
            : Thumbnails::renderer_available() ? "no (disabled)" : "no (chafa missing!)",
        state_.logged_in ? state_.auth_browser.c_str() : "none",
        theme_to_string(state_.theme).c_str());
}

App::~App() { force_cleanup(); }
void App::force_cleanup() { player_.stop(); tui_.shutdown(); }
void App::set_player_options(const PlayerOptions& opts) { player_.set_options(opts); }

// Resolve config_.graphics -> state_.gfx_mode. "auto" probes $TERM/env; for an
// ambiguous xterm we additionally confirm sixel via a Device-Attributes query.
// MUST run before ncurses init (query_sixel_da touches the raw tty).
void App::resolve_graphics() {
    const std::string& m = config_.graphics;
    Thumbnails::Gfx g = Thumbnails::Gfx::Blocks;
    std::string why;

    if (m == "off") {
        g = Thumbnails::Gfx::None; why = "disabled by config";
    } else if (m == "sixel" || m == "kitty" || m == "iterm") {
        // Explicit opt-in to a raster protocol. Experimental — but still gated
        // on the capability probe: a non-sixel terminal (e.g. a MacPorts mlterm
        // built without --with-imagelib) renders sixel bytes as on-screen
        // garbage, so if the runtime probe contradicts the request we refuse
        // and fall back to block art rather than corrupt the UI.
        g = Thumbnails::parse_gfx_mode(m);
        why = "forced (experimental raster)";
        const TermCaps& caps = TermCaps::get();
        bool ok = true; const char* reason = "";
        if (m == "sixel" && !caps.sixel) {
            ok = false; reason = "terminal did not confirm sixel support (DA1 attribute 4)";
        } else if (m == "kitty" && !caps.kitty_gfx) {
            ok = false; reason = "terminal did not confirm the kitty graphics protocol";
        } else if (m == "iterm" && !caps.iterm_images) {
            ok = false; reason = "terminal is not an iTerm2-protocol terminal";
        }
        // force_features=true means "honour my config verbatim" — skip the
        // capability downgrade (the user accepts the risk).
        if (!ok && config_.force_features) {
            Log::write("Forcing --gfx %s despite unconfirmed support "
                       "(force_features=true)", m.c_str());
            ok = true;
        }
        if (!ok) {
            g = Thumbnails::Gfx::Blocks;
            why = "block art";
            Log::write("Ignoring --gfx %s: %s. Falling back to block art.", m.c_str(), reason);
            fprintf(stderr, "ytcui: --gfx %s ignored (%s); using block art.\n", m.c_str(), reason);
        }
    } else {
        // "blocks" (default) and "auto": always block art. Raster is NEVER
        // auto-enabled, because piping thumbnails through chafa cannot probe
        // the terminal cell-pixel size and mis-scaled images corrupt the TUI.
        g = Thumbnails::Gfx::Blocks;
        why = "block art";
        if (m == "auto") {
            auto d = Thumbnails::detect_gfx_ex();
            if (d.mode != Thumbnails::Gfx::Blocks && d.mode != Thumbnails::Gfx::None)
                Log::write("Terminal may support %s; try --gfx %s (experimental)",
                           Thumbnails::gfx_name(d.mode), Thumbnails::gfx_name(d.mode));
        }
    }

    if (!state_.thumbs_available) { g = Thumbnails::Gfx::None; why = "thumbnails unavailable"; }
    state_.gfx_mode = (int)g;

    // Auto hint comes from the real capability detection (queries), not just env.
    if (m == "auto") {
        const TermCaps& caps = TermCaps::get();
        int bg = caps.best_graphics();
        if (bg) {
            const char* n = bg == 3 ? "kitty" : bg == 2 ? "sixel" : "iterm";
            Log::write("Terminal (%s) may support %s; try --gfx %s (experimental)",
                       caps.id_name().c_str(), n, n);
        }
    }

    // For an explicit raster mode, feed the encoder the terminal cell-pixel size
    // so images are sized to fit. Prefer the size TermCaps already probed; fall
    // back to a dedicated probe if needed.
    if (g == Thumbnails::Gfx::Sixel || g == Thumbnails::Gfx::Kitty
        || g == Thumbnails::Gfx::Iterm) {
        const TermCaps& caps = TermCaps::get();
        if (caps.cell_px_w > 0 && caps.cell_px_h > 0) {
            Thumbnails::cell_px_w() = caps.cell_px_w;
            Thumbnails::cell_px_h() = caps.cell_px_h;
            Log::write("Cell size (from caps): %dx%d px", caps.cell_px_w, caps.cell_px_h);
        } else if (Thumbnails::probe_cell_px()) {
            Log::write("Cell size probed: %dx%d px", Thumbnails::cell_px_w(),
                       Thumbnails::cell_px_h());
        } else {
            Log::write("Cell size unknown; using fallback sizing");
        }
    }

    Log::write("Graphics mode: %s (config=%s; %s)",
               Thumbnails::gfx_name(g), config_.graphics.c_str(), why.c_str());
}

// Force-disable features the terminal genuinely cannot do, so a weak/odd
// terminal can never end up with a corrupted UI (no-colour block art, sixel
// bytes dumped as text, etc.). The user can opt out with force_features=true.
//
// Called twice: once in the constructor with the pre-ncurses capability
// estimate, and once from run() after ncurses reports the real colour count
// (authoritative=true), since COLORS is only known after start_color().
void App::apply_capability_overrides(bool authoritative) {
    const TermCaps& caps = TermCaps::get();

    // ── mlterm hardening (non-negotiable) ──────────────────────────────────────
    // Runs BEFORE the force_features escape hatch: on a terminal that renders
    // sixel as text and bold as reverse-video garbage, "honour my config
    // verbatim" must not be allowed to re-enable thumbnails or raster. This
    // also fires for the MLterm colour theme so chafa is never even spawned.
    bool mono = caps.mono_hardening || state_.theme == Theme::MLterm;
    if (mono) {
        if (state_.thumbs_available) {
            state_.thumbs_available = false;
            Log::write("mlterm hardening: thumbnails disabled (strict B&W, no "
                       "raster or block art)");
        }
        if (state_.gfx_mode != (int)Thumbnails::Gfx::None) {
            state_.gfx_mode = (int)Thumbnails::Gfx::None;
            Log::write("mlterm hardening: graphics forced off");
        }
    }

    if (config_.force_features) {
        if (authoritative)
            Log::write("Capability auto-override disabled (force_features=true) "
                       "— honouring config verbatim");
        return;
    }

    // ── Colour ────────────────────────────────────────────────────────────────
    // < 8 colours (or a terminal with no colour at all) means block-art
    // thumbnails are meaningless and any raster protocol is moot. Theming itself
    // degrades to monochrome automatically inside ncurses.
    bool has_colour = caps.colors >= 8;

    // ── Thumbnails (chafa block art) ───────────────────────────────────────────
    // Need: enabled in config, chafa present, and enough colour to be legible.
    bool chafa_ok = Thumbnails::renderer_available();
    if (state_.thumbs_available && !chafa_ok) {
        state_.thumbs_available = false;
        Log::write("Auto-override: thumbnails disabled (chafa not available)");
    }
    if (state_.thumbs_available && !has_colour) {
        state_.thumbs_available = false;
        Log::write("Auto-override: thumbnails disabled (terminal has < 8 colours)");
        if (authoritative)
            fprintf(stderr, "ytcui: thumbnails disabled — terminal has no usable colour "
                            "(set force_features=true to override)\n");
    }

    // ── Graphics protocol ──────────────────────────────────────────────────────
    int g = state_.gfx_mode;  // Thumbnails::Gfx
    auto disable_raster = [&](const char* reason) {
        // Drop to block art (or none if thumbnails are off / no colour).
        int ng = (state_.thumbs_available && has_colour)
                     ? (int)Thumbnails::Gfx::Blocks : (int)Thumbnails::Gfx::None;
        if (g != ng) {
            Log::write("Auto-override: %s -> %s (%s)",
                       Thumbnails::gfx_name((Thumbnails::Gfx)g),
                       Thumbnails::gfx_name((Thumbnails::Gfx)ng), reason);
            state_.gfx_mode = ng;
        }
        g = ng;
    };

    if (g == (int)Thumbnails::Gfx::Sixel && !caps.sixel)
        disable_raster("terminal did not confirm sixel support (DA1 attribute 4)");
    else if (g == (int)Thumbnails::Gfx::Kitty && !caps.kitty_gfx)
        disable_raster("terminal did not confirm the kitty graphics protocol");
    else if (g == (int)Thumbnails::Gfx::Iterm && !caps.iterm_images)
        disable_raster("terminal is not an iTerm2-protocol terminal");

    // If colour is gone entirely, no graphics path makes sense.
    if (!has_colour && state_.gfx_mode != (int)Thumbnails::Gfx::None) {
        Log::write("Auto-override: graphics disabled (no colour)");
        state_.gfx_mode = (int)Thumbnails::Gfx::None;
    }
}

void App::set_graphics_mode(const std::string& mode) {
    if (mode.empty()) return;
    config_.graphics = mode;
    resolve_graphics();
}

void App::set_ui_mode(const std::string& mode) {
    if (mode.empty()) return;
    config_.mode = mode;   // auto | normal | streamlined
}

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
    state_.actions.push_back({Action::Share,         "Share (QR + clipboard)"});
    state_.actions.push_back({Action::AddToQueue,    "Add to queue"});
    state_.actions.push_back({Action::PlayNext,      "Play next"});
    state_.actions.push_back({Action::LoopToggle,
        state_.loop_enabled ? "Loop: off" : "Loop: on"});

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
    apply_capability_overrides(/*authoritative=*/true);
    state_.status_message = rgreet();
    watch_state_.load();

    bool was_playing = false;
    std::string sb_loaded_for;  // video_id for which SB was last requested
    int prev_ui_mode = -1;

    while (state_.running) {
        tui_.get_dimensions(state_.term_w, state_.term_h);
        state_.is_playing  = player_.is_playing();
        state_.is_paused   = player_.is_paused();
        state_.now_playing = state_.is_playing ? player_.now_playing() : "";

        // Refresh the IPC cache once per frame (non-blocking) and read progress
        // from it. tick() also retries the socket connect until mpv is ready, so
        // nothing here ever waits on mpv.
        if (state_.is_playing) {
            player_.tick();
            state_.playback_pos = player_.get_position();
            state_.playback_dur = player_.get_duration();
            state_.playback_vol = player_.get_volume();
        } else {
            state_.playback_pos = 0;
            state_.playback_dur = 0;
        }

        // v5: end-of-play detection + sleep timer + SponsorBlock
        {
            bool just_ended   = was_playing && !state_.is_playing;
            bool just_started = !was_playing && state_.is_playing;
            was_playing = state_.is_playing;
            if (just_ended)   on_video_end();
            // Kick off a SB fetch whenever a NEW video starts (or a fresh play
            // after the last one ended). Compare against the video_id we already
            // fetched so we don't re-fetch on resume-from-pause.
            if (just_started || (state_.is_playing && !state_.now_playing_id.empty()
                                  && state_.now_playing_id != sb_loaded_for)) {
                if (state_.now_playing_id != sb_loaded_for) {
                    sb_loaded_for = state_.now_playing_id;
                    start_sb_fetch(state_.now_playing_id);
                }
            }
        }
        tick_timer();
        tick_sponsorblock();
        state_.timer_status = timer_.active() ? timer_.describe() : "";

        // Poll background download (non-blocking): notify when it finishes.
        if (download_pid_ > 0) {
            int wstatus = 0;
            pid_t r = ::waitpid(download_pid_, &wstatus, WNOHANG);
            if (r == download_pid_) {
                download_pid_ = -1;
                if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
                    state_.status_message = "Download complete! Files in ~/Downloads (^_^)b";
                else
                    state_.status_message = "Download failed — check ~/Downloads (;_;)";
            }
        }

        // ── Resolve UI mode ───────────────────────────────────────────────────
        // Default is the full ("normal") UI. We only drop to the streamlined
        // music-player layout when the terminal is RELIABLY very narrow: a
        // positive, sane width below the threshold. Terminals that can't report
        // a size fall back to 80 cols (or $COLUMNS), so they stay in normal mode.
        constexpr int STREAM_MAX_W = 48;   // enter streamlined below this
        constexpr int STREAM_EXIT_W = 52;  // exit streamlined at or above this (hysteresis)
        if (config_.mode == "streamlined")      state_.ui_mode = 1;
        else if (config_.mode == "normal")      state_.ui_mode = 0;
        else /* auto */ {
            bool narrow = (state_.term_w > 4 && state_.term_w < STREAM_MAX_W);
            bool wide   = (state_.term_w >= STREAM_EXIT_W);
            if      (narrow) state_.ui_mode = 1;
            else if (wide)   state_.ui_mode = 0;
            // hysteresis zone [48,51]: keep current mode, avoid thrash
        }
        if (prev_ui_mode != -1 && prev_ui_mode != state_.ui_mode)
            sync_mode_transition(prev_ui_mode, state_.ui_mode);
        prev_ui_mode = state_.ui_mode;

        // ── Clamp playlist indices before render ──────────────────────────────
        if (!state_.current_playlist_id.empty()) {
            const Playlist* pl = library_.get_playlist(state_.current_playlist_id);
            if (pl && !pl->videos.empty()) {
                int n = (int)pl->videos.size();
                state_.playlist_video_count = n;
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

        // Refresh the settings-UI display snapshot so the TUI can render the
        // panel without depending on Config directly.
        // Keep settings-display fields in sync whether or not the panel is open,
        // so the General tab shows live values even before first open.
        state_.sponsorblock_enabled = config_.sponsorblock;
        state_.config_mode          = config_.mode;
        state_.resume_playback      = config_.resume_playback;
        state_.loop_playlist        = config_.loop_playlist;
        state_.streamlined_loop     = config_.streamlined_loop;
        state_.show_status_bar      = config_.show_status_bar;
        state_.show_hints           = config_.show_hints;
        if (state_.show_settings) {
            state_.settings_theme_names.assign(kThemeNames, kThemeNames + kThemeCount);
            state_.settings_accel_labels.clear();
            state_.settings_accel_keys.clear();
            for (int i = 0; i < kAccelCount; i++) {
                state_.settings_accel_labels.push_back(kAccelRows[i].label);
                state_.settings_accel_keys.push_back(
                    Config::key_display(config_.keys.*(kAccelRows[i].field)));
            }
        }

        tui_.render(state_, &library_);

        int ch = getch();

        // Terminal resized: force a full clear + redraw on the next iteration so
        // no stale cells from the old geometry linger, and don't feed KEY_RESIZE
        // into the key handler as a stray keypress.
        if (ch == KEY_RESIZE) {
            clear();
            continue;
        }

        // ── In-app settings UI (Ctrl-S) ───────────────────────────────────────
        // Ctrl-S is byte 0x13 (also what Ctrl-Shift-S sends; terminals ignore
        // Shift for control chars). Flow control was disabled in TUI::init so
        // this reaches us instead of pausing terminal output.
        if (ch == 0x13 && !state_.show_settings) {   // open
            state_.show_settings = true;
            state_.settings_tab  = 0;
            state_.settings_sel  = 0;
            state_.settings_capturing = false;
            state_.settings_toast.clear();
            continue;
        }
        if (state_.show_settings) {
            handle_settings_key(ch);
            continue;
        }

        // ── v5 modal panels ───────────────────────────────────────────────────
        if (state_.show_timer_prompt) {
            // TimerPrompt is rendered / handled via Panel::TimerPrompt focus;
            // input_.handle already dispatches it via handle_timer_prompt().
        }

        // Sleep timer prompt (default Ctrl-T, rebindable in Settings → Accelerators)
        if (ch == config_.keys.timer && !state_.show_timer_prompt) {
            handle_timer_key();
            continue;
        }
        // Ctrl-R: loop toggle
        if (ch == 0x12) {
            state_.loop_enabled = !state_.loop_enabled;
            state_.status_message = state_.loop_enabled
                ? "Loop on — will restart when done (^_^)" : "Loop off";
            continue;
        }
        // Ctrl-B: backend cycle (ytcui-dl → yt-dlp → ytcui-we → ytcui-dl)
        if (ch == 0x02) {
            {
                BackendKind next = (state_.active_backend == BackendKind::YtcuiDl) ? BackendKind::YtDlp
                                 : (state_.active_backend == BackendKind::YtDlp)   ? BackendKind::YtcuiWe
                                 :                                                    BackendKind::YtcuiDl;
                switch_backend(next);
            }
            continue;
        }

        bool should_search = input_.handle(ch, state_);

        // ── Resolution menu requests (set by input.cpp, resolved here) ────────
        if (state_.res_menu_requested) {
            state_.res_menu_requested = false;
            open_res_menu();
        }
        if (state_.pending_res_height >= 0) {
            int h = state_.pending_res_height;
            state_.pending_res_height = -1;
            apply_resolution(h);
        }

        // ── Status message dispatch ───────────────────────────────────────────

        if (state_.status_message == "__PAUSE_TOGGLE__") {
            Log::write("[key] pause toggle (is_playing=%d)", (int)player_.is_playing());
            player_.toggle_pause();
            state_.is_paused = player_.is_paused();
            state_.status_message = state_.is_paused ? "Paused (*-_-*)" : "Resumed (^_^)b";
        }
        else if (state_.status_message == "__VOLUME_UP__") {
            player_.volume_up(5);
            char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "Volume: %d%%", player_.get_volume());
            state_.status_message = vbuf;
        }
        else if (state_.status_message == "__VOLUME_DOWN__") {
            player_.volume_down(5);
            char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "Volume: %d%%", player_.get_volume());
            state_.status_message = vbuf;
        }
        else if (state_.status_message == "__SEEK_FWD__") {
            player_.seek_forward(10.0);
            state_.status_message = ">> +10s";
        }
        else if (state_.status_message == "__SEEK_BACK__") {
            player_.seek_backward(10.0);
            state_.status_message = "<< -10s";
        }
        else if (state_.status_message == "__SEEK_TO__") {
            player_.seek_to(state_.seek_to_secs);
            int t = (int)state_.seek_to_secs;
            char sbuf[32]; snprintf(sbuf, sizeof(sbuf), ">> %d:%02d", t / 60, t % 60);
            state_.status_message = sbuf;
        }
        else if (state_.status_message == "__APPLY_TIMER__") {
            apply_timer_input();
        }
        else if (state_.status_message == "__QUEUE_PLAY__") {
            if (state_.queue_sel >= 0 && state_.queue_sel < (int)queue_.size()) {
                Video v = queue_.items()[state_.queue_sel];
                queue_.remove((std::size_t)state_.queue_sel);
                sync_queue_display();
                player_.stop();
                start_play(v, PlayMode::Audio);
                state_.status_message = "Playing: " + v.title;
                state_.show_queue = false;
                state_.focus = Panel::Results;
            } else {
                state_.status_message = rgreet();
            }
        }
        else if (state_.status_message == "__QUEUE_REMOVE__") {
            if (state_.queue_sel >= 0 && state_.queue_sel < (int)queue_.size()) {
                queue_.remove((std::size_t)state_.queue_sel);
                sync_queue_display();
                state_.queue_sel = std::clamp(state_.queue_sel, 0,
                                              std::max(0, (int)queue_.size() - 1));
                if (queue_.empty()) {
                    state_.show_queue = false;
                    state_.focus = Panel::Results;
                    state_.status_message = "Queue empty";
                } else {
                    state_.status_message = "Removed from queue";
                }
            }
        }

        // ── Streamlined-mode section loaders & playback ───────────────────────
        if (state_.status_message == "__STREAM_SEC_LIBRARY__" ||
            state_.status_message == "__STREAM_SEC_FEED__" ||
            state_.status_message == "__STREAM_SEC_HISTORY__") {
            bool lib = (state_.status_message == "__STREAM_SEC_LIBRARY__");
            state_.results.clear();
            state_.stream_on_playlists = false;
            if (lib) {
                state_.stream_section = "Library";
                for (const auto& e : library_.saved_videos()) {
                    Video v; v.id = e.id; v.title = e.title; v.channel = e.channel;
                    v.channel_id = e.channel_id; state_.results.push_back(v);
                }
            } else {
                // Feed = recently watched; History = full history. Both from history.
                state_.stream_section = (state_.status_message == "__STREAM_SEC_FEED__") ? "Feed" : "History";
                const auto& h = library_.history();
                // history is oldest→newest; show newest first
                for (auto it = h.rbegin(); it != h.rend(); ++it) {
                    Video v; v.id = it->id; v.title = it->title; v.channel = it->channel;
                    v.channel_id = it->channel_id; v.thumbnail_url = it->thumbnail_url;
                    state_.results.push_back(v);
                }
            }
            state_.selected_result = 0;
            state_.stream_screen = (int)StreamScreen::Browse;
            state_.status_message.clear();
        }

        if (state_.status_message == "__STREAM_SEC_PLAYLISTS__") {
            state_.playlist_names.clear();
            for (const auto& pl : library_.playlists())
                state_.playlist_names.push_back(pl.name + "  (" + std::to_string(pl.videos.size()) + ")");
            state_.stream_section = "Playlists";
            state_.stream_on_playlists = true;
            state_.selected_result = 0;
            state_.stream_screen = (int)StreamScreen::Browse;
            state_.status_message.clear();
        }

        if (state_.status_message == "__STREAM_OPEN_PLAYLIST__") {
            const auto& pls = library_.playlists();
            int idx = state_.selected_result;
            state_.results.clear();
            if (idx >= 0 && idx < (int)pls.size()) {
                state_.stream_section = pls[idx].name;
                for (const auto& e : pls[idx].videos) {
                    Video v; v.id = e.id; v.title = e.title; v.channel = e.channel;
                    v.channel_id = e.channel_id; v.thumbnail_url = e.thumbnail_url;
                    state_.results.push_back(v);
                }
            }
            state_.stream_on_playlists = false;
            state_.selected_result = 0;
            state_.status_message.clear();
        }

        if (state_.status_message == "__STREAM_PLAY_VIDEO__" ||
            state_.status_message == "__STREAM_PLAY_AUDIO__") {
            bool audio = (state_.status_message == "__STREAM_PLAY_AUDIO__");
            if (!state_.results.empty() &&
                state_.selected_result < (int)state_.results.size()) {
                state_.stream_now    = state_.results[state_.selected_result];
                state_.stream_screen = (int)StreamScreen::Playing;
                execute_action(audio ? Action::PlayAudio : Action::PlayVideo);
            } else {
                state_.status_message.clear();
            }
        }

        if (state_.status_message == "__STREAM_MORE_ACTIONS__") {
            build_actions();
            state_.selected_action = 0;
            state_.stream_screen   = (int)StreamScreen::MoreActions;
            state_.status_message.clear();
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

        if (state_.status_message == "__PLAYLIST_TAG__") {
            const auto& pls = library_.playlists();
            if (state_.selected_playlist >= 0 && state_.selected_playlist < (int)pls.size())
                library_.toggle_playlist_color(pls[state_.selected_playlist].id);
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
            if (state_.selected_action >= 0 && state_.selected_action < (int)state_.actions.size()) {
                Action act = state_.actions[state_.selected_action].action;
                execute_action(act);
                // In streamlined mode, play actions should land on the Playing screen.
                if (state_.ui_mode == 1 &&
                    (act == Action::PlayVideo || act == Action::PlayAudio || act == Action::PlayAudioLoop)) {
                    if (!state_.results.empty() && state_.selected_result < (int)state_.results.size())
                        state_.stream_now = state_.results[state_.selected_result];
                    state_.stream_screen = (int)StreamScreen::Playing;
                }
            }
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

        if (should_search && !state_.search_query.empty()) {
            do_search();
            if (state_.ui_mode == 1) {
                state_.selected_result = 0;
                state_.stream_on_playlists = false;
                state_.stream_section = "Results";
                state_.stream_screen = (int)StreamScreen::Browse;
            }
        }
    }

    tui_.shutdown();
    return 0;
}

void App::do_search() {
    // v5: if the query is a YouTube URL, bypass the search API entirely
    if (ytcui::url::looks_like_url(state_.search_query)) {
        auto p = ytcui::url::parse(state_.search_query);
        if (p.kind == ytcui::url::Kind::Video && !p.video_id.empty()) {
            Video v;
            v.id            = p.video_id;
            v.title         = "(direct link) " + p.video_id;
            v.thumbnail_url = "https://i.ytimg.com/vi/" + p.video_id + "/mqdefault.jpg";
            state_.results = { v };
            state_.selected_result = 0;
            state_.results_scroll  = 0;
            state_.actions_visible = true;
            state_.selected_action = 0;
            state_.active_tab      = Tab::Results;
            state_.focus           = Panel::Actions;
            state_.status_message  = "Direct link — press Enter to play (^_^)";
            prefetch_thumbnails();
            return;
        }
    }

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

    if (choice == 0) {
        state_.status_message = "Bookmarked! (*^_^*)";
    } else if (choice == 1) {
        // Download: both an MP4 (video+audio) and an MP3 land in the user's
        // actual Downloads folder. The heavy lifting happens in a re-exec'd
        // copy of ourselves (see main.cpp's --internal-download and the
        // comment there for why this is exec, not an in-process call or a
        // plain fork).
        std::string dir = std::string(getenv("HOME")) + "/Downloads";
        std::string exe = compat::self_exe_path(nullptr);
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
            const char* self = !exe.empty() ? exe.c_str() : "ytcui";
            if (!exe.empty()) {
                execl(self, self, "--internal-download", v.id.c_str(), dir.c_str(),
                      cookies.c_str(), nullptr);
            }
            // Fall back to a $PATH lookup if we couldn't resolve our own path.
            execlp("ytcui", "ytcui", "--internal-download", v.id.c_str(), dir.c_str(),
                   cookies.c_str(), nullptr);
            _exit(127);
        }
        // Track the PID so the render loop can notify on completion.
        if (pid > 0) download_pid_ = pid;
        state_.status_message = "Downloading MP4 + MP3 to ~/Downloads... (>w<)b";
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
            start_play(v, PlayMode::Video);
            state_.status_message = "Playing: " + v.title;
            return;
        }
        case Action::PlayAudio: {
            state_.status_message = "Loading audio... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id);
            start_play(v, PlayMode::Audio);
            state_.status_message = "Playing audio: " + v.title;
            return;
        }
        case Action::PlayAudioLoop: {
            state_.status_message = "Loading loop... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id);
            start_play(v, PlayMode::AudioLoop);
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
        case Action::Share: {
            std::string url = "https://youtu.be/" + v.id;
            show_share_menu(url);
            return;
        }
        case Action::AddToQueue:
            queue_.add(v);
            sync_queue_display();
            state_.status_message = "Added to queue: " + v.title.substr(0, 30);
            return;
        case Action::PlayNext:
            queue_.play_next(v);
            sync_queue_display();
            state_.status_message = "Play next: " + v.title.substr(0, 30);
            return;
        case Action::LoopToggle:
            state_.loop_enabled = !state_.loop_enabled;
            state_.status_message = state_.loop_enabled ? "Loop on (^_^)" : "Loop off";
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
            start_play(entry_to_video(v), PlayMode::Video);
            state_.status_message = "Playing: " + v.title;
            return;
        }
        case Action::PlayAudio: {
            state_.status_message = "Loading audio... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id,
                                    v.thumbnail_url, v.duration_seconds);
            start_play(entry_to_video(v), PlayMode::Audio);
            state_.status_message = "Playing audio: " + v.title;
            return;
        }
        case Action::PlayAudioLoop: {
            state_.status_message = "Loading loop... (~_~)";
            tui_.render(state_, &library_);
            library_.add_to_history(v.id, v.title, v.channel, v.channel_id,
                                    v.thumbnail_url, v.duration_seconds);
            start_play(entry_to_video(v), PlayMode::AudioLoop);
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

// ─── In-app settings: live theme + accelerators (Ctrl-S) ──────────────────────

// All 18 selectable themes, in display order. Index maps to settings_sel on
// the Theme tab.

void App::apply_theme_live(const std::string& theme_name) {
    config_.theme_name = theme_name;
    state_.theme       = config_.get_theme();
    state_.grayscale   = (state_.theme == Theme::Grayscale);
    // Recompute the palette; the next render() calls setup_colors() with it.
    state_.resolved_colors = config_.resolve_colors();
}

void App::handle_settings_key(int ch) {
    // getch() returns ERR (-1) on the input timeout when no key was pressed.
    // It must never be treated as a keypress — otherwise, while capturing a
    // rebind, an idle timeout would "bind" ERR and later crash JSON save with
    // an invalid byte. Ignore it entirely.
    if (ch == ERR) return;

    state_.settings_toast.clear();

    // While capturing a key for a rebind, the NEXT keypress becomes the binding
    // — but with guards. Some keys can't be bound (they'd make the UI or the
    // process uncontrollable), and the Enter that STARTED the capture must not
    // register as the bound key.
    if (state_.settings_capturing) {
        // Enter/CR/LF: this is almost always the residue of the Enter press
        // that began the capture (terminals send CR+LF; ncurses can deliver the
        // trailing newline on the next read). Ignore it and keep waiting, so we
        // never bind an action to Enter by accident. Enter is reserved anyway.
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            state_.settings_toast = "Enter can't be bound — press another key";
            return;   // stay in capture mode
        }
        if (ch == 27) {                     // Esc cancels the capture
            state_.settings_capturing = false;
            state_.settings_toast = "rebind cancelled";
            return;
        }
        // Reserved / un-bindable keys. Binding any of these would break the app
        // or the terminal: Ctrl-C (SIGINT), Ctrl-Z (SIGTSTP), Ctrl-S (opens
        // this very panel), Ctrl-\ (SIGQUIT). Refuse and keep waiting.
        if (ch == 3 || ch == 26 || ch == 0x13 || ch == 28) {
            const char* nm = (ch == 3)  ? "Ctrl-C" :
                             (ch == 26) ? "Ctrl-Z" :
                             (ch == 0x13)? "Ctrl-S" : "Ctrl-\\";
            char buf[64];
            snprintf(buf, sizeof(buf), "%s is reserved — press another key", nm);
            state_.settings_toast = buf;
            return;   // stay in capture mode
        }
        // A valid key — bind it.
        if (state_.settings_sel >= 0 && state_.settings_sel < kAccelCount) {
            config_.keys.*(kAccelRows[state_.settings_sel].field) = ch;
            config_.save();
            char buf[64];
            snprintf(buf, sizeof(buf), "bound '%s' to %s",
                     kAccelRows[state_.settings_sel].label,
                     Config::key_display(ch).c_str());
            state_.settings_toast = buf;
        }
        state_.settings_capturing = false;
        return;
    }

    switch (ch) {
        case 0x13:                          // Ctrl-S again closes
        case 27:                            // Esc closes
        case 'q':
            state_.show_settings = false;
            config_.save();                 // persist theme + any binding changes
            return;

        case '\t':                          // Tab switches sub-tabs
            state_.settings_tab = (state_.settings_tab + 1) % 5;
            state_.settings_sel = 0;
            return;

        case KEY_UP: case 'k':
            if (state_.settings_sel > 0) state_.settings_sel--;
            return;

        case KEY_DOWN: case 'j': {
            int n = (state_.settings_tab == 0) ? kThemeCount
                  : (state_.settings_tab == 1) ? kAccelCount
                  : (state_.settings_tab == 2) ? 8   // backend/mode/sb/resume/playlist/streamlined/statusbar/hints
                  : (state_.settings_tab == 3) ? 2   // timer + loop
                  : 0;                                 // about: display only
            if (state_.settings_sel < n - 1) state_.settings_sel++;
            return;
        }

        case KEY_MOUSE: {
            MEVENT ev;
            if (getmouse(&ev) != OK) return;
            if (!(ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED))) return;
            int pw = std::min(60, state_.term_w - 2);
            int ph = 22; if (ph > state_.term_h - 2) ph = state_.term_h - 2;
            int px = std::max(0, (state_.term_w - pw) / 2);
            int py = std::max(0, (state_.term_h - ph) / 2);
            int list_top  = py + 3;
            int list_rows = ph - 5;

            // Click on a settings tab label to switch to it.
            if (ev.y == py + 1) {
                static const char* tabs[] = {"Theme","Accelerators","General","Timer","About"};
                int tx = px + 2;
                for (int t = 0; t < 5; t++) {
                    int tw = (int)strlen(tabs[t]) + 3;
                    if (ev.x >= tx && ev.x < tx + tw) {
                        state_.settings_tab = t;
                        state_.settings_sel = 0;
                        return;
                    }
                    tx += tw;
                }
                return;
            }

            // Click on a list item within the panel.
            if (ev.y >= list_top && ev.y < list_top + list_rows &&
                ev.x >= px + 1 && ev.x < px + pw - 1) {
                int n = (state_.settings_tab == 0) ? kThemeCount
                      : (state_.settings_tab == 1) ? kAccelCount
                      : (state_.settings_tab == 2) ? 8
                      : (state_.settings_tab == 3) ? 2
                      : 0;
                if (n == 0) return;
                int first = (state_.settings_sel >= list_rows)
                            ? state_.settings_sel - list_rows + 1 : 0;
                int idx = first + (ev.y - list_top);
                if (idx >= 0 && idx < n) {
                    state_.settings_sel = idx;
                    handle_settings_key('\n');  // apply selection
                }
            }
            return;
        }

        case '\n': case '\r': case KEY_ENTER: case ' ':
            if (state_.settings_tab == 0) {
                // Theme tab: apply the highlighted theme instantly.
                if (state_.settings_sel >= 0 && state_.settings_sel < kThemeCount) {
                    apply_theme_live(kThemeNames[state_.settings_sel]);
                    state_.settings_toast = std::string("theme: ") +
                                            kThemeNames[state_.settings_sel];
                }
            } else if (state_.settings_tab == 1) {
                // Accelerators tab: begin capturing the next key.
                state_.settings_capturing = true;
                state_.settings_toast = "press a key to bind (Esc to cancel)";
            } else if (state_.settings_tab == 2) {
                // General tab: cycle values
                if (state_.settings_sel == 0) {
                    // Backend: cycle ytcui-dl → yt-dlp → ytcui-we → ytcui-dl
                    BackendKind next = (config_.backend == BackendKind::YtcuiDl) ? BackendKind::YtDlp
                                     : (config_.backend == BackendKind::YtDlp)   ? BackendKind::YtcuiWe
                                     :                                              BackendKind::YtcuiDl;
                    switch_backend(next);
                } else if (state_.settings_sel == 1) {
                    // UI mode: cycle auto → normal → streamlined
                    if (config_.mode == "auto")          config_.mode = "normal";
                    else if (config_.mode == "normal")   config_.mode = "streamlined";
                    else                                 config_.mode = "auto";
                    config_.save();
                    state_.settings_toast = "mode: " + config_.mode;
                } else if (state_.settings_sel == 2) {
                    // SponsorBlock toggle
                    config_.sponsorblock = !config_.sponsorblock;
                    state_.sponsorblock_enabled = config_.sponsorblock;
                    config_.save();
                    state_.settings_toast = std::string("SponsorBlock: ")
                                          + (config_.sponsorblock ? "on" : "off");
                } else if (state_.settings_sel == 3) {
                    // Resume playback toggle
                    config_.resume_playback = !config_.resume_playback;
                    config_.save();
                    state_.settings_toast = std::string("Resume: ")
                                          + (config_.resume_playback ? "on" : "off");
                } else if (state_.settings_sel == 4) {
                    // Loop playlist toggle
                    config_.loop_playlist = !config_.loop_playlist;
                    config_.save();
                    state_.settings_toast = std::string("Loop playlist: ")
                                          + (config_.loop_playlist ? "on" : "off");
                } else if (state_.settings_sel == 5) {
                    // Streamlined loop toggle
                    config_.streamlined_loop = !config_.streamlined_loop;
                    config_.save();
                    state_.settings_toast = std::string("Streamlined loop: ")
                                          + (config_.streamlined_loop ? "on" : "off");
                } else if (state_.settings_sel == 6) {
                    // Status bar toggle
                    config_.show_status_bar = !config_.show_status_bar;
                    state_.show_status_bar  = config_.show_status_bar;
                    config_.save();
                    state_.settings_toast = std::string("Status bar: ")
                                          + (config_.show_status_bar ? "on" : "off");
                } else if (state_.settings_sel == 7) {
                    // Hints toggle
                    config_.show_hints = !config_.show_hints;
                    state_.show_hints  = config_.show_hints;
                    config_.save();
                    state_.settings_toast = std::string("Hints: ")
                                          + (config_.show_hints ? "on" : "off");
                }
            } else if (state_.settings_tab == 3) {
                if (state_.settings_sel == 0) {
                    // Sleep timer row: close settings, open timer prompt
                    state_.show_settings = false;
                    config_.save();
                    handle_timer_key();
                } else {
                    // Loop toggle row
                    state_.loop_enabled = !state_.loop_enabled;
                    state_.settings_toast = state_.loop_enabled ? "Loop: on" : "Loop: off";
                }
            }
            return;

        default:
            // On the Theme tab, live-preview as the cursor moves via arrows is
            // handled above; nothing else to do here.
            return;
    }
}

// ─── v5 feature methods ────────────────────────────────────────────────────────

void App::tick_timer() {
    if (!timer_.active()) return;
    if (timer_.fire_if_due()) {
        if (player_.is_playing() && !player_.is_paused()) {
            player_.toggle_pause();
            state_.is_paused = true;
        }
        state_.status_message = "Sleep timer fired — paused (zzz)";
        Log::write("SleepTimer fired");
    }
}

void App::on_video_end() {
    // Record the finished video in watch state
    if (!state_.now_playing.empty())
        watch_state_.record(state_.now_playing_id, state_.now_playing, "", 0, 0, /*finished=*/true);

    // Streamlined mode always loops by default (unless config overrides it).
    bool effective_loop = state_.loop_enabled
                       || (config_.streamlined_loop && state_.ui_mode == 1);

    // Loop: replay the same video if loop is enabled and no queue entry follows
    if (effective_loop && queue_.empty()) {
        const Video* target = nullptr;
        if (!state_.results.empty() && state_.selected_result < (int)state_.results.size())
            target = &state_.results[state_.selected_result];
        else if (!state_.stream_now.empty())
            target = &state_.stream_now;
        if (target) {
            start_play(*target, PlayMode::Audio);
            state_.status_message = "Looping: " + target->title;
        }
        return;
    }

    // Queue: take the next item
    if (!queue_.empty()) {
        Video next = queue_.take();
        sync_queue_display();
        start_play(next, PlayMode::Audio);
        state_.status_message = "Playing next: " + next.title;
    }
}

void App::sync_queue_display() {
    state_.queue_items = queue_.items();
    state_.queue_sel   = std::clamp(state_.queue_sel, 0,
                                    std::max(0, (int)queue_.size() - 1));
}

void App::handle_timer_key() {
    state_.timer_prompt_input.clear();
    if (timer_.active()) {
        timer_.cancel();
        state_.status_message = "Timer cancelled";
        return;
    }
    state_.show_timer_prompt = true;
    state_.focus = Panel::TimerPrompt;
}

void App::apply_timer_input() {
    state_.show_timer_prompt = false;
    state_.focus = Panel::Results;
    int secs = ytcui::parse_duration(state_.timer_prompt_input);
    if (secs <= 0) {
        state_.status_message = "Bad duration — try '30m' or '1h23m'";
        return;
    }
    timer_.set(secs);
    state_.status_message = "Timer set: " + timer_.describe();
    state_.timer_prompt_input.clear();
}

void App::show_share_menu(const std::string& url) {
    auto res = ytcui::share::copy(url);
    state_.status_message = res.ok ? "Copied to clipboard!" : "Clipboard unavailable";
    tui_.render(state_, &library_);
    tui_.show_qr_fullscreen(url);
    state_.status_message = rgreet();
}

void App::switch_backend(BackendKind k) {
    state_.active_backend = k;
    config_.backend = k;
    config_.save();
    const char* name = (k == BackendKind::YtcuiDl) ? "ytcui-dl (native)"
                     : (k == BackendKind::YtcuiWe)  ? "ytcui-we"
                     :                                 "yt-dlp";
    state_.status_message = std::string("Backend: ") + name;
    Log::write("Backend switched to %s", name);
}

void App::open_res_menu() {
    auto rungs = player_.resolution_rungs();
    if (rungs.empty()) {
        state_.status_message = "No resolution data yet — play a video first";
        return;
    }
    state_.res_labels.clear();
    state_.res_heights.clear();
    // Prepend "Best available" (height 0) so user can reset to auto
    state_.res_labels.push_back("Best available");
    state_.res_heights.push_back(0);
    for (auto& [h, lab] : rungs) {
        state_.res_labels.push_back(lab);
        state_.res_heights.push_back(h);
    }
    // Set cursor to current preferred height; remember it for the "active" marker
    state_.res_active_height = config_.preferred_height;
    state_.res_sel = 0;
    for (int i = 0; i < (int)state_.res_heights.size(); i++) {
        if (state_.res_heights[i] == config_.preferred_height) {
            state_.res_sel = i;
            break;
        }
    }
    state_.show_res_menu = true;
    state_.focus = Panel::ResMenu;
}

void App::apply_resolution(int height) {
    config_.preferred_height = height;
    config_.save();
    PlayerOptions opts = player_.get_options();
    opts.preferred_height = height;
    player_.set_options(opts);
    state_.show_res_menu = false;
    state_.focus = Panel::Results;
    std::string label = (height == 0) ? "auto" : std::to_string(height) + "p";
    state_.status_message = "Quality: " + label;
    // If currently playing video, restart to apply the new resolution
    if (state_.is_playing && !state_.stream_now.empty()) {
        start_play(state_.stream_now, state_.play_mode);
    }
}

void App::start_play(const Video& v, PlayMode mode) {
    state_.now_playing_id = v.id;
    state_.stream_now     = v;
    state_.play_mode      = mode;
    user_stopped_         = false;
    {
        std::lock_guard<std::mutex> lk(sb_mutex_);
        sb_segs_     = {};
        sb_video_id_.clear();
    }
    sb_last_seek_ = {};
    // Sync runtime options into PlayerOptions before each play.
    PlayerOptions opts = player_.get_options();
    opts.preferred_height = config_.preferred_height;
    opts.backend          = config_.backend;
    opts.cookies_file     = config_.cookies_file;
    player_.set_options(opts);
    player_.play("https://www.youtube.com/watch?v=" + v.id, v.title, mode);
}

void App::sync_mode_transition(int from, int to) {
    if (from == 0 && to == 1) {
        // normal → streamlined: route to the most relevant screen
        if (state_.is_playing)
            state_.stream_screen = (int)StreamScreen::Playing;
        else if (!state_.results.empty()) {
            state_.stream_screen  = (int)StreamScreen::Browse;
            state_.stream_section = "Results";
        } else
            state_.stream_screen = (int)StreamScreen::Menu;
    }
    // streamlined → normal: no-op; normal-mode state is fully preserved
}

void App::start_sb_fetch(const std::string& video_id) {
    if (!config_.sponsorblock)  return;
    if (video_id.empty())       return;
    // Prevent overlapping fetches
    bool expected = false;
    if (!sb_fetching_.compare_exchange_strong(expected, true)) return;

    // Copy everything the thread needs — no shared pointers into App
    std::string vid  = video_id;
    std::string cats = config_.sponsorblock_categories;
    std::thread([this, vid, cats]() {
        auto segs = ytcui::sb::fetch(vid, cats);
        {
            std::lock_guard<std::mutex> lk(sb_mutex_);
            sb_segs_      = std::move(segs);
            sb_video_id_  = vid;
        }
        sb_fetching_.store(false);
    }).detach();
}

void App::tick_sponsorblock() {
    // Update the summary string (cheap even under lock)
    {
        std::lock_guard<std::mutex> lk(sb_mutex_);
        if (sb_segs_.fetched)
            state_.sponsorblock_summary = sb_segs_.summary();
        else
            state_.sponsorblock_summary.clear();
    }

    if (!config_.sponsorblock || !state_.is_playing || state_.is_paused) return;

    double pos = state_.playback_pos;
    if (pos < 0) return;

    double target;
    {
        std::lock_guard<std::mutex> lk(sb_mutex_);
        target = sb_segs_.skip_target(pos);
    }
    if (target >= 0) {
        auto now  = std::chrono::steady_clock::now();
        auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now - sb_last_seek_).count();
        if (ms > 1500) {
            player_.seek_to(target);
            sb_last_seek_ = now;
            Log::write("SponsorBlock skip: %.1f -> %.1f", pos, target);
        }
    }
}

} // namespace ytui

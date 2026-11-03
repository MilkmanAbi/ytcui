#pragma once
// config.json.
//
// Same file, same field names, same location as v4: ~/.config/ytcui/config.json.
// v5 reads every v4 field it had, including the "colors" and "keys" blocks, so
// upgrading changes nothing the user set. New fields are additive and all have
// defaults, so a v4 config is a valid v5 config.
//
// nlohmann/json is gone. Reading goes through yj.h (the scanner vendored with
// ytcui-dl) and writing through yjw.h, which is the same pair of headers used
// to talk to YouTube -- one JSON implementation in the project instead of two.

#include <map>
#include <string>
#include <vector>

#include "core/keymap.h"

namespace ytcui::config {

// How the UI is laid out. v4 had auto/normal/streamlined; minimal is new.
enum class UiMode { Auto, Normal, Streamlined, Minimal };

// Which extractor fetches search results and stream URLs. v4 chose this at
// build time and you had to rebuild to change it; in v5 it is a setting and
// can be switched while running.
enum class Backend { YtcuiDl, YtDlp };

struct Config {
    std::string theme = "default";

    // element name -> colour text, applied on top of the chosen theme.
    // Kept as text rather than parsed colours so that writing the file back
    // preserves what the user typed ("#8be9fd" stays hex, 214 stays 214).
    std::map<std::string, std::string> color_overrides;

    keys::Keymap keymap;

    std::string graphics = "auto";    // auto|sixel|kitty|iterm|blocks|off
    UiMode      mode     = UiMode::Auto;
    Backend     backend  = Backend::YtcuiDl;

    int  max_results       = 15;
    bool no_hardware_accel = false;
    bool show_thumbnails   = true;
    bool force_features    = false;
    std::string sort_by;
    std::string mpv_extra;            // extra args appended to mpv
    int  volume = -1;                 // -1 = leave mpv's default alone

    // v5 additions
    bool backend_auto_failover = true;   // fall back when the primary fails
    bool qr_light_background   = true;   // QR must scan, so dark-on-light

    // SponsorBlock. On by default, and defensible: the lookup uses the
    // hash-prefix endpoint, which sends only the first four hex characters of
    // sha256(videoID) and gets back every video sharing that prefix -- about
    // 128 of them -- so the server cannot tell which video is being watched.
    // Off here means no request is made at all.
    bool sponsorblock = true;
    std::string sponsorblock_categories =
        "sponsor,selfpromo,interaction,intro,outro,music_offtopic";

    // Where ytcui-we wrote the session. Empty means signed out. This is a
    // credential path, not a credential.
    std::string cookies_file;

    // Remember where you stopped and offer to resume.
    bool resume_playback = true;

    // Loop whatever is playing. Off by default: a video plays start to end.
    // Note the deliberate asymmetry with the queue -- looping repeats the
    // current item, while a non-empty queue always advances when an item
    // ends. With loop on, the item never ends, so the queue waits.
    bool loop_video = false;

    // What happens when a video ends and the queue is empty.
    //
    //   off   stop, as v4 did
    //   list  play the next item in whatever list you are looking at
    //   radio keep going past the end of the list by searching the channel
    //
    // "list" is the default because it is what a playlist is for. Note what
    // is NOT here: YouTube's own "related videos". Neither extractor exposes
    // them without extra work, and inventing a weaker recommendation while
    // calling it radio would be worse than saying what this actually does.
    std::string autoplay = "list";

    // A sleep timer to arm at launch, in seconds. 0 means none. Mostly for
    // `--sleep 45m` rather than for the config file.
    int sleep_seconds = 0;

    // Problems found while loading, for --diag and a status line. A bad field
    // never aborts the load: the default is used and the complaint recorded.
    std::vector<std::string> warnings;

    bool loaded_from_file = false;
};

// Read config_file(). A missing file is not an error: defaults are returned.
Config load();
Config load_from(const std::string& path);
Config parse_string(const std::string& json, const std::string& origin = "<string>");

// Write to config_file(), creating the directory. Only values that differ from
// the defaults are written, so the file stays readable and future default
// changes still reach the user.
bool save(const Config& c);
bool save_to(const Config& c, const std::string& path);

std::string serialize(const Config& c);

const char* to_string(UiMode m);
const char* to_string(Backend b);
UiMode  parse_mode(std::string_view s, bool* ok = nullptr);
Backend parse_backend(std::string_view s, bool* ok = nullptr);

}  // namespace ytcui::config

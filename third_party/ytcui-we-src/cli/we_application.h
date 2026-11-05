#pragma once
#include "we_collections.h"
#include "we_selector.h"
#include "we_subtitles.h"

struct WeOptions {
  std::string command, action = "download", output, path, cookies,
                       client = "auto", format, language = "en", audio_language,
                       subformat = "vtt", audioformat = "best", mergeformat,
                       archive, print, proxy;
  std::vector<std::string> targets, tokens;
  bool explicit_command = false, flat = false, skip = false,
       skip_formats = false, comments = false, write_info = false,
       write_subs = false, write_auto = false, list_subs = false,
       automatic = false, extract_audio = false, ignore_errors = false,
       no_cache = false, reverse = false, no_playlist = false, quiet = false,
       check_runtime = false, resolve_json = false, mpv_json = false, live_from_start = false;
  size_t limit = 1000, start = 1, end = 0;
  int record_seconds = 0;
  double section_start = -1, section_end = -1;
  Mode mode = Mode::AudioVideo;
  Quality quality;
  ytwe::CommentOptions comment_options;
  ytwe::DownloadConfig download;
  ytwe::JsSolver solver;
};
inline void we_help() {
  std::puts(
      "ytcui-we 0.2 — native C++ YouTube client\n\n"
      "ytcui-we [OPTIONS] URL [URL...]\n"
      "ytcui-we "
      "media|info|comments|playlist|search|channel|community|feed|mix|"
      "notifications|live-chat|subtitles|record INPUT [OPTIONS]\n\n"
      "Media: -p/--play, -g/--get-url, -F/-L/--list-formats, -d/--download\n"
      "       -f/--format EXPR, -q HEIGHT, -a/--audio, -V/--video\n"
      "       -o TEMPLATE, -P DIRECTORY, -N/--connections N, --no-continue\n"
      "       -x/--extract-audio, --audio-format "
      "best|mp3|m4a|aac|opus|flac|wav\n"
      "       --merge-output-format mp4|mkv|webm, --download-sections "
      "'*START-END'\n"
      "       --record-seconds N, --live-from-start, --mpv-args\n"
      "Metadata: -J/-j/--json, --skip-download, --write-info-json, --print "
      "TEMPLATE\n"
      "Comments: --write-comments, --max-comments N, --sort newest|top, "
      "--no-replies\n"
      "Subtitles: --list-subs, --write-subs, --write-auto-subs, --sub-langs "
      "REGEX, --sub-format FORMAT\n"
      "Collections: --flat-playlist, --limit N, --max-pages N, "
      "--playlist-start N, --playlist-end N, --playlist-reverse\n"
      "Auth: --cookies FILE, --client auto|all|CLIENT[,CLIENT...], --po-token "
      "CLIENT.gvs+TOKEN\n"
      "Runtime: --js-runtime NODE, --ejs-dir DIR, --cache-dir DIR, --no-cache, "
      "--proxy URL, -4/-6\n"
      "Batch: --batch-file FILE, --download-archive FILE, --ignore-errors\n\n"
      "Raw video URLs download by default. Explicit info/collection commands "
      "print JSON.\n"
      "No browser or Python process is used. Node runs the pinned EJS "
      "challenge helper; ffmpeg handles muxing and live recording.\n");
}
inline void write_text(const std::filesystem::path &target,
                       const std::string &value) {
  if (!target.parent_path().empty())
    std::filesystem::create_directories(target.parent_path());
  auto temp = target;
  temp += ".tmp";
  {
    std::ofstream f(temp, std::ios::binary);
    f << value;
    if (!f)
      throw std::runtime_error("cannot write " + target.string());
  }
  ytwe::atomic_move(temp, target);
}
inline void we_extractor_args(WeOptions &o, const std::string &input) {
  auto colon = input.find(':');
  auto body = colon == std::string::npos ? input : input.substr(colon + 1);
  std::istringstream fields(body);
  std::string field;
  while (std::getline(fields, field, ';')) {
    auto eq = field.find('=');
    if (eq == std::string::npos)
      throw std::runtime_error("invalid extractor argument");
    auto key = field.substr(0, eq), value = field.substr(eq + 1);
    if (key == "player_client")
      o.client = value;
    else if (key == "comment_sort")
      o.comment_options.newest = value != "top";
    else if (key == "max_comments")
      o.comment_options.limit =
          we_number(value.substr(0, value.find(',')), key);
    else if (key == "po_token") {
      std::istringstream list(value);
      std::string token;
      while (std::getline(list, token, ','))
        o.tokens.push_back(token);
    } else
      throw std::runtime_error("unsupported extractor argument: " + key);
  }
}
inline WeOptions we_options(int argc, char **argv) {
  WeOptions o;
  auto exe = ytwe::executable_path(argv[0]);
  o.solver.directory = exe.parent_path() / "vendor/ejs";
  if (!std::filesystem::exists(o.solver.directory))
    o.solver.directory = exe.parent_path().parent_path() / "vendor/ejs";
  if (!std::filesystem::exists(o.solver.directory))
    o.solver.directory = exe.parent_path().parent_path() / "share/ytcui-we/ejs";
  if (auto env = std::getenv("YTCUI_JS_RUNTIME"))
    o.solver.runtime = env;
  const std::set<std::string> commands{
      "media",  "info",          "comments",  "playlist",
      "search", "channel",       "community", "feed",
      "mix",    "notifications", "live-chat", "subtitles",
      "record", "clip",          "tab",       "search-date"};
  int first = 1;
  if (argc > 1 && commands.count(argv[1])) {
    o.command = argv[1];
    o.explicit_command = true;
    first = 2;
    o.action = (o.command == "record" ? "download" : "json");
  }
  bool compat = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--yt-dlp")
      compat = true;
  for (int i = first; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() {
      if (++i >= argc)
        throw std::runtime_error(a + " needs a value");
      return std::string(argv[i]);
    };
    if (a == "--") {
      while (++i < argc)
        o.targets.push_back(argv[i]);
      break;
    }
    if (a == "--check-runtime") {
      o.check_runtime = true;
      continue;
    } else if (a == "--yt-dlp")
      continue;
    if (a == "-p" || a == "--play")
      o.action = "play";
    else if (a == "-g" || a == "--get-url")
      o.action = "url";
    else if (a == "-F" || a == "-L" || a == "--list-formats")
      o.action = "formats";
    else if (a == "-d" || a == "--download")
      o.action = "download";
    else if (a == "-J" || a == "-j" || a == "--json" || a == "--dump-json" ||
             a == "--dump-single-json")
      o.action = "json";
    else if (a == "--skip-download" || a == "--simulate" ||
             (compat && a == "-s"))
      o.skip = true;
    else if (a == "-s" || a == "--search") {
      o.command = "search";
      o.targets.push_back(next());
      o.action = "json";
    } else if (a == "--cookies")
      o.cookies = next();
    else if (a == "--client")
      o.client = next();
    else if (a == "--po-token")
      o.tokens.push_back(next());
    else if (a == "--extractor-args")
      we_extractor_args(o, next());
    else if (a == "--js-runtime" || a == "--js-runtimes") {
      o.solver.runtime = next();
      if (o.solver.runtime.rfind("node:", 0) == 0)
        o.solver.runtime.erase(0, 5);
    } else if (a == "--ejs-dir")
      o.solver.directory = std::filesystem::u8path(next());
    else if (a == "--cache-dir")
      o.solver.cache.directory = std::filesystem::u8path(next());
    else if (a == "--no-cache")
      o.no_cache = true;
    else if (a == "--proxy")
      o.proxy = next();
    else if (a == "-4" || a == "--force-ipv4")
      ytwe::Http::ip_version = CURL_IPRESOLVE_V4;
    else if (a == "-6" || a == "--force-ipv6")
      ytwe::Http::ip_version = CURL_IPRESOLVE_V6;
    else if (a == "--skip-formats")
      o.skip_formats = true;
    else if (a == "-a" || a == "--audio") {
      if (compat) {
        std::ifstream f(std::filesystem::u8path(next()));
        if (!f)
          throw std::runtime_error("cannot read batch file");
        std::string line;
        while (std::getline(f, line))
          if (!line.empty() && line[0] != '#')
            o.targets.push_back(line);
      } else
        o.mode = Mode::AudioOnly;
    } else if (a == "-V" || a == "--video")
      o.mode = Mode::VideoOnly;
    else if (a == "-q" || a == "--quality") {
      if (compat)
        o.quiet = true;
      else
        o.quality = Quality::at(static_cast<int>(we_number(next(), a)));
    } else if (a == "--quiet" || a == "--no-warnings")
      o.quiet = true;
    else if (a == "-f" || a == "--format")
      o.format = next();
    else if (a == "-o" || a == "--output")
      o.output = next();
    else if (a == "-P" || a == "--paths")
      o.path = next();
    else if (a == "-N" || a == "--connections" || a == "--concurrent-fragments")
      o.download.connections = we_number(next(), a);
    else if (a == "--no-continue")
      o.download.resume = false;
    else if (a == "--continue" || a == "-c")
      o.download.resume = true;
    else if (a == "--force-overwrites")
      o.download.overwrite = true;
    else if (a == "--no-overwrites" || a == "-w")
      o.download.overwrite = false;
    else if (a == "-x" || a == "--extract-audio") {
      o.extract_audio = true;
      o.mode = Mode::AudioOnly;
    } else if (a == "--audio-format")
      o.audioformat = next();
    else if (a == "--merge-output-format" || a == "--remux-video")
      o.mergeformat = next();
    else if (a == "--record-seconds" || a == "--duration") {
      auto seconds = we_number(next(), a);
      if (!seconds ||
          seconds > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error(
            "recording duration must be a positive number of seconds");
      o.record_seconds = static_cast<int>(seconds);
    } else if (a == "--live-from-start")
      o.live_from_start = true;
    else if (a == "--download-sections") {
      auto value = next();
      if (!value.empty() && value[0] == '*')
        value.erase(0, 1);
      auto dash = value.find('-');
      if (dash == std::string::npos)
        throw std::runtime_error("section syntax: *START-END");
      o.section_start = ytwe::timestamp_seconds(value.substr(0, dash));
      o.section_end = ytwe::timestamp_seconds(value.substr(dash + 1));
      if (o.section_end <= o.section_start)
        throw std::runtime_error("section end must follow start");
    } else if (a == "--resolve-json") {
      o.resolve_json = true;
      o.action = "play";
    } else if (a == "--mpv-args") {
      o.mpv_json = true;
      o.action = "play";
    } else if (a == "--max-comments")
      o.comment_options.limit = we_number(next(), a);
    else if (a == "--max-depth")
      o.comment_options.max_depth = we_number(next(), a);
    else if (a == "--max-replies-per-thread")
      o.comment_options.max_replies_per_thread = we_number(next(), a);
    else if (a == "--max-pages")
      o.comment_options.max_pages = we_number(next(), a);
    else if (a == "--sort") {
      auto value = next();
      if (value != "top" && value != "newest")
        throw std::runtime_error("sort must be newest or top");
      o.comment_options.newest = value == "newest";
    } else if (a == "--no-replies")
      o.comment_options.replies = false;
    else if (a == "--write-comments")
      o.comments = true;
    else if (a == "--write-info-json")
      o.write_info = true;
    else if (a == "--write-subs" || a == "--write-sub")
      o.write_subs = true;
    else if (a == "--write-auto-subs" || a == "--write-auto-sub")
      o.write_auto = true;
    else if (a == "--list-subs") {
      o.list_subs = true;
      o.skip = true;
    } else if (a == "--audio-lang" || a == "--audio-language")
      o.audio_language = next();
    else if (a == "--lang" || a == "--sub-langs" || a == "--sub-lang")
      o.language = next();
    else if (a == "--sub-format")
      o.subformat = next();
    else if (a == "--auto-subs")
      o.automatic = true;
    else if (a == "--flat-playlist")
      o.flat = true;
    else if (a == "--no-playlist")
      o.no_playlist = true;
    else if (a == "--yes-playlist")
      o.no_playlist = false;
    else if (a == "--limit")
      o.limit = we_number(next(), a);
    else if (a == "--playlist-start")
      o.start = we_number(next(), a);
    else if (a == "--playlist-end")
      o.end = we_number(next(), a);
    else if (a == "--playlist-reverse")
      o.reverse = true;
    else if (a == "--print")
      o.print = next();
    else if (a == "--download-archive")
      o.archive = next();
    else if (a == "--ignore-errors" || a == "-i")
      o.ignore_errors = true;
    else if (a == "--batch-file") {
      std::ifstream f(std::filesystem::u8path(next()));
      if (!f)
        throw std::runtime_error("cannot read batch file");
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty() && line[0] != '#')
          o.targets.push_back(line);
      }
    } else if (!a.empty() && a[0] == '-')
      throw std::runtime_error("unsupported option: " + a);
    else
      o.targets.push_back(a);
  }
  if (o.command == "notifications" && o.targets.empty())
    o.targets.push_back("notifications");
  if (o.targets.empty() && !o.check_runtime)
    throw std::runtime_error("an input URL, ID or query is required");
  if (!o.comment_options.max_pages)
    throw std::runtime_error("max-pages must be positive");
  if (!o.download.connections || o.download.connections > 32)
    throw std::runtime_error("connections must be between 1 and 32");
  if (o.subformat == "best")
    o.subformat = "vtt";
  if (o.subformat.find('/') != std::string::npos) {
    for (auto &f : ytwe::split_expression(o.subformat, '/'))
      if (f == "vtt" || f == "ttml" || f == "srv3" || f == "srv2" ||
          f == "srv1" || f == "json3") {
        o.subformat = f;
        break;
      }
  }
  o.solver.cache.enabled = !o.no_cache;
  if (o.action == "play")
    o.quality.for_playback = true;
  return o;
}
inline std::filesystem::path
we_output_path(const WeOptions &o, const VideoInfo &info, const StreamFormat &f,
               const std::string &ext, size_t index = 0,
               const std::string &playlist = "") {
  auto name = ytwe::output_template(
      o.output.empty() ? "%(title)s [%(id)s].%(ext)s" : o.output, info, f, ext,
      index, playlist);
  auto path = std::filesystem::u8path(name);
  if (!o.path.empty() && !path.is_absolute())
    path = std::filesystem::u8path(o.path) / path;
  return path;
}
inline int we_video(WeOptions options, ytwe::Youtube &yt,
                    const std::string &input, size_t index = 0,
                    const std::string &playlist = "") {
  const bool clip = input.find("/clip/") != std::string::npos;
  const bool live_alias = (input.find("/@") != std::string::npos ||
                           input.find("/channel/") != std::string::npos) &&
                          input.find("/live") != std::string::npos;
  auto id = (clip || live_alias) ? std::string{} : we_video_id(input);
  if (clip || live_alias) {
    yt.bootstrap_url(input, "");
    auto root = yj::parse(yt.initial_json());
    id = ytwe::str(
        yj::path(root, "currentVideoEndpoint", "watchEndpoint", "videoId"));
    if (id.empty())
      id = ytwe::str(
          yj::path(yj::parse(yt.player_json()), "videoDetails", "videoId"));
    auto loop = clip ? ytwe::first(root, "loopCommand") : yj::Val{};
    if (loop) {
      options.section_start = yj::get(loop, "startTimeMs").dbl() / 1000;
      options.section_end = yj::get(loop, "endTimeMs").dbl() / 1000;
    }
    if (id.empty())
      throw std::runtime_error("redirected video ID is missing");
  }
  if (!options.archive.empty() && options.action == "download") {
    std::ifstream archive(std::filesystem::u8path(options.archive));
    std::string line;
    while (std::getline(archive, line))
      if (line == "youtube " + id) {
        if (!options.quiet)
          std::fprintf(stderr, "%s already in archive\n", id.c_str());
        return 0;
      }
  }
  yt.bootstrap(id);
  if (options.command == "comments") {
    auto result =
        yt.comments(id, options.comment_options, [&](const ytwe::Comment &c) {
          yjw::Writer w(false);
          c.write(w);
          std::puts(w.str().c_str());
        });
    if (!options.quiet)
      std::fprintf(stderr, "%zu comments, %zu pages (%s)\n", result.count,
                   result.pages, result.status.c_str());
    return 0;
  }
  std::optional<ytwe::MediaResult> media;
  if (!options.skip_formats || options.action == "url" ||
      options.action == "formats" || options.action == "play")
    media = ytwe::resolve_media(yt, id, options.solver, options.client);
  std::string caption_player = yt.player_json(), caption_client = "web";
  std::string caption_agent =
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/131.0.0.0 Safari/537.36";
  if (media && yj::get(yj::parse(media->player_json), "captions")) {
    caption_player = media->player_json;
    caption_client = media->client;
    caption_agent = media->ua;
  }
  if ((options.command == "subtitles" || options.list_subs ||
       options.write_auto || options.write_subs) &&
      (!media || !yj::get(yj::parse(caption_player), "captions"))) {
    auto alternative = yt.player_response(id, "tv_samsung");
    if (yj::get(yj::parse(alternative), "captions"))
      caption_player = alternative;
    caption_client = "tv_samsung";
    caption_agent =
        ytwe::str(yj::path(yj::parse(ytwe::client_spec(caption_client).context),
                           "client", "userAgent"));
  }
  caption_player =
      ytwe::caption_translation_context(caption_player, yt.player_json());
  auto tracks = ytwe::subtitle_tracks(caption_player);
  if (options.command == "subtitles" || options.list_subs) {
    size_t count = 0;
    for (auto &track : tracks) {
      if (!ytwe::language_matches(track.language, options.language) ||
          track.ext != options.subformat)
        continue;
      if (options.command == "subtitles" &&
          track.automatic != options.automatic)
        continue;
      if (options.list_subs)
        std::printf("%s\t%s\t%s\t%s\n", track.language.c_str(),
                    track.automatic ? "automatic" : "manual", track.ext.c_str(),
                    track.name.c_str());
      else if (options.output.empty())
        std::puts(track.url.c_str());
      else
        ytwe::save_subtitle(track, std::filesystem::u8path(options.output),
                            caption_agent, yt.token(caption_client, "subs", id),
                            options.download.overwrite);
      ++count;
    }
    if (options.command == "subtitles" && !count)
      throw std::runtime_error("no matching subtitle tracks");
    if (options.command == "subtitles")
      return 0;
  }
  if (options.action == "formats") {
    if (!media)
      throw std::runtime_error("format resolution required");
    print_formats(media->info);
    return 0;
  }
  auto metadata = we_metadata(yt, id, options.comments, options.comment_options,
                              media ? &*media : nullptr);
  if (options.section_start >= 0) {
    metadata = ytwe::json_set(metadata, "section_start",
                              std::to_string(options.section_start));
    metadata = ytwe::json_set(metadata, "section_end",
                              std::to_string(options.section_end));
  }
  if (options.action == "json" || options.command == "info" ||
      options.command == "clip")
    std::puts(metadata.c_str());
  if (!media || options.action == "json" || options.command == "info" ||
      options.skip) {
    VideoInfo fallback;
    if (!media)
      ytwe::parse_player(yt.player_json(), id, fallback);
    const auto &info = media ? media->info : fallback;
    StreamFormat empty;
    empty.format_id = "metadata";
    const auto &f = info.formats.empty() ? empty : info.formats.front();
    auto output = we_output_path(options, info, f, "mp4", index, playlist);
    if (options.write_info || options.comments) {
      output.replace_extension(".info.json");
      write_text(output, metadata);
    }
    if (options.write_subs || options.write_auto) {
      std::set<std::string> saved;
      for (auto &track : tracks)
        if (track.ext == options.subformat &&
            ytwe::language_matches(track.language, options.language) &&
            ((track.automatic && options.write_auto) ||
             (!track.automatic && options.write_subs)) &&
            saved.insert(track.language).second) {
          auto sub = output;
          sub.replace_extension("." + track.language + "." + track.ext);
          ytwe::save_subtitle(track, sub, caption_agent,
                              yt.token(caption_client, "subs", id),
                              options.download.overwrite);
        }
    }
    if (!options.print.empty()) {
      auto pattern = options.print.find("%(") == std::string::npos
                         ? "%(" + options.print + ")s"
                         : options.print;
      std::puts(
          ytwe::output_template(pattern, info, f, "mp4", index, playlist, false)
              .c_str());
    }
    return 0;
  }
  if (!options.audio_language.empty() && options.audio_language != "all") {
    auto &formats = media->info.formats;
    formats.erase(
        std::remove_if(formats.begin(), formats.end(),
                       [&](const StreamFormat &f) {
                         return f.is_audio_only() && !f.language.empty() &&
                                !ytwe::language_matches(f.language,
                                                        options.audio_language);
                       }),
        formats.end());
  }
  auto selected = ytwe::select_format(media->info.formats, options.format,
                                      options.mode, options.quality);
  const bool manifest_only =
      media->info.formats.empty() && !media->info.hls_url.empty();
  if (!selected.ok() && !manifest_only)
    throw std::runtime_error("no formats match the requested tracks/quality");
  if (options.action == "url") {
    if (manifest_only)
      std::puts(media->info.hls_url.c_str());
    else {
      if (selected.video)
        std::puts(selected.video->url.c_str());
      if (selected.audio)
        std::puts(selected.audio->url.c_str());
    }
    return 0;
  }
  if (options.resolve_json) {
    yjw::Writer w(false);
    w.obj();
    w.kv("video_url", manifest_only ? media->info.hls_url :
         selected.video ? selected.video->url : selected.audio->url);
    w.kv("audio_url", selected.video && selected.audio ? selected.audio->url : std::string{});
    w.kv("user_agent", manifest_only ? media->ua :
         selected.video ? selected.video->user_agent : selected.audio->user_agent);
    w.kv("is_live", manifest_only || media->info.is_live_now);
    w.key("heights").arr();
    std::set<int, std::greater<int>> heights;
    for (const auto& f : media->info.formats)
      if (f.has_video && f.height > 0 && !f.url.empty()) heights.insert(f.height);
    for (int height : heights) w.val(height);
    w.end().end();
    std::puts(w.str().c_str());
    return 0;
  }
  if (options.action == "play" || options.mpv_json) {
    auto args = manifest_only
                    ? mpv_args_hls(media->info.hls_url, options.mode,
                                   media->info.title, 100, media->ua.c_str())
                    : mpv_args(selected, selected.mode, media->info.title, 100,
                               (selected.mode != Mode::AudioOnly),
                               (selected.video ? selected.video->user_agent
                                               : selected.audio->user_agent)
                                   .c_str());
    if (options.section_start >= 0) {
      args.push_back("--start=" + std::to_string(options.section_start));
      args.push_back("--end=" + std::to_string(options.section_end));
    }
    if (options.live_from_start)
      args.push_back("--demuxer-lavf-o=live_start_index=0");
    if (options.mpv_json) {
      yjw::Writer w(false);
      w.arr();
      for (auto &a : args)
        w.val(a);
      w.end();
      std::puts(w.str().c_str());
      return 0;
    }
    return spawn_mpv(args);
  }
  const auto *main = selected.video ? selected.video : selected.audio;
  StreamFormat manifest;
  if (!main) {
    manifest.url = media->info.hls_url;
    manifest.protocol = "m3u8_native";
    manifest.user_agent = media->ua;
    manifest.has_video = manifest.has_audio = true;
    manifest.format_id = "hls";
    main = &manifest;
    selected.video = main;
    selected.muxed = true;
  }
  std::string ext = main->has_video ? "mp4"
                    : main->container == "webm" || main->audio_codec == "opus"
                        ? "opus"
                        : "m4a";
  if (selected.video && selected.audio) {
    bool webm = selected.video->container == "webm" &&
                selected.audio->audio_codec == "opus";
    ext = webm ? "webm" : "mkv";
  }
  if (!options.mergeformat.empty())
    ext = options.mergeformat;
  if (options.extract_audio && options.audioformat != "best")
    ext = options.audioformat;
  auto output =
      we_output_path(options, media->info, *main, ext, index, playlist);
  if (!options.print.empty()) {
    auto pattern = options.print.find("%(") == std::string::npos
                       ? "%(" + options.print + ")s"
                       : options.print;
    std::puts(ytwe::output_template(pattern, media->info, *main, ext, index,
                                    playlist, false)
                  .c_str());
    return 0;
  }
  if (options.write_info || options.comments) {
    auto sidecar = output;
    sidecar.replace_extension(".info.json");
    write_text(sidecar, metadata);
  }
  if (options.write_subs || options.write_auto) {
    std::set<std::string> saved;
    for (auto &track : tracks)
      if (track.ext == options.subformat &&
          ytwe::language_matches(track.language, options.language) &&
          ((track.automatic && options.write_auto) ||
           (!track.automatic && options.write_subs)) &&
          saved.insert(track.language).second) {
        auto subtitle = output;
        subtitle.replace_extension("." + track.language + "." + track.ext);
        ytwe::save_subtitle(track, subtitle, caption_agent,
                            yt.token(caption_client, "subs", id),
                            options.download.overwrite);
      }
  }
  if (!options.quiet)
    std::fprintf(stderr, "%s -> %s\n", media->info.title.c_str(),
                 output.u8string().c_str());
  if (std::filesystem::exists(output) && !options.download.overwrite) {
    if (!options.quiet)
      std::fprintf(stderr, "output exists, skipping\n");
    return 0;
  }
  if (options.section_start >= 0 || options.live_from_start ||
      options.record_seconds > 0) {
    std::vector<std::string> args{"-y"};
    auto add = [&](const StreamFormat *f) {
      if (!f)
        return;
      if (options.live_from_start && f->protocol == "m3u8_native")
        args.insert(args.end(), {"-live_start_index", "0"});
      if (options.section_start >= 0)
        args.insert(args.end(), {"-ss", std::to_string(options.section_start)});
      args.insert(args.end(),
                  {"-protocol_whitelist", "file,http,https,tcp,tls,crypto,data",
                   "-user_agent", f->user_agent, "-i", ytwe::playback_url(*f)});
    };
    add(selected.video);
    add(selected.audio);
    if (selected.video && selected.audio)
      args.insert(args.end(), {"-map", "0:v:0", "-map", "1:a:0"});
    if (options.section_end > options.section_start &&
        options.section_start >= 0)
      args.insert(args.end(), {"-t", std::to_string(options.section_end -
                                                    options.section_start)});
    else if (options.record_seconds > 0)
      args.insert(args.end(), {"-t", std::to_string(options.record_seconds)});
    args.insert(args.end(), {"-c", "copy", output.u8string()});
    if (ytwe::ffmpeg(args))
      throw std::runtime_error("section/live download failed");
  } else
    ytwe::download_selection(selected.video, selected.audio, output,
                             options.download,
                             options.extract_audio ? options.audioformat : "",
                             options.record_seconds);
  if (!options.archive.empty()) {
    std::ofstream archive(std::filesystem::u8path(options.archive),
                          std::ios::app);
    archive << "youtube " << id << '\n';
    if (!archive)
      throw std::runtime_error("cannot update download archive");
  }
  return 0;
}
inline int we_application(int argc, char **argv) {
  if (argc == 1 || std::string(argv[1]) == "--help" ||
      std::string(argv[1]) == "-h" || std::string(argv[1]) == "--native-help") {
    we_help();
    return 0;
  }
  if (std::string(argv[1]) == "--version") {
    std::puts("ytcui-we 0.2.0 (native C++ YouTube client)");
    return 0;
  }
  try {
    auto options = we_options(argc, argv);
    if (options.check_runtime) {
      const auto script = ytwe::read_file(options.solver.directory / "yt.solver.lib.js") +
          "\nObject.assign(globalThis, lib);\n" +
          ytwe::read_file(options.solver.directory / "yt.solver.core.js") +
          "\nif(typeof jsc !== 'function') throw Error('missing jsc'); console.log('ytcui-ejs-ready');\n";
      if (ytwe::run_js(options.solver.runtime, script) != "ytcui-ejs-ready\n")
        throw std::runtime_error("unexpected EJS runtime self-test output");
      std::puts("EJS assets and Node runtime ready");
      return 0;
    }
    ytwe::Youtube yt;
    yt.cache = options.solver.cache;
    ytwe::manifest_cache.directory = options.solver.cache.directory;
    if (!options.cookies.empty())
      yt.cookies(options.cookies);
    for (auto &token : options.tokens)
      yt.set_po_token(token);
    ytwe::Http::default_proxy = options.proxy;
    int result = 0;
    for (auto input : options.targets) {
      try {
        auto local = options;
        std::string kind = local.command;
        if (kind.empty()) {
          if (input.rfind("ytsearch", 0) == 0) {
            auto colon = input.find(':');
            if (colon == std::string::npos)
              throw std::runtime_error("search syntax: ytsearchN:QUERY");
            const bool newest = input.rfind("ytsearchdate", 0) == 0;
            const size_t prefix = newest ? 12 : 8;
            auto amount = input.substr(prefix, colon - prefix);
            local.limit = amount.empty()    ? 1
                          : amount == "all" ? 1000000
                                            : we_number(amount, "search count");
            input = input.substr(colon + 1);
            kind = newest ? "search-date" : "search";
          } else if (input.find("/results?") != std::string::npos) {
            kind = "search";
            input = ytwe::url_query(input, "search_query");
          } else if ((input.find("/@") != std::string::npos ||
                      input.find("/channel/") != std::string::npos) &&
                     input.find("/live") != std::string::npos)
            kind = "media";
          else if (input.find("/playlist?") != std::string::npos &&
                   !local.no_playlist)
            kind =
                we_playlist_id(input).rfind("RD", 0) == 0 ? "mix" : "playlist";
          else if (input.find("/@") != std::string::npos ||
                   input.find("/channel/") != std::string::npos ||
                   input.find("/user/") != std::string::npos ||
                   input.find("/c/") != std::string::npos)
            kind = input.find("/posts") != std::string::npos ||
                           input.find("/community") != std::string::npos
                       ? "community"
                       : "channel";
          else if (input.find("/feed/") != std::string::npos ||
                   input.find("/hashtag/") != std::string::npos ||
                   input.find("/post/") != std::string::npos)
            kind = "tab";
          else if (input == ":ytnotifications")
            kind = "notifications";
          else
            kind = "media";
        }
        if (kind == "live-chat") {
          auto r = ytwe::live_chat(
              yt, we_video_id(input), local.limit,
              local.comment_options.max_pages,
              [](const std::string &action) { std::puts(action.c_str()); });
          if (!local.quiet)
            std::fprintf(stderr, "%zu chat actions (%zu pages)\n", r.count,
                         r.pages);
          continue;
        }
        bool list = kind == "playlist" || kind == "search" ||
                    kind == "search-date" || kind == "tab" ||
                    kind == "channel" || kind == "community" ||
                    kind == "feed" || kind == "mix" || kind == "notifications";
        if (list) {
          std::vector<ytwe::Entry> entries;
          ytwe::CollectionResult collection_result;
          if (kind == "playlist") {
            auto id = we_playlist_id(input);
            yt.bootstrap_playlist(id);
            auto r =
                yt.playlist(id, local.limit, local.comment_options.max_pages,
                            [&](const ytwe::Youtube::PlaylistEntry &e) {
                              ytwe::Entry entry;
                              entry.id = e.id;
                              entry.title = e.title;
                              entry.channel = e.channel;
                              entry.duration = e.duration;
                              entry.url =
                                  "https://www.youtube.com/watch?v=" + e.id;
                              entries.push_back(entry);
                            });
            collection_result.count = r.count;
            collection_result.pages = r.pages;
            collection_result.exhausted = r.exhausted;
          } else
            collection_result = ytwe::collection(
                yt, kind, input, local.limit, local.comment_options.max_pages,
                [&](const ytwe::Entry &e) { entries.push_back(e); });
          if (local.reverse)
            std::reverse(entries.begin(), entries.end());
          bool download =
              local.action == "play" ||
              (!local.explicit_command && local.action == "download" &&
               !local.flat && !local.skip);
          if (download) {
            for (size_t i = 0; i < entries.size(); ++i)
              if (i + 1 >= local.start && (!local.end || i + 1 <= local.end) &&
                  entries[i].type == "video") {
                auto video_options = local;
                video_options.command = "media";
                we_video(video_options, yt, entries[i].url, i + 1, input);
              }
          } else {
            yjw::Writer w;
            w.obj()
                .kv("_type", "playlist")
                .kv("id", input)
                .kv("title", collection_result.title)
                .kv("extractor", "youtube:" + kind)
                .kv("exhausted", collection_result.exhausted)
                .kv("pages", static_cast<int64_t>(collection_result.pages))
                .key("entries")
                .arr();
            for (size_t i = 0; i < entries.size(); ++i)
              if (i + 1 >= local.start && (!local.end || i + 1 <= local.end))
                entries[i].write(w);
            w.end().end();
            std::puts(w.str().c_str());
          }
          continue;
        }
        local.command = kind;
        if (kind == "info")
          local.action = "json";
        if (kind == "record")
          local.action = "download";
        auto status = we_video(local, yt, input);
        if (status)
          result = status;
      } catch (const std::exception &e) {
        std::fprintf(stderr, "ytcui-we: %s\n", e.what());
        result = 1;
        if (!options.ignore_errors)
          break;
      }
    }
    return result;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ytcui-we: %s\n", e.what());
    return 2;
  }
}

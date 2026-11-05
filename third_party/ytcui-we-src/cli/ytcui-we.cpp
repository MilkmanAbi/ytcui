// The native extractor reuses the established C++ downloader/player CLI.
// No browser automation and no Python/yt-dlp subprocess are involved.
#include "we_cli_support.h"
#include "we_subtitles.h"
#include "yt_media.h"
#include "yt_youtube.h"
#include <limits>
#include <map>
#include <optional>

static size_t we_number(const std::string &s, const std::string &flag) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error(flag + " needs a nonnegative integer");
  try {
    return std::stoull(s);
  } catch (...) {
    throw std::runtime_error(flag + " is out of range");
  }
}
static std::string we_playlist_id(std::string input) {
  auto pos = input.find("list=");
  if (pos != std::string::npos)
    input =
        input.substr(pos + 5, input.find_first_of("&#", pos + 5) - (pos + 5));
  if (input.empty() ||
      input.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
          std::string::npos)
    throw std::runtime_error("invalid playlist ID or URL");
  return input;
}
static std::string we_video_id(const std::string &input) {
  auto id = ytfast::extract_video_id(input);
  if (id.size() != 11 ||
      id.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
          std::string::npos)
    throw std::runtime_error("invalid YouTube video ID or URL");
  return id;
}
static void we_captions(yjw::Writer &w, yj::Val player, bool automatic) {
  w.obj();
  std::map<std::string, std::vector<ytwe::SubtitleTrack>> tracks;
  // The response string owns every scanner view until traversal ends.
  for (auto &track : ytwe::subtitle_tracks(ytwe::raw(player)))
    if (track.automatic == automatic)
      tracks[track.language].push_back(track);
  for (auto &language : tracks) {
    w.key(language.first).arr();
    for (auto &track : language.second)
      w.obj()
          .kv("ext", track.ext)
          .kv("name", track.name)
          .kv("url", track.url)
          .end();
    w.end();
  }
  w.end();
}
static std::string we_metadata(ytwe::Youtube &yt, const std::string &id,
                               bool with_comments,
                               const ytwe::CommentOptions &opt,
                               const ytwe::MediaResult *media) {
  auto player = yj::parse(yt.player_json());
  auto details = yj::get(player, "videoDetails");
  if (!details && media) {
    player = yj::parse(media->player_json);
    details = yj::get(player, "videoDetails");
  }
  auto micro = yj::path(player, "microformat", "playerMicroformatRenderer");
  if (!details)
    throw std::runtime_error(
        "No video metadata: " +
        ytwe::str(yj::path(player, "playabilityStatus", "reason")));
  yjw::Writer w;
  w.obj()
      .kv("id", id)
      .kv("extractor", "youtube")
      .kv("extractor_key", "Youtube")
      .kv("webpage_url", "https://www.youtube.com/watch?v=" + id)
      .kv("title", ytwe::str(yj::get(details, "title")))
      .kv("description", ytwe::str(yj::get(details, "shortDescription")))
      .kv("channel", ytwe::str(yj::get(details, "author")))
      .kv("uploader", ytwe::str(yj::get(details, "author")))
      .kv("channel_id", ytwe::str(yj::get(details, "channelId")))
      .kv("duration", yj::get(details, "lengthSeconds").i64())
      .kv("view_count", yj::get(details, "viewCount").i64())
      .kv("is_live", yj::get(details, "isLiveContent").boolean())
      .kv("live_status",
          yj::get(details, "isLive").boolean()
              ? "is_live"
              : (yj::get(details, "isLiveContent").boolean() ? "was_live"
                                                             : "not_live"))
      .kv("playability_status",
          ytwe::str(yj::path(player, "playabilityStatus", "status")))
      .kv("upload_date",
          ytwe::date_digits(ytwe::str(yj::get(micro, "uploadDate"))))
      .kv("release_date",
          ytwe::date_digits(ytwe::str(yj::get(micro, "publishDate"))))
      .kv("category", ytwe::str(yj::get(micro, "category")));
  const auto channel_id = ytwe::str(yj::get(details, "channelId"));
  if (!channel_id.empty())
    w.kv("channel_url", "https://www.youtube.com/channel/" + channel_id);
  w.kv("availability",
       yj::get(details, "isPrivate").boolean()
           ? "private"
           : (yj::get(micro, "isUnlisted").boolean() ? "unlisted" : "public"));
  w.key("tags").arr();
  yj::each_elem(yj::get(details, "keywords"), [&](yj::Val t) {
    w.val(ytwe::str(t));
    return true;
  });
  w.end();
  w.key("thumbnails").arr();
  yj::each_elem(yj::path(details, "thumbnail", "thumbnails"), [&](yj::Val t) {
    w.obj()
        .kv("url", ytwe::str(yj::get(t, "url")))
        .kv("width", yj::get(t, "width").i64())
        .kv("height", yj::get(t, "height").i64())
        .end();
    return true;
  });
  w.end();
  auto caption_body =
      media && yj::get(yj::parse(media->player_json), "captions")
          ? media->player_json
          : yt.player_json();
  caption_body =
      ytwe::caption_translation_context(caption_body, yt.player_json());
  auto captions = yj::parse(caption_body);
  w.key("subtitles");
  we_captions(w, captions, false);
  w.key("automatic_captions");
  we_captions(w, captions, true);
  w.key("chapters").arr();
  std::vector<std::pair<int64_t, std::string>> chapters;
  auto initial = yj::parse(yt.initial_json());
  const std::string_view chapter_keys[] = {"chapterRenderer"};
  yj::find_any(initial, chapter_keys, 1,
               [&](std::string_view, yj::Val chapter) {
                 auto start = yj::get(chapter, "timeRangeStartMillis").i64();
                 auto title = ytwe::text(yj::get(chapter, "title"));
                 if (chapters.empty() || chapters.back().first != start)
                   chapters.emplace_back(start, title);
                 return true;
               });
  std::sort(chapters.begin(), chapters.end());
  chapters.erase(std::unique(chapters.begin(), chapters.end()), chapters.end());
  for (size_t i = 0; i < chapters.size(); ++i)
    w.obj()
        .kv("start_time", chapters[i].first / 1000.0)
        .kv("end_time", i + 1 < chapters.size()
                            ? chapters[i + 1].first / 1000.0
                            : yj::get(details, "lengthSeconds").dbl())
        .kv("title", chapters[i].second)
        .end();
  w.end();
  w.key("formats").arr();
  size_t unresolved = 0;
  if (media)
    for (const auto &f : media->info.formats)
      emit_format(w, f);
  else
    for (const auto *name : {"formats", "adaptiveFormats"})
      yj::each_elem(yj::path(player, "streamingData", name), [&](yj::Val f) {
        auto url = ytwe::str(yj::get(f, "url"));
        if (url.empty()) {
          ++unresolved;
          return true;
        }
        w.obj()
            .kv("format_id", std::to_string(yj::get(f, "itag").i64()))
            .kv("url", url)
            .kv("mime_type", ytwe::str(yj::get(f, "mimeType")))
            .kv("width", yj::get(f, "width").i64())
            .kv("height", yj::get(f, "height").i64())
            .kv("fps", yj::get(f, "fps").i64())
            .kv("tbr", yj::get(f, "bitrate").i64() / 1000.0)
            .kv("filesize", yj::get(f, "contentLength").i64())
            .end();
        return true;
      });
  w.end();
  w.kv("unresolved_format_count", static_cast<int64_t>(unresolved))
      .kv("hls_manifest_url",
          media
              ? media->info.hls_url
              : ytwe::str(yj::path(player, "streamingData", "hlsManifestUrl")))
      .kv("dash_manifest_url", media
                                   ? media->info.dash_url
                                   : ytwe::str(yj::path(player, "streamingData",
                                                        "dashManifestUrl")));
  if (media)
    w.kv("player_client", media->client).kv("http_user_agent", media->ua);
  if (with_comments) {
    w.key("comments").arr();
    auto result =
        yt.comments(id, opt, [&](const ytwe::Comment &c) { c.write(w); });
    w.end();
    w.kv("comment_count_extracted", static_cast<int64_t>(result.count))
        .kv("comments_status", result.status);
  }
  w.end();
  return w.str();
}
#include "we_application.h"
int main(int argc, char **argv) {
#ifdef _WIN32
  (void)argc;
  (void)argv;
  SetConsoleOutputCP(CP_UTF8);
  int count = 0;
  auto args = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> values;
  std::vector<char *> pointers;
  for (int i = 0; i < count; ++i)
    values.push_back(ytwe::utf8(args[i]));
  LocalFree(args);
  for (auto &s : values)
    pointers.push_back(s.data());
  return we_application(count, pointers.data());
#else
  return we_application(argc, argv);
#endif
}

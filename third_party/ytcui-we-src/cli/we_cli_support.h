#pragma once
#include "we_download.h"
#include "yt_media.h"
#include "yt_select.h"
#include <iostream>
using namespace ytfast;
struct CurlGlobalInit {};
inline void emit_format(yjw::Writer &w, const StreamFormat &f) {
  w.obj()
      .kv("itag", f.itag)
      .kv("format_id",
          f.format_id.empty() ? std::to_string(f.itag) : f.format_id)
      .kv("url", f.url)
      .kv("ext", std::string(f.container))
      .kv("mime_type", std::string(f.mime_type))
      .kv("width", f.width)
      .kv("height", f.height)
      .kv("fps", f.fps)
      .kv("vcodec", f.has_video ? std::string(f.video_codec) : "none")
      .kv("acodec", f.has_audio ? std::string(f.audio_codec) : "none")
      .kv("tbr", f.effective_bitrate() / 1000.0)
      .kv("bitrate", f.bitrate)
      .kv("filesize", f.content_length)
      .kv("language", f.language)
      .kv("format_note", f.format_note)
      .kv("protocol", f.protocol)
      .kv("source_client", f.source_client)
      .kv("has_audio", f.has_audio)
      .kv("has_video", f.has_video)
      .kv("is_drc", f.is_drc)
      .key("http_headers")
      .obj()
      .kv("User-Agent", f.user_agent)
      .end()
      .end();
}
inline void emit_info(yjw::Writer &w, const VideoInfo &info,
                      const Selection *) {
  w.obj()
      .kv("id", info.id)
      .kv("title", info.title)
      .kv("description", info.description)
      .kv("channel", info.channel)
      .kv("channel_id", info.channel_id)
      .kv("duration", info.duration_secs)
      .kv("view_count", info.view_count)
      .kv("is_live", info.is_live_now)
      .kv("webpage_url", info.url)
      .key("formats")
      .arr();
  for (auto &f : info.formats)
    emit_format(w, f);
  w.end().end();
}
inline void print_formats(const VideoInfo &info) {
  std::printf("%-24s %-8s %-12s %-18s %-12s %s\n", "ID", "EXT", "RESOLUTION",
              "CODEC", "LANG", "PROTOCOL");
  for (auto &f : info.formats)
    std::printf(
        "%-24s %-8s %-12s %-18s %-12s %s\n", f.format_id.c_str(),
        std::string(f.container).c_str(),
        (f.has_video ? std::to_string(f.width) + "x" + std::to_string(f.height)
                     : "audio")
            .c_str(),
        std::string(f.has_video ? f.video_codec : f.audio_codec).c_str(),
        f.language.c_str(), f.protocol.c_str());
}
inline std::vector<std::string> mpv_args(const Selection &sel, Mode mode,
                                         const std::string &title, int, bool,
                                         const char *ua) {
  std::vector<std::string> args{"mpv", "--ytdl=no",
                                "--force-media-title=" + title,
                                "--user-agent=" + std::string(ua)};
  if (mode == Mode::AudioOnly)
    args.push_back("--no-video");
  if (sel.video)
    args.push_back(ytwe::playback_url(*sel.video));
  if (sel.audio) {
    if (sel.video)
      args.push_back("--audio-file=" + ytwe::playback_url(*sel.audio));
    else
      args.push_back(ytwe::playback_url(*sel.audio));
  }
  return args;
}
inline std::vector<std::string> mpv_args_hls(const std::string &url, Mode mode,
                                             const std::string &title,
                                             int volume, const char *ua) {
  Selection s;
  StreamFormat f;
  f.url = url;
  s.video = &f;
  return mpv_args(s, mode, title, volume, true, ua);
}
inline int spawn_mpv(const std::vector<std::string> &args) {
  return ytwe::process(args).code;
}
struct DownloadOptions {
  std::string user_agent;
  size_t connections = 4;
  bool resume = true;
};
inline bool download_one(const StreamFormat &f, const std::string &target,
                         const DownloadOptions &options, const char *) {
  ytwe::DownloadConfig o;
  o.user_agent = options.user_agent;
  o.connections = options.connections;
  o.resume = options.resume;
  ytwe::download_file(f, target, o);
  return true;
}
struct StreamDownloader {
  struct Result {
    bool ok = true;
    std::string error;
  };
  static Result fetch(const std::string &video, const std::string &audio,
                      const std::string &path, const char *ua, bool audio_only,
                      bool) {
    try {
      StreamFormat v, a;
      v.url = video;
      v.user_agent = ua;
      a.url = audio;
      a.user_agent = ua;
      ytwe::download_selection(video.empty() ? nullptr : &v,
                               audio.empty() ? nullptr : &a, path, {},
                               audio_only ? "mp3" : "");
      return {};
    } catch (const std::exception &e) {
      return {false, e.what()};
    }
  }
};

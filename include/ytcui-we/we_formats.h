#pragma once
#include "yt_types.h"
#include "yt_youtube.h"
namespace ytwe {
inline void parse_player(const std::string &response, const std::string &id,
                         ytfast::VideoInfo &info) {
  auto root = yj::parse(response);
  auto details = yj::get(root, "videoDetails");
  auto sd = yj::get(root, "streamingData");
  info.id = id;
  info.url = "https://www.youtube.com/watch?v=" + id;
  info.title = str(yj::get(details, "title"));
  info.description = str(yj::get(details, "shortDescription"));
  info.channel = str(yj::get(details, "author"));
  info.channel_id = str(yj::get(details, "channelId"));
  info.duration_secs =
      static_cast<int>(yj::get(details, "lengthSeconds").i64());
  info.view_count = yj::get(details, "viewCount").i64();
  info.upload_date = date_digits(str(yj::path(
      root, "microformat", "playerMicroformatRenderer", "uploadDate")));
  info.is_live = yj::get(details, "isLiveContent").boolean();
  info.is_live_now = yj::get(details, "isLive").boolean();
  info.hls_url = str(yj::get(sd, "hlsManifestUrl"));
  info.dash_url = str(yj::get(sd, "dashManifestUrl"));
  yj::each_elem(yj::path(details, "thumbnail", "thumbnails"), [&](yj::Val t) {
    info.thumbnails.push_back({str(yj::get(t, "url")),
                               static_cast<int>(yj::get(t, "width").i64()),
                               static_cast<int>(yj::get(t, "height").i64())});
    return true;
  });
  if (!info.thumbnails.empty())
    info.thumbnail_url = info.thumbnails.back().url;
  auto &arena = info.ensure_arena();
  size_t missing = 0;
  for (const char *name : {"formats", "adaptiveFormats"})
    yj::each_elem(yj::get(sd, name), [&](yj::Val f) {
      ytfast::StreamFormat sf;
      sf.url = str(yj::get(f, "url"));
      if (sf.url.empty()) {
        ++missing;
        return true;
      }
      sf.itag = static_cast<int>(yj::get(f, "itag").i64());
      sf.format_id = std::to_string(sf.itag);
      sf.content_id = id;
      sf.width = static_cast<int>(yj::get(f, "width").i64());
      sf.height = static_cast<int>(yj::get(f, "height").i64());
      sf.fps = static_cast<int>(yj::get(f, "fps").i64());
      sf.bitrate = yj::get(f, "bitrate").i64();
      sf.average_bitrate = yj::get(f, "averageBitrate").i64();
      sf.content_length = yj::get(f, "contentLength").i64();
      sf.audio_channels = static_cast<int>(yj::get(f, "audioChannels").i64());
      sf.audio_sample_rate =
          static_cast<int>(yj::get(f, "audioSampleRate").i64());
      sf.is_drc = yj::get(f, "isDrc").boolean();
      sf.quality = arena.intern(str(yj::get(f, "quality")));
      sf.quality_label = arena.intern(str(yj::get(f, "qualityLabel")));
      auto mime = str(yj::get(f, "mimeType"));
      sf.mime_type = arena.intern(mime);
      auto slash = mime.find('/'), semi = mime.find(';');
      if (slash != std::string::npos)
        sf.container = arena.intern(mime.substr(
            slash + 1,
            (semi == std::string::npos ? mime.size() : semi) - slash - 1));
      auto start = mime.find("codecs=\"");
      if (start != std::string::npos) {
        auto end = mime.find('"', start + 8);
        auto codecs = mime.substr(start + 8, end - start - 8);
        size_t offset = 0;
        while (offset < codecs.size()) {
          auto comma = codecs.find(',', offset);
          auto codec = codecs.substr(offset, comma == std::string::npos
                                                 ? std::string::npos
                                                 : comma - offset);
          while (!codec.empty() && codec.front() == ' ')
            codec.erase(0, 1);
          if (codec.rfind("avc", 0) == 0 || codec.rfind("av01", 0) == 0 ||
              codec.rfind("vp", 0) == 0 || codec.rfind("hev", 0) == 0 ||
              codec.rfind("hvc", 0) == 0) {
            sf.has_video = true;
            sf.video_codec = arena.intern(codec);
          } else {
            sf.has_audio = true;
            sf.audio_codec = arena.intern(codec);
          }
          if (comma == std::string::npos)
            break;
          offset = comma + 1;
        }
      }
      if (mime.rfind("audio/", 0) == 0)
        sf.has_audio = true;
      if (mime.rfind("video/", 0) == 0)
        sf.has_video = true;
      if (auto itag = ytfast::itag_lookup(sf.itag)) {
        if (sf.video_codec.empty() && itag->has_video())
          sf.video_codec = itag->vcodec;
        if (sf.audio_codec.empty() && itag->has_audio())
          sf.audio_codec = itag->acodec;
        if (itag->has_audio())
          sf.has_audio = true;
      }
      auto audio = yj::get(f, "audioTrack");
      auto track_id = str(yj::get(audio, "id"));
      sf.language = track_id.substr(0, track_id.find('.'));
      sf.format_note = str(yj::get(audio, "displayName"));
      if (!track_id.empty())
        sf.format_id += "-" + track_id;
      if (sf.is_drc)
        sf.format_id += "-drc";
      info.formats.push_back(std::move(sf));
      return true;
    });
  info.sabr_only = info.formats.empty() && missing > 0;
}
} // namespace ytwe

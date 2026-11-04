#pragma once
#include "pugixml.hpp"
#include "we_formats.h"
#include <cmath>
#include <map>
#include <sstream>
namespace ytwe {
inline std::string join_url(const std::string &base,
                            const std::string &relative) {
  CURLU *u = curl_url();
  if (!u)
    throw std::runtime_error("URL allocation failed");
  std::string result;
  char *out = nullptr;
  if (curl_url_set(u, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
      curl_url_set(u, CURLUPART_URL, relative.c_str(), 0) == CURLUE_OK &&
      curl_url_get(u, CURLUPART_URL, &out, 0) == CURLUE_OK)
    result = out;
  curl_free(out);
  curl_url_cleanup(u);
  if (result.empty())
    throw std::runtime_error("invalid manifest URL");
  return result;
}
inline std::map<std::string, std::string>
hls_attributes(std::string_view text) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ',' || text[i] == ' '))
      ++i;
    auto equal = text.find('=', i);
    if (equal == std::string_view::npos)
      break;
    std::string key(text.substr(i, equal - i));
    i = equal + 1;
    std::string value;
    if (i < text.size() && text[i] == '"') {
      auto end = text.find('"', ++i);
      if (end == std::string_view::npos)
        break;
      value = text.substr(i, end - i);
      i = end + 1;
    } else {
      auto end = text.find(',', i);
      value = text.substr(i, end == std::string_view::npos ? text.size() - i
                                                           : end - i);
      i = end == std::string_view::npos ? text.size() : end + 1;
    }
    out[key] = value;
  }
  return out;
}
inline void parse_hls(const std::string &body, const std::string &url,
                      ytfast::VideoInfo &info, const std::string &source,
                      const std::string &ua) {
  if (body.rfind("#EXTM3U", 0) != 0)
    throw std::runtime_error("invalid HLS manifest");
  auto &arena = info.ensure_arena();
  std::istringstream lines(body);
  std::string line;
  std::map<std::string, std::string> pending;
  size_t index = 0;
  bool master = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("#EXT-X-MEDIA:", 0) == 0) {
      auto a = hls_attributes(std::string_view(line).substr(13));
      if (a["TYPE"] != "AUDIO" || a["URI"].empty())
        continue;
      master = true;
      ytfast::StreamFormat f;
      f.format_id = "hls-audio-" + std::to_string(++index) + "-" + source;
      f.url = join_url(url, a["URI"]);
      f.protocol = "m3u8_native";
      f.language = a["LANGUAGE"];
      f.format_note = a["NAME"];
      f.has_audio = true;
      f.audio_codec = arena.intern("mp4a.40.2");
      f.container = arena.intern("m4a");
      f.source_client = source;
      f.user_agent = ua;
      info.formats.push_back(std::move(f));
    } else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
      pending = hls_attributes(std::string_view(line).substr(18));
      master = true;
    } else if (!line.empty() && line[0] != '#' && !pending.empty()) {
      ytfast::StreamFormat f;
      auto x = pending["RESOLUTION"].find('x');
      if (x != std::string::npos) {
        f.width = std::atoi(pending["RESOLUTION"].c_str());
        f.height = std::atoi(pending["RESOLUTION"].c_str() + x + 1);
      }
      f.fps = static_cast<int>(std::atof(pending["FRAME-RATE"].c_str()));
      f.bitrate = std::atoll(pending["AVERAGE-BANDWIDTH"].c_str());
      if (!f.bitrate)
        f.bitrate = std::atoll(pending["BANDWIDTH"].c_str());
      f.url = join_url(url, line);
      f.protocol = "m3u8_native";
      f.format_id = "hls-" + std::to_string(f.height) + "-" +
                    std::to_string(++index) + "-" + source;
      f.has_video = true;
      f.has_audio = pending["AUDIO"].empty();
      auto codecs = pending["CODECS"];
      auto comma = codecs.find(',');
      f.video_codec = arena.intern(codecs.substr(0, comma));
      if (f.has_audio && comma != std::string::npos)
        f.audio_codec = arena.intern(codecs.substr(comma + 1));
      f.container = arena.intern("mp4");
      f.source_client = source;
      f.user_agent = ua;
      info.formats.push_back(std::move(f));
      pending.clear();
    }
  }
  if (!master) {
    ytfast::StreamFormat f;
    f.url = url;
    f.protocol = "m3u8_native";
    f.format_id = "hls-" + source;
    f.has_audio = f.has_video = true;
    f.container = arena.intern("mp4");
    f.source_client = source;
    f.user_agent = ua;
    info.formats.push_back(std::move(f));
  }
}
inline std::string local_name(const char *n) {
  std::string name = n;
  auto p = name.find(':');
  return p == std::string::npos ? name : name.substr(p + 1);
}
inline pugi::xml_node child(pugi::xml_node node, const char *name) {
  for (auto c : node.children())
    if (local_name(c.name()) == name)
      return c;
  return {};
}
inline double iso_duration(const std::string &s) {
  std::smatch match;
  std::regex pattern("([0-9]+(?:\\.[0-9]+)?)([DHMS])");
  double result = 0;
  bool time = false;
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] == 'T')
      time = true;
  (void)time;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), pattern);
       it != std::sregex_iterator(); ++it) {
    double v = std::stod((*it)[1]);
    char unit = (*it)[2].str()[0];
    result += v * (unit == 'D'   ? 86400
                   : unit == 'H' ? 3600
                   : unit == 'M' ? 60
                                 : 1);
  }
  return result;
}
inline std::string dash_substitute(std::string pattern, const std::string &id,
                                   int64_t number, int64_t time,
                                   int64_t bandwidth) {
  for (size_t pos = 0; (pos = pattern.find('$', pos)) != std::string::npos;) {
    if (pos + 1 < pattern.size() && pattern[pos + 1] == '$') {
      pattern.replace(pos, 2, "$");
      ++pos;
      continue;
    }
    auto end = pattern.find('$', pos + 1);
    if (end == std::string::npos)
      break;
    auto key = pattern.substr(pos + 1, end - pos - 1);
    std::string value;
    int width = 0;
    auto percent = key.find('%');
    if (percent != std::string::npos) {
      width = std::atoi(key.c_str() + percent + 1);
      key.resize(percent);
    }
    if (key == "RepresentationID")
      value = id;
    else if (key == "Number")
      value = std::to_string(number);
    else if (key == "Time")
      value = std::to_string(time);
    else if (key == "Bandwidth")
      value = std::to_string(bandwidth);
    else {
      pos = end + 1;
      continue;
    }
    if (width > 0 && width < 32 && value.size() < static_cast<size_t>(width))
      value.insert(0, width - value.size(), '0');
    pattern.replace(pos, end - pos + 1, value);
    pos += value.size();
  }
  return pattern;
}
inline void parse_dash(const std::string &body, const std::string &url,
                       ytfast::VideoInfo &info, const std::string &source,
                       const std::string &ua) {
  pugi::xml_document doc;
  auto loaded = doc.load_buffer(body.data(), body.size());
  if (!loaded)
    throw std::runtime_error("invalid DASH manifest");
  auto mpd = child(doc, "MPD");
  if (!mpd)
    throw std::runtime_error("DASH MPD missing");
  auto base = url;
  if (auto b = child(mpd, "BaseURL"))
    base = join_url(base, b.text().as_string());
  double total_duration =
      iso_duration(mpd.attribute("mediaPresentationDuration").value());
  auto &arena = info.ensure_arena();
  for (auto period : mpd.children()) {
    if (local_name(period.name()) != "Period")
      continue;
    auto pb = base;
    if (auto b = child(period, "BaseURL"))
      pb = join_url(pb, b.text().as_string());
    double duration = iso_duration(period.attribute("duration").value());
    if (!duration)
      duration = total_duration;
    for (auto adaptation : period.children()) {
      if (local_name(adaptation.name()) != "AdaptationSet")
        continue;
      auto ab = pb;
      if (auto b = child(adaptation, "BaseURL"))
        ab = join_url(ab, b.text().as_string());
      for (auto rep : adaptation.children()) {
        if (local_name(rep.name()) != "Representation")
          continue;
        ytfast::StreamFormat f;
        auto id = std::string(rep.attribute("id").value());
        f.itag = std::atoi(id.c_str());
        f.format_id = "dash-" + id + "-" + source;
        f.source_client = source;
        f.user_agent = ua;
        f.language = adaptation.attribute("lang").value();
        f.bitrate = rep.attribute("bandwidth").as_llong();
        f.width = rep.attribute("width").as_int(
            adaptation.attribute("width").as_int());
        f.height = rep.attribute("height").as_int(
            adaptation.attribute("height").as_int());
        auto attr = [&](const char *name) {
          auto a = rep.attribute(name);
          return std::string(a ? a.value()
                               : adaptation.attribute(name).value());
        };
        auto mime = attr("mimeType"), codec = attr("codecs"),
             fps = attr("frameRate");
        f.has_video = mime.rfind("video", 0) == 0 || f.height > 0;
        f.has_audio = mime.rfind("audio", 0) == 0;
        f.mime_type = arena.intern(mime);
        f.container =
            arena.intern(mime.find("webm") != std::string::npos ? "webm"
                         : f.has_audio                          ? "m4a"
                                                                : "mp4");
        if (f.has_audio)
          f.audio_codec = arena.intern(codec);
        else
          f.video_codec = arena.intern(codec);
        auto div = fps.find('/');
        f.fps = static_cast<int>(
            std::atof(fps.c_str()) /
            (div == std::string::npos
                 ? 1
                 : std::max(1.0, std::atof(fps.c_str() + div + 1))));
        auto rb = ab;
        if (auto b = child(rep, "BaseURL")) {
          rb = join_url(rb, b.text().as_string());
          for (auto a : b.attributes())
            if (local_name(a.name()) == "contentLength")
              f.content_length = a.as_llong();
        }
        f.url = rb;
        auto segment_list = child(rep, "SegmentList");
        if (!segment_list)
          segment_list = child(adaptation, "SegmentList");
        if (segment_list) {
          auto init = child(segment_list, "Initialization");
          if (init)
            f.fragments.push_back(
                {init.attribute("sourceURL")
                     ? join_url(rb, init.attribute("sourceURL").value())
                     : rb,
                 init.attribute("range").value(), 0});
          for (auto segment : segment_list.children())
            if (local_name(segment.name()) == "SegmentURL")
              f.fragments.push_back(
                  {join_url(rb, segment.attribute("media").value()),
                   segment.attribute("mediaRange").value(), 0});
        }
        auto templ = child(rep, "SegmentTemplate");
        if (!templ)
          templ = child(adaptation, "SegmentTemplate");
        if (templ) {
          auto timescale =
              std::max<int64_t>(1, templ.attribute("timescale").as_llong(1));
          auto number = templ.attribute("startNumber").as_llong(1);
          auto init = std::string(templ.attribute("initialization").value()),
               media = std::string(templ.attribute("media").value());
          if (!init.empty())
            f.fragments.push_back(
                {join_url(rb, dash_substitute(init, id, number, 0, f.bitrate)),
                 "", 0});
          auto timeline = child(templ, "SegmentTimeline");
          int64_t t = 0;
          size_t generated = 0;
          auto add = [&](int64_t d) {
            if (++generated > 50000)
              throw std::runtime_error("DASH segment limit exceeded");
            f.fragments.push_back(
                {join_url(rb,
                          dash_substitute(media, id, number++, t, f.bitrate)),
                 "", static_cast<double>(d) / timescale});
            t += d;
          };
          if (timeline) {
            for (auto s : timeline.children()) {
              if (local_name(s.name()) != "S")
                continue;
              if (s.attribute("t"))
                t = s.attribute("t").as_llong();
              auto d = s.attribute("d").as_llong();
              if (d <= 0)
                throw std::runtime_error("invalid DASH timeline duration");
              auto repeat = s.attribute("r").as_llong();
              if (repeat < 0) {
                auto next = s.next_sibling("S");
                auto end = next && next.attribute("t")
                               ? next.attribute("t").as_llong()
                               : static_cast<int64_t>(duration * timescale);
                if (end <= t)
                  continue;
                repeat = (end - t + d - 1) / d - 1;
              }
              for (int64_t r = 0; r <= repeat; ++r)
                add(d);
            }
          } else if (auto d = templ.attribute("duration").as_llong();
                     d > 0 && duration > 0) {
            auto count =
                static_cast<int64_t>(std::ceil(duration * timescale / d));
            for (int64_t i = 0; i < count; ++i)
              add(d);
          }
        }
        if (!f.fragments.empty()) {
          f.protocol = "http_dash_segments";
          f.content_length = 0;
        } else if (rb == url || rb == base) {
          f.protocol = "dash";
          f.url = url;
        }
        if (templ || segment_list || f.protocol == "dash") {
          pugi::xml_document selected;
          auto root = selected.append_child(mpd.name());
          for (auto a : mpd.attributes())
            root.append_copy(a);
          auto p = root.append_child(period.name());
          for (auto a : period.attributes())
            p.append_copy(a);
          auto a = p.append_child(adaptation.name());
          for (auto attribute : adaptation.attributes())
            a.append_copy(attribute);
          for (auto node : adaptation.children())
            if (local_name(node.name()) != "Representation")
              a.append_copy(node);
          auto representation = a.append_copy(rep);
          auto base_node = child(representation, "BaseURL");
          if (!base_node)
            base_node = representation.append_child("BaseURL");
          base_node.text().set(rb.c_str());
          std::ostringstream xml;
          selected.save(xml);
          f.manifest_content = xml.str();
        }
        f.content_id = info.id;
        info.formats.push_back(std::move(f));
      }
    }
  }
}
inline Cache manifest_cache;
inline std::string playback_url(const ytfast::StreamFormat &format) {
  if (format.manifest_content.empty())
    return format.url;
  const auto key = "dash-playback:" + format.manifest_content;
  manifest_cache.put(key, format.manifest_content);
  auto path = manifest_cache.directory / digest(key);
  if (!std::filesystem::exists(path))
    throw std::runtime_error("cannot create selected DASH manifest");
  return path.u8string();
}
} // namespace ytwe

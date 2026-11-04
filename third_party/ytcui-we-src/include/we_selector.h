#pragma once
#include "yt_media.h"
#include "yt_select.h"
#include <tuple>
namespace ytwe {
inline std::vector<std::string> split_expression(const std::string &input,
                                                 char delimiter) {
  std::vector<std::string> out;
  int depth = 0;
  char quote_char = 0;
  size_t begin = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    auto c = input[i];
    if (quote_char) {
      if (c == quote_char && (i == 0 || input[i - 1] != '\\'))
        quote_char = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote_char = c;
      continue;
    }
    if (c == '[' || c == '(')
      ++depth;
    if (c == ']' || c == ')')
      --depth;
    if (c == delimiter && !depth) {
      out.push_back(input.substr(begin, i - begin));
      begin = i + 1;
    }
  }
  out.push_back(input.substr(begin));
  return out;
}
inline double numeric_value(std::string input) {
  double multiplier = 1;
  if (input.size() > 2 && input.substr(input.size() - 2) == "iB") {
    input.resize(input.size() - 2);
    multiplier = 1024;
  } else if (!input.empty() && input.back() == 'B')
    input.pop_back();
  if (!input.empty()) {
    char suffix = static_cast<char>(
        std::toupper(static_cast<unsigned char>(input.back())));
    auto p = std::string("KMGT").find(suffix);
    if (p != std::string::npos) {
      input.pop_back();
      multiplier =
          std::pow(multiplier == 1024 ? 1024 : 1000, static_cast<int>(p) + 1);
    }
  }
  size_t used = 0;
  auto value = std::stod(input, &used);
  if (used != input.size())
    throw std::runtime_error("invalid numeric format filter");
  return value * multiplier;
}
inline bool format_filter(const ytfast::StreamFormat &f,
                          const std::string &expression) {
  std::smatch m;
  if (!std::regex_match(expression, m,
                        std::regex("([A-Za-z_]+)(!\\^=|!\\$=|!\\*=|!=|<=|>=|\\^"
                                   "=|\\$=|\\*=|~=|=|<|>)([?]?)(.*)")))
    throw std::runtime_error("invalid format filter: " + expression);
  auto field = m[1].str(), op = m[2].str(), value = m[4].str();
  bool unknown_ok = !m[3].str().empty();
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')))
    value = value.substr(1, value.size() - 2);
  double number = -1;
  bool numeric = true;
  if (field == "height")
    number = f.height;
  else if (field == "width")
    number = f.width;
  else if (field == "fps")
    number = f.fps;
  else if (field == "filesize" || field == "filesize_approx")
    number = f.content_length;
  else if (field == "tbr" || field == "vbr" || field == "abr")
    number = f.effective_bitrate() / 1000.0;
  else if (field == "asr")
    number = f.audio_sample_rate;
  else if (field == "audio_channels")
    number = f.audio_channels;
  else
    numeric = false;
  if (numeric) {
    if (number <= 0)
      return unknown_ok;
    auto wanted = numeric_value(value);
    if (op == "<=")
      return number <= wanted;
    if (op == ">=")
      return number >= wanted;
    if (op == "<")
      return number < wanted;
    if (op == ">")
      return number > wanted;
    if (op == "=")
      return number == wanted;
    if (op == "!=")
      return number != wanted;
    throw std::runtime_error("invalid numeric comparison");
  }
  std::string actual;
  if (field == "ext" || field == "container")
    actual = f.container;
  else if (field == "vcodec")
    actual = f.has_video ? std::string(f.video_codec) : "none";
  else if (field == "acodec")
    actual = f.has_audio ? std::string(f.audio_codec) : "none";
  else if (field == "format_id")
    actual = f.format_id;
  else if (field == "language")
    actual = f.language;
  else if (field == "protocol")
    actual = f.protocol;
  else
    throw std::runtime_error("unsupported format field: " + field);
  if (actual.empty())
    return unknown_ok;
  if (op == "=")
    return actual == value;
  if (op == "!=")
    return actual != value;
  if (op == "^=")
    return actual.rfind(value, 0) == 0;
  if (op == "$=")
    return actual.size() >= value.size() &&
           actual.compare(actual.size() - value.size(), value.size(), value) ==
               0;
  if (op == "*=")
    return actual.find(value) != std::string::npos;
  if (op == "!^=")
    return actual.rfind(value, 0) != 0;
  if (op == "!$=")
    return !(
        actual.size() >= value.size() &&
        actual.compare(actual.size() - value.size(), value.size(), value) == 0);
  if (op == "!*=")
    return actual.find(value) == std::string::npos;
  if (op == "~=")
    return std::regex_search(actual, std::regex(value));
  throw std::runtime_error("unsupported string comparison");
}
inline ytfast::Selection
select_format(const std::vector<ytfast::StreamFormat> &formats,
              const std::string &expression, ytfast::Mode mode,
              const ytfast::Quality &quality) {
  if (expression.empty())
    return ytfast::Selector::select(formats, mode, quality);
  for (auto &alternative : split_expression(expression, '/')) {
    auto merge = split_expression(alternative, '+');
    ytfast::Selection result;
    result.mode = mode;
    bool valid = true;
    for (auto &spec : merge) {
      auto bracket = spec.find('[');
      auto base = spec.substr(0, bracket);
      std::vector<ytfast::StreamFormat> candidates;
      std::vector<const ytfast::StreamFormat *> originals;
      for (auto &f : formats) {
        bool match = true;
        size_t cursor = bracket;
        while (cursor != std::string::npos) {
          auto end = spec.find(']', cursor);
          if (end == std::string::npos)
            throw std::runtime_error("unterminated format filter");
          if (!format_filter(f, spec.substr(cursor + 1, end - cursor - 1))) {
            match = false;
            break;
          }
          cursor = spec.find('[', end + 1);
        }
        if (!match)
          continue;
        bool video = base == "bestvideo" || base == "bv" ||
                     base == "worstvideo" || base == "wv",
             audio = base == "bestaudio" || base == "ba" ||
                     base == "worstaudio" || base == "wa";
        bool muxed =
            base == "best" || base == "b" || base == "worst" || base == "w";
        bool anyvideo = base == "bestvideo*" || base == "bv*" ||
                        base == "worstvideo*" || base == "wv*";
        bool any =
            base == "best*" || base == "b*" || base == "worst*" || base.empty();
        if (video && !f.is_video_only())
          continue;
        if (audio && !f.is_audio_only())
          continue;
        if (muxed && !f.is_muxed())
          continue;
        if (anyvideo && !f.has_video)
          continue;
        if (!video && !audio && !muxed && !anyvideo && !any &&
            base != f.format_id && base != std::to_string(f.itag))
          continue;
        candidates.push_back(f);
        originals.push_back(&f);
      }
      if (candidates.empty()) {
        valid = false;
        break;
      }
      auto q = quality;
      q.smallest = base.rfind("worst", 0) == 0 || base == "w" || base == "wv" ||
                   base == "wa";
      auto pick_mode = (base == "bestaudio" || base == "ba" ||
                        base == "worstaudio" || base == "wa")
                           ? ytfast::Mode::AudioOnly
                           : ytfast::Mode::VideoOnly;
      auto picked = ytfast::Selector::select(candidates, pick_mode, q);
      const auto *item = picked.video ? picked.video : picked.audio;
      if (!item && candidates[0].is_audio_only())
        item = &candidates[0];
      if (!item) {
        valid = false;
        break;
      }
      auto chosen = originals[static_cast<size_t>(item - candidates.data())];
      if (chosen->has_video) {
        result.video = chosen;
        result.muxed = chosen->is_muxed();
      } else
        result.audio = chosen;
    }
    if (valid) {
      if (result.video && !result.audio && !result.muxed)
        result.mode = ytfast::Mode::VideoOnly;
      if (!result.video && result.audio)
        result.mode = ytfast::Mode::AudioOnly;
    }
    if (valid && result.ok()) {
      if (result.muxed)
        result.audio = nullptr;
      return result;
    }
  }
  throw std::runtime_error("requested format is not available: " + expression);
}
inline std::string safe_filename(std::string s) {
  for (char &c : s)
    if (static_cast<unsigned char>(c) < 32 ||
        std::string("<>:\"/\\|?*").find(c) != std::string::npos)
      c = '_';
  while (!s.empty() && (s.back() == '.' || s.back() == ' '))
    s.pop_back();
  if (s.empty())
    s = "untitled";
  if (s.size() > 180) {
    size_t end = 180;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80)
      --end;
    s.resize(end);
  }
  auto stem = s.substr(0, s.find('.'));
  std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      std::regex_match(stem, std::regex("(?:COM|LPT)[1-9]")))
    s.insert(0, "_");
  return s;
}
inline std::string
output_template(const std::string &pattern, const ytfast::VideoInfo &info,
                const ytfast::StreamFormat &f, const std::string &ext,
                size_t playlist_index = 0, const std::string &playlist = "",
                bool sanitize = true) {
  std::map<std::string, std::string> fields{
      {"id", info.id},
      {"title", info.title},
      {"fulltitle", info.title},
      {"channel", info.channel},
      {"uploader", info.channel},
      {"channel_id", info.channel_id},
      {"ext", ext},
      {"format_id", f.format_id},
      {"height", std::to_string(f.height)},
      {"width", std::to_string(f.width)},
      {"resolution",
       f.height ? std::to_string(f.width) + "x" + std::to_string(f.height)
                : "audio only"},
      {"playlist_index", std::to_string(playlist_index)},
      {"playlist", playlist},
      {"playlist_id", playlist},
      {"duration", std::to_string(info.duration_secs)},
      {"upload_date", info.upload_date}};
  std::string result;
  for (size_t i = 0; i < pattern.size();) {
    if (pattern.compare(i, 2, "%%") == 0) {
      result += '%';
      i += 2;
      continue;
    }
    if (pattern.compare(i, 2, "%(") != 0) {
      result += pattern[i++];
      continue;
    }
    auto close = pattern.find(')', i + 2);
    if (close == std::string::npos)
      throw std::runtime_error("invalid output template");
    auto key = pattern.substr(i + 2, close - i - 2);
    size_t end = close + 1;
    while (end < pattern.size() &&
           (std::isdigit(static_cast<unsigned char>(pattern[end])) ||
            pattern[end] == '.'))
      ++end;
    if (end >= pattern.size() || (pattern[end] != 's' && pattern[end] != 'd'))
      throw std::runtime_error("unsupported output template conversion");
    auto it = fields.find(key);
    std::string value = it == fields.end() ? "NA" : it->second;
    auto width = std::atoi(pattern.c_str() + close + 1);
    if (pattern[end] == 'd' && width > 0 && width < 32 &&
        value.size() < static_cast<size_t>(width))
      value.insert(0, width - value.size(), '0');
    result += sanitize ? safe_filename(value) : value;
    i = end + 1;
  }
  return result;
}
} // namespace ytwe

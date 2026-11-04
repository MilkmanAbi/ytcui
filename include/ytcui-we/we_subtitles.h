#pragma once
#include "yt_media.h"
namespace ytwe {
inline std::string caption_translation_context(const std::string &player,
                                               const std::string &initial) {
  auto translations =
      yj::path(yj::parse(initial), "captions",
               "playerCaptionsTracklistRenderer", "translationLanguages");
  if (!translations.is_arr())
    return player;
  auto captions = raw(yj::get(yj::parse(player), "captions"));
  auto tracks =
      raw(yj::get(yj::parse(captions), "playerCaptionsTracklistRenderer"));
  if (tracks == "null")
    return player;
  tracks = json_set(tracks, "translationLanguages", raw(translations));
  return json_set(
      player, "captions",
      json_set(captions, "playerCaptionsTracklistRenderer", tracks));
}
struct SubtitleTrack {
  std::string language, name, ext, url;
  bool automatic = false, translated = false;
};
inline std::vector<SubtitleTrack> subtitle_tracks(const std::string &player,
                                                  bool translations = true) {
  auto captions = yj::path(yj::parse(player), "captions",
                           "playerCaptionsTracklistRenderer");
  std::vector<std::pair<std::string, std::string>> languages;
  yj::each_elem(yj::get(captions, "translationLanguages"), [&](yj::Val l) {
    languages.emplace_back(str(yj::get(l, "languageCode")),
                           text(yj::get(l, "languageName")));
    return true;
  });
  std::vector<SubtitleTrack> out;
  std::set<std::string> seen;
  yj::each_elem(yj::get(captions, "captionTracks"), [&](yj::Val track) {
    auto language = str(yj::get(track, "languageCode")),
         name = text(yj::get(track, "name")),
         base = remove_query(str(yj::get(track, "baseUrl")), "xosf");
    bool automatic = str(yj::get(track, "kind")) == "asr";
    for (auto ext : {"vtt", "ttml", "srv3", "srv2", "srv1", "json3"}) {
      out.push_back(
          {language, name, ext, set_query(base, "fmt", ext), automatic, false});
      if (translations && yj::get(track, "isTranslatable").boolean())
        for (auto &l : languages) {
          if (l.first == language)
            continue;
          auto key = l.first + ":" + ext;
          if (!seen.insert(key).second)
            continue;
          out.push_back(
              {l.first, l.second + " (translated from " + language + ")", ext,
               set_query(set_query(base, "fmt", ext), "tlang", l.first), true,
               true});
        }
    }
    return true;
  });
  return out;
}
inline bool language_matches(const std::string &language,
                             const std::string &patterns) {
  bool positive = false, selected = false;
  std::istringstream list(patterns);
  std::string part;
  while (std::getline(list, part, ',')) {
    bool negative = !part.empty() && part[0] == '-';
    if (negative)
      part.erase(0, 1);
    else
      positive = true;
    bool match = part == "all";
    try {
      match = match || std::regex_match(language, std::regex(part));
    } catch (const std::regex_error &) {
      throw std::runtime_error("invalid subtitle language expression");
    }
    if (match) {
      if (negative)
        return false;
      selected = true;
    }
  }
  return selected || !positive;
}
inline void save_subtitle(const SubtitleTrack &track,
                          const std::filesystem::path &target,
                          const std::string &ua, const std::string &pot = "",
                          bool overwrite = false) {
  if (std::filesystem::exists(target) && !overwrite)
    return;
  Http h;
  auto url = pot.empty() ? track.url : set_query(track.url, "pot", pot);
  if (!pot.empty())
    url = set_query(url, "potc", "1");
  auto r = h.get(url, {"User-Agent: " + ua});
  if (r.status != 200 || r.body.empty())
    throw std::runtime_error("subtitle request failed (HTTP " +
                             std::to_string(r.status) + ")");
  if (!target.parent_path().empty())
    std::filesystem::create_directories(target.parent_path());
  auto part = target;
  part += ".part";
  {
    std::ofstream file(part, std::ios::binary);
    file.write(r.body.data(), r.body.size());
    if (!file)
      throw std::runtime_error("subtitle output write failed");
  }
#ifdef _WIN32
  if (!MoveFileExW(part.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("cannot finalize subtitle file");
#else
  std::filesystem::rename(part, target);
#endif
}
} // namespace ytwe

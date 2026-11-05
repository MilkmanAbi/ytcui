#pragma once
#include "we_manifests.h"
#include "yt_youtube.h"
namespace ytwe {
struct Entry {
  std::string type = "video", id, url, title, channel, description, thumbnail;
  int64_t duration = 0, views = -1;
  void write(yjw::Writer &w) const {
    w.obj()
        .kv("_type", "url")
        .kv("ie_key", type == "video" ? "Youtube" : "YoutubeTab")
        .kv("entry_type", type)
        .kv("id", id)
        .kv("url", url)
        .kv("title", title)
        .kv("channel", channel)
        .kv("description", description)
        .kv("thumbnail", thumbnail)
        .kv("duration", duration);
    if (views >= 0)
      w.kv("view_count", views);
    w.end();
  }
};
inline int64_t duration_text(const std::string &input) {
  if (input.find(':') == std::string::npos)
    return 0;
  int64_t total = 0, part = 0;
  for (char c : input) {
    if (c == ':') {
      total = total * 60 + part;
      part = 0;
    } else if (c >= '0' && c <= '9')
      part = part * 10 + c - '0';
  }
  return total * 60 + part;
}
inline Entry parse_entry(std::string_view kind, yj::Val r) {
  Entry e;
  if (kind == "lockupViewModel") {
    auto type = str(yj::get(r, "contentType"));
    e.id = str(yj::get(r, "contentId"));
    if (type == "LOCKUP_CONTENT_TYPE_PLAYLIST" ||
        type == "LOCKUP_CONTENT_TYPE_PODCAST")
      e.type = "playlist";
    else if (type != "LOCKUP_CONTENT_TYPE_VIDEO")
      return {};
    auto md = yj::path(r, "metadata", "lockupMetadataViewModel");
    e.title = text(yj::get(md, "title"));
    const std::string_view parts[] = {"metadataParts"};
    yj::find_any(md, parts, 1, [&](std::string_view, yj::Val row) {
      yj::each_elem(row, [&](yj::Val p) {
        auto txt = text(yj::get(p, "text"));
        if (first(p, "browseEndpoint") && e.channel.empty())
          e.channel = txt;
        else if (txt.find("view") != std::string::npos)
          e.views = count_text(txt);
        return true;
      });
      return true;
    });
    auto badge = first(yj::get(r, "contentImage"), "thumbnailBadgeViewModel");
    e.duration = duration_text(str(yj::get(badge, "text")));
    auto img = first(yj::get(r, "contentImage"), "sources");
    yj::each_elem(img, [&](yj::Val t) {
      e.thumbnail = str(yj::get(t, "url"));
      return true;
    });
  } else if (kind == "shortsLockupViewModel") {
    e.id = str(yj::path(r, "onTap", "innertubeCommand", "reelWatchEndpoint",
                        "videoId"));
    if (e.id.empty())
      e.id = str(yj::get(first(r, "reelWatchEndpoint"), "videoId"));
    e.title = text(yj::path(r, "overlayMetadata", "primaryText"));
    e.views = count_text(text(yj::path(r, "overlayMetadata", "secondaryText")));
    e.url = "https://www.youtube.com/shorts/" + e.id;
  } else if (kind == "backstagePostRenderer" || kind == "postViewModel" ||
             kind == "backstagePostViewModel") {
    e.type = "post";
    e.id = str(yj::get(r, "postId"));
    e.description = text(yj::get(r, "contentText"));
    if (e.description.empty())
      e.description = text(yj::get(r, "content"));
    e.title = e.description;
    e.channel = text(yj::get(r, "authorText"));
  } else if (kind == "notificationRenderer") {
    e.id = str(yj::path(r, "navigationEndpoint", "watchEndpoint", "videoId"));
    e.title = text(yj::get(r, "shortMessage"));
    if (e.id.empty()) {
      auto path = str(yj::path(r, "navigationEndpoint", "browseEndpoint",
                               "canonicalBaseUrl"));
      auto p = path.find("/post/");
      if (p != std::string::npos) {
        e.type = "post";
        e.id = path.substr(p + 6);
        e.url = "https://www.youtube.com" + path;
      }
    }
  } else {
    e.id = str(yj::get(r, "videoId"));
    e.title = text(yj::get(r, "title"));
    e.channel = text(yj::get(r, "longBylineText"));
    if (e.channel.empty())
      e.channel = text(yj::get(r, "shortBylineText"));
    if (e.channel.empty())
      e.channel = text(yj::get(r, "ownerText"));
    e.duration = yj::get(r, "lengthSeconds").i64();
    if (!e.duration)
      e.duration = duration_text(text(yj::get(r, "lengthText")));
    e.views = count_text(text(yj::get(r, "viewCountText")));
    e.description = text(yj::get(r, "descriptionSnippet"));
    if (kind == "playlistRenderer" || kind == "gridPlaylistRenderer") {
      e.type = "playlist";
      e.id = str(yj::get(r, "playlistId"));
    }
    if (kind == "channelRenderer" || kind == "gridChannelRenderer") {
      e.type = "channel";
      e.id = str(yj::get(r, "channelId"));
    }
    yj::each_elem(yj::path(r, "thumbnail", "thumbnails"), [&](yj::Val t) {
      e.thumbnail = str(yj::get(t, "url"));
      return true;
    });
  }
  if (e.url.empty()) {
    if (e.type == "video")
      e.url = "https://www.youtube.com/watch?v=" + e.id;
    else if (e.type == "playlist")
      e.url = "https://www.youtube.com/playlist?list=" + e.id;
    else if (e.type == "channel")
      e.url = "https://www.youtube.com/channel/" + e.id;
    else
      e.url = "https://www.youtube.com/post/" + e.id;
  }
  return e;
}
struct CollectionResult {
  size_t count = 0, pages = 0;
  bool exhausted = false;
  std::string title;
};
inline yj::Val collection_contents(yj::Val root) {
  auto contents = yj::get(root, "contents");
  if (!contents)
    return root;
  yj::Val selected;
  const std::string_view tabs[] = {"tabRenderer", "expandableTabRenderer"};
  yj::find_any(contents, tabs, 2, [&](std::string_view, yj::Val tab) {
    if (yj::get(tab, "selected").boolean()) {
      selected = yj::get(tab, "content");
      return false;
    }
    return true;
  });
  return selected ? selected : contents;
}
inline CollectionResult
collection(Youtube &yt, const std::string &kind, const std::string &input,
           size_t limit, size_t max_pages,
           const std::function<void(const Entry &)> &sink) {
  CollectionResult result;
  std::string response, endpoint = "browse", next;
  std::set<std::string> tokens, seen;
  if (kind == "search" || kind == "search-date") {
    yt.bootstrap_url("https://www.youtube.com/results?search_query=" +
                         Http::url_encode(input) +
                         (kind == "search-date" ? "&sp=CAI%3D" : ""),
                     "");
    response = yt.initial_json();
    endpoint = "search";
  } else if (kind == "notifications") {
    if (!yt.authenticated())
      throw std::runtime_error("notifications require --cookies");
    yt.bootstrap_url("https://www.youtube.com", "");
    response = yt.api("notification/get_notification_menu", "{}");
    endpoint = "notification/get_notification_menu";
  } else if (kind == "mix") {
    yt.bootstrap_playlist(input);
    auto seed = input.size() >= 13 ? input.substr(input.size() - 11) : "";
    response = yt.api("next", yjw::Writer(false)
                                  .obj()
                                  .kv("playlistId", input)
                                  .kv("videoId", seed)
                                  .end()
                                  .str());
    endpoint = "next";
  } else {
    std::string url = input;
    if (kind == "feed")
      url = "https://www.youtube.com/feed/" + input;
    else if (input.rfind("http", 0) != 0)
      url = std::string("https://www.youtube.com/") +
            (input.rfind("UC", 0) == 0 ? "channel/" : "") + input;
    if (kind == "channel" && url.find("/videos") == std::string::npos &&
        url.find("/shorts") == std::string::npos &&
        url.find("/streams") == std::string::npos &&
        url.find("/playlists") == std::string::npos)
      url += "/videos";
    if (kind == "community" && url.find("/posts") == std::string::npos &&
        url.find("/community") == std::string::npos)
      url += "/posts";
    yt.bootstrap_url(url, "");
    response = yt.initial_json();
  }
  const std::string_view markers[] = {"videoRenderer",
                                      "compactVideoRenderer",
                                      "gridVideoRenderer",
                                      "playlistVideoRenderer",
                                      "playlistPanelVideoRenderer",
                                      "lockupViewModel",
                                      "shortsLockupViewModel",
                                      "reelItemRenderer",
                                      "playlistRenderer",
                                      "gridPlaylistRenderer",
                                      "channelRenderer",
                                      "gridChannelRenderer",
                                      "backstagePostRenderer",
                                      "postViewModel",
                                      "backstagePostViewModel",
                                      "notificationRenderer",
                                      "continuationItemRenderer",
                                      "continuationItemViewModel"};
  while (!response.empty() && result.count < limit &&
         result.pages < max_pages) {
    auto root = yj::parse(response);
    ++result.pages;
    next.clear();
    auto contents = kind == "mix" ? first(root, "playlistPanelRenderer")
                                  : collection_contents(root);
    if (result.title.empty())
      result.title =
          str(yj::path(root, "metadata", "channelMetadataRenderer", "title"));
    size_t added = 0;
    std::string mix_endpoint;
    yj::find_any(
        contents, markers, sizeof(markers) / sizeof(markers[0]),
        [&](std::string_view tag, yj::Val r) {
          if (tag == "continuationItemRenderer" ||
              tag == "continuationItemViewModel") {
            if (kind == "notifications")
              next = str(yj::path(r, "continuationEndpoint",
                                  "getNotificationMenuEndpoint", "ctoken"));
            else
              next = continuation(r);
            return true;
          }
          auto e = parse_entry(tag, r);
          if (e.id.empty())
            return true;
          auto key = e.type + ":" + e.id;
          if (!seen.insert(key).second)
            return true;
          if (kind == "mix")
            mix_endpoint =
                raw(yj::path(r, "navigationEndpoint", "watchEndpoint"));
          sink(e);
          ++result.count;
          ++added;
          return result.count < limit;
        });
    if (kind == "mix" && next.empty() && added && mix_endpoint != "null") {
      auto key = digest(mix_endpoint);
      if (!tokens.insert(key).second)
        break;
      response = yt.api("next", mix_endpoint);
      continue;
    }
    if (next.empty()) {
      result.exhausted = true;
      break;
    }
    if (!tokens.insert(next).second)
      throw std::runtime_error("collection continuation loop detected");
    if (result.count >= limit || result.pages >= max_pages)
      break;
    response = yt.api(
        endpoint,
        yjw::Writer(false)
            .obj()
            .kv(kind == "notifications" ? "ctoken" : "continuation", next)
            .end()
            .str());
  }
  if (result.count >= limit)
    result.exhausted = false;
  return result;
}
inline CollectionResult
live_chat(Youtube &yt, const std::string &video, size_t limit, size_t max_pages,
          const std::function<void(const std::string &)> &sink) {
  yt.bootstrap(video);
  auto initial = yj::parse(yt.initial_json());
  auto renderer = first(initial, "liveChatRenderer");
  bool replay = yj::get(renderer, "isReplay").boolean();
  auto token =
      str(yj::get(first(renderer, "reloadContinuationData"), "continuation"));
  if (token.empty())
    throw std::runtime_error("live chat is unavailable");
  CollectionResult result;
  std::set<std::string> seen;
  std::string previous;
  int empty_pages = 0;
  while (!token.empty() && result.count < limit && result.pages < max_pages) {
    auto body = yt.api(
        replay ? "live_chat/get_live_chat_replay" : "live_chat/get_live_chat",
        yjw::Writer(false).obj().kv("continuation", token).end().str());
    ++result.pages;
    auto root = yj::parse(body);
    auto chat = yj::path(root, "continuationContents", "liveChatContinuation");
    size_t added = 0;
    yj::each_elem(yj::get(chat, "actions"), [&](yj::Val action) {
      auto identity = digest(raw(action));
      if (seen.insert(identity).second) {
        sink(raw(action));
        ++result.count;
        ++added;
      }
      return result.count < limit;
    });
    auto next = first(chat, replay ? "liveChatReplayContinuationData"
                                   : "invalidationContinuationData");
    if (!next && !replay)
      next = first(chat, "timedContinuationData");
    previous = token;
    token = str(yj::get(next, "continuation"));
    if (added)
      empty_pages = 0;
    else
      ++empty_pages;
    if (replay && previous == token && !added)
      break;
    if (empty_pages >= 10)
      break;
    auto delay =
        std::clamp<int64_t>(yj::get(next, "timeoutMs").i64(), 0, 10000);
    if (delay && result.count < limit)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  result.exhausted = token.empty();
  return result;
}
} // namespace ytwe

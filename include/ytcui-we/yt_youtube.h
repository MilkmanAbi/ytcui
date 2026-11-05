#pragma once
// Native YouTube metadata/comment/playlist extraction. Algorithm reference:
// yt-dlp b77c9304e1c4220cca450c583491904daf8455af,
// extractor/youtube/{_video,_tab}.py. Network and JSON scanning use this
// repository's existing C++ implementation.
#include "we_cache.h"
#include "we_clients.h"
#include "we_http.h"
#include "we_time.h"
#include "yj.h"
#include "yjw.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <openssl/evp.h>
#include <regex>
#include <set>
#include <thread>
#include <unordered_map>

namespace ytwe {
inline std::string str(yj::Val v) { return yj::unescape(v.raw()); }
inline std::string raw(yj::Val v) {
  return v ? std::string(v.p, yj::skip_value(v.p, v.e)) : "null";
}
inline std::string json_set(const std::string &body, const std::string &key,
                            const std::string &value) {
  std::string out = "{";
  yj::each_member(yj::parse(body), [&](std::string_view k, yj::Val v) {
    if (k != key)
      out += yjw::Writer(false).val(k).str() + ":" + raw(v) + ",";
    return true;
  });
  return out + yjw::Writer(false).val(key).str() + ":" + value + "}";
}
inline std::string text(yj::Val v) {
  if (v.is_str())
    return str(v);
  auto s = yj::get(v, "simpleText");
  if (s.is_str())
    return str(s);
  s = yj::get(v, "content");
  if (s.is_str())
    return str(s);
  std::string out;
  yj::each_elem(yj::get(v, "runs"), [&](yj::Val r) {
    out += str(yj::get(r, "text"));
    return true;
  });
  return out;
}
inline yj::Val first(yj::Val v, std::string_view key) {
  yj::Val out;
  const std::string_view keys[] = {key};
  yj::find_any(v, keys, 1, [&](std::string_view, yj::Val x) {
    out = x;
    return false;
  });
  return out;
}
// Unlike regex matching, JSON structural scanning handles braces inside
// strings.
inline std::string embedded_json(const std::string &html,
                                 std::string_view marker) {
  size_t pos = 0;
  while ((pos = html.find(marker, pos)) != std::string::npos) {
    pos += marker.size();
    const char *p = html.data() + pos;
    const char *e = html.data() + html.size();
    p = yj::skip_ws(p, e);
    if (p < e && (*p == '=' || *p == ':'))
      p = yj::skip_ws(p + 1, e);
    if (p < e && *p == '{') {
      const char *stop = yj::skip_value(p, e);
      if (stop > p && stop[-1] == '}')
        return std::string(p, stop);
    }
  }
  return {};
}
inline std::string base64(std::string_view bytes) {
  static const char *abc =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned v = 0;
  int bits = -6;
  for (unsigned char c : bytes) {
    v = (v << 8) | c;
    bits += 8;
    while (bits >= 0) {
      out += abc[(v >> bits) & 63];
      bits -= 6;
    }
  }
  if (bits > -6)
    out += abc[((v << 8) >> (bits + 8)) & 63];
  while (out.size() % 4)
    out += '=';
  return out;
}
inline std::string comment_continuation(const std::string &id) {
  if (id.size() != 11)
    throw std::runtime_error("expected an 11-character YouTube video id");
  std::string s("\x12\x0d\x12\x0b", 4);
  s += id;
  s.append("\x18\x06\x32\x27\x22\x11\x22\x0b", 8);
  s += id;
  s.append("\x30\x00\x78\x02\x30\x00\x42\x10", 8);
  s += "comments-section";
  return base64(s);
}
inline std::string continuation(yj::Val v) {
  auto cmd = first(v, "continuationCommand");
  auto tok = yj::get(cmd, "token");
  if (tok.is_str())
    return str(tok);
  tok = yj::get(first(cmd, "continuationCommand"), "token");
  if (tok.is_str())
    return str(tok);
  tok = yj::get(first(v, "nextContinuationData"), "continuation");
  return str(tok);
}
inline int64_t count_text(std::string s) {
  // YouTube a11y strings commonly contain a full comma-separated count.
  size_t pos = s.find_first_of("0123456789");
  if (pos == std::string::npos)
    return -1;
  std::string n;
  while (pos < s.size() && (std::isdigit((unsigned char)s[pos]) ||
                            s[pos] == ',' || s[pos] == '.')) {
    if (s[pos] != ',')
      n += s[pos];
    ++pos;
  }
  double value = std::stod(n);
  if (pos < s.size())
    switch (s[pos]) {
    case 'K':
    case 'k':
      value *= 1000;
      break;
    case 'M':
    case 'm':
      value *= 1000000;
      break;
    case 'B':
    case 'b':
      value *= 1000000000;
      break;
    }
  return static_cast<int64_t>(value);
}
struct Comment {
  std::string id, parent = "root", body, author, author_id, author_url,
                  thumbnail, time_text;
  int64_t likes = -1;
  bool uploader = false, verified = false, pinned = false, hearted = false;
  void write(yjw::Writer &w) const {
    w.obj()
        .kv("id", id)
        .kv("parent", parent)
        .kv("text", body)
        .kv("author", author)
        .kv("author_id", author_id)
        .kv("author_url", author_url)
        .kv("author_thumbnail", thumbnail)
        .kv("_time_text", time_text);
    auto timestamp = parse_time_text(time_text);
    if (timestamp >= 0)
      w.kv("timestamp", timestamp);
    else
      w.knull("timestamp");
    if (likes >= 0)
      w.kv("like_count", likes);
    else
      w.knull("like_count");
    w.kv("author_is_uploader", uploader)
        .kv("author_is_verified", verified)
        .kv("is_pinned", pinned)
        .kv("is_favorited", hearted)
        .end();
  }
};
inline Comment old_comment(yj::Val r, const std::string &parent) {
  Comment c;
  c.parent = parent;
  c.id = str(yj::get(r, "commentId"));
  c.body = text(yj::get(r, "contentText"));
  c.author = text(yj::get(r, "authorText"));
  c.author_id =
      str(yj::path(r, "authorEndpoint", "browseEndpoint", "browseId"));
  c.author_url =
      str(yj::path(r, "authorEndpoint", "browseEndpoint", "canonicalBaseUrl"));
  if (c.author_url.empty() && !c.author_id.empty())
    c.author_url = "/channel/" + c.author_id;
  if (!c.author_url.empty() && c.author_url[0] == '/')
    c.author_url = "https://www.youtube.com" + c.author_url;
  yj::each_elem(yj::path(r, "authorThumbnail", "thumbnails"), [&](yj::Val t) {
    c.thumbnail = str(yj::get(t, "url"));
    return true;
  });
  c.time_text = text(yj::get(r, "publishedTimeText"));
  c.likes = count_text(text(yj::get(r, "voteCount")));
  c.uploader = yj::get(r, "authorIsChannelOwner").boolean();
  c.pinned = yj::get(r, "pinnedCommentBadge").is_obj();
  c.hearted = yj::path(r, "actionButtons", "commentActionButtonsRenderer",
                       "creatorHeart")
                  .is_obj();
  auto badge = raw(yj::get(r, "authorCommentBadge"));
  c.verified = badge.find("VERIFIED") != std::string::npos;
  return c;
}
inline Comment
entity_comment(yj::Val model,
               const std::unordered_map<std::string, std::string> &entities,
               const std::string &parent) {
  Comment c;
  c.parent = parent;
  auto it = entities.find(str(yj::get(model, "commentKey")));
  if (it == entities.end())
    return c;
  auto entity = yj::parse(it->second);
  auto p = yj::path(entity, "payload", "commentEntityPayload");
  c.id = str(yj::path(p, "properties", "commentId"));
  c.body = str(yj::path(p, "properties", "content", "content"));
  c.time_text = str(yj::path(p, "properties", "publishedTime"));
  c.likes = count_text(str(yj::path(p, "toolbar", "likeCountA11y")));
  auto author = yj::get(p, "author");
  c.author = str(yj::get(author, "displayName"));
  c.author_id = str(yj::get(author, "channelId"));
  c.thumbnail = str(yj::get(author, "avatarThumbnailUrl"));
  c.uploader = yj::get(author, "isCreator").boolean();
  c.verified = yj::get(author, "isVerified").boolean();
  auto command = yj::path(author, "channelCommand", "innertubeCommand");
  c.author_url = str(yj::path(command, "browseEndpoint", "canonicalBaseUrl"));
  if (c.author_url.empty())
    c.author_url =
        str(yj::path(command, "commandMetadata", "webCommandMetadata", "url"));
  if (c.author_url.empty() && !c.author_id.empty())
    c.author_url = "/channel/" + c.author_id;
  if (!c.author_url.empty() && c.author_url[0] == '/')
    c.author_url = "https://www.youtube.com" + c.author_url;
  c.pinned = yj::get(model, "pinnedText").is_str();
  it = entities.find(str(yj::get(model, "toolbarStateKey")));
  if (it != entities.end())
    c.hearted = str(yj::path(yj::parse(it->second), "payload",
                             "engagementToolbarStateEntityPayload",
                             "heartState")) == "TOOLBAR_HEART_STATE_HEARTED";
  return c;
}
struct CommentOptions {
  size_t limit = 100, max_pages = 1000, max_depth = 5,
         max_replies_per_thread = 100;
  bool newest = true, replies = true;
};
struct CommentResult {
  size_t count = 0, pages = 0;
  bool exhausted = false;
  std::string status;
};

class Youtube {
public:
  Cache cache;
  bool authenticated() const { return !sapisid_.empty(); }
  using TokenProvider =
      std::function<std::string(const std::string &, const std::string &,
                                const std::string &, const std::string &)>;
  TokenProvider token_provider;
  void set_po_token(const std::string &specification) {
    auto plus = specification.find('+'), dot = specification.find('.');
    if (plus == std::string::npos || dot == std::string::npos || dot > plus)
      throw std::runtime_error(
          "PO token syntax: CLIENT.gvs+TOKEN (or .player / .subs)");
    auto type = specification.substr(dot + 1, plus - dot - 1);
    if (type != "gvs" && type != "player" && type != "subs")
      throw std::runtime_error("unknown PO-token context");
    tokens_[specification.substr(0, plus)] = specification.substr(plus + 1);
  }
  std::string token(const std::string &client, const std::string &type,
                    const std::string &id) {
    auto key = client + "." + type;
    auto it = tokens_.find(key);
    if (it != tokens_.end())
      return it->second;
    if (token_provider)
      return token_provider(client, type, id, visitor_);
    return {};
  }
  void proxy(const std::string &value) { http_.set_proxy(value); }
  std::string get_url(const std::string &url) { return fetch(url, ""); }
  const std::string &config_json() const { return config_; }
  using Transport =
      std::function<std::string(const std::string &, const std::string &,
                                const std::vector<std::string> &)>;
  explicit Youtube(Transport transport = {})
      : transport_(std::move(transport)) {
    http_.set_timeouts(10000, 30000);
  }
  void cookies(const std::string &filename) {
    std::ifstream f(filename);
    if (!f)
      throw std::runtime_error("cannot read cookies file");
    std::string line;
    cookies_.clear();
    sapisid_.clear();
    sapisid1_.clear();
    sapisid3_.clear();
    while (std::getline(f, line)) {
      if (line.rfind("#HttpOnly_", 0) == 0)
        line.erase(0, 10);
      if (line.empty() || line[0] == '#')
        continue;
      if (line.back() == '\r')
        line.pop_back();
      std::vector<std::string> fields;
      size_t start = 0;
      for (size_t i = 0; i <= line.size(); ++i)
        if (i == line.size() || line[i] == '\t') {
          fields.push_back(line.substr(start, i - start));
          start = i + 1;
        }
      if (fields.size() != 7)
        continue;
      auto domain = fields[0];
      if (!domain.empty() && domain[0] == '.')
        domain.erase(0, 1);
      if (domain != "youtube.com" && domain != "www.youtube.com")
        continue;
      if (fields[5].find_first_of("\r\n;") != std::string::npos ||
          fields[6].find_first_of("\r\n") != std::string::npos)
        continue;
      int64_t expiry = std::strtoll(fields[4].c_str(), nullptr, 10);
      if (expiry > 0 && expiry < std::time(nullptr))
        continue;
      if (!cookies_.empty())
        cookies_ += "; ";
      cookies_ += fields[5] + "=" + fields[6];
      if (fields[5] == "SAPISID" ||
          (sapisid_.empty() && fields[5] == "__Secure-3PAPISID"))
        sapisid_ = fields[6];
      if (fields[5] == "__Secure-1PAPISID")
        sapisid1_ = fields[6];
      if (fields[5] == "__Secure-3PAPISID")
        sapisid3_ = fields[6];
    }
    if (cookies_.empty())
      throw std::runtime_error("no unexpired YouTube cookies in file");
  }
  void bootstrap(const std::string &video_id) {
    bootstrap_url("https://www.youtube.com/watch?v=" +
                      Http::url_encode(video_id) + "&hl=en",
                  video_id);
  }
  void bootstrap_playlist(const std::string &playlist_id) {
    bootstrap_url("https://www.youtube.com/playlist?list=" +
                      Http::url_encode(playlist_id) + "&hl=en",
                  "");
  }
  void bootstrap_url(const std::string &url, const std::string &video_id) {
    html_ = fetch(url, "");
    player_code_.clear();
    initial_ = embedded_json(html_, "ytInitialData =");
    if (initial_.empty())
      initial_ = embedded_json(html_, "ytInitialData");
    if (initial_.empty())
      initial_ = embedded_json(html_, "\"ytInitialData\"");
    player_ = embedded_json(html_, "ytInitialPlayerResponse =");
    if (player_.empty())
      player_ = embedded_json(html_, "ytInitialPlayerResponse");
    config_.clear();
    size_t pos = 0;
    while ((pos = html_.find("ytcfg.set(", pos)) != std::string::npos) {
      auto candidate = embedded_json(html_.substr(pos), "ytcfg.set(");
      if (yj::get(yj::parse(candidate), "INNERTUBE_CONTEXT").is_obj()) {
        config_ = candidate;
        break;
      }
      pos += 10;
    }
    auto cfg = yj::parse(config_);
    context_ = raw(yj::get(cfg, "INNERTUBE_CONTEXT"));
    key_ = str(yj::get(cfg, "INNERTUBE_API_KEY"));
    if (context_ == "null")
      throw std::runtime_error("YouTube did not provide its API context "
                               "(consent, login or network interstitial)");
    auto client = yj::path(yj::parse(context_), "client");
    version_ = str(yj::get(client, "clientVersion"));
    visitor_ = str(yj::get(client, "visitorData"));
    client_id_ = yj::get(cfg, "INNERTUBE_CONTEXT_CLIENT_NAME").i64(1);
    delegated_ = str(yj::get(cfg, "DELEGATED_SESSION_ID"));
    user_session_ = str(yj::get(cfg, "USER_SESSION_ID"));
    session_index_ = yj::get(cfg, "SESSION_INDEX").i64();
    auto sync = str(yj::get(cfg, "DATASYNC_ID"));
    auto sep = sync.find("||");
    if (sep != std::string::npos) {
      auto one = sync.substr(0, sep), two = sync.substr(sep + 2);
      if (two.empty()) {
        if (user_session_.empty())
          user_session_ = one;
      } else {
        if (delegated_.empty())
          delegated_ = one;
        if (user_session_.empty())
          user_session_ = two;
      }
    }
    if (player_.empty() && !video_id.empty())
      player_ =
          api("player",
              yjw::Writer(false).obj().kv("videoId", video_id).end().str());
  }
  std::string player_url() const {
    auto cfg = yj::parse(config_);
    auto url = str(yj::get(cfg, "PLAYER_JS_URL"));
    if (url.empty())
      url = str(first(cfg, "jsUrl"));
    if (url.empty()) {
      auto pos = html_.find("/s/player/");
      if (pos != std::string::npos) {
        auto end = html_.find('"', pos);
        if (end != std::string::npos)
          url = html_.substr(pos, end - pos);
      }
    }
    if (url.rfind("//", 0) == 0)
      return "https:" + url;
    if (!url.empty() && url[0] == '/')
      return "https://www.youtube.com" + url;
    return url;
  }
  std::string player_code() {
    if (!player_code_.empty())
      return player_code_;
    auto url = player_url();
    if (url.empty())
      throw std::runtime_error("YouTube player JavaScript URL missing");
    auto cached = cache.get("player:" + url);
    if (!cached.empty())
      return player_code_ = cached;
    player_code_ = fetch(url, "");
    cache.put("player:" + url, player_code_);
    return player_code_;
  }
  std::string player_response(const std::string &id,
                              const std::string &client_name) {
    auto saved_context = context_, saved_version = version_, saved_ua = ua_,
         saved_cookies = cookies_, saved_sapisid = sapisid_, saved1 = sapisid1_,
         saved3 = sapisid3_;
    auto saved_client = client_id_;
    auto restore = [&]() {
      context_ = saved_context;
      version_ = saved_version;
      client_id_ = saved_client;
      ua_ = saved_ua;
      cookies_ = saved_cookies;
      sapisid_ = saved_sapisid;
      sapisid1_ = saved1;
      sapisid3_ = saved3;
    };
    try {
      const auto &spec = client_spec(client_name);
      if (spec.authentication && !authenticated())
        throw std::runtime_error(client_name +
                                 " requires explicit account cookies");
      if (!spec.cookies) {
        cookies_.clear();
        sapisid_.clear();
        sapisid1_.clear();
        sapisid3_.clear();
      }
      context_ = client_name == "web" ? saved_context : spec.context;
      auto client_json = raw(yj::get(yj::parse(context_), "client"));
      if (!visitor_.empty())
        client_json = json_set(client_json, "visitorData",
                               yjw::Writer(false).val(visitor_).str());
      client_json = json_set(client_json, "timeZone", "\"UTC\"");
      client_json = json_set(client_json, "utcOffsetMinutes", "0");
      context_ = json_set(context_, "client", client_json);
      version_ = str(yj::get(yj::parse(client_json), "clientVersion"));
      client_id_ = spec.id;
      auto agent = str(yj::get(yj::parse(client_json), "userAgent"));
      if (!agent.empty())
        ua_ = agent;
      yjw::Writer body(false);
      body.obj()
          .kv("videoId", id)
          .kv("contentCheckOk", true)
          .kv("racyCheckOk", true);
      if (!spec.params.empty())
        body.kv("params", spec.params);
      auto pot = token(client_name, "player", id);
      if (!pot.empty())
        body.key("serviceIntegrityDimensions").obj().kv("poToken", pot).end();
      if (spec.javascript) {
        auto sts = yj::get(yj::parse(config_), "STS").i64();
        if (!sts) {
          auto code = player_code();
          std::smatch match;
          if (std::regex_search(
                  code, match,
                  std::regex("(?:signatureTimestamp|sts)\\s*:\\s*([0-9]{5})")))
            sts = std::stoll(match[1]);
        }
        body.key("playbackContext")
            .obj()
            .key("contentPlaybackContext")
            .obj()
            .kv("html5Preference", "HTML5_PREF_WANTS");
        if (sts)
          body.kv("signatureTimestamp", sts);
        body.end().end();
      }
      body.end();
      auto response = api("player", body.str());
      restore();
      return response;
    } catch (...) {
      restore();
      throw;
    }
  }
  const std::string &player_json() const { return player_; }
  const std::string &initial_json() const { return initial_; }
  struct PlaylistEntry {
    std::string id, title, channel;
    int64_t duration = 0;
  };
  struct PlaylistResult {
    size_t count = 0, pages = 0;
    bool exhausted = false;
  };
  PlaylistResult
  playlist(const std::string &playlist_id, size_t limit, size_t max_pages,
           const std::function<void(const PlaylistEntry &)> &sink) {
    if (context_.empty())
      throw std::runtime_error("bootstrap must run before playlist extraction");
    auto response = api("browse", yjw::Writer(false)
                                      .obj()
                                      .kv("browseId", "VL" + playlist_id)
                                      .end()
                                      .str());
    PlaylistResult result;
    std::set<std::string> tokens;
    // Keep duplicate video IDs: a playlist may intentionally repeat videos.
    for (;;) {
      auto root = yj::parse(response);
      std::string next;
      const std::string_view lists[] = {"playlistVideoListRenderer",
                                        "playlistVideoListContinuation"};
      bool found = false;
      auto consume = [&](yj::Val list) {
        const std::string_view markers[] = {
            "playlistVideoRenderer", "lockupViewModel",
            "continuationItemRenderer", "continuationItemViewModel"};
        yj::find_any(list, markers, 4, [&](std::string_view kind, yj::Val v) {
          if (kind == "continuationItemRenderer" ||
              kind == "continuationItemViewModel") {
            next = continuation(v);
            return true;
          }
          PlaylistEntry e;
          if (kind == "lockupViewModel") {
            if (str(yj::get(v, "contentType")) != "LOCKUP_CONTENT_TYPE_VIDEO")
              return true;
            e.id = str(yj::get(v, "contentId"));
            auto metadata = yj::path(v, "metadata", "lockupMetadataViewModel");
            e.title = text(yj::get(metadata, "title"));
            const std::string_view parts[] = {"metadataParts"};
            yj::find_any(metadata, parts, 1,
                         [&](std::string_view, yj::Val row) {
                           yj::each_elem(row, [&](yj::Val part) {
                             if (first(part, "browseEndpoint")) {
                               e.channel = text(yj::get(part, "text"));
                               return false;
                             }
                             return true;
                           });
                           return e.channel.empty();
                         });
            auto badge =
                first(yj::get(v, "contentImage"), "thumbnailBadgeViewModel");
            auto duration = str(yj::get(badge, "text"));
            if (duration.find(':') != std::string::npos) {
              int64_t segment = 0;
              for (char c : duration) {
                if (c == ':') {
                  e.duration = e.duration * 60 + segment;
                  segment = 0;
                } else if (c >= '0' && c <= '9')
                  segment = segment * 10 + (c - '0');
              }
              e.duration = e.duration * 60 + segment;
            }
          } else {
            e.id = str(yj::get(v, "videoId"));
            e.title = text(yj::get(v, "title"));
            e.channel = text(yj::get(v, "shortBylineText"));
            e.duration = yj::get(v, "lengthSeconds").i64();
          }
          if (!e.id.empty())
            found = true;
          if (!e.id.empty() && result.count < limit) {
            sink(e);
            ++result.count;
          }
          return result.count < limit;
        });
        if (next.empty())
          next =
              str(yj::get(first(list, "nextContinuationData"), "continuation"));
      };
      yj::find_any(root, lists, 2, [&](std::string_view, yj::Val list) {
        found = true;
        consume(list);
        return false;
      });
      if (!found)
        consume(yj::get(root, "contents"));
      if (!found)
        for (auto v : items(root)) {
          consume(v);
        }
      ++result.pages;
      if (!found && result.pages == 1)
        throw std::runtime_error(
            "Playlist unavailable or unsupported response");
      if (next.empty()) {
        result.exhausted = result.count < limit;
        break;
      }
      if (result.count >= limit || result.pages >= max_pages)
        break;
      if (!tokens.insert(next).second)
        throw std::runtime_error(
            "YouTube repeated a playlist continuation token");
      response =
          api("browse",
              yjw::Writer(false).obj().kv("continuation", next).end().str());
    }
    return result;
  }
  static std::vector<yj::Val> items(yj::Val response) {
    std::vector<yj::Val> out;
    const std::string_view keys[] = {"reloadContinuationItemsCommand",
                                     "appendContinuationItemsAction"};
    yj::find_any(response, keys, 2, [&](std::string_view, yj::Val cmd) {
      yj::each_elem(yj::get(cmd, "continuationItems"), [&](yj::Val item) {
        out.push_back(item);
        return true;
      });
      return true;
    });
    return out;
  }
  CommentResult comments(const std::string &id, const CommentOptions &opt,
                         const std::function<void(const Comment &)> &sink) {
    if (context_.empty())
      bootstrap(id);
    CommentResult result;
    if (opt.limit == 0) {
      result.status = "limit";
      return result;
    }
    std::string token;
    const std::string_view sections[] = {"itemSectionRenderer"};
    yj::find_any(yj::parse(initial_), sections, 1,
                 [&](std::string_view, yj::Val section) {
                   if (str(yj::get(section, "sectionIdentifier")) ==
                       "comment-item-section") {
                     token = continuation(section);
                     return false;
                   }
                   return true;
                 });
    if (token.empty())
      token = comment_continuation(id);
    std::set<std::string> seen_ids, seen_tokens;
    std::unordered_map<std::string, std::string> entities;
    bool budget = false;
    bool header_seen = false;
    std::function<bool(std::string, const std::string &, size_t, size_t)> pages;
    std::function<void(const std::vector<yj::Val> &, const std::string &,
                       size_t, size_t, size_t)>
        consume;
    consume = [&](const std::vector<yj::Val> &values, const std::string &parent,
                  size_t depth, size_t start_count, size_t thread_limit) {
      for (auto item : values) {
        if (result.count >= opt.limit ||
            result.count - start_count >= thread_limit)
          return;
        auto thread = yj::get(item, "commentThreadRenderer");
        auto old = thread ? yj::path(thread, "comment", "commentRenderer")
                          : yj::get(item, "commentRenderer");
        auto model =
            thread ? yj::path(thread, "commentViewModel", "commentViewModel")
                   : yj::get(item, "commentViewModel");
        Comment c = old ? old_comment(old, parent)
                        : entity_comment(model, entities, parent);
        if (c.id.empty() || !seen_ids.insert(c.id).second)
          continue;
        sink(c);
        ++result.count;
        if (!opt.replies || depth >= opt.max_depth || result.count >= opt.limit)
          continue;
        auto replies = yj::path(thread, "replies", "commentRepliesRenderer");
        if (!replies)
          continue;
        const size_t reply_start = result.count;
        std::vector<yj::Val> inline_items;
        yj::each_elem(yj::get(replies, "contents"), [&](yj::Val x) {
          inline_items.push_back(x);
          return true;
        });
        yj::each_elem(yj::get(replies, "subThreads"), [&](yj::Val x) {
          inline_items.push_back(x);
          return true;
        });
        consume(inline_items, c.id, depth + 1, reply_start,
                opt.max_replies_per_thread);
        std::string next;
        for (auto x : inline_items)
          if (yj::get(x, "continuationItemRenderer")) {
            next = continuation(x);
            break;
          }
        if (next.empty())
          next = str(
              yj::get(first(replies, "nextContinuationData"), "continuation"));
        if (!next.empty() &&
            result.count - reply_start < opt.max_replies_per_thread)
          pages(next, c.id, depth + 1,
                opt.max_replies_per_thread - (result.count - reply_start));
      }
    };
    pages = [&](std::string next, const std::string &parent, size_t depth,
                size_t thread_limit) {
      const size_t start_count = result.count;
      while (!next.empty() && result.count < opt.limit &&
             result.count - start_count < thread_limit) {
        if (result.pages >= opt.max_pages) {
          budget = true;
          return false;
        }
        if (!seen_tokens.insert(parent + ":" + next).second)
          throw std::runtime_error("YouTube repeated a comments continuation "
                                   "token; extraction stopped");
        const auto requested_token = next;
        auto response =
            api("next",
                yjw::Writer(false).obj().kv("continuation", next).end().str());
        ++result.pages;
        auto root = yj::parse(response);
        auto vd = str(yj::path(root, "responseContext", "visitorData"));
        if (!vd.empty())
          visitor_ = vd;
        yj::each_elem(yj::path(root, "frameworkUpdates", "entityBatchUpdate",
                               "mutations"),
                      [&](yj::Val e) {
                        auto k = str(yj::get(e, "entityKey"));
                        if (!k.empty())
                          entities[k] = raw(e);
                        return true;
                      });
        auto values = items(root);
        next.clear();
        if (parent == "root" && !header_seen) {
          for (auto item : values) {
            auto header = yj::get(item, "commentsHeaderRenderer");
            if (!header)
              continue;
            header_seen = true;
            size_t index = 0;
            yj::each_elem(yj::path(header, "sortMenu",
                                   "sortFilterSubMenuRenderer", "subMenuItems"),
                          [&](yj::Val menu) {
                            if (index++ == (opt.newest ? 1u : 0u)) {
                              next = continuation(menu);
                              return false;
                            }
                            return true;
                          });
            break;
          }
          if (!next.empty()) {
            // Top sorting may return the initial token again. It is
            // a one-time header transition, not an infinite page loop.
            if (next == requested_token)
              seen_tokens.erase(parent + ":" + next);
            continue;
          }
        }
        consume(values, parent, depth, start_count, thread_limit);
        for (auto item : values)
          if (yj::get(item, "continuationItemRenderer")) {
            next = continuation(item);
            break;
          }
        if (values.empty() && !header_seen && result.count == 0) {
          auto message = text(first(root, "messageRenderer"));
          throw std::runtime_error(message.empty()
                                       ? "Comments unavailable, disabled, or "
                                         "an unsupported response was received"
                                       : message);
        }
      }
      return next.empty();
    };
    result.exhausted = pages(token, "root", 1, opt.limit);
    if (result.count >= opt.limit)
      result.exhausted = false;
    result.status =
        budget ? "page_limit" : (result.exhausted ? "complete" : "limit");
    if (budget)
      result.exhausted = false;
    return result;
  }
  std::string api(const std::string &endpoint, const std::string &fields) {
    if (context_.empty())
      throw std::runtime_error("bootstrap must run before API requests");
    std::string body = "{\"context\":" + context_;
    auto f = yj::parse(fields);
    yj::each_member(f, [&](std::string_view k, yj::Val v) {
      body += "," + yjw::Writer(false).val(k).str() + ":" + raw(v);
      return true;
    });
    body += "}";
    std::string url = "https://www.youtube.com/youtubei/v1/" + endpoint +
                      "?prettyPrint=false";
    if (!key_.empty())
      url += "&key=" + Http::url_encode(key_);
    auto response = fetch(url, body);
    auto root = yj::parse(response);
    if (yj::get(root, "error"))
      throw std::runtime_error("YouTube API error: " +
                               str(yj::path(root, "error", "message")));
    if (!root.is_obj())
      throw std::runtime_error("YouTube returned a non-JSON API response");
    return response;
  }

private:
  std::string fetch(const std::string &url, const std::string &body) {
    std::vector<std::string> headers = {
        "User-Agent: " + ua_, "Origin: https://www.youtube.com",
        "Referer: https://www.youtube.com/",
        "X-Youtube-Client-Name: " + std::to_string(client_id_)};
    if (!version_.empty())
      headers.push_back("X-Youtube-Client-Version: " + version_);
    if (!visitor_.empty())
      headers.push_back("X-Goog-Visitor-Id: " + visitor_);
    const bool account_origin = url.rfind("https://www.youtube.com/", 0) == 0;
    if (account_origin && !cookies_.empty())
      headers.push_back("Cookie: " + cookies_);
    if (account_origin && !sapisid_.empty()) {
      auto now = std::to_string(std::time(nullptr));
      std::string authorization;
      for (auto scheme :
           {std::pair<const char *, std::string>{"SAPISIDHASH", sapisid_},
            {"SAPISID1PHASH", sapisid1_},
            {"SAPISID3PHASH", sapisid3_}}) {
        if (scheme.second.empty())
          continue;
        auto input = (user_session_.empty() ? "" : user_session_ + " ") + now +
                     " " + scheme.second + " https://www.youtube.com";
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned len = 0;
        if (EVP_Digest(input.data(), input.size(), hash, &len, EVP_sha1(),
                       nullptr) != 1)
          throw std::runtime_error("SHA1 signing failed");
        std::string hex;
        for (unsigned i = 0; i < len; ++i) {
          hex += "0123456789abcdef"[hash[i] >> 4];
          hex += "0123456789abcdef"[hash[i] & 15];
        }
        if (!authorization.empty())
          authorization += ' ';
        authorization += std::string(scheme.first) + " " + now + "_" + hex +
                         (user_session_.empty() ? "" : "_u");
      }
      headers.push_back("Authorization: " + authorization);
      headers.push_back("X-Origin: https://www.youtube.com");
      headers.push_back("X-Goog-AuthUser: " + std::to_string(session_index_));
      if (!delegated_.empty())
        headers.push_back("X-Goog-PageId: " + delegated_);
      if (yj::get(yj::parse(config_), "LOGGED_IN").boolean())
        headers.push_back("X-Youtube-Bootstrap-Logged-In: true");
    }
    if (transport_)
      return transport_(url, body, headers);
    for (int attempt = 0; attempt < 3; ++attempt) {
      try {
        auto r = body.empty() ? http_.get(url, headers)
                              : http_.post(url, body, headers);
        if (r.status >= 200 && r.status < 300)
          return r.body;
        if (r.status != 429 && r.status < 500)
          throw std::logic_error("YouTube request failed (HTTP " +
                                 std::to_string(r.status) + ")");
        throw std::runtime_error("YouTube request failed (HTTP " +
                                 std::to_string(r.status) + ")");
      } catch (const std::logic_error &) {
        throw;
      } catch (...) {
        if (attempt == 2)
          throw;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(250 * (1 << attempt)));
      }
    }
    throw std::runtime_error("YouTube request retries exhausted");
  }
  Http http_;
  Transport transport_;
  std::string html_, config_, context_, key_, initial_, player_, version_,
      visitor_, cookies_, sapisid_, sapisid1_, sapisid3_, delegated_,
      user_session_;
  int64_t session_index_ = 0;
  std::map<std::string, std::string> tokens_;
  std::string player_code_,
      ua_ = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
            "Gecko) Chrome/131.0.0.0 Safari/537.36";
  int64_t client_id_ = 1;
};
} // namespace ytwe

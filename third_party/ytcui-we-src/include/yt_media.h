#pragma once
#include "we_formats.h"
#include "we_manifests.h"
#include "yt_js.h"
#include "yt_youtube.h"
#include <map>

namespace ytwe {
inline std::string url_decode(std::string_view s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 &&
        hex(s[i + 2]) >= 0) {
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else
      out += s[i] == '+' ? ' ' : s[i];
  }
  return out;
}
inline std::map<std::string, std::string> query(std::string_view s) {
  std::map<std::string, std::string> out;
  while (!s.empty()) {
    auto end = s.find('&');
    auto part = s.substr(0, end);
    auto eq = part.find('=');
    out[url_decode(part.substr(0, eq))] =
        eq == std::string_view::npos ? "" : url_decode(part.substr(eq + 1));
    if (end == std::string_view::npos)
      break;
    s.remove_prefix(end + 1);
  }
  return out;
}
inline std::string url_query(const std::string &url, const std::string &key) {
  auto p = url.find('?');
  if (p == std::string::npos)
    return {};
  auto q = query(std::string_view(url).substr(p + 1));
  auto it = q.find(key);
  return it == q.end() ? "" : it->second;
}
inline std::string remove_query(const std::string &url,
                                const std::string &key) {
  auto question = url.find('?');
  if (question == std::string::npos)
    return url;
  std::string result = url.substr(0, question);
  bool first = true;
  size_t start = question + 1;
  while (start < url.size()) {
    auto end = url.find('&', start);
    if (end == std::string::npos)
      end = url.size();
    auto part = std::string_view(url).substr(start, end - start);
    auto equal = part.find('=');
    if (url_decode(part.substr(0, equal)) != key) {
      result += first ? '?' : '&';
      first = false;
      result.append(part);
    }
    start = end + 1;
  }
  return result;
}
inline std::string set_query(std::string url, const std::string &key,
                             const std::string &value) {
  auto p = url.find('?');
  if (p != std::string::npos) {
    size_t start = p + 1;
    while (start < url.size()) {
      auto end = url.find('&', start);
      if (end == std::string::npos)
        end = url.size();
      auto eq = url.find('=', start);
      if (eq < end &&
          url_decode(std::string_view(url).substr(start, eq - start)) == key) {
        url.replace(eq + 1, end - eq - 1, Http::url_encode(value));
        return url;
      }
      start = end + 1;
    }
  }
  return url + (p == std::string::npos ? "?" : "&") + Http::url_encode(key) +
         "=" + Http::url_encode(value);
}
inline std::string quote(std::string_view s) {
  return yjw::Writer(false).val(s).str();
}
// Copies existing JSON fields structurally and substitutes only resolved URLs.
inline std::string resolve_player_json(
    const std::string &response,
    const std::function<std::unordered_map<std::string, std::string>(
        const std::vector<std::string> &, const std::vector<std::string> &)>
        &solve,
    const std::string &pot = "") {
  struct Pending {
    std::string json, url, signature, sp = "signature";
  };
  std::map<std::string, std::vector<Pending>> formats;
  std::set<std::string> nset, sset;
  auto root = yj::parse(response);
  auto sd = yj::get(root, "streamingData");
  for (const auto *name : {"formats", "adaptiveFormats"})
    yj::each_elem(yj::get(sd, name), [&](yj::Val f) {
      Pending p;
      p.json = raw(f);
      p.url = str(yj::get(f, "url"));
      if (p.url.empty()) {
        auto cipher = str(yj::get(f, "signatureCipher"));
        if (cipher.empty())
          cipher = str(yj::get(f, "cipher"));
        auto q = query(cipher);
        p.url = q["url"];
        p.signature = q["s"];
        if (!q["sp"].empty())
          p.sp = q["sp"];
      }
      if (p.url.empty())
        return true;
      auto n = url_query(p.url, "n");
      if (!n.empty())
        nset.insert(n);
      if (!p.signature.empty())
        sset.insert(p.signature);
      formats[name].push_back(std::move(p));
      return true;
    });
  auto manifest_n = [](const std::string &url) {
    auto n = url_query(url, "n");
    if (!n.empty())
      return n;
    auto start = url.find("/n/");
    if (start == std::string::npos)
      return std::string{};
    start += 3;
    return url_decode(
        std::string_view(url).substr(start, url.find('/', start) - start));
  };
  for (auto key : {"hlsManifestUrl", "dashManifestUrl"}) {
    auto n = manifest_n(str(yj::get(sd, key)));
    if (!n.empty())
      nset.insert(n);
  }
  auto solved = solve({nset.begin(), nset.end()}, {sset.begin(), sset.end()});
  auto rewritten = std::string("{");
  bool comma = false;
  yj::each_member(root, [&](std::string_view k, yj::Val v) {
    if (comma)
      rewritten += ',';
    comma = true;
    rewritten += quote(k) + ":";
    if (k != "streamingData") {
      rewritten += raw(v);
      return true;
    }
    rewritten += "{";
    bool member = false;
    yj::each_member(v, [&](std::string_view key, yj::Val val) {
      if (member)
        rewritten += ',';
      member = true;
      rewritten += quote(key) + ":";
      if (key == "hlsManifestUrl" || key == "dashManifestUrl") {
        auto url = str(val);
        auto n = manifest_n(url);
        if (!n.empty()) {
          auto it = solved.find("n:" + n);
          if (it == solved.end()) {
            rewritten += quote("");
            return true;
          }
          if (!url_query(url, "n").empty())
            url = set_query(url, "n", it->second);
          else {
            auto pos = url.find("/n/") + 3;
            url.replace(pos, url.find('/', pos) - pos,
                        Http::url_encode(it->second));
          }
        }
        if (!pot.empty())
          url = set_query(url, "pot", pot);
        rewritten += quote(url);
        return true;
      }
      if (key != "formats" && key != "adaptiveFormats") {
        rewritten += raw(val);
        return true;
      }
      rewritten += '[';
      bool entry = false;
      for (auto &p : formats[std::string(key)]) {
        if (!p.signature.empty()) {
          auto it = solved.find("sig:" + p.signature);
          if (it == solved.end())
            continue;
          p.url = set_query(p.url, p.sp, it->second);
        }
        auto n = url_query(p.url, "n");
        if (!n.empty()) {
          auto it = solved.find("n:" + n);
          if (it == solved.end())
            continue;
          p.url = set_query(p.url, "n", it->second);
        }
        if (!pot.empty())
          p.url = set_query(p.url, "pot", pot);
        if (entry)
          rewritten += ',';
        entry = true;
        rewritten += '{';
        yj::each_member(
            yj::parse(p.json), [&](std::string_view fk, yj::Val fv) {
              if (fk == "url" || fk == "signatureCipher" || fk == "cipher")
                return true;
              rewritten += quote(fk) + ":" + raw(fv) + ",";
              return true;
            });
        rewritten += "\"url\":" + quote(p.url) + "}";
      }
      rewritten += ']';
      return true;
    });
    rewritten += '}';
    return true;
  });
  rewritten += '}';
  return rewritten;
}
struct MediaResult {
  ytfast::VideoInfo info;
  std::string client, ua;
  std::string player_json;
  std::vector<std::string> diagnostics;
};
inline void select_live_transports(ytfast::VideoInfo &info) {
  if (!info.is_live_now)
    return;
  const bool manifest =
      std::any_of(info.formats.begin(), info.formats.end(),
                  [](const auto &f) { return f.protocol != "https"; });
  if (manifest)
    info.formats.erase(
        std::remove_if(info.formats.begin(), info.formats.end(),
                       [](const auto &f) { return f.protocol == "https"; }),
        info.formats.end());
  else if (!info.hls_url.empty())
    info.formats.clear();
  else
    throw std::runtime_error(
        "live playback requires an HLS/DASH manifest; segmented live URLs "
        "cannot be used as ordinary files");
}
inline MediaResult resolve_media(Youtube &yt, const std::string &id,
                                 const JsSolver &solver,
                                 const std::string &requested = "auto") {
  MediaResult result;
  parse_player(yt.player_json(), id, result.info);
  result.info.formats.clear();
  std::vector<std::string> requested_clients;
  if (requested == "auto")
    requested_clients =
        yt.authenticated()
            ? std::vector<std::string>{"web_embedded", "tv_samsung", "web"}
            : std::vector<std::string>{"tv_samsung", "visionos", "web"};
  else if (requested == "all") {
    for (auto &c : clients())
      if (!c.authentication || yt.authenticated())
        requested_clients.push_back(c.name);
  } else {
    std::istringstream list(requested);
    std::string name;
    while (std::getline(list, name, ','))
      requested_clients.push_back(name);
  }
  std::set<std::string> seen;
  for (auto &client : requested_clients) {
    try {
      const auto &spec = client_spec(client);
      auto response =
          client == "web" ? yt.player_json() : yt.player_response(id, client);
      auto status =
          str(yj::path(yj::parse(response), "playabilityStatus", "status"));
      if (status != "OK" && status != "LIVE_STREAM_OFFLINE")
        throw std::runtime_error(
            status + ": " +
            str(yj::path(yj::parse(response), "playabilityStatus", "reason")));
      auto response_id =
          str(yj::path(yj::parse(response), "videoDetails", "videoId"));
      if (!response_id.empty() && response_id != id)
        throw std::runtime_error(
            "player response belongs to a different video");
      auto resolved = resolve_player_json(
          response,
          [&](const std::vector<std::string> &n,
              const std::vector<std::string> &sig) {
            if (n.empty() && sig.empty())
              return std::unordered_map<std::string, std::string>{};
            return solver.solve(yt.player_code(), n, sig);
          },
          yt.token(client, "gvs", id));
      ytfast::VideoInfo info;
      parse_player(resolved, id, info);
      if (result.info.title.empty()) {
        result.info.title = info.title;
        result.info.channel = info.channel;
        result.info.channel_id = info.channel_id;
        result.info.description = info.description;
        result.info.thumbnails = info.thumbnails;
        result.info.thumbnail_url = info.thumbnail_url;
      }
      if (!result.info.duration_secs)
        result.info.duration_secs = info.duration_secs;
      if (!result.info.view_count)
        result.info.view_count = info.view_count;
      result.info.is_live_now = result.info.is_live_now || info.is_live_now;
      auto ua = str(yj::path(yj::parse(spec.context), "client", "userAgent"));
      if (ua.empty())
        ua = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
             "Gecko) Chrome/131.0.0.0 Safari/537.36";
      if (!info.hls_url.empty())
        try {
          Http h;
          auto r = h.get(info.hls_url, {"User-Agent: " + ua});
          if (r.status == 200)
            parse_hls(r.body, info.hls_url, info, client, ua);
        } catch (const std::exception &e) {
          result.diagnostics.push_back(client + " HLS: " + e.what());
        }
      if (!info.dash_url.empty())
        try {
          Http h;
          auto r = h.get(info.dash_url, {"User-Agent: " + ua});
          if (r.status == 200)
            parse_dash(r.body, info.dash_url, info, client, ua);
        } catch (const std::exception &e) {
          result.diagnostics.push_back(client + " DASH: " + e.what());
        }
      if (info.formats.empty() && info.hls_url.empty() && info.dash_url.empty())
        throw std::runtime_error(
            info.sabr_only ? "SABR-only response has no direct media URLs"
                           : "no usable media formats");
      if (result.client.empty()) {
        result.client = client;
        result.ua = ua;
        result.player_json = resolved;
      }
      if (result.info.hls_url.empty())
        result.info.hls_url = info.hls_url;
      if (result.info.dash_url.empty())
        result.info.dash_url = info.dash_url;
      result.info.backing_arenas.push_back(info.arena);
      for (auto &f : info.formats) {
        if (f.source_client.empty())
          f.source_client = client;
        if (f.user_agent.empty())
          f.user_agent = ua;
        auto key = f.format_id + ":" + f.protocol + ":" + f.language;
        if (seen.insert(key).second)
          result.info.formats.push_back(std::move(f));
      }
    } catch (const std::exception &e) {
      result.diagnostics.push_back(client + ": " + e.what());
    }
  }
  select_live_transports(result.info);
  if (!result.info.formats.empty() || !result.info.hls_url.empty())
    return result;
  std::string message = "No playable formats";
  for (auto &d : result.diagnostics)
    message += "\n  " + d;
  throw std::runtime_error(message);
}

} // namespace ytwe

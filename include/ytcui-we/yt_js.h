#pragma once
#include "we_cache.h"
#include "yj.h"
#include "yjw.h"
#include "yt_youtube.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ytwe {
// yt-dlp also executes its EJS helper in a JS runtime. The native port keeps
// HTTP, challenge batching, JSON, URL transformation and process control in
// C++. Google player JavaScript and the upstream EJS AST helper stay
// JavaScript.
inline std::string read_file(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot read " + p.string());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
inline std::string run_js(const std::string &runtime, const std::string &script,
                          int timeout_ms = 60000) {
  auto result = process({runtime, "--permission", "--no-warnings", "-"},
                        &script, true, timeout_ms);
  if (result.code)
    throw std::runtime_error(
        "JS challenge solver failed; Node >=23.5 is required");
  return result.output;
}
class JsSolver {
public:
  std::string runtime = "node";
  Cache cache;
  std::filesystem::path directory = "vendor/ejs";
  std::unordered_map<std::string, std::string>
  solve(const std::string &player, const std::vector<std::string> &n,
        const std::vector<std::string> &sig) const {
    if (n.empty() && sig.empty())
      return {};
    const auto player_key = "ejs-0.8.0:" + digest(player);
    std::unordered_map<std::string, std::string> results;
    std::vector<std::string> missing_n, missing_sig;
    for (auto &c : n) {
      auto v = cache.get(player_key + ":n:" + c);
      if (v.empty())
        missing_n.push_back(c);
      else
        results["n:" + c] = v;
    }
    for (auto &c : sig) {
      auto v = cache.get(player_key + ":sig:" + c);
      if (v.empty())
        missing_sig.push_back(c);
      else
        results["sig:" + c] = v;
    }
    if (missing_n.empty() && missing_sig.empty())
      return results;
    const auto prepared = cache.get(player_key + ":prepared");
    yjw::Writer request(false);
    request.obj();
    if (prepared.empty())
      request.kv("type", "player")
          .kv("player", player)
          .kv("output_preprocessed", true);
    else
      request.kv("type", "preprocessed").kv("preprocessed_player", prepared);
    request.key("requests").arr();
    for (const auto *type : {"n", "sig"}) {
      const auto &challenges =
          std::string(type) == "n" ? missing_n : missing_sig;
      if (challenges.empty())
        continue;
      request.obj().kv("type", type).key("challenges").arr();
      for (auto &c : challenges)
        request.val(c);
      request.end().end();
    }
    request.end().end();
    auto script = read_file(directory / "yt.solver.lib.js") +
                  "\nObject.assign(globalThis, lib);\n" +
                  read_file(directory / "yt.solver.core.js") +
                  "\nconsole.log(JSON.stringify(jsc(" + request.str() +
                  ")));\n";
    auto body = run_js(runtime, script);
    auto root = yj::parse(body);
    if (str(yj::get(root, "type")) == "error")
      throw std::runtime_error("EJS: " + str(yj::get(root, "error")));
    if (!yj::get(root, "responses").is_arr())
      throw std::runtime_error("invalid EJS solver response");
    const auto compiled = str(yj::get(root, "preprocessed_player"));
    if (!compiled.empty())
      cache.put(player_key + ":prepared", compiled);
    size_t index = 0;
    yj::each_elem(yj::get(root, "responses"), [&](yj::Val response) {
      std::string type = !missing_n.empty() && index == 0 ? "n" : "sig";
      ++index;
      if (str(yj::get(response, "type")) == "error")
        throw std::runtime_error("EJS " + type + ": " +
                                 str(yj::get(response, "error")));
      yj::each_member(yj::get(response, "data"),
                      [&](std::string_view key, yj::Val value) {
                        auto k = type + ":" + yj::unescape(key);
                        results[k] = str(value);
                        cache.put(player_key + ":" + k, str(value));
                        return true;
                      });
      return true;
    });
    return results;
  }
};
} // namespace ytwe

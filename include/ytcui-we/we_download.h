#pragma once
#include "we_platform.h"
#include "yt_media.h"
#include <atomic>
#include <future>
#include <mutex>
namespace ytwe {
struct DownloadConfig {
  size_t connections = 4;
  bool resume = true, overwrite = false;
  std::string user_agent;
};
inline void atomic_move(const std::filesystem::path &from,
                        const std::filesystem::path &to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("cannot finalize " + to.string());
#else
  std::filesystem::rename(from, to);
#endif
}
inline void download_file(const ytfast::StreamFormat &format,
                          const std::filesystem::path &target,
                          const DownloadConfig &options = {}) {
  if (!target.parent_path().empty())
    std::filesystem::create_directories(target.parent_path());
  if (std::filesystem::exists(target) && !options.overwrite) {
    if (format.content_length > 0 &&
        std::filesystem::file_size(target) ==
            static_cast<uint64_t>(format.content_length))
      return;
    throw std::runtime_error("output exists: " + target.string());
  }
  auto partial = target;
  partial += ".part";
  auto state_path = partial;
  state_path += ".ranges";
  const int64_t chunk_size = 1024 * 1024, total = format.content_length;
  auto headers = std::vector<std::string>{
      "User-Agent: " +
          (format.user_agent.empty() ? options.user_agent : format.user_agent),
      "Accept-Encoding: identity"};
  if (!format.fragments.empty()) {
    auto index_path = partial;
    index_path += ".fragment";
    size_t next_fragment = 0;
    uint64_t committed = 0;
    std::string identity_input = format.content_id + ":" + format.format_id;
    for (auto &f : format.fragments)
      identity_input += "\n" + f.url + ":" + f.range;
    auto identity = digest(identity_input);
    if (options.resume && std::filesystem::exists(partial)) {
      std::ifstream state(index_path);
      std::string saved;
      state >> saved >> next_fragment >> committed;
      if (saved != identity || next_fragment > format.fragments.size() ||
          std::filesystem::file_size(partial) < committed) {
        next_fragment = 0;
        committed = 0;
      } else
        std::filesystem::resize_file(partial, committed);
    }
    std::ofstream output(partial,
                         std::ios::binary |
                             (next_fragment ? std::ios::app : std::ios::trunc));
    if (!output)
      throw std::runtime_error("cannot open fragmented output");
    Http http;
    http.set_timeouts(10000, 60000);
    for (size_t i = next_fragment; i < format.fragments.size(); ++i) {
      auto &fragment = format.fragments[i];
      auto h = headers;
      if (!fragment.range.empty())
        h.push_back("Range: bytes=" + fragment.range);
      Http::Response r;
      for (int attempt = 0;; ++attempt) {
        try {
          r = http.get(fragment.url, h);
          if (r.status < 200 || r.status >= 300)
            throw std::runtime_error("fragment HTTP " +
                                     std::to_string(r.status));
          if (!fragment.range.empty() && r.status != 206)
            throw std::runtime_error("fragment server ignored range");
          break;
        } catch (...) {
          if (attempt == 2)
            throw;
          std::this_thread::sleep_for(
              std::chrono::milliseconds(250 * (1 << attempt)));
        }
      }
      output.write(r.body.data(), r.body.size());
      output.flush();
      if (!output)
        throw std::runtime_error("fragment output write failed");
      committed += r.body.size();
      auto tmp = index_path;
      tmp += ".tmp";
      {
        std::ofstream state(tmp);
        state << identity << ' ' << (i + 1) << ' ' << committed;
      }
      atomic_move(tmp, index_path);
    }
    output.close();
    atomic_move(partial, target);
    std::error_code ec;
    std::filesystem::remove(index_path, ec);
    return;
  }
  auto sequential = [&]() {
    std::ofstream f(partial, std::ios::binary | std::ios::trunc);
    if (!f)
      throw std::runtime_error("cannot create output: " + partial.string());
    Http http;
    http.set_timeouts(10000, 0);
    auto r = http.stream(format.url, headers, [&](const char *p, size_t n) {
      f.write(p, n);
      return static_cast<bool>(f);
    });
    f.close();
    if (r.status < 200 || r.status >= 300)
      throw std::runtime_error("media HTTP " + std::to_string(r.status));
    if (total > 0 &&
        std::filesystem::file_size(partial) != static_cast<uint64_t>(total))
      throw std::runtime_error("incomplete media download");
    atomic_move(partial, target);
    std::error_code ec;
    std::filesystem::remove(state_path, ec);
  };
  if (total <= 0 || options.connections <= 1) {
    sequential();
    return;
  }
  const size_t chunks =
      static_cast<size_t>((total + chunk_size - 1) / chunk_size);
  std::vector<unsigned char> done(chunks, 0);
  if (options.resume && std::filesystem::exists(partial) &&
      std::filesystem::file_size(partial) == static_cast<uint64_t>(total)) {
    std::ifstream f(state_path);
    int64_t saved_total = 0, saved_chunk = 0;
    std::string saved_identity;
    f >> saved_total >> saved_chunk >> saved_identity;
    if (saved_total == total && saved_chunk == chunk_size &&
        saved_identity == digest(format.content_id + ":" + format.format_id +
                                 ":" + std::to_string(total))) {
      size_t index;
      while (f >> index)
        if (index < chunks)
          done[index] = 1;
    }
  }
  if (!std::filesystem::exists(partial) ||
      std::filesystem::file_size(partial) != static_cast<uint64_t>(total)) {
    std::ofstream f(partial, std::ios::binary | std::ios::trunc);
    if (!f)
      throw std::runtime_error("cannot create output");
    f.seekp(total - 1);
    f.put('\0');
    if (!f)
      throw std::runtime_error("cannot allocate output file");
  }
  std::fstream output(partial, std::ios::binary | std::ios::in | std::ios::out);
  if (!output)
    throw std::runtime_error("cannot open partial output");
  std::mutex mutex;
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::string error;
  bool ignored_range = false;
  auto save = [&]() {
    auto tmp = state_path;
    tmp += ".tmp";
    {
      std::ofstream state(tmp);
      state << total << ' ' << chunk_size << ' '
            << digest(format.content_id + ":" + format.format_id + ":" +
                      std::to_string(total))
            << '\n';
      for (size_t i = 0; i < chunks; ++i)
        if (done[i])
          state << i << '\n';
    }
    atomic_move(tmp, state_path);
  };
  auto worker = [&]() {
    try {
      Http http;
      http.set_timeouts(10000, 60000);
      while (!failed) {
        auto i = next.fetch_add(1);
        if (i >= chunks)
          break;
        if (done[i])
          continue;
        auto start = static_cast<int64_t>(i) * chunk_size,
             end = std::min(total - 1, start + chunk_size - 1);
        auto h = headers;
        h.push_back("Range: bytes=" + std::to_string(start) + "-" +
                    std::to_string(end));
        Http::Response r;
        for (int attempt = 0;; ++attempt) {
          try {
            r = http.get(format.url, h);
            if (r.status == 429 || r.status >= 500)
              throw std::runtime_error("media HTTP " +
                                       std::to_string(r.status));
            break;
          } catch (...) {
            if (attempt == 2)
              throw;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(250 * (1 << attempt)));
          }
        }
        if (r.status == 200) {
          std::lock_guard<std::mutex> lock(mutex);
          ignored_range = true;
          throw std::runtime_error("server ignored byte range");
        }
        if (r.status != 206 ||
            r.content_range.rfind("bytes " + std::to_string(start) + "-" +
                                      std::to_string(end) + "/",
                                  0) != 0 ||
            r.body.size() != static_cast<size_t>(end - start + 1))
          throw std::runtime_error("invalid media range response (HTTP " +
                                   std::to_string(r.status) + ")");
        {
          std::lock_guard<std::mutex> lock(mutex);
          output.seekp(start);
          output.write(r.body.data(), r.body.size());
          output.flush();
          if (!output)
            throw std::runtime_error("media file write failed");
          done[i] = 1;
          save();
        }
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(mutex);
      if (error.empty())
        error = e.what();
      failed = true;
    }
  };
  std::vector<std::thread> workers;
  for (size_t i = 0;
       i <
       std::min<size_t>(std::clamp<size_t>(options.connections, 1, 32), chunks);
       ++i)
    workers.emplace_back(worker);
  for (auto &t : workers)
    t.join();
  output.close();
  if (ignored_range) {
    sequential();
    return;
  }
  if (failed)
    throw std::runtime_error(error);
  atomic_move(partial, target);
  std::error_code ec;
  std::filesystem::remove(state_path, ec);
}
inline int ffmpeg(const std::vector<std::string> &args) {
  std::vector<std::string> cmd{"ffmpeg", "-hide_banner", "-nostdin"};
  cmd.insert(cmd.end(), args.begin(), args.end());
  return process(cmd).code;
}
inline void download_selection(const ytfast::StreamFormat *video,
                               const ytfast::StreamFormat *audio,
                               const std::filesystem::path &target,
                               const DownloadConfig &options = {},
                               const std::string &audio_format = "",
                               int record_seconds = 0) {
  if (std::filesystem::exists(target) && !options.overwrite)
    return;
  const auto *single = video ? video : audio;
  if (!single)
    throw std::runtime_error("no selected streams");
  if (single->protocol != "https" && single->fragments.empty()) {
    std::vector<std::string> args{"-y",
                                  "-protocol_whitelist",
                                  "file,http,https,tcp,tls,crypto,data",
                                  "-user_agent",
                                  single->user_agent,
                                  "-i",
                                  playback_url(*single)};
    if (video && audio)
      args.insert(args.end(),
                  {"-user_agent", audio->user_agent, "-i", playback_url(*audio),
                   "-map", "0:v:0", "-map", "1:a:0"});
    if (record_seconds > 0) {
      args.push_back("-t");
      args.push_back(std::to_string(record_seconds));
    }
    if (!video)
      args.push_back("-vn");
    args.insert(args.end(), {"-c", "copy", target.string()});
    if (ffmpeg(args))
      throw std::runtime_error("manifest recording failed");
    return;
  }
  if (!(video && audio) && audio_format.empty()) {
    download_file(*single, target, options);
    return;
  }
  auto temp = target;
  temp += ".video.part-input";
  auto sound = target;
  sound += ".audio.part-input";
  if (video && audio) {
    auto audio_job = std::async(
        std::launch::async, [&]() { download_file(*audio, sound, options); });
    download_file(*video, temp, options);
    audio_job.get();
  } else {
    if (video)
      download_file(*video, temp, options);
    if (audio)
      download_file(*audio, sound, options);
  }
  std::vector<std::string> args{"-y"};
  if (video) {
    args.push_back("-i");
    args.push_back(temp.string());
  }
  if (audio) {
    args.push_back("-i");
    args.push_back(sound.string());
  }
  if (audio_format.empty()) {
    args.insert(args.end(), {"-map", "0:v:0", "-map", "1:a:0", "-c", "copy"});
  } else {
    args.insert(args.end(), {"-vn"});
    if (audio_format == "best")
      args.insert(args.end(), {"-c:a", "copy"});
    else if (audio_format == "mp3")
      args.insert(args.end(), {"-c:a", "libmp3lame", "-q:a", "2"});
    else if (audio_format == "opus")
      args.insert(args.end(), {"-c:a", "libopus"});
    else if (audio_format == "m4a" || audio_format == "aac")
      args.insert(args.end(), {"-c:a", "aac"});
    else if (audio_format == "flac")
      args.insert(args.end(), {"-c:a", "flac"});
    else if (audio_format == "wav")
      args.insert(args.end(), {"-c:a", "pcm_s16le"});
    else
      throw std::runtime_error("unsupported audio output format");
  }
  args.push_back(target.string());
  if (ffmpeg(args))
    throw std::runtime_error(
        "ffmpeg mux/transcode failed; input files retained");
  std::error_code ec;
  if (video)
    std::filesystem::remove(temp, ec);
  if (audio)
    std::filesystem::remove(sound, ec);
}
} // namespace ytwe

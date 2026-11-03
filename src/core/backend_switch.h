#pragma once
// Two backends, one interface, switchable while running.
//
// v4 chose its extractor with a Makefile variable, so when YouTube broke
// ytcui-dl every installed copy that had picked it was stuck until the user
// rebuilt. That is the problem this solves: ^B switches, and a failure that
// another extractor might survive is retried there automatically.
//
// Failover is deliberately conservative. It only fires for failures where a
// different extractor plausibly helps -- a PO-token wall, an unknown error, a
// missing binary -- and never for a private or deleted video, where retrying
// is just a slower way to show the same error. Backend::Failure::worth_failover
// draws that line.

#include <functional>
#include <memory>
#include <string>

#include "core/backend.h"
#include "core/config.h"

namespace ytcui {

class SwitchingBackend final : public Backend {
public:
    // `primary` is what the config asked for; the other one is built lazily,
    // because constructing the yt-dlp backend looks up PATH and there is no
    // reason to pay for it unless it is used.
    SwitchingBackend(config::Backend primary, bool allow_failover);
    ~SwitchingBackend() override;

    const char* name() const override;
    bool available(std::string* detail = nullptr) const override;

    std::vector<Video> search(const std::string& query, int max_results,
                              Failure* fail = nullptr) override;
    Video info(const std::string& video_id, Failure* fail = nullptr) override;
    Streams resolve(const std::string& video_id, PlayMode mode, int max_height,
                    Failure* fail = nullptr) override;
    void shutdown() override;

    // --- switching -------------------------------------------------------
    config::Backend active() const { return active_; }

    // Swap to the other backend. Returns false with a reason in `why` when the
    // other one cannot run -- asking for yt-dlp when it is not installed
    // should say so rather than silently doing nothing.
    bool toggle(std::string* why = nullptr);
    bool use(config::Backend which, std::string* why = nullptr);

    // Set when a call silently fell back, so the UI can say so once rather
    // than pretending the primary worked.
    bool   took_fallback() const { return took_fallback_; }
    std::string last_fallback_reason() const { return fallback_reason_; }
    void   clear_fallback_flag() { took_fallback_ = false; }

    // Called after a successful switch or an automatic fallback, so the UI can
    // refresh its status line.
    void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

private:
    Backend* get(config::Backend which);
    Backend* primary();
    Backend* other();
    config::Backend other_kind() const;
    void note_fallback(const std::string& reason);

    config::Backend active_;
    bool            allow_failover_;
    std::unique_ptr<Backend> ytcuidl_;
    std::unique_ptr<Backend> ytdlp_;
    bool        took_fallback_ = false;
    std::string fallback_reason_;
    std::function<void()> on_change_;
};

std::unique_ptr<Backend> make_ytdlp_backend();
std::unique_ptr<Backend> make_ytcuidl_backend();

}  // namespace ytcui

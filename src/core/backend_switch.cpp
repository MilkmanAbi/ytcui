#include "core/backend_switch.h"

#include "core/log.h"

namespace ytcui {

SwitchingBackend::SwitchingBackend(config::Backend primary, bool allow_failover)
    : active_(primary), allow_failover_(allow_failover) {}

SwitchingBackend::~SwitchingBackend() { shutdown(); }

Backend* SwitchingBackend::get(config::Backend which) {
    // Built on first use: constructing the yt-dlp backend searches PATH, and
    // a user who never switches should not pay for it.
    if (which == config::Backend::YtDlp) {
        if (!ytdlp_) ytdlp_ = make_ytdlp_backend();
        return ytdlp_.get();
    }
    if (!ytcuidl_) ytcuidl_ = make_ytcuidl_backend();
    return ytcuidl_.get();
}

Backend* SwitchingBackend::primary() { return get(active_); }

config::Backend SwitchingBackend::other_kind() const {
    return active_ == config::Backend::YtcuiDl ? config::Backend::YtDlp
                                               : config::Backend::YtcuiDl;
}

Backend* SwitchingBackend::other() { return get(other_kind()); }

const char* SwitchingBackend::name() const {
    return config::to_string(active_);
}

bool SwitchingBackend::available(std::string* detail) const {
    // const, but the backends are built lazily; the cast is confined here
    // rather than making every member mutable.
    return const_cast<SwitchingBackend*>(this)->primary()->available(detail);
}

void SwitchingBackend::note_fallback(const std::string& reason) {
    took_fallback_ = true;
    fallback_reason_ = reason;
    log::warn("backend fallback: " + reason);
    if (on_change_) on_change_();
}

bool SwitchingBackend::use(config::Backend which, std::string* why) {
    if (which == active_) {
        if (why) *why = std::string("already using ") + config::to_string(which);
        return true;
    }
    Backend* target = get(which);
    std::string detail;
    if (!target->available(&detail)) {
        if (why)
            *why = std::string("cannot switch to ") + config::to_string(which) +
                   ": " + detail;
        return false;
    }
    active_ = which;
    took_fallback_ = false;
    fallback_reason_.clear();
    log::info(std::string("backend switched to ") + config::to_string(which));
    if (why) *why = std::string("backend: ") + config::to_string(which);
    if (on_change_) on_change_();
    return true;
}

bool SwitchingBackend::toggle(std::string* why) { return use(other_kind(), why); }

std::vector<Video> SwitchingBackend::search(const std::string& query,
                                            int max_results, Failure* fail) {
    Failure f;
    std::vector<Video> out = primary()->search(query, max_results, &f);
    if (!out.empty()) {
        if (fail) fail->clear();
        return out;
    }

    if (allow_failover_ && f.worth_failover()) {
        Backend* alt = other();
        std::string detail;
        if (alt->available(&detail)) {
            Failure f2;
            std::vector<Video> alt_out = alt->search(query, max_results, &f2);
            if (!alt_out.empty()) {
                note_fallback(std::string(primary()->name()) + " failed (" +
                              f.message + "); used " + alt->name());
                if (fail) fail->clear();
                return alt_out;
            }
        }
    }
    if (fail) *fail = f;
    return out;
}

Video SwitchingBackend::info(const std::string& video_id, Failure* fail) {
    Failure f;
    Video v = primary()->info(video_id, &f);
    if (!v.empty()) {
        if (fail) fail->clear();
        return v;
    }
    if (allow_failover_ && f.worth_failover()) {
        Backend* alt = other();
        std::string detail;
        if (alt->available(&detail)) {
            Failure f2;
            Video alt_v = alt->info(video_id, &f2);
            if (!alt_v.empty()) {
                note_fallback(std::string(primary()->name()) + " failed (" +
                              f.message + "); used " + alt->name());
                if (fail) fail->clear();
                return alt_v;
            }
        }
    }
    if (fail) *fail = f;
    return v;
}

Streams SwitchingBackend::resolve(const std::string& video_id, PlayMode mode,
                                  int max_height, Failure* fail) {
    Failure f;
    Streams s = primary()->resolve(video_id, mode, max_height, &f);
    if (s.ok()) {
        if (fail) fail->clear();
        return s;
    }

    // This is the case that matters most. A video ytcui-dl cannot serve --
    // "made for kids", most often, where the only tokenless client that would
    // work declines it -- is exactly what yt-dlp with its own PO-token plugins
    // may manage. Failing over here is the difference between "this video does
    // not play" and "this video plays".
    if (allow_failover_ && f.worth_failover()) {
        Backend* alt = other();
        std::string detail;
        if (alt->available(&detail)) {
            Failure f2;
            Streams alt_s = alt->resolve(video_id, mode, max_height, &f2);
            if (alt_s.ok()) {
                note_fallback(std::string(primary()->name()) + " could not play it (" +
                              f.message + "); used " + alt->name());
                if (fail) fail->clear();
                return alt_s;
            }
        } else {
            log::debug("failover wanted but " + std::string(alt->name()) +
                       " unavailable: " + detail);
        }
    }
    if (fail) *fail = f;
    return s;
}

void SwitchingBackend::shutdown() {
    if (ytcuidl_) ytcuidl_->shutdown();
    if (ytdlp_) ytdlp_->shutdown();
}

}  // namespace ytcui

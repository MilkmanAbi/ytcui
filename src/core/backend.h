#pragma once
// The extractor behind ytcui.
//
// v4 picked this at build time with a Makefile variable, so changing your mind
// meant a rebuild -- and when YouTube broke ytcui-dl, every installed copy that
// had chosen it was simply stuck. v5 makes it an interface with two
// implementations, selected at runtime and switchable while the program is
// running.
//
// Each call takes an `error` out-parameter and returns empty on failure rather
// than throwing. The reason is failover: the switching backend added in the
// next slice needs to try one extractor, see a specific failure, and quietly
// try the other, and exceptions across that boundary make that read worse than
// it needs to.

#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/video.h"

namespace ytcui {

// Why a resolve failed, so the caller can decide whether another backend is
// worth trying. The distinction matters: a video that is private will fail on
// every backend, and retrying is just a delay with extra steps.
enum class FailKind {
    None,
    Network,        // no connection, DNS, TLS
    NotAvailable,   // private, removed, region blocked -- nobody can serve it
    NeedsToken,     // PO-Token wall / byte cap: another backend may do better
    NeedsAuth,      // age-gated or members-only: cookies would fix it
    BackendMissing, // the tool itself is not installed
    Unknown,
};

struct Failure {
    FailKind    kind = FailKind::None;
    std::string message;

    bool failed() const { return kind != FailKind::None; }
    // Is trying a different extractor plausibly worth it?
    bool worth_failover() const {
        return kind == FailKind::NeedsToken || kind == FailKind::Unknown ||
               kind == FailKind::BackendMissing;
    }
    void clear() { kind = FailKind::None; message.clear(); }
};

const char* to_string(FailKind k);

class Backend {
public:
    virtual ~Backend() = default;

    // "ytcui-dl" / "yt-dlp". Shown in the status line.
    virtual const char* name() const = 0;

    // Can this backend run at all right now? For --diag and for deciding
    // whether a switch is even possible. `detail` gets a human explanation.
    virtual bool available(std::string* detail = nullptr) const = 0;

    virtual std::vector<Video> search(const std::string& query, int max_results,
                                      Failure* fail = nullptr) = 0;

    // Metadata for one id, for a link dropped in without a search.
    virtual Video info(const std::string& video_id, Failure* fail = nullptr) = 0;

    // Resolve playable streams. `max_height` of 0 means best available.
    virtual Streams resolve(const std::string& video_id, PlayMode mode,
                            int max_height, Failure* fail = nullptr) = 0;

    // Release any sockets/threads. Called before exit.
    virtual void shutdown() {}
};

std::unique_ptr<Backend> make_backend(config::Backend which);
std::unique_ptr<Backend> make_backend(config::Backend which,
                                      bool allow_failover);

}  // namespace ytcui

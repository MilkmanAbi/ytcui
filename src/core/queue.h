#pragma once
// A play queue.
//
// Ranked third in docs/RESEARCH.md. ytfzf gets this through fzf's multi-select
// and people clearly use it; in a full TUI it is the obvious missing verb.
// "Play this next" and "add to the end" are different intentions and both are
// wanted, so both exist here.
//
// Deliberately in-memory and not persisted. A queue is what you are about to
// watch in this sitting; restoring last week's queue on launch would be
// surprising rather than helpful, and what you watched is already remembered
// by WatchState.

#include <cstddef>
#include <string>
#include <vector>

#include "types.h"

namespace ytui {

class Queue {
public:
    // Appended to the end.
    void add(const Video& v);
    // Jumps whatever is already waiting.
    void play_next(const Video& v);

    // Take the next one off the front. Empty video when there is nothing.
    Video take();
    const Video* peek() const;

    void remove(std::size_t index);
    void clear();

    // Reordering for the queue panel. No-ops at the ends rather than wrapping,
    // because wrapping a move is never what anyone means.
    bool move_up(std::size_t index);
    bool move_down(std::size_t index);

    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    const std::vector<Video>& items() const { return items_; }

    // Is this video already queued? Used to stop a double keypress silently
    // queueing the same thing twice.
    bool contains(const std::string& id) const;

    // Total runtime of what is waiting, for the panel header.
    int total_seconds() const;

private:
    std::vector<Video> items_;
};

}  // namespace ytui

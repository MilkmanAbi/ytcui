#include "core/queue.h"

#include <algorithm>

namespace ytui {

void Queue::add(const Video& v) {
    if (v.empty()) return;
    // Queueing the same thing twice is almost always a double keypress, not an
    // intention to watch it twice in a row.
    if (contains(v.id)) return;
    items_.push_back(v);
}

void Queue::play_next(const Video& v) {
    if (v.empty()) return;
    // If it is already waiting further down, this means "move it up" rather
    // than "add a second copy".
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == v.id) {
            Video existing = items_[i];
            items_.erase(items_.begin() + (std::ptrdiff_t)i);
            items_.insert(items_.begin(), std::move(existing));
            return;
        }
    }
    items_.insert(items_.begin(), v);
}

Video Queue::take() {
    if (items_.empty()) return {};
    Video v = items_.front();
    items_.erase(items_.begin());
    return v;
}

const Video* Queue::peek() const {
    return items_.empty() ? nullptr : &items_.front();
}

void Queue::remove(std::size_t index) {
    if (index >= items_.size()) return;
    items_.erase(items_.begin() + (std::ptrdiff_t)index);
}

void Queue::clear() { items_.clear(); }

bool Queue::move_up(std::size_t index) {
    if (index == 0 || index >= items_.size()) return false;
    std::swap(items_[index - 1], items_[index]);
    return true;
}

bool Queue::move_down(std::size_t index) {
    if (index + 1 >= items_.size()) return false;
    std::swap(items_[index], items_[index + 1]);
    return true;
}

bool Queue::contains(const std::string& id) const {
    if (id.empty()) return false;
    return std::any_of(items_.begin(), items_.end(),
                       [&](const Video& v) { return v.id == id; });
}

int Queue::total_seconds() const {
    int t = 0;
    for (const Video& v : items_)
        // A live stream has no length; counting it as 0 is the only honest
        // answer and keeps the header from claiming a total it cannot know.
        if (!v.is_live && v.duration_seconds > 0) t += v.duration_seconds;
    return t;
}

}  // namespace ytui

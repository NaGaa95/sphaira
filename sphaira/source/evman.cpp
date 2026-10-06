#include "evman.hpp"
#include <mutex>
#include <optional>
#include <algorithm>
#include <list>

namespace sphaira::evman {
namespace {

std::mutex mutex{};
std::list<EventData> events;

void remove_if_matching(const EventData& e, bool remove_matching) {
    if (remove_matching) {
        events.remove_if([&e](const EventData&a) { return e.index() == a.index(); });
    }
}

} // namespace

auto push(const EventData& e, bool remove_matching) -> bool {
    std::scoped_lock lock(mutex);
    remove_if_matching(e, remove_matching);
    events.push_back(e);
    return true;
}

auto push(EventData&& e, bool remove_matching) -> bool {
    std::scoped_lock lock(mutex);
    remove_if_matching(e, remove_matching);
    events.emplace_back(std::forward<EventData>(e));
    return true;
}

auto count() -> std::size_t {
    std::scoped_lock lock(mutex);
    return events.size();
}

auto pop() -> std::optional<EventData> {
    std::scoped_lock lock(mutex);
    if (events.empty()) {
        return std::nullopt;
    }
    // moved rather than copied: an event carries its whole result, and a page of
    // catalog is two dozen entries' worth of strings to duplicate for nothing.
    auto e = std::move(events.front());
    events.pop_front();
    return e;
}

auto popall() -> std::list<EventData> {
    std::scoped_lock lock(mutex);
    auto list_copy = events;
    events.clear();
    return list_copy;
}

} // namespace sphaira::evman

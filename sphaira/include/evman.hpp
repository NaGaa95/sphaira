// simple thread-safe list of events.
#pragma once

#include <optional>
#include <variant>
#include <list>
#include <string>
#include <vector>
#include <functional>
#include <stop_token>
#include <switch.h>
#include <nxlink.h>
#include "download.hpp"

namespace sphaira::evman {

struct LaunchNroEventData {
    std::string path;
    std::string argv;
};

struct ExitEventData {
    bool dummy;
};

// a worker's result, already bound to what happens with it on the main thread:
// the payload rides in the callback's captures, so evman needs none of its
// headers. the owner's token drops the callback rather than running it against
// something since destroyed. note this is the one event that must always be
// pushed with remove_matching false - its type no longer says what it is about,
// so matching would drop an unrelated callback.
struct CallbackEventData {
    std::function<void(void)> callback;
    std::stop_token stoken;
};

using EventData = std::variant<
    LaunchNroEventData,
    ExitEventData,
    NxlinkCallbackData,
    curl::DownloadEventData,
    CallbackEventData
>;

// returns number of events
auto count() -> std::size_t;

// thread-safe
auto push(const EventData& e, bool remove_matching = true) -> bool;
auto push(EventData&& e, bool remove_matching = true) -> bool;

// events are returned FIFO style, so if you push event a,b,c
// then pop() will return a then b then c.
auto pop() -> std::optional<EventData>;

// this pops all events, this is ideal to stop the main thread from
// hanging if loads of events are pushed and popped at the same time.
auto popall() -> std::list<EventData>;

} // namespace sphaira::evman

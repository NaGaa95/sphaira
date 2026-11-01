#pragma once

#include "owo.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sphaira::ui::forwarder {

struct Values {
    std::string title;
    std::string author;
    std::string version;
    std::vector<u8> icon;
    bool profile_selection{};
    ForwarderAddressSpace address_space{ForwarderAddressSpace::Bit36};
    CpuCoreMode core_mode{CpuCoreMode::Three};
    bool screenshot{true};
    bool video_capture{true};
    ForwarderNativeGameMode native_game_mode{ForwarderNativeGameMode::Automatic};
    ForwarderSvcDebugMode svc_debug_mode{ForwarderSvcDebugMode::Automatic};
};

using CreateCallback = std::function<bool(const Values& values)>;

struct Config {
    Values values;
    std::string icon_source{};
    std::string steam_query;
    std::string screen_title;
    std::string title_label;
    std::string submit_label;
    bool show_author{};
    bool show_version{};
    bool show_forwarder_options{true};
    CreateCallback on_create;
};

void Show(Config config);

} // namespace sphaira::ui::forwarder

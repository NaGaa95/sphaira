#pragma once

#include "ui/sidebar.hpp"
#include <string>
#include <vector>
#include <functional>

namespace sphaira::ownfoil {

// a saved Ownfoil API server location.
struct Config {
    // the entry's key and its ini section name: the server's own id, only ever
    // learned from a discovery reply or the handshake. an entry that has never
    // reached its server holds a `pending-` placeholder until then.
    std::string uid{};

    // owned by the server, refreshed from the handshake on every connect.
    std::string name{};

    std::string local_address{};   // host[:port], optional.
    std::string remote_address{};  // host[:port] or hostname, optional.
    std::string user{};
    std::string pass{};

    // the root url that last answered, so a connect goes straight there rather
    // than paying a connect timeout on a dead local address every launch.
    std::string resolved_url{};
};
using Configs = std::vector<Config>;

void LoadConfigs(Configs& out_configs);

// true for a locally generated stand-in uid. the `pending-` namespace is
// reserved by the API spec, so it can never collide with a real one.
auto IsPlaceholderUid(const std::string& uid) -> bool;

// creates or updates a saved server, keyed by config.uid (the ini section), and
// assigns a placeholder uid when it is empty. pass the entry's former uid as
// `previous_uid` so the old section is removed - the only way a key ever moves.
// any *other* entry carrying a real uid is dropped: the server issued it, so
// two entries sharing one describe the same server.
void SaveConfig(Config& config, const std::string& previous_uid = {});

void DeleteConfig(const std::string& uid);

// an empty uid matches nothing, and a placeholder never matches a real uid.
auto FindByUid(Configs& configs, const std::string& uid) -> Config*;

// the one way a saved entry is ever created or changed.
struct OwnfoilForm final : public ui::FormSidebar {
    using OnSaved = std::function<void(const Config&)>;

    explicit OwnfoilForm(const Config& config = {}, const OnSaved& on_saved = {});

private:
    void SetupButtons();

private:
    Config m_config{};
    const OnSaved m_on_saved{};

    ui::SidebarEntryTextInput* m_name{};
    ui::SidebarEntryTextInput* m_local_address{};
    ui::SidebarEntryTextInput* m_remote_address{};
    ui::SidebarEntryTextInput* m_user{};
    ui::SidebarEntryTextInput* m_pass{};
};

} // namespace sphaira::ownfoil

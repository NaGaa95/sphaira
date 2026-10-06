#include "utils/ownfoil.hpp"

#include "ui/sidebar.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <minIni.h>
#include <switch.h>

namespace sphaira::ownfoil {
namespace {

using namespace sphaira::ui;

#define OWNFOIL_INI_PATH "/config/sphaira/ownfoil.ini"

constexpr const char* PLACEHOLDER_UID_PREFIX = "pending-";

// the random part only has to be unique among local entries; it is discarded
// the moment the server supplies a real uid.
auto GeneratePlaceholderUid() -> std::string {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%s%016lx", PLACEHOLDER_UID_PREFIX, randomGet64());
    return buf;
}

} // namespace

OwnfoilForm::OwnfoilForm(const Config& config, const OnSaved& on_saved)
: FormSidebar{"Ownfoil Server"_i18n}
, m_config{config}
, m_on_saved{on_saved} {
    SetupButtons();
}

void OwnfoilForm::SetupButtons() {
    m_name = this->Add<SidebarEntryTextInput>(
        "Name"_i18n, m_config.name, "", "", -1, 32,
        "Set the display name of the server.\n\n"
        "Replaced by the name the server reports once it has been connected to."_i18n
    );

    m_local_address = this->Add<SidebarEntryTextInput>(
        "Local address"_i18n, m_config.local_address, "", "", -1, PATH_MAX,
        "Address used to reach the server from the same Wi-Fi network.\n\n"
        "At least one of Local address or Remote address must be set."_i18n
    );

    m_remote_address = this->Add<SidebarEntryTextInput>(
        "Remote address"_i18n, m_config.remote_address, "", "", -1, PATH_MAX,
        "Address used to reach the server remotely.\n\n"
        "At least one of Local address or Remote address must be set."_i18n
    );

    m_user = this->Add<SidebarEntryTextInput>(
        "User"_i18n, m_config.user, "", "", -1, PATH_MAX,
        "Optional: username used to log in to the server."_i18n
    );

    m_pass = this->Add<SidebarEntryTextInput>(
        "Pass"_i18n, m_config.pass, "", "", -1, PATH_MAX,
        "Optional: password used to log in to the server."_i18n
    );

    const auto callback = this->Add<SidebarEntryCallback>("Save"_i18n, [this](){
        m_config.name = m_name->GetValue();
        m_config.local_address = m_local_address->GetValue();
        m_config.remote_address = m_remote_address->GetValue();
        m_config.user = m_user->GetValue();
        m_config.pass = m_pass->GetValue();

        // a brand new entry gets a placeholder uid until its first connect.
        SaveConfig(m_config);
        App::Notify("Ownfoil server saved."_i18n);

        if (m_on_saved) {
            m_on_saved(m_config);
        }

        this->SetPop();
    },  "Saves the Ownfoil server entry."_i18n);

    callback->Depends([this](){
        return
            !m_name->GetValue().empty() &&
            (!m_local_address->GetValue().empty() || !m_remote_address->GetValue().empty());
    }, "Name and at least one of Local/Remote address must be set!"_i18n);
}

void LoadConfigs(Configs& out_configs) {
    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<Configs*>(UserData);
        if (!Section || !Key || !Value) {
            return 1;
        }

        // add new entry if the section (uid) changed.
        if (e->empty() || std::strcmp(Section, e->back().uid.c_str())) {
            e->emplace_back();
            e->back().uid = Section;
        }

        if (!std::strcmp(Key, "name")) {
            e->back().name = Value;
        } else if (!std::strcmp(Key, "local_address")) {
            e->back().local_address = Value;
        } else if (!std::strcmp(Key, "remote_address")) {
            e->back().remote_address = Value;
        } else if (!std::strcmp(Key, "user")) {
            e->back().user = Value;
        } else if (!std::strcmp(Key, "pass")) {
            e->back().pass = Value;
        } else if (!std::strcmp(Key, "resolved_url")) {
            e->back().resolved_url = Value;
        } else {
            log_write("[OWNFOIL] INI: unknown key %s=%s\n", Key, Value);
        }

        return 1;
    };

    out_configs.resize(0);
    ini_browse(cb, &out_configs, OWNFOIL_INI_PATH);
    log_write("[OWNFOIL] Found %zu server configs\n", out_configs.size());
}

auto IsPlaceholderUid(const std::string& uid) -> bool {
    return uid.starts_with(PLACEHOLDER_UID_PREFIX);
}

void SaveConfig(Config& config, const std::string& previous_uid) {
    if (config.uid.empty()) {
        config.uid = GeneratePlaceholderUid();
    }

    // the uid is the ini section, so replacing it is a delete plus a re-write.
    if (!previous_uid.empty() && previous_uid != config.uid) {
        log_write("[OWNFOIL] re-keying entry %s: %s -> %s\n", config.name.c_str(),
            IsPlaceholderUid(previous_uid) ? "placeholder" : previous_uid.c_str(), config.uid.c_str());
        DeleteConfig(previous_uid);
    }

    // no duplicate pass is needed: the uid *is* the section, so promoting a
    // hand-added entry onto an already-saved uid collapses the two.
    fs::FsNativeSd().CreateDirectoryRecursively("/config/sphaira/");
    ini_puts(config.uid.c_str(), "name", config.name.c_str(), OWNFOIL_INI_PATH);
    ini_puts(config.uid.c_str(), "local_address", config.local_address.c_str(), OWNFOIL_INI_PATH);
    ini_puts(config.uid.c_str(), "remote_address", config.remote_address.c_str(), OWNFOIL_INI_PATH);
    ini_puts(config.uid.c_str(), "user", config.user.c_str(), OWNFOIL_INI_PATH);
    ini_puts(config.uid.c_str(), "pass", config.pass.c_str(), OWNFOIL_INI_PATH);
    ini_puts(config.uid.c_str(), "resolved_url", config.resolved_url.c_str(), OWNFOIL_INI_PATH);
}

void DeleteConfig(const std::string& uid) {
    ini_puts(uid.c_str(), nullptr, nullptr, OWNFOIL_INI_PATH);
}

auto FindByUid(Configs& configs, const std::string& uid) -> Config* {
    if (uid.empty()) {
        return nullptr;
    }

    const auto it = std::find_if(configs.begin(), configs.end(), [&uid](const auto& e) {
        return e.uid == uid;
    });

    return it == configs.end() ? nullptr : &*it;
}

} // namespace sphaira::ownfoil

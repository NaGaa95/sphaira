#include "utils/devoptab_common.hpp"
#include "utils/thread.hpp"

#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "download.hpp"
#include "i18n.hpp"

#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <minIni.h>
#include <curl/curl.h>

namespace sphaira::devoptab {
namespace {

#define MOUNT_PATH "/config/sphaira/mount/"

using namespace sphaira::ui;
using namespace sphaira::devoptab::common;

// todo: support for disabling some / all mounts.
enum class DevoptabType {
    HTTP,
    FTP,
#ifdef ENABLE_DEVOPTAB_SFTP
    SFTP,
#endif
    NFS,
    SMB,
    WEBDAV,
};

struct TypeEntry {
    const char* name;
    const char* scheme;
    long port;
    DevoptabType type;
};

const TypeEntry TYPE_ENTRIES[] = {
#ifdef ENABLE_DEVOPTAB_HTTP
    {"HTTP", "http://", 80, DevoptabType::HTTP},
#endif
#ifdef ENABLE_DEVOPTAB_FTP
    {"FTP", "ftp://", 21, DevoptabType::FTP},
#endif
#ifdef ENABLE_DEVOPTAB_SFTP
    {"SFTP", "sftp://", 22, DevoptabType::SFTP},
#endif
#ifdef ENABLE_DEVOPTAB_NFS
    {"NFS", "nfs://", 2049, DevoptabType::NFS},
#endif
#ifdef ENABLE_DEVOPTAB_SMB2
    {"SMB", "smb://", 445, DevoptabType::SMB},
#endif
#ifdef ENABLE_DEVOPTAB_WEBDAV
    {"WEBDAV", "webdav://", 80, DevoptabType::WEBDAV},
#endif
};

struct TypeConfig {
    TypeEntry type;
    MountConfig config;
};
using TypeConfigs = std::vector<TypeConfig>;

auto BuildIniPathFromType(DevoptabType type) -> fs::FsPath {
    switch (type) {
        case DevoptabType::HTTP: return MOUNT_PATH "/http.ini";
        case DevoptabType::FTP: return MOUNT_PATH "/ftp.ini";
#ifdef ENABLE_DEVOPTAB_SFTP
        case DevoptabType::SFTP: return MOUNT_PATH "/sftp.ini";
#endif
        case DevoptabType::NFS: return MOUNT_PATH "/nfs.ini";
        case DevoptabType::SMB: return MOUNT_PATH "/smb.ini";
        case DevoptabType::WEBDAV: return MOUNT_PATH "/webdav.ini";
    }

    std::unreachable();
}

auto GetTypeName(const TypeConfig& type_config) -> std::string {
    char name[128]{};
    std::snprintf(name, sizeof(name), "[%s] %s", type_config.type.name, type_config.config.name.c_str());
    return name;
}

void LoadAllConfigs(TypeConfigs& out_configs) {
    out_configs.clear();

    for (const auto& e : TYPE_ENTRIES) {
        const auto ini_path = BuildIniPathFromType(e.type);

        MountConfigs configs{};
        LoadConfigsFromIni(ini_path, configs);

        for (const auto& config : configs) {
            out_configs.emplace_back(e, config);
        }
    }
}

struct DevoptabForm final : public FormSidebar {
    // create new.
    explicit DevoptabForm();
    // modify existing.
    explicit DevoptabForm(DevoptabType type, const MountConfig& config);

private:
    void SetupButtons(bool type_change);
    void UpdateSchemeURL();

private:
    DevoptabType m_type{};
    MountConfig m_config{};

    SidebarEntryTextInput* m_name{};
    SidebarEntryTextInput* m_url{};
    SidebarEntryTextInput* m_port{};
    // SidebarEntryTextInput* m_timeout{};
    SidebarEntryTextInput* m_user{};
    SidebarEntryTextInput* m_pass{};
    SidebarEntryTextInput* m_dump_path{};
};

DevoptabForm::DevoptabForm(DevoptabType type, const MountConfig& config)
: FormSidebar{"Mount Creator"_i18n}
, m_type{type}
, m_config{config} {
    SetupButtons(false);
}

DevoptabForm::DevoptabForm() : FormSidebar{"Mount Creator"_i18n} {
    if constexpr (sizeof(TYPE_ENTRIES) > 0) {
        m_type = TYPE_ENTRIES[0].type;
    }

    SetupButtons(true);
}

void DevoptabForm::UpdateSchemeURL() {
    for (const auto& e : TYPE_ENTRIES) {
        if (e.type == m_type) {
            const auto scheme_start = m_url->GetValue().find("://");
            if (scheme_start != std::string::npos) {
                m_url->SetValue(e.scheme + m_url->GetValue().substr(scheme_start + 3));
            } else if (m_url->GetValue().starts_with("://")) {
                m_url->SetValue(e.scheme + m_url->GetValue().substr(3));
            } else if (m_url->GetValue().empty()) {
                m_url->SetValue(e.scheme);
            }

            m_port->SetNumValue(e.port);
            break;
        }
    }
}

void DevoptabForm::SetupButtons(bool type_change) {
    // an empty type list crashes when drawn, so skip it entirely.
    if (type_change && sizeof(TYPE_ENTRIES) > 0) {
        SidebarEntryArray::Items items;
        s64 selected = 0;

        for (const auto& e : TYPE_ENTRIES) {
            if (e.type == m_type) {
                selected = (s64)items.size();
            }
            items.emplace_back(e.name);
        }

        this->Add<SidebarEntryArray>(
            "Type"_i18n, items, [this](s64& index) {
                m_type = TYPE_ENTRIES[index].type;
                UpdateSchemeURL();
            },
            selected,
            "Select the type of the forwarder."_i18n
        );
    }

    m_name = this->Add<SidebarEntryTextInput>(
        "Name"_i18n, m_config.name, "", "", -1, 32,
        "Set the name of the application"_i18n
    );

    m_url = this->Add<SidebarEntryTextInput>(
        "URL"_i18n, m_config.url, "", "", -1, PATH_MAX,
        "Set the URL of the application"_i18n
    );

    m_port = this->Add<SidebarEntryTextInput>(
        "Port"_i18n, m_config.port, "", "", 1, 5,
        "Optional: Set the port of the server. If left empty, the default port for the protocol will be used."_i18n
    );

    #if 0
    m_timeout = this->Add<SidebarEntryTextInput>(
        "Timeout"_i18n, m_config.timeout, "Timeout in milliseconds", 1, 5,
        "Optional: Set the timeout in seconds."_i18n
    );
    #endif

    m_user = this->Add<SidebarEntryTextInput>(
        "User"_i18n, m_config.user, "", "", -1, PATH_MAX,
        "Optional: Set the username of the application"_i18n
    );

    m_pass = this->Add<SidebarEntryTextInput>(
        "Pass"_i18n, m_config.pass, "", "", -1, PATH_MAX,
        "Optional: Set the password of the application"_i18n
    );

    m_dump_path = this->Add<SidebarEntryTextInput>(
        "Dump path"_i18n, m_config.dump_path, "", "", -1, PATH_MAX,
        "Optional: Set the dump path used when exporting games and saves."_i18n
    );

    this->Add<SidebarEntryBool>(
        "Read only"_i18n, m_config.read_only,
        i18n::get("mount_readonly_info",
            "Mount the filesystem as read only.\n\n"
            "Setting this option also hidens the mount from being show as an export option.")
    );

    this->Add<SidebarEntryBool>(
        "No stat file"_i18n, m_config.no_stat_file,
        i18n::get("filecheck_disable_info",
            "Enabling stops the file browser from checking the file size and timestamp of each file. "
            "This improves browsing performance.")
    );

    this->Add<SidebarEntryBool>(
        "No stat dir"_i18n, m_config.no_stat_dir,
        i18n::get("dircheck_disable_info",
            "Enabling stops the file browser from checking how many files and folders are in a folder. "
            "This improves browsing performance, especially for servers that has slow directory listing.")
    );

    this->Add<SidebarEntryBool>(
        "FS hidden"_i18n, m_config.fs_hidden,
        "Hide the mount from being visible in the file browser."_i18n
    );

    this->Add<SidebarEntryBool>(
        "Export hidden"_i18n, m_config.dump_hidden,
        "Hide the mount from being visible as a export option for games and saves."_i18n
    );

    // set default scheme when creating a new entry.
    if (type_change) {
        UpdateSchemeURL();
    }

    const auto callback = this->Add<SidebarEntryCallback>("Save"_i18n, [this](){
        m_config.name = m_name->GetValue();
        m_config.url = m_url->GetValue();
        m_config.user = m_user->GetValue();
        m_config.pass = m_pass->GetValue();
        m_config.dump_path = m_dump_path->GetValue();
        m_config.port = std::stoul(m_port->GetValue());
        // m_config.timeout = m_timeout->GetValue();

        const auto ini_path = BuildIniPathFromType(m_type);

        fs::FsNativeSd().CreateDirectoryRecursively(MOUNT_PATH);
        ini_puts(m_config.name.c_str(), "url", m_config.url.c_str(), ini_path);
        ini_puts(m_config.name.c_str(), "user", m_config.user.c_str(), ini_path);
        ini_puts(m_config.name.c_str(), "pass", m_config.pass.c_str(), ini_path);
        ini_puts(m_config.name.c_str(), "dump_path", m_config.dump_path.c_str(), ini_path);
        ini_putl(m_config.name.c_str(), "port", m_config.port, ini_path);
        ini_putl(m_config.name.c_str(), "timeout", m_config.timeout, ini_path);
        // todo: update minini to have put_bool.
        ini_puts(m_config.name.c_str(), "read_only", m_config.read_only ? "true" : "false", ini_path);
        ini_puts(m_config.name.c_str(), "no_stat_file", m_config.no_stat_file ? "true" : "false", ini_path);
        ini_puts(m_config.name.c_str(), "no_stat_dir", m_config.no_stat_dir ? "true" : "false", ini_path);
        ini_puts(m_config.name.c_str(), "fs_hidden", m_config.fs_hidden ? "true" : "false", ini_path);
        ini_puts(m_config.name.c_str(), "dump_hidden", m_config.dump_hidden ? "true" : "false", ini_path);

        App::Notify("Mount entry saved. Restart Sphaira to apply changes."_i18n);

        this->SetPop();
    },  "Saves the mount entry.\n\n"
        "NOTE: You must restart Sphaira for changes to take effect!"_i18n);

    // ensure that all fields are valid.
    callback->Depends([this](){
        return
            !m_name->GetValue().empty() &&
            !m_url->GetValue().empty() &&
            !m_url->GetValue().ends_with("://");
    }, "Name and URL must be set!"_i18n);
}

} // namespace

void DisplayDevoptabSideBar() {
    if (sizeof(TYPE_ENTRIES) == 0) {
        App::Notify("No mount protocols are enabled in this build."_i18n);
        return;
    }

    auto options = std::make_unique<Sidebar>("Devoptab Options"_i18n, Sidebar::Side::LEFT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<SidebarEntryCallback>("Create New Entry"_i18n, [](){
        App::Push<DevoptabForm>();
    }, "Creates a new mount option.\n\n"
        "NOTE: You must restart Sphaira for changes to take effect!"_i18n);

    options->Add<SidebarEntryCallback>("Modify Existing Entry"_i18n, [](){
        PopupList::Items items;
        TypeConfigs configs;
        LoadAllConfigs(configs);

        for (const auto& e : configs) {
            items.emplace_back(GetTypeName(e));
        }

        if (items.empty()) {
            App::Notify("No mount entries found."_i18n);
            return;
        }

        App::Push<PopupList>("Modify Entry"_i18n, items, [configs](std::optional<s64> index){
            if (!index.has_value()) {
                return;
            }

            const auto& entry = configs[index.value()];
            App::Push<DevoptabForm>(entry.type.type, entry.config);
        });
    },  "Modify an existing mount option.\n\n"
        "NOTE: You must restart Sphaira for changes to take effect!"_i18n);

    options->Add<SidebarEntryCallback>("Delete Existing Entry"_i18n, [](){
        PopupList::Items items;
        TypeConfigs configs;
        LoadAllConfigs(configs);

        for (const auto& e : configs) {
            items.emplace_back(GetTypeName(e));
        }

        if (items.empty()) {
            App::Notify("No mount entries found."_i18n);
            return;
        }

        App::Push<PopupList>("Delete Entry"_i18n, items, [configs](std::optional<s64> index){
            if (!index.has_value()) {
                return;
            }

            const auto& entry = configs[index.value()];
            const auto ini_path = BuildIniPathFromType(entry.type.type);
            ini_puts(entry.config.name.c_str(), nullptr, nullptr, ini_path);
        });
    },  "Delete an existing mount option.\n\n"
        "NOTE: You must restart Sphaira for changes to take effect!"_i18n);
}

} // namespace sphaira::devoptab

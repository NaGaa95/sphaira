#include "ui/menus/main_menu.hpp"

#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "ui/menus/homebrew.hpp"
#include "ui/menus/filebrowser.hpp"
#include "ui/menus/irs_menu.hpp"
#include "ui/menus/themezer.hpp"
#include "ui/menus/ghdl.hpp"
#include "ui/menus/usb_menu.hpp"
#include "ui/menus/ftp_menu.hpp"
#include "ui/menus/mtp_menu.hpp"
#include "ui/menus/gc_menu.hpp"
#include "ui/menus/game_menu.hpp"
#include "ui/menus/save_menu.hpp"
#include "ui/menus/appstore.hpp"
#include "ui/menus/multi_repo_settings.hpp"

#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "threaded_file_transfer.hpp"

#include <cstring>
#include <yyjson.h>
#include <iomanip>

namespace sphaira::ui::menu::main {
namespace {

constexpr const char* GITHUB_URL{"https://api.github.com/repos/ITotalJustice/sphaira/releases/latest"};
constexpr fs::FsPath CACHE_PATH{"/switch/sphaira/cache/sphaira_latest.json"};

// paths where sphaira can be installed, used when updating
constexpr const fs::FsPath SPHAIRA_PATHS[]{
    "/hbmenu.nro",
    "/switch/sphaira.nro",
    "/switch/sphaira/sphaira.nro",
};

template<typename T>
auto MiscMenuFuncGenerator(u32 flags) {
    return std::make_unique<T>(flags);
}

const MiscMenuEntry MISC_MENU_ENTRIES[] = {
    { .name = "Homebrew", .title = "Homebrew", .func = MiscMenuFuncGenerator<ui::menu::homebrew::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "The homebrew menu.\n\n"
        "Allows you to launch, delete and mount homebrew!"},

    { .name = "Appstore", .title = "Appstore", .func = MiscMenuFuncGenerator<ui::menu::appstore::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Download and update apps.\n\n"
        "Internet connection required." },

    { .name = "RepoSettings", .title = "Repo Settings", .func = MiscMenuFuncGenerator<ui::menu::multi_repo_settings::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Manage AppStore repositories.\n\n"
        "Add/remove repos, change order and switch merge mode." },

    { .name = "Games", .title = "Games", .func = MiscMenuFuncGenerator<ui::menu::game::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "View all installed games. "
        "In this menu you can launch, backup, create savedata and much more." },

    { .name = "FileBrowser", .title = "FileBrowser", .func = MiscMenuFuncGenerator<ui::menu::filebrowser::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Browse files on you SD Card. "
        "You can move, copy, delete, extract zip, create zip, upload and much more.\n\n"
        "A connected USB/HDD can be opened by mounting it in the advanced options." },

    { .name = "Saves", .title = "Saves", .func = MiscMenuFuncGenerator<ui::menu::save::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "View save data for each user. "
        "You can backup and restore saves.\n\n"
        "Experimental support for backing up system saves is possible." },

#if 0
    { .name = "Themezer", .title = "Themezer", .func = MiscMenuFuncGenerator<ui::menu::themezer::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Download themes from themezer.net. "
        "Themes are downloaded to /themes/sphaira\n"
        "To install the themes, NXThemesInstaller needs to be installed (can be downloaded via the AppStore)." },
#endif

    { .name = "GitHub", .title = "GitHub", .func = MiscMenuFuncGenerator<ui::menu::gh::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Download releases directly from GitHub. "
        "Custom entries can be added to /config/sphaira/github" },

#ifdef ENABLE_FTPSRV
    { .name = "FTP", .title = "FTP Install", .func = MiscMenuFuncGenerator<ui::menu::ftp::Menu>, .flag = MiscMenuFlag_Install, .info =
        "Install apps via FTP." },
#endif // ENABLE_FTPSRV

#ifdef ENABLE_LIBHAZE
    { .name = "MTP", .title = "MTP Install", .func = MiscMenuFuncGenerator<ui::menu::mtp::Menu>, .flag = MiscMenuFlag_Install, .info =
        "Install apps via MTP." },
#endif // ENABLE_LIBHAZE

    { .name = "USB", .title = "USB Install", .func = MiscMenuFuncGenerator<ui::menu::usb::Menu>, .flag = MiscMenuFlag_Install, .info =
        "Install apps via USB.\n\n"
        "A USB client is required on PC." },

    { .name = "GameCard", .title = "GameCard", .func = MiscMenuFuncGenerator<ui::menu::gc::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "View info on the inserted Game Card (GC). "
        "You can backup and install the inserted GC. "
        "To swap GC's, simply remove the old GC and insert the new one. "
        "You do not need to exit the menu." },

    { .name = "IRS", .title = "IRS (Infrared Joycon Camera)", .func = MiscMenuFuncGenerator<ui::menu::irs::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "InfraRed Sensor (IRS) is the small camera found on right JoyCon." },
};

auto InstallUpdate(ProgressBox* pbox, const std::string url, const std::string version) -> Result {
    static fs::FsPath zip_out{"/switch/sphaira/cache/update.zip"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    // 1. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", version));
        log_write("starting download: %s\n", url.c_str());

        const auto result = curl::Api().ToFile(
            curl::Url{url},
            curl::Path{zip_out},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        R_UNLESS(result.success, Result_MainFailedToDownloadUpdate);
    }

    ON_SCOPE_EXIT(fs.DeleteFile(zip_out));

    // 2. extract the zip
    if (!pbox->ShouldExit()) {
        const auto exe_path = App::GetExePath();
        bool found_exe{};

        R_TRY(thread::TransferUnzipAll(pbox, zip_out, &fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
            if (std::strstr(path, "sphaira.nro")) {
                path = exe_path;
                found_exe = true;
            }
            return true;
        }));

        // check if we have sphaira installed in other locations and update them.
        if (found_exe) {
            for (auto& path : SPHAIRA_PATHS) {
                log_write("[UPD] checking path: %s\n", path.s);
                // skip if we already updated this path.
                if (exe_path == path) {
                    log_write("[UPD] skipped as already updated\n");
                    continue;
                }

                // check that this is really sphaira.
                log_write("[UPD] checking nacp\n");
                NacpStruct nacp;
                if (R_SUCCEEDED(nro_get_nacp(path, nacp)) && !std::strcmp(nacp.lang[0].name, "sphaira")) {
                    log_write("[UPD] found, updating\n");
                    pbox->NewTransfer(path);
                    R_TRY(pbox->CopyFile(&fs, exe_path, path));
                }
            }
        }
    }

    log_write("finished update :)\n");
    R_SUCCEED();
}

auto CreateCenterMenu(std::string& name_out) -> std::unique_ptr<MenuBase> {
    const auto name = App::GetApp()->m_center_menu.Get();

    for (auto& e : GetMenuMenuEntries()) {
        if (e.name == name) {
            name_out = name;
            return e.func(MenuFlag_Tab);
        }
    }

    name_out = "Homebrew";
    return std::make_unique<ui::menu::homebrew::Menu>(MenuFlag_Tab);
}

auto CreateLeftSideMenu(std::string_view center_name, std::string& name_out) -> std::unique_ptr<MenuBase> {
    const auto name = App::GetApp()->m_left_menu.Get();

    // handle if the user tries to mount the same menu twice.
    if (name == center_name) {
        // check if we can mount the default.
        if (center_name != "FileBrowser") {
            return std::make_unique<ui::menu::filebrowser::Menu>(MenuFlag_Tab);
        } else {
            // otherwise, fallback to center default.
            return std::make_unique<ui::menu::homebrew::Menu>(MenuFlag_Tab);
        }
    }

    for (auto& e : GetMenuMenuEntries()) {
        if (e.name == name) {
            name_out = name;
            return e.func(MenuFlag_Tab);
        }
    }

    name_out = "FileBrowser";
    return std::make_unique<ui::menu::filebrowser::Menu>(MenuFlag_Tab);
}

// todo: handle center / left menu being the same.
auto CreateRightSideMenu(std::string_view left_name) -> std::unique_ptr<MenuBase> {
    const auto name = App::GetApp()->m_right_menu.Get();

    // handle if the user tries to mount the same menu twice.
    if (name == left_name) {
        // check if we can mount the default.
        if (left_name != "AppStore") {
            return std::make_unique<ui::menu::appstore::Menu>(MenuFlag_Tab);
        } else {
            // otherwise, fallback to left side default.
            return std::make_unique<ui::menu::filebrowser::Menu>(MenuFlag_Tab);
        }
    }

    for (auto& e : GetMenuMenuEntries()) {
        if (e.name == name) {
            return e.func(MenuFlag_Tab);
        }
    }

    return std::make_unique<ui::menu::appstore::Menu>(MenuFlag_Tab);
}

} // namespace

auto GetMenuMenuEntries() -> std::span<const MiscMenuEntry> {
    return MISC_MENU_ENTRIES;
}

MainMenu::MainMenu() {
    curl::Api().ToFileAsync(
        curl::Url{GITHUB_URL},
        curl::Path{CACHE_PATH},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::Header{
            { "Accept", "application/vnd.github+json" },
        },
        curl::OnComplete{[this](auto& result){
            log_write("inside github download\n");
            m_update_state = UpdateState::Error;
            ON_SCOPE_EXIT( log_write("update status: %u\n", (u8)m_update_state) );

            if (!result.success) {
                return false;
            }

            auto json = yyjson_read_file(CACHE_PATH, YYJSON_READ_NOFLAG, nullptr, nullptr);
            R_UNLESS(json, false);
            ON_SCOPE_EXIT(yyjson_doc_free(json));

            auto root = yyjson_doc_get_root(json);
            R_UNLESS(root, false);

            auto tag_key = yyjson_obj_get(root, "tag_name");
            R_UNLESS(tag_key, false);

            const auto version = yyjson_get_str(tag_key);
            R_UNLESS(version, false);
            if (!App::IsVersionNewer(APP_VERSION, version)) {
                m_update_state = UpdateState::None;
                return true;
            }

            auto body_key = yyjson_obj_get(root, "body");
            R_UNLESS(body_key, false);

            const auto body = yyjson_get_str(body_key);
            R_UNLESS(body, false);

            auto assets = yyjson_obj_get(root, "assets");
            R_UNLESS(assets, false);

            auto idx0 = yyjson_arr_get(assets, 0);
            R_UNLESS(idx0, false);

            auto url_key = yyjson_obj_get(idx0, "browser_download_url");
            R_UNLESS(url_key, false);

            const auto url = yyjson_get_str(url_key);
            R_UNLESS(url, false);

            m_update_version = version;
            m_update_url = url;
            m_update_description = body;
            m_update_state = UpdateState::Update;
            log_write("found url: %s\n", url);
            log_write("found body: %s\n", body);
            App::Notify("Update avaliable: "_i18n + m_update_version);
            App::Notify("Download via the Network options!"_i18n);

            return true;
        }
    });

    this->SetActions(
        std::make_pair(Button::START, Action{App::Exit}),
        std::make_pair(Button::SELECT, Action{App::DisplayMenuOptions}),
        std::make_pair(Button::Y, Action{"Menu"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("Menu Options"_i18n, "v" APP_DISPLAY_VERSION, Sidebar::Side::LEFT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            SidebarEntryArray::Items language_items;
            language_items.push_back("Auto"_i18n);
            language_items.push_back("English"_i18n);
            language_items.push_back("Japanese"_i18n);
            language_items.push_back("French"_i18n);
            language_items.push_back("German"_i18n);
            language_items.push_back("Italian"_i18n);
            language_items.push_back("Spanish"_i18n);
            language_items.push_back("Chinese (Simplified)"_i18n);
            language_items.push_back("Korean"_i18n);
            language_items.push_back("Dutch"_i18n);
            language_items.push_back("Portuguese"_i18n);
            language_items.push_back("Russian"_i18n);
            language_items.push_back("Chinese (Traditional)"_i18n);
            language_items.push_back("Swedish"_i18n);
            language_items.push_back("Vietnamese"_i18n);
            language_items.push_back("Ukrainian"_i18n);

            // build menus info.
            std::string menus_info = "Launch one of Sphaira's menus:\n"_i18n;
            for (auto& e : GetMenuMenuEntries()) {
                if (e.name == App::GetApp()->m_left_menu.Get()) {
                    continue;
                } else if (e.name == App::GetApp()->m_right_menu.Get()) {
                    continue;
                }

                menus_info += "- " + i18n::get(e.title) + "\n";
            }
            menus_info += "\nYou can change the left/right menu in the Advanced Options."_i18n;

            options->Add<SidebarEntryCallback>("Menus"_i18n, [](){
                App::DisplayMenuOptions();
            },  menus_info);

            options->Add<SidebarEntryCallback>("Network"_i18n, [this](){
                auto options = std::make_unique<Sidebar>("Network Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(std::move(options)));

                if (m_update_state == UpdateState::Update) {
                    options->Add<SidebarEntryCallback>("Download update: "_i18n + m_update_version, [this](){
                        App::Push<ProgressBox>(0, "Downloading "_i18n, "Sphaira v" + m_update_version, [this](auto pbox) -> Result {
                            return InstallUpdate(pbox, m_update_url, m_update_version);
                        }, [this](Result rc){
                            App::PushErrorBox(rc, "Failed to download update"_i18n);

                            if (R_SUCCEEDED(rc)) {
                                m_update_state = UpdateState::None;
                                App::Notify(i18n::Reorder("Updated to ", m_update_version));
                                App::Push<OptionBox>(
                                    "Press OK to restart Sphaira"_i18n, "OK"_i18n, [](auto){
                                        App::ExitRestart();
                                    }
                                );
                            }
                        });
                    });
                }

                options->Add<SidebarEntryCallback>("FTP"_i18n, [](){ App::DisplayFtpOptions(); },
                    i18n::get("ftp_settings_info",
                        "Enable / modify the FTP server settings such as port, user/pass and the folders that are shown.\n\n"
                        "NOTE: Changing any of the options will automatically restart the FTP server when exiting the options menu.")
                );

                options->Add<SidebarEntryCallback>("MTP"_i18n, [](){ App::DisplayMtpOptions(); },
                    i18n::get("mtp_settings_info",
                        "Enable / modify the MTP responder settings such as the folders that are shown.\n\n"
                        "NOTE: Changing any of the options will automatically restart the MTP server when exiting the options menu.")
                );

                options->Add<SidebarEntryCallback>("HDD"_i18n, [](){
                    App::DisplayHddOptions();
                },  "Enable / modify the HDD mount options."_i18n);

                options->Add<SidebarEntryBool>("NXlink"_i18n, App::GetNxlinkEnable(), [](bool& enable){
                    App::SetNxlinkEnable(enable);
                },  i18n::get("nxlink_enable_info",
                        "Enable NXlink server to run in the background. "
                        "NXlink is used to send .nro's from PC to the switch\n\n"
                        "If you are not a developer, you can disable this option."));

            },  i18n::get("nxlink_toggle_info",
                    "Toggle FTP, MTP, HDD and NXlink\n\n"
                    "If Sphaira has a update available, you can download it from this menu"));

            options->Add<SidebarEntryCallback>("Theme"_i18n, [](){
                App::DisplayThemeOptions();
            }, "Customise the look of Sphaira by changing the theme"_i18n);

            options->Add<SidebarEntryCallback>("Repo Settings"_i18n, [](){
                App::Push<ui::menu::multi_repo_settings::Menu>(0);
            }, "Manage AppStore repositories and merge mode."_i18n);

            options->Add<SidebarEntryArray>("Language"_i18n, language_items, [](s64& index_out){
                App::SetLanguage(index_out);
            }, (s64)App::GetLanguage(),
                i18n::get("translation_info",
                    "Change the language.\n\n"
                    "If your language isn't found, or translations are missing, please consider opening a PR at "
                    "github.com/ITotalJustice/sphaira"));

            options->Add<SidebarEntryCallback>("Advanced Options"_i18n, [](){
                App::DisplayAdvancedOptions();
            },  i18n::get("advanced_options_info",
                    "Change the advanced options. "
                    "Please view the info boxes to better understand each option."));
        }}
    ));

    std::string center_name;
    m_centre_menu = CreateCenterMenu(center_name);
    m_current_menu = m_centre_menu.get();

    std::string left_side_name;
    m_left_menu = CreateLeftSideMenu(center_name, left_side_name);

    m_right_menu = CreateRightSideMenu(left_side_name);

    AddOnLRPress();

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }
}

MainMenu::~MainMenu() {

}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
    m_current_menu->Update(controller, touch);
}

void MainMenu::Draw(NVGcontext* vg, Theme* theme) {
    m_current_menu->Draw(vg, theme);
}

void MainMenu::OnFocusGained() {
    Widget::OnFocusGained();
    m_current_menu->OnFocusGained();
}

void MainMenu::OnFocusLost() {
    Widget::OnFocusLost();
    m_current_menu->OnFocusLost();
}

void MainMenu::OnLRPress(MenuBase* menu, Button b) {
    m_current_menu->OnFocusLost();
    if (m_current_menu == m_centre_menu.get()) {
        m_current_menu = menu;
        RemoveAction(b);
    } else {
        m_current_menu = m_centre_menu.get();
    }

    AddOnLRPress();
    m_current_menu->OnFocusGained();

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }
}

void MainMenu::AddOnLRPress() {
    if (m_current_menu != m_left_menu.get()) {
        const auto label = m_current_menu == m_centre_menu.get() ? m_left_menu->GetShortTitle() : m_centre_menu->GetShortTitle();
        SetAction(Button::L, Action{i18n::get(label), [this]{
            OnLRPress(m_left_menu.get(), Button::L);
        }});
    }

    if (m_current_menu != m_right_menu.get()) {
        const auto label = m_current_menu == m_centre_menu.get() ? m_right_menu->GetShortTitle() : m_centre_menu->GetShortTitle();
        SetAction(Button::R, Action{i18n::get(label), [this]{
            OnLRPress(m_right_menu.get(), Button::R);
        }});
    }
}

} // namespace sphaira::ui::menu::main

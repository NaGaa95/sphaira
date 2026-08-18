#include "ui/menus/main_menu.hpp"

#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"
#include "ui/scrollable_text.hpp"
#include "ui/nvg_util.hpp"

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
#include "ui/menus/storage_menu.hpp"
#include "ui/menus/appstore.hpp"

#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "threaded_file_transfer.hpp"
#include "nacp_compat.hpp"

#include <cstring>
#include <yyjson.h>
#include <iomanip>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace sphaira::ui::menu::main {
namespace {

constexpr const char* GITHUB_URL{"https://api.github.com/repos/NaGaa95/sphaira/releases/latest"};
constexpr fs::FsPath CACHE_PATH{"/switch/sphaira/cache/sphaira_latest.json"};

// paths where sphaira can be installed, used when updating
constexpr const fs::FsPath SPHAIRA_PATHS[]{
    "/hbmenu.nro",
    "/switch/sphaira.nro",
    "/switch/sphaira/sphaira.nro",
};

auto Trim(std::string_view value) -> std::string_view {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

auto Lower(std::string_view value) -> std::string {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](char c){
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

auto EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) -> bool {
    return value.size() >= suffix.size()
        && !strncasecmp(value.data() + value.size() - suffix.size(), suffix.data(), suffix.size());
}

auto IsSphairaNacp(const NacpStruct& nacp) -> bool {
    for (const auto& language : NacpLanguageEntries(nacp)) {
        if (!strncasecmp(language.name, "sphaira", 7)) {
            return true;
        }
    }
    return false;
}

auto ReadNotes(yyjson_val* value) -> std::string {
    if (!value) {
        return {};
    }
    if (yyjson_is_str(value)) {
        const auto text = yyjson_get_str(value);
        return text ? text : "";
    }
    if (!yyjson_is_arr(value)) {
        return {};
    }

    std::string notes;
    size_t index{}, count{};
    yyjson_val* item{};
    yyjson_arr_foreach(value, index, count, item) {
        if (const auto text = yyjson_get_str(item); text && *text) {
            if (!notes.empty()) {
                notes += '\n';
            }
            notes += "- ";
            notes += text;
        }
    }
    return notes;
}

auto ReadLocalizedNotes(yyjson_val* object, std::string_view language) -> std::string {
    if (!object || !yyjson_is_obj(object)) {
        return {};
    }

    auto value = yyjson_obj_getn(object, language.data(), language.size());
    if (!value && language.find('-') != std::string_view::npos) {
        const auto base = language.substr(0, language.find('-'));
        value = yyjson_obj_getn(object, base.data(), base.size());
    }
    if (!value) {
        value = yyjson_obj_get(object, "en");
    }
    return ReadNotes(value);
}

// Supports the language-keyed changelog format used by the supplied NRO. It
// also accepts an object containing language keys directly, which makes the
// same format convenient for a GitHub release body.
auto ParseLocalizedNotesJson(std::string_view input, std::string_view version, std::string_view language) -> std::string {
    const auto document = yyjson_read(input.data(), input.size(), YYJSON_READ_NOFLAG);
    if (!document) {
        return {};
    }
    ON_SCOPE_EXIT(yyjson_doc_free(document));

    const auto root = yyjson_doc_get_root(document);
    if (!root || !yyjson_is_obj(root)) {
        return {};
    }

    const auto versions = yyjson_obj_get(root, "versions");
    if (versions && yyjson_is_arr(versions)) {
        const auto wanted = App::GetVersionFromString(std::string{version}.c_str());
        size_t index{}, count{};
        yyjson_val* entry{};
        yyjson_arr_foreach(versions, index, count, entry) {
            const auto version_value = yyjson_obj_get(entry, "version");
            const auto entry_version = version_value ? yyjson_get_str(version_value) : nullptr;
            if (!entry_version) {
                continue;
            }
            const auto parsed = App::GetVersionFromString(entry_version);
            if ((wanted && parsed == wanted) || (!wanted && version == entry_version)) {
                return ReadLocalizedNotes(entry, language);
            }
        }
        return {};
    }

    return ReadLocalizedNotes(root, language);
}

auto ParseLanguageHeading(std::string_view line) -> std::optional<std::string> {
    line = Trim(line);
    if (line.starts_with("<!--") && line.ends_with("-->")) {
        line.remove_prefix(4);
        line.remove_suffix(3);
        line = Trim(line);
        if (const auto colon = line.find(':'); colon != std::string_view::npos
            && Lower(Trim(line.substr(0, colon))) == "lang") {
            line = Trim(line.substr(colon + 1));
        } else {
            return {};
        }
    } else {
        while (!line.empty() && line.front() == '#') {
            line.remove_prefix(1);
        }
        line = Trim(line);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            line.remove_prefix(1);
            line.remove_suffix(1);
        }
    }

    auto code = Lower(Trim(line));
    static constexpr std::array<std::string_view, 15> languages{
        "en", "ja", "fr", "de", "it", "es", "zh-cn", "ko", "nl",
        "pt", "ru", "zh-tw", "se", "vi", "uk",
    };
    if (std::ranges::find(languages, code) == languages.end()) {
        return {};
    }
    if (code == "zh-cn") return "zh-CN";
    if (code == "zh-tw") return "zh-TW";
    return code;
}

auto LocalizeReleaseNotes(std::string_view notes, std::string_view version) -> std::string {
    if (notes.empty()) {
        return "No changelog available."_i18n;
    }

    const auto language = i18n::GetLanguageCode();
    if (const auto json_notes = ParseLocalizedNotesJson(notes, version, language); !json_notes.empty()) {
        return json_notes;
    }

    // Optional Markdown format:
    //   ## [en]
    //   English notes
    //   ## [fr]
    //   French notes
    // Automatic language uses the resolved console language here.
    std::string selected;
    std::string english;
    std::string active_language;
    bool found_language_section{};
    for (std::size_t offset{}; offset <= notes.size();) {
        const auto end = notes.find('\n', offset);
        const auto line = notes.substr(offset, end == std::string_view::npos ? notes.size() - offset : end - offset);
        if (const auto heading = ParseLanguageHeading(line)) {
            active_language = *heading;
            found_language_section = true;
        } else if (!active_language.empty()) {
            auto& output = active_language == language ? selected : (active_language == "en" ? english : selected);
            if (active_language == language || active_language == "en") {
                output.append(line);
                output.push_back('\n');
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }

    if (found_language_section) {
        auto result = !selected.empty() ? std::move(selected) : std::move(english);
        while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
            result.pop_back();
        }
        if (!result.empty()) {
            return result;
        }
    }
    return std::string{notes};
}

class UpdateInfoMenu final : public MenuBase {
public:
    UpdateInfoMenu(std::string version, std::string notes, bool update_available, std::function<void()> on_update)
    : MenuBase{"Update Checker"_i18n, MenuFlag_None}
    , m_version{std::move(version)}
    , m_update_available{update_available}
    , m_on_update{std::move(on_update)} {
        SetAction(Button::B, Action{"Back"_i18n, [this](){ SetPop(); }});
        if (m_update_available) {
            SetAction(Button::A, Action{"Update Now"_i18n, [this](){
                if (m_on_update) {
                    m_on_update();
                }
            }});
        }
        m_notes = std::make_unique<ScrollableText>(notes, 0.f, 205.f, 385.f, 1000.f, 20.f);
        SetTitleSubHeading(m_version.empty() ? "v" APP_DISPLAY_VERSION : m_version);
    }

    auto GetShortTitle() const -> const char* override { return "Update"; }

    void Update(Controller* controller, TouchInfo* touch) override {
        MenuBase::Update(controller, touch);
        m_notes->Update(controller, touch);
    }

    void Draw(NVGcontext* vg, Theme* theme) override {
        MenuBase::Draw(vg, theme);
        const auto status = m_update_available ? "New version available"_i18n : "You are already on the latest version."_i18n;
        gfx::drawText(vg, 110.f, 125.f, 25.f, theme->GetColour(ThemeEntryID_TEXT), status.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        gfx::drawText(vg, 110.f, 166.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "Changelog"_i18n.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        gfx::drawRect(vg, 110.f, 187.f, 1040.f, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        m_notes->Draw(vg, theme);
    }

private:
    std::string m_version;
    bool m_update_available{};
    std::function<void()> m_on_update;
    std::unique_ptr<ScrollableText> m_notes;
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

    { .name = "Games", .title = "Games", .func = MiscMenuFuncGenerator<ui::menu::game::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "View all installed games. "
        "In this menu you can launch, backup, create savedata and much more." },

    { .name = "Storage", .title = "Data Management", .func = MiscMenuFuncGenerator<ui::menu::storage::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "See how much space each game uses on the microSD card and in system memory. "
        "Games are listed largest first, so it's easy to find what to remove." },

    { .name = "FileBrowser", .title = "FileBrowser", .func = MiscMenuFuncGenerator<ui::menu::filebrowser::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Browse files on you SD Card. "
        "You can move, copy, delete, extract zip, create zip, upload and much more.\n\n"
        "A connected USB/HDD can be opened by mounting it in the advanced options." },

    { .name = "Saves", .title = "Saves", .func = MiscMenuFuncGenerator<ui::menu::save::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "View save data for each user. "
        "You can backup and restore saves.\n\n"
        "Experimental support for backing up system saves is possible." },

    { .name = "Themezer", .title = "Themezer", .func = MiscMenuFuncGenerator<ui::menu::themezer::Menu>, .flag = MiscMenuFlag_Shortcut, .info =
        "Download themes from themezer.net. "
        "Themes are downloaded to /themes/sphaira\n"
        "To install the themes, NXThemesInstaller needs to be installed (can be downloaded via the AppStore)." },

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
    static const fs::FsPath zip_out{"/switch/sphaira/cache/update.zip"};
    static const fs::FsPath staged_nro{"/switch/sphaira/cache/update.nro.tmp"};
    static const fs::FsPath backup_nro{"/switch/sphaira/cache/update.nro.backup"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    const auto exe_path = App::GetExePath();
    const bool direct_nro = EndsWithCaseInsensitive(url, ".nro");
    const auto& download_path = direct_nro ? staged_nro : zip_out;
    fs.DeleteFile(staged_nro);
    fs.DeleteFile(zip_out);
    fs.DeleteFile(backup_nro);
    ON_SCOPE_EXIT({
        fs.DeleteFile(staged_nro);
        fs.DeleteFile(zip_out);
    });

    // 1. Download to a staging path. Never overwrite the running executable
    // before the complete update has been validated.
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", version));
        log_write("starting download: %s\n", url.c_str());

        const auto result = curl::Api().ToFile(
            curl::Url{url},
            curl::Path{download_path},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        R_UNLESS(result.success, Result_MainFailedToDownloadUpdate);
    }
    R_TRY(pbox->ShouldExitResult());

    // 2. Extract only sphaira.nro from archives. Other release files cannot
    // write to arbitrary SD-card paths during self-update.
    bool found_exe{direct_nro};
    if (!direct_nro) {
        R_TRY(thread::TransferUnzipAll(pbox, zip_out, &fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
            if (name.ends_with("sphaira.nro")) {
                path = staged_nro;
                found_exe = true;
                return true;
            }
            return false;
        }));
    }
    R_TRY(pbox->ShouldExitResult());
    R_UNLESS(found_exe, Result_MainFailedToDownloadUpdate);

    // 3. Validate the embedded NACP and expected release version.
    NacpStruct staged_nacp{};
    R_UNLESS(R_SUCCEEDED(nro_get_nacp(staged_nro, staged_nacp)), Result_MainDownloadedUpdateInvalid);
    R_UNLESS(IsSphairaNacp(staged_nacp), Result_MainDownloadedUpdateInvalid);
    const auto expected_version = App::GetVersionFromString(version.c_str());
    const auto staged_version = App::GetVersionFromString(staged_nacp.display_version);
    R_UNLESS(staged_version && (!expected_version || staged_version == expected_version), Result_MainDownloadedUpdateInvalid);

    // 4. Keep a recovery copy while replacing the running NRO.
    pbox->NewTransfer("Backing up current version"_i18n);
    R_TRY(pbox->CopyFile(&fs, exe_path, backup_nro));
    pbox->NewTransfer("Installing validated update"_i18n);
    const auto install_result = pbox->CopyFile(&fs, staged_nro, exe_path);
    if (R_FAILED(install_result)) {
        log_write("[UPD] install failed, restoring backup\n");
        const auto restore_result = pbox->CopyFile(&fs, backup_nro, exe_path);
        if (R_FAILED(restore_result)) {
            log_write("[UPD] backup restore failed: 0x%X\n", R_VALUE(restore_result));
        }
        R_THROW(Result_MainFailedToInstallUpdate);
    }

    // Read the file back from its final location before deleting the backup.
    // This catches interrupted or corrupted SD-card writes while recovery is
    // still possible.
    NacpStruct installed_nacp{};
    const auto installed_nacp_result = nro_get_nacp(exe_path, installed_nacp);
    const auto installed_version = R_SUCCEEDED(installed_nacp_result)
        ? App::GetVersionFromString(installed_nacp.display_version)
        : 0;
    if (R_FAILED(installed_nacp_result) || !IsSphairaNacp(installed_nacp) || installed_version != staged_version) {
        log_write("[UPD] installed update failed verification, restoring backup\n");
        const auto restore_result = pbox->CopyFile(&fs, backup_nro, exe_path);
        if (R_FAILED(restore_result)) {
            log_write("[UPD] backup restore failed: 0x%X\n", R_VALUE(restore_result));
        }
        R_THROW(Result_MainFailedToInstallUpdate);
    }
    fs.DeleteFile(backup_nro);

    // 5. Update known secondary installations, but only when their NACP
    // identifies them as Sphaira.
    for (const auto& path : SPHAIRA_PATHS) {
        if (exe_path == path) {
            continue;
        }
        NacpStruct nacp{};
        if (R_SUCCEEDED(nro_get_nacp(path, nacp))) {
            if (IsSphairaNacp(nacp)) {
                pbox->NewTransfer(path);
                const auto secondary_result = pbox->CopyFile(&fs, staged_nro, path);
                if (R_FAILED(secondary_result)) {
                    // The active installation is already valid. A stale or
                    // locked secondary copy must not turn that success into a
                    // failed update.
                    log_write("[UPD] failed to update secondary path %s: 0x%X\n", path.s, R_VALUE(secondary_result));
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
    CheckForUpdates(false);

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

            },  i18n::get("network_options_info",
                    "Toggle FTP, MTP, HDD and NXlink"));

            options->Add<SidebarEntryCallback>("Theme"_i18n, [](){
                App::DisplayThemeOptions();
            }, "Customise the look of Sphaira by changing the theme"_i18n);

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

            // Keep update controls in the main options list, directly below
            // Advanced Options (not inside its submenu).
            if (m_update_state == UpdateState::Update) {
                options->Add<SidebarEntryCallback>("Update available: "_i18n + m_update_version, [this](){
                    ShowUpdateInfo();
                }, true, "View the changelog and install the available update."_i18n);
            }

            options->Add<SidebarEntryCallback>("Check for Updates"_i18n, [this](){
                CheckForUpdates(true);
            }, true, "Check for Sphaira updates and view the changelog."_i18n);
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

void MainMenu::CheckForUpdates(bool show_result) {
    const auto generation = ++m_update_generation;
    m_update_state = UpdateState::Pending;
    if (show_result) {
        App::Notify("Checking for updates"_i18n);
    }

    const auto queued = curl::Api().ToFileAsync(
        curl::Url{GITHUB_URL},
        curl::Path{CACHE_PATH},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::Header{
            { "Accept", "application/vnd.github+json" },
            { "X-GitHub-Api-Version", "2022-11-28" },
        },
        curl::OnComplete{[this, generation, show_result](auto& result){
            if (generation != m_update_generation) {
                return;
            }

            const bool parsed = result.success && ParseUpdateMetadata(CACHE_PATH);
            if (!parsed) {
                m_update_state = UpdateState::Error;
            }
            log_write("update status: %u http: %ld\n", static_cast<u8>(m_update_state), result.code);

            if (show_result) {
                ShowUpdateInfo();
            } else if (m_update_state == UpdateState::Update) {
                App::Notify("Update available: "_i18n + m_update_version);
                App::Notify("Open Menu Options to view the changelog."_i18n);
            }
        }}
    );

    if (!queued) {
        m_update_state = UpdateState::Error;
        if (show_result) {
            ShowUpdateInfo();
        }
    }
}

auto MainMenu::ParseUpdateMetadata(const fs::FsPath& path) -> bool {
    auto document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    if (!document) {
        return false;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(document));

    const auto root = yyjson_doc_get_root(document);
    if (!root || !yyjson_is_obj(root)) {
        return false;
    }

    const auto tag_value = yyjson_obj_get(root, "tag_name");
    const auto version = tag_value ? yyjson_get_str(tag_value) : nullptr;
    if (!version || !*version) {
        return false;
    }

    std::string body;
    if (const auto body_value = yyjson_obj_get(root, "body"); body_value && yyjson_is_str(body_value)) {
        if (const auto value = yyjson_get_str(body_value)) {
            body = value;
        }
    }

    m_update_version = version;
    m_update_description = LocalizeReleaseNotes(body, m_update_version);
    m_update_url.clear();

    if (!App::IsVersionNewer(APP_VERSION, version)) {
        m_update_state = UpdateState::None;
        return true;
    }

    const auto assets = yyjson_obj_get(root, "assets");
    if (!assets || !yyjson_is_arr(assets)) {
        return false;
    }

    int best_score{};
    size_t index{}, count{};
    yyjson_val* asset{};
    yyjson_arr_foreach(assets, index, count, asset) {
        if (!asset || !yyjson_is_obj(asset)) {
            continue;
        }
        const auto name_value = yyjson_obj_get(asset, "name");
        const auto url_value = yyjson_obj_get(asset, "browser_download_url");
        const auto name = name_value ? yyjson_get_str(name_value) : nullptr;
        const auto url = url_value ? yyjson_get_str(url_value) : nullptr;
        if (!name || !url || !*url) {
            continue;
        }

        const auto lower_name = Lower(name);
        const bool sphaira_asset = lower_name.find("sphaira") != std::string::npos;
        int score{};
        if (EndsWithCaseInsensitive(name, ".nro")) {
            score = sphaira_asset ? 4 : 3;
        } else if (EndsWithCaseInsensitive(name, ".zip")) {
            score = sphaira_asset ? 2 : 1;
        }
        if (score > best_score) {
            best_score = score;
            m_update_url = url;
        }
    }

    if (m_update_url.empty()) {
        return false;
    }

    m_update_state = UpdateState::Update;
    log_write("found update version: %s url: %s\n", m_update_version.c_str(), m_update_url.c_str());
    return true;
}

void MainMenu::ShowUpdateInfo() {
    if (m_update_state == UpdateState::Pending) {
        App::Notify("Checking for updates"_i18n);
        return;
    }
    if (m_update_state == UpdateState::Error) {
        App::Push<OptionBox>("Failed to check for updates"_i18n, "OK"_i18n);
        return;
    }

    App::Push<UpdateInfoMenu>(
        m_update_version,
        m_update_description.empty() ? "No changelog available."_i18n : m_update_description,
        m_update_state == UpdateState::Update,
        [this](){ StartUpdateInstall(); }
    );
}

void MainMenu::StartUpdateInstall() {
    if (m_update_state != UpdateState::Update || m_update_url.empty()) {
        return;
    }

    App::Push<ProgressBox>(0, "Downloading "_i18n, "Sphaira " + m_update_version, [this](auto pbox) -> Result {
        return InstallUpdate(pbox, m_update_url, m_update_version);
    }, [this](Result rc){
        App::PushErrorBox(rc, "Failed to install update"_i18n);
        if (R_SUCCEEDED(rc)) {
            m_update_state = UpdateState::None;
            App::Notify("Update complete. Restarting Sphaira."_i18n);
            App::Push<OptionBox>(
                "Press OK to restart Sphaira"_i18n, "OK"_i18n, [](auto){
                    App::ExitRestart();
                }
            );
        }
    });
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

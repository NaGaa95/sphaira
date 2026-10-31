#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"

#include "ui/menus/homebrew.hpp"
#include "ui/forwarder_editor.hpp"
#include "ui/menus/filebrowser.hpp"

#include "ui/sidebar.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/nvg_util.hpp"

#include "utils/devoptab.hpp"
#include "utils/profile.hpp"
#include "utils/core.hpp"

#include "owo.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "image.hpp"

#include <minIni.h>
#include <utility>
#include <algorithm>
#include <optional>

namespace sphaira::ui::menu::homebrew {
namespace {

Menu* g_menu{};
std::atomic_bool g_change_signalled{};
constexpr const char* SEARCH_PATHS_INI_SECTION = "homebrew_paths";

auto NormalizeSearchPath(const fs::FsPath& input) -> std::optional<fs::FsPath> {
    const std::string_view path{input};
    if (path.empty() || path.front() != '/' || path.size() >= PATH_MAX) {
        return std::nullopt;
    }

    std::string normalized;
    normalized.reserve(path.size());
    for (const auto c : path) {
        if (c != '/' || normalized.empty() || normalized.back() != '/') {
            normalized.push_back(c);
        }
    }
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    std::string_view components{normalized};
    while (!components.empty()) {
        const auto slash = components.find('/');
        const auto component = components.substr(0, slash);
        if (component == "." || component == "..") {
            return std::nullopt;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        components.remove_prefix(slash + 1);
    }

    return fs::FsPath{normalized};
}

auto LoadSearchPaths() -> std::vector<fs::FsPath> {
    std::vector<fs::FsPath> paths;
    ini_browse([](const mTCHAR* section, const mTCHAR*, const mTCHAR* value, void* user_data) -> int {
        if (std::strcmp(section, SEARCH_PATHS_INI_SECTION) || !value) {
            return 1;
        }

        auto& paths = *static_cast<std::vector<fs::FsPath>*>(user_data);
        const auto path = NormalizeSearchPath(value);
        if (!path || *path == "/" || *path == "/switch") {
            return 1;
        }

        if (std::ranges::none_of(paths, [&path](const auto& entry){ return entry == *path; })) {
            paths.emplace_back(*path);
        }
        return 1;
    }, &paths, App::CONFIG_PATH);
    return paths;
}

auto SaveSearchPaths(const std::vector<fs::FsPath>& paths) -> bool {
    if (!ini_puts(SEARCH_PATHS_INI_SECTION, nullptr, nullptr, App::CONFIG_PATH)) {
        return false;
    }

    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto key = "path_" + std::to_string(i);
        if (!ini_puts(SEARCH_PATHS_INI_SECTION, key.c_str(), paths[i], App::CONFIG_PATH)) {
            return false;
        }
    }
    return true;
}

void AppendUniqueEntries(std::vector<NroEntry>& entries, std::vector<NroEntry>& scanned) {
    for (auto& entry : scanned) {
        const auto found = std::ranges::any_of(entries, [&entry](const auto& current){
            return current.path == entry.path;
        });
        if (!found) {
            entries.emplace_back(std::move(entry));
        }
    }
}

auto GenerateStarPath(const fs::FsPath& nro_path) -> fs::FsPath {
    fs::FsPath out{};
    const auto dilem = std::strrchr(nro_path.s, '/');
    std::snprintf(out, sizeof(out), "%.*s.%s.star", int(dilem - nro_path.s + 1), nro_path.s, dilem + 1);
    return out;
}

void FreeEntry(NVGcontext* vg, NroEntry& e) {
    nvgDeleteImage(vg, e.image);
    e.image = 0;
}

} // namespace

void SignalChange() {
    g_change_signalled = true;
}

auto IsSearchPath(const fs::FsPath& path) -> bool {
    const auto normalized = NormalizeSearchPath(path);
    if (!normalized) {
        return false;
    }
    if (*normalized == "/switch") {
        return true;
    }

    const auto paths = LoadSearchPaths();
    return std::ranges::any_of(paths, [&normalized](const auto& entry){ return entry == *normalized; });
}

auto AddSearchPath(const fs::FsPath& path) -> bool {
    const auto normalized = NormalizeSearchPath(path);
    if (!normalized || *normalized == "/" || *normalized == "/switch") {
        return false;
    }

    auto paths = LoadSearchPaths();
    if (std::ranges::any_of(paths, [&normalized](const auto& entry){ return entry == *normalized; })) {
        return false;
    }

    paths.emplace_back(*normalized);
    if (!SaveSearchPaths(paths)) {
        return false;
    }
    SignalChange();
    return true;
}

auto RemoveSearchPath(const fs::FsPath& path) -> bool {
    const auto normalized = NormalizeSearchPath(path);
    if (!normalized) {
        return false;
    }

    auto paths = LoadSearchPaths();
    const auto removed = std::erase_if(paths, [&normalized](const auto& entry){ return entry == *normalized; });
    if (!removed || !SaveSearchPaths(paths)) {
        return false;
    }
    SignalChange();
    return true;
}

auto GetNroEntries() -> std::span<const NroEntry> {
    if (!g_menu) {
        return {};
    }

    return g_menu->GetHomebrewList();
}

Menu::Menu(u32 flags) : grid::Menu{"Homebrew"_i18n, flags} {
    g_menu = this;

    this->SetActions(
        std::make_pair(Button::A, Action{"Launch"_i18n, [this](){
            const auto rc = App::CanSetCpuCores()
                ? nro_launch(GetEntry().path, {}, GetEntry().hbini.core_mode)
                : nro_launch(GetEntry().path);
            App::PushErrorBox(rc, "Failed to launch homebrew"_i18n);
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            DisplayOptions();
        }})
    );

    // Add Back button only when entered via Menus (Not center, righit, left tab).
    if (!(flags & MenuFlag_Tab)) {
        this->SetAction(Button::B, Action{"Back"_i18n, [this](){
            this->SetPop();
        }});
    }

    OnLayoutChange();
}

Menu::~Menu() {
    g_menu = {};
    FreeEntries();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (g_change_signalled.exchange(false)) {
        m_dirty = true;
    }

    if (m_dirty) {
        SortAndFindLastFile(true);
    }

    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries_current.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    m_list->Draw(vg, theme, m_entries_current.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto index = m_entries_current[pos];
        auto& e = m_entries[index];

        // lazy load image
        if (image_load_count < image_load_max) {
            if (!e.image && e.icon_size && e.icon_offset) {
                image_load_count++;
                const auto icon = nro_get_icon(e.path, e.icon_size, e.icon_offset);
                TimeStamp ts;
                if (!icon.empty()) {
                    const auto image = ImageLoadIcon(icon);
                    if (!image.data.empty()) {
                        e.image = nvgCreateImageRGBA(vg, image.w, image.h, 0, image.data.data());
                        if (e.image > 0) {
                            log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
                        }
                    }
                }

                if (e.image <= 0) {
                    e.icon_offset = e.icon_size = 0;
                }
            }
        }


        bool has_star = false;
        if (IsStarEnabled()) {
            if (!e.has_star.has_value()) {
                e.has_star = fs::FsNativeSd().FileExists(GenerateStarPath(e.path));
            }
            has_star = e.has_star.value();
        }

        std::string name;
        if (has_star) {
            name = std::string("\u2605 ") + e.GetName();
        } else {
            name = e.GetName();
        }

        const auto selected = pos == m_index;
        const auto image = e.image > 0 || (e.icon_size && e.icon_offset)
            ? e.image : App::GetDefaultImage();
        DrawEntry(vg, theme, m_layout.Get(), v, selected, image, name.c_str(), e.GetAuthor(), e.GetDisplayVersion());
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    if (m_entries.empty()) {
        ScanHomebrew();
    }
}

void Menu::SetIndex(s64 index) {
    if (m_entries_current.empty()) {
        m_index = 0;
        m_list->SetYoff(0);
        RemoveAction(Button::R3);
        SetTitleSubHeading("");
        SetSubHeading("0 / 0");
        return;
    }

    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    if (IsStarEnabled()) {
        const auto star_path = GenerateStarPath(GetEntry().path);
        if (fs::FsNativeSd().FileExists(star_path)) {
            SetAction(Button::R3, Action{"Unstar"_i18n, [this](){
                fs::FsNativeSd().DeleteFile(GenerateStarPath(GetEntry().path));
                App::Notify(i18n::Reorder("Unstarred ", GetEntry().GetName()));
                SortAndFindLastFile();
            }});
        } else {
            SetAction(Button::R3, Action{"Star"_i18n, [this](){
                fs::FsNativeSd().CreateFile(GenerateStarPath(GetEntry().path));
                App::Notify(i18n::Reorder("Starred ", GetEntry().GetName()));
                SortAndFindLastFile();
            }});
        }
    } else {
        RemoveAction(Button::R3);
    }

    // TimeCalendarTime caltime;
    // timeToCalendarTimeWithMyRule()
    // todo: fix GetFileTimeStampRaw being different to timeGetCurrentTime
    // log_write("name: %s hbini.ts: %lu file.ts: %lu smaller: %s\n", e.GetName(), e.hbini.timestamp, e.timestamp.modified, e.hbini.timestamp < e.timestamp.modified ? "true" : "false");

    SetTitleSubHeading(GetEntry().path);
    this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries_current.size()));
}

void Menu::InstallHomebrew() {
    const auto& nro = GetEntry();
    const auto path = nro.path;
    auto icon = nro_get_icon(nro.path, nro.icon_size, nro.icon_offset);
    ShowForwarderForm(path, std::move(icon));
}

void Menu::CustomizeHomebrew() {
    const auto path = GetEntry().path;
    auto icon = nro_get_icon(path, GetEntry().icon_size, GetEntry().icon_offset);
    if (icon.empty()) {
        App::Notify("This homebrew does not contain a customizable icon"_i18n);
        return;
    }

    forwarder::Config editor{};
    editor.values.title = GetEntry().GetName();
    editor.values.icon = std::move(icon);
    editor.icon_source = "NRO Icon"_i18n;
    editor.steam_query = GetEntry().GetName();
    editor.screen_title = "Customize Homebrew"_i18n;
    editor.title_label = "Homebrew Name"_i18n;
    editor.submit_label = "Save Changes"_i18n;
    editor.show_forwarder_options = false;
    editor.on_create = [path](const forwarder::Values& values) {
        if (R_FAILED(nro_update_info(path, values.title, values.icon))) {
            App::Notify("Failed to save NRO information"_i18n);
            return false;
        }

        SignalChange();
        App::Notify("Homebrew name and icon saved successfully."_i18n);
        return true;
    };
    forwarder::Show(std::move(editor));
}

void Menu::ScanHomebrew() {
    g_change_signalled = false;
    FreeEntries();

    {
        SCOPED_TIMESTAMP("nro scan");
        nro_scan("/switch", m_entries);

        for (const auto& path : LoadSearchPaths()) {
            std::vector<NroEntry> scanned;
            nro_scan_depth(path, scanned, 2);
            AppendUniqueEntries(m_entries, scanned);
        }
    }

    struct IniUser {
        std::vector<NroEntry>& entires;
        Hbini* ini{};
        std::string last_section{};
    } ini_user{ m_entries };

    ini_browse([](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto user = static_cast<IniUser*>(UserData);

        if (user->last_section != Section) {
            user->last_section = Section;
            user->ini = nullptr;

            for (auto& e : user->entires) {
                if (e.path == Section) {
                    user->ini = &e.hbini;
                    break;
                }
            }
        }

        if (user->ini) {
            if (!strcmp(Key, "timestamp")) {
                user->ini->timestamp = ini_parse_getl(Value, 0);
            } else if (!strcmp(Key, "hidden")) {
                user->ini->hidden = ini_parse_getbool(Value, false);
            } else if (!strcmp(Key, "cpu_cores")) {
                user->ini->core_mode = ini_parse_getl(Value, 3) == 4 ? CpuCoreMode::Four : CpuCoreMode::Three;
            }
        }

        // log_write("found: %s %s %s\n", Section, Key, Value);
        return 1;
    }, &ini_user, App::PLAYLOG_PATH);

    // pre-allocate the max size.
    for (auto& index : m_entries_index) {
        index.reserve(m_entries.size());
    }

    for (u32 i = 0; i < m_entries.size(); i++) {
        auto& e = m_entries[i];

        m_entries_index[Filter_All].emplace_back(i);

        if (!e.hbini.hidden) {
            m_entries_index[Filter_HideHidden].emplace_back(i);
        }
    }

    this->Sort();
    SetIndex(0);
    m_dirty = false;
}

void Menu::Sort() {
    if (IsStarEnabled()) {
        fs::FsNativeSd fs;
        fs::FsPath star_path;
        for (auto& p : m_entries) {
            p.has_star = fs.FileExists(GenerateStarPath(p.path));
        }
    }

    // returns true if lhs should be before rhs
    const auto sort = m_sort.Get();
    const auto order = m_order.Get();

    const auto sorter = [this, sort, order](u32 _lhs, u32 _rhs) -> bool {
        const auto& lhs = m_entries[_lhs];
        const auto& rhs = m_entries[_rhs];

        const auto name_cmp = [order](const NroEntry& lhs, const NroEntry& rhs) -> bool {
            auto r = strcasecmp(lhs.GetName(), rhs.GetName());
            if (!r) {
                r = strcasecmp(lhs.GetAuthor(), rhs.GetAuthor());
                if (!r) {
                    r = strcasecmp(lhs.path, rhs.path);
                }
            }

            if (order == OrderType_Descending) {
                return r < 0;
            } else {
                return r > 0;
            }
        };

        switch (sort) {
            case SortType_UpdatedStar:
                if (lhs.has_star.value() && !rhs.has_star.value()) {
                    return true;
                } else if (!lhs.has_star.value() && rhs.has_star.value()) {
                    return false;
                }
                [[fallthrough]];
            case SortType_Updated: {
                auto lhs_timestamp = lhs.hbini.timestamp;
                auto rhs_timestamp = rhs.hbini.timestamp;
                if (lhs.timestamp.is_valid && lhs_timestamp < lhs.timestamp.modified) {
                    lhs_timestamp = lhs.timestamp.modified;
                }
                if (rhs.timestamp.is_valid && rhs_timestamp < rhs.timestamp.modified) {
                    rhs_timestamp = rhs.timestamp.modified;
                }

                if (lhs_timestamp == rhs_timestamp) {
                    return name_cmp(lhs, rhs);
                } else if (order == OrderType_Descending) {
                    return lhs_timestamp > rhs_timestamp;
                } else {
                    return lhs_timestamp < rhs_timestamp;
                }
            } break;

            case SortType_SizeStar:
                if (lhs.has_star.value() && !rhs.has_star.value()) {
                    return true;
                } else if (!lhs.has_star.value() && rhs.has_star.value()) {
                    return false;
                }
                [[fallthrough]];
            case SortType_Size: {
                if (lhs.size == rhs.size) {
                    return name_cmp(lhs, rhs);
                } else if (order == OrderType_Descending) {
                    return lhs.size > rhs.size;
                } else {
                    return lhs.size < rhs.size;
                }
            } break;

            case SortType_AlphabeticalStar:
                if (lhs.has_star.value() && !rhs.has_star.value()) {
                    return true;
                } else if (!lhs.has_star.value() && rhs.has_star.value()) {
                    return false;
                }
                [[fallthrough]];
            case SortType_Alphabetical: {
                return name_cmp(lhs, rhs);
            } break;
        }

        std::unreachable();
    };

    if (m_show_hidden.Get()) {
        m_entries_current = m_entries_index[Filter_All];
    } else {
        m_entries_current = m_entries_index[Filter_HideHidden];
    }

    std::ranges::sort(m_entries_current, sorter);
}

void Menu::SortAndFindLastFile(bool scan) {
    fs::FsPath path;
    if (!m_entries_current.empty()) {
        path = GetEntry().path;
    }

    if (scan) {
        ScanHomebrew();
    } else {
        Sort();
    }
    SetIndex(0);

    if (path.empty()) {
        return;
    }

    s64 index = -1;
    for (u64 i = 0; i < m_entries_current.size(); i++) {
        if (path == GetEntry(i).path) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        const auto row = m_list->GetRow();
        const auto page = m_list->GetPage();
        // guesstimate where the position is
        if (index >= page) {
            m_list->SetYoff((((index - page) + row) / row) * m_list->GetMaxY());
        } else {
            m_list->SetYoff(0);
        }
        SetIndex(index);
    }
}

void Menu::FreeEntries() {
    auto vg = App::GetVg();

    for (auto&p : m_entries) {
        FreeEntry(vg, p);
    }

    m_entries.clear();
    m_entries_current = {};
    for (auto& e : m_entries_index) {
        e.clear();
    }
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

Result Menu::InstallHomebrew(const fs::FsPath& path, const std::vector<u8>& icon, ForwarderAddressSpace address_space) {
    OwoConfig config{};
    config.nro_path = path.toString();
    R_TRY(nro_get_nacp(path, config.nacp));
    config.icon = icon;
    config.address_space = address_space;
    return App::Install(config);
}

Result Menu::InstallHomebrewFromPath(const fs::FsPath& path, ForwarderAddressSpace address_space) {
    return InstallHomebrew(path, nro_get_icon(path), address_space);
}

void Menu::ShowForwarderForm(const fs::FsPath& path, std::vector<u8> icon) {
    NroEntry nro{};
    NacpStruct nacp{};
    if (R_FAILED(nro_parse(path, nro)) || R_FAILED(nro_get_nacp(path, nacp))) {
        App::Notify("Failed to parse nro"_i18n);
        return;
    }

    if (icon.empty()) {
        icon = nro_get_icon(path, nro.icon_size, nro.icon_offset);
    }

    forwarder::Config editor{};
    editor.values.title = nro.GetName();
    editor.values.author = nro.GetAuthor();
    editor.values.version = nacp.display_version;
    editor.values.icon = std::move(icon);
    editor.icon_source = "NRO Icon"_i18n;
    editor.on_create = [path, nacp](const forwarder::Values& values) mutable {
        OwoConfig config{};
        config.nro_path = path.toString();
        config.name = values.title;
        config.author = values.author;
        config.nacp = nacp;
        config.icon = values.icon;
        config.profile_selection = values.profile_selection;
        config.address_space = values.address_space;
        config.core_mode = values.core_mode;
        config.screenshot = values.screenshot;
        config.video_capture = values.video_capture;
        config.native_game_mode = values.native_game_mode;
        config.svc_debug_mode = values.svc_debug_mode;

        if (R_FAILED(App::Install(config))) {
            App::Notify("Failed to install forwarder"_i18n);
            return false;
        }
        return true;
    };
    forwarder::Show(std::move(editor));
}

void Menu::DisplayOptions() {
    auto options = std::make_unique<Sidebar>("Homebrew Options"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<SidebarEntryCallback>("Sort By"_i18n, [this](){
        auto options = std::make_unique<Sidebar>("Sort Options"_i18n, Sidebar::Side::RIGHT);
        ON_SCOPE_EXIT(App::Push(std::move(options)));

        SidebarEntryArray::Items sort_items;
        sort_items.push_back("Updated"_i18n);
        sort_items.push_back("Alphabetical"_i18n);
        sort_items.push_back("Size"_i18n);
        sort_items.push_back("Updated (Star)"_i18n);
        sort_items.push_back("Alphabetical (Star)"_i18n);
        sort_items.push_back("Size (Star)"_i18n);

        SidebarEntryArray::Items order_items;
        order_items.push_back("Descending"_i18n);
        order_items.push_back("Ascending"_i18n);

        SidebarEntryArray::Items layout_items;
        layout_items.push_back("List"_i18n);
        layout_items.push_back("Icon"_i18n);
        layout_items.push_back("Grid"_i18n);

        options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this, sort_items](s64& index_out){
            m_sort.Set(index_out);
            SortAndFindLastFile();
        }, m_sort.Get());

        options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this, order_items](s64& index_out){
            m_order.Set(index_out);
            SortAndFindLastFile();
        }, m_order.Get(), "Display entries in Ascending or Descending order."_i18n);

        options->Add<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
            m_layout.Set(index_out);
            OnLayoutChange();
        }, m_layout.Get(), "Change the layout to List, Icon and Grid."_i18n);

        options->Add<SidebarEntryBool>("Show hidden"_i18n, m_show_hidden.Get(), [this](bool& enable){
            m_show_hidden.Set(enable);
            SortAndFindLastFile();
        }, "Shows all hidden homebrew."_i18n);
    });

    // for testing stuff.
    #if 0
    options->Add<SidebarEntrySlider>("Test"_i18n, 1, 0, 2, 10, [](auto& v_out){

    });
    #endif

    if (!m_entries_current.empty()) {
        #if 0
        options->Add<SidebarEntryCallback>("Info"_i18n, [this](){

        });
        #endif

        auto customize_entry = options->Add<SidebarEntryCallback>("Customize Homebrew"_i18n, [this](){
            CustomizeHomebrew();
        }, true, "Change the name and icon embedded in the selected NRO."_i18n);
        customize_entry->Depends([this](){ return GetEntry().is_nacp_valid; }, "This NRO does not contain editable application information."_i18n);

        if (App::CanSetCpuCores()) {
            SidebarEntryArray::Items core_items;
            core_items.push_back("3 (Default)"_i18n);
            core_items.push_back("4 (Advanced)"_i18n);
            options->Add<SidebarEntryArray>("CPU Cores"_i18n, core_items, [this](s64& index_out){
                if (index_out == 1) {
                    index_out = GetEntry().hbini.core_mode == CpuCoreMode::Four ? 1 : 0;
                    App::Push<OptionBox>(
                        "4 CPU Cores\n\nCore 3 is shared with system services. Only enable this for homebrew that manages thread affinity correctly; unrestricted use can cause system lag or instability."_i18n,
                        "Back"_i18n,
                        "Enable"_i18n,
                        0,
                        [this](auto index) {
                            if (index && *index == 1) {
                                GetEntry().hbini.core_mode = CpuCoreMode::Four;
                                ini_putl(GetEntry().path, "cpu_cores", 4, App::PLAYLOG_PATH);
                                App::PopToMenu();
                            }
                        }
                    );
                } else {
                    GetEntry().hbini.core_mode = CpuCoreMode::Three;
                    ini_putl(GetEntry().path, "cpu_cores", 3, App::PLAYLOG_PATH);
                }
            }, GetEntry().hbini.core_mode == CpuCoreMode::Four ? 1 : 0,
                "Three cores reserves core 3 for system services. Four cores should only be used by homebrew that manages thread affinity correctly."_i18n);
        }

        options->Add<SidebarEntryBool>("Hide"_i18n, GetEntry().hbini.hidden, [this](bool& v_out){
            ini_putl(GetEntry().path, "hidden", v_out, App::PLAYLOG_PATH);
            ScanHomebrew();
            App::PopToMenu();
        },  "Hides the selected homebrew.\n\n"
            "To unhide homebrew, enable \"Show hidden\" in the sort options."_i18n);

        options->Add<SidebarEntryCallback>("Mount NRO Fs"_i18n, [this](){
            const auto rc = MountNroFs();
            App::PushErrorBox(rc, "Failed to mount NRO FileSystem"_i18n);
        },  "Mounts the NRO FileSystem (icon, nacp and RomFS)."_i18n);

        options->Add<SidebarEntryCallback>("Delete"_i18n, [this](){
            const auto buf = i18n::Reorder("Are you sure you want to delete ", GetEntry().path.toString()) + "?";
            App::Push<OptionBox>(
                buf,
                "Back"_i18n, "Delete"_i18n, 1, [this](auto op_index){
                    if (op_index && *op_index) {
                        if (R_SUCCEEDED(fs::FsNativeSd().DeleteFile(GetEntry().path))) {
                            // todo: remove from list using real index here.
                            FreeEntry(App::GetVg(), GetEntry());
                            ScanHomebrew();
                            // m_entries.erase(m_entries.begin() + m_index);
                            // SetIndex(m_index ? m_index - 1 : 0);
                            App::PopToMenu();
                        }
                    }
                }, GetEntry().image
            );
        },  i18n::get("hb_remove_info",
                "Perminately delete the selected homebrew.\n\n"
                "Files and folders created by the homebrew will still remain. "
                "Use the FileBrowser to delete them."));

        auto forwarder_entry = options->Add<SidebarEntryCallback>("Install Forwarder"_i18n, [this](){
            InstallHomebrew();
        }, true);

        forwarder_entry->Depends(App::GetInstallEnable, i18n::get(App::INSTALL_DEPENDS_STR), App::ShowEnableInstallPrompt);
    }
}

Result Menu::MountNroFs() {
    const auto& e = GetEntry();

    fs::FsPath root;
    R_TRY(devoptab::MountNro(App::GetApp()->m_fs.get(), e.path, root));

    auto fs = std::make_shared<filebrowser::FsStdioWrapper>(root, [root](){
        devoptab::UmountNeworkDevice(root);
    });

    filebrowser::MountFsHelper(fs, root);
    R_SUCCEED();
}

} // namespace sphaira::ui::menu::homebrew

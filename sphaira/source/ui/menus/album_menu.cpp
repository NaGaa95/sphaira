#include "app.hpp"
#include "log.hpp"
#include "image.hpp"
#include "defines.hpp"
#include "i18n.hpp"

#include "ui/menus/album_menu.hpp"
#include "ui/menus/album_share_menu.hpp"
#include "ui/menus/image_viewer.hpp"

#include "ui/sidebar.hpp"
#include "ui/option_box.hpp"
#include "ui/nvg_util.hpp"

#include "utils/utils.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace sphaira::ui::menu::album {
namespace {

std::atomic_bool g_change_signalled{};

// an album can hold thousands of captures and every decoded thumbnail costs
// vram, so only the ones around what is on screen are kept.
constexpr s64 IMAGE_LIMIT = 64;
constexpr s64 IMAGE_KEEP_RANGE = 32;

// captures taken outside a game (the home menu, the album applet itself) are
// filed under an applet id, which has no title to look up.
constexpr u64 APPLET_ID_MIN = 0x0100000000001000;
constexpr u64 APPLET_ID_MAX = 0x0100000000001FFF;

auto HasTitle(u64 application_id) -> bool {
    return application_id && (application_id < APPLET_ID_MIN || application_id > APPLET_ID_MAX);
}

auto MakeDate(const CapsAlbumFileDateTime& datetime) -> u64 {
    return (u64)datetime.year * 10000000000ULL
        + (u64)datetime.month * 100000000ULL
        + (u64)datetime.day * 1000000ULL
        + (u64)datetime.hour * 10000ULL
        + (u64)datetime.minute * 100ULL
        + (u64)datetime.second;
}

auto FormatDate(const CapsAlbumFileDateTime& datetime) -> std::string {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)datetime.year, (unsigned)datetime.month, (unsigned)datetime.day,
        (unsigned)datetime.hour, (unsigned)datetime.minute, (unsigned)datetime.second);

    return buf;
}

auto IsVideoContent(u8 content) -> bool {
    return content == CapsAlbumFileContents_Movie || content == CapsAlbumFileContents_ExtraMovie;
}

// the console's own name for a capture, minus the hash it appends.
auto MakeFileName(const CapsAlbumFileId& file_id) -> std::string {
    const auto& d = file_id.datetime;

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u%02u%02u%02u%02u.%s",
        (unsigned)d.year, (unsigned)d.month, (unsigned)d.day, (unsigned)d.hour,
        (unsigned)d.minute, (unsigned)d.second, (unsigned)d.id,
        IsVideoContent(file_id.content) ? "mp4" : "jpg");

    return buf;
}

// fits the image inside v without stretching it, captures are 16:9 and the
// grid cells are not.
auto FitImage(const Vec4& v, float w, float h) -> Vec4 {
    if (w <= 0 || h <= 0) {
        return v;
    }

    const auto scale = std::min(v.w / w, v.h / h);
    const auto fit_w = w * scale;
    const auto fit_h = h * scale;

    return Vec4{v.x + (v.w - fit_w) / 2, v.y + (v.h - fit_h) / 2, fit_w, fit_h};
}

} // namespace

void SignalChange() {
    g_change_signalled = true;
}

Menu::Menu(u32 flags) : grid::Menu{"Album"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::L3, Action{[this](){
            if (m_view.empty()) {
                return;
            }

            auto& e = m_entries[m_view[m_index]];
            e.selected ^= 1;

            if (e.selected) {
                m_selected_count++;
            } else {
                m_selected_count--;
            }
        }}),
        std::make_pair(Button::R3, Action{[this](){
            if (m_view.empty()) {
                return;
            }

            if (m_selected_count == (s64)m_view.size()) {
                ClearSelection();
            } else {
                ClearSelection();
                m_selected_count = m_view.size();
                for (const auto i : m_view) {
                    m_entries[i].selected = true;
                }
            }
        }}),
        std::make_pair(Button::A, Action{"View"_i18n, [this](){
            OnEntrySelected();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            DisplayOptions();
        }})
    );

    OnLayoutChange();
    title::Init();

    // anything signalled before the menu opened is already in the scan below.
    g_change_signalled = false;

    m_scan_rc = caps::Init();
    if (R_SUCCEEDED(m_scan_rc)) {
        m_caps_init = true;
        Scan();
    } else {
        log_write("[ALBUM] failed to init caps:a: 0x%X\n", m_scan_rc);
        SetIndex(0);
    }
}

Menu::~Menu() {
    m_scan_thread.reset();

    FreeEntries();

    if (m_caps_init) {
        caps::Exit();
    }

    title::Exit();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (g_change_signalled.exchange(false)) {
        m_dirty = true;
    }

    // a rescan runs off the ui thread, so a refresh is never felt.
    if (m_dirty && m_caps_init) {
        m_dirty = false;
        Scan();
        App::Notify("Album updated"_i18n);
    }

    if (m_scanning && m_scan_done) {
        m_scan_thread.reset();
        FinishScan();
    }

    if (R_FAILED(m_scan_rc)) {
        App::PushErrorBox(m_scan_rc, "Failed to open the album"_i18n);
        m_scan_rc = 0;
    }

    MenuBase::Update(controller, touch);

    m_list->OnUpdate(controller, touch, m_index, m_view.size(), [this](bool touch, auto i) {
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

    if (m_scanning) {
        const auto cx = GetX() + GetW() / 2.f;
        const auto cy = GetY() + GetH() / 2.f;
        gfx::drawSpinner(vg, theme, cx, cy - 30.f, 30.f, armTicksToNs(armGetSystemTick()) / 1e+9);
        gfx::drawTextArgs(vg, cx, cy + 40.f, 24.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Loading"_i18n.c_str());
        return;
    }

    if (m_view.empty()) {
        gfx::drawTextArgs(vg, GetX() + GetW() / 2.f, GetY() + GetH() / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    // what is actually on screen, which is not always centred on the cursor
    // because the list can be scrolled by touch without moving it.
    s64 first_drawn = m_view.size();
    s64 last_drawn = 0;

    m_list->Draw(vg, theme, m_view.size(), [this, &image_load_count, &first_drawn, &last_drawn, image_load_max](auto* vg, auto* theme, auto v, auto pos) {
        first_drawn = std::min<s64>(first_drawn, pos);
        last_drawn = std::max<s64>(last_drawn, pos);

        const auto& [x, y, w, h] = v;
        auto& e = m_entries[m_view[pos]];

        LoadTitle(e);

        // lazy load image
        if (image_load_count < image_load_max) {
            if (LoadImage(e)) {
                image_load_count++;
            }
        }

        const auto selected = pos == m_index;
        const auto image_v = DrawEntryNoImage(vg, theme, m_layout.Get(), v, selected, e.name.c_str(), e.title.c_str(), e.info.c_str());

        if (e.image) {
            gfx::drawImage(vg, FitImage(image_v, e.image_w, e.image_h), e.image, 5);
        } else if (e.image_failed) {
            DrawElement(image_v, e.IsVideo() ? ThemeEntryID_ICON_VIDEO : ThemeEntryID_ICON_IMAGE);
        } else {
            gfx::drawSpinner(vg, theme, image_v.x + image_v.w / 2, image_v.y + image_v.h / 2, image_v.h / 6, armTicksToNs(armGetSystemTick()) / 1e+9);
        }

        // a clip's thumbnail looks just like a screenshot, mark which is which.
        if (e.IsVideo() && e.image) {
            DrawElement(image_v.x + image_v.w - 40, image_v.y + image_v.h - 40, 32, 32, ThemeEntryID_ICON_VIDEO);
        }

        if (e.selected) {
            gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_FOCUS), 5);
            gfx::drawText(vg, x + w / 2, y + h / 2, 24.f, "\uE14B", nullptr, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
        }
    });

    EvictImages(first_drawn, last_drawn);
}

void Menu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    const auto position = m_view.empty()
        ? std::string{"0 / 0"}
        : std::to_string(m_index + 1) + " / " + std::to_string(m_view.size());

    this->SetSubHeading(position
        + " | " + "Screenshots"_i18n + ": " + std::to_string(m_screenshot_count)
        + " | " + "Videos"_i18n + ": " + std::to_string(m_video_count));

    const auto storage = m_storage.Get() == StorageType_Nand ? "System memory"_i18n : "microSD card"_i18n;
    auto sub_heading = storage + " | " + utils::formatSizeStorage(m_total_size);

    if (m_filter) {
        sub_heading = GetGameName(m_filter) + " | " + sub_heading;
    }

    SetTitleSubHeading(sub_heading);
}

void Menu::Scan() {
    // joins any scan still running, its results are about to be thrown away.
    m_scan_thread.reset();

    FreeEntries();

    m_index = 0;
    m_selected_count = 0;
    m_screenshot_count = 0;
    m_video_count = 0;
    m_total_size = 0;
    SetIndex(0);

    const auto storage = GetStorage();

    m_scan_done = false;
    m_scanning = true;

    m_scan_thread = std::make_unique<utils::Async>([this, storage](){
        ScanWorker(storage);
    });

    // no thread to be had, better to block for a moment than to sit on a
    // loading screen that never goes away.
    if (!m_scan_thread->IsRunning()) {
        m_scan_thread.reset();
        ScanWorker(storage);
        FinishScan();
    }
}

// runs on the scan thread: talks to the album service and builds the entries,
// and touches nothing else the ui is using.
void Menu::ScanWorker(CapsAlbumStorage storage) {
    ScanResult result{};

    std::vector<CapsAlbumEntry> entries;
    result.rc = caps::GetEntries(storage, entries);

    if (R_FAILED(result.rc)) {
        log_write("[ALBUM] failed to list the album: 0x%X\n", result.rc);
    } else {
        result.entries.reserve(entries.size());

        for (const auto& entry : entries) {
            Entry e{};
            e.file_id = entry.file_id;
            e.size = entry.size;
            e.date = MakeDate(entry.file_id.datetime);
            e.name = FormatDate(entry.file_id.datetime);
            e.info = (e.IsVideo() ? "Video"_i18n : "Screenshot"_i18n) + " | " + utils::formatSizeStorage(e.size);

            const auto app_id = e.file_id.application_id;
            const auto game = std::ranges::find_if(result.games, [app_id](const auto& g){
                return g.application_id == app_id;
            });

            if (game == result.games.end()) {
                result.games.emplace_back(GameInfo{app_id, e.date, 1});
            } else {
                game->count++;
                game->newest = std::max(game->newest, e.date);
            }

            result.entries.emplace_back(std::move(e));
        }

        // most recently captured first, the same as the album itself.
        std::ranges::sort(result.games, [](const auto& a, const auto& b){
            return a.newest > b.newest;
        });
    }

    m_scan_data = std::move(result);
    m_scan_done = true;
}

void Menu::FinishScan() {
    m_scanning = false;
    m_scan_done = false;

    m_entries = std::move(m_scan_data.entries);
    m_games = std::move(m_scan_data.games);
    m_scan_rc = m_scan_data.rc;
    m_scan_data = {};

    Sort();

    // queued from here rather than the scan thread, the title cache is only
    // ever fed from the ui thread elsewhere.
    for (const auto& game : m_games) {
        if (HasTitle(game.application_id)) {
            title::PushAsync(game.application_id);
        }
    }

    // the game that was filtered on may not be in this storage at all.
    if (m_filter && !FindGame(m_filter)) {
        m_filter = 0;
    }

    ApplyFilter();
}

void Menu::ApplyFilter() {
    m_view.clear();
    m_view.reserve(m_entries.size());

    m_screenshot_count = 0;
    m_video_count = 0;
    m_total_size = 0;
    m_selected_count = 0;

    for (s64 i = 0; i < (s64)m_entries.size(); i++) {
        auto& e = m_entries[i];

        if (m_filter && e.file_id.application_id != m_filter) {
            // it cannot be drawn, so it has no business holding a texture.
            FreeEntry(e);
            e.selected = false;
            continue;
        }

        if (e.selected) {
            m_selected_count++;
        }

        m_view.emplace_back(i);

        m_total_size += e.size;
        if (e.IsVideo()) {
            m_video_count++;
        } else {
            m_screenshot_count++;
        }
    }

    m_list->SetYoff(0);
    SetIndex(0);
}

auto Menu::FindGame(u64 application_id) -> const GameInfo* {
    const auto it = std::ranges::find_if(m_games, [application_id](const auto& g){
        return g.application_id == application_id;
    });

    return it == m_games.end() ? nullptr : std::addressof(*it);
}

auto Menu::GetGameName(u64 application_id) -> std::string {
    if (!HasTitle(application_id)) {
        return "System"_i18n;
    }

    if (const auto result = title::GetAsync(application_id)) {
        if (result->status == title::NacpLoadStatus::Loaded && result->lang.name[0]) {
            return result->lang.name;
        }
    }

    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lX", application_id);
    return buf;
}

void Menu::Sort() {
    const auto sort = m_sort.Get();
    const auto order = m_order.Get();

    const auto sorter = [sort, order](const Entry& lhs, const Entry& rhs) -> bool {
        if (sort == SortType_Size && lhs.size != rhs.size) {
            return order == OrderType_Descending ? lhs.size > rhs.size : lhs.size < rhs.size;
        }

        // captures taken in the same second still need a stable order, and the
        // datetime carries an id that separates them.
        if (lhs.date == rhs.date) {
            return order == OrderType_Descending
                ? lhs.file_id.datetime.id > rhs.file_id.datetime.id
                : lhs.file_id.datetime.id < rhs.file_id.datetime.id;
        }

        return order == OrderType_Descending ? lhs.date > rhs.date : lhs.date < rhs.date;
    };

    std::ranges::sort(m_entries, sorter);
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

void Menu::FreeEntry(Entry& e) {
    if (e.image) {
        nvgDeleteImage(App::GetVg(), e.image);
        e.image = 0;
        m_image_count--;
    }
}

void Menu::FreeEntries() {
    for (auto& e : m_entries) {
        FreeEntry(e);
    }

    m_entries.clear();
    m_view.clear();
    m_image_count = 0;
}

void Menu::EvictImages(s64 first_drawn, s64 last_drawn) {
    if (m_image_count <= IMAGE_LIMIT) {
        return;
    }

    // anything well outside what was just drawn is cheap to load again.
    const auto keep_first = first_drawn - IMAGE_KEEP_RANGE;
    const auto keep_last = last_drawn + IMAGE_KEEP_RANGE;

    for (s64 i = 0; i < (s64)m_view.size() && m_image_count > IMAGE_LIMIT; i++) {
        if (i < keep_first || i > keep_last) {
            FreeEntry(m_entries[m_view[i]]);
        }
    }
}

void Menu::LoadTitle(Entry& e) {
    const auto app_id = e.file_id.application_id;

    if (e.status == title::NacpLoadStatus::None) {
        if (!HasTitle(app_id)) {
            e.title = "System"_i18n;
            e.status = title::NacpLoadStatus::Error;
            return;
        }

        title::PushAsync(app_id);
        e.status = title::NacpLoadStatus::Progress;
        return;
    }

    if (e.status == title::NacpLoadStatus::Progress) {
        if (const auto result = title::GetAsync(app_id)) {
            e.status = result->status;
            if (result->status == title::NacpLoadStatus::Loaded) {
                e.title = result->lang.name;
            }
        }
    }
}

auto Menu::LoadImage(Entry& e) -> bool {
    if (e.image || e.image_failed) {
        return false;
    }

    // don't try a capture that cannot be decoded over and over again.
    e.image_failed = true;

    // the console keeps a 320x180 thumbnail for every capture, which is what
    // its own album grid uses, so there is nothing to decode at full size.
    std::vector<u8> buf;
    if (R_FAILED(caps::LoadThumbnail(e.file_id, buf)) || buf.empty()) {
        return false;
    }

    const auto result = ImageLoadFromMemory(buf, ImageFlag_JPEG);
    if (result.data.empty()) {
        return false;
    }

    const auto image = nvgCreateImageRGBA(App::GetVg(), result.w, result.h, 0, result.data.data());
    if (image <= 0) {
        return false;
    }

    e.image = image;
    e.image_w = result.w;
    e.image_h = result.h;
    e.image_failed = false;
    m_image_count++;

    return true;
}

void Menu::OnEntrySelected() {
    if (m_view.empty()) {
        return;
    }

    const auto& e = m_entries[m_view[m_index]];
    if (e.IsVideo()) {
        App::Notify("Video playback is not supported"_i18n);
        return;
    }

    std::vector<u8> buf;
    const auto rc = caps::LoadFile(e.file_id, buf);
    if (R_FAILED(rc) || buf.empty()) {
        App::PushErrorBox(rc, "Failed to load image"_i18n);
        return;
    }

    App::Push<imageview::Menu>(buf, ImageFlag_JPEG);
}

void Menu::ClearSelection() {
    for (auto& e : m_entries) {
        e.selected = false;
    }

    m_selected_count = 0;
}

auto Menu::GetSelected() -> std::vector<std::reference_wrapper<Entry>> {
    std::vector<std::reference_wrapper<Entry>> out;

    if (m_selected_count) {
        for (const auto i : m_view) {
            if (m_entries[i].selected) {
                out.emplace_back(m_entries[i]);
            }
        }
    } else if (!m_view.empty()) {
        out.emplace_back(m_entries[m_view[m_index]]);
    }

    return out;
}

void Menu::DeleteSelected() {
    Result rc{};

    for (auto& entry : GetSelected()) {
        auto& e = entry.get();

        const auto delete_rc = caps::DeleteFile(e.file_id);
        if (R_FAILED(delete_rc)) {
            log_write("[ALBUM] failed to delete a capture: 0x%X\n", delete_rc);
            rc = delete_rc;
            continue;
        }

        FreeEntry(e);

        m_total_size -= e.size;
        if (e.IsVideo()) {
            m_video_count--;
        } else {
            m_screenshot_count--;
        }

        // marks the entry for removal below.
        e.name.clear();
    }

    std::erase_if(m_entries, [](const Entry& e) {
        return e.name.empty();
    });

    // the indices in the view no longer mean anything.
    ApplyFilter();

    App::PushErrorBox(rc, "Failed to delete one or more files"_i18n);
}

void Menu::DisplayOptions() {
    auto options = std::make_unique<Sidebar>("Album Options"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    SidebarEntryArray::Items storage_items;
    storage_items.push_back("microSD card"_i18n);
    storage_items.push_back("System memory"_i18n);

    options->Add<SidebarEntryCallback>("Sort By"_i18n, [this](){
        auto options = std::make_unique<Sidebar>("Sort Options"_i18n, Sidebar::Side::RIGHT);
        ON_SCOPE_EXIT(App::Push(std::move(options)));

        SidebarEntryArray::Items sort_items;
        sort_items.push_back("Date"_i18n);
        sort_items.push_back("Size"_i18n);

        SidebarEntryArray::Items order_items;
        order_items.push_back("Descending"_i18n);
        order_items.push_back("Ascending"_i18n);

        SidebarEntryArray::Items layout_items;
        layout_items.push_back("List"_i18n);
        layout_items.push_back("Icon"_i18n);
        layout_items.push_back("Grid"_i18n);

        options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
            m_sort.Set(index_out);
            Sort();
            ApplyFilter();
        }, m_sort.Get());

        options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
            m_order.Set(index_out);
            Sort();
            ApplyFilter();
        }, m_order.Get());

        options->Add<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
            m_layout.Set(index_out);
            OnLayoutChange();
            SetIndex(0);
        }, m_layout.Get());
    });

    options->Add<SidebarEntryArray>("Storage"_i18n, storage_items, [this](s64& index_out){
        m_storage.Set(index_out);
        Scan();
        App::PopToMenu();
    }, m_storage.Get());

    if (m_games.size() > 1) {
        // the count each one holds goes next to the name, as the console does.
        const auto with_count = [](const std::string& name, s64 count) {
            return name + " (" + std::to_string(count) + ")";
        };

        SidebarEntryArray::Items game_items;
        game_items.push_back(with_count("All"_i18n, m_entries.size()));

        s64 game_index = 0;
        for (const auto& game : m_games) {
            if (m_filter && m_filter == game.application_id) {
                game_index = game_items.size();
            }

            game_items.push_back(with_count(GetGameName(game.application_id), game.count));
        }

        options->Add<SidebarEntryArray>("Game"_i18n, game_items, [this](s64& index_out){
            m_filter = index_out ? m_games[index_out - 1].application_id : 0;
            ClearSelection();
            ApplyFilter();
            App::PopToMenu();
        }, game_index,
            "Shows only the captures taken in one game."_i18n
        );
    }

    options->Add<SidebarEntryCallback>("Refresh"_i18n, [this](){
        m_dirty = true;
        App::PopToMenu();
    }, "Looks for captures taken, or deleted, since this menu was opened."_i18n);

    if (!m_view.empty()) {
        options->Add<SidebarEntryCallback>("Browse from phone"_i18n, [this](){
            // what is on show is what gets served.
            albumsrv::Items items;
            items.reserve(m_view.size());

            for (const auto i : m_view) {
                const auto& e = m_entries[i];
                items.emplace_back(albumsrv::Item{e.file_id, e.name, e.title, MakeFileName(e.file_id), e.size, e.IsVideo()});
            }

            App::Push<ShareMenu>(std::move(items), (u16)m_web_port.Get());
        }, true,
            "Serves the album over the network, so that it can be browsed from a phone."_i18n
        );

        options->Add<SidebarEntryCallback>("Delete"_i18n, [this](){
            const auto count = m_selected_count ? m_selected_count : 1;

            char buf[128];
            std::snprintf(buf, sizeof(buf), "Delete %zd file(s)?"_i18n.c_str(), count);

            App::Push<OptionBox>(
                buf,
                "Back"_i18n, "Delete"_i18n, 0, [this](auto op_index){
                    if (op_index && *op_index) {
                        DeleteSelected();
                    }
                }
            );
        }, true,
            "Permanently deletes the selected capture(s)."_i18n
        );
    }
}

} // namespace sphaira::ui::menu::album

#include "ui/menus/storage_menu.hpp"
#include "ui/menus/game_meta_menu.hpp"

#include "ui/nvg_util.hpp"
#include "ui/sidebar.hpp"

#include "yati/nx/ncm.hpp"
#include "yati/nx/ns.hpp"

#include "utils/utils.hpp"

#include "title_info.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "i18n.hpp"
#include "image.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>

namespace sphaira::ui::menu::storage {
namespace {

// sizing an entry costs a handful of ncm queries, so it is spread over
// multiple frames rather than blocking the ui for several seconds on a large
// library. a cached entry only costs the meta listing, so budget by time and
// let a warm cache burn through far more entries per frame.
constexpr std::size_t SCAN_MIN_PER_FRAME = 4;
constexpr std::size_t SCAN_MAX_PER_FRAME = 64;
constexpr u64 SCAN_BUDGET_MS = 4;
// max images uploaded per frame, in order to not hit io / gpu too hard.
constexpr int IMAGE_LOAD_MAX = 2;

constexpr float BAR_ROUNDING = 2.f;

// measured sizes are kept next to the other sphaira state, walking ncm for
// every game is by far the slowest part of this menu.
constexpr fs::FsPath CACHE_PATH{"/config/sphaira/storage_cache.bin"};
constexpr u32 CACHE_MAGIC = 0x53535053; // 'SPSS'
constexpr u32 CACHE_VERSION = 1;

struct CacheHeader {
    u32 magic;
    u32 version;
    u64 count;
};
static_assert(sizeof(CacheHeader) == 0x10);

struct CacheRecord {
    u64 app_id;
    u64 fingerprint;
    s64 size_base;
    s64 size_update;
    s64 size_dlc;
    s64 size_sd;
    s64 size_nand;
};
static_assert(sizeof(CacheRecord) == 0x38);

// left side panel.
constexpr Vec4 PANEL{30, 90, 375, 555};
constexpr float PANEL_X = PANEL.x + 20;
constexpr float PANEL_W = PANEL.w - 40;

bool LoadControlImage(game::Entry& e, title::ThreadResultData* result) {
    if (!e.image && result && !result->icon.empty()) {
        const auto image = ImageLoadFromMemory(result->icon, ImageFlag_JPEG);
        if (!image.data.empty()) {
            e.image = nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
            return true;
        }
    }

    return false;
}

void LoadResultIntoEntry(game::Entry& e, title::ThreadResultData* result) {
    if (result) {
        e.status = result->status;
        e.lang = result->lang;
    }
}

// blocking control load, only used when sorting by name.
void LoadControlEntry(game::Entry& e) {
    if (e.status != title::NacpLoadStatus::Loaded) {
        LoadResultIntoEntry(e, title::Get(e.app_id));
    }
}

// identifies the exact set of content that is installed for an application.
// the order that ns returns the metas in isn't documented, so sort first to
// keep the value stable across scans.
auto MakeFingerprint(const title::MetaEntries& entries) -> u64 {
    std::vector<std::array<u64, 2>> keys;
    keys.reserve(entries.size());

    for (const auto& e : entries) {
        keys.emplace_back(std::array<u64, 2>{
            e.application_id,
            ((u64)e.version << 16) | ((u64)e.meta_type << 8) | (u64)e.storageID,
        });
    }

    std::ranges::sort(keys);

    // fnv-1a.
    u64 hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](u64 value) {
        for (int i = 0; i < 8; i++) {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 0x100000001B3ULL;
        }
    };

    mix(keys.size());
    for (const auto& key : keys) {
        mix(key[0]);
        mix(key[1]);
    }

    return hash;
}

// draws the outline and the empty portion of a bar, returns the area that
// the filled segments should be drawn into.
Vec4 DrawBarFrame(NVGcontext* vg, Theme* theme, const Vec4& v) {
    gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_TEXT_INFO), BAR_ROUNDING);
    gfx::drawRect(vg, v.x + 1, v.y + 1, v.w - 2, v.h - 2, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), BAR_ROUNDING);
    return Vec4{v.x + 2, v.y + 2, v.w - 4, v.h - 4};
}

// draws value/total of the bar, starting at offset, returns the new offset.
float DrawBarSegment(NVGcontext* vg, Theme* theme, const Vec4& inner, float offset, s64 value, s64 total, ThemeEntryID id) {
    if (value <= 0 || total <= 0 || offset >= inner.w) {
        return offset;
    }

    auto width = (float)((double)value / (double)total * (double)inner.w);
    width = std::min(width, inner.w - offset);
    if (width <= 0.f) {
        return offset;
    }

    gfx::drawRect(vg, inner.x + offset, inner.y, width, inner.h, theme->GetColour(id), BAR_ROUNDING);
    return offset + width;
}

} // namespace

Menu::Menu(u32 flags) : MenuBase{"Data Management"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::A, Action{"View Content"_i18n, [this](){
            if (!HasEntry()) {
                return;
            }
            App::Push<game::meta::Menu>(GetEntry().game);
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("Storage Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            SidebarEntryArray::Items sort_items;
            sort_items.push_back("Size"_i18n);
            sort_items.push_back("Title"_i18n);
            sort_items.push_back("Title ID"_i18n);

            SidebarEntryArray::Items order_items;
            order_items.push_back("Descending"_i18n);
            order_items.push_back("Ascending"_i18n);

            SidebarEntryArray::Items filter_items;
            filter_items.push_back("All"_i18n);
            filter_items.push_back("microSD card"_i18n);
            filter_items.push_back("System memory"_i18n);

            options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
                m_sort.Set(index_out);
                FilterAndSort();
                SetIndex(0);
            }, m_sort.Get());

            options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
                m_order.Set(index_out);
                FilterAndSort();
                SetIndex(0);
            }, m_order.Get());

            options->Add<SidebarEntryArray>("Storage"_i18n, filter_items, [this](s64& index_out){
                m_filter.Set(index_out);
                FilterAndSort();
                SetIndex(0);
            }, m_filter.Get(),
                "Only list games that have content installed on the selected storage."_i18n);

            options->Add<SidebarEntryCallback>("Refresh"_i18n, [this](){
                m_dirty = true;
                App::PopToMenu();
            }, "Looks for new or removed games. Sizes are only recalculated for games whose content has changed."_i18n);

            options->Add<SidebarEntryCallback>("Rebuild cache"_i18n, [this](){
                ClearCache();
                m_dirty = true;
                App::PopToMenu();
            }, "Discards the saved sizes and measures every game again from scratch."_i18n);
        }})
    );

    const Vec4 v{485, GetY() + 1.f + 42.f, 720, 70};
    m_list = std::make_unique<List>(1, 7, m_pos, v);

    ns::Initialize();
    title::Init();
}

Menu::~Menu() {
    // a scan that was left part way through still measured some games,
    // don't throw that work away.
    if (m_cache_dirty) {
        SaveCache();
    }

    FreeEntries();
    SetBoost(false);
    title::Exit();
    ns::Exit();
}

void Menu::SetBoost(bool enable) {
    if (m_boost == enable) {
        return;
    }

    m_boost = enable;
    App::SetBoostMode(enable);
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (m_dirty) {
        ScanRecords();
    }
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (m_dirty) {
        ScanRecords();
    }

    if (m_scanning) {
        ScanSizeStep();
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

    const auto pdata = GetPolledData();

    // left side panel, storage summary followed by the selected game.
    gfx::drawRect(vg, PANEL, theme->GetColour(ThemeEntryID_GRID));

    DrawStorageBar(vg, theme, PANEL_X, PANEL.y + 20, PANEL_W, "microSD card"_i18n, m_games_sd, pdata.sd_free, pdata.sd_total);
    DrawStorageBar(vg, theme, PANEL_X, PANEL.y + 144, PANEL_W, "System memory"_i18n, m_games_nand, pdata.emmc_free, pdata.emmc_total);

    gfx::drawRect(vg, PANEL_X, PANEL.y + 266, PANEL_W, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));

    DrawDetails(vg, theme);

    if (m_view.empty()) {
        const auto text = m_scanning ? "Calculating..."_i18n : "Empty..."_i18n;
        gfx::drawTextArgs(vg, 845.f, GetY() + GetH() / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", text.c_str());
        return;
    }

    int image_load_count = 0;
    const auto count = (s64)m_view.size();

    m_list->Draw(vg, theme, count, [this, count, &image_load_count](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        auto& entry = m_entries[m_view[i]];
        auto& game = entry.game;

        if (game.status == title::NacpLoadStatus::None) {
            title::PushAsync(game.app_id);
            game.status = title::NacpLoadStatus::Progress;
        } else if (game.status == title::NacpLoadStatus::Progress) {
            LoadResultIntoEntry(game, title::GetAsync(game.app_id));
        }

        // lazy load image.
        if (image_load_count < IMAGE_LOAD_MAX) {
            if (LoadControlImage(game, title::GetAsync(game.app_id))) {
                image_load_count++;
            }
        }

        const auto selected = m_index == i;
        auto text_id = ThemeEntryID_TEXT;
        auto info_id = ThemeEntryID_TEXT_INFO;

        if (selected) {
            text_id = info_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i + 1 < count) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        gfx::drawImage(vg, x + 14, y + 11, 48, 48, game.image ? game.image : App::GetDefaultImage(), 4);

        constexpr float text_x = 76;
        constexpr float bar_w = 470;
        m_scroll_name.Draw(vg, selected, x + text_x, y + 12, bar_w, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(text_id), game.GetName());

        // bar is relative to the largest game in the list, which makes the
        // difference between entries readable no matter the library size.
        const Vec4 bar{x + text_x, y + 44, bar_w, 12};
        const auto inner = DrawBarFrame(vg, theme, bar);
        DrawBarSegment(vg, theme, inner, 0.f, entry.size_total, m_largest, ThemeEntryID_PROGRESSBAR);

        gfx::drawTextArgs(vg, x + w - 16, y + 16, 21.f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(text_id), "%s", utils::formatSizeStorage(entry.size_total).c_str());

        std::string location;
        if (entry.size_sd && entry.size_nand) {
            location = "microSD card"_i18n + " + " + "System memory"_i18n;
        } else if (entry.size_nand) {
            location = "System memory"_i18n;
        } else {
            location = "microSD card"_i18n;
        }

        gfx::drawTextArgs(vg, x + w - 16, y + 44, 15.f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(info_id), "%s", location.c_str());
    });
}

void Menu::DrawStorageBar(NVGcontext* vg, Theme* theme, float x, float y, float w, const std::string& name, s64 games, s64 free, s64 total) const {
    // GetPolledData() reports 1/1 when the size lookup fails, guard against
    // drawing a completely full bar in that case.
    if (total <= 1) {
        total = 0;
        free = 0;
    }

    const auto used = std::max<s64>(0, total - free);
    const auto other = std::max<s64>(0, used - games);

    gfx::drawTextArgs(vg, x, y, 19.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", name.c_str());
    gfx::drawTextArgs(vg, x + w, y + 2, 16.f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s / %s", utils::formatSizeStorage(used).c_str(), utils::formatSizeStorage(total).c_str());

    const Vec4 bar{x, y + 26, w, 14};
    const auto inner = DrawBarFrame(vg, theme, bar);
    auto offset = DrawBarSegment(vg, theme, inner, 0.f, games, total, ThemeEntryID_PROGRESSBAR);
    DrawBarSegment(vg, theme, inner, offset, other, total, ThemeEntryID_TEXT_INFO);

    // legend, one row per segment.
    const auto draw_legend = [&](float row_y, ThemeEntryID colour, bool outline, const std::string& label, s64 value) {
        const Vec4 swatch{x, row_y + 2, 12, 12};
        if (outline) {
            gfx::drawRect(vg, swatch, theme->GetColour(ThemeEntryID_TEXT_INFO), BAR_ROUNDING);
            gfx::drawRect(vg, swatch.x + 1, swatch.y + 1, swatch.w - 2, swatch.h - 2, theme->GetColour(colour), BAR_ROUNDING);
        } else {
            gfx::drawRect(vg, swatch, theme->GetColour(colour), BAR_ROUNDING);
        }

        gfx::drawTextArgs(vg, x + 20, row_y, 16.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", label.c_str());
        gfx::drawTextArgs(vg, x + w, row_y, 16.f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", utils::formatSizeStorage(value).c_str());
    };

    draw_legend(y + 50, ThemeEntryID_PROGRESSBAR, false, "Games"_i18n, games);
    draw_legend(y + 72, ThemeEntryID_TEXT_INFO, false, "Other"_i18n, other);
    draw_legend(y + 94, ThemeEntryID_PROGRESSBAR_BACKGROUND, true, "Free"_i18n, free);
}

void Menu::DrawDetails(NVGcontext* vg, Theme* theme) {
    if (!HasEntry()) {
        return;
    }

    const auto& e = GetEntry();
    const auto& game = e.game;

    gfx::drawImage(vg, PANEL_X, PANEL.y + 284, 84, 84, game.image ? game.image : App::GetDefaultImage(), 5);

    const auto text_x = PANEL_X + 100;
    const auto text_w = PANEL_W - 100;
    m_scroll_detail_name.Draw(vg, true, text_x, PANEL.y + 286, text_w, 19.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), game.GetName());
    gfx::drawTextArgs(vg, text_x, PANEL.y + 314, 15.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%016lX", game.app_id);
    gfx::drawTextArgs(vg, text_x, PANEL.y + 340, 22.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", utils::formatSizeStorage(e.size_total).c_str());

    const auto draw_row = [&](float row_y, const std::string& label, s64 value) {
        gfx::drawTextArgs(vg, PANEL_X, row_y, 17.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", label.c_str());
        gfx::drawTextArgs(vg, PANEL_X + PANEL_W, row_y, 17.f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", utils::formatSizeStorage(value).c_str());
    };

    draw_row(PANEL.y + 396, "Base game"_i18n, e.size_base);
    draw_row(PANEL.y + 422, "Update data"_i18n, e.size_update);
    draw_row(PANEL.y + 448, "DLC"_i18n, e.size_dlc);

    gfx::drawRect(vg, PANEL_X, PANEL.y + 478, PANEL_W, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));

    draw_row(PANEL.y + 490, "microSD card"_i18n, e.size_sd);
    draw_row(PANEL.y + 516, "System memory"_i18n, e.size_nand);
}

void Menu::SetIndex(s64 index) {
    if (m_view.empty()) {
        m_index = 0;
    } else {
        m_index = std::clamp<s64>(index, 0, (s64)m_view.size() - 1);
    }

    if (!m_index) {
        m_list->SetYoff(0);
    }

    UpdateSubHeading();
}

void Menu::UpdateSubHeading() {
    const auto count = m_view.size();
    const auto index = count ? m_index + 1 : 0;
    this->SetSubHeading(std::to_string(index) + " / " + std::to_string(count));

    // with a warm cache the list is already on screen, so the scan is a
    // background detail rather than something to wait on.
    if (m_scanning && m_view.empty()) {
        this->SetTitleSubHeading(
            "Calculating..."_i18n + " " + std::to_string(m_scan_index) + " / " + std::to_string(m_entries.size()));
    } else {
        this->SetTitleSubHeading(
            std::to_string(count) + " " + "Games"_i18n + " | " + utils::formatSizeStorage(m_games_sd + m_games_nand));
    }
}

void Menu::ScanRecords() {
    constexpr auto ENTRY_CHUNK_COUNT = 1000;

    SetBoost(true);
    FreeEntries();
    m_dirty = false;

    if (!m_cache_loaded) {
        m_cache_loaded = true;
        LoadCache();
    }

    m_cache_hits = 0;

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset{};

    while (true) {
        s32 record_count{};
        if (R_FAILED(nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count))) {
            log_write("[STORAGE] failed to list application records at offset: %d\n", offset);
            break;
        }

        // finished parsing all entries.
        if (!record_count) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            auto& entry = m_entries.emplace_back();
            entry.game.app_id = record_list[i].application_id;
            entry.game.last_event = record_list[i].last_event;
        }

        offset += record_count;
    }

    // seed from the cache so the list is usable on the first frame. every
    // entry is still verified below, that just happens in the background
    // instead of behind a "Calculating..." screen.
    for (auto& e : m_entries) {
        if (const auto it = m_cache.find(e.game.app_id); it != m_cache.end()) {
            ApplyCacheValue(e, it->second);
            m_games_sd += e.size_sd;
            m_games_nand += e.size_nand;
        }
    }

    m_scan_index = 0;
    m_scanning = !m_entries.empty();

    if (!m_scanning) {
        OnScanComplete();
        return;
    }

    FilterAndSort();
    SetIndex(0);
}

void Menu::ScanSizeStep() {
    const TimeStamp ts;
    const auto end = std::min(m_scan_index + SCAN_MAX_PER_FRAME, m_entries.size());

    for (std::size_t done = 0; m_scan_index < end; m_scan_index++, done++) {
        // always make some progress, then keep going for as long as the
        // frame budget allows.
        if (done >= SCAN_MIN_PER_FRAME && ts.GetMs() >= SCAN_BUDGET_MS) {
            break;
        }

        auto& e = m_entries[m_scan_index];

        // the seeded contribution is replaced by the verified one.
        m_games_sd -= e.size_sd;
        m_games_nand -= e.size_nand;

        CalculateSize(e);

        m_games_sd += e.size_sd;
        m_games_nand += e.size_nand;
    }

    if (m_scan_index >= m_entries.size()) {
        OnScanComplete();
    } else {
        UpdateSubHeading();
    }
}

void Menu::OnScanComplete() {
    m_scanning = false;
    SetBoost(false);

    // the list was already browsable during the scan, so keep whatever the
    // user had highlighted rather than snapping back to the top.
    const u64 selected_id = HasEntry() ? GetEntry().game.app_id : 0;

    // drop cache entries for games that are no longer installed. this is only
    // safe here, where every application record has just been visited.
    std::unordered_set<u64> live;
    live.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (e.size_total > 0) {
            live.emplace(e.game.app_id);
        }
    }

    if (std::erase_if(m_cache, [&live](const auto& entry) { return !live.contains(entry.first); })) {
        m_cache_dirty = true;
    }

    // records without installed content (archived games, or a game card that
    // isn't inserted) don't use any space, so drop them from the list.
    std::erase_if(m_entries, [](const auto& e) {
        return e.size_total <= 0;
    });

    log_write("[STORAGE] scanned %zu games using %s (%zu from cache)\n", m_entries.size(), utils::formatSizeStorage(m_games_sd + m_games_nand).c_str(), m_cache_hits);

    if (m_cache_dirty) {
        SaveCache();
    }

    FilterAndSort();

    s64 index = 0;
    for (u32 i = 0; i < m_view.size(); i++) {
        if (m_entries[m_view[i]].game.app_id == selected_id) {
            index = i;
            break;
        }
    }

    SetIndex(index);
}

void Menu::ClearSizes(Entry& e) {
    e.size_base = 0;
    e.size_update = 0;
    e.size_dlc = 0;
    e.size_sd = 0;
    e.size_nand = 0;
    e.size_total = 0;
}

void Menu::ApplyCacheValue(Entry& e, const CacheValue& value) {
    e.size_base = value.size_base;
    e.size_update = value.size_update;
    e.size_dlc = value.size_dlc;
    e.size_sd = value.size_sd;
    e.size_nand = value.size_nand;
    e.size_total = value.size_sd + value.size_nand;
}

void Menu::CalculateSize(Entry& e) {
    // anything already seeded from the cache is unverified, drop it and
    // rebuild from what ncm reports now.
    ClearSizes(e);

    title::MetaEntries meta_entries;
    if (R_FAILED(title::GetMetaEntries(e.game.app_id, meta_entries))) {
        return;
    }

    // listing the metas costs two ipc calls, walking them costs several more
    // per meta. when the fingerprint still matches, the walk is skipped and
    // the previously measured sizes are reused.
    const auto fingerprint = MakeFingerprint(meta_entries);

    if (const auto it = m_cache.find(e.game.app_id); it != m_cache.end() && it->second.fingerprint == fingerprint) {
        ApplyCacheValue(e, it->second);
        m_cache_hits++;
        return;
    }

    for (const auto& status : meta_entries) {
        // game card content lives on the cart, not on the console.
        if (status.storageID != NcmStorageId_SdCard && status.storageID != NcmStorageId_BuiltInUser) {
            continue;
        }

        game::NcmMetaData meta;
        if (R_FAILED(game::GetNcmMetaFromMetaStatus(status, meta))) {
            continue;
        }

        std::vector<NcmContentInfo> infos;
        if (R_FAILED(ncm::GetContentInfos(meta.db, &meta.key, infos))) {
            continue;
        }

        s64 size{};
        for (const auto& info : infos) {
            u64 content_size;
            ncmContentInfoSizeToU64(&info, &content_size);
            size += content_size;
        }

        switch (status.meta_type) {
            case NcmContentMetaType_Application:
                e.size_base += size;
                break;
            case NcmContentMetaType_Patch:
                e.size_update += size;
                break;
            case NcmContentMetaType_AddOnContent:
            case NcmContentMetaType_DataPatch:
                e.size_dlc += size;
                break;
        }

        if (status.storageID == NcmStorageId_SdCard) {
            e.size_sd += size;
        } else {
            e.size_nand += size;
        }

        e.size_total += size;
    }

    if (e.size_total > 0) {
        m_cache[e.game.app_id] = CacheValue{fingerprint, e.size_base, e.size_update, e.size_dlc, e.size_sd, e.size_nand};
        m_cache_dirty = true;
    } else if (m_cache.erase(e.game.app_id)) {
        // nothing is installed on the console anymore.
        m_cache_dirty = true;
    }
}

void Menu::LoadCache() {
    m_cache.clear();
    m_cache_dirty = false;

    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult())) {
        return;
    }

    std::vector<u8> data;
    if (R_FAILED(fs.read_entire_file(CACHE_PATH, data)) || data.size() < sizeof(CacheHeader)) {
        return;
    }

    CacheHeader header;
    std::memcpy(&header, data.data(), sizeof(header));

    if (header.magic != CACHE_MAGIC || header.version != CACHE_VERSION) {
        log_write("[STORAGE] discarding cache, magic / version mismatch\n");
        return;
    }

    // tolerate a truncated file rather than throwing the whole cache away.
    const auto available = (data.size() - sizeof(CacheHeader)) / sizeof(CacheRecord);
    const auto count = std::min<u64>(header.count, available);

    m_cache.reserve(count);
    for (u64 i = 0; i < count; i++) {
        CacheRecord record;
        std::memcpy(&record, data.data() + sizeof(CacheHeader) + i * sizeof(CacheRecord), sizeof(record));

        m_cache.emplace(record.app_id, CacheValue{
            record.fingerprint,
            record.size_base, record.size_update, record.size_dlc,
            record.size_sd, record.size_nand,
        });
    }

    log_write("[STORAGE] loaded %zu cached sizes\n", m_cache.size());
}

void Menu::SaveCache() {
    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult())) {
        return;
    }

    std::vector<u8> data(sizeof(CacheHeader) + m_cache.size() * sizeof(CacheRecord));

    const CacheHeader header{CACHE_MAGIC, CACHE_VERSION, m_cache.size()};
    std::memcpy(data.data(), &header, sizeof(header));

    auto offset = sizeof(CacheHeader);
    for (const auto& [app_id, value] : m_cache) {
        const CacheRecord record{
            app_id, value.fingerprint,
            value.size_base, value.size_update, value.size_dlc,
            value.size_sd, value.size_nand,
        };

        std::memcpy(data.data() + offset, &record, sizeof(record));
        offset += sizeof(record);
    }

    fs.CreateDirectoryRecursivelyWithPath(CACHE_PATH);

    const auto rc = fs.write_entire_file(CACHE_PATH, data);
    if (R_FAILED(rc)) {
        log_write("[STORAGE] failed to write size cache: 0x%X\n", R_VALUE(rc));
        return;
    }

    m_cache_dirty = false;
    log_write("[STORAGE] wrote %zu cached sizes\n", m_cache.size());
}

void Menu::ClearCache() {
    m_cache.clear();
    m_cache_loaded = true;
    m_cache_dirty = false;

    fs::FsNativeSd fs;
    if (R_SUCCEEDED(fs.GetFsOpenResult())) {
        fs.DeleteFile(CACHE_PATH);
    }
}

void Menu::FilterAndSort() {
    const auto filter = m_filter.Get();

    m_view.clear();
    m_view.reserve(m_entries.size());

    for (u32 i = 0; i < m_entries.size(); i++) {
        const auto& e = m_entries[i];

        // not measured yet, or nothing installed on the console.
        if (e.size_total <= 0) {
            continue;
        }
        if (filter == FilterType_Sd && !e.size_sd) {
            continue;
        }
        if (filter == FilterType_Nand && !e.size_nand) {
            continue;
        }

        m_view.emplace_back(i);
    }

    switch (m_sort.Get()) {
        case SortType_Title:
            for (auto i : m_view) {
                LoadControlEntry(m_entries[i].game);
            }
            std::ranges::sort(m_view, [this](auto lhs, auto rhs) {
                return strcasecmp(m_entries[lhs].GetName(), m_entries[rhs].GetName()) < 0;
            });
            break;

        case SortType_TitleID:
            std::ranges::sort(m_view, [this](auto lhs, auto rhs) {
                return m_entries[lhs].game.app_id < m_entries[rhs].game.app_id;
            });
            break;

        case SortType_Size:
        default:
            std::ranges::sort(m_view, [this](auto lhs, auto rhs) {
                return m_entries[lhs].size_total > m_entries[rhs].size_total;
            });
            break;
    }

    if (m_order.Get() == OrderType_Ascending) {
        std::ranges::reverse(m_view);
    }

    // the per game bars are drawn relative to the biggest entry.
    m_largest = 1;
    for (auto i : m_view) {
        m_largest = std::max(m_largest, m_entries[i].size_total);
    }

    m_scroll_name.Reset();
    m_scroll_detail_name.Reset();
}

void Menu::FreeEntries() {
    auto vg = App::GetVg();

    for (auto& e : m_entries) {
        nvgDeleteImage(vg, e.game.image);
        e.game.image = 0;
    }

    m_entries.clear();
    m_view.clear();
    m_index = 0;
    m_scan_index = 0;
    m_scanning = false;
    m_games_sd = 0;
    m_games_nand = 0;
    m_largest = 1;
}

} // namespace sphaira::ui::menu::storage

#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/menus/game_menu.hpp"
#include "ui/list.hpp"
#include "ui/scrolling_text.hpp"
#include "option.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace sphaira::ui::menu::storage {

// space used by a single application, split by content type and by storage.
// game card content is never counted, as it lives on the cart rather than
// on the console.
struct Entry {
    game::Entry game{};

    s64 size_base{};
    s64 size_update{};
    s64 size_dlc{};

    s64 size_sd{};
    s64 size_nand{};

    s64 size_total{};

    auto GetName() const -> const char* {
        return game.GetName();
    }

    auto GetAuthor() const -> const char* {
        return game.GetAuthor();
    }
};

enum SortType {
    SortType_Size,
    SortType_Title,
    SortType_TitleID,
};

enum OrderType {
    OrderType_Descending,
    OrderType_Ascending,
};

enum FilterType {
    FilterType_All,
    FilterType_Sd,
    FilterType_Nand,
};

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Storage"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    // a previously measured application. the fingerprint covers every
    // installed content meta, so it changes as soon as the game is updated,
    // has dlc added / removed, is moved between storages, or is deleted.
    struct CacheValue {
        u64 fingerprint{};
        s64 size_base{};
        s64 size_update{};
        s64 size_dlc{};
        s64 size_sd{};
        s64 size_nand{};
    };

    void SetIndex(s64 index);
    void UpdateSubHeading();

    // lists every application record, sizes are fetched afterwards.
    void ScanRecords();
    // sizes a handful of entries per call so that the ui stays responsive
    // whilst a large library is being measured.
    void ScanSizeStep();
    void OnScanComplete();
    void CalculateSize(Entry& e);
    // cpu boost is held for the duration of the scan rather than toggled
    // every frame.
    void SetBoost(bool enable);

    void LoadCache();
    void SaveCache();
    void ClearCache();

    static void ClearSizes(Entry& e);
    static void ApplyCacheValue(Entry& e, const CacheValue& value);

    void FilterAndSort();
    void FreeEntries();

    void DrawStorageBar(NVGcontext* vg, Theme* theme, float x, float y, float w, const std::string& name, s64 games, s64 free, s64 total) const;
    void DrawDetails(NVGcontext* vg, Theme* theme);

    auto HasEntry() const -> bool {
        return !m_view.empty() && m_index < (s64)m_view.size();
    }

    auto GetEntry() -> Entry& {
        return m_entries[m_view[m_index]];
    }

    auto GetEntry() const -> const Entry& {
        return m_entries[m_view[m_index]];
    }

private:
    static constexpr inline const char* INI_SECTION = "storage";

    // every application that owns installed content, owns the icons.
    std::vector<Entry> m_entries{};
    // indices into m_entries, filtered and sorted for display.
    std::vector<u32> m_view{};

    s64 m_index{};
    std::unique_ptr<List> m_list{};

    // index of the next entry in m_entries to be sized.
    std::size_t m_scan_index{};
    bool m_scanning{};
    bool m_dirty{true};
    bool m_boost{};

    // totals across every scanned entry.
    s64 m_games_sd{};
    s64 m_games_nand{};
    // largest entry in the current view, used to scale the per game bars.
    s64 m_largest{1};

    // measured sizes from previous scans, keyed by application id. loaded
    // from and written back to the sd card so that re-opening the menu
    // doesn't have to walk ncm again.
    std::unordered_map<u64, CacheValue> m_cache{};
    bool m_cache_loaded{};
    bool m_cache_dirty{};
    std::size_t m_cache_hits{};

    ScrollingText m_scroll_name{};
    ScrollingText m_scroll_detail_name{};

    option::OptionLong m_sort{INI_SECTION, "sort", SortType::SortType_Size};
    option::OptionLong m_order{INI_SECTION, "order", OrderType::OrderType_Descending};
    option::OptionLong m_filter{INI_SECTION, "filter", FilterType::FilterType_All};
};

} // namespace sphaira::ui::menu::storage

#pragma once

#include "album_caps.hpp"
#include "option.hpp"
#include "title_info.hpp"
#include "ui/list.hpp"
#include "ui/menus/grid_menu_base.hpp"
#include "utils/thread.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace sphaira::ui::menu::album {

struct Entry {
    CapsAlbumFileId file_id{};
    u64 size{};
    // yyyymmddhhmmss, built from the capture's datetime, for sorting.
    u64 date{};

    std::string name{};  // date and time of the capture.
    std::string title{}; // the game it was taken in, once that is known.
    std::string info{};  // type and size.

    title::NacpLoadStatus status{title::NacpLoadStatus::None};

    int image{};
    int image_w{};
    int image_h{};
    // set when the thumbnail cannot be loaded, so it isn't retried every frame.
    bool image_failed{};
    bool selected{};

    auto IsVideo() const -> bool {
        return file_id.content == CapsAlbumFileContents_Movie
            || file_id.content == CapsAlbumFileContents_ExtraMovie;
    }
};

enum SortType {
    SortType_Date,
    SortType_Size,
};

enum OrderType {
    OrderType_Descending,
    OrderType_Ascending,
};

enum StorageType {
    StorageType_Sd,
    StorageType_Nand,
};

using LayoutType = grid::LayoutType;

// tells an open album menu that the album has changed, ie. a capture was
// taken whilst sphaira was running. safe to call from any thread.
void SignalChange();

struct Menu final : grid::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Album"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    // a game with captures in the album, for the filter list.
    struct GameInfo {
        u64 application_id{};
        // the most recent capture taken in it, which is the order the
        // console's own album lists games in.
        u64 newest{};
        s64 count{};
    };

    struct ScanResult {
        std::vector<Entry> entries{};
        std::vector<GameInfo> games{};
        Result rc{};
    };

private:
    void SetIndex(s64 index);
    void Scan();
    // the listing runs off the ui thread, the album service can take a moment
    // over it and the menu should not be stuck behind that.
    void ScanWorker(CapsAlbumStorage storage);
    void FinishScan();
    void Sort();
    // rebuilds the list of entries on show, and the counts that go with it.
    void ApplyFilter();
    auto GetGameName(u64 application_id) -> std::string;
    auto FindGame(u64 application_id) -> const GameInfo*;
    void OnLayoutChange();
    void DisplayOptions();

    void FreeEntry(Entry& e);
    void FreeEntries();
    // keeps the number of decoded thumbnails bounded, an album can hold
    // thousands of entries and each texture costs vram.
    void EvictImages(s64 first_drawn, s64 last_drawn);
    auto LoadImage(Entry& e) -> bool;
    void LoadTitle(Entry& e);

    void OnEntrySelected();
    void DeleteSelected();
    void ClearSelection();
    auto GetSelected() -> std::vector<std::reference_wrapper<Entry>>;

    auto GetStorage() -> CapsAlbumStorage {
        return m_storage.Get() == StorageType_Nand ? CapsAlbumStorage_Nand : CapsAlbumStorage_Sd;
    }

private:
    static constexpr inline const char* INI_SECTION = "album";

    std::vector<Entry> m_entries{};
    // indices into m_entries, in the order they are shown. everything the ui
    // touches goes through this, so that filtering by game is just a rebuild.
    std::vector<s64> m_view{};
    // every game in the album, most recently captured first.
    std::vector<GameInfo> m_games{};
    // 0 shows everything.
    u64 m_filter{};

    s64 m_index{};
    s64 m_selected_count{};
    s64 m_image_count{};
    s64 m_screenshot_count{};
    s64 m_video_count{};
    s64 m_total_size{};
    std::unique_ptr<List> m_list{};

    // a failed scan is reported on the next update, the menu isn't on the
    // widget stack yet whilst it is being constructed.
    Result m_scan_rc{};
    bool m_caps_init{};
    bool m_dirty{};

    // filled in by the scan thread, only read once m_scan_done is set.
    ScanResult m_scan_data{};
    std::unique_ptr<utils::Async> m_scan_thread{};
    std::atomic_bool m_scan_done{};
    bool m_scanning{};

    option::OptionLong m_sort{INI_SECTION, "sort", SortType::SortType_Date};
    option::OptionLong m_order{INI_SECTION, "order", OrderType::OrderType_Descending};
    option::OptionLong m_layout{INI_SECTION, "layout", LayoutType::LayoutType_Grid};
    option::OptionLong m_storage{INI_SECTION, "storage", StorageType::StorageType_Sd};
    option::OptionLong m_web_port{INI_SECTION, "web_port", 8080};
};

} // namespace sphaira::ui::menu::album

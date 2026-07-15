#pragma once

#include "ui/menus/grid_menu_base.hpp"
#include "ui/scrollable_text.hpp"
#include "ui/scrolling_text.hpp"
#include "ui/list.hpp"
#include "fs.hpp"
#include "option.hpp"
#include "repo_manager.hpp"
#include <span>

namespace sphaira::ui::menu::appstore {

struct ManifestEntry {
    char command;
    fs::FsPath path;
};

using ManifestEntries = std::vector<ManifestEntry>;

enum class ImageDownloadState {
    None, // not started
    Progress, // Download started
    Done, // finished downloading
    Failed, // attempted to download but failed
};

struct LazyImage {
    LazyImage() = default;
    ~LazyImage();
    int image{};
    int w{}, h{};
    bool tried_cache{};
    bool cached{};
    ImageDownloadState state{ImageDownloadState::None};
    u8 first_pixel[4]{};
};

enum class EntryStatus {
    Get,
    Installed,
    Local,
    Update,
};

struct Entry {
    std::string category{}; // todo: lable
    std::string binary{}; // optional, only valid for .nro
    std::string updated{}; // date of update
    std::string name{};
    std::string license{}; // optional
    std::string title{}; // same as name but with spaces
    std::string url{}; // url of repo (optional?)
    std::string repo_source{}; // source repo url
    std::string description{};
    std::string author{};
    std::string changelog{}; // optional
    u64 screens{}; // number of screenshots
    u64 extracted{}; // extracted size in KiB
    std::string version{};
    u64 filesize{}; // compressed size in KiB
    std::string details{};
    u64 app_dls{};
    std::string md5{}; // md5 of the zip

    LazyImage image{};
    u32 updated_num{};
    EntryStatus status{EntryStatus::Get};
};

// number to index m_entries to get entry
using EntryMini = u32;
struct Menu; // fwd

struct EntryMenu final : MenuBase {
    EntryMenu(Entry& entry, const LazyImage& default_icon, Menu& menu);
    ~EntryMenu();

    auto GetShortTitle() const -> const char* override { return "Entry"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    // void OnFocusGained() override;

    void ShowChangelogAction();
    void SetIndex(s64 index);

    void UpdateOptions();

private:
    struct Option {
        using Callback = std::function<void(void)>;

        Option(const std::string& dt, const std::string& ct, const Callback& f)
        : display_text{dt}, confirm_text{ct}, func{f} {}
        Option(const std::string& dt, const Callback& f)
        : display_text{dt}, func{f} {}

        const std::string display_text;
        const std::string confirm_text;
        const Callback func{};
    };

    Entry& m_entry;
    const LazyImage& m_default_icon;
    Menu& m_menu;

    s64 m_index{}; // where i am in the array
    std::vector<Option> m_options{};
    LazyImage m_banner{};
    std::unique_ptr<List> m_list{};

    std::unique_ptr<ScrollableText> m_details{};
    std::unique_ptr<ScrollableText> m_changelog{};
    ScrollableText* m_detail_changelog{};
    std::unique_ptr<ScrollableText> m_manifest_list{};

    bool m_show_changlog{};
    bool m_show_file_list{};
    ImageDownloadState m_file_list_state{};
};

enum Filter {
    Filter_All,
    Filter_Games,
    Filter_Emulators,
    Filter_Tools,
    Filter_Advanced,
    Filter_Themes,
    Filter_Legacy,
    Filter_Misc,
    Filter_MAX,
};

enum SortType {
    SortType_Updated,
    SortType_Downloads,
    SortType_Size,
    SortType_Alphabetical,
};

enum OrderType {
    OrderType_Descending,
    OrderType_Ascending,
};

using LayoutType = grid::LayoutType;

struct Menu final : grid::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Store"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

    void SetAuthor();

    auto GetEntry(s64 i) -> Entry& {
        return m_entries[m_entries_current[i]];
    }

    auto GetEntry() -> Entry& {
        return GetEntry(m_index);
    }

    auto SetDirty() {
        m_dirty = true;
    }

private:
    void SetIndex(s64 index);
    void ScanHomebrew();
    void Sort();
    void SortAndFindLastFile();
    void SetFilter();
    void SetSearch(const std::string& term);
    void OnLayoutChange();
    void StartRepoDownload();

private:
    static constexpr inline const char* INI_SECTION = "appstore";

    std::vector<Entry> m_entries{};
    std::vector<EntryMini> m_entries_index[Filter_MAX]{};
    std::vector<EntryMini> m_entries_index_author{};
    std::vector<EntryMini> m_entries_index_search{};
    std::vector<EntryMini> m_entries_index_repo{};
    std::span<EntryMini> m_entries_current{};

    option::OptionLong m_filter{INI_SECTION, "filter", Filter::Filter_All};
    option::OptionLong m_sort{INI_SECTION, "sort", SortType::SortType_Updated};
    option::OptionLong m_order{INI_SECTION, "order", OrderType::OrderType_Descending};
    option::OptionLong m_layout{INI_SECTION, "layout", LayoutType::LayoutType_GridDetail};
    option::OptionString m_repo_filter{INI_SECTION, "repo_filter", ""};

    s64 m_index{}; // where i am in the array
    LazyImage m_default_image{};
    LazyImage m_update{};
    LazyImage m_get{};
    LazyImage m_local{};
    LazyImage m_installed{};
    ImageDownloadState m_repo_download_state{ImageDownloadState::None};
    std::vector<RepoConfig> m_repo_download_list{};
    std::size_t m_repo_download_index{};
    u32 m_repo_download_success{};
    std::unique_ptr<List> m_list{};

    std::string m_search_term{};
    std::string m_author_term{};
    s64 m_entry_search_jump_back{};
    s64 m_entry_author_jump_back{};
    bool m_is_search{};
    bool m_is_author{};
    bool m_dirty{}; // if set, does a sort
};

} // namespace sphaira::ui::menu::appstore

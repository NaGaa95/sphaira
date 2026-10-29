#pragma once

#include "ui/menus/grid_menu_base.hpp"
#include "ui/list.hpp"

#include "yati/container/base.hpp"
#include "yati/nx/keys.hpp"

#include "title_info.hpp"
#include "fs.hpp"
#include "option.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <span>

namespace sphaira::ui {
class SidebarEntryBase;
} // namespace sphaira::ui

namespace sphaira::ui::menu::game {

struct PlaytimeWorker;

struct Entry {
    u64 app_id{};
    u8 last_event{};
    NacpLanguageEntry lang{};
    int image{};
    bool selected{};
    u64 last_played{};
    u64 playtime{};
    u64 playtime_cached_last_played{};
    bool playtime_cached{};
    std::vector<u64> user_playtimes{};
    title::NacpLoadStatus status{title::NacpLoadStatus::None};
    // installed and gc_title are loaded once the entry is first drawn, as
    // they cost a few ipc calls per entry.
    bool content_loaded{};
    // ns counts nothing on a game card as installed.
    bool installed{true};
    // the record points at a game card, inserted or not.
    bool gc_title{};
    // the inserted game card holds this title.
    bool gc_inserted{};

    auto GetName() const -> const char* {
        return lang.name;
    }

    auto GetAuthor() const -> const char* {
        return lang.author;
    }

    // the record is kept but no content is installed, like the system's own
    // "Archive Software". a game card title with nothing installed is not
    // archived, it plays from its card.
    auto IsArchived() const -> bool {
        return !installed && !IsGameCard();
    }

    auto IsGameCard() const -> bool {
        return gc_title || gc_inserted;
    }
};

enum SortType {
    SortType_Updated = 0,
    SortType_Title = 1,
    SortType_TitleID = 2,
    SortType_LastPlayed = 3,
    SortType_TotalPlayTime = 4,
    SortType_Publisher = 5,
};

enum OrderType {
    OrderType_Descending,
    OrderType_Ascending,
};

using LayoutType = grid::LayoutType;

void SignalChange();

struct Menu final : grid::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Games"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void ScanHomebrew();
    void Filter();
    void Sort();
    void SortAndFindLastFile(bool scan);
    void FreeEntries();
    void OnLayoutChange();
    void LoadPlaytime();
    void StartPlaytimeWorker();
    void StopPlaytimeWorker(bool apply_results);
    void ApplyPlaytimeResults();
    void SyncEntryToMaster(const Entry& entry);
    void SetPlayStatsEnabled(bool enable);
    void SetMissingContentFilter(bool enable);
    void StartMissingContentScan();
    void DownloadAndScanMissingContent();
    void InvalidateMissingContentCache();
    void RefreshGameCard();

    auto IsPlayStatsEnabled() -> bool {
        return m_play_stats.Get();
    }

    auto GetSelectedEntries() const {
        std::vector<Entry> out;
        for (auto& e : m_entries) {
            if (e.selected) {
                out.emplace_back(e);
            }
        }

        if (!m_entries.empty() && out.empty()) {
            out.emplace_back(m_entries[m_index]);
        }

        return out;
    }

    void ClearSelection() {
        for (auto& e : m_entries) {
            e.selected = false;
        }

        m_selected_count = 0;
    }

    void DeleteGames();
    void ArchiveGames();
    void ExportOptions(bool to_nsz);
    void DumpGames(u32 flags, bool to_nsz);
    void CreateSaves(AccountUid uid);

private:
    static constexpr inline const char* INI_SECTION = "games";
    static constexpr inline const char* INI_SECTION_DUMP = "dump";

    std::vector<Entry> m_entries{};
    std::vector<Entry> m_all_entries{};
    std::unordered_set<u64> m_missing_content_app_ids{};
    std::string m_search_query{};
    std::vector<AccountProfileBase> m_accounts{};
    s64 m_index{}; // where i am in the array
    s64 m_selected_count{};
    std::unique_ptr<List> m_list{};
    bool m_is_reversed{};
    bool m_dirty{};
    bool m_pdm_initialized{};
    bool m_missing_content_filter{};
    bool m_missing_content_scanned{};
    u64 m_missing_content_catalog_revision{};
    std::unique_ptr<PlaytimeWorker> m_playtime_worker{};

    // use for detection game card removal to force a refresh.
    Event m_gc_event{};
    FsEventNotifier m_gc_event_notifier{};
    // the card is mounted, or dropped, a moment after the event fires, so it
    // is polled for a while afterwards.
    std::vector<u64> m_gc_app_ids{};
    u64 m_gc_poll_end{};
    u64 m_gc_poll_next{};

    option::OptionLong m_sort{INI_SECTION, "sort", SortType::SortType_Updated};
    option::OptionLong m_order{INI_SECTION, "order", OrderType::OrderType_Descending};
    option::OptionLong m_layout{INI_SECTION, "layout", LayoutType::LayoutType_Grid};
    option::OptionBool m_hide_forwarders{INI_SECTION, "hide_forwarders", false};
    // when disabled, pdm:qry is never opened and playlog.ini is never touched
    // by this menu, as both add per-entry io whilst navigating the list.
    option::OptionBool m_play_stats{INI_SECTION, "play_stats", true};
};

struct NcmMetaData {
    // points to global service, do not close manually!
    NcmContentStorage* cs{};
    NcmContentMetaDatabase* db{};
    u64 app_id{};
    NcmContentMetaKey key{};
};

Result GetMetaEntries(const Entry& e, title::MetaEntries& out, u32 flags = title::ContentFlag_All);

Result GetNcmMetaFromMetaStatus(const NsApplicationContentMetaStatus& status, NcmMetaData& out);
void DeleteMetaEntries(u64 app_id, int image, const std::string& name, const title::MetaEntries& entries);
// removes the content of each application but keeps its record, icon and
// saves, same as the system's "Archive Software".
void ArchiveEntries(std::vector<Entry> targets);
// greys out the archive option when the entry has nothing installed.
void DependsArchive(SidebarEntryBase* archive, const Entry& e);

struct TikEntry {
    FsRightsId id{};
    u8 key_gen{};
    std::vector<u8> tik_data{};
    std::vector<u8> cert_data{};
};

struct NspEntry {
    // application name.
    std::string application_name{};
    // name of the nsp (name [id][v0][BASE].nsp).
    fs::FsPath path{};
    // tickets and cert data, will be empty if title key crypto isn't used.
    std::vector<TikEntry> tickets{};
    // all the collections for this nsp, such as nca's and tickets.
    std::vector<yati::container::CollectionEntry> collections{};
    // raw nsp data (header, file table and string table).
    std::vector<u8> nsp_data{};
    // size of the entier nsp.
    s64 nsp_size{};
    // copy of ncm cs, it is not closed.
    NcmContentStorage cs{};
    // copy of the icon, if invalid, it will use the default icon.
    int icon{};

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read);

private:
    static auto InRange(s64 off, s64 offset, s64 size) -> bool {
        return off < offset + size && off >= offset;
    }

    static auto ClipSize(s64 off, s64 size, s64 file_size) -> s64 {
        return std::min(size, file_size - off);
    }
};

struct ContentInfoEntry {
    NsApplicationContentMetaStatus status{};
    std::vector<NcmContentInfo> content_infos{};
    std::vector<NcmRightsId> ncm_rights_id{};
};

auto BuildNspPath(const Entry& e, std::string_view export_name, const NsApplicationContentMetaStatus& status, bool to_nsz = false) -> fs::FsPath;
Result BuildContentEntry(const NsApplicationContentMetaStatus& status, ContentInfoEntry& out, bool to_nsz = false);
Result BuildNspEntry(const Entry& e, std::string_view export_name, const ContentInfoEntry& info, const keys::Keys& keys, NspEntry& out, bool to_nsz = false);
Result BuildNspEntries(Entry& e, const title::MetaEntries& meta_entries, std::vector<NspEntry>& out, bool to_nsz = false);
Result BuildNspEntries(Entry& e, u32 flags, std::vector<NspEntry>& out, bool to_nsz = false);

// dumps the array of nsp entries.
void DumpNsp(const std::vector<NspEntry>& entries, bool to_nsz);

} // namespace sphaira::ui::menu::game

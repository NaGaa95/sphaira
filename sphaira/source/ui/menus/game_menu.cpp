#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "dumper.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "image.hpp"
#include "nx_versions.hpp"
#include "swkbd.hpp"

#include "utils/utils.hpp"
#include "utils/thread.hpp"
#include "utils/nsz_dumper.hpp"

#include "ui/menus/game_menu.hpp"
#include "ui/menus/game_meta_menu.hpp"
#include "ui/menus/save_menu.hpp"
#include "ui/menus/gc_menu.hpp" // remove when gc event pr is merged.
#include "ui/sidebar.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"

#include "yati/nx/ncm.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ns.hpp"
#include "yati/nx/es.hpp"
#include "yati/container/base.hpp"
#include "yati/container/nsp.hpp"

#include <utility>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>
#include <minIni.h>

namespace sphaira::ui::menu::game {
namespace {

std::atomic_bool g_change_signalled{};

struct NspSource final : dump::BaseSource {
    NspSource(const std::vector<NspEntry>& entries) : m_entries{entries} {
        m_is_file_based_emummc = App::IsFileBaseEmummc();
    }

    Result Read(const std::string& path, void* buf, s64 off, s64 size, u64* bytes_read) override {
        const auto it = std::ranges::find_if(m_entries, [&path](auto& e){
            return path.find(e.path.s) != path.npos;
        });
        R_UNLESS(it != m_entries.end(), Result_GameBadReadForDump);

        const auto rc = it->Read(buf, off, size, bytes_read);
        if (m_is_file_based_emummc) {
            svcSleepThread(2e+6); // 2ms
        }
        return rc;
    }

    Result Read(const std::string& path, void* buf, s64 off, s64 size) {
        u64 bytes_read = 0;
        return Read(path, buf, off, size, &bytes_read);
    }

    auto GetName(const std::string& path) const -> std::string {
        const auto it = std::ranges::find_if(m_entries, [&path](auto& e){
            return path.find(e.path.s) != path.npos;
        });

        if (it != m_entries.end()) {
            return it->application_name;
        }

        return {};
    }

    auto GetSize(const std::string& path) const -> s64 {
        const auto it = std::ranges::find_if(m_entries, [&path](auto& e){
            return path.find(e.path.s) != path.npos;
        });

        if (it != m_entries.end()) {
            return it->nsp_size;
        }

        return 0;
    }

    auto GetIcon(const std::string& path) const -> int override {
        const auto it = std::ranges::find_if(m_entries, [&path](auto& e){
            return path.find(e.path.s) != path.npos;
        });

        if (it != m_entries.end()) {
            return it->icon;
        }

        return App::GetDefaultImage();
    }

    Result GetEntryFromPath(const std::string& path, NspEntry& out) const {
        const auto it = std::ranges::find_if(m_entries, [&path](auto& e){
            return path.find(e.path.s) != path.npos;
        });
        R_UNLESS(it != m_entries.end(), Result_GameBadReadForDump);

        out = *it;
        R_SUCCEED();
    }

private:
    std::vector<NspEntry> m_entries{};
    bool m_is_file_based_emummc{};
};

#ifdef ENABLE_NSZ
Result NszExport(ProgressBox* pbox, const keys::Keys& keys, dump::BaseSource* _source, dump::WriteSource* writer, const fs::FsPath& path) {
    auto source = (NspSource*)_source;

    NspEntry entry;
    R_TRY(source->GetEntryFromPath(path, entry));

    const auto nca_creator = [&entry](const nca::Header& header, const keys::KeyEntry& title_key, const utils::nsz::Collection& collection) {
        const auto content_id = ncm::GetContentIdFromStr(collection.name.c_str());
        return std::make_unique<nca::NcaReader>(
            header, &title_key, collection.size,
            std::make_shared<ncm::NcmSource>(&entry.cs, &content_id)
        );
    };

    auto& collections = entry.collections;
    s64 read_offset = entry.nsp_data.size();
    s64 write_offset = entry.nsp_data.size();

    R_TRY(utils::nsz::NszExport(pbox, nca_creator, read_offset, write_offset, collections, keys, source, writer, path));

    // zero base the offsets.
    for (auto& collection : collections) {
        collection.offset -= entry.nsp_data.size();
    }

    // build new nsp collection with the updated offsets and sizes.
    s64 nsp_size = 0;
    const auto nsp_data = yati::container::Nsp::Build(collections, nsp_size);
    R_TRY(writer->Write(nsp_data.data(), 0, nsp_data.size()));

    // update with actual size.
    R_TRY(writer->SetSize(nsp_size));

    R_SUCCEED();
}
#endif // ENABLE_NSZ

Result Notify(Result rc, const std::string& error_message) {
    if (R_FAILED(rc)) {
        App::Push<ui::ErrorBox>(rc,
            i18n::get(error_message)
        );
    } else {
        App::Notify("Success"_i18n);
    }

    return rc;
}

bool LoadControlImage(Entry& e, title::ThreadResultData* result) {
    if (!e.image && result && !result->icon.empty()) {
        TimeStamp ts;
        const auto image = ImageLoadFromMemory(result->icon, ImageFlag_JPEG);
        if (!image.data.empty()) {
            e.image = nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
            log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
            return true;
        }
    }

    return false;
}

void LoadResultIntoEntry(Entry& e, title::ThreadResultData* result) {
    if (result) {
        e.status = result->status;
        e.lang = result->lang;
        e.status = result->status;
    }
}

void LoadControlEntry(Entry& e, bool force_image_load = false) {
    if (e.status != title::NacpLoadStatus::Loaded) {
        LoadResultIntoEntry(e, title::Get(e.app_id));
    }

    if (force_image_load && e.status == title::NacpLoadStatus::Loaded) {
        LoadControlImage(e, title::Get(e.app_id));
    }
}

void FreeEntry(NVGcontext* vg, Entry& e) {
    nvgDeleteImage(vg, e.image);
    e.image = 0;
}

void LaunchEntry(const Entry& e) {
    const auto rc = appletRequestLaunchApplication(e.app_id, nullptr);
    Notify(rc, "Failed to launch application"_i18n);
}

Result CreateSave(u64 app_id, AccountUid uid) {
    u64 actual_size;
    auto data = std::make_unique<NsApplicationControlData>();
    R_TRY(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, data.get(), sizeof(NsApplicationControlData), &actual_size));

    FsSaveDataAttribute attr{};
    attr.application_id = app_id;
    attr.uid = uid;
    attr.save_data_type = FsSaveDataType_Account;

    FsSaveDataCreationInfo info{};
    info.save_data_size = data->nacp.user_account_save_data_size;
    info.journal_size = data->nacp.user_account_save_data_journal_size;
    info.available_size = data->nacp.user_account_save_data_size; // todo: check what this should be.
    info.owner_id = data->nacp.save_data_owner_id;
    info.save_data_space_id = FsSaveDataSpaceId_User;

    // https://switchbrew.org/wiki/Filesystem_services#CreateSaveDataFileSystem
    FsSaveDataMetaInfo meta{};
    meta.size = 0x40060;
    meta.type = FsSaveDataMetaType_Thumbnail;

    R_TRY(fsCreateSaveDataFileSystem(&attr, &info, &meta));

    R_SUCCEED();
}

} // namespace

struct PlaytimeWorker {
    struct Job {
        u64 app_id{};
        u64 last_played{};

        bool operator==(const Job&) const = default;
    };

    struct Data {
        u64 playtime{};
        std::vector<u64> user_playtimes{};
    };

    struct ResultData {
        Job job{};
        Data data{};
    };

    explicit PlaytimeWorker(const std::vector<AccountProfileBase>& accounts) : m_accounts{accounts} {
        ueventCreate(&m_uevent, true);
        mutexInit(&m_job_mutex);
        mutexInit(&m_result_mutex);
        m_running = true;

        if (R_FAILED(utils::CreateThread(&m_thread, ThreadFunc, this, 1024 * 32))) {
            m_running = false;
            return;
        }

        if (R_FAILED(threadStart(&m_thread))) {
            threadClose(&m_thread);
            m_running = false;
            return;
        }

        m_started = true;
    }

    ~PlaytimeWorker() {
        Stop();
    }

    bool IsRunning() const {
        return m_started && m_running;
    }

    void Stop() {
        if (!m_started) {
            return;
        }

        m_running = false;
        ueventSignal(&m_uevent);
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_started = false;
    }

    void Request(const Job& job) {
        if (!IsRunning()) {
            return;
        }

        SCOPED_MUTEX(&m_job_mutex);
        if ((m_active && *m_active == job) || (m_completed && *m_completed == job)) {
            m_pending.reset();
            return;
        }
        if (m_pending && *m_pending == job) {
            return;
        }

        m_pending = job;
        ueventSignal(&m_uevent);
    }

    auto TakeResults() -> std::vector<ResultData> {
        SCOPED_MUTEX(&m_result_mutex);
        std::vector<ResultData> out;
        std::swap(out, m_results);
        return out;
    }

    static auto Query(u64 app_id, const std::vector<AccountProfileBase>& accounts, const std::atomic_bool* running = nullptr) -> std::optional<Data> {
        Data data;
        data.user_playtimes.reserve(accounts.size());

        bool query_succeeded{};
        for (const auto& account : accounts) {
            if (running && !*running) {
                return std::nullopt;
            }

            PdmPlayStatistics stats{};
            u64 user_playtime{};
            if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(app_id, account.uid, true, &stats))) {
                user_playtime = stats.playtime;
                query_succeeded = true;
            }

            data.playtime += user_playtime;
            data.user_playtimes.push_back(user_playtime);
        }

        if (!data.playtime) {
            if (running && !*running) {
                return std::nullopt;
            }

            PdmPlayStatistics stats{};
            if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(app_id, true, &stats))) {
                data.playtime = stats.playtime;
                query_succeeded = true;
            }
        }

        if (!query_succeeded) {
            return std::nullopt;
        }
        return data;
    }

    static void WriteCache(const Job& job, const Data& data) {
        char section[33];
        std::snprintf(section, sizeof(section), "%016lX", job.app_id);

        for (size_t i = 0; i < data.user_playtimes.size(); i++) {
            char key[32];
            std::snprintf(key, sizeof(key), "user_%zu_mins", i);
            ini_putl(section, key, data.user_playtimes[i] / 60000000000ULL, App::PLAYLOG_PATH);
        }

        ini_putl(section, "last_played", job.last_played, App::PLAYLOG_PATH);
        ini_putl(section, "playtime_mins", data.playtime / 60000000000ULL, App::PLAYLOG_PATH);
    }

private:
    static void ThreadFunc(void* user) {
        static_cast<PlaytimeWorker*>(user)->Run();
    }

    void Run() {
        const auto waiter = waiterForUEvent(&m_uevent);
        while (m_running) {
            waitSingle(waiter, UINT64_MAX);
            if (!m_running) {
                return;
            }

            std::optional<Job> job;
            {
                SCOPED_MUTEX(&m_job_mutex);
                if (m_pending) {
                    job = std::exchange(m_pending, std::nullopt);
                    m_active = job;
                }
            }
            if (!job) {
                continue;
            }

            auto data = Query(job->app_id, m_accounts, &m_running);
            if (data && m_running) {
                WriteCache(*job, *data);
                {
                    SCOPED_MUTEX(&m_result_mutex);
                    m_results.push_back({*job, std::move(*data)});
                }
            }

            {
                SCOPED_MUTEX(&m_job_mutex);
                if (data) {
                    m_completed = job;
                }
                m_active.reset();
            }
        }
    }

private:
    std::vector<AccountProfileBase> m_accounts{};
    std::vector<ResultData> m_results{};
    std::optional<Job> m_pending{};
    std::optional<Job> m_active{};
    std::optional<Job> m_completed{};
    UEvent m_uevent{};
    Mutex m_job_mutex{};
    Mutex m_result_mutex{};
    Thread m_thread{};
    std::atomic_bool m_running{};
    bool m_started{};
};

Result NspEntry::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    if (off == nsp_size) {
        log_write("[NspEntry::Read] read at eof...\n");
        *bytes_read = 0;
        R_SUCCEED();
    }

    if (off < nsp_data.size()) {
        *bytes_read = size = ClipSize(off, size, nsp_data.size());
        std::memcpy(buf, nsp_data.data() + off, size);
        R_SUCCEED();
    }

    // adjust offset.
    off -= nsp_data.size();

    for (const auto& collection : collections) {
        if (InRange(off, collection.offset, collection.size)) {
            // adjust offset relative to the collection.
            off -= collection.offset;
            *bytes_read = size = ClipSize(off, size, collection.size);

            if (collection.name.ends_with(".nca")) {
                const auto id = ncm::GetContentIdFromStr(collection.name.c_str());
                return ncmContentStorageReadContentIdFile(&cs, buf, size, &id, off);
            } else if (collection.name.ends_with(".tik") || collection.name.ends_with(".cert")) {
                FsRightsId id;
                keys::parse_hex_key(&id, collection.name.c_str());

                const auto it = std::ranges::find_if(tickets, [&id](auto& e){
                    return !std::memcmp(&id, &e.id, sizeof(id));
                });
                R_UNLESS(it != tickets.end(), Result_GameBadReadForDump);

                const auto& data = collection.name.ends_with(".tik") ? it->tik_data : it->cert_data;
                std::memcpy(buf, data.data() + off, size);
                R_SUCCEED();
            }
        }
    }

    log_write("did not find collection...\n");
    return 0x1;
}

void SignalChange() {
    g_change_signalled = true;
}

Menu::Menu(u32 flags) : grid::Menu{"Games"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::L3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            m_entries[m_index].selected ^= 1;

            if (m_entries[m_index].selected) {
                m_selected_count++;
            } else {
                m_selected_count--;
            }
        }}),
        std::make_pair(Button::R3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            if (m_selected_count == m_entries.size()) {
                ClearSelection();
            } else {
                m_selected_count = m_entries.size();
                for (auto& e : m_entries) {
                    e.selected = true;
                }
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::A, Action{"Launch"_i18n, [this](){
            if (m_entries.empty()) {
                return;
            }
            LaunchEntry(m_entries[m_index]);
        }}),
        std::make_pair(Button::Y, Action{"View Content"_i18n, [this](){
            if (m_entries.empty()) {
                return;
            }
            App::Push<meta::Menu>(m_entries[m_index]);
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("Game Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            if (!m_all_entries.empty() || !m_search_query.empty()) {
                options->Add<SidebarEntryCallback>("Search"_i18n, [this](){
                    std::string out;
                    if (R_SUCCEEDED(swkbd::ShowText(out, "Search"_i18n.c_str(), "Enter title name..."_i18n.c_str(), m_search_query.c_str()))) {
                        m_search_query = std::move(out);
                        Filter();
                        SortAndFindLastFile(false);
                        ClearSelection();
                    }
                }, true);
            }

            if (!m_all_entries.empty()) {
                options->Add<SidebarEntryCallback>("Sort By"_i18n, [this](){
                    auto options = std::make_unique<Sidebar>("Sort Options"_i18n, Sidebar::Side::RIGHT);
                    ON_SCOPE_EXIT(App::Push(std::move(options)));

                    // the play statistic sorts are hidden when play statistics
                    // are disabled, as there is no data to sort by.
                    SidebarEntryArray::Items sort_items;
                    std::vector<s64> sort_map;
                    const auto add_sort = [&](const std::string& name, SortType type) {
                        sort_items.push_back(name);
                        sort_map.push_back(type);
                    };

                    add_sort("Updated"_i18n, SortType_Updated);
                    add_sort("Title"_i18n, SortType_Title);
                    add_sort("Title ID"_i18n, SortType_TitleID);
                    if (IsPlayStatsEnabled()) {
                        add_sort("Last played"_i18n, SortType_LastPlayed);
                        add_sort("Total playtime"_i18n, SortType_TotalPlayTime);
                    }
                    add_sort("Publisher"_i18n, SortType_Publisher);

                    auto sort_index = std::distance(sort_map.begin(), std::ranges::find(sort_map, m_sort.Get()));
                    if (sort_index >= (s64)sort_map.size()) {
                        sort_index = 0;
                    }

                    SidebarEntryArray::Items order_items;
                    order_items.push_back("Descending"_i18n);
                    order_items.push_back("Ascending"_i18n);

                    SidebarEntryArray::Items layout_items;
                    layout_items.push_back("List"_i18n);
                    layout_items.push_back("Icon"_i18n);
                    layout_items.push_back("Grid"_i18n);

                    options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this, sort_map](s64& index_out){
                        const auto sort = sort_map[index_out];
                        if (sort == SortType_TotalPlayTime) {
                            LoadPlaytime();
                        } else {
                            m_sort.Set(sort);
                            SortAndFindLastFile(false);
                        }
                    }, sort_index);

                    options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
                        m_order.Set(index_out);
                        SortAndFindLastFile(false);
                    }, m_order.Get());

                    options->Add<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
                        m_layout.Set(index_out);
                        OnLayoutChange();
                    }, m_layout.Get());

                    options->Add<SidebarEntryBool>("Hide forwarders"_i18n, m_hide_forwarders.Get(), [this](bool& v_out){
                        m_hide_forwarders.Set(v_out);
                        m_dirty = true;
                    });

                    options->Add<SidebarEntryBool>("Missing updates or DLC"_i18n, m_missing_content_filter, [this](bool& enable){
                        App::PopToMenu();
                        SetMissingContentFilter(enable);
                    });
                });
            }

            if (m_entries.size()) {
                options->Add<SidebarEntryCallback>("View application content"_i18n, [this](){
                    App::Push<meta::Menu>(m_entries[m_index]);
                });

                options->Add<SidebarEntryCallback>("Launch random game"_i18n, [this](){
                    const auto random_index = randomGet64() % std::size(m_entries);
                    auto& e = m_entries[random_index];
                    LoadControlEntry(e, true);
                    SyncEntryToMaster(e);

                    App::Push<OptionBox>(
                        i18n::Reorder("Launch ", e.GetName()) + '?',
                        "Back"_i18n, "Launch"_i18n, 1, [this, &e](auto op_index){
                            if (op_index && *op_index) {
                                LaunchEntry(e);
                            }
                        }, e.image
                    );
                });

                auto export_nsp = options->Add<SidebarEntryCallback>("Export NSP"_i18n, [this](){
                    ExportOptions(false);
                });
                export_nsp->Depends(App::IsApplication, "Not supported in Applet Mode"_i18n);

                auto export_nsz = options->Add<SidebarEntryCallback>("Export NSZ"_i18n, [this](){
                    ExportOptions(true);
                },  "Exports to NSZ (compressed NSP)"_i18n);
                export_nsz->Depends(App::IsApplication, "Not supported in Applet Mode"_i18n);

                options->Add<SidebarEntryCallback>("Export options"_i18n, [this](){
                    App::DisplayDumpOptions(false);
                });

                // completely deletes the application record and all data.
                options->Add<SidebarEntryCallback>("Delete"_i18n, [this](){
                    const auto buf = i18n::Reorder("Are you sure you want to delete ", m_entries[m_index].GetName()) + "?";
                    App::Push<OptionBox>(
                        buf,
                        "Back"_i18n, "Delete"_i18n, 0, [this](auto op_index){
                            if (op_index && *op_index) {
                                DeleteGames();
                            }
                        }, m_entries[m_index].image
                    );
                }, true);
            }

            options->Add<SidebarEntryCallback>("Advanced options"_i18n, [this](){
                auto options = std::make_unique<Sidebar>("Advanced Options"_i18n, Sidebar::Side::RIGHT);
                ON_SCOPE_EXIT(App::Push(std::move(options)));

                options->Add<SidebarEntryCallback>("Refresh"_i18n, [this](){
                    m_dirty = true;
                    App::PopToMenu();
                });

                options->Add<SidebarEntryBool>("Play statistics"_i18n, m_play_stats.Get(), [this](bool& v_out){
                    SetPlayStatsEnabled(v_out);
                }, "Shows playtime and enables the play statistic sorts. Disable for faster list refreshes and fewer SD card writes on large libraries."_i18n);

                if (!m_entries.empty()) {
                    options->Add<SidebarEntryCallback>("Create contents folder"_i18n, [this](){
                        const auto rc = fs::FsNativeSd().CreateDirectory(title::GetContentsPath(m_entries[m_index].app_id));
                        App::PushErrorBox(rc, "Folder create failed!"_i18n);

                        if (R_SUCCEEDED(rc)) {
                            App::Notify("Folder created!"_i18n);
                        }
                    });

                    options->Add<SidebarEntryCallback>("Create save"_i18n, [this](){
                        ui::PopupList::Items items{};
                        const auto accounts = App::GetAccountList();
                        for (auto& p : accounts) {
                            items.emplace_back(p.nickname);
                        }

                        App::Push<ui::PopupList>(
                            "Select user to create save for"_i18n, items, [this, accounts](auto op_index){
                                if (op_index) {
                                    CreateSaves(accounts[*op_index].uid);
                                }
                            }
                        );
                    });
                }

                options->Add<SidebarEntryCallback>("Delete title cache"_i18n, [this](){
                    App::Push<OptionBox>(
                        "Are you sure you want to delete the title cache?"_i18n,
                        "Back"_i18n, "Delete"_i18n, 0, [this](auto op_index){
                            if (op_index && *op_index) {
                                m_dirty = true;
                                title::Clear();
                                App::PopToMenu();
                            }
                        }
                    );
                });
            });
        }})
    );

    OnLayoutChange();

    ns::Initialize();
    es::Initialize();
    title::Init();
    if (m_play_stats.Get()) {
        m_pdm_initialized = R_SUCCEEDED(pdmqryInitialize());
        if (!m_pdm_initialized) {
            log_write("[PDM] failed to initialize pdm:qry; play statistics will be unavailable\n");
        }
    }

    fsOpenGameCardDetectionEventNotifier(std::addressof(m_gc_event_notifier));
    fsEventNotifierGetEventHandle(std::addressof(m_gc_event_notifier), std::addressof(m_gc_event), true);
}

Menu::~Menu() {
    StopPlaytimeWorker(false);
    title::Exit();

    FreeEntries();
    if (m_pdm_initialized) {
        pdmqryExit();
    }
    ns::Exit();
    es::Exit();

    eventClose(std::addressof(m_gc_event));
    fsEventNotifierClose(std::addressof(m_gc_event_notifier));
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    ApplyPlaytimeResults();

    if (g_change_signalled.exchange(false)) {
        m_dirty = true;
    }

    if (R_SUCCEEDED(eventWait(&m_gc_event, 0))) {
        m_dirty = true;
    }

    if (m_dirty) {
        App::Notify("Updating application record list"_i18n);
        SortAndFindLastFile(true);
    }

    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries.size(), [this](bool touch, auto i) {
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

    if (m_entries.empty()) {
        gfx::drawTextArgs(vg, GetX() + GetW() / 2.f, GetY() + GetH() / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    m_list->Draw(vg, theme, m_entries.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        auto& e = m_entries[pos];

        if (e.status == title::NacpLoadStatus::None) {
            title::PushAsync(e.app_id);
            e.status = title::NacpLoadStatus::Progress;
        } else if (e.status == title::NacpLoadStatus::Progress) {
            LoadResultIntoEntry(e, title::GetAsync(e.app_id));
        }

        // lazy load image
        if (image_load_count < image_load_max) {
            if (LoadControlImage(e, title::GetAsync(e.app_id))) {
                image_load_count++;
            }
        }

        SyncEntryToMaster(e);

        char title_id[33];
        std::snprintf(title_id, sizeof(title_id), "%016lX", e.app_id);

        const auto selected = pos == m_index;
        DrawEntry(vg, theme, m_layout.Get(), v, selected, e.image, e.GetName(), e.GetAuthor(), title_id);

        if (e.selected) {
            gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_FOCUS), 5);
            gfx::drawText(vg, x + w / 2, y + h / 2, 24.f, "\uE14B", nullptr, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
        }
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    if (m_all_entries.empty()) {
        ScanHomebrew();
    } else if (m_missing_content_scanned && m_missing_content_catalog_revision != nx_versions::GetCatalogRevision()) {
        const bool was_filtered = m_missing_content_filter;
        InvalidateMissingContentCache();
        if (was_filtered) {
            Filter();
            SortAndFindLastFile(false);
        }
    }
}

void Menu::SetIndex(s64 index) {
    if (m_entries.empty()) {
        m_index = 0;
        SetTitleSubHeading("");
        this->SetSubHeading("0 / 0");
        return;
    }

    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    auto& entry = m_entries[m_index];
    char title_id[33];
    std::snprintf(title_id, sizeof(title_id), "%016lX", entry.app_id);

    // play statistics are disabled, the entries carry no playtime and no
    // worker is running, so there is nothing to display or request.
    if (!m_play_stats.Get()) {
        SetTitleSubHeading(title_id);
        this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries.size()));
        return;
    }

    std::string title_info = title_id;
    if (!entry.user_playtimes.empty()) {
        bool showed_profile{};
        if (entry.user_playtimes.size() > 1) {
            for (size_t i = 0; i < entry.user_playtimes.size(); i++) {
                if (!entry.user_playtimes[i]) {
                    continue;
                }

                const u64 total_minutes = entry.user_playtimes[i] / 60000000000ULL;
                title_info += " | P" + std::to_string(i + 1) + " " + std::to_string(total_minutes / 60) + "h "_i18n + std::to_string(total_minutes % 60) + "m"_i18n;
                showed_profile = true;
            }
        }

        if (!showed_profile) {
            const u64 total_minutes = entry.playtime / 60000000000ULL;
            title_info += " | " + std::to_string(total_minutes / 60) + "h "_i18n + std::to_string(total_minutes % 60) + "m"_i18n;
        }
    } else if (entry.playtime_cached) {
        const u64 total_minutes = entry.playtime / 60000000000ULL;
        title_info += " | " + std::to_string(total_minutes / 60) + "h "_i18n + std::to_string(total_minutes % 60) + "m"_i18n;
    } else {
        title_info += " | " + "No statistics"_i18n;
    }

    SetTitleSubHeading(title_info);
    this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries.size()));

    const bool profile_cache_missing = !m_accounts.empty() && entry.user_playtimes.size() != m_accounts.size();
    if (m_playtime_worker && (!entry.playtime_cached || entry.last_played != entry.playtime_cached_last_played || profile_cache_missing)) {
        m_playtime_worker->Request({entry.app_id, entry.last_played});
    }
}

void Menu::ScanHomebrew() {
    StopPlaytimeWorker(false);

    constexpr auto ENTRY_CHUNK_COUNT = 1000;
    const auto hide_forwarders = m_hide_forwarders.Get();
    const auto play_stats = m_play_stats.Get();
    TimeStamp ts;

    App::SetBoostMode(true);
    ON_SCOPE_EXIT(App::SetBoostMode(false));

    FreeEntries();
    m_entries.reserve(ENTRY_CHUNK_COUNT);
    g_change_signalled = false;

    if (play_stats && m_accounts.empty()) {
        m_accounts = App::GetAccountList();
    }

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset{};
    while (true) {
        s32 record_count{};
        if (R_FAILED(nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count))) {
            log_write("failed to list application records at offset: %d\n", offset);
        }

        // finished parsing all entries.
        if (!record_count) {
            break;
        }

        std::vector<u64> batch_ids;
        batch_ids.reserve(record_count);
        const auto batch_start = m_entries.size();

        for (s32 i = 0; i < record_count; i++) {
            const auto& e = record_list[i];

            if (hide_forwarders && (e.application_id & 0x0500000000000000) == 0x0500000000000000) {
                continue;
            }

            auto& entry = m_entries.emplace_back(e.application_id, e.last_event);
            batch_ids.push_back(entry.app_id);

            // the cached playtime lookup is a full scan of playlog.ini per
            // entry (and per user), skip it entirely when disabled.
            if (!play_stats) {
                continue;
            }

            char section[33];
            std::snprintf(section, sizeof(section), "%016lX", entry.app_id);
            entry.playtime_cached_last_played = static_cast<u64>(ini_getl(section, "last_played", 0, App::PLAYLOG_PATH));
            const auto cached_minutes = ini_getl(section, "playtime_mins", -1, App::PLAYLOG_PATH);
            if (cached_minutes >= 0) {
                entry.playtime = static_cast<u64>(cached_minutes) * 60000000000ULL;
                entry.playtime_cached = true;

                bool user_cache_complete{true};
                for (size_t user = 0; user < m_accounts.size(); user++) {
                    char key[32];
                    std::snprintf(key, sizeof(key), "user_%zu_mins", user);
                    const auto user_minutes = ini_getl(section, key, -1, App::PLAYLOG_PATH);
                    if (user_minutes < 0) {
                        user_cache_complete = false;
                        break;
                    }
                    entry.user_playtimes.push_back(static_cast<u64>(user_minutes) * 60000000000ULL);
                }
                if (!user_cache_complete) {
                    entry.user_playtimes.clear();
                }
            }
        }

        if (play_stats && m_pdm_initialized && !batch_ids.empty()) {
            std::vector<PdmLastPlayTime> play_times(batch_ids.size());
            s32 play_time_count{};
            if (R_SUCCEEDED(pdmqryQueryLastPlayTime(true, play_times.data(), batch_ids.data(), batch_ids.size(), &play_time_count))) {
                for (s32 i = 0; i < play_time_count; i++) {
                    const auto& play_time = play_times[i];
                    if (!play_time.flag) {
                        continue;
                    }

                    const auto it = std::find_if(m_entries.begin() + batch_start, m_entries.end(), [&play_time](const auto& entry) {
                        return entry.app_id == play_time.application_id;
                    });
                    if (it != m_entries.end()) {
                        it->last_played = pdmPlayTimestampToPosix(play_time.timestamp_user);
                    }
                }
            }
        }

        offset += record_count;
    }

    m_all_entries = m_entries;
    m_is_reversed = false;
    m_dirty = false;
    log_write("games found: %zu time_taken: %.2f seconds %zu ms %zu ns\n", m_all_entries.size(), ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
    this->Filter();
    this->Sort();
    StartPlaytimeWorker();
    SetIndex(0);
    ClearSelection();
}

void Menu::Filter() {
    if (m_search_query.empty() && !m_missing_content_filter) {
        m_entries = m_all_entries;
        return;
    }

    auto query = m_search_query;
    if (!query.empty()) {
        std::ranges::transform(query, query.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    }

    m_entries.clear();
    for (auto& entry : m_all_entries) {
        if (m_missing_content_filter && !m_missing_content_app_ids.contains(entry.app_id)) {
            continue;
        }

        if (query.empty()) {
            m_entries.push_back(entry);
            continue;
        }

        LoadControlEntry(entry);
        auto name = std::string{entry.GetName()};
        std::ranges::transform(name, name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (name.find(query) != std::string::npos) {
            m_entries.push_back(entry);
        }
    }
}

void Menu::Sort() {
    auto sort = m_sort.Get();
    const auto order = m_order.Get();

    // guards against a stale sort being loaded from the config.
    if (!m_play_stats.Get() && (sort == SortType_LastPlayed || sort == SortType_TotalPlayTime)) {
        sort = SortType_Updated;
        m_sort.Set(sort);
    }

    switch (sort) {
        case SortType_Updated:
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                return lhs.last_event > rhs.last_event;
            });
            break;

        case SortType_Title:
            for (auto& entry : m_entries) {
                LoadControlEntry(entry);
                SyncEntryToMaster(entry);
            }
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                return strcasecmp(lhs.GetName(), rhs.GetName()) > 0;
            });
            break;

        case SortType_TitleID:
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                return lhs.app_id > rhs.app_id;
            });
            break;

        case SortType_LastPlayed:
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                return lhs.last_played > rhs.last_played;
            });
            break;

        case SortType_TotalPlayTime:
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                return lhs.playtime > rhs.playtime;
            });
            break;

        case SortType_Publisher:
            for (auto& entry : m_entries) {
                LoadControlEntry(entry);
                SyncEntryToMaster(entry);
            }
            std::ranges::sort(m_entries, [](const auto& lhs, const auto& rhs) {
                const auto publisher_order = strcasecmp(lhs.GetAuthor(), rhs.GetAuthor());
                if (publisher_order) {
                    return publisher_order > 0;
                }

                const auto title_order = strcasecmp(lhs.GetName(), rhs.GetName());
                if (title_order) {
                    return title_order > 0;
                }

                return lhs.app_id > rhs.app_id;
            });
            break;

        default:
            m_sort.Set(SortType_Updated);
            Sort();
            return;
    }

    if (order == OrderType_Ascending) {
        std::ranges::reverse(m_entries);
    }
    m_is_reversed = order == OrderType_Ascending;
}

void Menu::SyncEntryToMaster(const Entry& entry) {
    const auto it = std::ranges::find_if(m_all_entries, [&entry](const auto& candidate) {
        return candidate.app_id == entry.app_id;
    });
    if (it != m_all_entries.end()) {
        const bool selected = it->selected;
        *it = entry;
        it->selected = selected;
    }
}

void Menu::StartPlaytimeWorker() {
    if (!m_play_stats.Get() || !m_pdm_initialized || m_playtime_worker) {
        return;
    }

    m_playtime_worker = std::make_unique<PlaytimeWorker>(m_accounts);
    if (!m_playtime_worker->IsRunning()) {
        log_write("[PDM] failed to start playtime worker\n");
        m_playtime_worker.reset();
    }
}

void Menu::StopPlaytimeWorker(bool apply_results) {
    if (!m_playtime_worker) {
        return;
    }

    m_playtime_worker->Stop();
    if (apply_results) {
        ApplyPlaytimeResults();
    }
    m_playtime_worker.reset();
}

void Menu::ApplyPlaytimeResults() {
    if (!m_playtime_worker) {
        return;
    }

    const auto selected_app_id = m_entries.empty() ? 0 : m_entries[m_index].app_id;
    bool selected_updated{};

    for (auto& result : m_playtime_worker->TakeResults()) {
        const auto apply = [&result](Entry& entry) {
            entry.playtime = result.data.playtime;
            entry.user_playtimes = result.data.user_playtimes;
            entry.playtime_cached_last_played = result.job.last_played;
            entry.playtime_cached = true;
        };

        const auto master = std::ranges::find_if(m_all_entries, [&result](const auto& entry) {
            return entry.app_id == result.job.app_id;
        });
        if (master != m_all_entries.end()) {
            apply(*master);
        }

        const auto visible = std::ranges::find_if(m_entries, [&result](const auto& entry) {
            return entry.app_id == result.job.app_id;
        });
        if (visible != m_entries.end()) {
            apply(*visible);
            selected_updated |= visible->app_id == selected_app_id;
        }
    }

    if (selected_updated && !m_entries.empty()) {
        SetIndex(m_index);
    }
}

void Menu::SetPlayStatsEnabled(bool enable) {
    if (enable == m_play_stats.Get() && enable == m_pdm_initialized) {
        return;
    }

    m_play_stats.Set(enable);

    if (enable) {
        if (!m_pdm_initialized) {
            m_pdm_initialized = R_SUCCEEDED(pdmqryInitialize());
            if (!m_pdm_initialized) {
                log_write("[PDM] failed to initialize pdm:qry; play statistics will be unavailable\n");
                App::Notify("Play statistics unavailable"_i18n);
            }
        }
    } else {
        // the worker must go before pdm:qry is closed, as it queries from
        // its own thread.
        StopPlaytimeWorker(false);

        if (m_pdm_initialized) {
            pdmqryExit();
            m_pdm_initialized = false;
        }

        // fall back to a sort that does not need play statistics.
        const auto sort = m_sort.Get();
        if (sort == SortType_LastPlayed || sort == SortType_TotalPlayTime) {
            m_sort.Set(SortType_Updated);
        }
    }

    // rebuild the list so the stale playtime data is dropped / repopulated.
    m_dirty = true;
}

void Menu::SetMissingContentFilter(bool enable) {
    if (!enable) {
        m_missing_content_filter = false;
        Filter();
        SortAndFindLastFile(false);
        ClearSelection();
        return;
    }

    const auto state = nx_versions::GetCatalogState();
    if (state == nx_versions::CatalogState::Current) {
        StartMissingContentScan();
        return;
    }

    App::Push<OptionBox>(
        "Version list is missing or outdated. Download it now?"_i18n,
        "No"_i18n, "Download"_i18n, 0, [this, state](auto op_index){
            if (!op_index) {
                return;
            }

            if (*op_index) {
                DownloadAndScanMissingContent();
            } else if (state == nx_versions::CatalogState::Stale) {
                StartMissingContentScan();
            }
        }
    );
}

void Menu::StartMissingContentScan() {
    const auto catalog_revision = nx_versions::GetCatalogRevision();
    if (m_missing_content_scanned && m_missing_content_catalog_revision == catalog_revision) {
        m_missing_content_filter = true;
        Filter();
        SortAndFindLastFile(false);
        ClearSelection();
        return;
    }

    auto app_ids = std::make_shared<std::vector<u64>>();
    app_ids->reserve(m_all_entries.size());
    for (const auto& entry : m_all_entries) {
        app_ids->push_back(entry.app_id);
    }

    auto missing_content = std::make_shared<std::unordered_set<u64>>();
    App::Push<ProgressBox>(0, "Scanning "_i18n, "Missing updates or DLC"_i18n,
        [app_ids, missing_content](auto pbox) -> Result {
            missing_content->reserve(app_ids->size());

            nx_versions::InstalledVersions installed;
            installed.reserve(app_ids->size() * 4);
            title::MetaEntries meta_entries;
            size_t attempted_queries{};
            size_t successful_queries{};
            Result last_error{};

            pbox->UpdateTransfer(0, app_ids->size());
            for (size_t i = 0; i < app_ids->size(); i++) {
                R_TRY(pbox->ShouldExitResult());

                const auto app_id = (*app_ids)[i];
                if (nx_versions::HasEntries(app_id)) {
                    attempted_queries++;
                    meta_entries.clear();
                    const auto rc = title::GetMetaEntries(app_id, meta_entries);
                    if (R_SUCCEEDED(rc)) {
                        successful_queries++;
                        for (const auto& meta : meta_entries) {
                            const auto [it, inserted] = installed.try_emplace(meta.application_id, meta.version);
                            if (!inserted) {
                                it->second = std::max(it->second, meta.version);
                            }
                        }

                        if (nx_versions::HasAvailable(app_id, installed)) {
                            missing_content->insert(app_id);
                        }
                    } else {
                        last_error = rc;
                        log_write("[nx_versions] failed to query metadata for %016lX: 0x%X\n", app_id, rc);
                    }
                }

                pbox->SetTitle(std::to_string(i + 1) + " / " + std::to_string(app_ids->size()));
                pbox->UpdateTransfer(i + 1, app_ids->size());
            }

            if (attempted_queries && !successful_queries) {
                return last_error;
            }

            R_SUCCEED();
        },
        [this, missing_content, catalog_revision](Result rc) {
            if (R_SUCCEEDED(rc)) {
                m_missing_content_app_ids = std::move(*missing_content);
                m_missing_content_catalog_revision = catalog_revision;
                m_missing_content_scanned = true;
                m_missing_content_filter = true;
                Filter();
                SortAndFindLastFile(false);
                ClearSelection();
            } else if (rc != Result_TransferCancelled) {
                App::PushErrorBox(rc, "An error occurred"_i18n);
            }
        }
    );
}

void Menu::DownloadAndScanMissingContent() {
    App::Push<ProgressBox>(0, "Downloading "_i18n, "Version List"_i18n, [](auto pbox) -> Result {
        return nx_versions::Download(pbox);
    }, [this](Result rc) {
        if (R_SUCCEEDED(rc)) {
            StartMissingContentScan();
        } else if (rc != Result_TransferCancelled) {
            App::PushErrorBox(rc, "Failed to update version list"_i18n);
        }
    });
}

void Menu::InvalidateMissingContentCache() {
    m_missing_content_filter = false;
    m_missing_content_scanned = false;
    m_missing_content_catalog_revision = 0;
    m_missing_content_app_ids.clear();
}

void Menu::LoadPlaytime() {
    if (!m_play_stats.Get() || !m_pdm_initialized) {
        App::Notify("Play statistics unavailable"_i18n);
        return;
    }

    StopPlaytimeWorker(true);

    if (m_accounts.empty()) {
        m_accounts = App::GetAccountList();
    }

    std::vector<size_t> update_indices;
    for (size_t i = 0; i < m_all_entries.size(); i++) {
        const auto& entry = m_all_entries[i];
        const bool profile_cache_missing = !m_accounts.empty() && entry.user_playtimes.size() != m_accounts.size();
        if (!entry.playtime_cached || entry.last_played != entry.playtime_cached_last_played || profile_cache_missing) {
            update_indices.push_back(i);
        }
    }

    if (update_indices.empty()) {
        StartPlaytimeWorker();
        m_sort.Set(SortType_TotalPlayTime);
        Filter();
        SortAndFindLastFile(false);
        return;
    }

    App::Push<ProgressBox>(0, "Updating play statistics"_i18n, "", [this, update_indices](auto pbox) -> Result {
        pbox->UpdateTransfer(0, update_indices.size());

        for (size_t i = 0; i < update_indices.size(); i++) {
            R_TRY(pbox->ShouldExitResult());

            auto& entry = m_all_entries[update_indices[i]];
            const PlaytimeWorker::Job job{entry.app_id, entry.last_played};
            if (auto data = PlaytimeWorker::Query(entry.app_id, m_accounts)) {
                entry.playtime = data->playtime;
                entry.user_playtimes = data->user_playtimes;
                entry.playtime_cached_last_played = entry.last_played;
                entry.playtime_cached = true;
                PlaytimeWorker::WriteCache(job, *data);
            }

            pbox->SetTitle(std::to_string(i + 1) + " / " + std::to_string(update_indices.size()));
            pbox->UpdateTransfer(i + 1, update_indices.size());
        }

        R_SUCCEED();
    }, [this](Result rc) {
        StartPlaytimeWorker();
        if (R_SUCCEEDED(rc)) {
            m_sort.Set(SortType_TotalPlayTime);
            Filter();
            SortAndFindLastFile(false);
        } else {
            App::PushErrorBox(rc, "Failed to update play statistics!"_i18n);
        }
    });
}

void Menu::SortAndFindLastFile(bool scan) {
    const bool had_entry = !m_entries.empty();
    const auto app_id = had_entry ? m_entries[m_index].app_id : 0;
    if (scan) {
        ScanHomebrew();
    } else {
        Sort();
    }

    if (m_entries.empty()) {
        SetIndex(0);
        return;
    }

    SetIndex(0);

    s64 index = -1;
    for (u64 i = 0; i < m_entries.size(); i++) {
        if (had_entry && app_id == m_entries[i].app_id) {
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

    for (auto&p : m_all_entries) {
        FreeEntry(vg, p);
    }

    m_entries.clear();
    m_all_entries.clear();
    InvalidateMissingContentCache();
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

void Menu::DeleteGames() {
    App::Push<ProgressBox>(0, "Deleting"_i18n, "", [this](auto pbox) -> Result {
        auto targets = GetSelectedEntries();

        for (s64 i = 0; i < std::size(targets); i++) {
            auto& e = targets[i];

            LoadControlEntry(e);
            pbox->SetTitle(e.GetName());
            pbox->UpdateTransfer(i + 1, std::size(targets));
            R_TRY(nsDeleteApplicationCompletely(e.app_id));
        }

        R_SUCCEED();
    }, [this](Result rc){
        App::PushErrorBox(rc, "Delete failed!"_i18n);

        ClearSelection();
        m_dirty = true;

        if (R_SUCCEEDED(rc)) {
            App::Notify("Delete successfull!"_i18n);
        }
    });
}

void Menu::ExportOptions(bool to_nsz) {
    auto options = std::make_unique<Sidebar>("Select content to export"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<SidebarEntryCallback>("Export All"_i18n, [this, to_nsz](){
        DumpGames(title::ContentFlag_All, to_nsz);
    }, true);
    options->Add<SidebarEntryCallback>("Export Application"_i18n, [this, to_nsz](){
        DumpGames(title::ContentFlag_Application, to_nsz);
    }, true);
    options->Add<SidebarEntryCallback>("Export Patch"_i18n, [this, to_nsz](){
        DumpGames(title::ContentFlag_Patch, to_nsz);
    }, true);
    options->Add<SidebarEntryCallback>("Export AddOnContent"_i18n, [this, to_nsz](){
        DumpGames(title::ContentFlag_AddOnContent, to_nsz);
    }, true);
    options->Add<SidebarEntryCallback>("Export DataPatch"_i18n, [this, to_nsz](){
        DumpGames(title::ContentFlag_DataPatch, to_nsz);
    }, true);
}

void Menu::DumpGames(u32 flags, bool to_nsz) {
    auto targets = GetSelectedEntries();

    std::vector<NspEntry> nsp_entries;
    for (auto& e : targets) {
        BuildNspEntries(e, flags, nsp_entries, to_nsz);
    }

    DumpNsp(nsp_entries, to_nsz);
}

void Menu::CreateSaves(AccountUid uid) {
    App::Push<ProgressBox>(0, "Creating"_i18n, "", [this, uid](auto pbox) -> Result {
        auto targets = GetSelectedEntries();

        for (s64 i = 0; i < std::size(targets); i++) {
            auto& e = targets[i];

            LoadControlEntry(e);
            pbox->SetTitle(e.GetName());
            pbox->UpdateTransfer(i + 1, std::size(targets));
            const auto rc = CreateSave(e.app_id, uid);

            // don't error if the save already exists.
            if (R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
                R_THROW(rc);
            }
        }

        R_SUCCEED();
    }, [this](Result rc){
        App::PushErrorBox(rc, "Save create failed!"_i18n);

        ClearSelection();
        save::SignalChange();

        if (R_SUCCEEDED(rc)) {
            App::Notify("Save create successfull!"_i18n);
        }
    });
}

Result GetMetaEntries(const Entry& e, title::MetaEntries& out, u32 flags) {
    return title::GetMetaEntries(e.app_id, out, flags);
}

Result GetNcmMetaFromMetaStatus(const NsApplicationContentMetaStatus& status, NcmMetaData& out) {
    out.cs = &title::GetNcmCs(status.storageID);
    out.db = &title::GetNcmDb(status.storageID);
    out.app_id = ncm::GetAppId(status.meta_type, status.application_id);

    auto id_min = status.application_id;
    auto id_max = status.application_id;
    // workaround N bug where they don't check the full range in the ID filter.
    // https://github.com/Atmosphere-NX/Atmosphere/blob/1d3f3c6e56b994b544fc8cd330c400205d166159/libraries/libstratosphere/source/ncm/ncm_on_memory_content_meta_database_impl.cpp#L22
    if (status.storageID == NcmStorageId_None || status.storageID == NcmStorageId_GameCard) {
        id_min -= 1;
        id_max += 1;
    }

    s32 meta_total;
    s32 meta_entries_written;
    R_TRY(ncmContentMetaDatabaseList(out.db, &meta_total, &meta_entries_written, &out.key, 1, (NcmContentMetaType)status.meta_type, out.app_id, id_min, id_max, NcmContentInstallType_Full));
    // log_write("ncmContentMetaDatabaseList(): AppId: %016lX Id: %016lX total: %d written: %d storageID: %u key.id %016lX\n", out.app_id, status.application_id, meta_total, meta_entries_written, status.storageID, out.key.id);
    R_UNLESS(meta_total == 1, Result_GameMultipleKeysFound);
    R_UNLESS(meta_entries_written == 1, Result_GameMultipleKeysFound);

    R_SUCCEED();
}

// deletes the array of entries (remove nca, remove ncm db, remove ns app records).
void DeleteMetaEntries(u64 app_id, int image, const std::string& name, const title::MetaEntries& entries) {
    App::Push<ProgressBox>(image, "Delete"_i18n, name, [app_id, entries](ProgressBox* pbox) -> Result {
        R_TRY(ns::Initialize());
        ON_SCOPE_EXIT(ns::Exit());

        // fetch current app records.
        std::vector<ncm::ContentStorageRecord> records;
        R_TRY(ns::GetApplicationRecords(app_id, records));

        // on exit, delete old record list and push the new one.
        ON_SCOPE_EXIT(
            R_TRY(ns::DeleteApplicationRecord(app_id));
            return ns::PushApplicationRecord(app_id, records.data(), records.size());
        )

        // on exit, set the new lowest version.
        ON_SCOPE_EXIT(
            ns::SetLowestLaunchVersion(app_id, records);
        )

        for (u32 i = 0; i < std::size(entries); i++) {
            const auto& status = entries[i];

            // check if the user wants to exit, only in-between each successful delete.
            R_TRY(pbox->ShouldExitResult());

            char transfer_str[33];
            std::snprintf(transfer_str, sizeof(transfer_str), "%016lX", status.application_id);
            pbox->NewTransfer(transfer_str).UpdateTransfer(i, std::size(entries));

            NcmMetaData meta;
            R_TRY(GetNcmMetaFromMetaStatus(status, meta));

            // only delete form non read-only storage.
            if (status.storageID == NcmStorageId_BuiltInUser || status.storageID == NcmStorageId_SdCard) {
                R_TRY(ncm::DeleteKey(meta.cs, meta.db, &meta.key));
            }

            // find and remove record.
            std::erase_if(records, [&meta](auto& e){
                return meta.key.id == e.key.id;
            });
        }

        R_SUCCEED();
    }, [](Result rc){
        App::PushErrorBox(rc, "Failed to delete meta entry"_i18n);
    });
}

auto BuildNspPath(const Entry& e, std::string_view export_name, const NsApplicationContentMetaStatus& status, bool to_nsz) -> fs::FsPath {
    char version[sizeof(NacpStruct::display_version) + 1]{};
    if (status.meta_type == NcmContentMetaType_Patch) {
        u64 program_id;
        fs::FsPath path;
        if (R_SUCCEEDED(title::GetControlPathFromStatus(status, &program_id, &path))) {
            char display_version[0x10];
            if (R_SUCCEEDED(nca::ParseControl(path, program_id, display_version, sizeof(display_version), nullptr, offsetof(NacpStruct, display_version)))) {
                std::snprintf(version, sizeof(version), "%s ", display_version);
            }
        }
    }

    const auto ext = to_nsz ? "nsz" : "nsp";

    fs::FsPath file_name;
    if (export_name.empty()) {
        std::snprintf(file_name, sizeof(file_name), "%s[%016lX][v%u][%s].%s", version, status.application_id, status.version, ncm::GetMetaTypeShortStr(status.meta_type), ext);
    } else {
        std::snprintf(file_name, sizeof(file_name), "%.*s %s[%016lX][v%u][%s].%s", static_cast<int>(export_name.size()), export_name.data(), version, status.application_id, status.version, ncm::GetMetaTypeShortStr(status.meta_type), ext);
    }

    if (App::GetApp()->m_dump_app_folder.Get()) {
        fs::FsPath folder;
        if (export_name.empty()) {
            std::snprintf(folder, sizeof(folder), "%016lX", e.app_id);
        } else {
            std::snprintf(folder, sizeof(folder), "%.*s", static_cast<int>(export_name.size()), export_name.data());
        }
        return fs::AppendPath(folder, file_name);
    }

    return file_name;
}

Result BuildContentEntry(const NsApplicationContentMetaStatus& status, ContentInfoEntry& out, bool to_nsz) {
    NcmMetaData meta;
    R_TRY(GetNcmMetaFromMetaStatus(status, meta));

    std::vector<NcmContentInfo> infos;
    R_TRY(ncm::GetContentInfos(meta.db, &meta.key, infos));

    std::vector<NcmContentInfo> cnmt_infos;
    for (const auto& info : infos) {
        // check if we need to fetch tickets.
        NcmRightsId ncm_rights_id;
        R_TRY(ncmContentStorageGetRightsIdFromContentId(meta.cs, std::addressof(ncm_rights_id), std::addressof(info.content_id), FsContentAttributes_All));

        if (es::IsRightsIdValid(ncm_rights_id.rights_id)) {
            const auto it = std::ranges::find_if(out.ncm_rights_id, [&ncm_rights_id](auto& e){
                return !std::memcmp(&e, &ncm_rights_id, sizeof(ncm_rights_id));
            });

            if (it == out.ncm_rights_id.end()) {
                out.ncm_rights_id.emplace_back(ncm_rights_id);
            }
        }

        if (info.content_type == NcmContentType_Meta) {
            cnmt_infos.emplace_back(info);
        } else {
            out.content_infos.emplace_back(info);
        }
    }

    // append cnmt at the end of the list, following StandardNSP spec.
    out.content_infos.insert_range(out.content_infos.end(), cnmt_infos);
    out.status = status;
    R_SUCCEED();
}

Result BuildNspEntry(const Entry& e, std::string_view export_name, const ContentInfoEntry& info, const keys::Keys& keys, NspEntry& out, bool to_nsz) {
    out.application_name = e.GetName();
    out.path = BuildNspPath(e, export_name, info.status, to_nsz);
    s64 offset{};

    for (auto& e : info.content_infos) {
        char nca_name[64];
        std::snprintf(nca_name, sizeof(nca_name), "%s%s", utils::hexIdToStr(e.content_id).str, e.content_type == NcmContentType_Meta ? ".cnmt.nca" : ".nca");

        u64 size;
        ncmContentInfoSizeToU64(std::addressof(e), std::addressof(size));

        out.collections.emplace_back(nca_name, offset, size);
        offset += size;
    }

    for (auto& ncm_rights_id : info.ncm_rights_id) {
        const auto rights_id = ncm_rights_id.rights_id;
        const auto key_gen = ncm_rights_id.key_generation;

        TikEntry entry{rights_id, key_gen};
        log_write("rights id is valid, fetching common ticket and cert\n");

        // todo: fetch array of tickets to know where the ticket is stored.
        if (R_FAILED(es::GetCommonTicketAndCertificate(rights_id, entry.tik_data, entry.cert_data))) {
            R_TRY(es::GetPersonalisedTicketAndCertificate(rights_id, entry.tik_data, entry.cert_data));
        }

        // patch fake ticket / convert personalised to common if needed.
        R_TRY(es::PatchTicket(entry.tik_data, entry.cert_data, key_gen, keys, App::GetApp()->m_dump_convert_to_common_ticket.Get()));

        char tik_name[64];
        std::snprintf(tik_name, sizeof(tik_name), "%s%s", utils::hexIdToStr(rights_id).str, ".tik");

        char cert_name[64];
        std::snprintf(cert_name, sizeof(cert_name), "%s%s", utils::hexIdToStr(rights_id).str, ".cert");

        out.collections.emplace_back(tik_name, offset, entry.tik_data.size());
        offset += entry.tik_data.size();

        out.collections.emplace_back(cert_name, offset, entry.cert_data.size());
        offset += entry.cert_data.size();

        out.tickets.emplace_back(entry);
    }

    out.nsp_data = yati::container::Nsp::Build(out.collections, out.nsp_size);
    out.cs = title::GetNcmCs(info.status.storageID);

    R_SUCCEED();
}

Result BuildNspEntries(Entry& e, const title::MetaEntries& meta_entries, std::vector<NspEntry>& out, bool to_nsz) {
    LoadControlEntry(e);

    const auto fix_filenames = App::GetApp()->m_dump_fix_filenames.Get();
    const auto english_name = fix_filenames ? title::GetEnglishTitleName(e.app_id) : std::string{};
    const auto export_name = title::MakeExportTitleName(e.GetName(), english_name, fix_filenames);

    keys::Keys keys;
    R_TRY(keys::parse_keys(keys, true));

    for (const auto& status : meta_entries) {
        ContentInfoEntry info;
        R_TRY(BuildContentEntry(status, info));

        NspEntry nsp;
        R_TRY(BuildNspEntry(e, export_name, info, keys, nsp, to_nsz));
        out.emplace_back(nsp).icon = e.image;
    }

    R_UNLESS(!out.empty(), Result_GameNoNspEntriesBuilt);
    R_SUCCEED();
}

Result BuildNspEntries(Entry& e, u32 flags, std::vector<NspEntry>& out, bool to_nsz) {
    title::MetaEntries meta_entries;
    R_TRY(GetMetaEntries(e, meta_entries, flags));

    return BuildNspEntries(e, meta_entries, out, to_nsz);
}

void DumpNsp(const std::vector<NspEntry>& entries, bool to_nsz) {
    std::vector<fs::FsPath> paths;
    for (auto& e : entries) {
        if (to_nsz) {
            paths.emplace_back(fs::AppendPath("/dumps/NSZ", e.path));
        } else {
            paths.emplace_back(fs::AppendPath("/dumps/NSP", e.path));
        }
    }

    auto source = std::make_shared<NspSource>(entries);

    if (to_nsz) {
#ifdef ENABLE_NSZ
        // todo: log keys error.
        keys::Keys keys;
        keys::parse_keys(keys, true);

        dump::Dump(source, paths, [keys](ProgressBox* pbox, dump::BaseSource* source, dump::WriteSource* writer, const fs::FsPath& path) {
            return NszExport(pbox, keys, source, writer, path);
        });
#endif // ENABLE_NSZ
    } else {
        dump::Dump(source, paths);
    }
}

} // namespace sphaira::ui::menu::game

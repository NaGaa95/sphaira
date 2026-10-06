#include "title_info.hpp"
#include "defines.hpp"
#include "ui/types.hpp"
#include "log.hpp"
#include "app.hpp"
#include "nacp_compat.hpp"

#include "yati/nx/ns.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"

#include "utils/thread.hpp"
#include "i18n.hpp"

#include <cstring>
#include <atomic>
#include <ranges>
#include <algorithm>

#include <nxtc.h>
#include <minIni.h>
#include <zlib.h>

namespace sphaira::title {
namespace {

struct ThreadData {
    ThreadData(bool title_cache);

    void Run();
    void Close();
    void Clear();

    void PushAsync(u64 id);
    void PushAsync(const std::span<const NsApplicationRecord> app_ids);
    auto GetAsync(u64 app_id) -> ThreadResultData*;
    auto Get(u64 app_id, bool* cached = nullptr) -> ThreadResultData*;

    auto IsRunning() const -> bool {
        return m_running;
    }

    auto IsTitleCacheEnabled() const {
        return m_title_cache;
    }

private:
    fs::FsNativeSd m_fs{};
    UEvent m_uevent{};
    Mutex m_mutex_id{};
    Mutex m_mutex_result{};
    bool m_title_cache{};

    // app_ids pushed to the queue, signal uevent when pushed.
    std::vector<u64> m_ids{};
    // control data pushed to the queue.
    std::vector<std::unique_ptr<ThreadResultData>> m_result{};

    std::atomic_bool m_running{};
};

Mutex g_mutex{};
Thread g_thread{};
u32 g_ref_count{};
std::unique_ptr<ThreadData> g_thread_data{};

void NormalizeNacpLangData(NacpStruct& nacp);

struct NcmEntry {
    const NcmStorageId storage_id;
    NcmContentStorage cs{};
    NcmContentMetaDatabase db{};

    void Open() {
        if (R_FAILED(ncmOpenContentMetaDatabase(std::addressof(db), storage_id))) {
            log_write("\tncmOpenContentMetaDatabase() failed. storage_id: %u\n", storage_id);
        } else {
            log_write("\tncmOpenContentMetaDatabase() success. storage_id: %u\n", storage_id);
        }

        if (R_FAILED(ncmOpenContentStorage(std::addressof(cs), storage_id))) {
            log_write("\tncmOpenContentStorage() failed. storage_id: %u\n", storage_id);
        } else {
            log_write("\tncmOpenContentStorage() success. storage_id: %u\n", storage_id);
        }
    }

    void Close() {
        ncmContentMetaDatabaseClose(std::addressof(db));
        ncmContentStorageClose(std::addressof(cs));

        db = {};
        cs = {};
    }
};

constinit NcmEntry ncm_entries[] = {
    // on memory, will become invalid on the gamecard being inserted / removed.
    { NcmStorageId_GameCard },
    // normal (save), will remain valid.
    { NcmStorageId_BuiltInUser },
    { NcmStorageId_SdCard },
};

auto& GetNcmEntry(u8 storage_id) {
    auto it = std::ranges::find_if(ncm_entries, [storage_id](auto& e){
        return storage_id == e.storage_id;
    });

    if (it == std::end(ncm_entries)) {
        log_write("unable to find valid ncm entry: %u\n", storage_id);
        return ncm_entries[0];
    }

    return *it;
}

// also sets the status to error.
void FakeNacpEntry(ThreadResultData* e) {
    e->status = NacpLoadStatus::Error;
    // fake the nacp entry
    std::strcpy(e->lang.name, "Corrupted"_i18n.c_str());
    std::strcpy(e->lang.author, "Corrupted"_i18n.c_str());
}

Result LoadControlNacp(u64 id, NacpStruct& nacp, std::vector<u8>* icon) {
    MetaEntries entries;
    R_TRY(GetMetaEntries(id, entries, ContentFlag_Nacp));
    R_UNLESS(!entries.empty(), Result_GameEmptyMetaEntries);

    u64 program_id;
    fs::FsPath path;
    R_TRY(GetControlPathFromStatus(entries.back(), &program_id, &path));
    return nca::ParseControl(path, program_id, &nacp, sizeof(nacp), icon);
}

Result LoadControlManual(u64 id, NacpStruct& nacp, ThreadResultData* data) {
    TimeStamp ts;

    R_TRY(LoadControlNacp(id, nacp, &data->icon));

    log_write("\t\t[manual control] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
    R_SUCCEED();
}

// HOS 21.0.0 introduced a compressed title-language table while keeping the
// NACP binary size and offsets stable. This project currently builds against a
// libnx NacpStruct which predates the named fields, so access the two new fields
// by their documented on-disk offsets. This also remains compatible with newer
// libnx headers.
void NormalizeNacpLangData(NacpStruct& nacp) {
    static constexpr size_t LanguageDataSize = 0x3000;
    static constexpr size_t TitlesDataFormatOffset = 0x3215;
    static constexpr size_t CompressedBufferOffset = sizeof(u16);
    static constexpr size_t DecompressedLanguageCount = 32;
    static constexpr size_t LegacyLanguageCount = 16;

    static_assert(sizeof(NacpStruct) > TitlesDataFormatOffset);
    static_assert(sizeof(NacpLanguageEntry) * LegacyLanguageCount == LanguageDataSize);

    auto* raw = reinterpret_cast<u8*>(&nacp);
    if (raw[TitlesDataFormatOffset] != 1) {
        return;
    }

    u16 compressed_size{};
    std::memcpy(&compressed_size, raw, sizeof(compressed_size));
    if (!compressed_size || compressed_size > LanguageDataSize - CompressedBufferOffset) {
        log_write("[NACP] invalid compressed title data size: %u\n", static_cast<unsigned>(compressed_size));
        return;
    }

    auto decompressed = std::make_unique<NacpLanguageEntry[]>(DecompressedLanguageCount);
    std::memset(decompressed.get(), 0, sizeof(NacpLanguageEntry) * DecompressedLanguageCount);

    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) {
        log_write("[NACP] failed to initialize title data decompression\n");
        return;
    }

    stream.next_in = raw + CompressedBufferOffset;
    stream.avail_in = compressed_size;
    stream.next_out = reinterpret_cast<Bytef*>(decompressed.get());
    stream.avail_out = sizeof(NacpLanguageEntry) * DecompressedLanguageCount;
    const int inflate_result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);

    if (inflate_result != Z_STREAM_END) {
        log_write("[NACP] failed to decompress title data: %d\n", inflate_result);
        return;
    }

    bool has_legacy_language{};
    for (size_t i = 0; i < LegacyLanguageCount; i++) {
        if (decompressed[i].name[0]) {
            has_legacy_language = true;
            break;
        }
    }

    std::memset(raw, 0, LanguageDataSize);
    if (has_legacy_language) {
        // Preserve all legacy language slots so nsGetApplicationDesiredLanguage
        // can still select the user's system language.
        std::memcpy(raw, decompressed.get(), LanguageDataSize);
    } else {
        // Future formats may only populate one of the extra slots. Keep a
        // readable fallback in slot zero for older libnx versions.
        for (size_t i = LegacyLanguageCount; i < DecompressedLanguageCount; i++) {
            if (decompressed[i].name[0]) {
                std::memcpy(raw, &decompressed[i], sizeof(NacpLanguageEntry));
                break;
            }
        }
    }

    raw[TitlesDataFormatOffset] = 0;
}

ThreadData::ThreadData(bool title_cache) : m_title_cache{title_cache} {
    ueventCreate(&m_uevent, true);
    mutexInit(&m_mutex_id);
    mutexInit(&m_mutex_result);
    m_running = true;
}

void ThreadData::Run() {
    TimeStamp ts{};
    bool cached{true};
    const auto waiter = waiterForUEvent(&m_uevent);

    while (IsRunning()) {
        const auto rc = waitSingle(waiter, 3e+9);

        // if we timed out, flush the cache and poll again.
        if (R_FAILED(rc)) {
            nxtcFlushCacheFile();
            continue;
        }

        if (!IsRunning()) {
            return;
        }

        std::vector<u64> ids;
        {
            SCOPED_MUTEX(&m_mutex_id);
            std::swap(ids, m_ids);
        }

        for (u64 i = 0; i < std::size(ids); i++) {
            if (!IsRunning()) {
                return;
            }

            // sleep after every other entry loaded.
            const auto elapsed = (s64)2e+6 - (s64)ts.GetNs();
            if (!cached && elapsed > 0) {
                svcSleepThread(elapsed);
            }

            // loads new entry into cache.
            std::ignore = Get(ids[i], &cached);
            ts.Update();
        }
    }
}

void ThreadData::Close() {
    m_running = false;
    ueventSignal(&m_uevent);
}

void ThreadData::Clear() {
    SCOPED_MUTEX(&m_mutex_id);
    SCOPED_MUTEX(&m_mutex_result);
    m_result.clear();
    nxtcWipeCache();
}

void ThreadData::PushAsync(u64 id) {
    SCOPED_MUTEX(&m_mutex_id);
    SCOPED_MUTEX(&m_mutex_result);

    const auto it_id = std::ranges::find(m_ids, id);
    const auto it_result = std::ranges::find_if(m_result, [id](auto& e){
        return id == e->id;
    });

    if (it_id == m_ids.end() && it_result == m_result.end()) {
        m_ids.emplace_back(id);
        ueventSignal(&m_uevent);
    }
}

void ThreadData::PushAsync(const std::span<const NsApplicationRecord> app_ids) {
    SCOPED_MUTEX(&m_mutex_id);
    SCOPED_MUTEX(&m_mutex_result);
    bool added_at_least_one = false;

    for (auto& record : app_ids) {
        const auto id = record.application_id;

        const auto it_id = std::ranges::find(m_ids, id);
        const auto it_result = std::ranges::find_if(m_result, [id](auto& e){
            return id == e->id;
        });

        if (it_id == m_ids.end() && it_result == m_result.end()) {
            m_ids.emplace_back(id);
            added_at_least_one = true;
        }
    }

    if (added_at_least_one) {
        ueventSignal(&m_uevent);
    }
}

auto ThreadData::GetAsync(u64 app_id) -> ThreadResultData* {
    SCOPED_MUTEX(&m_mutex_result);

    for (s64 i = 0; i < std::size(m_result); i++) {
        if (app_id == m_result[i]->id) {
            return m_result[i].get();
        }
    }

    return {};
}

auto ThreadData::Get(u64 app_id, bool* cached) -> ThreadResultData* {
    // try and fetch from results first, before manually loading.
    if (auto data = GetAsync(app_id)) {
        if (cached) {
            *cached = true;
        }
        return data;
    }

    TimeStamp ts;
    auto result = std::make_unique<ThreadResultData>(app_id);
    result->status = NacpLoadStatus::Error;

    if (auto data = nxtcGetApplicationMetadataEntryById(app_id)) {
        log_write("[NXTC] loaded from cache time taken: %.2fs %zums %zuns\n", ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
        ON_SCOPE_EXIT(nxtcFreeApplicationMetadata(&data));

        if (cached) {
            *cached = true;
        }

        result->status = NacpLoadStatus::Loaded;
        std::strcpy(result->lang.name, data->name);
        std::strcpy(result->lang.author, data->publisher);
        result->icon.resize(data->icon_size);
        std::memcpy(result->icon.data(), data->icon_data, result->icon.size());
    } else {
        if (cached) {
            *cached = false;
        }

        bool has_nacp = false;
        bool manual_load = false;
        u64 actual_size{};
        auto control = std::make_unique<NsApplicationControlData>();

        if (!ns::IsNsControlFetchSlow()) {
            TimeStamp ts;
            if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, app_id, control.get(), sizeof(NsApplicationControlData), &actual_size))) {
                has_nacp = true;
                log_write("\t\t[ns control cache] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
            }
        }

        if (!has_nacp) {
            has_nacp = manual_load = R_SUCCEEDED(LoadControlManual(app_id, control->nacp, result.get()));
        }

        if (!has_nacp) {
            TimeStamp ts;
            if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, control.get(), sizeof(NsApplicationControlData), &actual_size))) {
                has_nacp = true;
                log_write("\t\t[ns control storage] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
            }
        }

        if (!has_nacp) {
            FakeNacpEntry(result.get());
        } else {
            NormalizeNacpLangData(control->nacp);

            bool valid = true;
            NacpLanguageEntry* lang;
            if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&control->nacp, &lang)) && lang) {
                result->lang = *lang;
            } else {
                FakeNacpEntry(result.get());
                valid = false;
            }

            if (!manual_load) {
                const auto jpeg_size = actual_size - sizeof(NacpStruct);
                result->icon.resize(jpeg_size);
                std::memcpy(result->icon.data(), control->icon, result->icon.size());
            }

            // add new entry to cache, if valid.
            if (valid) {
                nxtcAddEntry(app_id, &control->nacp, result->icon.size(), result->icon.data(), true);
            }

            result->status = NacpLoadStatus::Loaded;
        }
    }

    // load override from sys-tweak.
    if (result->status == NacpLoadStatus::Loaded) {
        const auto tweak_path = GetContentsPath(app_id);
        if (m_fs.DirExists(tweak_path)) {
            log_write("[TITLE] found contents path: %s\n", tweak_path.s);

            std::vector<u8> icon;
            m_fs.read_entire_file(fs::AppendPath(tweak_path, "icon.jpg"), icon);

            struct Overrides {
                std::string name;
                std::string author;
            } overrides;

            static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
                auto e = static_cast<Overrides*>(UserData);

                if (!std::strcmp(Section, "override_nacp")) {
                    if (!std::strcmp(Key, "name")) {
                        e->name = Value;
                    } else if (!std::strcmp(Key, "author")) {
                        e->author = Value;
                    }
                }

                return 1;
            };

            ini_browse(cb, &overrides, fs::AppendPath(tweak_path, "config.ini"));

            if (!icon.empty() && icon.size() < sizeof(NsApplicationControlData::icon)) {
                log_write("[TITLE] overriding icon: %zu -> %zu\n", result->icon.size(), icon.size());
                result->icon = icon;
            }

            if (!overrides.name.empty() && overrides.name.length() < sizeof(result->lang.name)) {
                log_write("[TITLE] overriding name: %s -> %s\n", result->lang.name, overrides.name.c_str());
                std::strcpy(result->lang.name, overrides.name.c_str());
            }

            if (!overrides.author.empty() && overrides.author.length() < sizeof(result->lang.author)) {
                log_write("[TITLE] overriding author: %s -> %s\n", result->lang.author, overrides.author.c_str());
                std::strcpy(result->lang.author, overrides.author.c_str());
            }
        }
    }

    SCOPED_MUTEX(&m_mutex_result);
    return m_result.emplace_back(std::move(result)).get();
}

void ThreadFunc(void* user) {
    auto data = static_cast<ThreadData*>(user);

    const bool cache_initialized = !data->IsTitleCacheEnabled() || nxtcInitialize();
    if (!cache_initialized) {
        log_write("[NXTC] failed to init cache\n");
    }
    ON_SCOPE_EXIT(nxtcExit());

    if (data->IsTitleCacheEnabled() && cache_initialized && !ini_getbool("cache", "nacp_format1_fix_v1", false, App::CONFIG_PATH)) {
        log_write("[NXTC] clearing cached titles for compressed NACP support\n");
        nxtcWipeCache();
        ini_putl("cache", "nacp_format1_fix_v1", 1, App::CONFIG_PATH);
    }

    while (data->IsRunning()) {
        data->Run();
    }
}

} // namespace

// starts background thread.
Result Init() {
    SCOPED_MUTEX(&g_mutex);

    if (!g_ref_count) {
        R_TRY(ns::Initialize());
        R_TRY(ncmInitialize());

        for (auto& e : ncm_entries) {
            e.Open();
        }

        g_thread_data = std::make_unique<ThreadData>(true);
        R_TRY(utils::CreateThread(&g_thread, ThreadFunc, g_thread_data.get(), 1024*32));
        R_TRY(threadStart(&g_thread));
    }

    g_ref_count++;
    R_SUCCEED();
}

void Exit() {
    SCOPED_MUTEX(&g_mutex);

    if (!g_ref_count) {
        return;
    }

    g_ref_count--;
    if (!g_ref_count) {
        g_thread_data->Close();

        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_data.reset();

        for (auto& e : ncm_entries) {
            e.Close();
        }

        ns::Exit();
        ncmExit();
    }
}

void Clear() {
    SCOPED_MUTEX(&g_mutex);
    if (g_thread_data) {
        g_thread_data->Clear();
    }
}

void PushAsync(u64 app_id) {
    SCOPED_MUTEX(&g_mutex);
    if (g_thread_data) {
        g_thread_data->PushAsync(app_id);
    }
}

void PushAsync(const std::span<const NsApplicationRecord> app_ids) {
    SCOPED_MUTEX(&g_mutex);
    if (g_thread_data) {
        g_thread_data->PushAsync(app_ids);
    }
}

auto GetAsync(u64 app_id) -> ThreadResultData* {
    SCOPED_MUTEX(&g_mutex);
    if (g_thread_data) {
        return g_thread_data->GetAsync(app_id);
    }
    return {};
}

auto Get(u64 app_id, bool* cached) -> ThreadResultData* {
    SCOPED_MUTEX(&g_mutex);
    if (g_thread_data) {
        return g_thread_data->Get(app_id, cached);
    }
    return {};
}

auto GetNcmCs(u8 storage_id) -> NcmContentStorage& {
    return GetNcmEntry(storage_id).cs;
}

auto GetNcmDb(u8 storage_id) -> NcmContentMetaDatabase& {
    return GetNcmEntry(storage_id).db;
}

Result ForEachApplicationRecord(const std::function<void(std::span<const NsApplicationRecord>)>& callback) {
    constexpr s32 ENTRY_CHUNK_COUNT = 1000;
    std::vector<NsApplicationRecord> records(ENTRY_CHUNK_COUNT);
    s32 offset{};

    while (true) {
        s32 count{};
        if (const auto rc = nsListApplicationRecord(records.data(), records.size(), offset, &count); R_FAILED(rc)) {
            log_write("failed to list application records at offset: %d\n", offset);
            return rc;
        }

        // finished parsing all entries.
        if (!count) {
            R_SUCCEED();
        }

        callback(std::span(records.data(), count));
        offset += count;
    }
}

Result GetMetaEntries(u64 id, MetaEntries& out, u32 flags) {
    s32 count;
    R_TRY(nsCountApplicationContentMeta(id, &count));

    std::vector<NsApplicationContentMetaStatus> entries(count);
    R_TRY(nsListApplicationContentMetaStatus(id, 0, entries.data(), entries.size(), &count));
    entries.resize(count);

    for (const auto& e : entries) {
        if (flags & ContentMetaTypeToContentFlag(e.meta_type)) {
            out.emplace_back(e);
        }
    }

    R_SUCCEED();
}

Result GetControlPathFromStatus(const NsApplicationContentMetaStatus& status, u64* out_program_id, fs::FsPath* out_path) {
    const auto& ee = status;
    if (ee.storageID != NcmStorageId_SdCard && ee.storageID != NcmStorageId_BuiltInUser && ee.storageID != NcmStorageId_GameCard) {
        return 0x1;
    }

    return GetControlPath(&GetNcmDb(ee.storageID), &GetNcmCs(ee.storageID), ee.application_id, out_program_id, out_path);
}

Result GetControlPath(NcmContentMetaDatabase* db, NcmContentStorage* cs, u64 id, u64* out_program_id, fs::FsPath* out_path) {
    NcmContentMetaKey key;
    R_TRY(ncmContentMetaDatabaseGetLatestContentMetaKey(db, &key, id));

    NcmContentId content_id;
    R_TRY(ncmContentMetaDatabaseGetContentIdByType(db, &content_id, &key, NcmContentType_Control));

    return ncm::GetFsPathFromContentId(cs, key, content_id, out_program_id, out_path);
}

auto GetEnglishTitleName(u64 app_id) -> std::string {
    NacpStruct nacp{};
    if (R_FAILED(LoadControlNacp(app_id, nacp, nullptr))) {
        return {};
    }

    NormalizeNacpLangData(nacp);

    // nacp entries are NOT indexed by SetLanguage, they use their own order
    // (see g_nacpLanguageTable in libnx). SetLanguage_ENUS would read
    // BritishEnglish and SetLanguage_ENGB would read Korean.
    // slot 0 is also where NormalizeNacpLangData() stores the fallback name.
    enum NacpLanguage {
        NacpLanguage_AmericanEnglish = 0,
        NacpLanguage_BritishEnglish = 1,
    };

    for (const auto language : {NacpLanguage_AmericanEnglish, NacpLanguage_BritishEnglish}) {
        const auto& entry = NacpLanguageEntries(nacp)[language];
        if (entry.name[0]) {
            return {std::begin(entry.name), std::find(std::begin(entry.name), std::end(entry.name), '\0')};
        }
    }

    return {};
}

auto MakeExportTitleName(std::string_view current_name, std::string_view english_name, bool fix_name) -> std::string {
    const auto sanitise = [](std::string_view src, bool ascii_only) -> std::string {
        std::string name{src};
        if (name.empty()) {
            return {};
        }

        utilsReplaceIllegalCharacters(name.data(), ascii_only);
        name.resize(std::strlen(name.c_str()));

        const auto usable = std::ranges::any_of(name, [](unsigned char c) {
            return c != '_' && c != ' ' && c != '.';
        });
        return usable ? name : std::string{};
    };

    if (fix_name) {
        // the english title is preferred as it's always ascii safe.
        if (auto name = sanitise(english_name, true); !name.empty()) {
            return name;
        }

        // the english title isn't always available (title has no english nacp
        // entry, the control nca failed to parse, sys-tweak override, ...).
        // fallback to the displayed name rather than losing the name entirely.
        // if that has no ascii safe characters either, fall back to the title id.
        return sanitise(current_name, true);
    }

    return sanitise(current_name, false);
}

// taken from nxdumptool.
void utilsReplaceIllegalCharacters(char *str, bool ascii_only)
{
    static const char g_illegalFileSystemChars[] = "\\/:*?\"<>|";

    size_t str_size = 0, cur_pos = 0;

    if (!str || !(str_size = strlen(str))) return;

    u8 *ptr1 = (u8*)str, *ptr2 = ptr1;
    ssize_t units = 0;
    u32 code = 0;
    bool repl = false;

    while(cur_pos < str_size)
    {
        units = decode_utf8(&code, ptr1);
        if (units < 0) break;

        if (code < 0x20 || (!ascii_only && code == 0x7F) || (ascii_only && code >= 0x7F) || \
            (units == 1 && memchr(g_illegalFileSystemChars, (int)code, std::size(g_illegalFileSystemChars))))
        {
            if (!repl)
            {
                *ptr2++ = '_';
                repl = true;
            }
        } else {
            if (ptr2 != ptr1) memmove(ptr2, ptr1, (size_t)units);
            ptr2 += units;
            repl = false;
        }

        ptr1 += units;
        cur_pos += (size_t)units;
    }

    *ptr2 = '\0';
}

auto GetContentsPath(u64 app_id) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "/atmosphere/contents/%016lX", app_id);
    return path;
}

} // namespace sphaira::title

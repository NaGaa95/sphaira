#include "download.hpp"
#include "log.hpp"
#include "defines.hpp"
#include "evman.hpp"
#include "fs.hpp"
#include "app.hpp"
#include "utils/thread.hpp"

#include <switch.h>
#include <cstring>
#include <cassert>
#include <vector>
#include <deque>
#include <mutex>
#include <algorithm>
#include <ranges>
#include <curl/curl.h>
#include <yyjson.h>

namespace sphaira::curl {
namespace {

#define CURL_EASY_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_easy_setopt(handle, opt, v); r != CURLE_OK) { \
        log_write("curl_easy_setopt(%s, %s) msg: %s\n", #opt, #v, curl_easy_strerror(r)); \
    } \

#define CURL_SHARE_SETOPT_LOG(handle, opt, v) \
    if (auto r = curl_share_setopt(handle, opt, v); r != CURLSHE_OK) { \
        log_write("curl_share_setopt(%s, %s) msg: %s\n", #opt, #v, curl_share_strerror(r)); \
    } \

constexpr u64 CHUNK_SIZE = 1024*1024;
constexpr auto MAX_THREADS = 4;

std::atomic_bool g_running{};
CURLSH* g_curl_share{};
// the handle for blocking requests, kept so one isn't created for each. any thread
// can make one, so it is used only under its mutex (see RunBlocking).
CURL* g_curl_single{};
Mutex g_curl_single_mutex{};
Mutex g_mutex_share[CURL_LOCK_DATA_LAST]{};

struct UploadStruct {
    std::span<const u8> data;
    s64 offset{};
    s64 size{};
    fs::File f{};
};

struct DataStruct {
    std::vector<u8> data;
    s64 offset{};
    fs::File f{};
    s64 file_offset{};
};

struct SeekCustomData {
    OnUploadSeek cb{};
    s64 size{};
};

auto generate_key_from_path(const fs::FsPath& path) -> std::string {
    const auto key = crc32Calculate(path.s, path.size());
    return std::to_string(key);
}

void Yield() {
    svcSleepThread(YieldType_WithoutCoreMigration);
}

struct Cache {
    using Value = std::pair<std::string, std::string>;

    bool init() {
        SCOPED_MUTEX(&m_mutex);

        if (!m_json) {
            auto json_in = yyjson_read_file(JSON_PATH, YYJSON_READ_NOFLAG, nullptr, nullptr);
            if (json_in) {
                log_write("loading old json doc\n");
                m_json = yyjson_doc_mut_copy(json_in, nullptr);
                yyjson_doc_free(json_in);
                m_root = yyjson_mut_doc_get_root(m_json);
            } else {
                log_write("creating new json doc\n");
                m_json = yyjson_mut_doc_new(nullptr);
                m_root = yyjson_mut_obj(m_json);
                yyjson_mut_doc_set_root(m_json, m_root);
            }
        }

        if (!m_json) {
            return false;
        }

        m_init_ref_count++;
        log_write("[ETAG] init: %u\n", m_init_ref_count);
        return true;
    }

    void exit() {
        SCOPED_MUTEX(&m_mutex);

        if (!m_json) {
            return;
        }

        m_init_ref_count--;
        if (m_init_ref_count) {
            return;
        }

        // note: this takes 20ms
        if (!yyjson_mut_write_file(JSON_PATH, m_json, YYJSON_WRITE_NOFLAG, nullptr, nullptr)) {
            log_write("[ETAG] failed to write etag json: %s\n", JSON_PATH.s);
        }

        yyjson_mut_doc_free(m_json);
        m_json = nullptr;
        m_root = nullptr;
        log_write("[ETAG] exit\n");
    }

    // the download threads and any thread making a blocking request can all be
    // in here at once.
    void get(const fs::FsPath& path, curl::Header& header) {
        SCOPED_MUTEX(&m_mutex);

        const auto [etag, last_modified] = get_internal(path);
        if (!etag.empty()) {
            header.m_map.emplace("if-none-match", etag);
        }

        if (!last_modified.empty()) {
            header.m_map.emplace("if-modified-since", last_modified);
        }
    }

    void set(const fs::FsPath& path, const curl::Header& value) {
        SCOPED_MUTEX(&m_mutex);

        std::string etag_str;
        std::string last_modified_str;

        if (auto it = value.Find(ETAG_STR); it != value.m_map.end()) {
            etag_str = it->second;
        }
        if (auto it = value.Find(LAST_MODIFIED_STR); it != value.m_map.end()) {
            last_modified_str = it->second;
        }

        if (!etag_str.empty() || !last_modified_str.empty()) {
            set_internal(path, Value{etag_str, last_modified_str});
        }
    }

private:
    auto get_internal(const fs::FsPath& path) -> Value {
        if (!fs::FsNativeSd().FileExists(path)) {
            return {};
        }

        const auto kkey = generate_key_from_path(path);
        const auto it = m_cache.find(kkey);
        if (it != m_cache.end()) {
            return it->second;
        }

        auto hash_key = yyjson_mut_obj_getn(m_root, kkey.c_str(), kkey.length());
        if (!hash_key) {
            return {};
        }

        auto etag_key = yyjson_mut_obj_get(hash_key, ETAG_STR);
        auto last_modified_key = yyjson_mut_obj_get(hash_key, LAST_MODIFIED_STR);

        const auto etag_value = yyjson_mut_get_str(etag_key);
        const auto etag_value_len = yyjson_mut_get_len(etag_key);
        const auto last_modified_value = yyjson_mut_get_str(last_modified_key);
        const auto last_modified_value_len = yyjson_mut_get_len(last_modified_key);

        if ((!etag_value || !etag_value_len) && (!last_modified_value || !last_modified_value_len)) {
            return {};
        }

        std::string etag;
        std::string last_modified;
        if (etag_value && etag_value_len) {
            etag.assign(etag_value, etag_value_len);
        }
        if (last_modified_value && last_modified_value_len) {
            last_modified.assign(last_modified_value, last_modified_value_len);
        }

        const Value ret{etag, last_modified};
        m_cache.insert_or_assign(it, kkey, ret);
        return ret;
    }

    void set_internal(const fs::FsPath& path, const Value& value) {
        const auto kkey = generate_key_from_path(path);

        // check if we already have this entry
        const auto it = m_cache.find(kkey);
        if (it != m_cache.end() && it->second == value) {
            log_write("already has etag, not updating, path: %s key: %s\n", path.s, kkey.c_str());
            return;
        }

        if (it != m_cache.end()) {
            log_write("updating etag, path: %s key: %s\n", path.s, kkey.c_str());
        } else {
            log_write("setting new etag, path: %s key: %s\n", path.s, kkey.c_str());
        }

        // insert new entry into cache, this will never fail.
        const auto& [jkey, jvalue] = *m_cache.insert_or_assign(it, kkey, value);
        const auto& [etag, last_modified] = jvalue;

        // check if we need to add a new entry to root or simply update the value.
        auto hash_key = yyjson_mut_obj_getn(m_root, kkey.c_str(), kkey.length());
        if (!hash_key) {
            hash_key = yyjson_mut_obj_add_obj(m_json, m_root, jkey.c_str());
        }

        if (!hash_key) {
            log_write("failed to set new cache key obj, path: %s key: %s\n", path.s, jkey.c_str());
        } else {
            const auto update_entry = [this, &hash_key](const char* tag, const std::string& value) {
                if (value.empty()) {
                    // workaround for appstore accepting etags but not returning them.
                    yyjson_mut_obj_remove_str(hash_key, tag);
                    return true;
                } else {
                    auto key = yyjson_mut_obj_get(hash_key, tag);
                    if (!key) {
                        return yyjson_mut_obj_add_str(m_json, hash_key, tag, value.c_str());
                    } else {
                        return yyjson_mut_set_str(key, value.c_str());
                    }
                }
            };

            if (!update_entry("etag", etag)) {
                log_write("failed to set new etag, path: %s key: %s\n", path.s, jkey.c_str());
            }

            if (!update_entry("last-modified", last_modified)) {
                log_write("failed to set new last-modified, path: %s key: %s\n", path.s, jkey.c_str());
            }
        }
    }

    static constexpr inline fs::FsPath JSON_PATH{"/switch/sphaira/cache/etag_v2.json"};
    static constexpr inline const char* ETAG_STR{"etag"};
    static constexpr inline const char* LAST_MODIFIED_STR{"last-modified"};

    Mutex m_mutex{};
    yyjson_mut_doc* m_json{};
    yyjson_mut_val* m_root{};
    std::unordered_map<std::string, Value> m_cache{};
    u32 m_init_ref_count{};
};

struct ThreadEntry {
    auto Create() -> Result {
        m_curl = curl_easy_init();
        R_UNLESS(m_curl != nullptr, Result_CurlFailedEasyInit);

        ueventCreate(&m_uevent, true);
        R_TRY(utils::CreateThread(&m_thread, ThreadFunc, this, 1024*32));
        R_TRY(threadStart(&m_thread));
        R_SUCCEED();
    }

    void SignalClose() {
        ueventSignal(&m_uevent);
    }

    void Close() {
        SignalClose();
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        if (m_curl) {
            curl_easy_cleanup(m_curl);
            m_curl = nullptr;
        }
    }

    auto InProgress() -> bool {
        return m_in_progress == true;
    }

    auto Setup(const Api& api) -> bool {
        assert(m_in_progress == false && "Setting up thread while active");
        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        if (m_in_progress) {
            return false;
        }
        m_api = api;
        m_in_progress = true;
        // log_write("started download :)\n");
        ueventSignal(&m_uevent);
        return true;
    }

    static void ThreadFunc(void* p);

    CURL* m_curl{};
    Thread m_thread{};
    Api m_api{};
    std::atomic_bool m_in_progress{};
    Mutex m_mutex{};
    UEvent m_uevent{};
};

struct ThreadQueueEntry {
    Api api;
    bool m_delete{};
};

struct ThreadQueue {
    std::deque<ThreadQueueEntry> m_entries{};
    Thread m_thread{};
    Mutex m_mutex{};
    UEvent m_uevent{};

    auto Create() -> Result {
        ueventCreate(&m_uevent, true);
        R_TRY(utils::CreateThread(&m_thread, ThreadFunc, this, 1024*32));
        R_TRY(threadStart(&m_thread));
        R_SUCCEED();
    }

    void SignalClose() {
        ueventSignal(&m_uevent);
    }

    void Close() {
        SignalClose();
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
    }

    auto Add(const Api& api, bool is_upload = false) -> bool {
        if (api.GetUrl().empty() || !api.GetOnComplete()) {
            return false;
        }

        mutexLock(&m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&m_mutex));

        switch (api.GetPriority()) {
            case Priority::Normal:
                m_entries.emplace_back(api).api.SetUpload(is_upload);
                break;
            case Priority::High:
                m_entries.emplace_front(api).api.SetUpload(is_upload);
                break;
        }

        ueventSignal(&m_uevent);
        return true;
    }

    static void ThreadFunc(void* p);
};

ThreadEntry g_threads[MAX_THREADS]{};
ThreadQueue g_thread_queue;
Cache g_cache;

void GetDownloadTempPath(fs::FsPath& buf) {
    static Mutex mutex{};
    static u64 count{};

    mutexLock(&mutex);
    const auto count_copy = count;
    count++;
    mutexUnlock(&mutex);

    std::snprintf(buf, sizeof(buf), "/switch/sphaira/cache/download_temp%lu", count_copy);
}

auto ProgressCallbackFunc(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> size_t {
    auto api = static_cast<Api*>(clientp);
    if (!g_running || api->GetToken().stop_requested()) {
        return 1;
    }

    // log_write("pcall called %u %u %u %u\n", dltotal, dlnow, ultotal, ulnow);
    if (api->GetOnProgress() && !api->GetOnProgress()(dltotal, dlnow, ultotal, ulnow)) {
        return 1;
    }

    Yield();
    return 0;
}

auto SeekCallback(void *clientp, curl_off_t offset, int origin) -> int {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<UploadStruct*>(clientp);

    if (origin == SEEK_SET) {
        offset = offset;
    } else if (origin == SEEK_CUR) {
        offset = data_struct->offset + offset;
    } else if (origin == SEEK_END) {
        offset = data_struct->size;
    }

    if (offset < 0 || offset > data_struct->size) {
        return CURL_SEEKFUNC_CANTSEEK;
    }

    data_struct->offset = offset;
    return CURL_SEEKFUNC_OK;
}

auto SeekCustomCallback(void *clientp, curl_off_t offset, int origin) -> int {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<SeekCustomData*>(clientp);
    if (origin != SEEK_SET || offset < 0 || offset > data_struct->size) {
        return CURL_SEEKFUNC_CANTSEEK;
    }

    if (!data_struct->cb(offset)) {
        return CURL_SEEKFUNC_CANTSEEK;
    }

    return CURL_SEEKFUNC_OK;
}

auto ReadFileCallback(char *ptr, size_t size, size_t nmemb, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<UploadStruct*>(userp);
    const auto realsize = size * nmemb;

    u64 bytes_read;
    if (R_FAILED(data_struct->f.Read(data_struct->offset, ptr, realsize, FsReadOption_None, &bytes_read))) {
        log_write("reading file error\n");
        return 0;
    }

    data_struct->offset += bytes_read;
    Yield();
    return bytes_read;
}

auto ReadMemoryCallback(char *ptr, size_t size, size_t nmemb, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<UploadStruct*>(userp);
    auto realsize = size * nmemb;
    realsize = std::min(realsize, data_struct->data.size() - data_struct->offset);

    std::memcpy(ptr, data_struct->data.data(), realsize);
    data_struct->offset += realsize;

    Yield();
    return realsize;
}

auto ReadCustomCallback(char *ptr, size_t size, size_t nmemb, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<UploadInfo*>(userp);
    auto realsize = size * nmemb;
    const auto result = data_struct->m_callback(ptr, realsize);

    Yield();
    return result;
}

auto WriteMemoryCallback(void *contents, size_t size, size_t num_files, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<DataStruct*>(userp);
    const auto realsize = size * num_files;

    // give it more memory
    if (data_struct->data.capacity() < data_struct->offset + realsize) {
        data_struct->data.reserve(data_struct->data.capacity() + CHUNK_SIZE);
    }

    data_struct->data.resize(data_struct->offset + realsize);
    std::memcpy(data_struct->data.data() + data_struct->offset, contents, realsize);
    data_struct->offset += realsize;

    Yield();
    return realsize;
}

auto WriteFileCallback(void *contents, size_t size, size_t num_files, void *userp) -> size_t {
    if (!g_running) {
        return 0;
    }

    auto data_struct = static_cast<DataStruct*>(userp);
    const auto realsize = size * num_files;

    // flush data if incomming data would overflow the buffer
    if (data_struct->offset && data_struct->data.size() < data_struct->offset + realsize) {
        if (R_FAILED(data_struct->f.Write(data_struct->file_offset, data_struct->data.data(), data_struct->offset, FsWriteOption_None))) {
            return 0;
        }

        data_struct->file_offset += data_struct->offset;
        data_struct->offset = 0;
    }

    // we have a huge chunk! write it directly to file
    if (data_struct->data.size() < realsize) {
        if (R_FAILED(data_struct->f.Write(data_struct->file_offset, contents, realsize, FsWriteOption_None))) {
            return 0;
        }

        data_struct->file_offset += realsize;
    } else {
        // buffer data until later
        std::memcpy(data_struct->data.data() + data_struct->offset, contents, realsize);
        data_struct->offset += realsize;
    }

    Yield();
    return realsize;
}

auto header_callback(char* b, size_t size, size_t nitems, void* userdata) -> size_t {
    auto header = static_cast<Header*>(userdata);
    const auto numbytes = size * nitems;

    if (b && numbytes) {
        const auto dilem = (const char*)memchr(b, ':', numbytes);
        if (dilem) {
            const int key_len = dilem - b;
            const int value_len = numbytes - key_len - 4; // "\r\n"
            if (key_len > 0 && value_len > 0) {
                const std::string key(b, key_len);
                const std::string value(dilem + 2, value_len);
                header->m_map.insert_or_assign(key, value);
            }
        }
    }

    return numbytes;
}

auto EscapeString(CURL* curl, const std::string& str) -> std::string {
    char* s{};
    if (!curl) {
        s = curl_escape(str.data(), str.length());
    } else {
        s = curl_easy_escape(curl, str.data(), str.length());
    }

    if (!s) {
        return str;
    }

    const std::string result = s;
    curl_free(s);
    return result;
}

auto EncodeUrl(const std::string& url) -> std::string {
    log_write("[CURL] encoding url\n");

    auto clu = curl_url();
    R_UNLESS(clu, url);
    ON_SCOPE_EXIT(curl_url_cleanup(clu));

    log_write("[CURL] setting url\n");
    CURLUcode clu_code;
    clu_code = curl_url_set(clu, CURLUPART_URL, url.c_str(), CURLU_DEFAULT_SCHEME | CURLU_URLENCODE);
    R_UNLESS(clu_code == CURLUE_OK, url);
    log_write("[CURL] set url success\n");

    char* encoded_url;
    clu_code = curl_url_get(clu, CURLUPART_URL, &encoded_url, 0);
    R_UNLESS(clu_code == CURLUE_OK, url);

    log_write("[CURL] encoded url: %s [vs]: %s\n", encoded_url, url.c_str());
    const std::string out = encoded_url;
    curl_free(encoded_url);
    return out;
}

void SetCommonCurlOptions(CURL* curl, const Api& e) {
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_USERAGENT, USER_AGENT);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_FAILONERROR, (e.GetFlags() & Flag_KeepErrorBody) ? 0L : 1L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_NOPROGRESS, 0L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_SHARE, g_curl_share);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_BUFFERSIZE, 1024*512);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1024*512);

    // enable all forms of compression supported by libcurl.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_ACCEPT_ENCODING, "");

    // for smb / ftp, try and use ssl if possible.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);

    // in most cases, this will use CURLAUTH_BASIC.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);

    // enable TE is server supports it.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_TRANSFER_ENCODING, 1L);

    // set flags.
    if (e.GetFlags() & Flag_NoBody) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_NOBODY, 1L);
    }

    // set custom request.
    if (!e.GetCustomRequest().empty()) {
        log_write("[CURL] setting custom request: %s\n", e.GetCustomRequest().c_str());
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_CUSTOMREQUEST, e.GetCustomRequest().c_str());
    }

    // set oath2 bearer.
    if (!e.GetBearer().empty()) {
        // CURLOPT_XOAUTH2_BEARER only supplies the token. Explicitly selecting
        // bearer authentication makes libcurl send the Authorization header on
        // the initial HTTP request instead of waiting for an auth challenge.
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BEARER);
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_XOAUTH2_BEARER, e.GetBearer().c_str());
    }

    // set ssh pub/priv key file.
    if (!e.GetPubKey().empty()) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSH_PUBLIC_KEYFILE, e.GetPubKey().c_str());
    }
    if (!e.GetPrivKey().empty()) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_SSH_PRIVATE_KEYFILE, e.GetPrivKey().c_str());
    }

    // set auth.
    if (!e.GetUserPass().m_user.empty()) {
        // CURLAUTH_ANY has no scheme until the server challenges, so libcurl
        // probes unauthenticated first; naming basic up front skips that.
        if (e.GetPreemptiveAuth() && e.GetBearer().empty()) {
            CURL_EASY_SETOPT_LOG(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        }
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_USERPWD, e.GetUserPass().m_user.c_str());
    }
    if (!e.GetUserPass().m_pass.empty()) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_PASSWORD, e.GetUserPass().m_pass.c_str());
    }

    // set port, if valid.
    if (e.GetPort()) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_PORT, (long)e.GetPort());
    }

    // progress calls, with or without a callback: they are also where a stop token
    // cancels the request.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_XFERINFODATA, &e);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackFunc);

}
auto DownloadInternal(CURL* curl, const Api& e) -> ApiResult {
    App::SetAutoSleepDisabled(true);
    ON_SCOPE_EXIT(App::SetAutoSleepDisabled(false));

    // check if stop has been requested before starting download
    if (e.GetToken().stop_requested()) {
        return {};
    }

    fs::FsPath tmp_buf;
    const bool has_file = !e.GetPath().empty() && e.GetPath() != "";
    const bool has_post = !e.GetFields().empty() && e.GetFields() != "";
    const auto encoded_url = EncodeUrl(e.GetUrl());

    DataStruct chunk;
    Header header_in = e.GetHeader();
    Header header_out;
    fs::FsNativeSd fs;

    if (has_file) {
        GetDownloadTempPath(tmp_buf);
        fs.CreateDirectoryRecursivelyWithPath(tmp_buf);

        if (auto rc = fs.CreateFile(tmp_buf, 0, 0); R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
            log_write("failed to create file: %s\n", tmp_buf.s);
            return {};
        }

        if (R_FAILED(fs.OpenFile(tmp_buf, FsOpenMode_Write|FsOpenMode_Append, &chunk.f))) {
            log_write("failed to open file: %s\n", tmp_buf.s);
            return {};
        }

        // only add etag if the dst file still exists.
        if ((e.GetFlags() & Flag_Cache) && fs::FileExists(&fs.m_fs, e.GetPath())) {
            g_cache.get(e.GetPath(), header_in);
        }
    }

    // reserve the first chunk
    chunk.data.reserve(CHUNK_SIZE);

    curl_easy_reset(curl);
    SetCommonCurlOptions(curl, e);

    CURL_EASY_SETOPT_LOG(curl, CURLOPT_URL, encoded_url.c_str());
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_HEADERFUNCTION, header_callback);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_HEADERDATA, &header_out);

    if (has_post) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_POSTFIELDS, e.GetFields().c_str());
        log_write("setting post field: %s\n", e.GetFields().c_str());
    }

    struct curl_slist* list = NULL;
    ON_SCOPE_EXIT(if (list) { curl_slist_free_all(list); } );

    for (const auto& [key, value] : header_in.m_map) {
        if (value.empty()) {
            continue;
        }

        // create header key value pair.
        const auto header_str = key + ": " + value;

        // try to append header chunk.
        auto temp = curl_slist_append(list, header_str.c_str());
        if (temp) {
            log_write("adding header: %s\n", header_str.c_str());
            list = temp;
        } else {
            log_write("failed to append header\n");
        }
    }

    if (list) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_HTTPHEADER, list);
    }

    // write calls.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEFUNCTION, has_file ? WriteFileCallback : WriteMemoryCallback);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEDATA, &chunk);

    // perform download and cleanup after and report the result.
    const auto res = curl_easy_perform(curl);
    bool success = res == CURLE_OK;

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (has_file) {
        ON_SCOPE_EXIT( fs.DeleteFile(tmp_buf) );
        if (res == CURLE_OK && chunk.offset) {
            chunk.f.Write(chunk.file_offset, chunk.data.data(), chunk.offset, FsWriteOption_None);
        }

        chunk.f.Close();

        if (res == CURLE_OK) {
            if (http_code == 304) {
                log_write("cached download: %s\n", e.GetUrl().c_str());
            } else {
                log_write("un-cached download: %s code: %lu\n", e.GetUrl().c_str(), http_code);
                if (e.GetFlags() & Flag_Cache) {
                    g_cache.set(e.GetPath(), header_out);
                }

                // enable to log received headers.
                #if 0
                log_write("\n\nLOGGING HEADER\n");
                    for (auto [a, b] : header_out.m_map) {
                        log_write("\t%s: %s\n", a.c_str(), b.c_str());
                    }
                log_write("\n\n");
                #endif

                fs.DeleteFile(e.GetPath());
                fs.CreateDirectoryRecursivelyWithPath(e.GetPath());
                if (R_FAILED(fs.RenameFile(tmp_buf, e.GetPath()))) {
                    success = false;
                }
            }
        }
        chunk.data.clear();
    } else {
        // empty data if we failed
        if (res != CURLE_OK) {
            chunk.data.clear();
        }
    }

    log_write("Downloaded %s code: %ld %s\n", e.GetUrl().c_str(), http_code, curl_easy_strerror(res));
    return {success, http_code, header_out, chunk.data, e.GetPath()};
}

auto UploadInternal(CURL* curl, const Api& e) -> ApiResult {
    // check if stop has been requested before starting download
    if (e.GetToken().stop_requested()) {
        return {};
    }

    const auto& info = e.GetUploadInfo();
    const auto url = e.GetUrl() + "/" + info.m_name;
    const auto encoded_url = EncodeUrl(url);
    const bool has_file = !e.GetPath().empty() && e.GetPath() != "";

    UploadStruct chunk{};
    DataStruct chunk_out{};
    SeekCustomData seek_data{};
    Header header_in = e.GetHeader();
    Header header_out;
    fs::FsNativeSd fs{};

    if (has_file) {
        if (R_FAILED(fs.OpenFile(e.GetPath(), FsOpenMode_Read, &chunk.f))) {
            log_write("failed to open file: %s\n", e.GetPath().s);
            return {};
        }

        chunk.f.GetSize(&chunk.size);
        log_write("got chunk size: %zd\n", chunk.size);
    } else {
        if (info.m_callback) {
            chunk.size = info.m_size;
            log_write("setting upload size: %zu\n", chunk.size);
        } else {
            chunk.size = info.m_data.size();
            chunk.data = info.m_data;
        }
    }

    if (url.starts_with("file://")) {
        const auto folder_path = fs::AppendPath("/", url.substr(std::strlen("file://")));
        log_write("creating local folder: %s\n", folder_path.s);
        // create the folder as libcurl doesn't seem to manually create it.
        fs.CreateDirectoryRecursivelyWithPath(folder_path);
        // remove the path so that libcurl can upload over it.
        fs.DeleteFile(folder_path);
    }

    // reserve the first chunk
    chunk_out.data.reserve(CHUNK_SIZE);

    curl_easy_reset(curl);
    SetCommonCurlOptions(curl, e);

    CURL_EASY_SETOPT_LOG(curl, CURLOPT_URL, encoded_url.c_str());
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_HEADERFUNCTION, header_callback);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_HEADERDATA, &header_out);

    CURL_EASY_SETOPT_LOG(curl, CURLOPT_UPLOAD, 1L);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)chunk.size);

    // instruct libcurl to create ftp folders if they don't yet exist.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR_RETRY);

    struct curl_slist* list = NULL;
    ON_SCOPE_EXIT(if (list) { curl_slist_free_all(list); } );

    for (const auto& [key, value] : header_in.m_map) {
        if (value.empty()) {
            continue;
        }

        // create header key value pair.
        const auto header_str = key + ": " + value;

        // try to append header chunk.
        auto temp = curl_slist_append(list, header_str.c_str());
        if (temp) {
            log_write("adding header: %s\n", header_str.c_str());
            list = temp;
        } else {
            log_write("failed to append header\n");
        }
    }

    if (list) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_HTTPHEADER, list);
    }

    // set callback for reading more data.
    if (info.m_callback) {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_READFUNCTION, ReadCustomCallback);
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_READDATA, &info);

        if (e.GetOnUploadSeek()) {
            seek_data.cb = e.GetOnUploadSeek();
            seek_data.size = chunk.size;
            CURL_EASY_SETOPT_LOG(curl, CURLOPT_SEEKFUNCTION, SeekCustomCallback);
            CURL_EASY_SETOPT_LOG(curl, CURLOPT_SEEKDATA, &seek_data);
        }
    } else {
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_READFUNCTION, has_file ? ReadFileCallback : ReadMemoryCallback);
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_READDATA, &chunk);

        // allow for seeking upon uploads, may be used for ftp and http.
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_SEEKFUNCTION, SeekCallback);
        CURL_EASY_SETOPT_LOG(curl, CURLOPT_SEEKDATA, &chunk);
    }

    // write calls.
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    CURL_EASY_SETOPT_LOG(curl, CURLOPT_WRITEDATA, &chunk_out);

    // perform upload and cleanup after and report the result.
    const auto res = curl_easy_perform(curl);
    bool success = res == CURLE_OK;

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (has_file) {
        chunk.f.Close();
    }

    log_write("Uploaded %s code: %ld %s\n", url.c_str(), http_code, curl_easy_strerror(res));
    return {success, http_code, header_out, chunk_out.data};
}

void my_lock(CURL *handle, curl_lock_data data, curl_lock_access laccess, void *useptr) {
    mutexLock(&g_mutex_share[data]);
}

void my_unlock(CURL *handle, curl_lock_data data, void *useptr) {
    mutexUnlock(&g_mutex_share[data]);
}

void ThreadEntry::ThreadFunc(void* p) {
    auto data = static_cast<ThreadEntry*>(p);

    if (!g_cache.init()) {
        log_write("failed to init json cache\n");
    }
    ON_SCOPE_EXIT(g_cache.exit());

    while (g_running) {
        auto rc = waitSingle(waiterForUEvent(&data->m_uevent), UINT64_MAX);
        // log_write("woke up\n");
        if (!g_running) {
            break;
        }

        if (R_FAILED(rc)) {
            continue;
        }

        const auto result = data->m_api.IsUpload() ? UploadInternal(data->m_curl, data->m_api) : DownloadInternal(data->m_curl, data->m_api);
        if (g_running && data->m_api.GetOnComplete() && !data->m_api.GetToken().stop_requested()) {
            evman::push(
                DownloadEventData{data->m_api.GetOnComplete(), result, data->m_api.GetToken()},
                false
            );
        }

        data->m_in_progress = false;
        // notify the queue that there's a space free
        ueventSignal(&g_thread_queue.m_uevent);
    }
    log_write("exited download thread\n");
}

void ThreadQueue::ThreadFunc(void* p) {
    auto data = static_cast<ThreadQueue*>(p);

    while (g_running) {
        auto rc = waitSingle(waiterForUEvent(&data->m_uevent), UINT64_MAX);
        log_write("[thread queue] woke up\n");
        if (!g_running) {
            return;
        }
        if (R_FAILED(rc)) {
            continue;
        }

        mutexLock(&data->m_mutex);
        ON_SCOPE_EXIT(mutexUnlock(&data->m_mutex));
        if (data->m_entries.empty()) {
            continue;
        }

        // find the next avaliable thread
        for (auto& entry : data->m_entries) {
            if (!g_running) {
                return;
            }

            bool keep_going{};

            for (auto& thread : g_threads) {
                if (!g_running) {
                    return;
                }

                if (!thread.InProgress()) {
                    if (thread.Setup(entry.api)) {
                        // log_write("[dl queue] starting download\n");
                        // mark entry for deletion
                        entry.m_delete = true;
                        // pop_count++;
                        keep_going = true;
                        break;
                    }
                }
            }

            if (!keep_going) {
                break;
            }
        }

        // delete all entries marked for deletion
        std::erase_if(data->m_entries, [](auto& e){
            return e.m_delete;
        });
    }

    log_write("exited download thread queue\n");
}

} // namespace

auto Init() -> bool {
    if (CURLE_OK != curl_global_init(CURL_GLOBAL_DEFAULT)) {
        return false;
    }

    g_curl_share = curl_share_init();
    if (g_curl_share) {
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_LOCKFUNC, my_lock);
        CURL_SHARE_SETOPT_LOG(g_curl_share, CURLSHOPT_UNLOCKFUNC, my_unlock);
    }

    g_running = true;

    if (R_FAILED(g_thread_queue.Create())) {
        log_write("!failed to create download thread queue\n");
    }

    for (auto& entry : g_threads) {
        if (R_FAILED(entry.Create())) {
            log_write("!failed to create download thread\n");
        }
    }

    g_curl_single = curl_easy_init();
    if (!g_curl_single) {
        log_write("failed to create g_curl_single\n");
    }

    log_write("finished creating threads\n");

    return true;
}

void ExitSignal() {
    g_running = false;

    g_thread_queue.SignalClose();

    for (auto& entry : g_threads) {
        entry.SignalClose();
    }
}

void Exit() {
    ExitSignal();

    g_thread_queue.Close();

    if (g_curl_single) {
        curl_easy_cleanup(g_curl_single);
        g_curl_single = nullptr;
    }

    for (auto& entry : g_threads) {
        entry.Close();
    }

    if (g_curl_share) {
        curl_share_cleanup(g_curl_share);
        g_curl_share = {};
    }

    curl_global_cleanup();
}

namespace {

// runs on g_curl_single when it is free, and on a handle of its own rather than
// waiting when it isn't: a long download would otherwise hold up every other
// blocking request. the two behave alike, since what a handle reuses -
// connections, dns, tls sessions - lives in g_curl_share.
auto RunBlocking(const Api& e, bool upload) -> ApiResult {
    const auto run = [&e, upload](CURL* curl) {
        return upload ? UploadInternal(curl, e) : DownloadInternal(curl, e);
    };

    if (mutexTryLock(&g_curl_single_mutex)) {
        ON_SCOPE_EXIT(mutexUnlock(&g_curl_single_mutex));
        return run(g_curl_single);
    }

    log_write("[CURL] blocking handle busy, using one of its own\n");
    const auto curl = curl_easy_init();
    if (!curl) {
        return {};
    }
    ON_SCOPE_EXIT(curl_easy_cleanup(curl));
    return run(curl);
}

} // namespace

auto ToMemory(const Api& e) -> ApiResult {
    if (!e.GetPath().empty()) {
        return {};
    }
    return RunBlocking(e, false);
}

auto ToFile(const Api& e) -> ApiResult {
    if (e.GetPath().empty()) {
        return {};
    }
    return RunBlocking(e, false);
}

auto FromMemory(const Api& e) -> ApiResult {
    if (!e.GetPath().empty()) {
        return {};
    }
    return RunBlocking(e, true);
}

auto FromFile(const Api& e) -> ApiResult {
    if (e.GetPath().empty()) {
        return {};
    }
    return RunBlocking(e, true);
}

auto ToMemoryAsync(const Api& api) -> bool {
    return g_thread_queue.Add(api);
}

auto ToFileAsync(const Api& e) -> bool {
    return g_thread_queue.Add(e);
}

auto FromMemoryAsync(const Api& api) -> bool {
    return g_thread_queue.Add(api, true);
}

auto FromFileAsync(const Api& e) -> bool {
    return g_thread_queue.Add(e, true);
}

auto EscapeString(const std::string& str) -> std::string {
    return EscapeString(nullptr, str);
}

} // namespace sphaira::curl

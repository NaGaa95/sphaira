#pragma once

#include "yati/source/file.hpp"
#include "utils/lru.hpp"
#include "location.hpp"
#include <memory>
#include <optional>
#include <span>
#include <functional>
#include <stop_token>
#include <unordered_map>
#include <curl/curl.h>

namespace sphaira::devoptab::common {

// max entries per devoptab, should be enough.
enum { MAX_ENTRIES = 4 };

struct BufferedDataBase : yati::source::Base {
    BufferedDataBase(const std::shared_ptr<yati::source::Base>& _source, u64 _size)
    : source{_source}
    , capacity{_size} {

    }

    virtual Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        return source->Read(buf, off, size, bytes_read);
    }

protected:
    std::shared_ptr<yati::source::Base> source;
    const u64 capacity;
};

// buffers data in 512k chunks to maximise throughput.
// not suitable if random access >= 512k is common.
// if that is needed, see the LRU cache varient used for fatfs.
struct BufferedData : BufferedDataBase {
    BufferedData(const std::shared_ptr<yati::source::Base>& _source, u64 _size, u64 _alloc = 1024 * 512)
    : BufferedDataBase{_source, _size} {
        m_data.resize(_alloc);
    }

    virtual Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    u64 m_off{};
    u64 m_size{};
    std::vector<u8> m_data{};
};

struct BufferedFileData {
    u8* data{};
    u64 off{};
    u64 size{};

    ~BufferedFileData() {
        if (data) {
            free(data);
        }
    }

    void Allocate(u64 new_size) {
        data = (u8*)realloc(data, new_size * sizeof(*data));
        off = 0;
        size = 0;
    }
};

constexpr u64 CACHE_LARGE_ALLOC_SIZE = 1024 * 512;
constexpr u64 CACHE_LARGE_SIZE = 1024 * 16;

struct LruBufferedData : BufferedDataBase {
    LruBufferedData(const std::shared_ptr<yati::source::Base>& _source, u64 _size, u32 small = 1024, u32 large = 2)
    : BufferedDataBase{_source, _size} {
        buffered_small.resize(small);
        buffered_large.resize(large);
        lru_cache[0].Init(buffered_small);
        lru_cache[1].Init(buffered_large);
    }

    virtual Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    utils::Lru<BufferedFileData> lru_cache[2]{};
    std::vector<BufferedFileData> buffered_small{}; // 1MiB (usually).
    std::vector<BufferedFileData> buffered_large{}; // 1MiB
};

bool fix_path(const char* str, char* out, bool strip_leading_slash = false);

void update_devoptab_for_read_only(devoptab_t* devoptab, bool read_only);

struct PushPullThreadData {
    static constexpr size_t MAX_BUFFER_SIZE = 1024 * 64; // 64KB max buffer

    explicit PushPullThreadData(CURL* _curl, std::stop_token token = {});
    virtual ~PushPullThreadData();

    Result CreateAndStart();
    void Cancel();
    bool IsRunning();

    // only set curl=true if called from a curl callback.
    size_t PullData(char* data, size_t total_size, bool curl = false);
    size_t PushData(const char* data, size_t total_size, bool curl = false);

    static size_t progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

private:
    static void thread_func(void* arg);

public:
    CURL* const curl{};
    std::vector<char> buffer{};
    Mutex mutex{};
    CondVar can_push{};
    CondVar can_pull{};

    long code{};
    bool error{};
    bool finished{};
    bool started{};

private:
    Thread thread{};
    const std::stop_token stop_token;
};

struct MountConfig {
    std::string name{};
    std::string url{};
    std::string user{};
    std::string pass{};
    std::string dump_path{};
    long port{};
    long timeout{};
    bool read_only{};
    bool no_stat_file{true};
    bool no_stat_dir{true};
    bool fs_hidden{};
    bool dump_hidden{};

    std::unordered_map<std::string, std::string> extra{};
};
using MountConfigs = std::vector<MountConfig>;

struct PullThreadData final : PushPullThreadData {
    using PushPullThreadData::PushPullThreadData;
    static size_t pull_thread_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
};

struct PushThreadData final : PushPullThreadData {
    using PushPullThreadData::PushPullThreadData;
    static size_t push_thread_callback(const char *ptr, size_t size, size_t nmemb, void *userdata);
};

struct MountDevice {
    MountDevice(const MountConfig& _config) : config{_config} {}
    virtual ~MountDevice() = default;

    virtual bool fix_path(const char* str, char* out, bool strip_leading_slash = false) {
        return common::fix_path(str, out, strip_leading_slash);
    }

    virtual bool Mount() = 0;
    virtual int devoptab_open(void *fileStruct, const char *path, int flags, int mode) { return -EIO; }
    virtual int devoptab_close(void *fd) { return -EIO; }
    virtual ssize_t devoptab_read(void *fd, char *ptr, size_t len) { return -EIO; }
    virtual ssize_t devoptab_write(void *fd, const char *ptr, size_t len) { return -EIO; }
    virtual ssize_t devoptab_seek(void *fd, off_t pos, int dir) { return 0; }
    virtual int devoptab_fstat(void *fd, struct stat *st) { return -EIO; }
    virtual int devoptab_unlink(const char *path) { return -EIO; }
    virtual int devoptab_rename(const char *oldName, const char *newName) { return -EIO; }
    virtual int devoptab_mkdir(const char *path, int mode) { return -EIO; }
    virtual int devoptab_rmdir(const char *path) { return -EIO; }
    virtual int devoptab_diropen(void* fd, const char *path) { return -EIO; }
    virtual int devoptab_dirreset(void* fd) { return -EIO; }
    virtual int devoptab_dirnext(void* fd, char *filename, struct stat *filestat) { return -EIO; }
    virtual int devoptab_dirclose(void* fd) { return -EIO; }
    virtual int devoptab_lstat(const char *path, struct stat *st) { return -EIO; }
    virtual int devoptab_ftruncate(void *fd, off_t len) { return -EIO; }
    virtual int devoptab_statvfs(const char *_path, struct statvfs *buf) { return -EIO; }
    virtual int devoptab_fsync(void *fd) { return -EIO; }
    virtual int devoptab_utimes(const char *_path, const struct timeval times[2]) { return -EIO; }

    const MountConfig config;
};

struct MountCurlDevice : MountDevice {
    using MountDevice::MountDevice;
    virtual ~MountCurlDevice();

    PushThreadData* CreatePushData(CURL* curl, const std::string& url, size_t offset);
    PullThreadData* CreatePullData(CURL* curl, const std::string& url, bool append = false);

    virtual bool Mount();
    virtual void curl_set_common_options(CURL* curl,  const std::string& url);
    static size_t write_memory_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t write_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t read_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
    static std::string html_decode(const std::string_view& str);
    static std::string url_decode(const std::string& str);
    std::string build_url(const std::string& path, bool is_dir);

protected:
    CURL* curl{};
    CURL* transfer_curl{};

private:
    // path extracted from the url.
    std::string m_url_path{};
    CURLU* curlu{};
    CURLSH* m_curl_share{};
    RwLock m_rwlocks[CURL_LOCK_DATA_LAST]{};
    bool m_mounted{};
};

void LoadConfigsFromIni(const fs::FsPath& path, MountConfigs& out_configs);

using CreateDeviceCallback = std::function<std::unique_ptr<MountDevice>(const MountConfig& config)>;
Result MountNetworkDevice(const CreateDeviceCallback& create_device, size_t file_size, size_t dir_size, const char* name, bool force_read_only = false);

// same as above but takes in the device and expects the mount name to be set.
bool MountNetworkDevice2(std::unique_ptr<MountDevice>&& device, const MountConfig& config, size_t file_size, size_t dir_size, const char* name, const char* mount_name);

bool MountReadOnlyIndexDevice(const CreateDeviceCallback& create_device, size_t file_size, size_t dir_size, const char* name, fs::FsPath& out_path);

} // namespace sphaira::devoptab::common

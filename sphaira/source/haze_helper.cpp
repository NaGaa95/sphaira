#include "haze_helper.hpp"

#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "evman.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <atomic>
#include <haze.h>
#include <limits>
#include <map>

namespace sphaira::libhaze {
namespace {

struct InstallSharedData {
    Mutex mutex;
    CondVar cv;
    std::string current_file;

    void* user;
    OnInstallStart on_start;
    OnInstallWrite on_write;
    OnInstallClose on_close;

    bool in_progress;
    // number of install callbacks currently executing. the callbacks block for
    // as long as an install takes, so they are *never* run whilst holding
    // mutex, this counter is what keeps them alive instead.
    u32 in_flight;
};

constexpr int THREAD_PRIO = 0x20;
constexpr int THREAD_CORE = 2;
std::atomic_bool g_should_exit = false;
bool g_is_running{false};
Mutex g_mutex{};

// kept as an atomic (rather than a field of the struct above) so that it can be
// polled by an in-flight callback without taking any lock.
std::atomic_bool g_install_enabled{false};

InstallSharedData g_shared_data{};

const char* SUPPORTED_EXT[] = {
    ".nsp", ".xci", ".nsz", ".xcz", ".msp",
};

// holds a reference to the install callbacks whilst one of them is running.
// DisableInstallMode() waits for every reference to be dropped before clearing
// the callbacks, so they stay valid for as long as the guard is alive, and the
// shared mutex stays free the whole time the (blocking) callback runs.
struct InstallCallbackGuard {
    InstallCallbackGuard() {
        SCOPED_MUTEX(&g_shared_data.mutex);
        if (!g_install_enabled) {
            return;
        }

        m_data = std::addressof(g_shared_data);
        m_data->in_flight++;
    }

    ~InstallCallbackGuard() {
        if (!m_data) {
            return;
        }

        SCOPED_MUTEX(&m_data->mutex);
        m_data->in_flight--;
        condvarWakeAll(std::addressof(m_data->cv));
    }

    InstallCallbackGuard(const InstallCallbackGuard&) = delete;
    void operator=(const InstallCallbackGuard&) = delete;

    explicit operator bool() const {
        return m_data != nullptr;
    }

private:
    InstallSharedData* m_data{};
};

bool StartInstall(std::string path) {
    InstallCallbackGuard guard;
    if (!guard) {
        return false;
    }

    OnInstallStart on_start;
    {
        SCOPED_MUTEX(&g_shared_data.mutex);
        if (g_shared_data.in_progress || !g_shared_data.current_file.empty() || !g_shared_data.on_start) {
            return false;
        }

        g_shared_data.current_file = path;
        on_start = g_shared_data.on_start;
    }

    // called without the lock held, on_start blocks until the previous install
    // has finished, which can take minutes.
    if (!on_start(path.c_str())) {
        SCOPED_MUTEX(&g_shared_data.mutex);
        g_shared_data.current_file.clear();
        return false;
    }

    SCOPED_MUTEX(&g_shared_data.mutex);
    g_shared_data.in_progress = true;
    return true;
}

struct FsProxyBase : haze::FileSystemProxyImpl {
    FsProxyBase(const char* name, const char* display_name) : m_name{name}, m_display_name{display_name} {

    }

    auto FixPath(const char* base, const char* path) const {
        fs::FsPath buf;
        const auto len = std::strlen(GetName());

        if (len && !strncasecmp(path, GetName(), len)) {
            std::snprintf(buf, sizeof(buf), "%s/%s", base, path + len);
        } else {
            std::snprintf(buf, sizeof(buf), "%s/%s", base, path);
            // std::strcpy(buf, path);
        }

        log_write("[FixPath] %s -> %s\n", path, buf.s);
        return buf;
    }

    const char* GetName() const override {
        return m_name.c_str();
    }
    const char* GetDisplayName() const override {
        return m_display_name.c_str();
    }

protected:
    const std::string m_name;
    const std::string m_display_name;
};

struct FsProxy final : FsProxyBase {
    using File = fs::File;
    using Dir = fs::Dir;
    using DirEntry = FsDirectoryEntry;

    FsProxy(std::unique_ptr<fs::Fs>&& fs, const char* name, const char* display_name)
    : FsProxyBase{name, display_name}
    , m_fs{std::forward<decltype(fs)>(fs)} {
    }

    ~FsProxy() {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            fsFsCommit(&fs->m_fs);
        }
    }

    auto FixPath(const char* path) const {
        return FsProxyBase::FixPath(m_fs->Root(), path);
    }

    // TODO: impl this for stdio
    Result GetTotalSpace(const char *path, s64 *out) override {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            return fsFsGetTotalSpace(&fs->m_fs, FixPath(path), out);
        }

        // todo: use statvfs.
        // then fallback to 256gb if not available.
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }

    Result GetFreeSpace(const char *path, s64 *out) override {
        if (m_fs->IsNative()) {
            auto fs = (fs::FsNative*)m_fs.get();
            return fsFsGetFreeSpace(&fs->m_fs, FixPath(path), out);
        }

        // todo: use statvfs.
        // then fallback to 256gb if not available.
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }

    Result GetEntryType(const char *path, haze::FileAttrType *out_entry_type) override {
        FsDirEntryType type;
        R_TRY(m_fs->GetEntryType(FixPath(path), &type));
        *out_entry_type = (type == FsDirEntryType_Dir) ? haze::FileAttrType_DIR : haze::FileAttrType_FILE;
        R_SUCCEED();
    }

    Result GetDirectoryEntry(const fs::FsPath& path, FsDirectoryEntry* out) {
        const std::string_view path_view{path.s};
        const auto slash = path_view.find_last_of('/');
        R_UNLESS(slash != std::string_view::npos && slash + 1 < path_view.size(), FsError_PathNotFound);

        const auto name = path_view.substr(slash + 1);
        const fs::FsPath parent = slash ? path_view.substr(0, slash) : std::string_view{"/"};

        fs::Dir dir;
        R_TRY(m_fs->OpenDirectory(parent, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir));

        constexpr size_t EntryCount = 16;
        FsDirectoryEntry entries[EntryCount];
        while (true) {
            s64 count{};
            R_TRY(dir.Read(&count, EntryCount, entries));
            if (!count) {
                break;
            }

            for (s64 i = 0; i < count; i++) {
                if (fs::FsPath::path_equal(name, entries[i].name)) {
                    *out = entries[i];
                    R_SUCCEED();
                }
            }
        }

        R_THROW(FsError_PathNotFound);
    }

    Result GetFileSize(const fs::FsPath& path, s64* out) {
        fs::File file;
        R_TRY(m_fs->OpenFile(path, FsOpenMode_Read, &file));
        return file.GetSize(out);
    }

    Result GetEntryAttributes(const char *path, haze::FileAttr *out) override {
        const auto fixed_path = FixPath(path);
        FsDirectoryEntry directory_entry{};
        bool has_directory_entry{};

        FsDirEntryType type;
        const auto type_result = m_fs->GetEntryType(fixed_path, &type);
        if (R_FAILED(type_result)) {
            log_write("[MTP] GetEntryType(%s) failed: 0x%X\n", fixed_path.s, type_result);
            R_UNLESS(m_fs->IsNative(), type_result);
            R_TRY(GetDirectoryEntry(fixed_path, &directory_entry));
            has_directory_entry = true;
            type = static_cast<FsDirEntryType>(directory_entry.type);
        }

        if (type == FsDirEntryType_File) {
            out->type = haze::FileAttrType_FILE;

            s64 size{};
            FsTimeStampRaw timestamp{};
            if (m_fs->IsNative()) {
                const auto timestamp_result = m_fs->GetFileTimeStampRaw(fixed_path, &timestamp);
                if (R_FAILED(timestamp_result)) {
                    log_write("[MTP] GetFileTimeStampRaw(%s) failed: 0x%X\n", fixed_path.s, timestamp_result);
                }

                const auto size_result = GetFileSize(fixed_path, &size);
                if (R_FAILED(size_result)) {
                    log_write("[MTP] GetFileSize(%s) failed: 0x%X\n", fixed_path.s, size_result);
                    if (!has_directory_entry) {
                        R_TRY(GetDirectoryEntry(fixed_path, &directory_entry));
                        has_directory_entry = true;
                    }
                    R_UNLESS(directory_entry.type == FsDirEntryType_File, FsError_PathNotFound);
                    size = directory_entry.file_size;
                }
            } else {
                R_TRY(m_fs->FileGetSizeAndTimestamp(fixed_path, &timestamp, &size));
            }

            out->size = size;
            if (timestamp.is_valid) {
                out->ctime = timestamp.created;
                out->mtime = timestamp.modified;
            }
        } else {
            out->type = haze::FileAttrType_DIR;
        }

        if (IsReadOnly()) {
            out->flag |= haze::FileAttrFlag_READ_ONLY;
        }

        R_SUCCEED();
    }

    Result CreateFile(const char* path, s64 size) override {
        log_write("[HAZE] CreateFile(%s)\n", path);
        return m_fs->CreateFile(FixPath(path), 0, 0);
    }

    Result DeleteFile(const char* path) override {
        log_write("[HAZE] DeleteFile(%s)\n", path);
        return m_fs->DeleteFile(FixPath(path));
    }

    Result RenameFile(const char *old_path, const char *new_path) override {
        log_write("[HAZE] RenameFile(%s -> %s)\n", old_path, new_path);
        return m_fs->RenameFile(FixPath(old_path), FixPath(new_path));
    }

    Result OpenFile(const char *path, haze::FileOpenMode mode, haze::File *out_file) override {
        log_write("[HAZE] OpenFile(%s)\n", path);

        u32 flags = FsOpenMode_Read;
        if (mode == haze::FileOpenMode_WRITE) {
            flags = FsOpenMode_Write | FsOpenMode_Append;
        }

        auto f = new File();
        const auto rc = m_fs->OpenFile(FixPath(path), flags, f);
        if (R_FAILED(rc)) {
            log_write("[HAZE] OpenFile(%s) failed: 0x%X\n", path, rc);
            delete f;
            return rc;
        }


        out_file->impl = f;
        R_SUCCEED();
    }

    Result GetFileSize(haze::File *file, s64 *out_size) override {
        auto f = static_cast<File*>(file->impl);
        return f->GetSize(out_size);
    }

    Result SetFileSize(haze::File *file, s64 size) override {
        auto f = static_cast<File*>(file->impl);
        return f->SetSize(size);
    }

    Result ReadFile(haze::File *file, s64 off, void *buf, u64 read_size, u64 *out_bytes_read) override {
        auto f = static_cast<File*>(file->impl);
        return f->Read(off, buf, read_size, FsReadOption_None, out_bytes_read);
    }

    Result WriteFile(haze::File *file, s64 off, const void *buf, u64 write_size) override {
        auto f = static_cast<File*>(file->impl);
        return f->Write(off, buf, write_size, FsWriteOption_None);
    }

    void CloseFile(haze::File *file) override {
        auto f = static_cast<File*>(file->impl);
        if (f) {
            delete f;
            file->impl = nullptr;
        }
    }

    Result CreateDirectory(const char* path) override {
        return m_fs->CreateDirectory(FixPath(path));
    }

    Result DeleteDirectoryRecursively(const char* path) override {
        return m_fs->DeleteDirectoryRecursively(FixPath(path));
    }

    Result RenameDirectory(const char *old_path, const char *new_path) override {
        return m_fs->RenameDirectory(FixPath(old_path), FixPath(new_path));
    }

    Result OpenDirectory(const char *path, haze::Dir *out_dir) override {
        auto dir = new Dir();
        const auto rc = m_fs->OpenDirectory(FixPath(path), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, dir);
        if (R_FAILED(rc)) {
            log_write("[HAZE] OpenDirectory(%s) failed: 0x%X\n", path, rc);
            delete dir;
            return rc;
        }

        out_dir->impl = dir;
        R_SUCCEED();
    }

    Result ReadDirectory(haze::Dir *d, s64 *out_total_entries, size_t max_entries, haze::DirEntry *buf) override {
        auto dir = static_cast<Dir*>(d->impl);

        std::vector<FsDirectoryEntry> entries(max_entries);
        R_TRY(dir->Read(out_total_entries, entries.size(), entries.data()));

        for (s64 i = 0; i < *out_total_entries; i++) {
            std::strcpy(buf[i].name, entries[i].name);
        }

        R_SUCCEED();
    }

    Result GetDirectoryEntryCount(haze::Dir *d, s64 *out_count) override {
        auto dir = static_cast<Dir*>(d->impl);
        return dir->GetEntryCount(out_count);
    }

    void CloseDirectory(haze::Dir *d) override {
        auto dir = static_cast<Dir*>(d->impl);
        if (dir) {
            delete dir;
            d->impl = nullptr;
        }
    }

private:
    std::unique_ptr<fs::Fs> m_fs{};
};

struct FsProxyVfs : FsProxyBase {
    struct Entry {
        std::string path;
        std::string parent;
        std::string name;
        haze::FileAttrType type{};
        s64 size{};
    };

    struct File {
        std::shared_ptr<Entry> entry;
        haze::FileOpenMode mode{};
        bool installing{};
    };

    struct Dir {
        std::vector<std::string> entries;
        size_t pos{};
    };

    using FsProxyBase::FsProxyBase;
    virtual ~FsProxyVfs() = default;

    Result NormalizePath(const char* path, std::string& out_path, std::string& out_key) const {
        R_UNLESS(path, FsError_PathNotFound);

        std::string_view input{path};
        while (!input.empty() && input.front() == '/') {
            input.remove_prefix(1);
        }

        const std::string_view storage{GetName()};
        if (input.size() >= storage.size() && !strncasecmp(input.data(), storage.data(), storage.size()) &&
            (input.size() == storage.size() || input[storage.size()] == '/')) {
            input.remove_prefix(storage.size());
        }
        while (!input.empty() && input.front() == '/') {
            input.remove_prefix(1);
        }

        out_path = "/";
        while (!input.empty()) {
            const auto slash = input.find('/');
            const auto component = input.substr(0, slash);
            R_UNLESS(!component.empty() && component != "." && component != ".." &&
                component.find('\\') == std::string_view::npos, FsError_PathNotFound);

            const auto separator_size = out_path.size() == 1 ? 0 : 1;
            R_UNLESS(out_path.size() + separator_size + component.size() < FS_MAX_PATH, FsError_PathNotFound);
            if (separator_size) {
                out_path.push_back('/');
            }
            out_path.append(component);

            if (slash == std::string_view::npos) {
                break;
            }
            input.remove_prefix(slash + 1);
            while (!input.empty() && input.front() == '/') {
                input.remove_prefix(1);
            }
        }

        out_key = out_path;
        std::ranges::transform(out_key, out_key.begin(), [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
        });
        R_SUCCEED();
    }

    static auto GetParent(std::string_view path) -> std::string {
        const auto slash = path.find_last_of('/');
        if (!slash) {
            return "/";
        }
        return std::string{path.substr(0, slash)};
    }

    static auto GetFileName(std::string_view path) -> std::string {
        const auto slash = path.find_last_of('/');
        return std::string{path.substr(slash + 1)};
    }

    static bool IsSameOrChild(std::string_view path, std::string_view parent) {
        return path == parent || (path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/');
    }

    auto FindEntry(const std::string& key) -> std::shared_ptr<Entry> {
        const auto it = m_entries.find(key);
        return it == m_entries.end() ? nullptr : it->second;
    }

    Result VerifyParent(const std::string& parent) {
        if (parent == "/") {
            R_SUCCEED();
        }

        const auto entry = FindEntry(parent);
        R_UNLESS(entry && entry->type == haze::FileAttrType_DIR, FsError_PathNotFound);
        R_SUCCEED();
    }

    Result CreateEntry(const char* path, s64 size, haze::FileAttrType type) {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));
        R_UNLESS(key != "/", FsError_PathAlreadyExists);

        const auto parent = GetParent(key);
        SCOPED_MUTEX(&m_entries_mutex);
        R_TRY(VerifyParent(parent));
        R_UNLESS(!m_entries.contains(key), FsError_PathAlreadyExists);

        auto entry = std::make_shared<Entry>();
        entry->path = std::move(normalized);
        entry->parent = parent;
        entry->name = GetFileName(entry->path);
        entry->type = type;
        entry->size = size;
        m_entries.emplace(std::move(key), std::move(entry));
        R_SUCCEED();
    }

    Result GetEntryType(const char *path, haze::FileAttrType *out_entry_type) override {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));

        if (key == "/") {
            *out_entry_type = haze::FileAttrType_DIR;
            R_SUCCEED();
        }

        SCOPED_MUTEX(&m_entries_mutex);
        const auto entry = FindEntry(key);
        R_UNLESS(entry, FsError_PathNotFound);
        *out_entry_type = entry->type;
        R_SUCCEED();
    }

    Result CreateFile(const char* path, s64 size) override {
        R_UNLESS(size >= 0, FsError_NotImplemented);
        return CreateEntry(path, size, haze::FileAttrType_FILE);
    }

    Result DeleteFile(const char* path) override {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));

        SCOPED_MUTEX(&m_entries_mutex);
        const auto it = m_entries.find(key);
        R_UNLESS(it != m_entries.end() && it->second->type == haze::FileAttrType_FILE, FsError_PathNotFound);
        m_entries.erase(it);
        R_SUCCEED();
    }

    Result RenameFile(const char *old_path, const char *new_path) override {
        std::string old_normalized;
        std::string old_key;
        std::string new_normalized;
        std::string new_key;
        R_TRY(NormalizePath(old_path, old_normalized, old_key));
        R_TRY(NormalizePath(new_path, new_normalized, new_key));
        R_UNLESS(new_key != "/", FsError_PathAlreadyExists);

        SCOPED_MUTEX(&m_entries_mutex);
        const auto it = m_entries.find(old_key);
        R_UNLESS(it != m_entries.end() && it->second->type == haze::FileAttrType_FILE, FsError_PathNotFound);
        R_TRY(VerifyParent(GetParent(new_key)));

        if (old_key != new_key) {
            R_UNLESS(!m_entries.contains(new_key), FsError_PathAlreadyExists);
            const auto entry = it->second;
            m_entries.erase(it);
            entry->path = std::move(new_normalized);
            entry->parent = GetParent(new_key);
            entry->name = GetFileName(entry->path);
            m_entries.emplace(std::move(new_key), entry);
        } else {
            it->second->path = std::move(new_normalized);
            it->second->name = GetFileName(it->second->path);
        }
        R_SUCCEED();
    }

    Result OpenFile(const char *path, haze::FileOpenMode mode, haze::File *out_file) override {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));

        std::shared_ptr<Entry> entry;
        {
            SCOPED_MUTEX(&m_entries_mutex);
            entry = FindEntry(key);
            R_UNLESS(entry && entry->type == haze::FileAttrType_FILE, FsError_PathNotFound);
        }

        auto f = new File();
        f->entry = std::move(entry);
        f->mode = mode;
        out_file->impl = f;
        R_SUCCEED();
    }

    Result GetFileSize(haze::File *file, s64 *out_size) override {
        auto f = static_cast<File*>(file->impl);
        R_UNLESS(f && f->entry, FsError_PathNotFound);
        SCOPED_MUTEX(&m_entries_mutex);
        *out_size = f->entry->size;
        R_SUCCEED();
    }

    Result SetFileSize(haze::File *file, s64 size) override {
        auto f = static_cast<File*>(file->impl);
        R_UNLESS(f && f->entry, FsError_PathNotFound);
        R_UNLESS(size >= 0, FsError_NotImplemented);
        SCOPED_MUTEX(&m_entries_mutex);
        f->entry->size = size;
        R_SUCCEED();
    }

    Result ReadFile(haze::File *file, s64 off, void *buf, u64 read_size, u64 *out_bytes_read) override {
        // stub for now as it may confuse users who think that the returned file is valid.
        // the code below can be used to benchmark mtp reads.
        R_THROW(FsError_NotImplemented);
    }

    Result WriteFile(haze::File *file, s64 off, const void *buf, u64 write_size) override {
        auto f = static_cast<File*>(file->impl);
        R_UNLESS(f && f->entry && off >= 0 && write_size <= static_cast<u64>(std::numeric_limits<s64>::max() - off), FsError_NotImplemented);
        SCOPED_MUTEX(&m_entries_mutex);
        f->entry->size = std::max<s64>(f->entry->size, off + write_size);
        R_SUCCEED();
    }

    void CloseFile(haze::File *file) override {
        auto f = static_cast<File*>(file->impl);
        if (f) {
            delete f;
            file->impl = nullptr;
        }
    }

    Result CreateDirectory(const char* path) override {
        return CreateEntry(path, 0, haze::FileAttrType_DIR);
    }

    Result DeleteDirectoryRecursively(const char* path) override {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));
        R_UNLESS(key != "/", FsError_NotImplemented);

        SCOPED_MUTEX(&m_entries_mutex);
        const auto root = FindEntry(key);
        R_UNLESS(root && root->type == haze::FileAttrType_DIR, FsError_PathNotFound);

        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (IsSameOrChild(it->first, key)) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
        R_SUCCEED();
    }

    Result RenameDirectory(const char *old_path, const char *new_path) override {
        std::string old_normalized;
        std::string old_key;
        std::string new_normalized;
        std::string new_key;
        R_TRY(NormalizePath(old_path, old_normalized, old_key));
        R_TRY(NormalizePath(new_path, new_normalized, new_key));
        R_UNLESS(old_key != "/" && new_key != "/", FsError_NotImplemented);
        R_UNLESS(new_key == old_key || !IsSameOrChild(new_key, old_key), FsError_NotImplemented);

        struct Move {
            std::string old_key;
            std::string new_key;
            std::string new_path;
            std::shared_ptr<Entry> entry;
        };

        SCOPED_MUTEX(&m_entries_mutex);
        const auto root = FindEntry(old_key);
        R_UNLESS(root && root->type == haze::FileAttrType_DIR, FsError_PathNotFound);
        R_TRY(VerifyParent(GetParent(new_key)));

        if (old_key != new_key) {
            R_UNLESS(!m_entries.contains(new_key), FsError_PathAlreadyExists);
        }

        std::vector<Move> moves;
        for (const auto& [key, entry] : m_entries) {
            if (IsSameOrChild(key, old_key)) {
                const auto key_suffix = std::string_view{key}.substr(old_key.size());
                const auto path_suffix = std::string_view{entry->path}.substr(root->path.size());
                R_UNLESS(new_normalized.size() + path_suffix.size() < FS_MAX_PATH, FsError_PathNotFound);
                moves.push_back({key, new_key + key_suffix, new_normalized + path_suffix, entry});
            }
        }

        for (const auto& move : moves) {
            if (const auto it = m_entries.find(move.new_key); it != m_entries.end() && !IsSameOrChild(it->first, old_key)) {
                R_THROW(FsError_PathAlreadyExists);
            }
        }

        for (const auto& move : moves) {
            m_entries.erase(move.old_key);
        }
        for (auto& move : moves) {
            move.entry->path = std::move(move.new_path);
            move.entry->parent = GetParent(move.new_key);
            move.entry->name = GetFileName(move.entry->path);
            m_entries.emplace(std::move(move.new_key), std::move(move.entry));
        }
        R_SUCCEED();
    }

    Result OpenDirectory(const char *path, haze::Dir *out_dir) override {
        std::string normalized;
        std::string key;
        R_TRY(NormalizePath(path, normalized, key));

        auto dir = new Dir();
        {
            SCOPED_MUTEX(&m_entries_mutex);
            if (key != "/") {
                const auto entry = FindEntry(key);
                if (!entry || entry->type != haze::FileAttrType_DIR) {
                    delete dir;
                    R_THROW(FsError_PathNotFound);
                }
            }

            for (const auto& [entry_key, entry] : m_entries) {
                if (entry->parent == key) {
                    dir->entries.emplace_back(entry->name);
                }
            }
        }

        std::ranges::sort(dir->entries, [](const auto& lhs, const auto& rhs) {
            return strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
        });
        out_dir->impl = dir;
        R_SUCCEED();
    }

    Result ReadDirectory(haze::Dir *d, s64 *out_total_entries, size_t max_entries, haze::DirEntry *buf) override {
        auto dir = static_cast<Dir*>(d->impl);

        R_UNLESS(dir, FsError_PathNotFound);
        const auto remaining = dir->entries.size() - std::min(dir->pos, dir->entries.size());
        max_entries = std::min(remaining, max_entries);

        for (size_t i = 0; i < max_entries; i++) {
            std::snprintf(buf[i].name, sizeof(buf[i].name), "%s", dir->entries[dir->pos + i].c_str());
        }

        dir->pos += max_entries;
        *out_total_entries = max_entries;
        R_SUCCEED();
    }

    Result GetDirectoryEntryCount(haze::Dir *d, s64 *out_count) override {
        auto dir = static_cast<Dir*>(d->impl);
        R_UNLESS(dir, FsError_PathNotFound);
        *out_count = dir->entries.size();
        R_SUCCEED();
    }

    void CloseDirectory(haze::Dir *d) override {
        auto dir = static_cast<Dir*>(d->impl);
        if (dir) {
            delete dir;
            d->impl = nullptr;
        }
    }

    void Reset() {
        SCOPED_MUTEX(&m_entries_mutex);
        m_entries.clear();
    }

protected:
    Mutex m_entries_mutex{};
    std::map<std::string, std::shared_ptr<Entry>> m_entries;
};

struct FsDevNullProxy final : FsProxyVfs {
    using FsProxyVfs::FsProxyVfs;

    Result GetTotalSpace(const char *path, s64 *out) override {
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }

    Result GetFreeSpace(const char *path, s64 *out) override {
        *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;
        R_SUCCEED();
    }
};

struct FsInstallProxy final : FsProxyVfs {
    using FsProxyVfs::FsProxyVfs;

    Result FailedIfNotEnabled() {
        if (!g_install_enabled) {
            App::Notify("Please launch MTP install menu before trying to install"_i18n);
            R_THROW(FsError_NotImplemented);
        }
        R_SUCCEED();
    }

    bool IsValidFileType(const char* name) const {
        const char* ext = std::strrchr(name, '.');
        if (!ext) {
            return false;
        }

        for (size_t i = 0; i < std::size(SUPPORTED_EXT); i++) {
            if (!strcasecmp(ext, SUPPORTED_EXT[i])) {
                return true;
            }
        }
        return false;
    }

    Result GetTotalSpace(const char *path, s64 *out) override {
        if (App::GetApp()->m_install_sd.Get()) {
            return fs::FsNativeContentStorage(FsContentStorageId_SdCard).GetTotalSpace("/", out);
        } else {
            return fs::FsNativeContentStorage(FsContentStorageId_User).GetTotalSpace("/", out);
        }
    }

    Result GetFreeSpace(const char *path, s64 *out) override {
        if (App::GetApp()->m_install_sd.Get()) {
            return fs::FsNativeContentStorage(FsContentStorageId_SdCard).GetFreeSpace("/", out);
        } else {
            return fs::FsNativeContentStorage(FsContentStorageId_User).GetFreeSpace("/", out);
        }
    }

    Result CreateDirectory(const char* path) override {
        R_TRY(FailedIfNotEnabled());
        return FsProxyVfs::CreateDirectory(path);
    }

    Result CreateFile(const char* path, s64 size) override {
        R_TRY(FailedIfNotEnabled());

        // another install is still running, the host retries after DeviceBusy.
        bool busy;
        {
            SCOPED_MUTEX(&g_shared_data.mutex);
            busy = g_shared_data.in_progress;
        }
        R_UNLESS(!busy, MAKERESULT(Module_Haze, 20)); // haze::ResultDeviceBusy

        return FsProxyVfs::CreateFile(path, size);
    }

    Result OpenFile(const char *path, haze::FileOpenMode mode, haze::File *out_file) override {
        if (mode == haze::FileOpenMode_WRITE) {
            R_TRY(FailedIfNotEnabled());
        }
        R_TRY(FsProxyVfs::OpenFile(path, mode, out_file));
        log_write("[MTP] done file open: %s mode: 0x%X\n", path, mode);

        if (mode == haze::FileOpenMode_WRITE) {
            auto f = static_cast<File*>(out_file->impl);
            f->installing = IsValidFileType(f->entry->name.c_str());
            if (f->installing) {
                auto install_path = f->entry->path;
                if (!install_path.empty() && install_path.front() == '/') {
                    install_path.erase(install_path.begin());
                }

                if (!StartInstall(std::move(install_path))) {
                    FsProxyVfs::CloseFile(out_file);
                    R_THROW(FsError_NotImplemented);
                }
            } else {
                log_write("[MTP] ignoring unsupported file in install folder: %s\n", f->entry->path.c_str());
            }
        }

        log_write("[MTP] got file: %s\n", path);
        R_SUCCEED();
    }

    Result WriteFile(haze::File *file, s64 off, const void *buf, u64 write_size) override {
        auto f = static_cast<File*>(file->impl);
        R_UNLESS(f && f->entry, FsError_PathNotFound);

        if (f->installing) {
            InstallCallbackGuard guard;
            if (!guard) {
                log_write("[MTP] failing as not enabled\n");
                R_THROW(FsError_NotImplemented);
            }

            OnInstallWrite on_write;
            {
                SCOPED_MUTEX(&g_shared_data.mutex);
                on_write = g_shared_data.on_write;
            }

            // called without the lock held, on_write blocks whilst the
            // installer catches up with the data already sent to it.
            if (!on_write || !on_write(buf, write_size)) {
                log_write("[MTP] failing as not written\n");
                R_THROW(FsError_NotImplemented);
            }
        } else if (!g_install_enabled) {
            log_write("[MTP] failing as not enabled\n");
            R_THROW(FsError_NotImplemented);
        }

        R_TRY(FsProxyVfs::WriteFile(file, off, buf, write_size));
        R_SUCCEED();
    }

    void CloseFile(haze::File *file) override {
        auto f = static_cast<File*>(file->impl);
        if (!f) {
            return;
        }

        if (f->installing) {
            log_write("[MTP] closing current file\n");

            {
                InstallCallbackGuard guard;

                OnInstallClose on_close;
                if (guard) {
                    SCOPED_MUTEX(&g_shared_data.mutex);
                    on_close = g_shared_data.on_close;
                }

                // called without the lock held. on_close returns once the file
                // has been handed to the installer, in_progress stays set until
                // FinishInstallProgress() so that the next CreateFile() is
                // answered with DeviceBusy in the meantime.
                if (on_close) {
                    on_close();
                }
            }

            SCOPED_MUTEX(&g_shared_data.mutex);
            g_shared_data.current_file.clear();
        }

        FsProxyVfs::CloseFile(file);
    }
};

std::shared_ptr<FsInstallProxy> g_install_fs{};
haze::FsEntries g_fs_entries{};

void haze_callback(const haze::CallbackData *data) {
    #if 0
    auto& e = *data;

    switch (e.type) {
        case haze::CallbackType_OpenSession: log_write("[LIBHAZE] Opening Session\n"); break;
        case haze::CallbackType_CloseSession: log_write("[LIBHAZE] Closing Session\n"); break;

        case haze::CallbackType_CreateFile: log_write("[LIBHAZE] Creating File: %s\n", e.file.filename); break;
        case haze::CallbackType_DeleteFile: log_write("[LIBHAZE] Deleting File: %s\n", e.file.filename); break;

        case haze::CallbackType_RenameFile: log_write("[LIBHAZE] Rename File: %s -> %s\n", e.rename.filename, e.rename.newname); break;
        case haze::CallbackType_RenameFolder: log_write("[LIBHAZE] Rename Folder: %s -> %s\n", e.rename.filename, e.rename.newname); break;

        case haze::CallbackType_CreateFolder: log_write("[LIBHAZE] Creating Folder: %s\n", e.file.filename); break;
        case haze::CallbackType_DeleteFolder: log_write("[LIBHAZE] Deleting Folder: %s\n", e.file.filename); break;

        case haze::CallbackType_ReadBegin: log_write("[LIBHAZE] Reading File Begin: %s \n", e.file.filename); break;
        case haze::CallbackType_ReadProgress: log_write("\t[LIBHAZE] Reading File: offset: %lld size: %lld\n", e.progress.offset, e.progress.size); break;
        case haze::CallbackType_ReadEnd: log_write("[LIBHAZE] Reading File Finished: %s\n", e.file.filename); break;

        case haze::CallbackType_WriteBegin: log_write("[LIBHAZE] Writing File Begin: %s \n", e.file.filename); break;
        case haze::CallbackType_WriteProgress: log_write("\t[LIBHAZE] Writing File: offset: %lld size: %lld\n", e.progress.offset, e.progress.size); break;
        case haze::CallbackType_WriteEnd: log_write("[LIBHAZE] Writing File Finished: %s\n", e.file.filename); break;
    }
    #endif

    if ((data->type == haze::CallbackType_OpenSession || data->type == haze::CallbackType_CloseSession) && g_install_fs) {
        g_install_fs->Reset();
    }

    App::NotifyFlashLed();
}

} // namespace

bool Init() {
    SCOPED_MUTEX(&g_mutex);
    if (g_is_running) {
        log_write("[MTP] already enabled, cannot open\n");
        return false;
    }

    // add default mount of the sd card.
    g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsNativeSd>(), "", "microSD card"));

    if (App::GetApp()->m_mtp_show_album.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsNativeImage>(FsImageDirectoryId_Sd), "Album", "Album (Image SD)"));
    }

    if (platform::SupportsContentStorageMounts && App::GetApp()->m_mtp_show_content_sd.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsNativeContentStorage>(FsContentStorageId_SdCard), "ContentsM", "Contents (microSD card)"));
    }

    if (platform::SupportsContentStorageMounts && App::GetApp()->m_mtp_show_content_system.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsNativeContentStorage>(FsContentStorageId_System), "ContentsS", "Contents (System)"));
    }

    if (platform::SupportsContentStorageMounts && App::GetApp()->m_mtp_show_content_user.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsNativeContentStorage>(FsContentStorageId_User), "ContentsU", "Contents (User)"));
    }

    if (platform::SupportsGames && App::GetApp()->m_mtp_show_games.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsStdio>(true, "games:/"), "Games", "Games"));
    }

    if (platform::SupportsInstall && App::GetApp()->m_mtp_show_install.Get()) {
        g_install_fs = std::make_shared<FsInstallProxy>("install", "Install (NSP, XCI, NSZ, XCZ, MSP)");
        g_fs_entries.emplace_back(g_install_fs);
    }

    if (App::GetApp()->m_mtp_show_mounts.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsProxy>(std::make_unique<fs::FsStdio>(true, "mounts:/"), "Mounts", "Mounts"));
    }

    if (App::GetApp()->m_mtp_show_speedtest.Get()) {
        g_fs_entries.emplace_back(std::make_shared<FsDevNullProxy>("DevNull", "DevNull (Speed Test)"));
    }

    g_should_exit = false;
    if (!haze::Initialize(haze_callback, g_fs_entries, App::GetApp()->m_mtp_vid.Get(), App::GetApp()->m_mtp_pid.Get(), "Sphaira/" APP_VERSION)) {
        return false;
    }

    log_write("[MTP] started\n");
    return g_is_running = true;
}

bool IsInit() {
    SCOPED_MUTEX(&g_mutex);
    return g_is_running;
}

void Exit() {
    // done before taking g_mutex, and before haze::Exit() joins the haze
    // thread, as that thread may still be sat inside a blocking install
    // callback. DisableInstallMode() is what releases it.
    DisableInstallMode();

    SCOPED_MUTEX(&g_mutex);
    if (!g_is_running) {
        return;
    }

    haze::Exit();
    g_is_running = false;
    g_should_exit = true;
    g_fs_entries.clear();
    g_install_fs.reset();

    log_write("[MTP] exitied\n");
}

void InitInstallMode(const OnInstallStart& on_start, const OnInstallWrite& on_write, const OnInstallClose& on_close) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    condvarInit(std::addressof(g_shared_data.cv));
    g_shared_data.on_start = on_start;
    g_shared_data.on_write = on_write;
    g_shared_data.on_close = on_close;
    g_shared_data.in_progress = false;
    g_shared_data.current_file.clear();
    g_install_enabled = true;
}

bool IsInstallModeEnabled() {
    return g_install_enabled;
}

void DisableInstallMode() {
    // published first so that any callback which is currently blocked can see
    // it and return, otherwise the wait below would never finish.
    g_install_enabled = false;

    SCOPED_MUTEX(&g_shared_data.mutex);
    while (g_shared_data.in_flight) {
        condvarWaitTimeout(std::addressof(g_shared_data.cv), std::addressof(g_shared_data.mutex), 1e+8);
    }

    // safe to drop the callbacks now, nothing can be using them.
    g_shared_data.on_start = {};
    g_shared_data.on_write = {};
    g_shared_data.on_close = {};
    g_shared_data.in_progress = false;
    g_shared_data.current_file.clear();
}

void FinishInstallProgress() {
    // called by the installer thread, the next CreateFile() is accepted again.
    SCOPED_MUTEX(&g_shared_data.mutex);
    g_shared_data.in_progress = false;
}

} // namespace sphaira::libhaze

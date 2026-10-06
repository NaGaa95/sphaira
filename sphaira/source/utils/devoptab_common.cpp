#include "utils/devoptab_common.hpp"
#include "utils/thread.hpp"

#include "defines.hpp"
#include "log.hpp"
#include "download.hpp"

#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <minIni.h>
#include <curl/curl.h>

namespace sphaira::devoptab::common {
namespace {

RwLock g_rwlock{};

// curl_url_strerror doesn't exist in the switch version of libcurl as its so old.
// todo: update libcurl and send patches to dkp.
const char* curl_url_strerror_wrap(CURLUcode code) {
    switch (code) {
        case CURLUE_OK: return "No error";
        case CURLUE_BAD_HANDLE: return "Invalid handle";
        case CURLUE_BAD_PARTPOINTER: return "Invalid pointer to a part of the URL";
        case CURLUE_MALFORMED_INPUT: return "Malformed input";
        case CURLUE_BAD_PORT_NUMBER: return "Invalid port number";
        case CURLUE_UNSUPPORTED_SCHEME: return "Unsupported scheme";
        case CURLUE_URLDECODE: return "Failed to decode URL component";
        case CURLUE_OUT_OF_MEMORY: return "Out of memory";
        case CURLUE_USER_NOT_ALLOWED: return "User not allowed in URL";
        case CURLUE_UNKNOWN_PART: return "Unknown URL part";
        case CURLUE_NO_SCHEME: return "No scheme found in URL";
        case CURLUE_NO_USER: return "No user found in URL";
        case CURLUE_NO_PASSWORD: return "No password found in URL";
        case CURLUE_NO_OPTIONS: return "No options found in URL";
        case CURLUE_NO_HOST: return "No host found in URL";
        case CURLUE_NO_PORT: return "No port number found in URL";
        case CURLUE_NO_QUERY: return "No query found in URL";
        case CURLUE_NO_FRAGMENT: return "No fragment found in URL";
        default: return "Unknown error code";
    }
}

struct Device {
    std::unique_ptr<MountDevice> mount_device;
    size_t file_size;
    size_t dir_size;

    MountConfig config{};
    Mutex mutex{};
};

struct File {
    Device* device;
    void* fd;
};

struct Dir {
    Device* device;
    void* fd;
};

int set_errno(struct _reent *r, int err) {
    r->_errno = err;
    return -1;
}

int devoptab_open(struct _reent *r, void *fileStruct, const char *_path, int flags, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    auto file = static_cast<File*>(fileStruct);
    std::memset(file, 0, sizeof(*file));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only && (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    file->fd = calloc(1, device->file_size);
    if (!file->fd) {
        return set_errno(r, ENOMEM);
    }

    const auto ret = device->mount_device->devoptab_open(file->fd, path, flags, mode);
    if (ret) {
        free(file->fd);
        file->fd = nullptr;
        return set_errno(r, -ret);
    }

    file->device = device;
    return r->_errno = 0;
}

int devoptab_close(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&file->device->mutex);

    if (file->fd) {
        file->device->mount_device->devoptab_close(file->fd);
        free(file->fd);
    }

    std::memset(file, 0, sizeof(*file));
    return r->_errno = 0;
}

ssize_t devoptab_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = file->device->mount_device->devoptab_read(file->fd, ptr, len);
    if (ret < 0) {
        return set_errno(r, -ret);
    }

    return ret;
}

ssize_t devoptab_write(struct _reent *r, void *fd, const char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = file->device->mount_device->devoptab_write(file->fd, ptr, len);
    if (ret < 0) {
        return set_errno(r, -ret);
    }

    return ret;
}

off_t devoptab_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = file->device->mount_device->devoptab_seek(file->fd, pos, dir);
    if (ret < 0) {
        set_errno(r, -ret);
        return 0;
    }

    r->_errno = 0;
    return ret;
}

int devoptab_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    std::memset(st, 0, sizeof(*st));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&file->device->mutex);

    const auto ret = file->device->mount_device->devoptab_fstat(file->fd, st);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_unlink(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_unlink(path);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rename(struct _reent *r, const char *_oldName, const char *_newName) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only) {
        return set_errno(r, EROFS);
    }

    char oldName[PATH_MAX]{};
    if (!device->mount_device->fix_path(_oldName, oldName)) {
        return set_errno(r, ENOENT);
    }

    char newName[PATH_MAX]{};
    if (!device->mount_device->fix_path(_newName, newName)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_rename(oldName, newName);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_mkdir(struct _reent *r, const char *_path, int mode) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_mkdir(path, mode);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_rmdir(struct _reent *r, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (device->config.read_only) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_rmdir(path);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

DIR_ITER* devoptab_diropen(struct _reent *r, DIR_ITER *dirState, const char *_path) {
    auto device = static_cast<Device*>(r->deviceData);
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(dir, 0, sizeof(*dir));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    log_write("[DEVOPTAB] diropen %s\n", _path);

    if (!device->mount_device) {
        log_write("[DEVOPTAB] diropen no mount device\n");
        set_errno(r, ENOENT);
        return nullptr;
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        set_errno(r, ENOENT);
        return nullptr;
    }

    log_write("[DEVOPTAB] diropen fixed path %s\n", path);

    if (!device->mount_device->Mount()) {
        set_errno(r, EIO);
        return nullptr;
    }

    log_write("[DEVOPTAB] diropen mounted\n");

    dir->fd = calloc(1, device->dir_size);
    if (!dir->fd) {
        set_errno(r, ENOMEM);
        return nullptr;
    }

    log_write("[DEVOPTAB] diropen allocated dir\n");

    const auto ret = device->mount_device->devoptab_diropen(dir->fd, path);
    if (ret) {
        free(dir->fd);
        dir->fd = nullptr;
        set_errno(r, -ret);
        return nullptr;
    }

    log_write("[DEVOPTAB] diropen opened dir\n");

    dir->device = device;
    return dirState;
}

int devoptab_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&dir->device->mutex);

    const auto ret = dir->device->mount_device->devoptab_dirreset(dir->fd);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    std::memset(filestat, 0, sizeof(*filestat));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&dir->device->mutex);

    const auto ret = dir->device->mount_device->devoptab_dirnext(dir->fd, filename, filestat);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto dir = static_cast<Dir*>(dirState->dirStruct);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&dir->device->mutex);

    if (dir->fd) {
        dir->device->mount_device->devoptab_dirclose(dir->fd);
        free(dir->fd);
    }

    std::memset(dir, 0, sizeof(*dir));
    return r->_errno = 0;
}

int devoptab_lstat(struct _reent *r, const char *_path, struct stat *st) {
    auto device = static_cast<Device*>(r->deviceData);
    std::memset(st, 0, sizeof(*st));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    // special case: root of the device.
    const auto dilem = std::strchr(_path, ':');
    if (dilem && (dilem > _path) && (dilem[1] == '\0' || (dilem[1] == '/' && dilem[2] == '\0'))) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_nlink = 1;
        return r->_errno = 0;
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_lstat(path, st);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_ftruncate(struct _reent *r, void *fd, off_t len) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    if (!file || !file->fd) {
        return set_errno(r, EBADF);
    }

    if (file->device->config.read_only) {
        return set_errno(r, EROFS);
    }

    const auto ret = file->device->mount_device->devoptab_ftruncate(file->fd, len);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_statvfs(struct _reent *r, const char *_path, struct statvfs *buf) {
    auto device = static_cast<Device*>(r->deviceData);
    std::memset(buf, 0, sizeof(*buf));
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_statvfs(path, buf);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_fsync(struct _reent *r, void *fd) {
    auto file = static_cast<File*>(fd);
    SCOPED_MUTEX(&file->device->mutex);

    if (!file || !file->fd) {
        return set_errno(r, EBADF);
    }

    if (file->device->config.read_only) {
        return set_errno(r, EROFS);
    }

    const auto ret = file->device->mount_device->devoptab_fsync(file->fd);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

int devoptab_utimes(struct _reent *r, const char *_path, const struct timeval times[2]) {
    auto device = static_cast<Device*>(r->deviceData);
    SCOPED_RWLOCK(&g_rwlock, false);
    SCOPED_MUTEX(&device->mutex);

    if (!times) {
        log_write("[DEVOPTAB] devoptab_utimes() times is null\n");
        return set_errno(r, EINVAL);
    }

    if (device->config.read_only) {
        return set_errno(r, EROFS);
    }

    char path[PATH_MAX]{};
    if (!device->mount_device->fix_path(_path, path)) {
        return set_errno(r, ENOENT);
    }

    if (!device->mount_device->Mount()) {
        return set_errno(r, EIO);
    }

    const auto ret = device->mount_device->devoptab_utimes(path, times);
    if (ret) {
        return set_errno(r, -ret);
    }

    return r->_errno = 0;
}

constexpr devoptab_t DEVOPTAB = {
    .structSize   = sizeof(File),
    .open_r       = devoptab_open,
    .close_r      = devoptab_close,
    .write_r      = devoptab_write,
    .read_r       = devoptab_read,
    .seek_r       = devoptab_seek,
    .fstat_r      = devoptab_fstat,
    .stat_r       = devoptab_lstat,
    .unlink_r     = devoptab_unlink,
    .rename_r     = devoptab_rename,
    .mkdir_r      = devoptab_mkdir,
    .dirStateSize = sizeof(Dir),
    .diropen_r    = devoptab_diropen,
    .dirreset_r   = devoptab_dirreset,
    .dirnext_r    = devoptab_dirnext,
    .dirclose_r   = devoptab_dirclose,
    .statvfs_r    = devoptab_statvfs,
    .ftruncate_r  = devoptab_ftruncate,
    .fsync_r      = devoptab_fsync,
    .rmdir_r      = devoptab_rmdir,
    .lstat_r      = devoptab_lstat,
    .utimes_r     = devoptab_utimes,
};

struct Entry {
    Device device{};
    devoptab_t devoptab{};
    fs::FsPath mount{};
    char name[32]{};
    s32 ref_count{};

    ~Entry() {
        RemoveDevice(mount);
    }
};

std::array<std::unique_ptr<Entry>, 16> g_entries;

} // namespace

// todo: change above function to handle bytes read instead.
Result BufferedData::Read(void *_buffer, s64 file_off, s64 read_size, u64* bytes_read) {
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;
    *bytes_read = 0;

    R_UNLESS(file_off < capacity, FsError_UnsupportedOperateRangeForFileStorage);
    read_size = std::min<s64>(read_size, capacity - file_off);

    if (m_size) {
        // check if we can read this data into the beginning of dst.
        if (file_off < m_off + m_size && file_off >= m_off) {
            const auto off = file_off - m_off;
            const auto size = std::min<s64>(read_size, m_size - off);
            if (size) {
                std::memcpy(dst, m_data.data() + off, size);

                read_size -= size;
                file_off += size;
                amount += size;
                dst += size;
            }
        }
    }

    if (read_size) {
        const auto alloc_size = std::min<s64>(m_data.size(), capacity - file_off);
        m_off = 0;
        m_size = 0;
        u64 bytes_read;

        // if the dst is big enough, read data in place.
        if (read_size > alloc_size) {
            R_TRY(source->Read(dst, file_off, read_size, &bytes_read));

            read_size -= bytes_read;
            file_off += bytes_read;
            amount += bytes_read;
            dst += bytes_read;

            // save the last chunk of data to the m_buffered io.
            const auto max_advance = std::min<u64>(amount, alloc_size);
            m_off = file_off - max_advance;
            m_size = max_advance;
            std::memcpy(m_data.data(), dst - max_advance, max_advance);
        } else {
            R_TRY(source->Read(m_data.data(), file_off, alloc_size, &bytes_read));
			const auto max_advance = std::min<u64>(read_size, bytes_read);
            std::memcpy(dst, m_data.data(), max_advance);

            m_off = file_off;
            m_size = bytes_read;

            read_size -= max_advance;
            file_off += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

    *bytes_read = amount;
    R_SUCCEED();
}

Result LruBufferedData::Read(void *_buffer, s64 file_off, s64 read_size, u64* bytes_read) {
    // log_write("[FATFS] read offset: %zu size: %zu\n", file_off, read_size);
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;
    *bytes_read = 0;

    R_UNLESS(file_off < capacity, FsError_UnsupportedOperateRangeForFileStorage);
    read_size = std::min<s64>(read_size, capacity - file_off);

    // fatfs reads in max 16k chunks.
    // knowing this, it's possible to detect large file reads by simply checking if
    // the read size is 16k (or more, maybe in the further).
    // however this would destroy random access performance, such as fetching 512 bytes.
    // the fix was to have 2 LRU caches, one for large data and the other for small (anything below 16k).
    // the results in file reads 32MB -> 184MB and directory listing is instant.
    const auto large_read = read_size >= 1024 * 16;
    auto& lru = large_read ? lru_cache[1] : lru_cache[0];

    for (auto list = lru.begin(); list; list = list->next) {
        const auto& m_buffered = list->data;
        if (m_buffered->size) {
            // check if we can read this data into the beginning of dst.
            if (file_off < m_buffered->off + m_buffered->size && file_off >= m_buffered->off) {
                const auto off = file_off - m_buffered->off;
                const auto size = std::min<s64>(read_size, m_buffered->size - off);
                if (size) {
                    // log_write("[FAT] cache HIT at: %zu\n", file_off);
                    std::memcpy(dst, m_buffered->data + off, size);

                    read_size -= size;
                    file_off += size;
                    amount += size;
                    dst += size;

                    lru.Update(list);
                    break;
                }
            }
        }
    }

    if (read_size) {
        // log_write("[FAT] cache miss at: %zu %zu\n", file_off, read_size);

        auto alloc_size = large_read ? CACHE_LARGE_ALLOC_SIZE : std::max<u64>(read_size, 512 * 24);
        alloc_size = std::min<s64>(alloc_size, capacity - file_off);
        u64 bytes_read;

        auto m_buffered = lru.GetNextFree();
        m_buffered->Allocate(alloc_size);

        // if the dst is big enough, read data in place.
        if (read_size > alloc_size) {
            R_TRY(source->Read(dst, file_off, read_size, &bytes_read));
            // R_TRY(fsStorageRead(storage, file_off, dst, read_size));
            read_size -= bytes_read;
            file_off += bytes_read;
            amount += bytes_read;
            dst += bytes_read;

            // save the last chunk of data to the m_buffered io.
            const auto max_advance = std::min<u64>(amount, alloc_size);
            m_buffered->off = file_off - max_advance;
            m_buffered->size = max_advance;
            std::memcpy(m_buffered->data, dst - max_advance, max_advance);
        } else {
            R_TRY(source->Read(m_buffered->data, file_off, alloc_size, &bytes_read));
            // R_TRY(fsStorageRead(storage, file_off, m_buffered->data, alloc_size));
			const auto max_advance = std::min<u64>(read_size, bytes_read);
            std::memcpy(dst, m_buffered->data, max_advance);

            m_buffered->off = file_off;
            m_buffered->size = bytes_read;

            read_size -= max_advance;
            file_off += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

    *bytes_read = amount;
    R_SUCCEED();
}

bool fix_path(const char* str, char* out, bool strip_leading_slash) {
    str = std::strchr(str, ':');
    if (!str) {
        return false;
    }

    // skip over ':'
    str++;
    size_t len = 0;

    // todo: hanle utf8 paths.
    for (size_t i = 0; str[i]; i++) {
        // skip multiple slashes.
        if (i && str[i] == '/' && str[i - 1] == '/') {
            continue;
        }

        if (!i) {
            // skip leading slash.
            if (strip_leading_slash && str[i] == '/') {
                continue;
            }

            // add leading slash.
            if (!strip_leading_slash && str[i] != '/') {
                out[len++] = '/';
            }
        }

        // save single char.
        out[len++] = str[i];
    }

    // skip trailing slash.
    if (len > 1 && out[len - 1] == '/') {
        out[len - 1] = '\0';
    }

    // null the end.
    out[len] = '\0';

    return true;
}

void update_devoptab_for_read_only(devoptab_t* devoptab, bool read_only) {
    // remove write functions if read_only is set.
    if (read_only) {
        devoptab->write_r = nullptr;
        devoptab->link_r = nullptr;
        devoptab->unlink_r = nullptr;
        devoptab->rename_r = nullptr;
        devoptab->mkdir_r = nullptr;
        devoptab->ftruncate_r = nullptr;
        devoptab->fsync_r = nullptr;
        devoptab->rmdir_r = nullptr;
        devoptab->utimes_r = nullptr;
        devoptab->symlink_r = nullptr;
    }
}

void LoadConfigsFromIni(const fs::FsPath& path, MountConfigs& out_configs) {
    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto e = static_cast<MountConfigs*>(UserData);
        if (!Section || !Key || !Value) {
            return 1;
        }

        // add new entry if use section changed.
        if (e->empty() || std::strcmp(Section, e->back().name.c_str())) {
            e->emplace_back(Section);
        }

        if (!std::strcmp(Key, "url")) {
            e->back().url = Value;
        } else if (!std::strcmp(Key, "user")) {
            e->back().user = Value;
        } else if (!std::strcmp(Key, "pass")) {
            e->back().pass = Value;
        } else if (!std::strcmp(Key, "dump_path")) {
            e->back().dump_path = Value;
        } else if (!std::strcmp(Key, "port")) {
            const auto port = ini_parse_getl(Value, -1);
            if (port < 0 || port > 65535) {
                log_write("[DEVOPTAB] INI: invalid port %s\n", Value);
            } else {
                e->back().port = port;
            }
        } else if (!std::strcmp(Key, "timeout")) {
            e->back().timeout = ini_parse_getl(Value, e->back().timeout);
        } else if (!std::strcmp(Key, "read_only")) {
            e->back().read_only = ini_parse_getbool(Value, e->back().read_only);
        } else if (!std::strcmp(Key, "no_stat_file")) {
            e->back().no_stat_file = ini_parse_getbool(Value, e->back().no_stat_file);
        } else if (!std::strcmp(Key, "no_stat_dir")) {
            e->back().no_stat_dir = ini_parse_getbool(Value, e->back().no_stat_dir);
        } else if (!std::strcmp(Key, "fs_hidden")) {
            e->back().fs_hidden = ini_parse_getbool(Value, e->back().fs_hidden);
        } else if (!std::strcmp(Key, "dump_hidden")) {
            e->back().dump_hidden = ini_parse_getbool(Value, e->back().dump_hidden);
        } else {
            log_write("[DEVOPTAB] INI: extra key %s=%s\n", Key, Value);
            e->back().extra.emplace(Key, Value);
        }

        return 1;
    };

    out_configs.resize(0);
    ini_browse(cb, &out_configs, path);
    log_write("[DEVOPTAB] Found %zu mount configs\n", out_configs.size());
}

bool MountNetworkDevice2(std::unique_ptr<MountDevice>&& device, const MountConfig& config, size_t file_size, size_t dir_size, const char* name, const char* mount_name) {
    if (!device) {
        log_write("[DEVOPTAB] No device for %s\n", mount_name);
        return false;
    }

    bool already_mounted = false;
    for (const auto& entry : g_entries) {
        if (entry && entry->mount == mount_name) {
            already_mounted = true;
            break;
        }
    }

    if (already_mounted) {
        log_write("[DEVOPTAB] Already mounted %s, skipping\n", mount_name);
        return false;
    }

    // otherwise, find next free entry.
    auto itr = std::ranges::find_if(g_entries, [](auto& e){
        return !e;
    });

    if (itr == g_entries.end()) {
        log_write("[DEVOPTAB] No free entries to mount %s\n", mount_name);
        return false;
    }

    auto entry = std::make_unique<Entry>();
    entry->device.mount_device = std::forward<decltype(device)>(device);
    entry->device.file_size = file_size;
    entry->device.dir_size = dir_size;
    entry->device.config = config;

    if (!entry->device.mount_device) {
        log_write("[DEVOPTAB] Failed to create device for %s\n", config.url.c_str());
        return false;
    }

    entry->devoptab = DEVOPTAB;
    entry->devoptab.name = entry->name;
    entry->devoptab.deviceData = &entry->device;
    std::snprintf(entry->name, sizeof(entry->name), "%s", name);
    std::snprintf(entry->mount, sizeof(entry->mount), "%s", mount_name);
    common::update_devoptab_for_read_only(&entry->devoptab, config.read_only);

    if (AddDevice(&entry->devoptab) < 0) {
        log_write("[DEVOPTAB] Failed to add device %s\n", mount_name);
        return false;
    }

    log_write("[DEVOPTAB] DEVICE SUCCESS %s %s\n", name, mount_name);

    entry->ref_count++;
    *itr = std::move(entry);
    log_write("[DEVOPTAB] Mounted %s at /%s\n", name, mount_name);

    return true;
}

bool MountReadOnlyIndexDevice(const CreateDeviceCallback& create_device, size_t file_size, size_t dir_size, const char* name, fs::FsPath& out_path) {
    static Mutex mutex{};
    static u32 next_index{};
    SCOPED_MUTEX(&mutex);

    MountConfig config{};
    config.read_only = true;
    config.no_stat_dir = false;
    config.no_stat_file = false;
    config.fs_hidden = true;
    config.dump_hidden = true;

    const auto index = next_index;
    next_index = (next_index + 1) % 30;

    fs::FsPath _name{};
    std::snprintf(_name, sizeof(_name), "%s_%u", name, index);

    fs::FsPath _mount{};
    std::snprintf(_mount, sizeof(_mount), "%s_%u:/", name, index);

    if (!common::MountNetworkDevice2(
        create_device(config),
        config, file_size, dir_size,
        _name, _mount
    )) {
        return false;
    }

    out_path = _mount;
    return true;
}

Result MountNetworkDevice(const CreateDeviceCallback& create_device, size_t file_size, size_t dir_size, const char* name, bool force_read_only) {
    {
        static Mutex rw_lock_init_mutex{};
        SCOPED_MUTEX(&rw_lock_init_mutex);

        static bool rwlock_init{};
        if (!rwlock_init) {
            rwlockInit(&g_rwlock);
            rwlock_init = true;
        }
    }

    SCOPED_RWLOCK(&g_rwlock, true);

    fs::FsPath config_path{};
    std::snprintf(config_path, sizeof(config_path), "/config/sphaira/mount/%s.ini", name);

    MountConfigs configs{};
    LoadConfigsFromIni(config_path, configs);

    for (auto& config : configs) {
        if (config.name.empty()) {
            log_write("[DEVOPTAB] Skipping empty name\n");
            continue;
        }

        if (config.url.empty()) {
            log_write("[DEVOPTAB] Skipping empty url for %s\n", config.name.c_str());
            continue;
        }

        if (force_read_only) {
            config.read_only = true;
        }

        fs::FsPath _name{};
        std::snprintf(_name, sizeof(_name), "[%s] %s", name, config.name.c_str());

        fs::FsPath _mount{};
        std::snprintf(_mount, sizeof(_mount), "[%s] %s:/", name, config.name.c_str());

        if (!MountNetworkDevice2(create_device(config), config, file_size, dir_size, _name, _mount)) {
            log_write("[DEVOPTAB] Failed to mount %s\n", config.name.c_str());
            continue;
        }
    }

    R_SUCCEED();
}

PushPullThreadData::PushPullThreadData(CURL* _curl, std::stop_token token) : curl{_curl}, stop_token{token} {
    mutexInit(&mutex);
    condvarInit(&can_push);
    condvarInit(&can_pull);
}

PushPullThreadData::~PushPullThreadData() {
    log_write("[PUSH:PULL] Destructor\n");
    Cancel();

    if (started) {
        log_write("[PUSH:PULL] Waiting for thread to exit\n");
        threadWaitForExit(&thread);
        log_write("[PUSH:PULL] Thread exited\n");
    }

    threadClose(&thread);
}

Result PushPullThreadData::CreateAndStart() {
    SCOPED_MUTEX(&mutex);

    if (started) {
        R_SUCCEED();
    }

    R_TRY(utils::CreateThread(&thread, thread_func, this));
    R_TRY(threadStart(&thread));

    started = true;
    R_SUCCEED();
}

void PushPullThreadData::Cancel() {
    SCOPED_MUTEX(&mutex);
    finished = true;
    condvarWakeOne(&can_pull);
    condvarWakeOne(&can_push);
}

bool PushPullThreadData::IsRunning() {
    SCOPED_MUTEX(&mutex);
    return !finished && !error;
}

size_t PushPullThreadData::PullData(char* data, size_t total_size, bool curl) {
    if (!data || !total_size) {
        return 0;
    }

    SCOPED_MUTEX(&mutex);
    ON_SCOPE_EXIT(condvarWakeOne(&can_push));

    if (curl) {
        // this should be handled in the progress function.
        // however i handle it here as well just in case.
        if (buffer.empty()) {
            if (finished) {
                log_write("[PUSH:PULL] PullData: finished and no data\n");
                return 0;
            }

            return CURL_READFUNC_PAUSE;
        }

        // read what we can.
        const auto rsize = std::min(total_size, buffer.size());
        std::memcpy(data, buffer.data(), rsize);
        buffer.erase(buffer.begin(), buffer.begin() + rsize);
        return rsize;
    } else {
        // if we are not in a curl callback, then we can block until we have data.
        size_t bytes_read = 0;
        while (bytes_read < total_size && !error) {
            if (buffer.empty()) {
                if (finished) {
                    break;
                }

                condvarWakeOne(&can_push);
                condvarWait(&can_pull, &mutex);
                continue;
            }

            const auto rsize = std::min(total_size - bytes_read, buffer.size());
            std::memcpy(data + bytes_read, buffer.data(), rsize);
            buffer.erase(buffer.begin(), buffer.begin() + rsize);
            bytes_read += rsize;
        }

        return bytes_read;
    }
}

size_t PushPullThreadData::PushData(const char* data, size_t total_size, bool curl) {
    if (!data || !total_size) {
        return 0;
    }

    SCOPED_MUTEX(&mutex);
    ON_SCOPE_EXIT(condvarWakeOne(&can_pull));

    if (curl) {
        // this should be handled in the progress function.
        // however i handle it here as well just in case.
        if (buffer.size() + total_size > MAX_BUFFER_SIZE) {
            return CURL_WRITEFUNC_PAUSE;
        }

        // blocking / pausing is handled in the progress function.
        // do NOT block here as curl does not like it and it will deadlock.
        // the mutex block above is fine as it only blocks to perform a memcpy.
        buffer.insert(buffer.end(), data, data + total_size);
        return total_size;
    } else {
        // if we are not in a curl callback, then we can block until we have space.
        size_t bytes_written = 0;
        while (bytes_written < total_size && !error && !finished) {
            const size_t space_left = MAX_BUFFER_SIZE - buffer.size();
            if (space_left == 0) {
                condvarWakeOne(&can_pull);
                condvarWait(&can_push, &mutex);
                continue;
            }

            const auto wsize = std::min(total_size - bytes_written, space_left);
            buffer.insert(buffer.end(), data + bytes_written, data + bytes_written + wsize);
            bytes_written += wsize;
        }

        return bytes_written;
    }
}

size_t PushThreadData::push_thread_callback(const char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!ptr || !userdata || !size || !nmemb) {
        return 0;
    }

    auto* data = static_cast<PushThreadData*>(userdata);
    return data->PushData(ptr, size * nmemb, true);
}

size_t PullThreadData::pull_thread_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!ptr || !userdata || !size || !nmemb) {
        return 0;
    }

    auto* data = static_cast<PullThreadData*>(userdata);
    return data->PullData(ptr, size * nmemb, true);
}

size_t PushPullThreadData::progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto *data = static_cast<PushPullThreadData*>(clientp);
    bool should_pause;

    {
        SCOPED_MUTEX(&data->mutex);

        // abort before handling a paused transfer.
        if (data->error || data->stop_token.stop_requested()) {
            log_write("[PUSH:PULL] progress_callback: aborting transfer\n");
            return 1;
        }

        // nothing yet.
        if (!dlnow && !ulnow) {
            return 0;
        }

        // workout if this is a download or upload.
        const auto is_download = dlnow > 0;

        if (is_download) {
            // no more data wanted, usually this is handled by curl using ranges.
            // however, if we did a seek, then we want to cancel early.
            if (data->finished) {
                log_write("[PUSH:PULL] progress_callback: cancelling download, finished set\n");
                return 1;
            }

            // pause if the buffer is full, otherwise continue.
            should_pause = data->buffer.size() >= MAX_BUFFER_SIZE;
        } else {
            // pause if we have no data to send, otherwise continue.
            // do not pause if finished as curl may have internal data pending to send.
            should_pause = !data->finished && data->buffer.empty();
        }
    }

    // curl_easy_pause(CONT) actually calls the read/write callback again immediately.
    // so we need to make sure we are not holding the mutex when calling it.
    // the curl handle is owned by this thread so no need to lock it.
    const auto res = curl_easy_pause(data->curl, should_pause ? CURLPAUSE_ALL : CURLPAUSE_CONT);
    if (res != CURLE_OK) {
        log_write("[PUSH:PULL] progress_callback: curl_easy_pause(%d) failed: %s\n", should_pause, curl_easy_strerror(res));
    }

    return 0;
}

void PushPullThreadData::thread_func(void* arg) {
    log_write("[PUSH:PULL] Read thread started\n");
    auto data = static_cast<PushPullThreadData*>(arg);

    curl_easy_setopt(data->curl, CURLOPT_XFERINFODATA, data);
    curl_easy_setopt(data->curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    const auto res = curl_easy_perform(data->curl);

    log_write("[PUSH:PULL] curl_easy_perform() returned: %s\n", curl_easy_strerror(res));

    // when finished, lock mutex and signal for anything waiting.
    SCOPED_MUTEX(&data->mutex);
    condvarWakeOne(&data->can_push);
    condvarWakeOne(&data->can_pull);

    data->finished = true;
    data->error = res != CURLE_OK;
    curl_easy_getinfo(data->curl, CURLINFO_RESPONSE_CODE, &data->code);

    log_write("[PUSH:PULL] Read thread finished, code: %ld, error: %d\n", data->code, data->error);
}

MountCurlDevice::~MountCurlDevice() {
    log_write("[CURL] Cleaning up mount device\n");
    if (curlu) {
        curl_url_cleanup(curlu);
    }

    if (curl) {
        curl_easy_cleanup(curl);
    }

    if (transfer_curl) {
        curl_easy_cleanup(transfer_curl);
    }

    if (m_curl_share) {
        curl_share_cleanup(m_curl_share);
    }
    log_write("[CURL] Cleaned up mount device\n");
}

bool MountCurlDevice::Mount() {
    if (m_mounted) {
        return true;
    }

    if (!curl) {
        curl = curl_easy_init();
        if (!curl) {
            log_write("[CURL] curl_easy_init() failed\n");
            return false;
        }
    }

    if (!transfer_curl) {
        transfer_curl = curl_easy_init();
        if (!transfer_curl) {
            log_write("[CURL] transfer curl_easy_init() failed\n");
            return false;
        }
    }

    // setup url, only the path is updated at runtime.
    if (!curlu) {
        curlu = curl_url();
        if (!curlu) {
            log_write("[CURL] curl_url() failed\n");
            return false;
        }

        auto url = config.url;
        if (url.starts_with("webdav://") || url.starts_with("webdavs://")) {
            log_write("[CURL] updating host: %s\n", url.c_str());
            url.replace(0, std::strlen("webdav"), "http");
            log_write("[CURL] updated host: %s\n", url.c_str());
        }

        // if (url.starts_with("sftp://")) {
        //     log_write("[CURL] updating host: %s\n", url.c_str());
        //     url.replace(0, std::strlen("sftp"), ""); // what should this be?
        //     log_write("[CURL] updated host: %s\n", url.c_str());
        // }

        const auto flags = CURLU_GUESS_SCHEME|CURLU_URLENCODE;
        CURLUcode rc = curl_url_set(curlu, CURLUPART_URL, url.c_str(), flags);
        if (rc != CURLUE_OK) {
            log_write("[CURL] curl_url_set() failed: %s\n", curl_url_strerror_wrap(rc));
            return false;
        }

        if (config.port > 0) {
            rc = curl_url_set(curlu, CURLUPART_PORT, std::to_string(config.port).c_str(), flags);
            if (rc != CURLUE_OK) {
                log_write("[CURL] curl_url_set() port failed: %s\n", curl_url_strerror_wrap(rc));
            }
        }

        if (!config.user.empty()) {
            rc = curl_url_set(curlu, CURLUPART_USER, config.user.c_str(), flags);
            if (rc != CURLUE_OK) {
                log_write("[CURL] curl_url_set() user failed: %s\n", curl_url_strerror_wrap(rc));
            }
        }

        if (!config.pass.empty()) {
            rc = curl_url_set(curlu, CURLUPART_PASSWORD, config.pass.c_str(), flags);
            if (rc != CURLUE_OK) {
                log_write("[CURL] curl_url_set() pass failed: %s\n", curl_url_strerror_wrap(rc));
            }
        }

        // try and parse the path from the url, if any.
        // eg, https://example.com/some/path/here
        char* path{};
        rc = curl_url_get(curlu, CURLUPART_PATH, &path, 0);
        if (rc == CURLUE_OK && path) {
            log_write("[CURL] base path: %s\n", path);
            m_url_path = path;
            curl_free(path);
        }
    }

    // create share handle, used to share info between curl and transfer_curl.
    if (!m_curl_share) {
        m_curl_share = curl_share_init();
        if (!m_curl_share) {
            log_write("[CURL] curl_share_init() failed\n");
            return false;
        }

        // todo: use a mutex instead.
        for (auto& e : m_rwlocks) {
            rwlockInit(&e);
        }

        static const auto lock_func = [](CURL* handle, curl_lock_data data, curl_lock_access access, void* userptr) {
            auto rwlocks = static_cast<RwLock*>(userptr);
            rwlockWriteLock(&rwlocks[data]);

            #if 0
            if (access == CURL_LOCK_ACCESS_SHARED) {
                rwlockReadLock(&rwlocks[data]);
            } else {
                rwlockWriteLock(&rwlocks[data]);
            }
            #endif
        };

        static const auto unlock_func = [](CURL* handle, curl_lock_data data, void* userptr) {
            auto rwlocks = static_cast<RwLock*>(userptr);
            rwlockWriteUnlock(&rwlocks[data]);
        };

        if (m_curl_share) {
            curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
            curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
            curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
            curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
            curl_share_setopt(m_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
            curl_share_setopt(m_curl_share, CURLSHOPT_USERDATA, m_rwlocks);
            curl_share_setopt(m_curl_share, CURLSHOPT_LOCKFUNC, lock_func);
            curl_share_setopt(m_curl_share, CURLSHOPT_UNLOCKFUNC, unlock_func);
        }
    }

    return m_mounted = true;
}

PushThreadData* MountCurlDevice::CreatePushData(CURL* curl, const std::string& url, size_t offset) {
    auto data = new PushThreadData{curl};
    if (!data) {
        log_write("[PUSH:PULL] Failed to allocate PushThreadData\n");
        return nullptr;
    }

    curl_set_common_options(curl, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, PushThreadData::push_thread_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)data);

    if (offset > 0) {
        char range[64];
        std::snprintf(range, sizeof(range), "%zu-", offset);
        log_write("[PUSH:PULL] Requesting range: %s\n", range);
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }

    if (R_FAILED(data->CreateAndStart())) {
        log_write("[PUSH:PULL] Failed to create and start push thread\n");
        delete data;
        return nullptr;
    }

    return data;
}

PullThreadData* MountCurlDevice::CreatePullData(CURL* curl, const std::string& url, bool append) {
    auto data = new PullThreadData{curl};
    if (!data) {
        log_write("[PUSH:PULL] Failed to allocate PullThreadData\n");
        return nullptr;
    }

    curl_set_common_options(curl, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, PullThreadData::pull_thread_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, (void *)data);

    if (append) {
        log_write("[PUSH:PULL] Setting append mode for upload\n");
        curl_easy_setopt(curl, CURLOPT_APPEND, 1L);
    }

    if (R_FAILED(data->CreateAndStart())) {
        log_write("[PUSH:PULL] Failed to create and start pull thread\n");
        delete data;
        return nullptr;
    }

    return data;
}

void MountCurlDevice::curl_set_common_options(CURL* curl, const std::string& url) {
    // NOTE: port, user and pass are set in the curl_url.
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, sphaira::curl::USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L * 64L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1024L * 64L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (config.timeout > 0) {
        // cancel if speed is less than 1 bytes/sec for timeout seconds.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        // todo: change config to accept seconds rather than ms.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, config.timeout / 1000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.timeout);
    }

    if (m_curl_share) {
        curl_easy_setopt(curl, CURLOPT_SHARE, m_curl_share);
    }
}

size_t MountCurlDevice::write_memory_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::vector<char>*>(userdata);

    // increase by chunk size.
    const auto realsize = size * nmemb;
    if (data->capacity() < data->size() + realsize) {
        const auto rsize = std::max(realsize, data->size() + 1024 * 1024);
        data->reserve(rsize);
    }

    // store the data.
    const auto offset = data->size();
    data->resize(offset + realsize);
    std::memcpy(data->data() + offset, ptr, realsize);

    return realsize;
}

size_t MountCurlDevice::write_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::span<char>*>(userdata);
    const auto rsize = std::min(size * nmemb, data->size());

    std::memcpy(data->data(), ptr, rsize);
    *data = data->subspan(rsize);
    return rsize;
}

size_t MountCurlDevice::read_data_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto data = static_cast<std::span<const char>*>(userdata);
    const auto rsize = std::min(size * nmemb, data->size());

    std::memcpy(ptr, data->data(), rsize);
    *data = data->subspan(rsize);
    return rsize;
}

// libcurl doesn't handle html encodings, so we have to do it manually.
std::string MountCurlDevice::html_decode(const std::string_view& str) {
    struct Entry {
        std::string_view key;
        char value;
    };

    static constexpr Entry map[]{
        { "&amp;", '&' },
        { "&lt;", '<' },
        { "&gt;", '>' },
        { "&quot;", '"' },
        { "&apos;", '\'' },
        { "&nbsp;", ' ' },
        { "&#38;", '&' },
        { "&#60;", '<' },
        { "&#62;", '>' },
        { "&#34;", '"' },
        { "&#39;", '\'' },
        { "&#160;", ' ' },
        { "&#35;", '#' },
        { "&#37;", '%' },
        { "&#43;", '+' },
        { "&#61;", '=' },
        { "&#64;", '@' },
        { "&#91;", '[' },
        { "&#93;", ']' },
        { "&#123;", '{' },
        { "&#125;", '}' },
        { "&#126;", '~' },
    };

    std::string output{};
    output.reserve(str.size());

    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '&') {
            bool found = false;
            for (const auto& e : map) {
                if (!str.compare(i, e.key.length(), e.key)) {
                    output += e.value;
                    i += e.key.length() - 1; // skip ahead.
                    found = true;
                    break;
                }
            }

            if (!found) {
                output += '&';
            }
        } else {
            output += str[i];
        }
    }

    return output;
}

std::string MountCurlDevice::url_decode(const std::string& str) {
    auto unescaped = curl_unescape(str.c_str(), str.length());
    if (!unescaped) {
        return str;
    }
    ON_SCOPE_EXIT(curl_free(unescaped));

    return html_decode(unescaped);
}

std::string MountCurlDevice::build_url(const std::string& _path, bool is_dir) {
    log_write("[CURL] building url for path: %s\n", _path.c_str());
    auto path = _path;
    if (is_dir && !path.ends_with('/')) {
        path += '/'; // append trailing slash for folder.
    }

    if (!m_url_path.empty()) {
        if (path.starts_with('/') || m_url_path.ends_with('/')) {
            path = m_url_path + path;
        } else {
            path = m_url_path + '/' + path;
        }
    }

    if (!path.empty()) {
        const auto rc = curl_url_set(curlu, CURLUPART_PATH, path.c_str(), CURLU_URLENCODE);
        if (rc != CURLUE_OK) {
            log_write("[CURL] failed to set path: %s\n", curl_url_strerror_wrap(rc));
            return {};
        }
    }

    char* encoded_url;
    const auto rc = curl_url_get(curlu, CURLUPART_URL, &encoded_url, 0);
    if (rc != CURLUE_OK) {
        log_write("[CURL] failed to get encoded url: %s\n", curl_url_strerror_wrap(rc));
        return {};
    }
    ON_SCOPE_EXIT(curl_free(encoded_url));

    log_write("[CURL] encoded url: %s\n", encoded_url);
    return encoded_url;
}

} // sphaira::devoptab::common

namespace sphaira::devoptab {

using namespace sphaira::devoptab::common;

Result GetNetworkDevices(location::StdioEntries& out) {
    SCOPED_RWLOCK(&g_rwlock, false);
    out.clear();

    for (const auto& entry : g_entries) {
        if (entry) {
            const auto& config = entry->device.config;

            u32 flags = 0;
            if (config.read_only) {
                flags |= location::FsEntryFlag::FsEntryFlag_ReadOnly;
            }
            if (config.no_stat_file) {
                flags |= location::FsEntryFlag::FsEntryFlag_NoStatFile;
            }
            if (config.no_stat_dir) {
                flags |= location::FsEntryFlag::FsEntryFlag_NoStatDir;
            }

            out.emplace_back(entry->mount, entry->name, flags, config.dump_path, config.fs_hidden, config.dump_hidden);
        }
    }

    R_SUCCEED();
}

void UmountAllNeworkDevices() {
    SCOPED_RWLOCK(&g_rwlock, true);

    for (auto& entry : g_entries) {
        if (!entry) {
            continue;
        }

        log_write("[DEVOPTAB] Unmounting %s URL: %s\n", entry->mount.s, entry->device.config.url.c_str());
        entry.reset();
    }
}

void UmountNeworkDevice(const fs::FsPath& mount) {
    SCOPED_RWLOCK(&g_rwlock, true);

    auto it = std::ranges::find_if(g_entries, [&](const auto& e){
        return e && e->mount == mount;
    });

    if (it != g_entries.end()) {
        log_write("[DEVOPTAB] Unmounting %s URL: %s\n", (*it)->mount.s, (*it)->device.config.url.c_str());
        it->reset();
    } else {
        log_write("[DEVOPTAB] No such mount %s\n", mount.s);
    }
}

} // sphaira::devoptab

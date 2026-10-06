
#include "utils/devoptab.hpp"
#include "utils/devoptab_common.hpp"

#include "defines.hpp"
#include "log.hpp"
#include "title_info.hpp"
#include "app.hpp"

#include "ui/menus/game_menu.hpp"

#include "yati/nx/es.hpp"
#include "yati/nx/ns.hpp"

#include <cstring>
#include <array>
#include <memory>
#include <algorithm>

namespace sphaira::devoptab {
namespace {

namespace game = ui::menu::game;

struct ContentEntry {
    NsApplicationContentMetaStatus status{};
    std::unique_ptr<game::NspEntry> nsp{};
};

struct Entry final : game::Entry {
    std::string name{};
    std::string export_name{};
    std::vector<ContentEntry> contents{};
};

struct File {
    game::NspEntry* nsp;
    size_t off;
};

struct Dir {
    Entry* entry;
    u32 index;
};

// truncates to at most len bytes, without splitting a utf-8 sequence.
auto TruncateUtf8(std::string str, size_t len) -> std::string {
    if (str.size() <= len) {
        return str;
    }

    // walk back over any continuation bytes so the name stays valid utf-8.
    while (len && (static_cast<unsigned char>(str[len]) & 0xC0) == 0x80) {
        len--;
    }

    str.resize(len);
    return str;
}

void ParseId(std::string_view path, u64& id_out) {
    id_out = 0;

    const auto start = path.find_first_of('[');
    const auto end = path.find_first_of(']', start);
    if (start != std::string_view::npos && end != std::string_view::npos && end > start + 1) {
        // doesn't alloc because of SSO which is 32 bytes.
        const std::string hex_str{path.substr(start + 1, end - start - 1)};
        id_out = std::stoull(hex_str, nullptr, 16);
    }
}

void ParseIds(std::string_view path, u64& app_id, u64& id) {
    app_id = 0;
    id = 0;

    // strip leading slashes (should only be one anyway).
    while (path.starts_with('/')) {
        path.remove_prefix(1);
    }

    // find dir/path.nsp seperator.
    const auto dir = path.find('/');
    if (dir != std::string_view::npos) {
        const auto folder = path.substr(0, dir);
        const auto file = path.substr(dir + 1);
        ParseId(folder, app_id);
        ParseId(file, id);
    } else {
        ParseId(path, app_id);
    }
}

struct Device final : common::MountDevice {
    Device(const common::MountConfig& _config)
    : MountDevice{_config} {

    }

    ~Device();

private:
    bool Mount() override;
    int devoptab_open(void *fileStruct, const char *path, int flags, int mode) override;
    int devoptab_close(void *fd) override;
    ssize_t devoptab_read(void *fd, char *ptr, size_t len) override;
    ssize_t devoptab_seek(void *fd, off_t pos, int dir) override;
    int devoptab_fstat(void *fd, struct stat *st) override;
    int devoptab_diropen(void* fd, const char *path) override;
    int devoptab_dirreset(void* fd) override;
    int devoptab_dirnext(void* fd, char *filename, struct stat *filestat) override;
    int devoptab_dirclose(void* fd) override;
    int devoptab_lstat(const char *path, struct stat *st) override;

    game::NspEntry* FindNspFromEntry(Entry& entry, u64 id) const;
    Entry* FindEntry(u64 app_id);
    Result LoadMetaEntries(Entry& entry) const;

private:
    std::vector<Entry> m_entries{};
    keys::Keys m_keys{};
    bool m_title_init{};
    bool m_es_init{};
    bool m_ns_init{};
    bool m_keys_init{};
    bool m_mounted{};
};

Device::~Device() {
    if (m_title_init) {
        title::Exit();
    }

    if (m_es_init) {
        es::Exit();
    }

    if (m_ns_init) {
        ns::Exit();
    }
}

Result Device::LoadMetaEntries(Entry& entry) const {
    // check if we have already loaded the meta entries.
    if (!entry.contents.empty()) {
        R_SUCCEED();
    }

    title::MetaEntries entry_status{};
    R_TRY(title::GetMetaEntries(entry.app_id, entry_status, title::ContentFlag_All));

    for (const auto& status : entry_status) {
        entry.contents.emplace_back(status);
    }

    R_SUCCEED();
}

game::NspEntry* Device::FindNspFromEntry(Entry& entry, u64 id) const {
    // load all meta entries if not yet loaded.
    if (R_FAILED(LoadMetaEntries(entry))) {
        log_write("[GAME] failed to load meta entries for app id: %016lx\n", entry.app_id);
        return nullptr;
    }

    // try and find the matching nsp entry.
    for (auto& content : entry.contents) {
        if (content.status.application_id == id) {
            // build nsp entry if not yet built.
            if (!content.nsp) {
                game::ContentInfoEntry info;
                if (R_FAILED(game::BuildContentEntry(content.status, info))) {
                    log_write("[GAME] failed to build content info for app id: %016lx\n", entry.app_id);
                    return nullptr;
                }

                content.nsp = std::make_unique<game::NspEntry>();
                if (R_FAILED(game::BuildNspEntry(entry, entry.export_name, info, m_keys, *content.nsp))) {
                    log_write("[GAME] failed to build nsp entry for app id: %016lx\n", entry.app_id);
                    content.nsp.reset();
                    return nullptr;
                }

                // update path to strip the folder, if it has one.
                const auto slash = std::strchr(content.nsp->path, '/');
                if (slash) {
                    std::memmove(content.nsp->path, slash + 1, std::strlen(slash));
                }
            }

            return content.nsp.get();
        }
    }

    log_write("[GAME] failed to find content for id: %016lx\n", id);
    return nullptr;
}

Entry* Device::FindEntry(u64 app_id) {
    for (auto& entry : m_entries) {
        if (entry.app_id == app_id) {
            // the error doesn't matter here, the fs will just report an empty dir.
            LoadMetaEntries(entry);
            return &entry;
        }
    }

    log_write("[GAME] failed to find entry for app id: %016lx\n", app_id);
    return nullptr;
}

bool Device::Mount() {
    if (m_mounted) {
        return true;
    }

    log_write("[GAME] Mounting...\n");

    if (!m_title_init) {
        if (R_FAILED(title::Init())) {
            log_write("[GAME] Failed to init title info\n");
            return false;
        }
        m_title_init = true;
    }

    if (!m_es_init) {
        if (R_FAILED(es::Initialize())) {
            log_write("[GAME] Failed to init es\n");
            return false;
        }
        m_es_init = true;
    }

    if (!m_ns_init) {
        if (R_FAILED(ns::Initialize())) {
            log_write("[GAME] Failed to init ns\n");
            return false;
        }
        m_ns_init = true;
    }

    if (!m_keys_init) {
        keys::parse_keys(m_keys, true);
    }

    if (m_entries.empty()) {
        m_entries.reserve(1000);

        title::ForEachApplicationRecord([this](std::span<const NsApplicationRecord> records) {
            title::PushAsync(records);

            for (const auto& e : records) {
                m_entries.emplace_back(game::Entry{e.application_id, e.last_event});
            }
        });
    }

    log_write("[GAME] mounted with %zu entries\n", m_entries.size());
    m_mounted = true;
    return true;
}

int Device::devoptab_open(void *fileStruct, const char *path, int flags, int mode) {
    auto file = static_cast<File*>(fileStruct);

    u64 app_id{}, id{};
    ParseIds(path, app_id, id);

    if (!app_id || !id) {
        log_write("[GAME] invalid path %s\n", path);
        return -ENOENT;
    }

    auto entry = FindEntry(app_id);
    if (!entry) {
        log_write("[GAME] failed to find entry for app id: %016lx\n", app_id);
        return -ENOENT;
    }

    // try and find the matching nsp entry.
    auto nsp = FindNspFromEntry(*entry, id);
    if (!nsp) {
        log_write("[GAME] failed to find nsp for content id: %016lx\n", id);
        return -ENOENT;
    }

    file->nsp = nsp;
    return 0;
}

int Device::devoptab_close(void *fd) {
    auto file = static_cast<File*>(fd);
    std::memset(file, 0, sizeof(*file));

    return 0;
}

ssize_t Device::devoptab_read(void *fd, char *ptr, size_t len) {
    auto file = static_cast<File*>(fd);

    const auto& nsp = file->nsp;
    len = std::min<u64>(len, nsp->nsp_size - file->off);
    if (!len) {
        return 0;
    }

    u64 bytes_read;
    if (R_FAILED(nsp->Read(ptr, file->off, len, &bytes_read))) {
        log_write("[GAME] failed to read from nsp %s off: %zu len: %zu size: %zu\n", nsp->path.s, file->off, len, nsp->nsp_size);
        return -EIO;
    }

    file->off += bytes_read;
    return bytes_read;
}

ssize_t Device::devoptab_seek(void *fd, off_t pos, int dir) {
    auto file = static_cast<File*>(fd);
    const auto& nsp = file->nsp;

    if (dir == SEEK_CUR) {
        pos += file->off;
    } else if (dir == SEEK_END) {
        pos = nsp->nsp_size;
    }

    return file->off = std::clamp<u64>(pos, 0, nsp->nsp_size);
}

int Device::devoptab_fstat(void *fd, struct stat *st) {
    auto file = static_cast<File*>(fd);
    const auto& nsp = file->nsp;

    st->st_nlink = 1;
    st->st_size = nsp->nsp_size;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    return 0;
}

int Device::devoptab_diropen(void* fd, const char *path) {
    auto dir = static_cast<Dir*>(fd);

    if (!std::strcmp(path, "/")) {
        return 0;
    } else {
        u64 app_id{}, id{};
        ParseIds(path, app_id, id);

        if (!app_id || id) {
            log_write("[GAME] invalid folder path %s\n", path);
            return -ENOENT;
        }

        auto entry = FindEntry(app_id);
        if (!entry) {
            log_write("[GAME] failed to find entry for app id: %016lx\n", app_id);
            return -ENOENT;
        }

        dir->entry = entry;
        return 0;
    }
}

int Device::devoptab_dirreset(void* fd) {
    auto dir = static_cast<Dir*>(fd);

    dir->index = 0;
    return 0;
}

int Device::devoptab_dirnext(void* fd, char *filename, struct stat *filestat) {
    auto dir = static_cast<Dir*>(fd);

    if (!dir->entry) {
        if (dir->index >= m_entries.size()) {
            log_write("[GAME] dirnext: no more entries\n");
            return -ENOENT;
        }

        auto& entry = m_entries[dir->index];
        if (entry.status == title::NacpLoadStatus::None) {
            // this will never be null as it blocks until a valid entry is loaded.
            auto result = title::Get(entry.app_id);
            entry.lang = result->lang;
            entry.status = result->status;

            char name[NAME_MAX]{};
            if (result->status == title::NacpLoadStatus::Loaded) {
                const auto fix_filenames = App::GetApp()->m_dump_fix_filenames.Get();
                const auto english_name = fix_filenames ? title::GetEnglishTitleName(entry.app_id) : std::string{};
                entry.export_name = title::MakeExportTitleName(result->lang.name, english_name, fix_filenames);

                // the listed folder keeps the real (localised) title, only the exported
                // nsp filenames follow the "fix export filenames" option.
                const auto display_name = TruncateUtf8(
                    title::MakeExportTitleName(result->lang.name, {}, false), sizeof(name) - 33
                );

                if (display_name.empty()) {
                    std::snprintf(name, sizeof(name), "[%016lX]", entry.app_id);
                } else {
                    std::snprintf(name, sizeof(name), "%s [%016lX]", display_name.c_str(), entry.app_id);
                }
            } else {
                std::snprintf(name, sizeof(name), "[%016lX]", entry.app_id);
                log_write("[GAME] failed to get title info for %s\n", name);
            }

            entry.name = name;
        }

        filestat->st_nlink = 1;
        filestat->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        std::strcpy(filename, entry.name.c_str());
        dir->index++;
    } else {
        auto& entry = dir->entry;
        do {
            if (dir->index >= entry->contents.size()) {
                log_write("[GAME] dirnext: no more entries\n");
                return -ENOENT;
            }

            const auto& content = entry->contents[dir->index];
            if (!content.nsp) {
                if (!FindNspFromEntry(*entry, content.status.application_id)) {
                    log_write("[GAME] failed to find nsp for content id: %016lx\n", content.status.application_id);
                    continue;
                }
            }

            filestat->st_nlink = 1;
            filestat->st_size = content.nsp->nsp_size;
            filestat->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            std::snprintf(filename, NAME_MAX, "%s", content.nsp->path.s);
            dir->index++;
            break;
        } while (++dir->index);
    }

    return 0;
}

int Device::devoptab_dirclose(void* fd) {
    auto dir = static_cast<Dir*>(fd);
    std::memset(dir, 0, sizeof(*dir));

    return 0;
}

int Device::devoptab_lstat(const char *path, struct stat *st) {
    st->st_nlink = 1;

    if (!std::strcmp(path, "/")) {
        st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        return 0;
    } else {
        u64 app_id{}, id{};
        ParseIds(path, app_id, id);
        if (!app_id) {
            log_write("[GAME] invalid path %s\n", path);
            return -ENOENT;
        }

        auto entry = FindEntry(app_id);
        if (!entry) {
            log_write("[GAME] failed to find entry for app id: %016lx\n", app_id);
            return -ENOENT;
        }

        if (!id) {
            st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;
        }

        auto nsp = FindNspFromEntry(*entry, id);
        if (!nsp) {
            log_write("[GAME] failed to find nsp for content id: %016lx\n", id);
            return -ENOENT;
        }

        st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        st->st_size = nsp->nsp_size;
        return 0;
    }
}

} // namespace

Result MountGameAll() {
    common::MountConfig config{};
    config.read_only = true;
    config.dump_hidden = true;
    config.no_stat_file = false;;

    if (!common::MountNetworkDevice2(
        std::make_unique<Device>(config),
        config,
        sizeof(File), sizeof(Dir),
        "games", "games:/"
    )) {
        log_write("[GAME] Failed to mount GAME\n");
        R_THROW(0x1);
    }

    R_SUCCEED();
}

} // namespace sphaira::devoptab

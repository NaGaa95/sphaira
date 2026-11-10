#include "nro.hpp"
#include "nacp_compat.hpp"
#include "defines.hpp"
#include "evman.hpp"
#include "app.hpp"
#include "log.hpp"
#include "owo.hpp"

#include <switch.h>
#include <vector>
#include <cstring>
#include <string_view>
#include <minIni.h>

namespace sphaira {
namespace {

auto nro_get_abi_revision(fs::File& file, const NroData& data) -> u32 {
    struct {
        u32 magic;
        u32 revision;
    } abi{};

    // The LNY2 ABI marker follows the module header, as checked by hbmenu.
    const u64 offset = u64(data.start.mod_offset) + 0x34;
    if (offset + sizeof(abi) > data.header.size) {
        return 0;
    }

    u64 bytes_read{};
    if (R_FAILED(file.Read(offset, &abi, sizeof(abi), FsReadOption_None, &bytes_read)) ||
        bytes_read != sizeof(abi) || abi.magic != 0x32594E4C) {
        return 0;
    }

    return abi.revision;
}

auto nro_parse_internal(fs::Fs* fs, const fs::FsPath& path, NroEntry& entry) -> Result {
    entry.path = path;

    // todo: special sorting for fw 2.0.0 to make it not look like shit
    if (hosversionAtLeast(3,0,0)) {
        // it doesn't matter if we fail
        entry.timestamp.is_valid = false;
        fs->GetFileTimeStampRaw(entry.path, &entry.timestamp);
        // if (R_FAILED(fsFsGetFileTimeStampRaw(fs, entry.path, &entry.timestamp))) {
        //     // log_write("failed to get timestamp for: %s\n", path);
        // }
    }

    fs::File f;
    R_TRY(fs->OpenFile(entry.path, FsOpenMode_Read, &f));

    // todo: buffer reads to 16k to avoid 2 fs read calls per entry.
    NroData data{};
    u64 bytes_read;
    R_TRY(f.Read(0, &data, sizeof(data), FsReadOption_None, &bytes_read));
    R_UNLESS(bytes_read == sizeof(data), Result_NroBadSize);
    R_UNLESS(data.header.magic == NROHEADER_MAGIC, Result_NroBadMagic);
    entry.abi_revision = nro_get_abi_revision(f, data);

    NroAssetHeader asset{};
    R_TRY(f.Read(data.header.size, &asset, sizeof(asset), FsReadOption_None, &bytes_read));
    // R_UNLESS(asset.magic == NROASSETHEADER_MAGIC, Result_NroBadMagic);

    // we can avoid a GetSize() call by calculating the size manually.
    entry.size = data.header.size;

    // some .nro (vgedit) have bad nacp, fake the nacp
    auto& nacp = entry.nacp;
    if (asset.magic != NROASSETHEADER_MAGIC || asset.nacp.offset == 0 || asset.nacp.size != sizeof(NacpStruct)) {
        std::memset(&asset, 0, sizeof(asset));
        std::memset(&nacp, 0, sizeof(nacp));

        // get the name without the .nro
        const auto file_name = std::strrchr(path, '/') + 1;
        const auto file_name_len = std::strlen(file_name);
        std::strncpy(nacp.lang.name, file_name, file_name_len - 4);
        std::strcpy(nacp.lang.author, "Unknown");
        std::strcpy(nacp.display_version, "Unknown");

        entry.icon_offset = entry.icon_size = 0;
        entry.is_nacp_valid = false;
    } else {
        entry.size += sizeof(asset) + asset.icon.size + asset.nacp.size + asset.romfs.size;
        R_TRY(f.Read(data.header.size + asset.nacp.offset, &nacp.lang, sizeof(nacp.lang), FsReadOption_None, &bytes_read));
        R_TRY(f.Read(data.header.size + asset.nacp.offset + offsetof(NacpStruct, display_version), nacp.display_version, sizeof(nacp.display_version), FsReadOption_None, &bytes_read));

        // lazy load the icons
        entry.icon_size = asset.icon.size;
        entry.icon_offset = data.header.size + asset.icon.offset;
        entry.is_nacp_valid = true;
    }

    R_SUCCEED();
}

// this function is recursive by 1 level deep
// if the nro is in switch/folder/folder2/app.nro it will NOT be found
// switch/folder/app.nro for example will work fine.
auto nro_scan_internal(fs::Fs* fs, const fs::FsPath& path, std::vector<NroEntry>& nros, bool nested, bool scan_all_dir, bool root) -> Result {
    // we don't need to scan for folders if we are not root
    u32 dir_open_type = FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize;
    if (root) {
        dir_open_type |= FsDirOpenMode_ReadDirs;
    }

    fs::Dir d;
    R_TRY(fs->OpenDirectory(path, dir_open_type, &d));

    // we won't run out of memory here
    std::vector<FsDirectoryEntry> entries;
    R_TRY(d.ReadAll(entries));

    for (const auto& e : entries) {
        // skip hidden files / folders
        if ('.' == e.name[0]) {
            continue;
        }

        if (e.type == FsDirEntryType_Dir) {
            // assert(!root && "dir should only be scanned on non-root!");
            fs::FsPath fullpath;
            std::snprintf(fullpath, sizeof(fullpath), "%s/%s/%s.nro", path.s, e.name, e.name);

            // fast path for detecting an nro in a folder
            NroEntry entry;
            if (R_SUCCEEDED(nro_parse_internal(fs, fullpath, entry))) {
                // log_write("NRO: fast path for: %s\n", fullpath);
                nros.emplace_back(entry);
            } else {
                // slow path...
                std::snprintf(fullpath, sizeof(fullpath), "%s/%s", path.s, e.name);
                nro_scan_internal(fs, fullpath, nros, nested, scan_all_dir, false);
            }
        } else if (e.type == FsDirEntryType_File && std::string_view{e.name}.ends_with(".nro")) {
            fs::FsPath fullpath;
            std::snprintf(fullpath, sizeof(fullpath), "%s/%s", path.s, e.name);

            NroEntry entry;
            if (R_SUCCEEDED(nro_parse_internal(fs, fullpath, entry))) {
                nros.emplace_back(entry);
                if (!root && !scan_all_dir) {
                    // log_write("NRO: slow path for: %s\n", fullpath);
                    R_SUCCEED();
                }
            } else {
                log_write("error when trying to parse %s\n", fullpath.s);
            }
        }
    }

    R_SUCCEED();
}

auto nro_scan_internal(const fs::FsPath& path, std::vector<NroEntry>& nros, bool nested, bool scan_all_dir, bool root) -> Result {
    fs::FsNativeSd fs;
    return nro_scan_internal(&fs, path, nros, nested, scan_all_dir, root);
}

auto nro_scan_depth_internal(fs::Fs* fs, const fs::FsPath& path, std::vector<NroEntry>& nros, u32 depth, u32 max_depth) -> Result {
    u32 open_mode = FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize;
    if (depth < max_depth) {
        open_mode |= FsDirOpenMode_ReadDirs;
    }

    fs::Dir dir;
    R_TRY(fs->OpenDirectory(path, open_mode, &dir));

    std::vector<FsDirectoryEntry> entries;
    R_TRY(dir.ReadAll(entries));

    for (const auto& entry : entries) {
        if (entry.name[0] == '.') {
            continue;
        }

        const auto full_path = fs::AppendPath(path, entry.name);
        if (entry.type == FsDirEntryType_Dir) {
            nro_scan_depth_internal(fs, full_path, nros, depth + 1, max_depth);
        } else if (entry.type == FsDirEntryType_File) {
            const std::string_view name{entry.name};
            if (name.size() < 4 || strcasecmp(name.data() + name.size() - 4, ".nro")) {
                continue;
            }

            NroEntry nro;
            if (R_SUCCEEDED(nro_parse_internal(fs, full_path, nro))) {
                nros.emplace_back(std::move(nro));
            } else {
                log_write("error when trying to parse %s\n", full_path.s);
            }
        }
    }

    R_SUCCEED();
}

auto nro_get_icon_internal(fs::File* f, u64 size, u64 offset) -> std::vector<u8> {
    // protect again really messed up sizes.
    if (size > 1024 * 1024) {
        return {};
    }

    std::vector<u8> icon;
    u64 bytes_read{};
    icon.resize(size);

    R_TRY_RESULT(f->Read(offset, icon.data(), icon.size(), FsReadOption_None, &bytes_read), {});
    R_UNLESS(bytes_read == icon.size(), {});

    return icon;
}

auto launch_internal(const std::string& path, const std::string& argv) -> Result {
    R_TRY(envSetNextLoad(path.c_str(), argv.c_str()));

    log_write("set launch with path: %s argv: %s\n", path.c_str(), argv.c_str());

    evman::push(evman::LaunchNroEventData{path, argv});
    R_SUCCEED();
}

auto process_matches_core_mode(CpuCoreMode core_mode) -> bool {
    u64 process_mask{};
    if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        return core_mode == CpuCoreMode::Three;
    }

    const auto requested_mask = core_mode == CpuCoreMode::Four
        ? utils::FourCoreMask
        : utils::SafeCoreMask;
    return (process_mask & utils::FourCoreMask) == requested_mask;
}

} // namespace

/*
    NRO INFO PAGE:
    - icon
    - name
    - author
    - path
    - filesize
    - launch count
    - timestamp created
    - timestamp modified
*/

auto nro_verify(std::span<const u8> data) -> Result {
    NroData nro;
    R_UNLESS(data.size() >= sizeof(nro), Result_NroBadSize);

    memcpy(&nro, data.data(), sizeof(nro));
    R_UNLESS(nro.header.magic == NROHEADER_MAGIC, Result_NroBadMagic);

    R_SUCCEED();
}

auto nro_parse(const fs::FsPath& path, NroEntry& entry) -> Result {
    fs::FsNativeSd fs;
    return nro_parse_internal(&fs, path, entry);
}

auto nro_scan(const fs::FsPath& path, std::vector<NroEntry>& nros, bool nested, bool scan_all_dir) -> Result {
    return nro_scan_internal(path, nros, nested, scan_all_dir, true);
}

auto nro_scan_depth(const fs::FsPath& path, std::vector<NroEntry>& nros, u32 max_depth) -> Result {
    fs::FsNativeSd fs;
    return nro_scan_depth_internal(&fs, path, nros, 0, max_depth);
}

auto nro_get_icon(const fs::FsPath& path, u64 size, u64 offset) -> std::vector<u8> {
    fs::FsNativeSd fs;
    fs::File f;
    R_TRY_RESULT(fs.OpenFile(path, FsOpenMode_Read, &f), {});
    return nro_get_icon_internal(&f, size, offset);
}

auto nro_get_icon(const fs::FsPath& path) -> std::vector<u8> {
    fs::FsNativeSd fs;
    NroData data;
    NroAssetHeader asset;
    u64 bytes_read;

    fs::File f;
    R_TRY_RESULT(fs.OpenFile(path, FsOpenMode_Read, &f), {});

    R_TRY_RESULT(f.Read(0, &data, sizeof(data), FsReadOption_None, &bytes_read), {});
    R_UNLESS(data.header.magic == NROHEADER_MAGIC, {});
    R_TRY_RESULT(f.Read(data.header.size, &asset, sizeof(asset), FsReadOption_None, &bytes_read), {});
    R_UNLESS(asset.magic == NROASSETHEADER_MAGIC, {});

    return nro_get_icon_internal(&f, asset.icon.size, data.header.size + asset.icon.offset);
}

auto nro_get_nacp(const fs::FsPath& path, NacpStruct& nacp) -> Result {
    fs::FsNativeSd fs;
    NroData data;
    NroAssetHeader asset;
    u64 bytes_read;

    fs::File f;
    R_TRY(fs.OpenFile(path, FsOpenMode_Read, &f));

    R_TRY(f.Read(0, &data, sizeof(data), FsReadOption_None, &bytes_read));
    R_UNLESS(data.header.magic == NROHEADER_MAGIC, Result_NroBadMagic);
    R_TRY(f.Read(data.header.size, &asset, sizeof(asset), FsReadOption_None, &bytes_read));
    R_UNLESS(asset.magic == NROASSETHEADER_MAGIC, Result_NroBadMagic);
    R_TRY(f.Read(data.header.size + asset.nacp.offset, &nacp, sizeof(nacp), FsReadOption_None, &bytes_read));

    R_SUCCEED();
}

auto nro_update_info(const fs::FsPath& path, std::string_view name, std::span<const u8> icon) -> Result {
    R_UNLESS(!name.empty() && name.size() < sizeof(NacpLanguageEntry::name), Result_NroBadSize);
    R_UNLESS(!icon.empty() && icon.size() <= 1024 * 1024, Result_NroBadSize);

    fs::FsNativeSd fs;
    std::vector<u8> original;
    R_TRY(fs.read_entire_file(path, original));
    R_UNLESS(original.size() >= sizeof(NroData), Result_NroBadSize);

    NroData nro{};
    std::memcpy(&nro, original.data(), sizeof(nro));
    R_UNLESS(nro.header.magic == NROHEADER_MAGIC, Result_NroBadMagic);

    const auto asset_base = static_cast<std::size_t>(nro.header.size);
    R_UNLESS(asset_base <= original.size() && sizeof(NroAssetHeader) <= original.size() - asset_base, Result_NroBadSize);

    NroAssetHeader asset{};
    std::memcpy(&asset, original.data() + asset_base, sizeof(asset));
    R_UNLESS(asset.magic == NROASSETHEADER_MAGIC, Result_NroBadMagic);

    const auto section_valid = [&original, asset_base](const NroAssetSection& section) {
        const auto tail_size = original.size() - asset_base;
        return section.offset <= tail_size && section.size <= tail_size - section.offset;
    };
    R_UNLESS(section_valid(asset.nacp) && asset.nacp.size == sizeof(NacpStruct), Result_NroBadSize);
    R_UNLESS(section_valid(asset.romfs), Result_NroBadSize);

    NacpStruct nacp{};
    std::memcpy(&nacp, original.data() + asset_base + asset.nacp.offset, sizeof(nacp));
    for (auto& language : NacpLanguageEntries(nacp)) {
        std::memset(language.name, 0, sizeof(language.name));
        std::memcpy(language.name, name.data(), name.size());
    }

    NroAssetHeader updated_asset = asset;
    updated_asset.icon.offset = sizeof(NroAssetHeader);
    updated_asset.icon.size = icon.size();
    updated_asset.nacp.offset = updated_asset.icon.offset + updated_asset.icon.size;
    updated_asset.nacp.size = sizeof(NacpStruct);
    updated_asset.romfs.offset = updated_asset.nacp.offset + updated_asset.nacp.size;

    const auto updated_size = asset_base + updated_asset.romfs.offset + updated_asset.romfs.size;
    R_UNLESS(updated_size >= asset_base, Result_NroBadSize);

    std::vector<u8> updated(updated_size);
    std::memcpy(updated.data(), original.data(), asset_base);
    std::memcpy(updated.data() + asset_base, &updated_asset, sizeof(updated_asset));
    std::memcpy(updated.data() + asset_base + updated_asset.icon.offset, icon.data(), icon.size());
    std::memcpy(updated.data() + asset_base + updated_asset.nacp.offset, &nacp, sizeof(nacp));
    if (updated_asset.romfs.size) {
        std::memcpy(
            updated.data() + asset_base + updated_asset.romfs.offset,
            original.data() + asset_base + asset.romfs.offset,
            asset.romfs.size
        );
    }

    const auto backup_name = path.toString() + ".sphaira.bak";
    R_UNLESS(backup_name.size() < PATH_MAX, Result_NroBadSize);
    const fs::FsPath backup_path{backup_name};

    // Keep a complete recovery copy until the rebuilt NRO has been written.
    R_TRY(fs.write_entire_file(backup_path, original));
    const auto write_result = fs.write_entire_file(path, updated);
    if (R_FAILED(write_result)) {
        fs.write_entire_file(path, original);
        return write_result;
    }

    fs.DeleteFile(backup_path);
    R_SUCCEED();
}

auto nro_launch(std::string path, std::string args) -> Result {
    if (path.empty()) {
        return 1;
    }

    // keeps compat with hbloader
    // https://github.com/ITotalJustice/Gamecard-Installer-NX/blob/master/source/main.c#L73
    // https://github.com/ITotalJustice/Gamecard-Installer-NX/blob/d549c5f916dea814fa0a7e5dc8c903fa3044ba15/source/main.c#L29
    if (!path.starts_with("sdmc:")) {
        path = "sdmc:" + path;
    }

    if (args.empty()) {
        args = nro_add_arg(path);
    } else {
        args = nro_add_arg(path) + ' ' + args;
    }

    return launch_internal(path, args);
}

auto nro_launch(std::string path, std::string args, CpuCoreMode core_mode) -> Result {
    R_UNLESS(!path.empty(), Result_OwoBadArgs);

    if (!path.starts_with("sdmc:")) {
        path = "sdmc:" + path;
    }

    if (args.empty()) {
        args = nro_add_arg(path);
    } else {
        args = nro_add_arg(path) + ' ' + args;
    }

    if (process_matches_core_mode(core_mode)) {
        return launch_internal(path, args);
    }

    R_TRY(prepare_core_launch(path, args, core_mode));
    log_write("set core launch with path: %s argv: %s cores: %u\n", path.c_str(), args.c_str(), static_cast<u32>(core_mode));
    evman::push(evman::LaunchNroEventData{path, args});
    R_SUCCEED();
}

auto nro_add_arg(std::string arg) -> std::string {
    if (arg.contains(' ')) {
        return '\"' + arg + '\"';
    }
    return arg;
}

auto nro_add_arg_file(std::string arg) -> std::string {
    if (!arg.starts_with("sdmc:")) {
        arg = "sdmc:" + arg;
    }
    if (arg.contains(' ')) {
        return '\"' + arg + '\"';
    }
    return arg;
}

auto nro_normalise_path(const std::string& p) -> std::string {
    if (p.starts_with("sdmc:")) {
        return p.substr(5);
    }
    return p;
}

auto nro_find(std::span<const NroEntry> array, std::string_view name, std::string_view author, const fs::FsPath& path) -> std::optional<NroEntry> {
    const auto it = std::find_if(array.cbegin(), array.cend(), [name, author, path](auto& e){
        if (!name.empty() && !author.empty() && !path.empty()) {
            return e.GetName() == name && e.GetAuthor() == author && e.path == path;
        } else if (!name.empty()) {
            return e.GetName() == name;
        } else if (!author.empty()) {
            return e.GetAuthor() == author;
        } else if (!path.empty()) {
            return e.path == path;
        }
        return false;
    });

    if (it == array.cend()) {
        return std::nullopt;
    }

    return *it;
}

auto nro_find_name(std::span<const NroEntry> array, std::string_view name) -> std::optional<NroEntry> {
    return nro_find(array, name, {}, {});
}

auto nro_find_author(std::span<const NroEntry> array, std::string_view author) -> std::optional<NroEntry> {
    return nro_find(array, {}, author, {});
}

auto nro_find_path(std::span<const NroEntry> array, const fs::FsPath& path) -> std::optional<NroEntry> {
    return nro_find(array, {}, {}, path);
}

} // namespace sphaira

#pragma once

#include <switch.h>
#include <vector>
#include <string>
#include <span>
#include <optional>
#include "fs.hpp"
#include "utils/core.hpp"

namespace sphaira {

inline constexpr u32 NroAbiCurrentRevision = 1;

struct NroData {
    NroStart start;
    NroHeader header;
};

struct Hbini {
    u64 timestamp{}; // timestamp of last launch
    bool hidden{};
    CpuCoreMode core_mode{CpuCoreMode::Three};
};

struct MiniNacp {
    NacpLanguageEntry lang;
    char display_version[0x10];
};

struct NroEntry {
    fs::FsPath path{};
    s64 size{};
    MiniNacp nacp{};

    u64 icon_size{};
    u64 icon_offset{};
    u32 abi_revision{};

    FsTimeStampRaw timestamp{};
    Hbini hbini{};

    int image{}; // nvg image
    int x,y,w,h{}; // image
    bool is_nacp_valid{};
    std::optional<bool> has_star{std::nullopt};

    auto GetName() const -> const char* {
        return nacp.lang.name;
    }

    auto GetAuthor() const -> const char* {
        return nacp.lang.author;
    }

    auto GetDisplayVersion() const -> const char* {
        return nacp.display_version;
    }

    auto IsAbiOutdated() const -> bool {
        return abi_revision < NroAbiCurrentRevision;
    }
};

auto nro_verify(std::span<const u8> data) -> Result;
auto nro_parse(const fs::FsPath& path, NroEntry& entry) -> Result;

/**
 * Scans a folder for all nro's.
 *
 * \param path path to the folder that is to be scanned.
 * \param nros output of all scanned nros.
 * \param nested if true, it will scan any folders for nros, ie path/folder/game.nro.
 * \param scan_all_dir if true, when a folder is found, it will scan the entire
 *                     folder for all nros, rather than stopping at the first
 *                     nro found.
 *                     this does nothing if nested=false.
 */
auto nro_scan(const fs::FsPath& path, std::vector<NroEntry>& nros, bool nested = true, bool scan_all_dir = true) -> Result;
auto nro_scan_depth(const fs::FsPath& path, std::vector<NroEntry>& nros, u32 max_depth) -> Result;

auto nro_get_icon(const fs::FsPath& path, u64 size, u64 offset) -> std::vector<u8>;
auto nro_get_icon(const fs::FsPath& path) -> std::vector<u8>;
auto nro_get_nacp(const fs::FsPath& path, NacpStruct& nacp) -> Result;
auto nro_update_info(const fs::FsPath& path, std::string_view name, std::span<const u8> icon) -> Result;

// path is pre-appended to args, such that argv[0] == path
auto nro_launch(std::string path, std::string args = {}) -> Result;
auto nro_launch(std::string path, std::string args, CpuCoreMode core_mode) -> Result;

// if the arg contains a space, it will wrap it in quotes
auto nro_add_arg(std::string arg) -> std::string;

// same as above but prefixes "sdmc" to keep compat with hbmenu
auto nro_add_arg_file(std::string arg) -> std::string;

// strips sdmc:
auto nro_normalise_path(const std::string& p) -> std::string;

// helpers to find nro entry, will be made methods soon once i convert vector into a struct.
auto nro_find(std::span<const NroEntry> array, std::string_view name, std::string_view author, const fs::FsPath& path) -> std::optional<NroEntry>;
auto nro_find_name(std::span<const NroEntry> array, std::string_view name) -> std::optional<NroEntry>;
auto nro_find_author(std::span<const NroEntry> array, std::string_view author) -> std::optional<NroEntry>;
auto nro_find_path(std::span<const NroEntry> array, const fs::FsPath& path) -> std::optional<NroEntry>;

} // namespace sphaira

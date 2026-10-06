#include "utils/ownfoil_installed.hpp"

#include "title_info.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ns.hpp"
#include "defines.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace sphaira::ownfoil::installed {
namespace {

// the shop stores ids uppercase and matches case-sensitively.
auto FormatId(u64 id) -> std::string {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lX", id);
    return buf;
}

auto ParseId(const std::string& id) -> u64 {
    return std::strtoull(id.c_str(), nullptr, 16);
}

} // namespace

auto Scan(bool content) -> Inventory {
    Inventory out{};

    // ns is only open while the games or saves menu exists, and nothing in the
    // shop holds it, so every query here opens it for itself.
    if (R_FAILED(ns::Initialize())) {
        log_write("[OWNFOIL] failed to open ns\n");
        return out;
    }
    ON_SCOPE_EXIT(ns::Exit());

    title::ForEachApplicationRecord([&](std::span<const NsApplicationRecord> records) {
        for (const auto& record : records) {
            Title title{};
            title.id = FormatId(record.application_id);

            if (content) {
                // a title can carry several updates; the newest is the one the
                // shop has to beat.
                title::MetaEntries metas;
                if (R_SUCCEEDED(title::GetMetaEntries(record.application_id, metas, title::ContentFlag_Patch | title::ContentFlag_AddOnContent))) {
                    for (const auto& meta : metas) {
                        if (meta.meta_type == NcmContentMetaType_AddOnContent) {
                            out.dlc_ids.emplace_back(FormatId(meta.application_id));
                        } else {
                            title.version = std::max(title.version, meta.version);
                        }
                    }
                }
            }

            out.titles.emplace_back(std::move(title));
        }
    });

    log_write("[OWNFOIL] scanned %zu installed titles, %zu dlc (content: %d)\n", out.titles.size(), out.dlc_ids.size(), content);
    return out;
}

auto UpdateVersionOf(const Inventory& inv, const std::string& title_id) -> u32 {
    const auto it = std::find_if(inv.titles.begin(), inv.titles.end(), [&](const auto& title) {
        return title.id == title_id;
    });
    return it == inv.titles.end() ? 0 : it->version;
}

auto TitleIds(const Inventory& inv) -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(inv.titles.size());
    for (const auto& title : inv.titles) {
        out.emplace_back(title.id);
    }
    return out;
}

auto InstalledVersionOf(const std::string& title_id) -> InstalledVersion {
    InstalledVersion out{};

    // opened for the query, as Scan does.
    if (R_FAILED(ns::Initialize())) {
        return out;
    }
    ON_SCOPE_EXIT(ns::Exit());

    title::MetaEntries metas;
    if (R_FAILED(title::GetMetaEntries(ParseId(title_id), metas, title::ContentFlag_Nacp)) || metas.empty()) {
        return out;
    }

    // the newest installed - an update, when there is one - is the version the
    // game reports itself as.
    const auto newest = std::max_element(metas.begin(), metas.end(), [](const auto& a, const auto& b) {
        return a.version < b.version;
    });
    out.installed = true;
    out.version = newest->version;

    // only the one database this read needs: title::Init would open every
    // storage's and start the game menu's title cache thread with them, then
    // tear it all down again, on every page opened while nothing else held it.
    NcmContentMetaDatabase db{};
    NcmContentStorage cs{};
    ON_SCOPE_EXIT(ncmContentMetaDatabaseClose(&db));
    ON_SCOPE_EXIT(ncmContentStorageClose(&cs));

    const auto storage_id = static_cast<NcmStorageId>(newest->storageID);
    u64 program_id;
    fs::FsPath path;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id)) ||
        R_FAILED(ncmOpenContentStorage(&cs, storage_id)) ||
        R_FAILED(title::GetControlPath(&db, &cs, newest->application_id, &program_id, &path))) {
        return out;
    }

    // one past the field, so a version string that fills it is still terminated.
    char display_version[sizeof(NacpStruct::display_version) + 1]{};
    if (R_SUCCEEDED(nca::ParseControl(path, program_id, display_version, sizeof(NacpStruct::display_version), nullptr, offsetof(NacpStruct, display_version)))) {
        out.display = display_version;
    }

    return out;
}

auto HasDlc(const std::string& title_id, const std::string& dlc_id) -> bool {
    // opened for the query, as Scan does.
    if (R_FAILED(ns::Initialize())) {
        return false;
    }
    ON_SCOPE_EXIT(ns::Exit());

    title::MetaEntries metas;
    if (R_FAILED(title::GetMetaEntries(ParseId(title_id), metas, title::ContentFlag_AddOnContent))) {
        return false;
    }

    const auto id = ParseId(dlc_id);
    return std::any_of(metas.begin(), metas.end(), [id](const auto& meta) {
        return meta.application_id == id;
    });
}

auto InstalledDlcIds(const std::string& title_id) -> std::vector<std::string> {
    std::vector<std::string> out;

    // opened for the query, as Scan does.
    if (R_FAILED(ns::Initialize())) {
        return out;
    }
    ON_SCOPE_EXIT(ns::Exit());

    title::MetaEntries metas;
    if (R_SUCCEEDED(title::GetMetaEntries(ParseId(title_id), metas, title::ContentFlag_AddOnContent))) {
        for (const auto& meta : metas) {
            out.emplace_back(FormatId(meta.application_id));
        }
    }

    return out;
}

} // namespace sphaira::ownfoil::installed

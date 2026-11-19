#include "yati/nx/ncm.hpp"
#include "defines.hpp"
#include <memory>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace sphaira::ncm {
namespace {

} // namespace

auto GetMetaTypeStr(u8 meta_type) -> const char* {
    switch (meta_type) {
        case NcmContentMetaType_Unknown: return "Unknown";
        case NcmContentMetaType_SystemProgram: return "SystemProgram";
        case NcmContentMetaType_SystemData: return "SystemData";
        case NcmContentMetaType_SystemUpdate: return "SystemUpdate";
        case NcmContentMetaType_BootImagePackage: return "BootImagePackage";
        case NcmContentMetaType_BootImagePackageSafe: return "BootImagePackageSafe";
        case NcmContentMetaType_Application: return "Application";
        case NcmContentMetaType_Patch: return "Patch";
        case NcmContentMetaType_AddOnContent: return "AddOnContent";
        case NcmContentMetaType_Delta: return "Delta";
        case NcmContentMetaType_DataPatch: return "DataPatch";
    }

    return "Unknown";
}

auto GetContentTypeStr(u8 content_type) -> const char* {
    switch (content_type) {
        case NcmContentType_Meta: return "Meta";
        case NcmContentType_Program: return "Program";
        case NcmContentType_Data: return "Data";
        case NcmContentType_Control: return "Control";
        case NcmContentType_HtmlDocument: return "Html";
        case NcmContentType_LegalInformation: return "Legal";
        case NcmContentType_DeltaFragment: return "Delta";
    }

    return "Unknown";
}

auto GetReadableMetaTypeStr(u8 meta_type) -> const char* {
    switch (meta_type) {
        default:                              return "Unknown";
        case NcmContentMetaType_Application:  return "Application";
        case NcmContentMetaType_Patch:        return "Update";
        case NcmContentMetaType_AddOnContent: return "DLC";
        case NcmContentMetaType_Delta:        return "Delta";
        case NcmContentMetaType_DataPatch:    return "DLC Update";
    }
}

// taken from nxdumptool
auto GetMetaTypeShortStr(u8 meta_type) -> const char* {
    switch (meta_type) {
        case NcmContentMetaType_Unknown: return "UNK";
        case NcmContentMetaType_SystemProgram: return "SYSPRG";
        case NcmContentMetaType_SystemData: return "SYSDAT";
        case NcmContentMetaType_SystemUpdate: return "SYSUPD";
        case NcmContentMetaType_BootImagePackage: return "BIP";
        case NcmContentMetaType_BootImagePackageSafe: return "BIPS";
        case NcmContentMetaType_Application: return "BASE";
        case NcmContentMetaType_Patch: return "UPD";
        case NcmContentMetaType_AddOnContent: return "DLC";
        case NcmContentMetaType_Delta: return "DELTA";
        case NcmContentMetaType_DataPatch: return "DLCUPD";
    }

    return "UNK";
}

auto GetStorageIdStr(u8 storage_id) -> const char* {
    switch (storage_id) {
        case NcmStorageId_None: return "None";
        case NcmStorageId_Host: return "Host";
        case NcmStorageId_GameCard: return "GameCard";
        case NcmStorageId_BuiltInSystem: return "BuiltInSystem";
        case NcmStorageId_BuiltInUser: return "BuiltInUser";
        case NcmStorageId_SdCard: return "SdCard";
        case NcmStorageId_Any: return "Any";
    }

    return "Unknown";
}

auto GetReadableStorageIdStr(u8 storage_id) -> const char* {
    switch (storage_id) {
        default:                       return "Unknown";
        case NcmStorageId_None:        return "None";
        case NcmStorageId_GameCard:    return "Game Card";
        case NcmStorageId_BuiltInUser: return "System memory";
        case NcmStorageId_SdCard:      return "microSD card";
    }
}

auto GetAppId(u8 meta_type, u64 id) -> u64 {
    if (meta_type == NcmContentMetaType_Patch) {
        return id ^ 0x800;
    } else if (meta_type == NcmContentMetaType_AddOnContent) {
        return (id ^ 0x1000) & ~0xFFF;
    } else {
        return id;
    }
}

auto GetAppId(const NcmContentMetaKey& key) -> u64 {
    return GetAppId(key.type, key.id);
}

auto GetAppId(const PackagedContentMeta& meta) -> u64 {
    return GetAppId(meta.meta_type, meta.title_id);
}

auto GetContentIdFromStr(const char* str) -> NcmContentId {
    char lowerU64[0x11]{};
    char upperU64[0x11]{};
    std::memcpy(lowerU64, str, 0x10);
    std::memcpy(upperU64, str + 0x10, 0x10);

    NcmContentId nca_id{};
    *(u64*)nca_id.c = std::byteswap(std::strtoul(lowerU64, nullptr, 0x10));
    *(u64*)(nca_id.c + 8) = std::byteswap(std::strtoul(upperU64, nullptr, 0x10));
    return nca_id;
}

Result Delete(NcmContentStorage* cs, const NcmContentId *content_id) {
    bool has;
    R_TRY(ncmContentStorageHas(cs, std::addressof(has), content_id));
    if (has) {
        R_TRY(ncmContentStorageDelete(cs, content_id));
    }
    R_SUCCEED();
}

Result Register(NcmContentStorage* cs, const NcmContentId *content_id, const NcmPlaceHolderId *placeholder_id) {
    R_TRY(Delete(cs, content_id));
    return ncmContentStorageRegister(cs, content_id, placeholder_id);
}

Result GetContentMeta(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, ContentMeta& out) {
    u64 size;
    return ncmContentMetaDatabaseGet(db, key, &size, &out, sizeof(out));
}

Result GetContentInfos(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, std::vector<NcmContentInfo>& out) {
    ContentMeta content_meta;
    R_TRY(GetContentMeta(db, key, content_meta));

    return GetContentInfos(db, key, content_meta.header, out);
}

Result GetContentInfos(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, const NcmContentMetaHeader& header, std::vector<NcmContentInfo>& out) {
    s32 entries_written;
    out.resize(header.content_count);
    R_TRY(ncmContentMetaDatabaseListContentInfo(db, &entries_written, out.data(), out.size(), key, 0));
    out.resize(entries_written);

    R_SUCCEED();
}

Result DeleteKey(NcmContentStorage* cs, NcmContentMetaDatabase *db, const NcmContentMetaKey *key) {
    // get list of infos.
    std::vector<NcmContentInfo> infos;
    R_TRY(GetContentInfos(db, key, infos));

    // delete ncas
    for (const auto& info : infos) {
        R_TRY(ncmContentStorageDelete(cs, &info.content_id));
    }

    // remove from ncm db.
    R_TRY(ncmContentMetaDatabaseRemove(db, key));
    R_TRY(ncmContentMetaDatabaseCommit(db));

    R_SUCCEED();
}

Result SetRequiredSystemVersion(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, u32 version) {
    // ensure that we can even reset the sys version.
    if (!HasRequiredSystemVersion(key)) {
        R_SUCCEED();
    }

    // get the old data size.
    u64 size{};
    R_TRY(ncmContentMetaDatabaseGetSize(db, &size, key));

    // fetch the old data.
    constexpr auto bad_meta = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    R_UNLESS(size <= std::numeric_limits<size_t>::max(), bad_meta);

    u64 out_size{};
    std::vector<u8> data(static_cast<size_t>(size));
    R_TRY(ncmContentMetaDatabaseGet(db, key, &out_size, data.data(), data.size()));

    // Ensure that the returned metadata contains the required system version field.
    constexpr auto version_offset = offsetof(ContentMeta, extened.application.required_system_version);
    constexpr auto extended_version_end = offsetof(NcmApplicationMetaExtendedHeader, required_system_version) + sizeof(version);
    R_UNLESS(out_size == data.size() && data.size() >= version_offset + sizeof(version), bad_meta);

    NcmContentMetaHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    R_UNLESS(header.extended_header_size >= extended_version_end, bad_meta);

    // patch the version.
    std::memcpy(data.data() + version_offset, &version, sizeof(version));

    // write the new data back.
    R_TRY(ncmContentMetaDatabaseSet(db, key, data.data(), data.size()));
    return ncmContentMetaDatabaseCommit(db);
}

Result GetFsPathFromContentId(NcmContentStorage* cs, const NcmContentMetaKey& key, const NcmContentId& id, u64* out_program_id, fs::FsPath* out_path, FsContentAttributes attr) {
    if (out_program_id) {
        *out_program_id = key.id; // todo: verify.
        if (hosversionAtLeast(17,0,0)) {
            R_TRY(ncmContentStorageGetProgramId(cs, out_program_id, &id, attr));
        }
    }

    return ncmContentStorageGetPath(cs, out_path->s, sizeof(*out_path), &id);
}

NcmSource::NcmSource(NcmContentStorage* cs, const NcmContentId* id) : m_cs{*cs}, m_id{*id} {

}

Result NcmSource::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    s64 max_size;
    R_TRY(GetSize(&max_size));

    size = std::min<s64>(size, max_size - off);
    R_TRY(ncmContentStorageReadContentIdFile(&m_cs, buf, size, &m_id, off));

    *bytes_read = size;
    R_SUCCEED();
}

Result NcmSource::GetSize(s64* size) {
    if (!m_size) {
        R_TRY(ncmContentStorageGetSizeFromContentId(&m_cs, &m_size, &m_id));
    }

    *size = m_size;
    R_SUCCEED();
}

} // namespace sphaira::ncm

#pragma once

#include "fs.hpp"
#include "yati/source/base.hpp"

#include <switch.h>
#include <vector>

namespace sphaira::ncm {

struct PackagedContentMeta {
    u64 title_id;
    u32 title_version;
    u8 meta_type; // NcmContentMetaType
    u8 content_meta_platform; // [17.0.0+]
    NcmContentMetaHeader meta_header;
    u8 install_type; // NcmContentInstallType
    u8 _0x17;
    u32 required_sys_version;
    u8 _0x1C[0x4];
};
static_assert(sizeof(PackagedContentMeta) == 0x20);

struct ContentStorageRecord {
    NcmContentMetaKey key;
    u8 storage_id; // NcmStorageId
    u8 padding[0x7];
};

union ExtendedHeader {
    NcmApplicationMetaExtendedHeader application;
    NcmPatchMetaExtendedHeader patch;
    NcmAddOnContentMetaExtendedHeader addon;
    NcmLegacyAddOnContentMetaExtendedHeader addon_legacy;
    NcmDataPatchMetaExtendedHeader data_patch;
};

struct ContentMeta {
    NcmContentMetaHeader header;
    ExtendedHeader extened;
};

auto GetMetaTypeStr(u8 meta_type) -> const char*;
auto GetContentTypeStr(u8 content_type) -> const char*;
auto GetStorageIdStr(u8 storage_id) -> const char*;
auto GetMetaTypeShortStr(u8 meta_type) -> const char*;

auto GetReadableMetaTypeStr(u8 meta_type) -> const char*;
auto GetReadableStorageIdStr(u8 storage_id) -> const char*;

auto GetAppId(u8 meta_type, u64 id) -> u64;
auto GetAppId(const NcmContentMetaKey& key) -> u64;
auto GetAppId(const PackagedContentMeta& meta) -> u64;

auto GetContentIdFromStr(const char* str) -> NcmContentId;

Result Delete(NcmContentStorage* cs, const NcmContentId *content_id);
Result Register(NcmContentStorage* cs, const NcmContentId *content_id, const NcmPlaceHolderId *placeholder_id);

// fills out with the content header, which includes the normal and extended header.
Result GetContentMeta(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, ContentMeta& out);

// fills out will a list of all content infos tied to the key.
Result GetContentInfos(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, std::vector<NcmContentInfo>& out);
// same as above but accepts the ncm header rather than fetching it.
Result GetContentInfos(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, const NcmContentMetaHeader& header, std::vector<NcmContentInfo>& out);

// removes key from ncm, including ncas and setting the db.
Result DeleteKey(NcmContentStorage* cs, NcmContentMetaDatabase *db, const NcmContentMetaKey *key);

// sets the required system version.
Result SetRequiredSystemVersion(NcmContentMetaDatabase *db, const NcmContentMetaKey *key, u32 version);

// returns true if type is application or update.
static constexpr inline bool HasRequiredSystemVersion(u8 meta_type) {
    return meta_type == NcmContentMetaType_Application || meta_type == NcmContentMetaType_Patch;
}

static constexpr inline bool HasRequiredSystemVersion(const NcmContentMetaKey *key) {
    return HasRequiredSystemVersion(key->type);
}

// fills program id and out path of the control nca.
Result GetFsPathFromContentId(NcmContentStorage* cs, const NcmContentMetaKey& key, const NcmContentId& id, u64* out_program_id, fs::FsPath* out_path, FsContentAttributes attr = FsContentAttributes_All);

// helper for reading nca from ncm.
struct NcmSource final : yati::source::Base {
    NcmSource(NcmContentStorage* cs, const NcmContentId* id);
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;
    Result GetSize(s64* size);

private:
    NcmContentStorage m_cs;
    NcmContentId m_id;
    s64 m_size{};
};

} // namespace sphaira::ncm

#pragma once

#include "fs.hpp"
#include "keys.hpp"
#include "ncm.hpp"
#include "yati/source/base.hpp"

#include <switch.h>
#include <vector>
#include <memory>

namespace sphaira::nca {

#define NCA0_MAGIC 0x3041434E
#define NCA2_MAGIC 0x3241434E
#define NCA3_MAGIC 0x3341434E

#define NCA_SECTOR_SIZE             0x200
#define NCA_XTS_SECTION_SIZE        0xC00
#define NCA_SECTION_TOTAL           0x4
#define NCA_MEDIA_REAL(x)((u64(x) * 0x200))

#define NCA_PROGRAM_LOGO_OFFSET     0x8000
#define NCA_META_CNMT_OFFSET        0xC20

enum KeyGenerationOld {
    KeyGenerationOld_100    = 0x0,
    KeyGenerationOld_Unused = 0x1,
    KeyGenerationOld_300    = 0x2,
};

enum KeyGeneration {
    KeyGeneration_301     = 0x3,
    KeyGeneration_400     = 0x4,
    KeyGeneration_500     = 0x5,
    KeyGeneration_600     = 0x6,
    KeyGeneration_620     = 0x7,
    KeyGeneration_700     = 0x8,
    KeyGeneration_810     = 0x9,
    KeyGeneration_900     = 0x0A,
    KeyGeneration_910     = 0x0B,
    KeyGeneration_1210    = 0x0C,
    KeyGeneration_1300    = 0x0D,
    KeyGeneration_1400    = 0x0E,
    KeyGeneration_1500    = 0x0F,
    KeyGeneration_1600    = 0x10,
    KeyGeneration_1700    = 0x11,
    KeyGeneration_1800    = 0x12,
    KeyGeneration_1900    = 0x13,
    KeyGeneration_2000    = 0x14,
    KeyGeneration_2100    = 0x15,
    KeyGeneration_2200    = 0x16,
    KeyGeneration_2300    = 0x17,
    KeyGeneration_Invalid = 0xFF,
};

enum KeyAreaEncryptionKeyIndex {
    KeyAreaEncryptionKeyIndex_Application = 0x0,
    KeyAreaEncryptionKeyIndex_Ocean       = 0x1,
    KeyAreaEncryptionKeyIndex_System      = 0x2
};

enum DistributionType {
    DistributionType_System   = 0x0,
    DistributionType_GameCard = 0x1
};

enum ContentType {
    ContentType_Program    = 0x0,
    ContentType_Meta       = 0x1,
    ContentType_Control    = 0x2,
    ContentType_Manual     = 0x3,
    ContentType_Data       = 0x4,
    ContentType_PublicData = 0x5,
};

enum FileSystemType {
    FileSystemType_RomFS = 0x0,
    FileSystemType_PFS0  = 0x1
};

enum HashType {
    HashType_Auto                    = 0x0,
    HashType_HierarchicalSha256      = 0x2,
    HashType_HierarchicalIntegrity   = 0x3
};

enum EncryptionType {
    EncryptionType_Auto                  = 0x0,
    EncryptionType_None                  = 0x1,
    EncryptionType_AesXts                = 0x2,
    EncryptionType_AesCtr                = 0x3,
    EncryptionType_AesCtrEx              = 0x4,
    EncryptionType_AesCtrSkipLayerHash   = 0x5, // [14.0.0+]
    EncryptionType_AesCtrExSkipLayerHash = 0x6, // [14.0.0+]
};

struct SectionTableEntry {
    u32 media_start_offset; // divided by 0x200.
    u32 media_end_offset;   // divided by 0x200.
    u8 _0x8[0x4];           // unknown.
    u8 _0xC[0x4];           // unknown.

    auto IsValid() const -> bool {
        return media_start_offset && media_end_offset;
    }

    auto GetOffset() const -> u64 {
        return NCA_MEDIA_REAL(media_start_offset);
    }

    auto GetOffsetEnd() const -> u64 {
        return NCA_MEDIA_REAL(media_end_offset);
    }

    auto GetSize() const -> u64 {
        return GetOffsetEnd() - GetOffset();
    }
};

struct LayerRegion {
    u64 offset;
    u64 size;
};

struct HierarchicalSha256Data {
    u8 master_hash[0x20];
    u32 block_size;
    u32 layer_count;
    LayerRegion hash_layer;
    LayerRegion pfs0_layer;
    LayerRegion unused_layers[3];
    u8 _0x78[0x80];
};

#pragma pack(push, 1)
struct HierarchicalIntegrityVerificationLevelInformation {
    u64 logical_offset;
    u64 hash_data_size;
    u32 block_size; // log2
    u32 _0x14; // reserved
};
#pragma pack(pop)

struct InfoLevelHash {
    u32 max_layers;
    HierarchicalIntegrityVerificationLevelInformation levels[6];
    u8 signature_salt[0x20];
};

struct IntegrityMetaInfo {
    u32 magic; // IVFC
    u32 version;
    u32 master_hash_size;
    InfoLevelHash info_level_hash;
    u8 master_hash[0x20];
    u8 _0xE0[0x18];
};

static_assert(sizeof(HierarchicalSha256Data) == 0xF8);
static_assert(sizeof(IntegrityMetaInfo) == 0xF8);
static_assert(sizeof(HierarchicalSha256Data) == sizeof(IntegrityMetaInfo));

struct BucketTreeHeader {
    u32 magic; // BKTR
    u32 version;
    u32 count;
    u8 _0xC[0x4];
};

struct PatchInfo {
    u64 indirect_offset;
    u64 indirect_size;
    BucketTreeHeader indirect_header;
    u64 aes_ctr_offset;
    u64 aes_ctr_size;
    BucketTreeHeader aes_ctr_header;
};
static_assert(sizeof(PatchInfo) == 0x40);

struct CompressionInfo {
    u64 table_offset;
    u64 table_size;
    BucketTreeHeader table_header;
    u8 _0x20[0x8];
};
static_assert(sizeof(CompressionInfo) == 0x28);

struct BktrEntry {
    u8 _0x0[0x4];
    u32 count;
    u64 size;
    u64 offsets[0x3FF0 / sizeof(u64)];
};
static_assert(sizeof(BktrEntry) == 0x4000);

struct NX_PACKED BktrRelocationEntry {
    u64 patched_addr;
    u64 source_addr;
    u32 flag;
};
static_assert(sizeof(BktrRelocationEntry) == 0x14);

struct BktrRelocationBucket {
    u8 _0x0[0x4];
    u32 count;
    u64 end_offset;
    BktrRelocationEntry entries[0x3FF0 / sizeof(BktrRelocationEntry)];
    u8 _[0x3FF0 % sizeof(BktrRelocationEntry)];
};
static_assert(sizeof(BktrRelocationBucket) == 0x4000);

struct FsHeader {
    u16 version;           // always 2.
    u8 fs_type;            // see FileSystemType.
    u8 hash_type;          // see HashType.
    u8 encryption_type;    // see EncryptionType.
    u8 metadata_hash_type;
    u8 _0x6[0x2];          // empty.

    union {
        HierarchicalSha256Data hierarchical_sha256_data;
        IntegrityMetaInfo integrity_meta_info; // used for romfs
    } hash_data;

    PatchInfo patch_info;
    u64 section_ctr;
    u8 spares_info[0x30];
    CompressionInfo compression_info;
    u8 meta_data_hash_data_info[0x30];
    u8 reserved[0x30];

    auto IsValid() const -> bool {
        return version == 2;
    }
};
static_assert(sizeof(FsHeader) == 0x200);
static_assert(sizeof(FsHeader::hash_data) == 0xF8);

struct SectionHeaderHash {
    u8 sha256[0x20];
};

struct KeyArea {
    u8 area[0x10];
};

struct Header {
    u8 rsa_fixed_key[0x100];
    u8 rsa_npdm[0x100];        // key from npdm.
    u32 magic;
    u8 distribution_type;      // see DistributionType.
    u8 content_type;           // see ContentType.
    u8 old_key_gen;            // see KeyGenerationOld.
    u8 kaek_index;             // see KeyAreaEncryptionKeyIndex.
    u64 size;
    u64 program_id;
    u32 context_id;
    union {
        u32 sdk_version;
        struct {
            u8 sdk_revision;
            u8 sdk_micro;
            u8 sdk_minor;
            u8 sdk_major;
        };
    };
    u8 key_gen;                // see KeyGeneration.
    u8 sig_key_gen;
    u8 _0x222[0xE];            // empty.
    FsRightsId rights_id;

    SectionTableEntry fs_table[NCA_SECTION_TOTAL];
    SectionHeaderHash fs_header_hash[NCA_SECTION_TOTAL];
    KeyArea key_area[NCA_SECTION_TOTAL];

    u8 _0x340[0xC0];           // empty.

    FsHeader fs_header[NCA_SECTION_TOTAL];

    auto IsValid() const -> bool {
        return magic == NCA3_MAGIC;
    }

    auto GetKeyGeneration() const -> u8 {
        if (old_key_gen < key_gen) {
            return key_gen;
        } else {
            return old_key_gen;
        }
    }

    void SetKeyGeneration(u8 key_generation) {
        if (key_generation <= 0x2) {
            old_key_gen = key_generation;
            key_gen = 0;
        } else {
            old_key_gen = 0x2;
            key_gen = key_generation;
        }
    }

    auto GetSectionCount() const -> u8 {
        u8 count = 0;
        for (u32 i = 0; i < NCA_SECTION_TOTAL; i++) {
            if (!fs_header[i].IsValid() || !fs_table[i].IsValid()) {
                break;
            }
            count++;
        }
        return count;
    }
};
static_assert(sizeof(Header) == 0xC00);

auto GetContentTypeStr(u8 content_type) -> const char*;
auto GetDistributionTypeStr(u8 distribution_type) -> const char*;

Result DecryptHeader(const void* in, const keys::Keys& keys, Header& out);

Result DecryptKeak(const keys::Keys& keys, Header& header);
Result EncryptKeak(const keys::Keys& keys, Header& header, u8 key_generation);
Result VerifyFixedKey(const Header& header);

// helpers that parse an nca.
Result ParseCnmt(const fs::FsPath& path, u64 program_id, ncm::PackagedContentMeta& header, std::vector<u8>& extended_header, std::vector<NcmPackagedContentInfo>& infos);
Result ParseControl(const fs::FsPath& path, u64 program_id, void* nacp_out = nullptr, s64 nacp_size = 0, std::vector<u8>* icon_out = nullptr, s64 nacp_off = 0);

auto GetKeyGenStr(u8 key_gen) -> const char*;

// finds and decrypts the title key, also decrypts header key area if needed.
Result GetDecryptedTitleKey(Header& header, const keys::Keys& keys, keys::KeyEntry& out);
// same as above but also checks the path for ticket.
Result GetDecryptedTitleKey(fs::Fs* fs, const fs::FsPath& path, Header& header, const keys::Keys& keys, keys::KeyEntry& out);

// helpers.
struct DecyptedData : yati::source::Base {
    DecyptedData(u64 align, const std::shared_ptr<yati::source::Base>& source);
    Result Read(void *_buf, s64 _off, s64 _size, u64* _bytes_read) override;
    virtual Result SetCtr(u64 ctr) = 0;

private:
    virtual Result Decrypt(void* buf, s64 off, s64 size) = 0;

private:
    std::shared_ptr<yati::source::Base> m_source;
    const u64 m_align;
};

// todo: add support for xts sections.
struct DecyptedDataCtr final : DecyptedData {
    DecyptedDataCtr(const void* key, u64 ctr, const std::shared_ptr<yati::source::Base>& source);
    Result SetCtr(u64 ctr) override;

private:
    Result Decrypt(void* buf, s64 off, s64 size) override;

private:
    Aes128CtrContext m_ctx{};
    u8 m_ctr[AES_BLOCK_SIZE]{};
};

struct NcaReader final : yati::source::Base {
    NcaReader(const nca::Header& decrypted_header, const void* key, u64 size, const std::shared_ptr<yati::source::Base>& source);
    Result Read(void *_buf, s64 off, s64 size, u64* bytes_read) override;
    Result ReadEncrypted(void *_buf, s64 off, s64 size, u64* bytes_read);

private:
    Result ReadInternal(void *_buf, s64 off, s64 size, u64* bytes_read, bool decrypt);

private:
    const nca::Header m_header;
    const u64 m_capacity;
    std::shared_ptr<yati::source::Base> m_source;
    std::unique_ptr<DecyptedData> m_decryptor{};
    u8 m_key[0x10]{};
};

} // namespace sphaira::nca

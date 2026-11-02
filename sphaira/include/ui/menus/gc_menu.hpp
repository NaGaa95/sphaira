#pragma once

#include "ui/menus/menu_base.hpp"
#include "yati/container/base.hpp"
#include "yati/source/base.hpp"
#include "ui/list.hpp"
#include <span>
#include <memory>
#include <string>

// todo: pr to libnx
extern "C" {

typedef enum {
    FsGameCardPartitionRaw_None   = -1,
    FsGameCardPartitionRaw_Normal = 0,
    FsGameCardPartitionRaw_Secure = 1,
} FsGameCardPartitionRaw;

Result fsOpenGameCardStorage(FsStorage* out, const FsGameCardHandle* handle, FsGameCardPartitionRaw partition);
Result fsOpenGameCardDetectionEventNotifier(FsEventNotifier* out);

}

namespace sphaira::ui::menu::gc {

////////////////////////////////////////////////
// The below structs are taken from nxdumptool./
////////////////////////////////////////////////

/// Located at offset 0x7000 in the gamecard image.
typedef struct {
    u8 signature[0x100];        ///< RSA-2048-PKCS#1 v1.5 with SHA-256 signature over the rest of the data.
    u32 magic;                  ///< "CERT".
    u32 version;
    u8 kek_index;
    u8 reserved[0x7];
    u8 t1_card_device_id[0x10];
    u8 iv[0x10];
    u8 hw_key[0x10];            ///< Encrypted.
    u8 data[0xC0];              ///< Encrypted.
} FsGameCardCertificate;

static_assert(sizeof(FsGameCardCertificate) == 0x200);

typedef struct {
    u8 maker_code;      ///< FsCardId1MakerCode.
    u8 memory_capacity; ///< Matches GameCardRomSize.
    u8 reserved;        ///< Known values: 0x00, 0x01, 0x02, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0E, 0x80.
    u8 memory_type;     ///< FsCardId1MemoryType.
} FsCardId1;

static_assert(sizeof(FsCardId1) == 0x4);

enum {
    FsCardId1MemoryType_IsLate = BIT(5), ///< 0: Fast, 1: Late.
};

/// Set in the gamecard header flags of T2 gamecards (Switch 2 and Switch 2 Edition).
/// These have a second header and certificate, but no initial data area.
enum {
    GameCardFlags_CardHeaderSignKey = BIT(7),
};

typedef struct {
    u8 card_security_number;    ///< FsCardId2CardSecurityNumber.
    u8 card_type;               ///< FsCardId2CardType.
    u8 reserved[0x2];           ///< Usually filled with zeroes.
} FsCardId2;

static_assert(sizeof(FsCardId2) == 0x4);

typedef struct {
    u8 reserved[0x4];   ///< Usually filled with zeroes.
} FsCardId3;

static_assert(sizeof(FsCardId3) == 0x4);

/// Returned by fsDeviceOperatorGetGameCardIdSet.
typedef struct {
    FsCardId1 id1;  ///< Specifies maker code, memory capacity and memory type.
    FsCardId2 id2;  ///< Specifies card security number and card type.
    FsCardId3 id3;  ///< Always zero (so far).
} FsGameCardIdSet;

/// Encrypted using AES-128-ECB with the common titlekek generator key (stored in the .rodata segment from the Lotus firmware).
typedef struct {
    union {
        u8 value[0x10];
        struct {
            u8 package_id[0x8]; ///< Matches package_id from GameCardHeader.
            u8 reserved[0x8];   ///< Just zeroes.
        };
    };
} GameCardKeySource;

static_assert(sizeof(GameCardKeySource) == 0x10);

/// Plaintext area. Dumped from FS program memory.
typedef struct {
    GameCardKeySource key_source;
    u8 encrypted_titlekey[0x10];    ///< Encrypted using AES-128-CCM with the decrypted key_source and the nonce from this section.
    u8 mac[0x10];                   ///< Used to verify the validity of the decrypted titlekey.
    u8 nonce[0xC];                  ///< Used as the IV to decrypt encrypted_titlekey using AES-128-CCM.
    u8 reserved[0x1C4];
} GameCardInitialData;

static_assert(sizeof(GameCardInitialData) == 0x200);

/// Encrypted using AES-128-CTR with the key and IV/counter from the `GameCardTitleKeyAreaEncryption` section. Assumed to be all zeroes in retail gamecards.
typedef struct {
    u8 titlekey[0x10];  ///< Decrypted titlekey from the `GameCardInitialData` section.
    u8 reserved[0xCF0];
} GameCardTitleKeyArea;

static_assert(sizeof(GameCardTitleKeyArea) == 0xD00);

/// Encrypted using RSA-2048-OAEP and a private OAEP key from AuthoringTool. Assumed to be all zeroes in retail gamecards.
typedef struct {
    u8 titlekey_encryption_key[0x10];   ///< Used as the AES-128-CTR key for the `GameCardTitleKeyArea` section. Randomly generated during XCI creation by AuthoringTool.
    u8 titlekey_encryption_iv[0x10];    ///< Used as the AES-128-CTR IV/counter for the `GameCardTitleKeyArea` section. Randomly generated during XCI creation by AuthoringTool.
    u8 reserved[0xE0];
} GameCardTitleKeyAreaEncryption;

static_assert(sizeof(GameCardTitleKeyAreaEncryption) == 0x100);

/// Used to secure communications between the Lotus and the inserted gamecard.
/// Supposedly precedes the gamecard header.
typedef struct {
    GameCardInitialData initial_data;
    GameCardTitleKeyArea titlekey_area;
    GameCardTitleKeyAreaEncryption titlekey_area_encryption;
} GameCardKeyArea;

static_assert(sizeof(GameCardKeyArea) == 0x1000);

typedef struct {
    u8 maker_code;              ///< GameCardUidMakerCode.
    u8 version;                 ///< TODO: determine whether this matches GameCardVersion or not.
    u8 card_type;               ///< GameCardUidCardType.
    u8 unique_data[0x9];
    u32 random;
    u8 platform_flag;
    u8 reserved[0xB];
    FsCardId1 card_id_1_mirror; ///< This field mirrors bit 5 of FsCardId1MemoryType.
    u8 mac[0x20];
} GameCardUid;

static_assert(sizeof(GameCardUid) == 0x40);

/// Plaintext area. Dumped from FS program memory.
/// Overall structure may change with each new LAFW version.
typedef struct {
    u32 asic_security_mode; ///< Determines how the Lotus ASIC initialised the gamecard security mode. Usually 0xFFFFFFF9.
    u32 asic_status;        ///< Bitmask of the internal gamecard interface status. Usually 0x20000000.
    FsCardId1 card_id1;
    FsCardId2 card_id2;
    GameCardUid card_uid;
    u8 reserved[0x190];
    u8 mac[0x20];           ///< Changes with each gamecard (re)insertion.
} GameCardSpecificData;

static_assert(sizeof(GameCardSpecificData) == 0x200);

/// Plaintext area. Dumped from FS program memory.
/// This struct is returned by Lotus command "ChangeToSecureMode" (0xF). This means it is only available *after* the gamecard secure area has been mounted.
/// A copy of the gamecard header without the RSA-2048 signature and a plaintext GameCardInfo precedes this struct in FS program memory.
/// T2 gamecards use a 0x400 byte certificate that also covers the reserved area,
/// and have unknown data in place of the initial data.
typedef struct {
    GameCardSpecificData specific_data;
    FsGameCardCertificate certificate;
    u8 reserved[0x200];
    GameCardInitialData initial_data;
} GameCardSecurityInformation;

static_assert(sizeof(GameCardSecurityInformation) == 0x800);

///////////////////
// nxdumptool fin./
///////////////////

struct GcCollection : yati::container::CollectionEntry {
    GcCollection(const char* _name, s64 _size, u8 _type, u8 _id_offset) {
        name = _name;
        size = _size;
        type = _type;
        id_offset = _id_offset;
    }

    // NcmContentType
    u8 type{};
    u8 id_offset{};
};

using GcCollections = std::vector<GcCollection>;

struct ApplicationEntry {
    u64 app_id{};
    u32 version{};
    u8 key_gen{};
    std::vector<u8> icon;
    NacpLanguageEntry lang_entry{};
    std::string english_name{};

    std::vector<GcCollections> application{};
    std::vector<GcCollections> patch{};
    std::vector<GcCollections> add_on{};
    std::vector<GcCollections> data_patch{};
    yati::container::Collections tickets{};

    auto GetSize() const -> s64;
    auto GetSize(const std::vector<GcCollections>& entries) const -> s64;
};

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "GC"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

    Result GcStorageRead(void* buf, s64 off, s64 size);

private:
    Result GcPoll(bool* inserted);
    Result GcOnEvent(bool force = false);

    // GameCard FS api.
    Result GcMount();
    void GcUnmount();

    // GameCard Storage api
    Result GcMountStorage();
    void GcUmountStorage();
    Result GcMountPartition(FsGameCardPartitionRaw partition);
    void GcUnmountPartition();
    Result GcStorageReadInternal(void* buf, s64 off, s64 size, u64* bytes_read);

    // taken from nxdumptool.
    Result GcGetSecurityInfo(GameCardSecurityInformation& out);

    Result LoadControlData(ApplicationEntry& e);
    Result UpdateStorageSize();
    void FreeImage();
    void OnChangeIndex(s64 new_index);
    Result DumpGames(u32 flags);
    Result DumpXcz(u32 flags);

    Result MountGcFs();

private:
    FsDeviceOperator m_dev_op{};
    FsGameCardHandle m_handle{};
    std::unique_ptr<fs::FsNativeGameCard> m_fs{};
    FsEventNotifier m_event_notifier{};
    Event m_event{};

    std::vector<ApplicationEntry> m_entries{};
    std::unique_ptr<List> m_list{};
    s64 m_entry_index{};
    s64 m_option_index{};

    s64 m_size_free_sd{};
    s64 m_size_total_sd{};
    s64 m_size_free_nand{};
    s64 m_size_total_nand{};
    int m_icon{};
    bool m_mounted{};

    FsStorage m_storage{};
    // size of normal partition.
    s64 m_partition_normal_size{};
    // size of secure partition.
    s64 m_partition_secure_size{};
    // used size reported in the xci header.
    s64 m_storage_trimmed_size{};
    // total size of m_partition_normal_size + m_partition_secure_size.
    s64 m_storage_total_size{};
    // reported size via rom_size in the xci header.
    s64 m_storage_full_size{};
    // found in xci header.
    u64 m_package_id{};
    // found in xci header.
    u8 m_initial_data_hash[SHA256_HASH_SIZE]{};
    // set from the xci header flags, T2 gamecards have no initial data.
    bool m_is_t2{};
    // currently mounted storage partiton.
    FsGameCardPartitionRaw m_partition{FsGameCardPartitionRaw_None};
    bool m_storage_mounted{};

    // set when the gc should be re-mounted, cleared when handled.
    bool m_dirty{};
};

} // namespace sphaira::ui::menu::gc

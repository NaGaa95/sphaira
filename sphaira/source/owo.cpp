// creates and installs nca's on the fly
// based on hacbrewpack (romfs creation) and yati (installation)
#include <switch.h>
#include <cstring>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <utility>

#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/npdm.hpp"
#include "yati/nx/ns.hpp"
#include "yati/nx/es.hpp"
#include "yati/nx/keys.hpp"
#include "yati/nx/crypto.hpp"

#include "owo.hpp"
#include "defines.hpp"
#include "app.hpp"
#include "fs.hpp"
#include "nro.hpp"
#include "nacp_compat.hpp"
#include "ui/progress_box.hpp"
#include "i18n.hpp"
#include "image.hpp"
#include "log.hpp"

namespace sphaira {
namespace {

constexpr u32 IVFC_MAX_LEVEL = 6;
constexpr u32 IVFC_HASH_BLOCK_SIZE = 0x4000;
constexpr u32 PFS0_EXEFS_HASH_BLOCK_SIZE = 0x10000;
constexpr u32 PFS0_LOGO_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_META_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_PADDING_SIZE = 0x200;
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;
constexpr u32 ROMFS_FILEPARTITION_OFS = 0x200;

constexpr const u8 HBL_MAIN_DATA[]{
    #embed <exefs/main>
};

constexpr const u8 HBL_NPDM_DATA[]{
    #embed <exefs/main.npdm>
};

bool g_core_launch_pending{};

auto lower_ascii(std::string_view value) -> std::string {
    std::string out(value);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return out;
}

auto is_nethersx2_nro_path(std::string_view path) -> bool {
    while (!path.empty() && (path.front() == ' ' || path.front() == '"')) {
        path.remove_prefix(1);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '"')) {
        path.remove_suffix(1);
    }

    const auto lower = lower_ascii(path);
    const auto slash = lower.find_last_of("/\\");
    const std::string_view file_name = slash == std::string::npos ? std::string_view(lower) : std::string_view(lower).substr(slash + 1);

    // The launcher is normally /switch/NetherSX2.nro, but users may keep it
    // under its own folder or use a differently-cased filename. Core NROs
    // extracted by the launcher also live below a NetherSX2 directory.
    return file_name == "nethersx2.nro" || lower.contains("/nethersx2/");
}

auto remove_sdmc_prefixes(std::string value) -> std::string {
    constexpr std::string_view prefix = "sdmc:";
    for (auto pos = value.find(prefix); pos != std::string::npos; pos = value.find(prefix, pos)) {
        value.erase(pos, prefix.size());
    }
    return value;
}

auto forwarder_path_hash(std::string_view hash_path) -> u64 {
    u64 hash_data[SHA256_HASH_SIZE / sizeof(u64)];
    sha256CalculateHash(hash_data, hash_path.data(), hash_path.length());
    return hash_data[0];
}

auto homebrew_forwarder_tid(u64 hash) -> u64 {
    return 0x0500000000000000 | (hash & 0x00FFFFFFFFFFF000);
}

auto legacy_retail_forwarder_tid(u64 hash) -> u64 {
    return 0x0100000000000000 | (hash & 0x00FFFFFFFFFFF000);
}

auto native_game_forwarder_tid(u64 hash) -> u64 {
    // The E000 mask guarantees (tid & 0x1F00) == 0, like a retail base title.
    return 0x0100000000000000 | (hash & 0x00FFFFFFFFFFE000);
}

void append_forwarder_tid_candidates(std::vector<u64>& tids, u64 hash) {
    tids.emplace_back(homebrew_forwarder_tid(hash));
    tids.emplace_back(legacy_retail_forwarder_tid(hash));
    tids.emplace_back(native_game_forwarder_tid(hash));
}

// Text drawn onto the HOME icon of forwarders using a non-default address
// space, so side-by-side entries for the same NRO can be told apart. The
// default 36-bit space keeps the original icon.
auto address_space_icon_label(ForwarderAddressSpace address_space) -> std::string_view {
    switch (address_space) {
        case ForwarderAddressSpace::Bit32: return "32-BIT";
        case ForwarderAddressSpace::Bit32NoAlias: return "32-BIT NA";
        case ForwarderAddressSpace::Bit39: return "39-BIT";
        case ForwarderAddressSpace::Bit36: return {};
    }
    return {};
}

// stdio-like wrapper for std::vector
struct BufHelper {
    BufHelper() = default;
    BufHelper(std::span<const u8> data) {
        write(data);
    }

    void write(const void* data, u64 size) {
        if (offset + size >= buf.size()) {
            buf.resize(offset + size);
        }
        std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }

    void write(std::span<const u8> data) {
        write(data.data(), data.size());
    }

    void seek(u64 where_to) {
        offset = where_to;
    }

    [[nodiscard]]
    auto tell() const {
        return offset;
    }

    std::vector<u8> buf;
    u64 offset{};
};

struct NcaEntry {
    NcaEntry(const BufHelper& buf, NcmContentType _type) : data{buf.buf}, type{_type} {
        sha256CalculateHash(hash, data.data(), data.size());
    }

    const std::vector<u8> data;
    const u8 type;
    u8 hash[SHA256_HASH_SIZE];
};

struct CnmtHeader {
    u64 title_id;
    u32 title_version;
    u8 meta_type; // NcmContentMetaType
    u8 _0xD;
    NcmContentMetaHeader meta_header;
    u8 install_type; // NcmContentInstallType
    u8 _0x17;
    u32 required_sys_version;
    u8 _0x1C[0x4];
};
static_assert(sizeof(CnmtHeader) == 0x20);

struct NcmContentMetaData {
    NcmContentMetaHeader header;
    NcmApplicationMetaExtendedHeader extended;
    NcmContentInfo infos[3];
};

struct NcaMetaEntry {
    NcaMetaEntry(const BufHelper& buf, NcmContentType type) : nca_entry{buf, type} { }

    NcaEntry nca_entry;
    NcmContentMetaHeader content_meta_header{};
    NcmContentMetaKey content_meta_key{};
    ncm::ContentStorageRecord content_storage_record{};
    NcmContentMetaData content_meta_data{};
};

struct Pfs0Header {
    u32 magic;
    u32 total_files;
    u32 string_table_size;
    u32 padding;
};

struct Pfs0FileTable {
    u64 data_offset;
    u64 data_size;
    u32 name_offset;
    u32 padding;
};

struct Pfs0StringTable {
    char name[256];
};

struct FileEntry {
    std::string name;
    std::vector<u8> data;
};

using FileEntries = std::vector<FileEntry>;

struct NpdmPatch {
    char title_name[0x10]{"Application"};
    char product_code[0x10]{};
    u64 tid;
    ForwarderAddressSpace address_space{ForwarderAddressSpace::Bit36};
    CpuCoreMode core_mode{CpuCoreMode::Three};
    ForwarderSvcDebugMode svc_debug_mode{ForwarderSvcDebugMode::Automatic};
    // sets AllowDebug instead of ForceDebug in the DebugFlags capability,
    // ie. the process can be debugged rather than being a debugger itself.
    bool allow_debug{};
};

struct NcapPatch {
    std::string name;
    std::string author;
    u64 tid;
    bool profile_selection{};
    bool screenshot{true};
    bool video_capture{true};
};

typedef struct romfs_dirent_ctx {
    u32 entry_offset;
    struct romfs_dirent_ctx *parent; /* Parent node */
    struct romfs_dirent_ctx *child; /* Child node */
    struct romfs_dirent_ctx *sibling; /* Sibling node */
    struct romfs_fent_ctx *file; /* File node */
    struct romfs_dirent_ctx *next; /* Next node */
} romfs_dirent_ctx_t;

typedef struct romfs_fent_ctx {
    u32 entry_offset;
    u64 offset;
    u64 size;
    romfs_dirent_ctx_t *parent; /* Parent dir */
    struct romfs_fent_ctx *sibling; /* Sibling file */
    struct romfs_fent_ctx *next; /* Logical next file */
} romfs_fent_ctx_t;

typedef struct {
    romfs_fent_ctx_t *files;
    u64 num_dirs;
    u64 num_files;
    u64 dir_table_size;
    u64 file_table_size;
    u64 dir_hash_table_size;
    u64 file_hash_table_size;
    u64 file_partition_size;
} romfs_ctx_t;

auto write_padding(BufHelper& buf, u64 off, u64 block) -> u64 {
    const u64 size = block - (off % block);
    if (size) {
        std::vector<u8> padding(size);
        buf.write(padding.data(), padding.size());
    }
    return size;
}

auto romfs_get_direntry(romfs_dir *directories, u32 offset) -> romfs_dir* {
    return (romfs_dir*)((u8*)directories + offset);
}

auto romfs_get_fentry(romfs_file *files, u32 offset) -> romfs_file* {
    return (romfs_file*)((u8*)files + offset);
}

auto calc_path_hash(u32 parent, const u8 *path, u32 start, u32 path_len) -> u32 {
    u32 hash = parent ^ 123456789;
    for (u32 i = 0; i < path_len; i++) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }

    return hash;
}

auto align(u32 offset, u32 alignment) -> u32 {
    const u32 mask = ~(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

auto align64(u64 offset, u64 alignment) -> u64 {
    const u64 mask = ~(u64)(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

auto romfs_get_hash_table_count(u32 num_entries) -> u32 {
    if (num_entries < 3) {
        return 3;
    } else if (num_entries < 19) {
        return num_entries | 1;
    }

    u32 count = num_entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 || count % 17 == 0) {
        count++;
    }

    return count;
}

void romfs_visit_dir(const FileEntries& entries, romfs_dirent_ctx_t *parent, romfs_ctx_t *romfs_ctx) {
    romfs_dirent_ctx_t *child_dir_tree = NULL;
    romfs_fent_ctx_t *child_file_tree = NULL;
    romfs_fent_ctx_t *cur_file = NULL;

    for (auto& e : entries) {
        /* File */
        cur_file = (romfs_fent_ctx_t*)calloc(1, sizeof(romfs_fent_ctx_t));

        romfs_ctx->num_files++;

        cur_file->parent = parent;
        cur_file->size = e.data.size();

        romfs_ctx->file_table_size += sizeof(romfs_file) + align(e.name.length() - 1, 4);

        /* Ordered insertion on sibling */
        if (child_file_tree == NULL) {
            cur_file->sibling = child_file_tree;
            child_file_tree = cur_file;
        } else {
            romfs_fent_ctx_t *child, *prev;
            prev = child_file_tree;
            child = child_file_tree->sibling;
            prev->sibling = cur_file;
            cur_file->sibling = child;
        }

        /* Ordered insertion on next */
        if (romfs_ctx->files == NULL) {
            cur_file->next = romfs_ctx->files;
            romfs_ctx->files = cur_file;
        } else {
            romfs_fent_ctx_t *child, *prev;
            prev = romfs_ctx->files;
            child = romfs_ctx->files->next;
            prev->next = cur_file;
            cur_file->next = child;
        }

        cur_file = NULL;
    }

    parent->child = child_dir_tree;
    parent->file = child_file_tree;
}

void build_romfs_into_file(const FileEntries& entries, BufHelper& buf) {
    auto root_ctx = (romfs_dirent_ctx_t*)calloc(1, sizeof(romfs_dirent_ctx_t));
    root_ctx->parent = root_ctx;

    romfs_ctx_t romfs_ctx{};

    romfs_ctx.dir_table_size = sizeof(romfs_dir); /* Root directory. */
    romfs_ctx.num_dirs = 1;

    /* Visit all directories. */
    romfs_visit_dir(entries, root_ctx, &romfs_ctx);
    const u32 dir_hash_table_entry_count = romfs_get_hash_table_count(romfs_ctx.num_dirs);
    const u32 file_hash_table_entry_count = romfs_get_hash_table_count(romfs_ctx.num_files);
    romfs_ctx.dir_hash_table_size = 4 * dir_hash_table_entry_count;
    romfs_ctx.file_hash_table_size = 4 * file_hash_table_entry_count;

    romfs_header header{};
    romfs_fent_ctx_t *cur_file{};
    romfs_dirent_ctx_t *cur_dir{};
    u32 entry_offset{};

    std::vector<u32> dir_hash_table(dir_hash_table_entry_count, ROMFS_ENTRY_EMPTY);
    std::vector<u32> file_hash_table(file_hash_table_entry_count, ROMFS_ENTRY_EMPTY);

    auto dir_table = (romfs_dir*)calloc(1, romfs_ctx.dir_table_size);
    auto file_table = (romfs_file *)calloc(1, romfs_ctx.file_table_size);

    /* Determine file offsets. */
    cur_file = romfs_ctx.files;
    entry_offset = 0;
    for (auto& e : entries) {
        romfs_ctx.file_partition_size = align64(romfs_ctx.file_partition_size, 0x10);
        cur_file->offset = romfs_ctx.file_partition_size;
        romfs_ctx.file_partition_size += cur_file->size;
        cur_file->entry_offset = entry_offset;
        entry_offset += sizeof(romfs_file) + align(e.name.length() - 1, 4);
        cur_file = cur_file->next;
    }

    /* Determine dir offsets. */
    root_ctx->entry_offset = 0x0;

    /* Populate file tables. */
    cur_file = romfs_ctx.files;
    for (auto& e : entries) {
        auto cur_entry = romfs_get_fentry(file_table, cur_file->entry_offset);
        cur_entry->parent = (cur_file->parent->entry_offset);
        cur_entry->sibling = (cur_file->sibling == NULL ? ROMFS_ENTRY_EMPTY : cur_file->sibling->entry_offset);
        cur_entry->dataOff = (cur_file->offset);
        cur_entry->dataSize = (cur_file->size);

        const u32 name_size = e.name.length() - 1;
        const u32 hash = calc_path_hash(cur_file->parent->entry_offset, (const u8 *)e.name.c_str(), 1, name_size);
        cur_entry->nextHash = file_hash_table[hash % file_hash_table_entry_count];
        file_hash_table[hash % file_hash_table_entry_count] = (cur_file->entry_offset);

        cur_entry->nameLen = name_size;
        std::memcpy(cur_entry->name, e.name.c_str() + 1, name_size);

        cur_file = cur_file->next;
    }

    /* Populate dir tables. */
    cur_dir = root_ctx;

    while (cur_dir != NULL) {
        auto cur_entry = romfs_get_direntry(dir_table, cur_dir->entry_offset);
        cur_entry->parent = cur_dir->parent->entry_offset;
        cur_entry->sibling = cur_dir->sibling == NULL ? ROMFS_ENTRY_EMPTY : cur_dir->sibling->entry_offset;
        cur_entry->childDir = cur_dir->child == NULL ? ROMFS_ENTRY_EMPTY : cur_dir->child->entry_offset;
        cur_entry->childFile = cur_dir->file == NULL ? ROMFS_ENTRY_EMPTY : cur_dir->file->entry_offset;

        const auto hash = calc_path_hash(0, 0, 0, 0);
        cur_entry->nextHash = dir_hash_table[hash % dir_hash_table_entry_count];
        dir_hash_table[hash % dir_hash_table_entry_count] = (cur_dir->entry_offset);

        cur_entry->nameLen = 0;

        auto temp = cur_dir;
        cur_dir = cur_dir->next;
        free(temp);
    }

    header.headerSize = sizeof(header);
    header.fileHashTableSize = romfs_ctx.file_hash_table_size;
    header.fileTableSize = romfs_ctx.file_table_size;
    header.dirHashTableSize = romfs_ctx.dir_hash_table_size;
    header.dirTableSize = romfs_ctx.dir_table_size;
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;

    header.dirHashTableOff = align64(romfs_ctx.file_partition_size + ROMFS_FILEPARTITION_OFS, 4);
    header.dirTableOff = header.dirHashTableOff + romfs_ctx.dir_hash_table_size;
    header.fileHashTableOff = header.dirTableOff + romfs_ctx.dir_table_size;
    header.fileTableOff = header.fileHashTableOff + romfs_ctx.file_hash_table_size;

    buf.write(&header, sizeof(header));

    /* Write files. */
    cur_file = romfs_ctx.files;
    for (auto&e : entries) {
        buf.seek(cur_file->offset + ROMFS_FILEPARTITION_OFS);
        buf.write(e.data.data(), e.data.size());

        auto temp = cur_file;
        cur_file = cur_file->next;
        free(temp);
    }

    buf.seek(header.dirHashTableOff);
    buf.write(dir_hash_table.data(), romfs_ctx.dir_hash_table_size);

    buf.write(dir_table, romfs_ctx.dir_table_size);
    free(dir_table);

    buf.write(file_hash_table.data(), romfs_ctx.file_hash_table_size);

    buf.write(file_table, romfs_ctx.file_table_size);
    free(file_table);
}

auto romfs_build(const FileEntries& entries, u64 *out_size) -> std::vector<u8> {
    BufHelper buf;

    build_romfs_into_file(entries, buf);

    // Write Padding
    buf.seek(buf.buf.size());
    *out_size = buf.tell();
    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);

    return buf.buf;
}

auto npdm_patch_kc(std::vector<u8>& npdm, u32 off, u32 size, u32 bitmask, u32 value) -> bool {
    const u32 pattern = BIT(bitmask) - 1;
    const u32 mask = BIT(bitmask) | pattern;

    for (u32 i = 0; i < size; i += 4) {
        u32 cup;
        std::memcpy(&cup, npdm.data() + off + i, sizeof(cup));
        if ((cup & mask) == pattern) {
            cup = value | pattern;
            std::memcpy(npdm.data() + off + i, &cup, sizeof(cup));
            return true;
        }
    }

    return false;
}

constexpr auto npdm_kernel_flags(CpuCoreMode mode) -> u32 {
    const u32 lowest_priority = mode == CpuCoreMode::Four ? 63 : 59;
    const u32 highest_priority = 28;
    const u32 lowest_cpu = 0;
    const u32 highest_cpu = mode == CpuCoreMode::Four ? 3 : 2;
    const u32 descriptor = (((highest_cpu << 8) | lowest_cpu) << 6 | highest_priority) << 6 | lowest_priority;
    return descriptor << 4;
}

static_assert((npdm_kernel_flags(CpuCoreMode::Three) | 0x7) == 0x020073B7);
static_assert((npdm_kernel_flags(CpuCoreMode::Four) | 0x7) == 0x030073F7);

// todo: manually build npdm
auto patch_npdm(std::vector<u8>& npdm, const NpdmPatch& patch) -> bool {
    constexpr u8 ADDRESS_SPACE_SHIFT = 1;
    constexpr u8 ADDRESS_SPACE_MASK = 0x7 << ADDRESS_SPACE_SHIFT;

    npdm::Meta meta{};
    npdm::Aci0 aci0{};
    npdm::Acid acid{};
    std::memcpy(&meta, npdm.data(), sizeof(meta));
    std::memcpy(&aci0, npdm.data() + meta.aci0_offset, sizeof(aci0));
    std::memcpy(&acid, npdm.data() + meta.acid_offset, sizeof(acid));

    // apply patch
    std::memcpy(meta.title_name, &patch.title_name, sizeof(meta.title_name));
    std::memcpy(meta.product_code, &patch.product_code, sizeof(patch.product_code));
    meta.flags = (meta.flags & ~ADDRESS_SPACE_MASK) | (static_cast<u8>(patch.address_space) << ADDRESS_SPACE_SHIFT);
    aci0.program_id = patch.tid;
    acid.program_id_min = patch.tid;
    acid.program_id_max = patch.tid;

    const auto kernel_flags = npdm_kernel_flags(patch.core_mode);
    const auto aci0_core_patched = npdm_patch_kc(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 3, kernel_flags);
    const auto acid_core_patched = npdm_patch_kc(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 3, kernel_flags);

    const auto enable_svc_debug = [&patch](){
        if (patch.svc_debug_mode == ForwarderSvcDebugMode::Enabled) {
            return true;
        }
        if (patch.svc_debug_mode == ForwarderSvcDebugMode::Disabled) {
            return false;
        }

        // Atmosphere 1.8.0 updated the NPDM MiscFlags layout for HOS 19.0.0.
        // Preserve Sphaira's original behavior by selecting the new ForceDebug
        // bit automatically when that version (or newer) is detected.
        u64 ver{};
        if (R_FAILED(splInitialize())) {
            return false;
        }
        ON_SCOPE_EXIT(splExit());

        const auto SplConfigItem_ExosphereVersion = (SplConfigItem)65000;
        if (R_FAILED(splGetConfig(SplConfigItem_ExosphereVersion, &ver))) {
            return false;
        }
        ver >>= 40;
        return ver >= MAKEHOSVERSION(1,8,0);
    }();

    if (enable_svc_debug) {
        npdm_patch_kc(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT(19));
        npdm_patch_kc(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT(19));
    }

    // must be applied after the svc debug block above, as npdm_patch_kc
    // rewrites the whole DebugFlags capability and only one of the three
    // debug flag bits may be set (HOS 19+ validation).
    // BIT(17) is AllowDebug, BIT(19) is ForceDebug.
    bool aci0_allow_debug_patched = !patch.allow_debug;
    bool acid_allow_debug_patched = !patch.allow_debug;
    if (patch.allow_debug) {
        aci0_allow_debug_patched = npdm_patch_kc(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT(17));
        acid_allow_debug_patched = npdm_patch_kc(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT(17));
        log_write("[owo] AllowDebug NPDM patch ACI0:%u ACID:%u\n", aci0_allow_debug_patched, acid_allow_debug_patched);
    }

    std::memcpy(npdm.data(), &meta, sizeof(meta));
    std::memcpy(npdm.data() + meta.aci0_offset, &aci0, sizeof(aci0));
    std::memcpy(npdm.data() + meta.acid_offset, &acid, sizeof(acid));
    return aci0_core_patched && acid_core_patched && aci0_allow_debug_patched && acid_allow_debug_patched;
}

void patch_nacp(NacpStruct& nacp, const NcapPatch& patch) {
    // patch title
    if (!patch.name.empty()) {
        for (auto& lang : NacpLanguageEntries(nacp)) {
            std::strncpy(lang.name, patch.name.c_str(), sizeof(lang.name)-1);
        }
    }

    // patch author
    if (!patch.author.empty()) {
        for (auto& lang : NacpLanguageEntries(nacp)) {
            std::strncpy(lang.author, patch.author.c_str(), sizeof(lang.author)-1);
        }
    }

    // misc
    nacp.startup_user_account = patch.profile_selection ? 0x01 : 0x00;
    nacp.user_account_switch_lock = 0x00; // allow account switch
    nacp.add_on_content_registration_type = 0x01; // on demand
    nacp.screenshot = patch.screenshot ? 0x0 : 0x1;
    nacp.video_capture = patch.video_capture ? 0x2 : 0x0; // auto or disabled
    nacp.logo_type = 0x2; // Nintendo
    nacp.logo_handling = 0x0; // auto
    nacp.data_loss_confirmation = 0x0; // disable as we don't use saves
    nacp.required_network_service_license_on_launch = 0x0; // don't require linked account
    const char error_code[] = "sphaira";
    static_assert(sizeof(error_code) <= 9);
    nacp.application_error_code_category = 0; // this is actually a char[8], not a u64 :)
    std::memcpy(&nacp.application_error_code_category, error_code, sizeof(error_code)-1);

    // update tid
    nacp.presence_group_id = patch.tid;
    nacp.save_data_owner_id = patch.tid;
    nacp.pseudo_device_id_seed = patch.tid;
    nacp.add_on_content_base_id = patch.tid ^ 0x1000;
    for (auto& id : nacp.local_communication_id) {
        id = patch.tid;
    }

    // enable play logging
    nacp.play_log_policy = 0x0; // open
    nacp.play_log_query_capability = 0x0;

    // disable save creation
    nacp.user_account_save_data_size = 0x0;
    nacp.user_account_save_data_journal_size = 0x0;
    nacp.device_save_data_size = 0x0;
    nacp.device_save_data_journal_size = 0x0;
    nacp.user_account_save_data_size_max = 0x0;
    nacp.user_account_save_data_journal_size_max = 0x0;
    nacp.device_save_data_size_max = 0x0;
    nacp.device_save_data_journal_size_max = 0x0;
}

void add_file_entry(FileEntries& entries, const char* name, const void* data, u64 size) {
    FileEntry entry;
    entry.name = name;
    entry.data.resize(size);
    std::memcpy(entry.data.data(), data, size);
    entries.emplace_back(entry);
}

void add_file_entry(FileEntries& entries, const char* name, std::span<const u8> data) {
    add_file_entry(entries, name, data.data(), data.size());
}

auto build_ivfc_master_hash(std::span<const u8> level1) -> std::vector<u8> {
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), level1.data(), level1.size());
    return hash;
}

auto build_pfs0(const FileEntries& entries) -> std::vector<u8> {
    BufHelper buf;

    Pfs0Header header{};
    std::vector<Pfs0FileTable> file_table(entries.size());
    std::vector<char> string_table;

    u64 string_offset{};
    u64 data_offset{};

    for (u32 i = 0; i < entries.size(); i++) {
        file_table[i].data_offset = data_offset;
        file_table[i].data_size = entries[i].data.size();
        file_table[i].name_offset = string_offset;
        file_table[i].padding = 0;

        string_table.resize(string_offset + entries[i].name.length() + 1);
        std::memcpy(string_table.data() + string_offset, entries[i].name.c_str(), entries[i].name.length() + 1);

        data_offset += entries[i].data.size();
        string_offset += entries[i].name.length() + 1;
    }

    // align table
    string_table.resize((string_table.size() + 0x1F) & ~0x1F);

    header.magic = 0x30534650;
    header.total_files = entries.size();
    header.string_table_size = string_table.size();
    header.padding = 0;

    buf.write(&header, sizeof(header));
    buf.write(file_table.data(), sizeof(Pfs0FileTable) * file_table.size());
    buf.write(string_table.data(), string_table.size());

    for (const auto&e : entries) {
        buf.write(e.data.data(), e.data.size());
    }

    return buf.buf;
}

auto build_pfs0_hash_table(const std::vector<u8>& pfs0, u32 block_size) -> std::vector<u8> {
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u32 read_size = block_size;

    for (u32 i = 0; i < pfs0.size(); i += read_size) {
        if (i + read_size >= pfs0.size()) {
            read_size = pfs0.size() - i;
        }
        sha256CalculateHash(hash, pfs0.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }

    return buf.buf;
}

auto build_pfs0_master_hash(const std::vector<u8>& pfs0_hash_table) -> std::vector<u8> {
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), pfs0_hash_table.data(), pfs0_hash_table.size());
    return hash;
}

void write_nca_padding(BufHelper& buf) {
    write_padding(buf, buf.tell(), 0x200);
}

void nca_encrypt_header(nca::Header* header, std::span<const u8> key) {
    Aes128XtsContext ctx{};
    aes128XtsContextCreate(&ctx, key.data(), key.data() + 0x10, true);

    u8 sector{};
    for (u64 pos = 0; pos < 0xC00; pos += 0x200) {
        aes128XtsContextResetSector(&ctx, sector++, true);
        aes128XtsEncrypt(&ctx, (u8*)header + pos, (const u8*)header + pos, 0x200);
    }
}

void write_nca_section(nca::Header& nca_header, u8 index, u64 start, u64 end) {
    auto& section = nca_header.fs_table[index];
    section.media_start_offset = start / 0x200; // 0xC00 / 0x200
    section.media_end_offset = end / 0x200; // Section end offset / 200
    section._0x8[0] = 0x1; // Always 1
}

void write_nca_fs_header_pfs0(nca::Header& nca_header, u8 index, const std::vector<u8>& master_hash, u64 hash_table_size, u32 block_size) {
    auto& fs_header = nca_header.fs_header[index];
    fs_header.hash_type = nca::HashType_HierarchicalSha256;
    fs_header.fs_type = nca::FileSystemType_PFS0;
    fs_header.version = 0x2; // Always 2
    fs_header.hash_data.hierarchical_sha256_data.layer_count = 0x2;
    fs_header.hash_data.hierarchical_sha256_data.block_size = block_size;
    fs_header.encryption_type = nca::EncryptionType_None;
    fs_header.hash_data.hierarchical_sha256_data.hash_layer.size = hash_table_size;
    std::memcpy(fs_header.hash_data.hierarchical_sha256_data.master_hash, master_hash.data(), master_hash.size());
    sha256CalculateHash(&nca_header.fs_header_hash[index], &fs_header, sizeof(fs_header));
}

void write_nca_fs_header_romfs(nca::Header& nca_header, u8 index) {
    auto& fs_header = nca_header.fs_header[index];
    fs_header.hash_type = nca::HashType_HierarchicalIntegrity;
    fs_header.fs_type = nca::FileSystemType_RomFS;
    fs_header.version = 0x2; // Always 2
    fs_header.hash_data.integrity_meta_info.magic = 0x43465649;
    fs_header.hash_data.integrity_meta_info.version = 0x20000; // Always 0x20000
    fs_header.hash_data.integrity_meta_info.master_hash_size = SHA256_HASH_SIZE;
    fs_header.hash_data.integrity_meta_info.info_level_hash.max_layers = 0x7;
    fs_header.encryption_type = nca::EncryptionType_None;
    fs_header.hash_data.integrity_meta_info.info_level_hash.levels[5].block_size = 0x0E; // 0x4000
    sha256CalculateHash(&nca_header.fs_header_hash[index], &fs_header, sizeof(fs_header));
}

void write_nca_pfs0(nca::Header& nca_header, u8 index, const FileEntries& entries, u32 block_size, BufHelper& buf) {
    const auto pfs0 = build_pfs0(entries);
    const auto pfs0_hash_table = build_pfs0_hash_table(pfs0, block_size);
    const auto pfs0_master_hash = build_pfs0_master_hash(pfs0_hash_table);

    buf.write(pfs0_hash_table.data(), pfs0_hash_table.size());
    const auto padding_size = write_padding(buf, pfs0_hash_table.size(), PFS0_PADDING_SIZE);

    nca_header.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.offset = pfs0_hash_table.size() + padding_size;
    nca_header.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.size = pfs0.size();

    buf.write(pfs0.data(), pfs0.size());
    write_nca_padding(buf);

    const auto section_start = index == 0 ? sizeof(nca_header) : nca_header.fs_table[index-1].media_end_offset * 0x200;
    write_nca_section(nca_header, index, section_start, buf.tell());
    write_nca_fs_header_pfs0(nca_header, index, pfs0_master_hash, pfs0_hash_table.size(), block_size);
}

auto ivfc_create_level(const std::vector<u8>& src) -> std::vector<u8> {
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u64 read_size = IVFC_HASH_BLOCK_SIZE;

    for (u32 i = 0; i < src.size(); i += read_size) {
        if (i + read_size >= src.size()) {
            read_size = src.size() - i;
        }
        sha256CalculateHash(hash, src.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }

    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);

    return buf.buf;
}

void write_nca_romfs(nca::Header& nca_header, u8 index, const FileEntries& entries, u32 block_size, BufHelper& buf) {
    auto& fs_header = nca_header.fs_header[index];
    auto& meta_info = fs_header.hash_data.integrity_meta_info;
    auto& info_level_hash = meta_info.info_level_hash;

    std::vector<u8> ivfc[IVFC_MAX_LEVEL];

    ivfc[5] = romfs_build(entries, &info_level_hash.levels[5].hash_data_size);

    for (int b = 4; b >= 0; b--) {
        ivfc[b] = ivfc_create_level(ivfc[b + 1]);
        info_level_hash.levels[b].hash_data_size = ivfc[b].size();
        info_level_hash.levels[b].block_size = 0x0E; // 0x4000
    }

    info_level_hash.levels[0].logical_offset = 0;
    for (int i = 1; i <= 5; i++) {
        info_level_hash.levels[i].logical_offset = info_level_hash.levels[i - 1].logical_offset + info_level_hash.levels[i - 1].hash_data_size;
    }

    for (const auto& iv : ivfc) {
        buf.write(iv.data(), iv.size());
    }

    write_nca_padding(buf);

    const auto ivfc_master_hash = build_ivfc_master_hash(ivfc[0]);
    std::memcpy(meta_info.master_hash, ivfc_master_hash.data(), sizeof(meta_info.master_hash));

    const auto section_start = index == 0 ? sizeof(nca_header) : nca_header.fs_table[index-1].media_end_offset * 0x200;
    write_nca_section(nca_header, index, section_start, buf.tell());
    write_nca_fs_header_romfs(nca_header, index);
}

void write_nca_header_encypted(nca::Header& nca_header, u64 tid, const keys::Keys& keys, nca::ContentType type, BufHelper& buf) {
    nca_header.magic = NCA3_MAGIC;
    nca_header.distribution_type = nca::DistributionType_System;
    nca_header.content_type = type;
    nca_header.program_id = tid;
    nca_header.sdk_version = 0x000C1100;
    nca_header.size = buf.tell();

    nca_encrypt_header(&nca_header, keys.header_key);
    buf.seek(0);
    buf.write(&nca_header, sizeof(nca_header));
}

auto create_program_nca(u64 tid, const keys::Keys& keys, const FileEntries& exefs, const FileEntries& romfs, const FileEntries& logo) -> NcaEntry {
    BufHelper buf;
    nca::Header nca_header{};
    buf.write(&nca_header, sizeof(nca_header));

    write_nca_pfs0(nca_header, 0, exefs, PFS0_EXEFS_HASH_BLOCK_SIZE, buf);
    write_nca_romfs(nca_header, 1, romfs, IVFC_HASH_BLOCK_SIZE, buf);
    // only write logo if set (can only 1 file be added?)
    if (logo.size() == 2 && !logo[0].data.empty() && !logo[1].data.empty()) {
        write_nca_pfs0(nca_header, 2, logo, PFS0_LOGO_HASH_BLOCK_SIZE, buf);
    }
    write_nca_header_encypted(nca_header, tid, keys, nca::ContentType_Program, buf);

    return {buf, NcmContentType_Program};
}

auto create_control_nca(u64 tid, const keys::Keys& keys, const FileEntries& romfs) -> NcaEntry{
    nca::Header nca_header{};
    BufHelper buf;
    buf.write(&nca_header, sizeof(nca_header));

    write_nca_romfs(nca_header, 0, romfs, IVFC_HASH_BLOCK_SIZE, buf);
    write_nca_header_encypted(nca_header, tid, keys, nca::ContentType_Control, buf);

    return {buf, NcmContentType_Control};
}

auto create_meta_nca(u64 tid, const keys::Keys& keys, NcmStorageId storage_id, const std::vector<NcaEntry>& ncas) -> NcaMetaEntry {
    CnmtHeader cnmt_header{};
    NcmApplicationMetaExtendedHeader cnmt_extended{};
    NcmPackagedContentInfo packaged_content_info[2]{};
    u8 digest[0x20]{};
    BufHelper buf;

    cnmt_header.title_id = tid;
    cnmt_header.title_version = 0; // todo: parse nacp.disaply_version
    cnmt_header.meta_type = NcmContentMetaType_Application;
    cnmt_header.meta_header.extended_header_size = sizeof(cnmt_extended);
    cnmt_header.meta_header.content_count = 0x2; // program + control
    cnmt_header.meta_header.content_meta_count = 0x1; // only 1 meta
    cnmt_header.meta_header.attributes = 0x0;
    cnmt_header.meta_header.storage_id = storage_id;
    cnmt_extended.patch_id = cnmt_header.title_id | 0x800;

    for (u32 i = 0; i < ncas.size(); i++) {
        std::memcpy(packaged_content_info[i].hash, ncas[i].hash, sizeof(packaged_content_info[i].hash));
        std::memcpy(&packaged_content_info[i].info.content_id, ncas[i].hash, sizeof(packaged_content_info[i].info.content_id));
        packaged_content_info[i].info.content_type = ncas[i].type;
        ncmU64ToContentInfoSize(ncas[i].data.size(), &packaged_content_info[i].info);
    }

    // create control
    BufHelper cnmt_buf;
    cnmt_buf.write(&cnmt_header, sizeof(cnmt_header));
    cnmt_buf.write(&cnmt_extended, sizeof(cnmt_extended));
    cnmt_buf.write(&packaged_content_info, sizeof(packaged_content_info));
    cnmt_buf.write(digest, sizeof(digest));

    FileEntries cnmt;
    char cnmt_name[34];
    std::snprintf(cnmt_name, sizeof(cnmt_name), "Application_%016lX.cnmt", tid);
    add_file_entry(cnmt, cnmt_name, cnmt_buf.buf.data(), cnmt_buf.buf.size());

    nca::Header nca_header{};
    buf.write(&nca_header, sizeof(nca_header));
    write_nca_pfs0(nca_header, 0, cnmt, PFS0_META_HASH_BLOCK_SIZE, buf);
    write_nca_header_encypted(nca_header, tid, keys, nca::ContentType_Meta, buf);

    // entry
    NcaMetaEntry entry{buf, NcmContentType_Meta};

    // header
    entry.content_meta_header = cnmt_header.meta_header;
    entry.content_meta_header.content_count++;
    entry.content_meta_header.storage_id = 0;

    // key
    entry.content_meta_key.id = cnmt_header.title_id;
    entry.content_meta_key.version = cnmt_header.title_version;
    entry.content_meta_key.type = cnmt_header.meta_type;
    entry.content_meta_key.install_type = NcmContentInstallType_Full;
    std::memset(entry.content_meta_key.padding, 0, sizeof(entry.content_meta_key.padding));

    // record
    entry.content_storage_record.key = entry.content_meta_key;
    entry.content_storage_record.storage_id = storage_id;
    std::memset(entry.content_storage_record.padding, 0, sizeof(entry.content_storage_record.padding));

    // data
    entry.content_meta_data.header = entry.content_meta_header;
    entry.content_meta_data.extended = cnmt_extended;

    // meta content info
    std::memcpy(&entry.content_meta_data.infos[0].content_id, entry.nca_entry.hash, sizeof(entry.content_meta_data.infos[0].content_id));
    entry.content_meta_data.infos[0].content_type = entry.nca_entry.type;
    entry.content_meta_data.infos[0].attr = 0;
    ncmU64ToContentInfoSize(cnmt_buf.buf.size(), &entry.content_meta_data.infos[0]);
    entry.content_meta_data.infos[0].id_offset = 0;

    // program + control content info
    entry.content_meta_data.infos[1] = packaged_content_info[0].info;
    entry.content_meta_data.infos[2] = packaged_content_info[1].info;

    return entry;
}

auto install_forwader_internal(ui::ProgressBox* pbox, OwoConfig& config, NcmStorageId storage_id) -> Result {
    pbox->SetTitle(config.name);
    pbox->SetImageDataConst(config.icon);

    R_UNLESS(!config.nro_path.empty(), Result_OwoBadArgs);
    R_UNLESS(!config.icon.empty(), Result_OwoBadArgs);

    R_TRY(splCryptoInitialize());
    ON_SCOPE_EXIT(splCryptoExit());

    R_TRY(ncmInitialize());
    ON_SCOPE_EXIT(ncmExit());

    R_TRY(ns::Initialize());
    ON_SCOPE_EXIT(ns::Exit());

    keys::Keys keys;
    R_TRY(keys::parse_keys(keys, false));

    // fix args to include nro path
    if (config.args.empty()) {
        config.args = config.nro_path;
    } else {
        config.args = config.nro_path + ' ' + config.args;
    }

    // create tid by using a hash over path + args. A non-default address
    // space is part of the hash, so forwarders for the same NRO that differ
    // only in address space (eg. a 32-bit and a 39-bit launcher entry) get
    // different title ids and coexist instead of overwriting each other.
    // The default 36-bit space keeps the historical unsalted hash, so the
    // title ids of forwarders installed by earlier builds do not change.
    auto hash_path = config.nro_path + config.args;
    if (config.address_space != ForwarderAddressSpace::Bit36) {
        hash_path += "\naddress-space=" + std::to_string(static_cast<unsigned>(config.address_space)) + "\nsphaira-forwarder=1";
    }
    const auto hash = forwarder_path_hash(hash_path);

    // A native-game forwarder gets a retail-range title id and AllowDebug
    // so SaltyNX (NX-FPS / FPSLocker) can attach to its process. SaltyNX
    // has no force-debug permission, so svcDebugActiveProcess fails with
    // 0xfa01 (kernel 125, InvalidState) for the normal homebrew 0x05 range.
    // Automatic keeps the existing built-in opt-in for NetherSX2, while
    // Enabled/Disabled let the user force the choice for any emulator or
    // other homebrew in the forwarder editor.
    const bool detected_nethersx2 = is_nethersx2_nro_path(config.nro_path);
    const bool native_game = config.native_game_mode == ForwarderNativeGameMode::Enabled
        || (config.native_game_mode == ForwarderNativeGameMode::Automatic && detected_nethersx2);
    const u64 tid = native_game ? native_game_forwarder_tid(hash) : homebrew_forwarder_tid(hash);

    // Reinstalling under a different mode changes the title id. Remove the
    // other representations Sphaira may have used for this same
    // path+args+address space, otherwise duplicate HOME icons can remain
    // and an old copy without the requested title-id/debug settings may be
    // launched. Forwarders hashed with a different address space are
    // separate entries and are deliberately kept. Also cover older
    // installs whose hash input did not include the sdmc: prefix.
    std::vector<u64> legacy_tids;
    append_forwarder_tid_candidates(legacy_tids, hash);
    const auto hash_path_without_sdmc = remove_sdmc_prefixes(hash_path);
    if (hash_path_without_sdmc != hash_path) {
        append_forwarder_tid_candidates(legacy_tids, forwarder_path_hash(hash_path_without_sdmc));
    }

    log_write("[owo] forwarder path:%s args:%s address_space:%u native_game_mode:%u detected_nethersx2:%u native_game:%u tid:%016lX\n",
        config.nro_path.c_str(), config.args.c_str(), static_cast<unsigned>(config.address_space),
        static_cast<unsigned>(config.native_game_mode), detected_nethersx2, native_game, tid);

    std::vector<NcaEntry> nca_entries;

    // create program
    if (config.program_nca.empty()) {
        pbox->NewTransfer("Creating Program"_i18n).UpdateTransfer(0, 8);
        FileEntries exefs;
        add_file_entry(exefs, "main", HBL_MAIN_DATA);
        add_file_entry(exefs, "main.npdm", HBL_NPDM_DATA);

        FileEntries romfs;
        add_file_entry(romfs, "/nextArgv", config.args.data(), config.args.length());
        add_file_entry(romfs, "/nextNroPath", config.nro_path.data(), config.nro_path.length());

        FileEntries logo;
        if (!config.logo.empty()) {
            add_file_entry(logo, "NintendoLogo.png", config.logo);
        }
        if (!config.gif.empty()) {
            add_file_entry(logo, "StartupMovie.gif", config.gif);
        }

        NpdmPatch npdm_patch;
        npdm_patch.tid = tid;
        npdm_patch.address_space = config.address_space;
        npdm_patch.core_mode = config.core_mode;
        npdm_patch.allow_debug = native_game;
        npdm_patch.svc_debug_mode = native_game ? ForwarderSvcDebugMode::Disabled : config.svc_debug_mode;
        R_UNLESS(patch_npdm(exefs[1].data, npdm_patch), Result_CoreUnavailable);

        nca_entries.emplace_back(
            create_program_nca(tid, keys, exefs, romfs, logo)
        );
    } else {
        nca_entries.emplace_back(
            BufHelper{config.program_nca}, NcmContentType_Program
        );
    }

    // create control
    {
        pbox->NewTransfer("Creating Control"_i18n).UpdateTransfer(1, 8);
        // patch nacp
        NcapPatch nacp_patch{};
        nacp_patch.tid = tid;
        nacp_patch.name = config.name;
        nacp_patch.author = config.author;
        nacp_patch.profile_selection = config.profile_selection;
        nacp_patch.screenshot = config.screenshot;
        nacp_patch.video_capture = config.video_capture;
        patch_nacp(config.nacp, nacp_patch);

        // Mark non-default address spaces on the icon itself, so multiple
        // forwarders for the same NRO can be told apart on the HOME menu.
        auto icon = config.icon;
        if (const auto label = address_space_icon_label(config.address_space); !label.empty()) {
            if (auto labelled = ImageIconWithLabel(icon, label); !labelled.empty()) {
                icon = std::move(labelled);
            } else {
                log_write("[owo] failed to draw address space label onto icon\n");
            }
        }

        FileEntries romfs;
        add_file_entry(romfs, "/control.nacp", &config.nacp, sizeof(config.nacp));
        add_file_entry(romfs, "/icon_AmericanEnglish.dat", icon);

        nca_entries.emplace_back(
            create_control_nca(tid, keys, romfs)
        );
    }

    // create meta
    NcmContentMetaHeader content_meta_header;
    NcmContentMetaKey content_meta_key;
    ncm::ContentStorageRecord content_storage_record;
    NcmContentMetaData content_meta_data;
    {
        pbox->NewTransfer("Creating Meta"_i18n).UpdateTransfer(2, 8);
        const auto meta_entry = create_meta_nca(tid, keys, storage_id, nca_entries);

        nca_entries.emplace_back(meta_entry.nca_entry);
        content_meta_header = meta_entry.content_meta_header;
        content_meta_key = meta_entry.content_meta_key;
        content_storage_record = meta_entry.content_storage_record;
        content_meta_data = meta_entry.content_meta_data;
    }

    // write ncas
    {
        NcmContentStorage cs;
        R_TRY(ncmOpenContentStorage(&cs, storage_id));
        ON_SCOPE_EXIT(ncmContentStorageClose(&cs));

        for (const auto& nca : nca_entries) {
            pbox->NewTransfer("Writing Nca"_i18n).UpdateTransfer(3, 8);
            NcmContentId content_id;
            NcmPlaceHolderId placeholder_id;
            std::memcpy(&content_id, nca.hash, sizeof(content_id));
            R_TRY(ncmContentStorageGeneratePlaceHolderId(&cs, &placeholder_id));
            ncmContentStorageDeletePlaceHolder(&cs, &placeholder_id);
            R_TRY(ncmContentStorageCreatePlaceHolder(&cs, &content_id, &placeholder_id, nca.data.size()));
            R_TRY(ncmContentStorageWritePlaceHolder(&cs, &placeholder_id, 0, nca.data.data(), nca.data.size()));
            ncmContentStorageDelete(&cs, &content_id);
            R_TRY(ncmContentStorageRegister(&cs, &content_id, &placeholder_id));
        }
    }

    // setup database
    {
        pbox->NewTransfer("Updating ncm database"_i18n).UpdateTransfer(4, 8);
        NcmContentMetaDatabase db;
        R_TRY(ncmOpenContentMetaDatabase(&db, storage_id));
        ON_SCOPE_EXIT(ncmContentMetaDatabaseClose(&db));

        R_TRY(ncmContentMetaDatabaseSet(&db, &content_meta_key, &content_meta_data, sizeof(content_meta_data)));
        R_TRY(ncmContentMetaDatabaseCommit(&db));
    }

    // push record
    {
        pbox->NewTransfer("Pushing application record"_i18n).UpdateTransfer(5, 8);

        // remove old ids for forwarders.
        bool failed_to_remove_old{};
        for (u32 i = 0; i < legacy_tids.size(); i++) {
            const auto legacy_tid = legacy_tids[i];
            if (legacy_tid == tid) {
                continue;
            }

            bool duplicate{};
            for (u32 j = 0; j < i; j++) {
                duplicate |= legacy_tids[j] == legacy_tid;
            }
            if (duplicate) {
                continue;
            }

            const auto rc = nsDeleteApplicationCompletely(legacy_tid);
            log_write("[owo] remove legacy forwarder tid:%016lX rc:0x%X\n", legacy_tid, rc);
            if (R_FAILED(rc) && rc != 0x410) { // not found
                failed_to_remove_old = true;
            }
            ns::InvalidateApplicationControlCache(legacy_tid);
        }
        if (failed_to_remove_old) {
            App::Notify("Failed to remove old forwarder, please manually remove it!"_i18n);
        }

        // remove previous ncas.
        nsDeleteApplicationEntity(tid);

        R_TRY(ns::PushApplicationRecord(tid, &content_storage_record, 1));

        // force flush.
        ns::InvalidateApplicationControlCache(tid);
    }

    R_SUCCEED();
}

} // namespace

auto prepare_core_launch(const std::string& nro_path, const std::string& args, CpuCoreMode core_mode) -> Result {
    R_UNLESS(App::CanSetCpuCores(), Result_CoreUnavailable);
    R_UNLESS(!nro_path.empty() && !args.empty(), Result_OwoBadArgs);

    u64 program_id{};
    R_TRY(svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0));
    R_UNLESS(program_id, Result_CoreUnavailable);

    R_TRY(splCryptoInitialize());
    ON_SCOPE_EXIT(splCryptoExit());

    keys::Keys keys;
    R_TRY(keys::parse_keys(keys, false));

    FileEntries exefs;
    add_file_entry(exefs, "main", HBL_MAIN_DATA);
    add_file_entry(exefs, "main.npdm", HBL_NPDM_DATA);

    NpdmPatch npdm_patch;
    npdm_patch.tid = program_id;
    npdm_patch.core_mode = core_mode;
    R_UNLESS(patch_npdm(exefs[1].data, npdm_patch), Result_CoreUnavailable);

    auto return_path = App::GetExePath().toString();
    if (!return_path.starts_with("sdmc:")) {
        return_path = "sdmc:" + return_path;
    }
    const auto return_args = nro_add_arg(return_path);

    R_TRY(lrInitialize());
    ON_SCOPE_EXIT(lrExit());

    LrLocationResolver resolver{};
    NcmStorageId redirect_storage = NcmStorageId_None;
    char original_path[FS_MAX_PATH]{};
    constexpr NcmStorageId storage_ids[]{
        NcmStorageId_SdCard,
        NcmStorageId_BuiltInUser,
        NcmStorageId_GameCard,
        NcmStorageId_BuiltInSystem,
    };
    for (const auto storage_id : storage_ids) {
        LrLocationResolver candidate{};
        if (R_SUCCEEDED(lrOpenLocationResolver(storage_id, &candidate))) {
            if (R_SUCCEEDED(lrLrResolveProgramPath(&candidate, program_id, original_path))) {
                resolver = candidate;
                redirect_storage = storage_id;
                break;
            }
            serviceClose(&candidate.s);
        }
    }
    R_UNLESS(redirect_storage != NcmStorageId_None, Result_CoreUnavailable);
    ON_SCOPE_EXIT(serviceClose(&resolver.s));

    constexpr const char* nca_sd_path = "/config/sphaira/core-launch/program.nca";
    const std::string nca_loader_path = "@Sdcard:" + std::string{nca_sd_path};

    FileEntries romfs;
    add_file_entry(romfs, "/nextArgv", args.data(), args.size());
    add_file_entry(romfs, "/nextNroPath", nro_path.data(), nro_path.size());
    add_file_entry(romfs, "/returnArgv", return_args.data(), return_args.size());
    add_file_entry(romfs, "/returnNroPath", return_path.data(), return_path.size());
    add_file_entry(romfs, "/redirectStorage", &redirect_storage, sizeof(redirect_storage));
    add_file_entry(romfs, "/redirectProgramId", &program_id, sizeof(program_id));
    add_file_entry(romfs, "/redirectPath", original_path, std::strlen(original_path));
    const auto program = create_program_nca(program_id, keys, exefs, romfs, {});
    fs::FsNativeSd sd;
    R_TRY(sd.CreateDirectoryRecursively("/config/sphaira/core-launch"));
    R_TRY(sd.write_entire_file(nca_sd_path, program.data));
    R_TRY(sd.Commit());

    R_TRY(lrLrRedirectProgramPath(&resolver, program_id, nca_loader_path.c_str()));

    char redirected_path[FS_MAX_PATH]{};
    const auto resolve_result = lrLrResolveProgramPath(&resolver, program_id, redirected_path);
    if (R_FAILED(resolve_result) || nca_loader_path != redirected_path) {
        lrLrRedirectProgramPath(&resolver, program_id, original_path);
        return R_FAILED(resolve_result) ? resolve_result : Result_CoreUnavailable;
    }

    const auto launch_result = appletRequestLaunchApplication(0, nullptr);
    if (R_FAILED(launch_result)) {
        lrLrRedirectProgramPath(&resolver, program_id, original_path);
        return launch_result;
    }

    g_core_launch_pending = true;
    R_SUCCEED();
}

auto core_launch_pending() -> bool {
    return g_core_launch_pending;
}

auto install_forwarder(ui::ProgressBox* pbox, OwoConfig& config, NcmStorageId storage_id) -> Result {
    return install_forwader_internal(pbox, config, storage_id);
}

auto install_forwarder(OwoConfig& config, NcmStorageId storage_id) -> Result {
    App::Push<ui::ProgressBox>(0, "Installing Forwarder"_i18n, config.name, [config, storage_id](auto pbox) mutable -> Result {
        return install_forwarder(pbox, config, storage_id);
    });
    R_SUCCEED();
}

} // namespace sphaira

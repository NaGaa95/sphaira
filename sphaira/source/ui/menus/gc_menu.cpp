#include "ui/menus/gc_menu.hpp"
#include "ui/menus/filebrowser.hpp"

#include "ui/nvg_util.hpp"
#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"

#include "yati/yati.hpp"
#include "yati/nx/nca.hpp"
#include "yati/container/xci.hpp"

#include "utils/utils.hpp"
#include "utils/nsz_dumper.hpp"
#include "utils/devoptab.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "i18n.hpp"
#include "download.hpp"
#include "dumper.hpp"
#include "image.hpp"
#include "title_info.hpp"
#include "threaded_file_transfer.hpp"

#include <cstring>
#include <algorithm>

// from Gamecard-Installer-NX
extern "C" {

Result fsOpenGameCardStorage(FsStorage* out, const FsGameCardHandle* handle, FsGameCardPartitionRaw partition) {
    const struct {
        FsGameCardHandle handle;
        u32 partition;
    } in = { *handle, (u32)partition };

    return serviceDispatchIn(fsGetServiceSession(), 30, in, .out_num_objects = 1, .out_objects = &out->s);
}

Result fsOpenGameCardDetectionEventNotifier(FsEventNotifier* out) {
    return serviceDispatch(fsGetServiceSession(), 501,
        .out_num_objects = 1,
        .out_objects = &out->s
    );
}

}

namespace sphaira::ui::menu::gc {
namespace {

constexpr u32 XCI_MAGIC = std::byteswap(0x48454144);
constexpr u32 REMOUNT_ATTEMPT_MAX = 8; // same as nxdumptool.
constexpr const char* DUMP_GAMECARD_BASE_PATH = "/dumps/Gamecard";
constexpr const char* DUMP_XCZ_BASE_PATH = "/dumps/XCZ";

enum DumpFileType {
    DumpFileType_XCI,
    DumpFileType_TrimmedXCI,
    DumpFileType_Set,
    DumpFileType_UID,
    DumpFileType_Cert,
    DumpFileType_Initial,
    DumpFileType_XCZ,
};

enum DumpFileFlag {
    DumpFileFlag_XCI = 1 << 0,
    DumpFileFlag_Set = 1 << 1,
    DumpFileFlag_UID = 1 << 2,
    DumpFileFlag_Cert = 1 << 3,
    DumpFileFlag_Initial = 1 << 4,

    DumpFileFlag_AllBin = DumpFileFlag_Set | DumpFileFlag_UID | DumpFileFlag_Cert | DumpFileFlag_Initial,
    DumpFileFlag_All = DumpFileFlag_XCI | DumpFileFlag_AllBin,
};

const char *g_option_list[] = {
    "Install",
    "Export XCI (Gamecard)",
    "Export XCZ (Compressed XCI)",
    "Mount Fs",
};

auto GetXciSizeFromRomSize(u8 rom_size) -> s64 {
    switch (rom_size) {
        case 0xFA: return 1024ULL * 1024ULL * 1024ULL * 1ULL;
        case 0xF8: return 1024ULL * 1024ULL * 1024ULL * 2ULL;
        case 0xF0: return 1024ULL * 1024ULL * 1024ULL * 4ULL;
        case 0xE0: return 1024ULL * 1024ULL * 1024ULL * 8ULL;
        case 0xE1: return 1024ULL * 1024ULL * 1024ULL * 16ULL;
        case 0xE2: return 1024ULL * 1024ULL * 1024ULL * 32ULL;
    }
    return 0;
}

auto GetDumpTypeStr(u8 type) -> const char* {
    switch (type) {
        case DumpFileType_TrimmedXCI:
            if (App::GetApp()->m_dump_label_trim_xci.Get()) {
                return " (trimmed).xci";
            } [[fallthrough]];

        case DumpFileType_XCI: return ".xci";
        case DumpFileType_Set: return " (Card ID Set).bin";
        case DumpFileType_UID: return " (Card UID).bin";
        case DumpFileType_Cert: return " (Certificate).bin";
        case DumpFileType_Initial: return " (Initial Data).bin";
        case DumpFileType_XCZ: return ".xcz";
    }

    return "";
}

auto BuildXciName(const ApplicationEntry& e) -> fs::FsPath {
    const auto export_name = title::MakeExportTitleName(
        e.lang_entry.name, e.english_name, App::GetApp()->m_dump_fix_filenames.Get()
    );

    fs::FsPath path;
    if (export_name.empty()) {
        std::snprintf(path, sizeof(path), "[%016lX][v%u]", e.app_id, e.version);
    } else {
        std::snprintf(path, sizeof(path), "%.*s [%016lX][v%u]", static_cast<int>(export_name.size()), export_name.data(), e.app_id, e.version);
    }
    return path;
}

auto BuildXciBasePath(std::span<const ApplicationEntry> entries) -> fs::FsPath {
    fs::FsPath path;
    for (s64 i = 0; i < std::size(entries); i++) {
        if (i) {
            path += " + ";
        }
        path += BuildXciName(entries[i]);
    }

    return path;
}

#if 0
// builds path suiteable for usb transfer.
auto BuildFilePath(DumpFileType type, std::span<const ApplicationEntry> entries) -> fs::FsPath {
    return BuildXciBasePath(entries) + GetDumpTypeStr(type);
}
#endif

// builds path suiteable for file dumps.
auto BuildFullDumpPath(DumpFileType type, std::span<const ApplicationEntry> entries, bool use_folder) -> fs::FsPath {
    const auto base_path = BuildXciBasePath(entries);
    fs::FsPath out;

    if (type == DumpFileType_XCZ) {
        out = base_path + GetDumpTypeStr(type);
        return fs::AppendPath(DUMP_XCZ_BASE_PATH, out);
    } else {
        if (use_folder) {
            if (App::GetApp()->m_dump_append_folder_with_xci.Get()) {
                out = base_path + ".xci/" + base_path + GetDumpTypeStr(type);
            } else {
                out = base_path + "/" + base_path + GetDumpTypeStr(type);
            }
        } else {
            out = base_path + GetDumpTypeStr(type);
        }

        return fs::AppendPath(DUMP_GAMECARD_BASE_PATH, out);
    }
}

auto BuildFullDumpPath(DumpFileType type, std::span<const ApplicationEntry> entries) -> fs::FsPath {
    // check if the base path is too long.
    const auto max_len = fs::FsPathReal::FS_REAL_MAX_LENGTH - std::strlen(DUMP_GAMECARD_BASE_PATH) - 30;
    auto use_folder = App::GetApp()->m_dump_app_folder.Get();

    for (;;) {
        const auto mult = use_folder ? 2 : 1;

        for (size_t i = entries.size(); i > 0; i--) {
            // see how many entries we can append to the file name.
            const auto span = entries.subspan(0, i);
            const auto base_path = BuildXciBasePath(span);

            if (std::strlen(base_path) * mult < max_len) {
                return BuildFullDumpPath(type, span, use_folder);
            }
        }

        if (!use_folder) {
            // if we get here, the game name is *really* long. Give up.
            log_write("[GC] huge game name, giving up: %s\n", BuildXciBasePath(entries).s);
            return {};
        } else {
            // try again, but without the folder.
            use_folder = false;
            log_write("[GC] huge game name trying again without the folder: %s\n", BuildXciBasePath(entries).s);
        }
    }
}

// @Gc is the mount point, S is for secure partion, the remaining is the
// the gamecard handle value in lower-case hex.
auto BuildGcPath(const char* name, const FsGameCardHandle* handle, FsGameCardPartition partiton = FsGameCardPartition_Secure) -> fs::FsPath {
    static const char mount_partition[] = {
        [FsGameCardPartition_Update] = 'U',
        [FsGameCardPartition_Normal] = 'N',
        [FsGameCardPartition_Secure] = 'S',
        [FsGameCardPartition_Logo] = 'L',
    };

    fs::FsPath path;
    std::snprintf(path, sizeof(path), "@Gc%c%08x://%s", mount_partition[partiton], handle->value, name);
    return path;
}

struct XciSource final : dump::BaseSource {
    // application name.
    std::string application_name{};
    // extra
    std::vector<u8> id_set{};
    std::vector<u8> uid{};
    std::vector<u8> cert{};
    std::vector<u8> initial{};
    // size of the entire xci.
    s64 xci_size{};
    Menu* menu{};
    int icon{};

    Result Read(const std::string& path, void* buf, s64 off, s64 size, u64* bytes_read) override {
        if (off == xci_size) {
            log_write("[XciSource::Read] read at eof...\n");
            *bytes_read = 0;
            R_SUCCEED();
        }

        if (path.ends_with(GetDumpTypeStr(DumpFileType_XCI)) || path.ends_with(GetDumpTypeStr(DumpFileType_XCZ))) {
            size = ClipSize(off, size, xci_size);
            *bytes_read = size;
            return menu->GcStorageRead(buf, off, size);
        } else {
            std::span<const u8> span;
            if (path.ends_with(GetDumpTypeStr(DumpFileType_Set))) {
                span = id_set;
            } else if (path.ends_with(GetDumpTypeStr(DumpFileType_UID))) {
                span = uid;
            } else if (path.ends_with(GetDumpTypeStr(DumpFileType_Cert))) {
                span = cert;
            } else if (path.ends_with(GetDumpTypeStr(DumpFileType_Initial))) {
                span = initial;
            }

            R_UNLESS(!span.empty(), Result_GcBadReadForDump);

            size = ClipSize(off, size, span.size());
            *bytes_read = size;

            std::memcpy(buf, span.data() + off, size);
            R_SUCCEED();
        }
    }

    auto GetName(const std::string& path) const -> std::string override {
        return application_name;
    }

    auto GetSize(const std::string& path) const -> s64 override {
        if (path.ends_with(GetDumpTypeStr(DumpFileType_XCI)) || path.ends_with(GetDumpTypeStr(DumpFileType_XCZ))) {
            return xci_size;
        } else if (path.ends_with(GetDumpTypeStr(DumpFileType_Set))) {
            return id_set.size();
        } else if (path.ends_with(GetDumpTypeStr(DumpFileType_UID))) {
            return uid.size();
        } else if (path.ends_with(GetDumpTypeStr(DumpFileType_Cert))) {
            return cert.size();
        } else if (path.ends_with(GetDumpTypeStr(DumpFileType_Initial))) {
            return initial.size();
        }
        return 0;
    }

    auto GetIcon(const std::string& path) const -> int override {
        return icon;
    }

private:
    static auto InRange(s64 off, s64 offset, s64 size) -> bool {
        return off < offset + size && off >= offset;
    }

    static auto ClipSize(s64 off, s64 size, s64 file_size) -> s64 {
        return std::min(size, file_size - off);
    }
};

struct Test final : yati::source::Base {
    Test(Menu* menu) : m_menu{menu} {

    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        R_TRY(m_menu->GcStorageRead(buf, off, size));
        *bytes_read = size;
        R_SUCCEED();
    }

private:
    Menu* m_menu;
};

struct NcaReader final : yati::source::Base {
    NcaReader(Test* source, s64 offset) : m_source{source}, m_offset{offset} {

    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        return m_source->Read(buf, m_offset + off, size, bytes_read);
    }

private:
    Test* m_source;
    const s64 m_offset;
};

#ifdef ENABLE_NSZ
Result NszExport(ProgressBox* pbox, const keys::Keys& keys, dump::BaseSource* _source, dump::WriteSource* writer, const fs::FsPath& path) {
    auto source = (XciSource*)_source;

    const auto threaded_write = [&](const std::string& name, s64& read_offset, s64& write_offset, s64 size) -> Result {
        if (size > 0) {
            pbox->NewTransfer(name);

            R_TRY(thread::Transfer(pbox, size,
                [&](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
                    return source->Read(path, data, read_offset + off, size, bytes_read);
                },
                [&](const void* data, s64 off, s64 size) -> Result {
                    return writer->Write(data, write_offset + off, size);
                }
            ));

            read_offset += size;
            write_offset += size;
        }

        R_SUCCEED();
    };

    // writes padding between partitions and files.
    const auto write_padding = [&](const std::string& name, s64& read_offset, s64& write_offset, s64 size) -> Result {
        return threaded_write("Writing padding - " + name, read_offset, write_offset, size);
    };

    Test yati_source(source->menu);
    yati::container::Xci xci{&yati_source};

    yati::container::Xci::Root root;
    R_TRY(xci.GetRoot(root));

    //
    s64 read_offset = 0;
    s64 write_offset = 0;

    for (u32 i = 0; i < std::size(root.partitions); i++) {
        auto& partition = root.partitions[i];
        auto& hfs0 = partition.hfs0;
        auto& collections = partition.collections;

        log_write("\tpartition name: %s offset: %zu size: %zu\n", partition.name.c_str(), partition.hfs0_offset, partition.hfs0_size);

        // read pading before hfs0
        R_TRY(write_padding("hfs0 before", read_offset, write_offset, partition.hfs0_offset - read_offset));

        // offset to the hfs0.
        const auto hfs0_offset = write_offset;
        // offset to the data within the hfs0.
        const auto hfs0_data_offset = hfs0_offset + hfs0.GetHfs0Size();
        // offset to the hfs0 within the root hfs0.
        const auto root_hfs0_data_offset = write_offset - root.hfs0.data_offset;

        // calculate the expected size of the partition.
        s64 expected_hfs0_data_size = 0;
        for (auto& collection : partition.collections) {
            expected_hfs0_data_size += collection.size;
        }

        if (!partition.collections.empty()) {
            R_TRY(write_padding(partition.name, read_offset, write_offset, partition.collections[0].offset - read_offset));
        } else {
            // empty hfs0, write it as is.
            log_write("empty hfs0 offset: %zu size: %zu get size: %zu\n", hfs0.data_offset, partition.hfs0_size, hfs0.GetHfs0Size());
            R_UNLESS(partition.hfs0_size == hfs0.GetHfs0Size(), 21);
            // R_UNLESS(hfs0.data_offset == 0, 14);
            R_TRY(write_padding(partition.name, read_offset, write_offset, partition.hfs0_size));
        }

        const auto nca_creator = [&yati_source](const nca::Header& header, const keys::KeyEntry& title_key, const utils::nsz::Collection& collection) {
            return std::make_unique<nca::NcaReader>(
                header, &title_key, collection.size,
                std::make_shared<NcaReader>(&yati_source, collection.offset)
            );
        };

        // todo: update write offset.
        R_TRY(utils::nsz::NszExport(pbox, nca_creator, read_offset, write_offset, collections, keys, source, writer, path));

        // update offset / size in file table and calculate new total data size.
        s64 new_hfs0_data_size = 0;
        for (u32 i = 0; i < std::size(collections); i++) {
            auto& collection = collections[i];
            auto& file_table = hfs0.file_table[i];

            // const auto offset = collection.offset - hfs0_data_offset;
            // log_write("offset: %zu\n", offset);
            // log_write("collection.offset: %zu\n", collection.offset);
            // log_write("hfs0.data_offset: %zu\n", hfs0.data_offset);
            // log_write("file_table.data_offset: %zu\n", file_table.data_offset);

            // R_UNLESS(file_table.data_offset == offset, 8);
            // R_UNLESS(file_table.data_size = collection.size, 9);

            // update file and string table from collection.
            file_table.data_offset = collection.offset - hfs0_data_offset;
            file_table.data_size = collection.size;
            hfs0.string_table[i] = collection.name;
            new_hfs0_data_size += collection.size;
        }

        // update offset and size of hfs0 in root file table.
        auto& root_file_table = root.hfs0.file_table[i];
        const auto hfs0_data_size = root_file_table.data_size - (expected_hfs0_data_size - new_hfs0_data_size);

        log_write("hfs0.data_offset: %zu\n", hfs0.data_offset);
        log_write("old data offset: %zu\n", root_file_table.data_offset);
        log_write("new data offset: %zu\n\n", root_hfs0_data_offset);

        log_write("old data size: %zu\n", root_file_table.data_size);
        log_write("new data size: %zu\n", hfs0_data_size);

        // R_UNLESS(root_file_table.data_offset == root_hfs0_data_offset, 5);
        // R_UNLESS(root_file_table.data_size == hfs0_data_size, 6);

        root_file_table.data_offset = root_hfs0_data_offset;
        root_file_table.data_size = hfs0_data_size;

        // re-write updated hfs0 partition.
        // R_UNLESS(partition.hfs0_offset == hfs0_offset, 7);
        const auto hfs0_data = hfs0.GetHfs0Data();
        R_TRY(writer->Write(hfs0_data.data(), hfs0_offset, hfs0_data.size()));
    }

    // add remaining padding, if needed.
    R_TRY(write_padding("hfs0 partition", read_offset, write_offset, read_offset % 512));

    // re-write updated root partition.
    const auto root_data = root.hfs0.GetHfs0Data();
    R_TRY(writer->Write(root_data.data(), root.hfs0_offset, root_data.size()));

    log_write("read_offset: %zu\n", read_offset);
    log_write("write_offset: %zu\n", write_offset);

    // update with actual size.
    R_TRY(writer->SetSize(write_offset));

    R_SUCCEED();
}
#endif // ENABLE_NSZ

struct GcSource final : yati::source::Base {
    GcSource(const ApplicationEntry& entry, fs::FsNativeGameCard* fs);
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read);

    yati::container::Collections m_collections{};
    yati::ConfigOverride m_config{};
    fs::FsNativeGameCard* m_fs{};
    fs::File m_file{};
    s64 m_offset{};
    s64 m_size{};

private:
    static auto InRange(s64 off, s64 offset, s64 size) -> bool {
        return off < offset + size && off >= offset;
    }
};

GcSource::GcSource(const ApplicationEntry& entry, fs::FsNativeGameCard* fs)
: m_fs{fs} {
    m_offset = -1;

    s64 offset{};
    const auto add_collections = [&](const auto& collections) {
        for (auto collection : collections) {
            collection.offset = offset;
            m_collections.emplace_back(collection);
            offset += collection.size;
        }
    };

    const auto add_entries = [&](const auto& entries) {
        for (auto& e : entries) {
            add_collections(e);
        }
    };

    // yati can handle all of this for use, however, yati lacks information
    // for ncas until it installs the cnmt and parses it.
    // as we already have this info, we can only send yati what we want to install.
    if (App::GetApp()->m_ticket_only.Get()) {
        add_collections(entry.tickets);
    } else {
        if (!App::GetApp()->m_skip_base.Get()) {
            add_entries(entry.application);
        }
        if (!App::GetApp()->m_skip_patch.Get()) {
            add_entries(entry.patch);
        }
        if (!App::GetApp()->m_skip_addon.Get()) {
            add_entries(entry.add_on);
        }
        if (!App::GetApp()->m_skip_data_patch.Get()) {
            add_entries(entry.data_patch);
        }
        if (!App::GetApp()->m_skip_ticket.Get()) {
            add_collections(entry.tickets);
        }
    }

    // we don't need to verify the nca's, this speeds up installs.
    m_config.skip_nca_hash_verify = true;
    m_config.skip_rsa_header_fixed_key_verify = true;
    m_config.skip_rsa_npdm_fixed_key_verify = true;
}

Result GcSource::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    // check is we need to open a new file.
    if (!InRange(off, m_offset, m_size)) {
        m_file.Close();

        // find new file based on the offset.
        bool found = false;
        for (auto& collection : m_collections) {
            if (InRange(off, collection.offset, collection.size)) {
                found = true;
                m_offset = collection.offset;
                m_size = collection.size;
                R_TRY(m_fs->OpenFile(fs::AppendPath("/", collection.name), FsOpenMode_Read, &m_file));
                break;
            }
        }

        // this will never fail, unless i break something in yati.
        R_UNLESS(found, Result_GcBadReadForDump);
    }

    return m_file.Read(off - m_offset, buf, size, 0, bytes_read);
}

} // namespace

auto ApplicationEntry::GetSize(const std::vector<GcCollections>& entries) const -> s64 {
    s64 size{};
    for (auto& e : entries) {
        for (auto& collection : e) {
            size += collection.size;
        }
    }
    return size;
}

auto ApplicationEntry::GetSize() const -> s64 {
    s64 size{};
    size += GetSize(application);
    size += GetSize(patch);
    size += GetSize(add_on);
    size += GetSize(data_patch);
    return size;
}

Menu::Menu(u32 flags) : MenuBase{"GameCard"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::A, Action{"OK"_i18n, [this](){
            if (!m_mounted) {
                return;
            }

            if (m_option_index == 0) {
                if (!App::GetInstallEnable()) {
                    App::ShowEnableInstallPrompt();
                } else {
                    log_write("[GC] doing install A\n");
                    App::Push<ui::ProgressBox>(m_icon, "Installing "_i18n, m_entries[m_entry_index].lang_entry.name, [this](auto pbox) -> Result {
                        auto source = std::make_unique<GcSource>(m_entries[m_entry_index], m_fs.get());
                        return yati::InstallFromCollections(pbox, source.get(), source->m_collections, source->m_config);
                    }, [this](Result rc){
                        App::PushErrorBox(rc, "Gc install failed!"_i18n);

                        if (R_SUCCEEDED(rc)) {
                            App::Notify("Gc install success!"_i18n);
                        }
                    }, ui::ProgressBoxOption::ScreenToggle);
                }
            } else if (m_option_index == 1) {
                auto options = std::make_unique<Sidebar>("Select content to dump"_i18n, Sidebar::Side::RIGHT);
                ON_SCOPE_EXIT(App::Push(std::move(options)));

                const auto add = [&](const std::string& name, u32 flags){
                    options->Add<SidebarEntryCallback>(name, [this, flags](){
                        App::PushErrorBox(DumpGames(flags), "Export failed!"_i18n);
                        m_dirty = true;
                    }, true);
                };

                add("Export All"_i18n, DumpFileFlag_All);
                add("Export All Bins"_i18n, DumpFileFlag_AllBin);
                add("Export XCI"_i18n, DumpFileFlag_XCI);
                add("Export Card ID Set"_i18n, DumpFileFlag_Set);
                add("Export Card UID"_i18n, DumpFileFlag_UID);
                add("Export Certificate"_i18n, DumpFileFlag_Cert);
                add("Export Initial Data"_i18n, DumpFileFlag_Initial);
            } else if (m_option_index == 2) {
#ifdef ENABLE_NSZ
                App::PushErrorBox(DumpXcz(0), "Export failed!"_i18n);
#endif // ENABLE_NSZ
            } else if (m_option_index == 3) {
                const auto rc = MountGcFs();
                App::PushErrorBox(rc, "Failed to mount GameCard filesystem"_i18n);
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("Game Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            options->Add<SidebarEntryCallback>("Install options"_i18n, [this](){
                App::DisplayInstallOptions(false);
            });

            options->Add<SidebarEntryCallback>("Export options"_i18n, [this](){
                App::DisplayDumpOptions(false);
            });
        }})
    );

    const Vec4 v{485, 275, 720, 70};
    const Vec2 pad{0, 23.75};

    m_list = std::make_unique<List>(1, 4, m_pos, v, pad);

    fsOpenDeviceOperator(std::addressof(m_dev_op));
    fsOpenGameCardDetectionEventNotifier(std::addressof(m_event_notifier));
    fsEventNotifierGetEventHandle(std::addressof(m_event_notifier), std::addressof(m_event), true);
    title::Init();
}

Menu::~Menu() {
    title::Exit();
    GcUnmount();
    eventClose(std::addressof(m_event));
    fsEventNotifierClose(std::addressof(m_event_notifier));
    fsDeviceOperatorClose(std::addressof(m_dev_op));
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    // poll for the gamecard first before handling inputs as the gamecard
    // may have been removed, thus pressing A would fail.
    if (m_dirty || R_SUCCEEDED(eventWait(std::addressof(m_event), 0))) {
        GcOnEvent(m_dirty);
        m_dirty = false;
    }

    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_option_index, std::size(g_option_list), [this](bool touch, auto i) {
        if (touch && m_option_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            m_option_index = i;
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    #define STORAGE_BAR_W   325
    #define STORAGE_BAR_H   14

    const auto size_sd_gb = (double)m_size_free_sd / 0x40000000;
    const auto size_nand_gb = (double)m_size_free_nand / 0x40000000;

    gfx::drawTextArgs(vg, 490, 135, 23.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "System memory %.1f GB"_i18n.c_str(), size_nand_gb);
    gfx::drawRect(vg, 480, 170, STORAGE_BAR_W, STORAGE_BAR_H, theme->GetColour(ThemeEntryID_TEXT));
    gfx::drawRect(vg, 480 + 1, 170 + 1, STORAGE_BAR_W - 2, STORAGE_BAR_H - 2, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND));
    gfx::drawRect(vg, 480 + 2, 170 + 2, STORAGE_BAR_W - (((double)m_size_free_nand / (double)m_size_total_nand) * STORAGE_BAR_W) - 4, STORAGE_BAR_H - 4, theme->GetColour(ThemeEntryID_TEXT));

    gfx::drawTextArgs(vg, 870, 135, 23.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "microSD card %.1f GB"_i18n.c_str(), size_sd_gb);
    gfx::drawRect(vg, 860, 170, STORAGE_BAR_W, STORAGE_BAR_H, theme->GetColour(ThemeEntryID_TEXT));
    gfx::drawRect(vg, 860 + 1, 170 + 1, STORAGE_BAR_W - 2, STORAGE_BAR_H - 2, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND));
    gfx::drawRect(vg, 860 + 2, 170 + 2, STORAGE_BAR_W - (((double)m_size_free_sd / (double)m_size_total_sd) * STORAGE_BAR_W) - 4, STORAGE_BAR_H - 4, theme->GetColour(ThemeEntryID_TEXT));

    gfx::drawRect(vg, 30, 90, 375, 555, theme->GetColour(ThemeEntryID_GRID));

    if (!m_entries.empty()) {
        const auto& e = m_entries[m_entry_index];
        const auto size = e.GetSize();
        gfx::drawImage(vg, 90, 130, 256, 256, m_icon ? m_icon : App::GetDefaultImage());

        nvgSave(vg);
            nvgIntersectScissor(vg, 50, 90, 325, 555);
            gfx::drawTextArgs(vg, 50, 415, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", e.lang_entry.name);
            gfx::drawTextArgs(vg, 50, 455, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", e.lang_entry.author);
            gfx::drawTextArgs(vg, 50, 495, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "App-ID: 0%lX"_i18n.c_str(), e.app_id);
            gfx::drawTextArgs(vg, 50, 535, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "Key-Gen: %u (%s)"_i18n.c_str(), e.key_gen, nca::GetKeyGenStr(e.key_gen));
            gfx::drawTextArgs(vg, 50, 575, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "Size: %.2f GB"_i18n.c_str(), (double)size / 0x40000000);
            gfx::drawTextArgs(vg, 50, 615, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "Base: %zu Patch: %zu Addon: %zu Data: %zu"_i18n.c_str(), e.application.size(), e.patch.size(), e.add_on.size(), e.data_patch.size());
        nvgRestore(vg);
    }

    m_list->Draw(vg, theme, std::size(g_option_list), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto text_y = y + (h / 2.f);
        auto colour = ThemeEntryID_TEXT;
        if (i == m_option_index) {
            gfx::drawRectOutline(vg, theme, 4.f, v);
            // g_background.selected_bar = create_shape(Colour_Nintendo_Cyan, 90, 230, 4, 45, true);
            // draw_shape_position(&g_background.selected_bar, 485, g_options[i].text->rect.y - 10);
            gfx::drawRect(vg, 490, text_y - 45.f / 2.f, 2, 45, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
            colour = ThemeEntryID_TEXT_SELECTED;
        }
        if (!m_mounted) {
            colour = ThemeEntryID_TEXT_INFO;
        }
        if (i == 2) {
#ifndef ENABLE_NSZ
            colour = ThemeEntryID_TEXT_INFO;
#endif // ENABLE_NSZ
        }

        gfx::drawTextArgs(vg, x + 15, y + (h / 2.f), 23.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(colour), "%s", i18n::get(g_option_list[i]).c_str());
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();

    GcOnEvent();
    UpdateStorageSize();
}

Result Menu::GcMount() {
    GcUnmount();

    // after storage has been mounted, it will take X attempts to mount
    // the fs, same as mounting storage.
    for (u32 i = 0; i < REMOUNT_ATTEMPT_MAX; i++) {
        R_TRY(fsDeviceOperatorGetGameCardHandle(std::addressof(m_dev_op), std::addressof(m_handle)));
        m_fs = std::make_unique<fs::FsNativeGameCard>(std::addressof(m_handle), FsGameCardPartition_Secure);
        if (R_SUCCEEDED(m_fs->GetFsOpenResult())) {
            break;
        }
    }

    R_TRY(m_fs->GetFsOpenResult());

    fs::Dir dir;
    R_TRY(m_fs->OpenDirectory("/", FsDirOpenMode_ReadFiles, std::addressof(dir)));

    std::vector<FsDirectoryEntry> buf;
    R_TRY(dir.ReadAll(buf));

    yati::container::Collections ticket_collections;
    for (const auto& e : buf) {
        if (!std::string_view(e.name).ends_with(".tik") && !std::string_view(e.name).ends_with(".cert")) {
            continue;
        }

        ticket_collections.emplace_back(e.name, 0, e.file_size);
    }

    for (const auto& e : buf) {
        // we could use ncm to handle finding all the ncas for us
        // however, we can parse faster than ncm.
        // not only that, the first few calls trying to mount ncm db for
        // the gamecard will fail as it has not yet been parsed (or it's locked?).
        // we could, of course, just wait until ncm is ready, which is about
        // 32ms, but i already have code for manually parsing cnmt so lets re-use it.
        if (!std::string_view(e.name).ends_with(".cnmt.nca")) {
            continue;
        }

        // we don't yet use the header or extended header.
        ncm::PackagedContentMeta header;
        std::vector<u8> extended_header;
        std::vector<NcmPackagedContentInfo> infos;
        const auto path = BuildGcPath(e.name, &m_handle);
        R_TRY(nca::ParseCnmt(path, 0, header, extended_header, infos));

        u8 key_gen;
        FsRightsId rights_id;
        R_TRY(fsGetRightsIdAndKeyGenerationByPath(path, FsContentAttributes_All, &key_gen, &rights_id));

        // always add tickets, yati will ignore them if not needed.
        GcCollections collections;
        // add cnmt file.
        collections.emplace_back(e.name, e.file_size, NcmContentType_Meta, 0);

        for (const auto& packed_info : infos) {
            const auto& info = packed_info.info;
            // these don't exist for gamecards, however i may copy/paste this code
            // somewhere so i'm future proofing against myself.
            if (info.content_type == NcmContentType_DeltaFragment) {
                continue;
            }

            // find the nca file, this will never fail for gamecards, see above comment.
            const auto str = utils::hexIdToStr(info.content_id);
            const auto it = std::find_if(buf.cbegin(), buf.cend(), [str](auto& e){
                return !std::strncmp(str.str, e.name, std::strlen(str.str));
            });

            R_UNLESS(it != buf.cend(), Result_YatiNcaNotFound);
            collections.emplace_back(it->name, it->file_size, info.content_type, info.id_offset);
        }

        const auto app_id = ncm::GetAppId(header);
        ApplicationEntry* app_entry{};
        for (auto& app : m_entries) {
            if (app.app_id == app_id) {
                app_entry = &app;
                break;
            }
        }

        if (!app_entry) {
            app_entry = &m_entries.emplace_back(app_id, header.title_version);
        }

        app_entry->version = std::max(app_entry->version, header.title_version);
        app_entry->key_gen = std::max(app_entry->key_gen, key_gen);

        if (header.meta_type == NcmContentMetaType_Application) {
            app_entry->application.emplace_back(collections);
        } else if (header.meta_type == NcmContentMetaType_Patch) {
            app_entry->patch.emplace_back(collections);
        } else if (header.meta_type == NcmContentMetaType_AddOnContent) {
            app_entry->add_on.emplace_back(collections);
        } else if (header.meta_type == NcmContentMetaType_DataPatch) {
            app_entry->data_patch.emplace_back(collections);
        }
    }

    R_UNLESS(m_entries.size(), Result_GcEmptyGamecard);

    // append tickets to every application, yati will ignore if undeeded.
    for (auto& e : m_entries) {
        e.tickets = ticket_collections;
    }

    // load all control data, icons are loaded when displayed.
    for (auto& e : m_entries) {
        R_TRY(LoadControlData(e));
    }

    if (m_entries.size() > 1) {
        SetAction(Button::L2, Action{"Prev"_i18n, [this](){
            if (m_entry_index != 0) {
                OnChangeIndex(m_entry_index - 1);
            }
        }});
        SetAction(Button::R2, Action{"Next"_i18n, [this](){
            if (m_entry_index < m_entries.size()) {
                OnChangeIndex(m_entry_index + 1);
            }
        }});
    }

    OnChangeIndex(0);
    m_mounted = true;
    R_SUCCEED();
}

void Menu::GcUnmount() {
    GcUmountStorage();

    m_fs.reset();
    m_entries.clear();
    m_entry_index = 0;
    m_mounted = false;
    FreeImage();

    RemoveAction(Button::L2);
    RemoveAction(Button::R2);
}

Result Menu::GcMountStorage() {
    GcUmountStorage();

    R_TRY(GcMountPartition(FsGameCardPartitionRaw_Normal));
    R_TRY(fsStorageGetSize(&m_storage, &m_storage_full_size));

    u8 header[0x200];
    R_TRY(fsStorageRead(&m_storage, 0, header, sizeof(header)));

    u32 magic;
    u32 trim_size;
    u8 rom_size;
    std::memcpy(&magic, header + 0x100, sizeof(magic));
    std::memcpy(&rom_size, header + 0x10D, sizeof(rom_size));
    std::memcpy(&trim_size, header + 0x118, sizeof(trim_size));
    std::memcpy(&m_package_id, header + 0x110, sizeof(m_package_id));
    std::memcpy(m_initial_data_hash, header + 0x160, sizeof(m_initial_data_hash));
    R_UNLESS(magic == XCI_MAGIC, Result_GcBadXciMagic);

    // calculate the reported size, error if not found.
    m_storage_full_size = GetXciSizeFromRomSize(rom_size);
    log_write("[GC] m_storage_full_size: %zd rom_size: 0x%X\n", m_storage_full_size, rom_size);
    R_UNLESS(m_storage_full_size > 0, Result_GcBadXciRomSize);

    R_TRY(fsStorageGetSize(&m_storage, &m_partition_normal_size));
    R_TRY(GcMountPartition(FsGameCardPartitionRaw_Secure));
    R_TRY(fsStorageGetSize(&m_storage, &m_partition_secure_size));

    m_storage_trimmed_size = sizeof(header) + trim_size * 512ULL;
    m_storage_total_size = m_partition_normal_size + m_partition_secure_size;
    m_storage_mounted = true;

    log_write("[GC] m_storage_trimmed_size: %zd\n", m_storage_trimmed_size);
    log_write("[GC] m_storage_total_size: %zd\n", m_storage_total_size);

    R_SUCCEED();
}

void Menu::GcUmountStorage() {
    if (m_storage_mounted) {
        m_storage_mounted = false;
        GcUnmountPartition();
    }
}

Result Menu::GcMountPartition(FsGameCardPartitionRaw partition) {
    if (m_partition == partition) {
        R_SUCCEED();
    }

    GcUnmountPartition();

    // first attempt always fails due to qlaunch having the secure area mounted.
    // the 2nd attempt will succeeded, but qlaunch will fail to mount
    // the gamecard as it will only attempt to mount once.
    Result rc;
    for (u32 i = 0; i < REMOUNT_ATTEMPT_MAX; i++) {
        R_TRY(fsDeviceOperatorGetGameCardHandle(&m_dev_op, &m_handle));
        if (R_SUCCEEDED(rc = fsOpenGameCardStorage(&m_storage, &m_handle, partition))){
            break;
        }
    }

    m_partition = partition;
    return rc;
}

void Menu::GcUnmountPartition() {
    if (m_partition != FsGameCardPartitionRaw_None) {
        m_partition = FsGameCardPartitionRaw_None;
        fsStorageClose(&m_storage);
    }
}

Result Menu::GcStorageReadInternal(void* buf, s64 off, s64 size, u64* bytes_read) {
    if (off < m_partition_normal_size) {
        size = std::min<s64>(size, m_partition_normal_size - off);
        R_TRY(GcMountPartition(FsGameCardPartitionRaw_Normal));
    } else {
        off = off - m_partition_normal_size;
        R_TRY(GcMountPartition(FsGameCardPartitionRaw_Secure));
    }

    R_TRY(fsStorageRead(&m_storage, off, buf, size));
    *bytes_read = size;
    R_SUCCEED();
}

Result Menu::GcStorageRead(void* _buf, s64 off, s64 size) {
    auto buf = static_cast<u8*>(_buf);
    u64 bytes_read;
    u8 data[0x200];

    size = std::min(size, m_storage_total_size - off);
    if (size <= 0) {
        R_SUCCEED();
    }

    const auto unaligned_off = off % 0x200;
    off -= unaligned_off;
    if (size > 0 && unaligned_off) {
        R_TRY(GcStorageReadInternal(data, off, sizeof(data), &bytes_read));

        const auto csize = std::min<s64>(size, 0x200 - unaligned_off);
        std::memcpy(buf, data + unaligned_off, csize);
        off += bytes_read;
        size -= csize;
        buf += csize;
    }

    const auto unaligned_size = size % 0x200;
    size -= unaligned_size;
    while (size > 0) {
        R_TRY(GcStorageReadInternal(buf, off, size, &bytes_read));

        off += bytes_read;
        size -= bytes_read;
        buf += bytes_read;
    }

    if (unaligned_size) {
        R_TRY(GcStorageReadInternal(data, off, sizeof(data), &bytes_read));
        std::memcpy(buf, data, unaligned_size);
    }

    R_SUCCEED();
}

Result Menu::GcPoll(bool* inserted) {
    R_TRY(fsDeviceOperatorIsGameCardInserted(&m_dev_op, inserted));

    // if the handle changed, re-mount the game card.
    if (*inserted && m_mounted) {
        FsGameCardHandle handle;
        R_TRY(fsDeviceOperatorGetGameCardHandle(std::addressof(m_dev_op), std::addressof(handle)));
        if (handle.value != m_handle.value) {
            R_TRY(GcMount());
        }
    }

    R_SUCCEED();
}

Result Menu::GcOnEvent(bool force) {
    bool inserted{};
    R_TRY(GcPoll(&inserted));

    if (force || m_mounted != inserted) {
        log_write("gc state changed\n");
        m_mounted = inserted;
        if (m_mounted) {
            log_write("trying to mount\n");
            m_mounted = R_SUCCEEDED(GcMount());
            if (m_mounted) {
                App::PlaySoundEffect(SoundEffect::Startup);
            }
        } else {
            log_write("trying to unmount\n");
            GcUnmount();
        }
    }

    R_SUCCEED();
}

Result Menu::UpdateStorageSize() {
    fs::FsNativeContentStorage fs_nand{FsContentStorageId_User};
    fs::FsNativeContentStorage fs_sd{FsContentStorageId_SdCard};

    R_TRY(fs_sd.GetFreeSpace("/", &m_size_free_sd));
    R_TRY(fs_sd.GetTotalSpace("/", &m_size_total_sd));
    R_TRY(fs_nand.GetFreeSpace("/", &m_size_free_nand));
    R_TRY(fs_nand.GetTotalSpace("/", &m_size_total_nand));
    R_SUCCEED();
}

void Menu::FreeImage() {
    if (m_icon) {
        nvgDeleteImage(App::GetVg(), m_icon);
        m_icon = 0;
    }
}

Result Menu::LoadControlData(ApplicationEntry& e) {
    const auto data = title::Get(e.app_id);
    R_UNLESS(data->status == title::NacpLoadStatus::Loaded, 0x1);

    e.icon = data->icon;
    e.lang_entry = data->lang;
    e.english_name = title::GetEnglishTitleName(e.app_id);
    R_SUCCEED();
}

void Menu::OnChangeIndex(s64 new_index) {
    FreeImage();
    m_entry_index = new_index;

    if (m_entries.empty()) {
        this->SetSubHeading("No GameCard inserted"_i18n);
    } else {
        const auto index = m_entries.empty() ? 0 : m_entry_index + 1;
        this->SetSubHeading(std::to_string(index) + " / " + std::to_string(m_entries.size()));

        const auto& e = m_entries[m_entry_index];

        TimeStamp ts;
        const auto image = ImageLoadFromMemory(e.icon, ImageFlag_JPEG);
        if (!image.data.empty()) {
            m_icon = nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
            log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
        }
    }
}

#ifdef ENABLE_NSZ
Result Menu::DumpXcz(u32 flags) {
    R_TRY(GcMountStorage());

    auto source = std::make_shared<XciSource>();
    source->menu = this;
    source->application_name = m_entries[m_entry_index].lang_entry.name;
    source->icon = m_icon;

    // todo: support for prepending cert area.
    std::vector<fs::FsPath> paths;
    source->xci_size = m_storage_trimmed_size;
    paths.emplace_back(BuildFullDumpPath(DumpFileType_XCZ, m_entries));

    // todo: log keys error.
    keys::Keys keys;
    R_TRY(keys::parse_keys(keys, true));

    dump::Dump(source, paths, [keys](ProgressBox* pbox, dump::BaseSource* source, dump::WriteSource* writer, const fs::FsPath& path) {
        return NszExport(pbox, keys, source, writer, path);
    });

    R_SUCCEED();
}
#endif // ENABLE_NSZ

Result Menu::DumpGames(u32 flags) {
    // first, try and mount the storage.
    // this will fill out the xci header, verify and get sizes.
    R_TRY(GcMountStorage());

    const auto do_dump = [this](u32 flags) -> Result {
        App::SetBoostMode(true);
        ON_SCOPE_EXIT(App::SetBoostMode(false));

        u32 location_flags = dump::DumpLocationFlag_All;

        // if we need to dump any of the bins, read fs memory until we find
        // what we are looking for.
        // the below code, along with the structs is taken from nxdumptool.
        GameCardSecurityInformation security_info;
        if ((flags &~ DumpFileFlag_XCI)) {
            location_flags &= ~dump::DumpLocationFlag_UsbS2S;
            R_TRY(GcGetSecurityInfo(security_info));
        }

        auto source = std::make_shared<XciSource>();
        source->menu = this;
        source->application_name = m_entries[m_entry_index].lang_entry.name;
        source->icon = m_icon;

        std::vector<fs::FsPath> paths;
        if (flags & DumpFileFlag_XCI) {
            if (App::GetApp()->m_dump_trim_xci.Get()) {
                source->xci_size = m_storage_trimmed_size;
                paths.emplace_back(BuildFullDumpPath(DumpFileType_TrimmedXCI, m_entries));
            } else {
                source->xci_size = m_storage_total_size;
                paths.emplace_back(BuildFullDumpPath(DumpFileType_XCI, m_entries));
            }
        }

        if (flags & DumpFileFlag_Set) {
            source->id_set.resize(sizeof(FsGameCardIdSet));
            R_TRY(fsDeviceOperatorGetGameCardIdSet(&m_dev_op, source->id_set.data(), source->id_set.size(), source->id_set.size()));
            paths.emplace_back(BuildFullDumpPath(DumpFileType_Set, m_entries));
        }

        if (flags & DumpFileFlag_UID) {
            source->uid.resize(sizeof(security_info.specific_data.card_uid));
            std::memcpy(source->uid.data(), &security_info.specific_data.card_uid, source->uid.size());
            paths.emplace_back(BuildFullDumpPath(DumpFileType_UID, m_entries));
        }

        if (flags & DumpFileFlag_Cert) {
            source->cert.resize(sizeof(security_info.certificate));
            std::memcpy(source->cert.data(), &security_info.certificate, source->cert.size());
            paths.emplace_back(BuildFullDumpPath(DumpFileType_Cert, m_entries));
        }

        if (flags & DumpFileFlag_Initial) {
            source->initial.resize(sizeof(security_info.initial_data));
            std::memcpy(source->initial.data(), &security_info.initial_data, source->initial.size());
            paths.emplace_back(BuildFullDumpPath(DumpFileType_Initial, m_entries));
        }


        dump::Dump(source, paths, nullptr, location_flags);
        R_SUCCEED();
    };

    // run some checks to see if the gamecard we can read past the trimmed size.
    // if we can, then this is a full / valid gamecard.
    // if it fails, it's likely a flashcart with a trimmed xci (will N check this?)
    bool is_trimmed = false;
    Result trim_rc = 0;
    if ((flags & DumpFileFlag_XCI) && m_storage_trimmed_size < m_storage_total_size) {
        const auto start_offset = std::min<s64>(0, m_storage_trimmed_size - 0x4000);
        // works on fw 1.2.0 and below.
        std::vector<u8> temp(1024*1024*1);
        if (R_FAILED(trim_rc = GcStorageRead(temp.data(), m_storage_trimmed_size, std::min<s64>(temp.size(), m_storage_total_size - start_offset)))) {
            log_write("[GC] WARNING1! GameCard is already trimmed: 0x%X FlashError: %u\n", trim_rc, trim_rc == 0x13D002);
            is_trimmed = true;
        }

        if (!is_trimmed) {
            // works on fw 1.2.0 and below.
            if (R_FAILED(trim_rc = GcStorageRead(temp.data(), m_storage_total_size - temp.size(), temp.size()))) {
                log_write("[GC] WARNING2! GameCard is already trimmed: 0x%X FlashError: %u\n", trim_rc, trim_rc == 0x13D002);
                is_trimmed = true;
            }
        }
    }

    // if trimmed and the user wants to dump the full xci, error.
    if ((flags & DumpFileFlag_XCI) && is_trimmed && App::GetApp()->m_dump_trim_xci.Get()) {
        // the callback runs after DumpGames has returned, so capture by value.
        App::Push<ui::OptionBox>(
            "WARNING: GameCard is already trimmed!"_i18n,
            "Back"_i18n, "Continue"_i18n, 0, [do_dump, flags](auto op_index){
                if (op_index && *op_index) {
                    App::PushErrorBox(do_dump(flags), "Export failed!"_i18n);
                }
            }, m_icon
        );
    } else if ((flags & DumpFileFlag_XCI) && is_trimmed) {
        App::PushErrorBox(trim_rc, "GameCard is trimmed, full dump is not possible!"_i18n);
    } else {
        R_TRY(do_dump(flags));
    }

    R_SUCCEED();
}

Result Menu::GcGetSecurityInfo(GameCardSecurityInformation& out) {
    R_TRY(GcMountPartition(FsGameCardPartitionRaw_Secure));

    constexpr u64 title_id = 0x0100000000000000; // FS
    Handle handle{};
    DebugEventInfo event_info{};
    u64 pids[0x50]{};
    s32 process_count{};

    R_TRY(svcGetProcessList(&process_count, pids, std::size(pids)));
    for (s32 i = 0; i < (process_count - 1); i++) {
        if (R_SUCCEEDED(svcDebugActiveProcess(&handle, pids[i]))) {
            ON_SCOPE_EXIT(svcCloseHandle(handle));

            if (R_FAILED(svcGetDebugEvent(&event_info, handle)) || title_id != event_info.info.create_process.program_id) {
                continue;
            }

            const auto package_id = m_package_id;
            static u64 addr{};
            MemoryInfo mem_info{};
            u32 page_info{};
            std::vector<u8> data{};

            for (;;) {
                R_TRY(svcQueryDebugProcessMemory(&mem_info, &page_info, handle, addr));

                // if addr=0 then we hit the reserved memory section
                addr = mem_info.addr + mem_info.size;
                if (!addr) {
                    break;
                }

                // skip memory that we don't want
                if (mem_info.attr || !mem_info.size || (mem_info.perm & Perm_Rw) != Perm_Rw || (mem_info.type & MemState_Type) != MemType_CodeMutable) {
                    continue;
                }

                data.resize(mem_info.size);
                R_TRY(svcReadDebugProcessMemory(data.data(), handle, mem_info.addr, data.size()));

                for (s64 i = 0; i < data.size(); i += 8) {
                    if (i + sizeof(out.initial_data) >= data.size()) {
                        break;
                    }

                    if (!std::memcmp(&package_id, data.data() + i, sizeof(m_package_id))) [[unlikely]] {
                        log_write("[GC] found the package id\n");
                        u8 hash[SHA256_HASH_SIZE];
                        sha256CalculateHash(hash, data.data() + i, 0x200);

                        if (!std::memcmp(hash, m_initial_data_hash, sizeof(hash))) {
                            // successive calls will jump to the addr as the location will not change.
                            addr = mem_info.addr;
                            log_write("[GC] found the security info\n");
                            log_write("\tperm: 0x%X\n", mem_info.perm);
                            log_write("\ttype: 0x%X\n", mem_info.type & MemState_Type);
                            log_write("\taddr: 0x%016lX\n", mem_info.addr);
                            log_write("\toff: 0x%016lX\n", mem_info.addr + i);
                            std::memcpy(&out, data.data() + i - offsetof(GameCardSecurityInformation, initial_data), sizeof(out));
                            R_SUCCEED();
                        }
                    }
                }
            }
        }
    }

    R_THROW(Result_GcFailedToGetSecurityInfo);
}

Result Menu::MountGcFs() {
    #if 1
    R_TRY(GcMountStorage());

    const auto& e = m_entries[m_entry_index];
    auto source = std::make_shared<Test>(this);

    fs::FsPath root;
    R_TRY(devoptab::MountXciSource(source, m_storage_trimmed_size, e.lang_entry.name, root));

    auto fs = std::make_shared<filebrowser::FsStdioWrapper>(root, [root](){
        devoptab::UmountNeworkDevice(root);
    });

    filebrowser::MountFsHelper(fs, e.lang_entry.name);

    #else
    // old code that only mounts secure partition.
    const auto& e = m_entries[m_entry_index];

    auto fs = std::make_shared<fs::FsNative>(&m_fs->m_fs, false);
    R_TRY(m_fs->GetFsOpenResult());

    const filebrowser::FsEntry fs_entry{
        .name = e.lang_entry.name,
        .root = "/",
        .type = filebrowser::FsType::Custom,
        .flags = filebrowser::FsEntryFlag_ReadOnly,
    };

    App::Push<filebrowser::Menu>(fs, fs_entry, "/");
    #endif
    R_SUCCEED();
}

} // namespace sphaira::ui::menu::gc

#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "location.hpp"
#include "image.hpp"
#include "threaded_file_transfer.hpp"
#include "minizip_helper.hpp"
#include "dumper.hpp"
#include "swkbd.hpp"

#include "utils/devoptab.hpp"

#include "ui/menus/save_menu.hpp"
#include "ui/menus/filebrowser.hpp"

#include "ui/sidebar.hpp"
#include "ui/error_box.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"

#include "yati/nx/ns.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/nca.hpp"

#include <utility>
#include <cstring>
#include <algorithm>
#include <minIni.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>

namespace sphaira::ui::menu::save {
namespace {

constexpr u32 NX_SAVE_META_MAGIC = 0x4A4B5356; // JKSV
constexpr u32 NX_SAVE_META_VERSION = 1;
constexpr const char* NX_SAVE_META_NAME = ".nx_save_meta.bin";

std::atomic_bool g_change_signalled{};

struct DumpSource final : dump::BaseSource {
    DumpSource(std::span<const std::reference_wrapper<Entry>> entries, std::span<const fs::FsPath> paths)
    : m_entries{entries}
    , m_paths{paths} {

    }

    Result Read(const std::string& path, void* buf, s64 off, s64 size, u64* bytes_read) override {
        R_SUCCEED();
    }

    auto GetName(const std::string& path) const -> std::string override {
        return GetEntry(path).GetName();
    }

    auto GetSize(const std::string& path) const -> s64 override {
        return 0;
    }

    auto GetIcon(const std::string& path) const -> int override {
        return GetEntry(path).image;
    }

    auto GetEntry(const std::string& path) const -> Entry& {
        const auto itr = std::ranges::find_if(m_paths, [&path](auto& e){
            return path == e;
        });
        const auto index = std::distance(m_paths.begin(), itr);
        return m_entries[index];
    }

private:
    std::span<const std::reference_wrapper<Entry>> m_entries;
    std::span<const fs::FsPath> m_paths;
};

// https://github.com/J-D-K/JKSV/issues/264#issuecomment-2618962807
struct NXSaveMeta {
    u32 magic{}; // NX_SAVE_META_MAGIC
    u32 version{}; // NX_SAVE_META_VERSION
    FsSaveDataAttribute attr{}; // FsSaveDataExtraData::attr
    u64 owner_id{}; // FsSaveDataExtraData::owner_id
    u64 timestamp{}; // FsSaveDataExtraData::timestamp
    u32 flags{}; // FsSaveDataExtraData::flags
    u32 unk_x54{}; // FsSaveDataExtraData::unk_x54
    s64 data_size{}; // FsSaveDataExtraData::data_size
    s64 journal_size{}; // FsSaveDataExtraData::journal_size
    u64 commit_id{}; // FsSaveDataExtraData::commit_id
    u64 raw_size{}; // FsSaveDataInfo::size
};
static_assert(sizeof(NXSaveMeta) == 128);

void GetFsSaveAttr(const AccountProfileBase& acc, u8 data_type, FsSaveDataSpaceId& space_id, FsSaveDataFilter& filter) {
    std::memset(&filter, 0, sizeof(filter));

    space_id = FsSaveDataSpaceId_User;
    filter.attr.save_data_type = data_type;
    filter.filter_by_save_data_type = true;

    switch (data_type) {
        case FsSaveDataType_System:
        case FsSaveDataType_SystemBcat:
            space_id = FsSaveDataSpaceId_System;
            break;
        case FsSaveDataType_Account:
            space_id = FsSaveDataSpaceId_User;
            filter.attr.uid = acc.uid;
            filter.filter_by_user_id = true;
            break;
        case FsSaveDataType_Bcat:
        case FsSaveDataType_Device:
            space_id = FsSaveDataSpaceId_User;
            break;
        case FsSaveDataType_Temporary:
            space_id = FsSaveDataSpaceId_Temporary;
            break;
        case FsSaveDataType_Cache:
            space_id = FsSaveDataSpaceId_SdUser;
            break;
    }
}

auto GetSaveFolder(u8 data_type) -> fs::FsPath {
    switch (data_type) {
        case FsSaveDataType_System:     return "Save System";
        case FsSaveDataType_SystemBcat: return "Save System BCAT";
        case FsSaveDataType_Account:    return "Save";
        case FsSaveDataType_Bcat:       return "Save BCAT";
        case FsSaveDataType_Device:     return "Save Device";
        case FsSaveDataType_Temporary:  return "Save Temporary";
        case FsSaveDataType_Cache:      return "Save Cache";
    }
    std::unreachable();
}

auto GetSaveFolder(const Entry& e) {
    return GetSaveFolder(e.save_data_type);
}

// https://switchbrew.org/wiki/Flash_Filesystem#SystemSaveData
auto GetSystemSaveName(u64 system_save_data_id) -> const char* {
    switch (system_save_data_id) {
        case 0x8000000000000000: return "fs"; break;
        case 0x8000000000000010: return "account"; break;
        case 0x8000000000000011: return "account"; break;
        case 0x8000000000000020: return "nfc"; break;
        case 0x8000000000000030: return "ns"; break;
        case 0x8000000000000031: return "ns"; break;
        case 0x8000000000000040: return "ns"; break;
        case 0x8000000000000041: return "ns"; break;
        case 0x8000000000000043: return "ns"; break;
        case 0x8000000000000044: return "ns"; break;
        case 0x8000000000000045: return "ns"; break;
        case 0x8000000000000046: return "ns"; break;
        case 0x8000000000000047: return "ns"; break;
        case 0x8000000000000048: return "ns"; break;
        case 0x8000000000000049: return "ns"; break;
        case 0x800000000000004A: return "ns"; break;
        case 0x8000000000000050: return "settings"; break;
        case 0x8000000000000051: return "settings"; break;
        case 0x8000000000000052: return "settings"; break;
        case 0x8000000000000053: return "settings"; break;
        case 0x8000000000000054: return "settings"; break;
        case 0x8000000000000060: return "ssl"; break;
        case 0x8000000000000061: return "ssl"; break; // guessing
        case 0x8000000000000070: return "nim"; break;
        case 0x8000000000000071: return "nim"; break;
        case 0x8000000000000072: return "nim"; break;
        case 0x8000000000000073: return "nim"; break;
        case 0x8000000000000074: return "nim"; break;
        case 0x8000000000000075: return "nim"; break;
        case 0x8000000000000076: return "nim"; break;
        case 0x8000000000000077: return "nim"; break;
        case 0x8000000000000078: return "nim"; break;
        case 0x8000000000000080: return "friends"; break;
        case 0x8000000000000081: return "friends"; break;
        case 0x8000000000000082: return "friends"; break;
        case 0x8000000000000090: return "bcat"; break;
        case 0x8000000000000091: return "bcat"; break;
        case 0x8000000000000092: return "bcat"; break;
        case 0x80000000000000A0: return "bcat"; break;
        case 0x80000000000000A1: return "bcat"; break;
        case 0x80000000000000A2: return "bcat"; break;
        case 0x80000000000000B0: return "bsdsockets"; break;
        case 0x80000000000000C1: return "bcat"; break;
        case 0x80000000000000C2: return "bcat"; break;
        case 0x80000000000000D1: return "erpt"; break;
        case 0x80000000000000E0: return "es"; break;
        case 0x80000000000000E1: return "es"; break;
        case 0x80000000000000E2: return "es"; break;
        case 0x80000000000000E3: return "es"; break;
        case 0x80000000000000E4: return "es"; break;
        case 0x80000000000000F0: return "ns"; break;
        case 0x8000000000000100: return "pctl"; break;
        case 0x8000000000000110: return "npns"; break;
        case 0x8000000000000120: return "ncm"; break;
        case 0x8000000000000121: return "ncm"; break;
        case 0x8000000000000122: return "ncm"; break;
        case 0x8000000000000130: return "migration"; break;
        case 0x8000000000000131: return "migration"; break;
        case 0x8000000000000132: return "migration"; break;
        case 0x8000000000000133: return "migration"; break;
        case 0x8000000000000140: return "capsrv"; break;
        case 0x8000000000000150: return "olsc"; break;
        case 0x8000000000000151: return "olsc"; break;
        case 0x8000000000000152: return "olsc"; break;
        case 0x8000000000000153: return "olsc"; break;
        case 0x8000000000000180: return "sdb"; break;
        case 0x8000000000000190: return "glue"; break;
        case 0x8000000000000200: return "bcat"; break;
        case 0x8000000000000210: return "account"; break;
        case 0x8000000000000220: return "erpt"; break;
        case 0x8000000000001010: return "qlaunch"; break;
        case 0x8000000000001011: return "qlaunch"; break;
        case 0x8000000000001020: return "swkbd"; break;
        case 0x8000000000001021: return "swkbd"; break;
        case 0x8000000000001030: return "auth"; break;
        case 0x8000000000001040: return "miiEdit"; break;
        case 0x8000000000001050: return "miiEdit"; break;
        case 0x8000000000001060: return "LibAppletShop"; break;
        case 0x8000000000001061: return "LibAppletShop"; break;
        case 0x8000000000001070: return "LibAppletWeb"; break;
        case 0x8000000000001071: return "LibAppletWeb"; break;
        case 0x8000000000001080: return "LibAppletOff"; break;
        case 0x8000000000001081: return "LibAppletOff"; break;
        case 0x8000000000001090: return "LibAppletLns"; break;
        case 0x8000000000001091: return "LibAppletLns"; break;
        case 0x80000000000010A0: return "LibAppletAuth"; break;
        case 0x80000000000010A1: return "LibAppletAuth"; break;
        case 0x80000000000010B0: return "playerSelect"; break;
        case 0x80000000000010C0: return "myPage"; break;
        case 0x80000000000010E1: return "qlaunch"; break;
        case 0x8000000000001100: return "qlaunch"; break;
        case 0x8000000000002000: return "DevMenu"; break;
        case 0x8000000000002020: return "ns"; break;
        case 0x8000000000010002: return "bcat"; break;
        case 0x8000000000010003: return "bcat"; break;
        case 0x8000000000010004: return "bcat"; break;
        case 0x8000000000010005: return "bcat"; break;
        case 0x8000000000010006: return "bcat"; break;
        case 0x8000000000010007: return "bcat"; break;
    }

    return "Unknown";
}

void FakeNacpEntryForSystem(Entry& e) {
    e.status = title::NacpLoadStatus::Loaded;

    // fake the nacp entry
    std::snprintf(e.lang.name, sizeof(e.lang.name), "%s | %016lX", GetSystemSaveName(e.system_save_data_id), e.system_save_data_id);
    std::strcpy(e.lang.author, "Nintendo");
}

bool LoadControlImage(Entry& e, title::ThreadResultData* result) {
    if (!e.image && result && !result->icon.empty()) {
        TimeStamp ts;
        const auto image = ImageLoadFromMemory(result->icon, ImageFlag_JPEG);
        if (!image.data.empty()) {
            e.image = nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
            log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
            return true;
        }
    }

    return false;
}

void LoadResultIntoEntry(Entry& e, title::ThreadResultData* result) {
    if (result) {
        e.status = result->status;
        e.lang = result->lang;
        e.status = result->status;
    }
}

void LoadControlEntry(Entry& e, bool force_image_load = false) {
    if (e.status != title::NacpLoadStatus::Loaded) {
        if (e.save_data_type == FsSaveDataType_System || e.save_data_type == FsSaveDataType_SystemBcat) {
            FakeNacpEntryForSystem(e);
        } else {
            LoadResultIntoEntry(e, title::Get(e.application_id));
        }
    }

    if (force_image_load && e.status == title::NacpLoadStatus::Loaded) {
        LoadControlImage(e, title::Get(e.application_id));
    }
}

auto BuildSaveName(const Entry& e) -> fs::FsPath {
    fs::FsPath name_buf = e.GetName();
    title::utilsReplaceIllegalCharacters(name_buf, true);
    return name_buf;
}

auto BuildSaveBasePath(const Entry& e, bool force_id_path = false) -> fs::FsPath {
    fs::FsPath name;
    if (e.save_data_type == FsSaveDataType_System || e.save_data_type == FsSaveDataType_SystemBcat) {
        std::snprintf(name, sizeof(name), "%016lX", e.system_save_data_id);
    } else if (force_id_path || !strcasecmp(e.GetName(), "corrupted")) {
        std::snprintf(name, sizeof(name), "%016lX", e.application_id);
    } else {
        name = BuildSaveName(e);
    }

    return fs::AppendPath("/dumps/" + GetSaveFolder(e), name);
}

void FreeEntry(NVGcontext* vg, Entry& e) {
    nvgDeleteImage(vg, e.image);
    e.image = 0;
}

} // namespace

void SignalChange() {
    g_change_signalled = true;
}

Menu::Menu(u32 flags) : grid::Menu{"Saves"_i18n, flags} {
    this->SetActions(
        std::make_pair(Button::L3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            m_entries[m_index].selected ^= 1;

            if (m_entries[m_index].selected) {
                m_selected_count++;
            } else {
                m_selected_count--;
            }
        }}),
        std::make_pair(Button::R3, Action{[this](){
            if (m_entries.empty()) {
                return;
            }

            if (m_selected_count == m_entries.size()) {
                ClearSelection();
            } else {
                m_selected_count = m_entries.size();
                for (auto& e : m_entries) {
                    e.selected = true;
                }
            }
        }}),
        std::make_pair(Button::A, Action{"Mount Fs"_i18n, [this](){
            if (!m_entries.empty()) {
                const auto rc = MountSaveFs();
                App::PushErrorBox(rc, "Failed to mount save filesystem"_i18n);
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            DisplayOptions();
        }})
    );

    OnLayoutChange();
    ns::Initialize();

    m_accounts = App::GetAccountList();

    // try and find the last / default account and set that.
    AccountUid uid{};
    if (R_FAILED(accountTrySelectUserWithoutInteraction(&uid, false))) {
        accountGetLastOpenedUser(&uid);
    }

    const auto it = std::ranges::find_if(m_accounts, [&uid](auto& e){
        return !std::memcmp(&uid, &e.uid, sizeof(uid));
    });

    if (it != m_accounts.end()) {
        m_account_index = std::distance(m_accounts.begin(), it);
        log_write("[SAVE] found account uid at: %zu\n", m_account_index);
    } else {
        log_write("[SAVE] account uid is not found: 0x%016lX%016lX\n", uid.uid[0], uid.uid[1]);
    }

    title::Init();
}

Menu::~Menu() {
    title::Exit();

    FreeEntries();
    ns::Exit();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    if (g_change_signalled.exchange(false)) {
        m_dirty = true;
    }

    if (m_dirty) {
        App::Notify("Updating application record list"_i18n);
        SortAndFindLastFile(true);
    }

    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_entries.empty()) {
        gfx::drawTextArgs(vg, GetX() + GetW() / 2.f, GetY() + GetH() / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 2;
    int image_load_count = 0;

    m_list->Draw(vg, theme, m_entries.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        auto& e = m_entries[pos];

        if (e.status == title::NacpLoadStatus::None) {
            if (m_data_type != FsSaveDataType_System && m_data_type != FsSaveDataType_SystemBcat) {
                title::PushAsync(e.application_id);
                e.status = title::NacpLoadStatus::Progress;
            } else {
                FakeNacpEntryForSystem(e);
            }
        } else if (e.status == title::NacpLoadStatus::Progress) {
            LoadResultIntoEntry(e, title::GetAsync(e.application_id));
        }

        // lazy load image
        if (image_load_count < image_load_max) {
            if (LoadControlImage(e, title::GetAsync(e.application_id))) {
                image_load_count++;
            }
        }

        const auto selected = pos == m_index;
        if (m_data_type != FsSaveDataType_System && m_data_type != FsSaveDataType_SystemBcat) {
            DrawEntry(vg, theme, m_layout.Get(), v, selected, e.image, e.GetName(), e.GetAuthor(), "");
        } else {
            const auto image_vec = DrawEntryNoImage(vg, theme, m_layout.Get(), v, selected, e.GetName(), e.GetAuthor(), "");
            gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_GRID), 5);
            gfx::drawTextArgs(vg, image_vec.x + image_vec.w / 2, image_vec.y + image_vec.w / 2, 20, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT), GetSystemSaveName(e.system_save_data_id));
        }

        if (e.selected) {
            gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_FOCUS), 5);
            gfx::drawText(vg, x + w / 2, y + h / 2, 24.f, "\uE14B", nullptr, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
        }
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    if (m_entries.empty()) {
        ScanHomebrew();
    }
}

void Menu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    if (m_accounts.empty()) {
        return;
    }

    u64 id{};
    if (!m_entries.empty()) {
        if (m_data_type == FsSaveDataType_System || m_data_type == FsSaveDataType_SystemBcat) {
            id = m_entries[m_index].system_save_data_id;
        } else {
            id = m_entries[m_index].application_id;
        }

        this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries.size()));
    } else {
        this->SetSubHeading("0 / 0");
    }

    char title[0x40];
    std::snprintf(title, sizeof(title), "%s | %016lX", m_accounts[m_account_index].nickname, id);
    SetTitleSubHeading(title);
}

void Menu::ScanHomebrew() {
    constexpr auto ENTRY_CHUNK_COUNT = 1000;
    TimeStamp ts;

    g_change_signalled = false;
    FreeEntries();
    ClearSelection();
    m_entries.reserve(ENTRY_CHUNK_COUNT);
    m_is_reversed = false;
    m_dirty = false;

    if (m_accounts.empty()) {
        return;
    }

    FsSaveDataSpaceId space_id;
    FsSaveDataFilter filter;
    GetFsSaveAttr(m_accounts[m_account_index], m_data_type, space_id, filter);

    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReaderWithFilter(&reader, space_id, &filter))) {
        log_write("[SAVE] failed to open reader\n");
    }
    ON_SCOPE_EXIT(fsSaveDataInfoReaderClose(&reader));

    std::vector<FsSaveDataInfo> info_list(ENTRY_CHUNK_COUNT);
    while (true) {
        s64 record_count{};
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, info_list.data(), info_list.size(), &record_count))) {
            log_write("failed fsSaveDataInfoReaderRead()\n");
            break;
        }

        // finished parsing all entries.
        if (!record_count) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            m_entries.emplace_back(info_list[i]);
        }
    }

    log_write("games found: %zu time_taken: %.2f seconds %zu ms %zu ns\n", m_entries.size(), ts.GetSecondsD(), ts.GetMs(), ts.GetNs());
    this->Sort();
    SetIndex(0);
    ClearSelection();
}

void Menu::Sort() {
    // const auto sort = m_sort.Get();
    const auto order = m_order.Get();

    if (order == OrderType_Ascending) {
        if (!m_is_reversed) {
            std::ranges::reverse(m_entries);
            m_is_reversed = true;
        }
    } else {
        if (m_is_reversed) {
            std::ranges::reverse(m_entries);
            m_is_reversed = false;
        }
    }
}

void Menu::SortAndFindLastFile(bool scan) {
    const auto app_id = m_entries[m_index].application_id;
    if (scan) {
        ScanHomebrew();
    } else {
        Sort();
    }
    SetIndex(0);

    s64 index = -1;
    for (u64 i = 0; i < m_entries.size(); i++) {
        if (app_id == m_entries[i].application_id) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        const auto row = m_list->GetRow();
        const auto page = m_list->GetPage();
        // guesstimate where the position is
        if (index >= page) {
            m_list->SetYoff((((index - page) + row) / row) * m_list->GetMaxY());
        } else {
            m_list->SetYoff(0);
        }
        SetIndex(index);
    }
}

void Menu::FreeEntries() {
    auto vg = App::GetVg();

    for (auto&p : m_entries) {
        FreeEntry(vg, p);
    }

    m_entries.clear();
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

void Menu::DisplayOptions() {
    auto options = std::make_unique<Sidebar>("Save Options"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    SidebarEntryArray::Items account_items;
    for (const auto& e : m_accounts) {
        account_items.emplace_back(e.nickname);
    }

    PopupList::Items data_type_items;
    data_type_items.emplace_back("System"_i18n);
    data_type_items.emplace_back("Account"_i18n);
    data_type_items.emplace_back("BCAT"_i18n);
    data_type_items.emplace_back("Device"_i18n);
    data_type_items.emplace_back("Temporary"_i18n);
    data_type_items.emplace_back("Cache"_i18n);
    data_type_items.emplace_back("System BCAT"_i18n);

    options->Add<SidebarEntryCallback>("Sort By"_i18n, [this](){
        auto options = std::make_unique<Sidebar>("Sort Options"_i18n, Sidebar::Side::RIGHT);
        ON_SCOPE_EXIT(App::Push(std::move(options)));

        SidebarEntryArray::Items sort_items;
        sort_items.push_back("Updated"_i18n);

        SidebarEntryArray::Items order_items;
        order_items.push_back("Descending"_i18n);
        order_items.push_back("Ascending"_i18n);

        SidebarEntryArray::Items layout_items;
        layout_items.push_back("List"_i18n);
        layout_items.push_back("Icon"_i18n);
        layout_items.push_back("Grid"_i18n);

        options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
            m_sort.Set(index_out);
            SortAndFindLastFile(false);
        }, m_sort.Get());

        options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
            m_order.Set(index_out);
            SortAndFindLastFile(false);
        }, m_order.Get());

        options->Add<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
            m_layout.Set(index_out);
            OnLayoutChange();
        }, m_layout.Get());
    });

    options->Add<SidebarEntryArray>("Account"_i18n, account_items, [this](s64& index_out){
        m_account_index = index_out;
        m_dirty = true;
        App::PopToMenu();
    }, m_account_index);

    options->Add<SidebarEntryArray>("Data Type"_i18n, data_type_items, [this](s64& index_out){
        m_data_type = index_out;
        m_dirty = true;
        App::PopToMenu();
    }, m_data_type);

    if (m_entries.size()) {
        options->Add<SidebarEntryCallback>("Backup"_i18n, [this](){
            std::vector<std::reference_wrapper<Entry>> entries;
            if (m_selected_count) {
                for (auto& e : m_entries) {
                    if (e.selected) {
                        entries.emplace_back(e);
                    }
                }
            } else {
                entries.emplace_back(m_entries[m_index]);
            }

            BackupSaves(entries, BackupFlag_None);
        }, true,
            "Backup the selected save(s) to a location of your choice."_i18n
        );

        if (!m_selected_count || m_selected_count == 1) {
            options->Add<SidebarEntryCallback>("Backup to..."_i18n, [this](){
                std::vector<std::reference_wrapper<Entry>> entries;
                if (m_selected_count) {
                    for (auto& e : m_entries) {
                        if (e.selected) {
                            entries.emplace_back(e);
                        }
                    }
                } else {
                    entries.emplace_back(m_entries[m_index]);
                }

                BackupSaves(entries, BackupFlag_SetName);
            }, true,
                "Backup the selected save(s) to a location of your choice, and set the name of the backup."_i18n
            );
        }

        if (m_entries[m_index].save_data_type == FsSaveDataType_Account || m_entries[m_index].save_data_type == FsSaveDataType_Bcat) {
            options->Add<SidebarEntryCallback>("Restore"_i18n, [this](){
                RestoreSave();
            }, true,
                i18n::get("save_backuprestore_info",
                    "Restore the save for the current title.\n"
                    "if \"Auto backup\" is enabled, the save will first be backed up and then restored. "
                    "Saves that are auto backed up will have \"Auto\" in their name."
                )
            );
        }
    }

    options->Add<SidebarEntryCallback>("Advanced"_i18n, [this](){
        auto options = std::make_unique<Sidebar>("Advanced Options"_i18n, Sidebar::Side::RIGHT);
        ON_SCOPE_EXIT(App::Push(std::move(options)));

        options->Add<SidebarEntryBool>("Auto backup on restore"_i18n, m_auto_backup_on_restore.Get(), [this](bool& v_out){
            m_auto_backup_on_restore.Set(v_out);
        }, "If enabled, when restoring a save, the current save will first be backed up."_i18n);

        options->Add<SidebarEntryBool>("Compress backup"_i18n, m_compress_save_backup.Get(), [this](bool& v_out){
            m_compress_save_backup.Set(v_out);
        },  i18n::get("save_backup_compress_info",
                "If enabled, backups will be compressed to a zip file.\n\n"
                "NOTE: Disabling this option does not disable the zip file, it only disables compressing "
                "the files stored in the zip.\n"
                "Disabling will result in a much faster backup, at the cost of the file size."
            )
        );
    });
}

void Menu::BackupSaves(std::vector<std::reference_wrapper<Entry>>& entries, u32 flags) {
    dump::DumpGetLocation("Select backup location"_i18n, dump::DumpLocationFlag_SdCard|dump::DumpLocationFlag_Stdio|dump::DumpLocationFlag_Usb, [this, entries, flags](const dump::DumpLocation& location){
        App::Push<ProgressBox>(0, "Backup"_i18n, "", [this, entries, location, flags](auto pbox) -> Result {
            return BackupSaveInternal(pbox, location, entries, flags);
        }, [](Result rc){
            App::PushErrorBox(rc, "Backup failed!"_i18n);

            if (R_SUCCEEDED(rc)) {
                App::Notify("Backup successfull!"_i18n);
            }
        });
    });
}

void Menu::RestoreSave() {
    dump::DumpGetLocation("Select restore location"_i18n, dump::DumpLocationFlag_SdCard|dump::DumpLocationFlag_Stdio, [this](const dump::DumpLocation& location){
        std::unique_ptr<fs::Fs> fs{};
        fs::FsPath mount{};

        if (location.entry.type == dump::DumpLocationType_Stdio) {
            mount = fs::AppendPath(location.stdio[location.entry.index].mount, location.stdio[location.entry.index].dump_path);
            fs = std::make_unique<fs::FsStdio>(true, location.stdio[location.entry.index].mount);
        } else if (location.entry.type == dump::DumpLocationType_SdCard) {
            fs = std::make_unique<fs::FsNativeSd>();
        } else {
            App::PushErrorBox(MAKERESULT(Module_Libnx, LibnxError_BadInput), "Invalid location type!"_i18n);
            return;
        }

        // get saves in /Saves/Name and /Saves/app_id
        filebrowser::FsDirCollection collections[2]{};
        for (auto i = 0; i < std::size(collections); i++) {
            const auto save_path = fs::AppendPath(mount, BuildSaveBasePath(m_entries[m_index], i != 0));
            filebrowser::FsView::get_collection(fs.get(), save_path, "", collections[i], true, false, false);
            // reverse as they will be sorted in oldest -> newest.
            // todo: better impl when both id and normal app folders are used.
            std::ranges::reverse(collections[i].files);
        }

        std::vector<fs::FsPath> paths;
        PopupList::Items items;
        for (const auto& collection : collections) {
            for (const auto&p : collection.files) {
                const auto view = std::string_view{p.name};
                if (view.starts_with("BCAT") || !view.ends_with(".zip")) {
                    continue;
                }

                items.emplace_back(p.name);
                paths.emplace_back(fs::AppendPath(collection.path, p.name));
            }
        }

        if (paths.empty()) {
            App::Push<ui::OptionBox>(
                i18n::Reorder("No saves found in ", fs::AppendPath(mount, BuildSaveBasePath(m_entries[m_index])).toString()),
                "OK"_i18n
            );
            return;
        }

        const auto title = "Restore save for: "_i18n + m_entries[m_index].GetName();
        App::Push<PopupList>(
            title, items, [this, paths, items, location](auto op_index){
                if (!op_index) {
                    return;
                }

                const auto file_name = items[*op_index];
                const auto file_path = paths[*op_index];

                App::Push<OptionBox>(
                    i18n::Reorder("Are you sure you want to restore ", file_name) + "?",
                    "Back"_i18n, "Restore"_i18n, 0, [this, file_path, location](auto op_index){
                        if (op_index && *op_index) {
                            App::Push<ProgressBox>(0, "Restore"_i18n, "", [this, file_path, location](auto pbox) -> Result {
                                // the entry may not have loaded yet.
                                LoadControlEntry(m_entries[m_index]);

                                if (m_auto_backup_on_restore.Get()) {
                                    pbox->SetActionName("Auto backup"_i18n);
                                    R_TRY(BackupSaveInternal(pbox, location, m_entries[m_index], BackupFlag_IsAuto));
                                }

                                pbox->SetActionName("Restore"_i18n);
                                return RestoreSaveInternal(pbox, m_entries[m_index], file_path);
                            }, [this](Result rc){
                                App::PushErrorBox(rc, "Restore failed!"_i18n);

                                if (R_SUCCEEDED(rc)) {
                                    App::Notify("Restore successfull!"_i18n);
                                }
                            });
                        }
                    }, m_entries[m_index].image
                );
            }
        );
    });
}

auto Menu::BuildSavePath(const Entry& e, u32 flags) const -> fs::FsPath {
    const auto t = std::time(NULL);
    const auto tm = std::localtime(&t);
    const auto base = BuildSaveBasePath(e);

    char time[64];
    std::snprintf(time, sizeof(time), "%u.%02u.%02u @ %02u.%02u.%02u", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

    fs::FsPath name;
    if (e.save_data_type == FsSaveDataType_Account) {
        const auto acc = m_accounts[m_account_index];

        fs::FsPath name_buf;
        if (flags & BackupFlag_IsAuto) {
            std::snprintf(name_buf, sizeof(name_buf), "AUTO - %s", acc.nickname);
        } else {
            std::snprintf(name_buf, sizeof(name_buf), "%s", acc.nickname);
        }

        title::utilsReplaceIllegalCharacters(name_buf, true);
        std::snprintf(name, sizeof(name), "%s - %s.zip", name_buf.s, time);
    } else {
        std::snprintf(name, sizeof(name), "%s.zip", time);
    }

    if (flags & BackupFlag_SetName) {
        std::string out;
        while (out.empty()) {
            const auto header = i18n::Reorder("Set name for ", e.GetName());
            if (R_FAILED(swkbd::ShowText(out, header.c_str(), "Set backup name"_i18n.c_str(), name, 1, 128))) {
                out.clear();
            }
        }

        name = out;
    }

    return fs::AppendPath(base, name);
}

Result Menu::RestoreSaveInternal(ProgressBox* pbox, const Entry& e, const fs::FsPath& path) {
    pbox->SetTitle(e.GetName());
    if (e.image) {
        pbox->SetImage(e.image);
    } else if (auto data = title::Get(e.application_id); data && !data->icon.empty()) {
        pbox->SetImageDataConst(data->icon);
    } else {
        pbox->SetImage(0);
    }

    const auto save_data_space_id = (FsSaveDataSpaceId)e.save_data_space_id;

    // try and get the journal and data size.
    FsSaveDataExtraData extra{};
    R_TRY(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), save_data_space_id, e.save_data_id));

    log_write("restoring save: %s\n", path.s);
    zlib_filefunc64_def file_func;
    mz::FileFuncStdio(&file_func);

    auto zfile = unzOpen2_64(path, &file_func);
    R_UNLESS(zfile, Result_UnzOpen2_64);
    ON_SCOPE_EXIT(unzClose(zfile));
    log_write("opened zip\n");

    std::optional<NXSaveMeta> meta{};

    // zips that were re-zipped by hand often have everything wrapped in a single folder.
    // this is only handled if the meta file is inside that folder, so that saves which
    // genuinely contain a single folder are never altered.
    std::string prefix;
    std::string meta_name = NX_SAVE_META_NAME;
    if (UNZ_END_OF_LIST_OF_FILE == unzLocateFile(zfile, NX_SAVE_META_NAME, 0) && UNZ_OK == unzGoToFirstFile(zfile)) {
        do {
            unz_file_info64 info;
            fs::FsPath name;
            if (UNZ_OK != unzGetCurrentFileInfo64(zfile, &info, name, sizeof(name), 0, 0, 0, 0)) {
                break;
            }

            const std::string_view view{name};
            const auto slash = view.find('/');
            if (slash != std::string_view::npos && slash == view.rfind('/') && view.substr(slash + 1) == NX_SAVE_META_NAME) {
                prefix = view.substr(0, slash + 1);
                meta_name = view;
                log_write("zip is wrapped in folder: %s\n", prefix.c_str());
                break;
            }
        } while (UNZ_OK == unzGoToNextFile(zfile));
    }

    // get manifest
    if (UNZ_END_OF_LIST_OF_FILE != unzLocateFile(zfile, meta_name.c_str(), 0)) {
        log_write("found meta file\n");
        if (UNZ_OK == unzOpenCurrentFile(zfile)) {
            log_write("opened meta file\n");
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            NXSaveMeta temp_meta;
            const auto len = unzReadCurrentFile(zfile, &temp_meta, sizeof(temp_meta));
            if (len == sizeof(temp_meta) && temp_meta.magic == NX_SAVE_META_MAGIC && temp_meta.version == NX_SAVE_META_VERSION) {
                meta = temp_meta;
                log_write("loaded meta!\n");
            }
        }
    }

    if (meta.has_value()) {
        // extending to a size that is not larger than the current one fails with FsError_InvalidSize.
        const auto data_size = std::max<s64>(meta->data_size, extra.data_size);
        const auto journal_size = std::max<s64>(meta->journal_size, extra.journal_size);
        if (data_size > extra.data_size || journal_size > extra.journal_size) {
            log_write("extending save file\n");
            R_TRY(fsExtendSaveDataFileSystem(save_data_space_id, e.save_data_id, data_size, journal_size));
            log_write("extended save file\n");
        }
    } else {
        log_write("doing manual meta parse\n");
        s64 total_size{};

        // todo:: manually calculate / guess the save size.
        unz_global_info64 ginfo;
        R_UNLESS(UNZ_OK == unzGetGlobalInfo64(zfile, &ginfo), Result_UnzGetGlobalInfo64);
        R_UNLESS(UNZ_OK == unzGoToFirstFile(zfile), Result_UnzGoToFirstFile);

        for (s64 i = 0; i < ginfo.number_entry; i++) {
            R_TRY(pbox->ShouldExitResult());

            if (i > 0) {
                R_UNLESS(UNZ_OK == unzGoToNextFile(zfile), Result_UnzGoToNextFile);
            }

            R_UNLESS(UNZ_OK == unzOpenCurrentFile(zfile), Result_UnzOpenCurrentFile);
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            fs::FsPath name;
            R_UNLESS(UNZ_OK == unzGetCurrentFileInfo64(zfile, &info, name, sizeof(name), 0, 0, 0, 0), Result_UnzGetCurrentFileInfo64);

            if (name == meta_name.c_str() || !std::string_view{name}.starts_with(prefix)) {
                continue;
            }
            total_size += info.uncompressed_size;
        }

        // save data is allocated in 16 KiB blocks, so round up.
        // only extend if the zip does not already fit, as shrinking / same size is invalid.
        constexpr s64 BLOCK_SIZE = 0x4000;
        const auto rounded_size = (total_size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);
        if (rounded_size > extra.data_size) {
            log_write("extending manual meta parse\n");
            R_TRY(fsExtendSaveDataFileSystem(save_data_space_id, e.save_data_id, rounded_size, extra.journal_size));
            log_write("extended manual meta parse\n");
        }
    }

    FsSaveDataAttribute attr{};
    attr.application_id = e.application_id;
    attr.uid = e.uid;
    attr.system_save_data_id = e.system_save_data_id;
    attr.save_data_type = e.save_data_type;
    attr.save_data_rank = e.save_data_rank;
    attr.save_data_index = e.save_data_index;

    // try and open the save file system.
    fs::FsNativeSave save_fs{(FsSaveDataType)e.save_data_type, save_data_space_id, &attr, false};
    R_TRY(save_fs.GetFsOpenResult());

    // delete all files in save.
    filebrowser::FsDirCollections collections;
    R_TRY(filebrowser::FsView::get_collections(&save_fs, "/", "", collections));
    R_TRY(filebrowser::FsView::DeleteAllCollections(pbox, &save_fs, collections));

    log_write("opened save file\n");
    // restore save data from zip.
    R_TRY(thread::TransferUnzipAll(pbox, zfile, &save_fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
        // skip restoring the meta file.
        if (name == meta_name.c_str()) {
            log_write("skipping meta\n");
            return false;
        }

        // skip anything outside of the wrapper folder and strip the folder from the rest.
        if (!prefix.empty()) {
            if (!std::string_view{name}.starts_with(prefix) || std::string_view{name}.size() == prefix.size()) {
                return false;
            }

            path = fs::AppendPath("/", name.s + prefix.size());
        }

        // restore everything else.
        log_write("restoring: %s\n", path.s);
        return true;
    }));

    log_write("finished save backup\n");
    R_SUCCEED();
}

Result Menu::BackupSaveInternal(ProgressBox* pbox, const dump::DumpLocation& location, std::span<const std::reference_wrapper<Entry>> entries, u32 flags) {
    std::vector<fs::FsPath> paths;
    for (auto& e : entries) {
        // ensure that we have title name and icon loaded.
        LoadControlEntry(e);
        paths.emplace_back(BuildSavePath(e, flags));
    }

    const auto compressed = m_compress_save_backup.Get();
    auto source = std::make_shared<DumpSource>(entries, paths);

    return dump::Dump(pbox, source, location, paths, [&](ui::ProgressBox* pbox, dump::BaseSource* _source, dump::WriteSource* writer, const fs::FsPath& path) -> Result {
        const auto source = (DumpSource*)_source;
        const auto& e = source->GetEntry(path);

        pbox->SetTitle(e.GetName());
        if (e.image) {
            pbox->SetImage(e.image);
        } else if (auto data = title::Get(e.application_id); data && !data->icon.empty()) {
            pbox->SetImageDataConst(data->icon);
        } else {
            pbox->SetImage(0);
        }

        const auto save_data_space_id = (FsSaveDataSpaceId)e.save_data_space_id;

        // try and get the journal and data size.
        FsSaveDataExtraData extra{};
        R_TRY(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), save_data_space_id, e.save_data_id));

        FsSaveDataAttribute attr{};
        attr.application_id = e.application_id;
        attr.uid = e.uid;
        attr.system_save_data_id = e.system_save_data_id;
        attr.save_data_type = e.save_data_type;
        attr.save_data_rank = e.save_data_rank;
        attr.save_data_index = e.save_data_index;

        // try and open the save file system
        fs::FsNativeSave save_fs{(FsSaveDataType)e.save_data_type, save_data_space_id, &attr, true};
        R_TRY(save_fs.GetFsOpenResult());

        // get a list of collections.
        filebrowser::FsDirCollections collections;
        R_TRY(filebrowser::FsView::get_collections(&save_fs, "/", "", collections));

        // the save file may be empty, this isn't an error, but we exit early.
        R_UNLESS(!collections.empty(), 0x0);

        const auto t = (time_t)extra.timestamp;
        const auto tm = std::localtime(&t);

        // pre-calculate the time rather than calculate it in the loop.
        zip_fileinfo zip_info_default{};
        zip_info_default.tmz_date.tm_sec = tm->tm_sec;
        zip_info_default.tmz_date.tm_min = tm->tm_min;
        zip_info_default.tmz_date.tm_hour = tm->tm_hour;
        zip_info_default.tmz_date.tm_mday = tm->tm_mday;
        zip_info_default.tmz_date.tm_mon = tm->tm_mon;
        zip_info_default.tmz_date.tm_year = tm->tm_year;

        // zip to memory if less than 1GB and not applet mode.
        // TODO: use my mmz code from ftpsrv to stream zip creation.
        // this will allow for zipping to memory and flushing every X bytes
        // such as flushing every 8MB.
        const auto file_download = App::IsApplet() || e.size >= 1024ULL * 1024ULL * 1024ULL;

        mz::MzMem mz_mem{};
        zlib_filefunc64_def file_func;
        if (!file_download) {
            mz::FileFuncMem(&mz_mem, &file_func);
        } else {
            dump::FileFuncWriter(writer, &file_func);
        }

        {
            auto zfile = zipOpen2_64(path, APPEND_STATUS_CREATE, nullptr, &file_func);
            R_UNLESS(zfile, Result_ZipOpen2_64);
            ON_SCOPE_EXIT(zipClose(zfile, "sphaira v" APP_DISPLAY_VERSION));

            // add save meta.
            {
                const NXSaveMeta meta{
                    .magic = NX_SAVE_META_MAGIC,
                    .version = NX_SAVE_META_VERSION,
                    .attr = extra.attr,
                    .owner_id = extra.owner_id,
                    .timestamp = extra.timestamp,
                    .flags = extra.flags,
                    .unk_x54 = extra.unk_x54,
                    .data_size = extra.data_size,
                    .journal_size = extra.journal_size,
                    .commit_id = extra.commit_id,
                    .raw_size = e.size,
                };

                R_UNLESS(ZIP_OK == zipOpenNewFileInZip(zfile, NX_SAVE_META_NAME, &zip_info_default, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_NO_COMPRESSION), Result_ZipOpenNewFileInZip);
                ON_SCOPE_EXIT(zipCloseFileInZip(zfile));
                R_UNLESS(ZIP_OK == zipWriteInFileInZip(zfile, &meta, sizeof(meta)), Result_ZipWriteInFileInZip);
            }

            const auto zip_add = [&](const fs::FsPath& file_path) -> Result {
                const char* file_name_in_zip = file_path.s;

                // strip root path (/ or ums0:)
                if (!std::strncmp(file_name_in_zip, save_fs.Root(), std::strlen(save_fs.Root()))) {
                    file_name_in_zip += std::strlen(save_fs.Root());
                }

                // root paths are banned in zips, they will warn when extracting otherwise.
                while (file_name_in_zip[0] == '/') {
                    file_name_in_zip++;
                }

                pbox->NewTransfer(file_name_in_zip);

                const auto level = compressed ? Z_DEFAULT_COMPRESSION : Z_NO_COMPRESSION;
                if (ZIP_OK != zipOpenNewFileInZip(zfile, file_name_in_zip, &zip_info_default, NULL, 0, NULL, 0, NULL, Z_DEFLATED, level)) {
                    log_write("failed to add zip for %s\n", file_path.s);
                    R_THROW(Result_ZipOpenNewFileInZip);
                }
                ON_SCOPE_EXIT(zipCloseFileInZip(zfile));

                return thread::TransferZip(pbox, zfile, &save_fs, file_path);
            };

            // loop through every save file and store to zip.
            for (const auto& collection : collections) {
                for (const auto& file : collection.files) {
                    const auto file_path = fs::AppendPath(collection.path, file.name);
                    R_TRY(zip_add(file_path));
                }
            }
        }

        // if we dumped the save to ram, flush the data to file.
        if (!file_download) {
            pbox->NewTransfer("Flushing zip to file"_i18n);
            R_TRY(writer->SetSize(mz_mem.buf.size()));

            R_TRY(thread::Transfer(pbox, mz_mem.buf.size(),
                [&](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
                    size = std::min<s64>(size, mz_mem.buf.size() - off);
                    std::memcpy(data, mz_mem.buf.data() + off, size);
                    *bytes_read = size;
                    R_SUCCEED();
                },
                [&](const void* data, s64 off, s64 size) -> Result {
                    return writer->Write(data, off, size);
                }
            ));
        }

        R_SUCCEED();
    });
}

Result Menu::BackupSaveInternal(ProgressBox* pbox, const dump::DumpLocation& location, Entry& e, u32 flags) {
    std::vector<std::reference_wrapper<Entry>> entries;
    entries.emplace_back(e);

    return BackupSaveInternal(pbox, location, entries, flags);
}

Result Menu::MountSaveFs() {
    const auto& e = m_entries[m_index];

    if (e.system_save_data_id) {
        fs::FsPath root;
        R_TRY(devoptab::MountSaveSystem(e.system_save_data_id, root));

        auto fs = std::make_shared<filebrowser::FsStdioWrapper>(root, [root](){
            devoptab::UmountNeworkDevice(root);
        });

        filebrowser::MountFsHelper(fs, e.GetName());
    } else {
        const auto save_data_space_id = (FsSaveDataSpaceId)e.save_data_space_id;

        FsSaveDataAttribute attr{};
        attr.application_id = e.application_id;
        attr.uid = e.uid;
        attr.system_save_data_id = e.system_save_data_id;
        attr.save_data_type = e.save_data_type;
        attr.save_data_rank = e.save_data_rank;
        attr.save_data_index = e.save_data_index;

        auto fs = std::make_shared<fs::FsNativeSave>((FsSaveDataType)e.save_data_type, save_data_space_id, &attr, true);
        R_TRY(fs->GetFsOpenResult());
        filebrowser::MountFsHelper(fs, e.GetName());
    }

    R_SUCCEED();
}

} // namespace sphaira::ui::menu::save

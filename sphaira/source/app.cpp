#include "ui/option_box.hpp"
#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "ui/menus/album_menu.hpp"
#include "ui/menus/main_menu.hpp"

#include "app.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "nro.hpp"
#include "nacp_compat.hpp"
#include "evman.hpp"
#include "owo.hpp"
#include "image.hpp"
#include "nxlink.h"
#include "fs.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "ftpsrv_helper.hpp"
#include "haze_helper.hpp"
#include "web.hpp"
#include "swkbd.hpp"
#include "usbdvd.hpp"

#include "utils/profile.hpp"
#include "utils/thread.hpp"
#include "utils/devoptab.hpp"

#include <nanovg_dk.h>
#include <minIni.h>
#include <algorithm>
#include <ranges>
#include <cassert>
#include <cstring>
#include <ctime>
#include <span>
#include <dirent.h>

#ifdef ENABLE_LIBUSBHSFS
    #include <usbhsfs.h>
#endif // ENABLE_LIBUSBHSFS

extern "C" {
    u32 __nx_applet_exit_mode = 0;
} // extern "C"

namespace sphaira {

#ifdef ENABLE_LIBUSBHSFS
namespace {

// builds the libusbhsfs mount flags out of the user's hdd options and pushes
// them to the library. only affects devices mounted *after* this call, use
// App::RemountHdd() if the change should be visible right away.
//
// most of these are NTFS only, they are ignored for FAT/exFAT volumes.
void ApplyHddMountFlags() {
    u32 flags = UsbHsFsMountFlags_None;

    if (App::GetWriteProtect()) {
        flags |= UsbHsFsMountFlags_ReadOnly;
    } else {
        // an NTFS volume with a dirty journal (the normal state after the drive
        // was pulled out of a pc without ejecting it) is refused for writing
        // unless the journal is replayed first.
        flags |= UsbHsFsMountFlags_ReplayJournal;
    }

    // windows marks plenty of ordinary folders as hidden, without this they
    // would silently not show up in the file browser.
    flags |= UsbHsFsMountFlags_ShowHiddenFiles;

    // deliberately *not* setting UsbHsFsMountFlags_UpdateAccessTimes (which is
    // part of the library default), writing back an access time after every
    // read costs a lot of speed when installing from an NTFS drive.

    if (App::GetHddShowSystemFiles()) {
        flags |= UsbHsFsMountFlags_ShowSystemFiles;
    }

    if (App::GetHddIgnoreHibernation()) {
        flags |= UsbHsFsMountFlags_IgnoreHibernation;
    }

    usbHsFsSetFileSystemMountFlags(flags);
}

} // namespace
#endif // ENABLE_LIBUSBHSFS

namespace {

constexpr const u8 DEFAULT_IMAGE_DATA[]{
    #embed <icons/default.png>
};

struct ThemeData {
    fs::FsPath music_path{};
    std::string elements[ThemeEntryID_MAX]{};
};

struct ThemeIdPair {
    const char* label;
    ThemeEntryID id;
    ElementType type{ElementType::None};
};

struct FrameBufferSize {
    Vec2 size;
    Vec2 scale;
};

struct NszOption {
    int value;
    const char* name;
};

constexpr KeyboardState::MapEntry KEYBOARD_BUTTON_MAP[] = {
    {HidKeyboardKey_UpArrow,        static_cast<u64>(Button::DPAD_UP)},
    {HidKeyboardKey_DownArrow,      static_cast<u64>(Button::DPAD_DOWN)},
    {HidKeyboardKey_LeftArrow,      static_cast<u64>(Button::DPAD_LEFT)},
    {HidKeyboardKey_RightArrow,     static_cast<u64>(Button::DPAD_RIGHT)},

    {HidKeyboardKey_W,              static_cast<u64>(Button::DPAD_UP)},
    {HidKeyboardKey_S,              static_cast<u64>(Button::DPAD_DOWN)},
    {HidKeyboardKey_A,              static_cast<u64>(Button::DPAD_LEFT)},
    {HidKeyboardKey_D,              static_cast<u64>(Button::DPAD_RIGHT)},

    // options (may swap).
    {HidKeyboardKey_Z,              static_cast<u64>(Button::Y)},
    {HidKeyboardKey_X,              static_cast<u64>(Button::X)},

    // menus.
    {HidKeyboardKey_Q,              static_cast<u64>(Button::L)},
    {HidKeyboardKey_E,              static_cast<u64>(Button::R)},

    // select and back.
    {HidKeyboardKey_Return,         static_cast<u64>(Button::A)},
    {HidKeyboardKey_Space,          static_cast<u64>(Button::A)},
    {HidKeyboardKey_Backspace,      static_cast<u64>(Button::B)},

    // exit.
    {HidKeyboardKey_Escape,         static_cast<u64>(Button::START)},

    // idk what this should map to.
    {HidKeyboardKey_R,              static_cast<u64>(Button::SELECT)},
};

constexpr NszOption NSZ_COMPRESS_LEVEL_OPTIONS[] = {
    { .value = 0, .name = "Level 0 (no compression)" },
    { .value = 1, .name = "Level 1" },
    { .value = 2, .name = "Level 2" },
    { .value = 3, .name = "Level 3 (default)" },
    { .value = 4, .name = "Level 4" },
    { .value = 5, .name = "Level 5" },
    { .value = 6, .name = "Level 6" },
    { .value = 7, .name = "Level 7" },
    { .value = 8, .name = "Level 8" },
};

constexpr NszOption NSZ_COMPRESS_THREAD_OPTIONS[] = {
    { .value = 0, .name = "0 (single threaded)" },
    { .value = 1, .name = "1" },
    { .value = 2, .name = "2" },
    { .value = 3, .name = "3 (default)" },
    { .value = 4, .name = "4" },
};

constexpr NszOption NSZ_COMPRESS_BLOCK_OPTIONS[] = {
    { .value = 14, .name = "16 KB" },
    { .value = 15, .name = "32 KB" },
    { .value = 16, .name = "64 KB" },
    { .value = 17, .name = "128 KB" },
    { .value = 18, .name = "256 KB" },
    { .value = 19, .name = "512 KB" },
    { .value = 20, .name = "1 MB (default)" },
    { .value = 21, .name = "2 MB" },
    { .value = 22, .name = "4 MB" },
    { .value = 23, .name = "8 MB" },
    { .value = 24, .name = "16 MB" },
};

constexpr ThemeIdPair THEME_ENTRIES[] = {
    { "background", ThemeEntryID_BACKGROUND },
    { "grid", ThemeEntryID_GRID },
    { "text", ThemeEntryID_TEXT, ElementType::Colour },
    { "text_info", ThemeEntryID_TEXT_INFO, ElementType::Colour },
    { "text_selected", ThemeEntryID_TEXT_SELECTED, ElementType::Colour },
    { "selected_background", ThemeEntryID_SELECTED_BACKGROUND, ElementType::Colour },
    { "error", ThemeEntryID_ERROR, ElementType::Colour },
    { "popup", ThemeEntryID_POPUP, ElementType::Colour },
    { "focus", ThemeEntryID_FOCUS, ElementType::Colour },
    { "line", ThemeEntryID_LINE, ElementType::Colour },
    { "line_separator", ThemeEntryID_LINE_SEPARATOR, ElementType::Colour },
    { "sidebar", ThemeEntryID_SIDEBAR, ElementType::Colour },
    { "scrollbar", ThemeEntryID_SCROLLBAR, ElementType::Colour },
    { "scrollbar_background", ThemeEntryID_SCROLLBAR_BACKGROUND, ElementType::Colour },
    { "progressbar", ThemeEntryID_PROGRESSBAR, ElementType::Colour },
    { "progressbar_background", ThemeEntryID_PROGRESSBAR_BACKGROUND, ElementType::Colour },
    { "highlight_1", ThemeEntryID_HIGHLIGHT_1, ElementType::Colour },
    { "highlight_2", ThemeEntryID_HIGHLIGHT_2, ElementType::Colour },
    { "icon_colour", ThemeEntryID_ICON_COLOUR, ElementType::Colour },
    { "icon_audio", ThemeEntryID_ICON_AUDIO, ElementType::Texture },
    { "icon_video", ThemeEntryID_ICON_VIDEO, ElementType::Texture },
    { "icon_image", ThemeEntryID_ICON_IMAGE, ElementType::Texture },
    { "icon_file", ThemeEntryID_ICON_FILE, ElementType::Texture },
    { "icon_folder", ThemeEntryID_ICON_FOLDER, ElementType::Texture },
    { "icon_zip", ThemeEntryID_ICON_ZIP, ElementType::Texture },
    { "icon_nro", ThemeEntryID_ICON_NRO, ElementType::Texture },
};

constinit App* g_app{};
Mutex g_screen_toggle_mutex{};
u32 g_screen_toggle_ref_count{};
bool g_screen_disabled{};

void deko3d_error_cb(void* userData, const char* context, DkResult result, const char* message) {
    switch (result) {
        case DkResult_Success:
            break;

        case DkResult_Fail:
            log_write("[DkResult_Fail] %s\n", message);
            App::Notify("DkResult_Fail");
            break;

        case DkResult_Timeout:
            log_write("[DkResult_Timeout] %s\n", message);
            App::Notify("DkResult_Timeout");
            break;

        case DkResult_OutOfMemory:
            log_write("[DkResult_OutOfMemory] %s\n", message);
            App::Notify("DkResult_OutOfMemory");
            break;

        case DkResult_NotImplemented:
            log_write("[DkResult_NotImplemented] %s\n", message);
            App::Notify("DkResult_NotImplemented");
            break;

        case DkResult_MisalignedSize:
            log_write("[DkResult_MisalignedSize] %s\n", message);
            App::Notify("DkResult_MisalignedSize");
            break;

        case DkResult_MisalignedData:
            log_write("[DkResult_MisalignedData] %s\n", message);
            App::Notify("DkResult_MisalignedData");
            break;

        case DkResult_BadInput:
            log_write("[DkResult_BadInput] %s\n", message);
            App::Notify("DkResult_BadInput");
            break;

        case DkResult_BadFlags:
            log_write("[DkResult_BadFlags] %s\n", message);
            App::Notify("DkResult_BadFlags");
            break;

        case DkResult_BadState:
            log_write("[DkResult_BadState] %s\n", message);
            App::Notify("DkResult_BadState");
            break;
    }
}

void on_applet_focus_state(App* app) {
    switch (appletGetFocusState()) {
        case AppletFocusState_InFocus:
            log_write("[APPLET] AppletFocusState_InFocus\n");
            // App::Notify("AppletFocusState_InFocus");
            break;

        case AppletFocusState_OutOfFocus:
            log_write("[APPLET] AppletFocusState_OutOfFocus\n");
            // App::Notify("AppletFocusState_OutOfFocus");
            break;

        case AppletFocusState_Background:
            log_write("[APPLET] AppletFocusState_Background\n");
            // App::Notify("AppletFocusState_Background");
            break;
    }
}

void on_applet_operation_mode(App* app) {
    switch (appletGetOperationMode()) {
        case AppletOperationMode_Handheld:
            log_write("[APPLET] AppletOperationMode_Handheld\n");
            App::Notify("Switch-Handheld!"_i18n);
            break;

        case AppletOperationMode_Console:
            log_write("[APPLET] AppletOperationMode_Console\n");
            App::Notify("Switch-Docked!"_i18n);
            break;
    }
}

void applet_on_performance_mode(App* app) {
    switch (appletGetPerformanceMode()) {
        case ApmPerformanceMode_Invalid:
            log_write("[APPLET] ApmPerformanceMode_Invalid\n");
            App::Notify("ApmPerformanceMode_Invalid");
            break;

        case ApmPerformanceMode_Normal:
            log_write("[APPLET] ApmPerformanceMode_Normal\n");
            App::Notify("ApmPerformanceMode_Normal");
            break;

        case ApmPerformanceMode_Boost:
            log_write("[APPLET] ApmPerformanceMode_Boost\n");
            App::Notify("ApmPerformanceMode_Boost");
            break;
    }
}

void appplet_hook_calback(AppletHookType type, void *param) {
    auto app = static_cast<App*>(param);
    switch (type) {
        case AppletHookType_OnFocusState:
            log_write("[APPLET] AppletHookType_OnFocusState\n");
            // App::Notify("AppletHookType_OnFocusState");
            on_applet_focus_state(app);
            break;

        case AppletHookType_OnOperationMode:
            log_write("[APPLET] AppletHookType_OnOperationMode\n");
            // App::Notify("AppletHookType_OnOperationMode");
            on_applet_operation_mode(app);
            break;

        case AppletHookType_OnPerformanceMode:
            log_write("[APPLET] AppletHookType_OnPerformanceMode\n");
            // App::Notify("AppletHookType_OnPerformanceMode");
            applet_on_performance_mode(app);
            break;

        case AppletHookType_OnExitRequest:
            log_write("[APPLET] AppletHookType_OnExitRequest\n");
            // App::Notify("AppletHookType_OnExitRequest");
            break;

        case AppletHookType_OnResume:
            log_write("[APPLET] AppletHookType_OnResume\n");
            // App::Notify("AppletHookType_OnResume");
            break;

        case AppletHookType_OnCaptureButtonShortPressed:
            log_write("[APPLET] AppletHookType_OnCaptureButtonShortPressed\n");
            // App::Notify("AppletHookType_OnCaptureButtonShortPressed");
            break;

        case AppletHookType_OnAlbumScreenShotTaken:
            log_write("[APPLET] AppletHookType_OnAlbumScreenShotTaken\n");
            // so that an open album menu picks the new capture up.
            ui::menu::album::SignalChange();
            break;

        case AppletHookType_RequestToDisplay:
            log_write("[APPLET] AppletHookType_RequestToDisplay\n");
            // App::Notify("AppletHookType_RequestToDisplay");
            break;

        case AppletHookType_Max:
            assert(!"AppletHookType_Max hit");
            break;
    }
}

auto GetFrameBufferSize() -> FrameBufferSize {
    FrameBufferSize fb{};

    switch (appletGetOperationMode()) {
        case AppletOperationMode_Handheld:
            fb.size.x = 1280;
            fb.size.y = 720;
            break;

        case AppletOperationMode_Console:
            fb.size.x = 1920;
            fb.size.y = 1080;
            break;
    }

    fb.scale.x = fb.size.x / SCREEN_WIDTH;
    fb.scale.y = fb.size.y / SCREEN_HEIGHT;
    return fb;
}

// this will try to decompress the icon and then re-convert it to jpg
// in order to strip exif data.
// this doesn't take long at all, but it's very overkill.
// todo: look into jpeg/exif spec to manually strip data
auto GetNroIcon(const std::vector<u8>& nro_icon) -> std::vector<u8> {
    auto icon = ImageNormalizeIcon(nro_icon);
    if (!icon.empty()) {
        return icon;
    }

    icon = ImageNormalizeIcon(DEFAULT_IMAGE_DATA);
    if (!icon.empty()) {
        return icon;
    }
    return {std::begin(DEFAULT_IMAGE_DATA), std::end(DEFAULT_IMAGE_DATA)};
}

auto LoadThemeMeta(const fs::FsPath& path, ThemeMeta& meta) -> bool {
    meta = {};

    auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto meta = static_cast<ThemeMeta*>(UserData);

        if (!std::strcmp(Section, "meta")) {
            if (!std::strcmp(Key, "name")) {
                meta->name = Value;
            } else if (!std::strcmp(Key, "author")) {
                meta->author = Value;
            } else if (!std::strcmp(Key, "version")) {
                meta->version = Value;
            } else if (!std::strcmp(Key, "inherit")) {
                meta->inherit = Value;
            }

            return 1;
        }

        return 0;
    };

    if (!ini_browse(cb, &meta, path)) {
        return false;
    }

    if (meta.name.empty() || meta.author.empty() || meta.version.empty()) {
        return false;
    }

    log_write("loaded meta from: %s\n", path.s);
    meta.ini_path = path;
    return true;
}

void LoadThemeInternal(ThemeMeta meta, ThemeData& theme_data, int inherit_level = 0) {
    constexpr auto inherit_level_max = 5;

    // all themes will inherit from black theme by default.
    if (meta.inherit.empty() && !inherit_level) {
        meta.inherit = "romfs:/themes/base_black_theme.ini";
    }

    // check if the theme inherits from another, if so, load it.
    // block inheriting from itself.
    if (inherit_level < inherit_level_max && !meta.inherit.empty() && strcasecmp(meta.inherit, "none") && meta.inherit != meta.ini_path) {
        log_write("inherit is not empty: %s\n", meta.inherit.s);
        if (R_SUCCEEDED(romfsInit())) {
            ThemeMeta inherit_meta;
            const auto has_meta = LoadThemeMeta(meta.inherit, inherit_meta);
            romfsExit();

            // base themes do not have a meta
            if (!has_meta) {
                inherit_meta.ini_path = meta.inherit;
            }

            LoadThemeInternal(inherit_meta, theme_data, inherit_level + 1);
        }
    }

    auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto theme_data = static_cast<ThemeData*>(UserData);

        if (!std::strcmp(Section, "theme")) {
            if (!std::strcmp(Key, "music")) {
                theme_data->music_path = Value;
            } else {
                for (auto& e : THEME_ENTRIES) {
                    if (!std::strcmp(Key, e.label)) {
                        theme_data->elements[e.id] = Value;
                        break;
                    }
                }
            }
        }

        return 1;
    };

    if (R_SUCCEEDED(romfsInit())) {
        ON_SCOPE_EXIT(romfsExit());

        if (!ini_browse(cb, &theme_data, meta.ini_path)) {
            log_write("failed to open ini: %s\n", meta.ini_path.s);
        } else {
            log_write("opened ini: %s\n", meta.ini_path.s);
        }
    }
}

void nxlink_callback(const NxlinkCallbackData *data) {
    App::NotifyFlashLed();
    evman::push(*data, false);
}

void on_i18n_change() {
    i18n::exit();
    i18n::init(App::GetLanguage());
}

} // namespace

void App::Loop() {
    // adjust these if FPSlocker ever supports different min/max fps.
    constexpr double min_delta    = 1000.0 / 120.0; // 120 fps
    constexpr double max_delta    = 1000.0 / 15.0;  // 15  fps
    constexpr double target_delta = 1000.0 / 60.0;  // 60  fps

    u64 start = armTicksToNs(armGetSystemTick());
    m_delta_time = 1.0;

    while (!m_quit && appletMainLoop()) {
        if (m_widgets.empty()) {
            m_quit = true;
            break;
        }

        ui::gfx::updateHighlightAnimation();

        // fire all events in in a 3ms timeslice
        TimeStamp ts_event;
        const u64 event_timeout = 3;

        // limit events to a max per frame in order to not block for too long.
        while (true) {
            if (ts_event.GetMs() >= event_timeout) {
                log_write("event loop timed-out\n");
                break;
            }

            auto event = evman::pop();
            if (!event.has_value()) {
                break;
            }

            std::visit([this](auto&& arg){
                using T = std::decay_t<decltype(arg)>;
                if constexpr(std::is_same_v<T, evman::LaunchNroEventData>) {
                    log_write("[LaunchNroEventData] got event\n");
                    u64 timestamp = 0;
                    timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp);
                    const auto nro_path = nro_normalise_path(arg.path);

                    // update timestamp
                    ini_putl(nro_path.c_str(), "timestamp", timestamp, App::PLAYLOG_PATH);
                    log_write("updating timestamp for: %s %lu\n", nro_path.c_str(), timestamp);

                    // force disable pop-back to main menu.
                    __nx_applet_exit_mode = 0;
                    m_quit = true;
                } else if constexpr(std::is_same_v<T, evman::ExitEventData>) {
                    log_write("[ExitEventData] got event\n");
                    m_quit = true;
                } else if constexpr(std::is_same_v<T, NxlinkCallbackData>) {
                    switch (arg.type) {
                        case NxlinkCallbackType_Connected:
                            log_write("[NxlinkCallbackType_Connected]\n");
                            App::Notify("Nxlink Connected"_i18n);
                            break;
                        case NxlinkCallbackType_WriteBegin:
                            log_write("[NxlinkCallbackType_WriteBegin] %s\n", arg.file.filename);
                            App::Notify("Nxlink Upload"_i18n);
                            break;
                        case NxlinkCallbackType_WriteProgress:
                            // log_write("[NxlinkCallbackType_WriteProgress]\n");
                            break;
                        case NxlinkCallbackType_WriteEnd:
                            log_write("[NxlinkCallbackType_WriteEnd] %s\n", arg.file.filename);
                            App::Notify("Nxlink Finished"_i18n);
                            break;
                    }
                } else if constexpr(std::is_same_v<T, curl::DownloadEventData>) {
                    log_write("[DownloadEventData] got event\n");
                    if (arg.callback && !arg.stoken.stop_requested()) {
                        arg.callback(arg.result);
                    }
                } else {
                    static_assert(false, "non-exhaustive visitor!");
                }
            }, event.value());
        }

        const auto fb = GetFrameBufferSize();
        if (fb.size.x != s_width || fb.size.y != s_height) {
            s_width = fb.size.x;
            s_height = fb.size.y;
            m_scale = fb.scale;
            this->destroyFramebufferResources();
            this->createFramebufferResources();
            renderer->UpdateViewSize(s_width, s_height);
        }

        this->Poll();
        this->Update();
        this->Draw();

        // check how long this frame took.
        const u64 now = armTicksToNs(armGetSystemTick());
        // convert to ns.
        const double delta = (double)(now - start) / 1e+6;
        // clamp and normalise to 1.0 as the target, higher values if we took too long.
        m_delta_time = std::clamp(delta, min_delta, max_delta) / target_delta;
        // save timestamp for next frame.
        start = now;
    }
}

auto App::Push(std::unique_ptr<ui::Widget>&& widget) -> void {
    log_write("[APP] pushing widget\n");

    // when freeing widges, this may cancel a transfer which causes it to push
    // an error box, so check if we are quitting first before adding.
    if (g_app->m_quit) {
        log_write("[APP] is quitting, not pushing widget\n");
        return;
    }

    // check if the widget wants to pop before adding.
    // this can happen if something failed in the constructor and the widget wants to exit.
    if (widget->ShouldPop()) {
        return;
    }

    if (!g_app->m_widgets.empty()) {
        g_app->m_widgets.back()->OnFocusLost();
    }

    log_write("doing focus gained\n");
    g_app->m_widgets.emplace_back(std::forward<decltype(widget)>(widget))->OnFocusGained();
    log_write("did it\n");
}

auto App::PopToMenu() -> void {
    for (auto& p : std::ranges::views::reverse(g_app->m_widgets)) {
        if (p->IsMenu()) {
            break;
        }

        p->SetPop();
    }
}

void App::Notify(const std::string& text, ui::NotifEntry::Side side) {
    g_app->m_notif_manager.Push({text, side});
}

void App::Notify(ui::NotifEntry entry) {
    g_app->m_notif_manager.Push(entry);
}

void App::NotifyPop(ui::NotifEntry::Side side) {
    g_app->m_notif_manager.Pop(side);
}

void App::NotifyClear(ui::NotifEntry::Side side) {
    g_app->m_notif_manager.Clear(side);
}

void App::NotifyFlashLed() {
    static constexpr HidsysNotificationLedPattern pattern = {
        .baseMiniCycleDuration = 0x1,             // 12.5ms.
        .totalMiniCycles = 0x1,                   // 1 mini cycle(s).
        .totalFullCycles = 0x1,                   // 1 full run(s).
        .startIntensity = 0xF,                    // 100%.
        .miniCycles = {{
            .ledIntensity = 0xF,                  // 100%.
            .transitionSteps = 0xF,               // 1 step(s). Total 12.5ms.
            .finalStepDuration = 0xF,             // Forced 12.5ms.
        }}
    };

    Result rc;
    s32 total;
    HidsysUniquePadId unique_pad_id;

    rc = hidsysGetUniquePadsFromNpad(HidNpadIdType_Handheld, &unique_pad_id, 1, &total);
    if (R_SUCCEEDED(rc) && total) {
        rc = hidsysSetNotificationLedPattern(&pattern, unique_pad_id);
    }

    if (R_FAILED(rc) || !total) {
        rc = hidsysGetUniquePadsFromNpad(HidNpadIdType_No1, &unique_pad_id, 1, &total);
        if (R_SUCCEEDED(rc) && total) {
            hidsysSetNotificationLedPattern(&pattern, unique_pad_id);
        }
    }
}

Result App::PushErrorBox(Result rc, const std::string& message) {
    if (R_FAILED(rc)) {
        App::Push<ui::ErrorBox>(rc, message);
    }
    return rc;
}

auto App::GetThemeMetaList() -> std::span<ThemeMeta> {
    return g_app->m_theme_meta_entries;
}

void App::SetTheme(s64 theme_index) {
    g_app->LoadTheme(g_app->m_theme_meta_entries[theme_index]);
    g_app->m_theme_index = theme_index;
}

auto App::GetThemeIndex() -> s64 {
    return g_app->m_theme_index;
}

auto App::GetDefaultImage() -> int {
    return g_app->m_default_image;
}

auto App::GetDefaultImageData() -> std::span<const u8> {
    return DEFAULT_IMAGE_DATA;
}

auto App::GetExePath() -> fs::FsPath {
    return g_app->m_app_path;
}

auto App::IsHbmenu() -> bool {
    return !strcasecmp(GetExePath().s, "/hbmenu.nro");
}

auto App::GetNxlinkEnable() -> bool {
    return g_app->m_nxlink_enabled.Get();
}

auto App::GetHddEnable() -> bool {
    return g_app->m_hdd_enabled.Get();
}

auto App::GetWriteProtect() -> bool {
    return g_app->m_hdd_write_protect.Get();
}

auto App::GetHddShowSystemFiles() -> bool {
    return g_app->m_hdd_show_system_files.Get();
}

auto App::GetHddIgnoreHibernation() -> bool {
    return g_app->m_hdd_ignore_hibernation.Get();
}

auto App::GetLogEnable() -> bool {
    return g_app->m_log_enabled.Get();
}

auto App::GetReplaceHbmenuEnable() -> bool {
    return g_app->m_replace_hbmenu.Get();
}

auto App::GetInstallEnable() -> bool {
    if (IsEmummc()) {
        return GetInstallEmummcEnable();
    } else {
        return GetInstallSysmmcEnable();
    }
}

auto App::GetInstallSysmmcEnable() -> bool {
    return g_app->m_install_sysmmc.GetOr("install");
}

auto App::GetInstallEmummcEnable() -> bool {
    return g_app->m_install_emummc.GetOr("install");
}

auto App::GetInstallSdEnable() -> bool {
    return g_app->m_install_sd.Get();
}

auto App::GetThemeMusicEnable() -> bool {
    return g_app->m_theme_music.Get();
}

auto App::GetMtpEnable() -> bool {
    return g_app->m_mtp_enabled.Get();
}

auto App::GetFtpEnable() -> bool {
    return g_app->m_ftp_enabled.Get();
}

auto App::GetLanguage() -> long {
    return g_app->m_language.Get();
}

auto App::GetTextScrollSpeed() -> long {
    return g_app->m_text_scroll_speed.Get();
}

auto App::GetNszCompressLevel() -> u8 {
    return NSZ_COMPRESS_LEVEL_OPTIONS[App::GetApp()->m_nsz_compress_level.Get()].value;
}

auto App::GetNszThreadCount() -> u8 {
    return NSZ_COMPRESS_THREAD_OPTIONS[App::GetApp()->m_nsz_compress_threads.Get()].value;
}

auto App::GetNszBlockExponent() -> u8 {
    return NSZ_COMPRESS_BLOCK_OPTIONS[App::GetApp()->m_nsz_compress_block_exponent.Get()].value;
}

void App::SetNxlinkEnable(bool enable) {
    if (App::GetNxlinkEnable() != enable) {
        g_app->m_nxlink_enabled.Set(enable);
        if (enable) {
            nxlinkInitialize(nxlink_callback);
        } else {
            nxlinkExit();
        }
    }
}

void App::SetHddEnable(bool enable) {
    if (App::GetHddEnable() != enable) {
        g_app->m_hdd_enabled.Set(enable);
#ifdef ENABLE_LIBUSBHSFS
        if (enable) {
            ApplyHddMountFlags();
            usbHsFsInitialize(1);
        } else {
            usbHsFsExit();
        }
#endif // ENABLE_LIBUSBHSFS
    }
}

void App::SetWriteProtect(bool enable) {
    if (App::GetWriteProtect() != enable) {
        g_app->m_hdd_write_protect.Set(enable);
        App::RemountHdd();
    }
}

void App::SetHddShowSystemFiles(bool enable) {
    if (App::GetHddShowSystemFiles() != enable) {
        g_app->m_hdd_show_system_files.Set(enable);
        App::RemountHdd();
    }
}

void App::SetHddIgnoreHibernation(bool enable) {
    if (App::GetHddIgnoreHibernation() != enable) {
        g_app->m_hdd_ignore_hibernation.Set(enable);
        App::RemountHdd();
    }
}

void App::RemountHdd() {
#ifdef ENABLE_LIBUSBHSFS
    ApplyHddMountFlags();

    // mount flags are only read when a device is mounted, so cycle the library
    // to pick them up, otherwise the user would have to unplug the drive.
    if (App::GetHddEnable()) {
        usbHsFsExit();
        usbHsFsInitialize(1);
    }
#endif // ENABLE_LIBUSBHSFS
}

void App::SetLogEnable(bool enable) {
    if (App::GetLogEnable() != enable) {
        g_app->m_log_enabled.Set(enable);
        if (enable) {
            log_file_init();
        } else {
            log_file_exit();
        }
    }
}

void App::SetReplaceHbmenuEnable(bool enable) {
    if (App::GetReplaceHbmenuEnable() != enable) {
        g_app->m_replace_hbmenu.Set(enable);
        if (!enable) {
            // check we have already replaced hbmenu with sphaira
            NacpStruct hbmenu_nacp{};
            if (R_SUCCEEDED(nro_get_nacp("/hbmenu.nro", hbmenu_nacp))) {
                if (std::strcmp(NacpLanguageEntries(hbmenu_nacp)[0].name, "sphaira")) {
                    return;
                }
            }

            // ask user if they want to restore hbmenu
            App::Push<ui::OptionBox>(
                "Restore hbmenu?"_i18n,
                "Back"_i18n, "Restore"_i18n, 1, [hbmenu_nacp](auto op_index){
                    if (!op_index || *op_index == 0) {
                        return;
                    }

                    NacpStruct actual_hbmenu_nacp;
                    if (R_FAILED(nro_get_nacp("/switch/hbmenu.nro", actual_hbmenu_nacp))) {
                        App::Push<ui::OptionBox>(
                            i18n::get("missing_hbmenu_info",
                                "Failed to find /switch/hbmenu.nro\n"
                                "Use the Appstore to re-install hbmenu"
                            ), "OK"_i18n
                        );
                        return;
                    }

                    // NOTE: do NOT use rename anywhere here as it's possible
                    // to have a race condition with another app that opens hbmenu as a file
                    // in between the delete + rename.
                    // this would require a sys-module to open hbmenu.nro, such as an ftp server.
                    // a copy means that it opens the file handle, if successfull, then
                    // the full read/write will succeed.
                    NacpStruct sphaira_nacp;
                    fs::FsPath sphaira_path = "/switch/sphaira/sphaira.nro";
                    Result rc;

                    // first, try and backup sphaira, its not super important if this fails.
                    rc = nro_get_nacp(sphaira_path, sphaira_nacp);
                    if (R_FAILED(rc) || std::strcmp(NacpLanguageEntries(sphaira_nacp)[0].name, "sphaira")) {
                        sphaira_path = "/switch/sphaira.nro";
                        rc = nro_get_nacp(sphaira_path, sphaira_nacp);
                    }

                    if (R_SUCCEEDED(rc) && !std::strcmp(NacpLanguageEntries(sphaira_nacp)[0].name, "sphaira")) {
                        if (IsVersionNewer(sphaira_nacp.display_version, hbmenu_nacp.display_version)) {
                            if (R_FAILED(rc = g_app->m_fs->copy_entire_file(sphaira_path, "/hbmenu.nro"))) {
                                log_write("failed to copy entire file: %s 0x%X module: %u desc: %u\n", sphaira_path.s, rc, R_MODULE(rc), R_DESCRIPTION(rc));
                            } else {
                                log_write("success with updating hbmenu!\n");
                            }
                        }
                    } else {
                        // sphaira doesn't yet exist, create a new file.
                        sphaira_path = "/switch/sphaira/sphaira.nro";
                        g_app->m_fs->CreateDirectoryRecursively("/switch/sphaira/");
                        g_app->m_fs->copy_entire_file(sphaira_path, "/hbmenu.nro");
                    }

                    // this should never fail, if it does, well then the sd card is fucked.
                    if (R_FAILED(rc = g_app->m_fs->copy_entire_file("/hbmenu.nro", "/switch/hbmenu.nro")))  {
                        // try and restore sphaira in a last ditch effort.
                        if (R_FAILED(rc = g_app->m_fs->copy_entire_file("/hbmenu.nro", sphaira_path))) {
                            App::PushErrorBox(rc, "Failed to, TODO: add message here"_i18n);
                            App::PushErrorBox(rc,
                                "Failed to restore hbmenu, please re-download hbmenu"_i18n
                            );
                        } else {
                            App::Push<ui::OptionBox>(
                                "Failed to restore hbmenu, using sphaira instead"_i18n,
                                "OK"_i18n
                            );
                        }
                        return;
                    }

                    // don't need this any more.
                    g_app->m_fs->DeleteFile("/switch/hbmenu.nro");

                    // if we were hbmenu, exit now (as romfs is gone).
                    if (IsHbmenu()) {
                        App::Push<ui::OptionBox>(
                            "Restored hbmenu, closing sphaira"_i18n,
                            "OK"_i18n, [](auto) {
                                App::Exit();
                            }
                        );
                    } else {
                        App::Notify("Restored hbmenu"_i18n);
                    }
                }
            );
        }
    }
}

void App::SetInstallSysmmcEnable(bool enable) {
    g_app->m_install_sysmmc.Set(enable);
}

void App::SetInstallEmummcEnable(bool enable) {
    g_app->m_install_emummc.Set(enable);
}

void App::SetInstallSdEnable(bool enable) {
    g_app->m_install_sd.Set(enable);
}

void App::SetThemeMusicEnable(bool enable) {
    if (App::GetThemeMusicEnable() != enable) {
        g_app->m_theme_music.Set(enable);
        if (enable) {
            g_app->LoadAndPlayThemeMusic();
        } else {
            g_app->CloseThemeBackgroundMusic();
        }
    }
}

void App::SetMtpEnable(bool enable) {
    if (App::GetMtpEnable() != enable) {
        g_app->m_mtp_enabled.Set(enable);

#ifdef ENABLE_LIBHAZE
        if (enable) {
            libhaze::Init();
        } else {
            libhaze::Exit();
        }
#endif // ENABLE_LIBHAZE
    }
}

void App::SetFtpEnable(bool enable) {
    if (App::GetFtpEnable() != enable) {
        g_app->m_ftp_enabled.Set(enable);

#ifdef ENABLE_FTPSRV
        if (enable) {
            ftpsrv::Init();
        } else {
            ftpsrv::Exit();
        }
#endif // ENABLE_FTPSRV
    }
}

void App::SetLanguage(long index) {
    if (App::GetLanguage() != index) {
        g_app->m_language.Set(index);
        on_i18n_change();

        App::Push<ui::OptionBox>(
            "Restart Sphaira?"_i18n,
            "Back"_i18n, "Restart"_i18n, 1, [](auto op_index){
                if (op_index && *op_index) {
                    App::ExitRestart();
                }
            }
        );
    }
}

void App::SetTextScrollSpeed(long index) {
    g_app->m_text_scroll_speed.Set(index);
}

auto App::Install(OwoConfig& config) -> Result {
    App::Push<ui::ProgressBox>(0, "Installing Forwarder"_i18n, config.name, [config](auto pbox) mutable -> Result {
        return Install(pbox, config);
    }, [](Result rc){
        App::PushErrorBox(rc, "Failed to install forwarder"_i18n);

        if (R_SUCCEEDED(rc)) {
            App::PlaySoundEffect(SoundEffect::Install);
            App::Notify("Installed!"_i18n);
        }
    });

    R_SUCCEED();
}

auto App::Install(ui::ProgressBox* pbox, OwoConfig& config) -> Result {
    config.nro_path = nro_add_arg_file(config.nro_path);
    if (config.icon.empty()) {
        config.icon.assign(std::begin(DEFAULT_IMAGE_DATA), std::end(DEFAULT_IMAGE_DATA));
    }
    config.icon = GetNroIcon(config.icon);

    if (config.logo.empty()) {
        g_app->m_fs->read_entire_file("/config/sphaira/logo/NintendoLogo.png", config.logo);
    }

    if (config.gif.empty()) {
        g_app->m_fs->read_entire_file("/config/sphaira/logo/StartupMovie.gif", config.gif);
    }

    return install_forwarder(pbox, config, GetInstallSdEnable() ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser);
}

auto App::IsEmummc() -> bool {
    const auto& paths = g_app->m_emummc_paths;
    return (paths.file_based_path[0] != '\0') || (paths.nintendo[0] != '\0');
}

auto App::IsParitionBaseEmummc() -> bool {
    const auto& paths = g_app->m_emummc_paths;
    return (paths.file_based_path[0] == '\0') && (paths.nintendo[0] != '\0');
}

auto App::IsFileBaseEmummc() -> bool {
    const auto& paths = g_app->m_emummc_paths;
    return (paths.file_based_path[0] != '\0') && (paths.nintendo[0] != '\0');
}

void App::Exit() {
    g_app->m_quit = true;
}

void App::ExitRestart() {
    nro_launch(GetExePath());
    Exit();
}

void App::Poll() {
    m_controller.Reset();

    HidTouchScreenState state{};
    hidGetTouchScreenStates(&state, 1);
    m_touch_info.is_clicked = false;

    // todo:
    #if 0
    HidMouseState mouse_state{};
    hidGetMouseStates(&mouse_state, 1);

    if (mouse_state.buttons) {
        log_write("[MOUSE] buttons: 0x%X x: %d y: %d dx: %d dy: %d wx: %d wy: %d\n", mouse_state.buttons, mouse_state.x, mouse_state.y, mouse_state.delta_x, mouse_state.delta_y, mouse_state.wheel_delta_x, mouse_state.wheel_delta_y);
    }
    #endif

// todo: replace old touch code with gestures from below
#if 0
    static HidGestureState prev_gestures[17]{};
    HidGestureState gestures[17]{};
    const auto gesture_count = hidGetGestureStates(gestures, std::size(gestures));
    for (int i = (int)gesture_count - 1; i >= 0; i--) {
        bool found = false;
        for (int j = 0; j < gesture_count; j++) {
            if (gestures[i].type == prev_gestures[j].type && gestures[i].sampling_number == prev_gestures[j].sampling_number) {
                found = true;
                break;
            }
        }

        if (found) {
            continue;
        }

        auto gesture = gestures[i];
        if (gesture_count && gesture.type == HidGestureType_Touch) {
            log_write("[TOUCH] got gesture attr: %u direction: %u sampling_number: %zu context_number: %zu\n", gesture.attributes, gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Swipe) {
            log_write("[SWIPE] got gesture direction: %u sampling_number: %zu context_number: %zu\n", gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Tap) {
            log_write("[TAP] got gesture direction: %u sampling_number: %zu context_number: %zu\n", gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Press) {
            log_write("[PRESS] got gesture direction: %u sampling_number: %zu context_number: %zu\n", gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Cancel) {
            log_write("[CANCEL] got gesture direction: %u sampling_number: %zu context_number: %zu\n", gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Complete) {
            log_write("[COMPLETE] got gesture direction: %u sampling_number: %zu context_number: %zu\n", gesture.direction, gesture.sampling_number, gesture.context_number);
        }
        else if (gesture_count && gesture.type == HidGestureType_Pan) {
            log_write("[PAN] got gesture direction: %u sampling_number: %zu context_number: %zu x: %d y: %d dx: %d dy: %d vx: %.2f vy: %.2f count: %d\n", gesture.direction, gesture.sampling_number, gesture.context_number, gesture.x, gesture.y, gesture.delta_x, gesture.delta_y, gesture.velocity_x, gesture.velocity_y, gesture.point_count);
        }
    }

    memcpy(prev_gestures, gestures, sizeof(gestures));
#endif

    // todo: support mouse scroll / touch.
    if (state.count == 1 && !m_touch_info.is_touching) {
        m_touch_info.initial = m_touch_info.cur = state.touches[0];
        m_touch_info.is_touching = true;
        m_touch_info.is_tap = true;
    } else if (state.count >= 1 && m_touch_info.is_touching) {
        m_touch_info.cur = state.touches[0];

        if (m_touch_info.is_tap &&
            (std::abs((s32)m_touch_info.initial.x - (s32)m_touch_info.cur.x) > 20 ||
            std::abs((s32)m_touch_info.initial.y - (s32)m_touch_info.cur.y) > 20)) {
            m_touch_info.is_tap = false;
            m_touch_info.is_scroll = true;
        }
    } else if (m_touch_info.is_touching) {
        m_touch_info.is_touching = false;
        m_touch_info.is_scroll = false;
        if (m_touch_info.is_tap) {
            m_touch_info.is_clicked = true;
        } else {
            m_touch_info.is_end = true;
        }
    }

    u64 kdown = 0;
    u64 kup = 0;
    u64 kheld = 0;

    // todo: better implement this to match hos
    if (!m_touch_info.is_touching && !m_touch_info.is_clicked) {
        // controller.
        padUpdate(&m_pad);
        kdown |= padGetButtonsDown(&m_pad);
        kheld |= padGetButtons(&m_pad);
        kup |= padGetButtonsUp(&m_pad);

        // keyboard.
        m_keyboard.Update();
        kdown |= m_keyboard.GetButtonsDown();
        kheld |= m_keyboard.GetButtons();
        kup |= m_keyboard.GetButtonsUp();
    }

    m_controller.m_kdown = kdown;
    m_controller.m_kheld = kheld;
    m_controller.m_kup = kup;
    m_controller.UpdateButtonHeld(static_cast<u64>(Button::ANY_DIRECTION), m_delta_time);
}

void App::Update() {
    // loop background music if it has finished.
    audio::State song_state;
    if (R_SUCCEEDED(audio::GetProgress(m_background_music, nullptr, &song_state))) {
        if (song_state == audio::State::Finished) {
            audio::SeekSong(m_background_music, 0);
        }
    }

    const bool has_wake_input = m_controller.m_kdown || m_touch_info.is_touching || m_touch_info.is_clicked;
    if (has_wake_input && App::IsScreenToggleEnabled() && App::IsScreenDisabled()) {
        App::SetScreenDisabled(false);

        // The input which wakes the display must not activate the hidden UI.
        m_controller.Reset();
        m_controller.m_kheld = 0;
        m_touch_info.is_tap = false;
        m_touch_info.is_scroll = false;
        m_touch_info.is_clicked = false;
        m_touch_info.is_end = false;
    }

    m_widgets.back()->Update(&m_controller, &m_touch_info);

    bool popped_at_least1 = false;
    while (true) {
        if (m_widgets.empty()) {
            log_write("[Mui] no widgets left, so we exit...");
            App::Exit();
            return;
        }

        if (m_widgets.back()->ShouldPop()) {
            log_write("popping widget\n");
            m_widgets.pop_back();
            popped_at_least1 = true;
        } else {
            break;
        }
    }

    if (!m_widgets.empty() && popped_at_least1) {
        m_widgets.back()->OnFocusGained();
    }
}

void App::Draw() {
    const auto slot = this->queue.acquireImage(this->swapchain);
    this->queue.submitCommands(this->framebuffer_cmdlists[slot]);
    this->queue.submitCommands(this->render_cmdlist);
    nvgBeginFrame(this->vg, s_width, s_height, 1.f);
    nvgScale(vg, m_scale.x, m_scale.y);

    // find the last menu in the list, start drawing from there
    auto menu_it = m_widgets.rend();
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); it++) {
        const auto& p = *it;
        if (!p->IsHidden() && p->IsMenu()) {
            menu_it = it;
            break;
        }
    }

    // reverse itr so loop backwards to go forwarders.
    if (menu_it != m_widgets.rend()) {
        for (auto it = menu_it; ; it--) {
            const auto& p = *it;

            // draw everything not hidden on top of the menu.
            if (!p->IsHidden()) {
                p->Draw(vg, &m_theme);
            }

            if (it == m_widgets.rbegin()) {
                break;
            }
        }
    }

    m_notif_manager.Draw(vg, &m_theme);

    nvgResetTransform(vg);
    nvgEndFrame(this->vg);
    this->queue.presentImage(this->swapchain, slot);
}

auto App::GetApp() -> App* {
    return g_app;
}

auto App::AcquireScreenToggle() -> bool {
    SCOPED_MUTEX(&g_screen_toggle_mutex);

    if (!g_screen_toggle_ref_count) {
        const auto rc = lblInitialize();
        if (R_FAILED(rc)) {
            log_write("[SCREEN] lblInitialize() failed: 0x%X\n", R_VALUE(rc));
            return false;
        }

        g_screen_disabled = false;
    }

    g_screen_toggle_ref_count++;
    return true;
}

void App::ReleaseScreenToggle() {
    SCOPED_MUTEX(&g_screen_toggle_mutex);

    if (!g_screen_toggle_ref_count) {
        return;
    }

    g_screen_toggle_ref_count--;
    if (!g_screen_toggle_ref_count) {
        const auto rc = appletSetLcdBacklightOffEnabled(false);
        if (R_FAILED(rc)) {
            log_write("[SCREEN] failed to restore backlight: 0x%X\n", R_VALUE(rc));
        }

        g_screen_disabled = false;
        lblExit();
    }
}

auto App::IsScreenToggleEnabled() -> bool {
    SCOPED_MUTEX(&g_screen_toggle_mutex);
    return g_screen_toggle_ref_count != 0;
}

auto App::IsScreenDisabled() -> bool {
    SCOPED_MUTEX(&g_screen_toggle_mutex);

    if (!g_screen_toggle_ref_count) {
        return false;
    }

    LblBacklightSwitchStatus status{};
    if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&status))) {
        g_screen_disabled = status != LblBacklightSwitchStatus_Enabled;
    }

    return g_screen_disabled;
}

void App::SetScreenDisabled(bool disable) {
    SCOPED_MUTEX(&g_screen_toggle_mutex);

    if (!g_screen_toggle_ref_count) {
        return;
    }

    const auto rc = appletSetLcdBacklightOffEnabled(disable);
    if (R_FAILED(rc)) {
        log_write("[SCREEN] failed to set backlight state: 0x%X\n", R_VALUE(rc));
        return;
    }

    g_screen_disabled = disable;
}

auto App::GetVg() -> NVGcontext* {
    return g_app->vg;
}

void DrawElement(float x, float y, float w, float h, ThemeEntryID id) {
    DrawElement({x, y, w, h}, id);
}

void DrawElement(const Vec4& v, ThemeEntryID id) {
    const auto& e = g_app->m_theme.elements[id];

    switch (e.type) {
        case ElementType::None: {
        } break;
        case ElementType::Texture: {
            auto paint = nvgImagePattern(g_app->vg, v.x, v.y, v.w, v.h, 0, e.texture, 1.f);
            // override the icon colours if set
            if (id > ThemeEntryID_ICON_COLOUR && id < ThemeEntryID_MAX) {
                if (g_app->m_theme.elements[ThemeEntryID_ICON_COLOUR].type != ElementType::None) {
                    paint.innerColor = g_app->m_theme.GetColour(ThemeEntryID_ICON_COLOUR);
                }
            }
            ui::gfx::drawRect(g_app->vg, v, paint);
        } break;
        case ElementType::Colour: {
            ui::gfx::drawRect(g_app->vg, v, e.colour);
        } break;
    }
}

auto App::LoadElementImage(std::string_view value) -> ElementEntry {
    ElementEntry entry{};

    entry.texture = nvgCreateImage(vg, value.data(), 0);
    if (entry.texture) {
        entry.type = ElementType::Texture;
    }

    return entry;
}

auto App::LoadElementColour(std::string_view value) -> ElementEntry {
    ElementEntry entry{};

    if (value.starts_with("0x")) {
        value = value.substr(2);
    } else {
        return {};
    }

    char* end;
    u32 c = std::strtoul(value.data(), &end, 16);
    if (!c && value.data() == end) {
        return {};
    }

    // force alpha bit if not already set.
    if (value.length() <= 6) {
        c <<= 8;
        c |= 0xFF;
    }

    entry.colour = nvgRGBA((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    entry.type = ElementType::Colour;
    return entry;
}

auto App::LoadElement(std::string_view value, ElementType type) -> ElementEntry {
    if (value.size() <= 1) {
        return {};
    }

    if (type == ElementType::None || type == ElementType::Colour) {
        // most assets are colours, so prioritise this first
        if (auto e = LoadElementColour(value); e.type != ElementType::None) {
            return e;
        }
    }

    if (type == ElementType::None || type == ElementType::Texture) {
        if (auto e = LoadElementImage(value); e.type != ElementType::None) {
            return e;
        }
    }

    return {};
}

void App::CloseThemeBackgroundMusic() {
    audio::CloseSong(&m_background_music);
}

void App::CloseTheme() {
    CloseThemeBackgroundMusic();

    for (auto& e : m_theme.elements) {
        if (e.type == ElementType::Texture) {
            nvgDeleteImage(vg, e.texture);
        }
    }

    m_theme = {};
}

void App::LoadTheme(const ThemeMeta& meta) {
    // reset theme
    CloseTheme();

    ThemeData theme_data{};
    theme_data.music_path = m_default_music.Get();
    LoadThemeInternal(meta, theme_data);
    m_theme.meta = meta;

    if (R_SUCCEEDED(romfsInit())) {
        ON_SCOPE_EXIT(romfsExit());

        // load all assets / colours.
        for (auto& e : THEME_ENTRIES) {
            m_theme.elements[e.id] = LoadElement(theme_data.elements[e.id], e.type);
        }

        // load music
        m_theme.music_path = theme_data.music_path;
        LoadAndPlayThemeMusic();
    }
}

// todo: only use opendir on if romfs, otherwise use native fs
void App::ScanThemes(const std::string& path) {
    auto dir = opendir(path.c_str());
    if (!dir) {
        return;
    }
    ON_SCOPE_EXIT(closedir(dir));


    while (auto d = readdir(dir)) {
        if (d->d_name[0] == '.') {
            continue;
        }

        if (d->d_type != DT_REG) {
            continue;
        }

        const std::string name = d->d_name;
        if (!name.ends_with(".ini")) {
            continue;
        }

        const auto full_path = path + name;

        ThemeMeta meta{};
        if (LoadThemeMeta(full_path, meta)) {
            m_theme_meta_entries.emplace_back(meta);
        }
    }
}

void App::ScanThemeEntries() {
    // load from romfs first
    if (R_SUCCEEDED(romfsInit())) {
        ScanThemes("romfs:/themes/");
        romfsExit();
    }

    // then load custom entries
    ScanThemes("/config/sphaira/themes/");
}

void App::LoadAndPlayThemeMusic() {
    if (App::GetThemeMusicEnable() && !m_theme.music_path.empty()) {
        audio::CloseSong(&m_background_music);
        audio::OpenSong(m_fs.get(), m_theme.music_path, audio::Flag_Loop, &m_background_music);
        audio::PlaySong(m_background_music);
    }
}

Result App::SetDefaultBackgroundMusic(fs::Fs* fs, const fs::FsPath& path) {
    constexpr const char* base_path = "/config/sphaira/themes/default_music.";

    const auto ext = std::strrchr(path, '.');
    R_UNLESS(ext, 0x1);

    std::vector<u8> buf;
    R_TRY(fs->read_entire_file(path, buf));

    // remove old file if we made a copy of it.
    audio::CloseSong(&g_app->m_background_music);
    if (g_app->m_default_music.Get().starts_with(base_path)) {
        log_write("[APP] removing previously copied background music file\n");
        g_app->m_fs->DeleteFile(g_app->m_default_music.Get());
    }

    // check if we can link to the file, or if we have to copy it over.
    // sd card fs can link to the file, but music contained in zips or hdd needs copying.
    fs::FsPath new_path{};
    if (path[0] == '/' && fs->IsSd() && g_app->m_fs->FileExists(path)) {
        new_path = path;
        log_write("[APP] linking background music\n");
    } else {
        std::snprintf(new_path, sizeof(new_path), "%s%s", base_path, ext + 1);
        R_TRY(g_app->m_fs->write_entire_file(new_path, buf));
        log_write("[APP] copying background music to sd card\n");
    }

    // save path to the music file.
    g_app->m_default_music.Set(new_path.toString());

    // load and play music file.
    audio::OpenSong(g_app->m_fs.get(), new_path, audio::Flag_Loop, &g_app->m_background_music);
    audio::PlaySong(g_app->m_background_music);

    R_SUCCEED();
}

void App::SetBackgroundMusicPause(bool pause) {
    if (pause) {
        audio::PauseSong(g_app->m_background_music);
    } else {
        audio::PlaySong(g_app->m_background_music);
    }
}

Result App::GetSdSize(s64* free, s64* total) {
    fs::FsNativeContentStorage fs{FsContentStorageId_SdCard};
    R_TRY(fs.GetFreeSpace("/", free));
    R_TRY(fs.GetTotalSpace("/", total));
    R_SUCCEED();
}

Result App::GetEmmcSize(s64* free, s64* total) {
    fs::FsNativeContentStorage fs{FsContentStorageId_User};
    R_TRY(fs.GetFreeSpace("/", free));
    R_TRY(fs.GetTotalSpace("/", total));
    R_SUCCEED();
}

App::App(const char* argv0) {
    // boost mode is enabled in userAppInit().
    ON_SCOPE_EXIT(App::SetBoostMode(false));
    SCOPED_TIMESTAMP("App Constructor");

    g_app = this;
    m_start_timestamp = armGetSystemTick();
    if (!std::strncmp(argv0, "sdmc:/", 6)) {
        // memmove(path, path + 5, strlen(path)-5);
        std::strncpy(m_app_path, argv0 + 5, std::strlen(argv0)-5);
    } else {
        m_app_path = argv0;
    }

    // set if we are hbmenu
    if (IsHbmenu()) {
        __nx_applet_exit_mode = 1;
    }

    // init fs for app use.
    m_fs = std::make_shared<fs::FsNativeSd>(true);

    static const auto cb = [](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) -> int {
        auto app = static_cast<App*>(UserData);

        if (!std::strcmp(Section, INI_SECTION)) {
            if (app->m_nxlink_enabled.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_enabled.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_enabled.LoadFrom(Key, Value)) {}
            else if (app->m_hdd_enabled.LoadFrom(Key, Value)) {}
            else if (app->m_hdd_write_protect.LoadFrom(Key, Value)) {}
            else if (app->m_hdd_show_system_files.LoadFrom(Key, Value)) {}
            else if (app->m_hdd_ignore_hibernation.LoadFrom(Key, Value)) {}
            else if (app->m_log_enabled.LoadFrom(Key, Value)) {}
            else if (app->m_replace_hbmenu.LoadFrom(Key, Value)) {}
            else if (app->m_default_music.LoadFrom(Key, Value)) {}
            else if (app->m_theme_path.LoadFrom(Key, Value)) {}
            else if (app->m_theme_music.LoadFrom(Key, Value)) {}
            else if (app->m_show_ip_addr.LoadFrom(Key, Value)) {}
            else if (app->m_language.LoadFrom(Key, Value)) {}
            else if (app->m_center_menu.LoadFrom(Key, Value)) {}
            else if (app->m_left_menu.LoadFrom(Key, Value)) {}
            else if (app->m_right_menu.LoadFrom(Key, Value)) {}
            else if (app->m_install_sysmmc.LoadFrom(Key, Value)) {}
            else if (app->m_install_emummc.LoadFrom(Key, Value)) {}
            else if (app->m_install_sd.LoadFrom(Key, Value)) {}
            else if (app->m_progress_boost_mode.LoadFrom(Key, Value)) {}
            else if (app->m_allow_downgrade.LoadFrom(Key, Value)) {}
            else if (app->m_skip_if_already_installed.LoadFrom(Key, Value)) {}
            else if (app->m_ticket_only.LoadFrom(Key, Value)) {}
            else if (app->m_skip_base.LoadFrom(Key, Value)) {}
            else if (app->m_skip_patch.LoadFrom(Key, Value)) {}
            else if (app->m_skip_addon.LoadFrom(Key, Value)) {}
            else if (app->m_skip_data_patch.LoadFrom(Key, Value)) {}
            else if (app->m_skip_ticket.LoadFrom(Key, Value)) {}
            else if (app->m_skip_nca_hash_verify.LoadFrom(Key, Value)) {}
            else if (app->m_skip_rsa_header_fixed_key_verify.LoadFrom(Key, Value)) {}
            else if (app->m_skip_rsa_npdm_fixed_key_verify.LoadFrom(Key, Value)) {}
            else if (app->m_ignore_distribution_bit.LoadFrom(Key, Value)) {}
            else if (app->m_convert_to_common_ticket.LoadFrom(Key, Value)) {}
            else if (app->m_convert_to_standard_crypto.LoadFrom(Key, Value)) {}
            else if (app->m_lower_master_key.LoadFrom(Key, Value)) {}
            else if (app->m_lower_system_version.LoadFrom(Key, Value)) {}
        } else if (!std::strcmp(Section, "accessibility")) {
            if (app->m_text_scroll_speed.LoadFrom(Key, Value)) {}
        } else if (!std::strcmp(Section, "dump")) {
            if (app->m_dump_fix_filenames.LoadFrom(Key, Value)) {}
            else if (app->m_dump_app_folder.LoadFrom(Key, Value)) {}
            else if (app->m_dump_append_folder_with_xci.LoadFrom(Key, Value)) {}
            else if (app->m_dump_trim_xci.LoadFrom(Key, Value)) {}
            else if (app->m_dump_label_trim_xci.LoadFrom(Key, Value)) {}
            else if (app->m_dump_convert_to_common_ticket.LoadFrom(Key, Value)) {}
            else if (app->m_nsz_compress_level.LoadFrom(Key, Value)) {}
            else if (app->m_nsz_compress_threads.LoadFrom(Key, Value)) {}
            else if (app->m_nsz_compress_ldm.LoadFrom(Key, Value)) {}
            else if (app->m_nsz_compress_block.LoadFrom(Key, Value)) {}
            else if (app->m_nsz_compress_block_exponent.LoadFrom(Key, Value)) {}
        } else if (!std::strcmp(Section, "ftp")) {
            if (app->m_ftp_port.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_anon.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_user.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_pass.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_album.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_ams_contents.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_bis_storage.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_bis_fs.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_content_system.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_content_user.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_content_sd.LoadFrom(Key, Value)) {}
            // else if (app->m_ftp_show_content_sd0.LoadFrom(Key, Value)) {}
            // else if (app->m_ftp_show_custom_system.LoadFrom(Key, Value)) {}
            // else if (app->m_ftp_show_custom_sd.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_games.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_install.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_mounts.LoadFrom(Key, Value)) {}
            else if (app->m_ftp_show_switch.LoadFrom(Key, Value)) {}
        } else if (!std::strcmp(Section, "mtp")) {
            if (app->m_mtp_vid.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_pid.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_allocate_file.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_album.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_content_sd.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_content_system.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_content_user.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_games.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_install.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_mounts.LoadFrom(Key, Value)) {}
            else if (app->m_mtp_show_speedtest.LoadFrom(Key, Value)) {}
        }

        return 1;
    };

    // load all configs ahead of time, as this is actually faster than
    // loading each config one by one as it avoids re-opening the file multiple times.
    {
        SCOPED_TIMESTAMP("config init");
        ini_browse(cb, this, CONFIG_PATH);
    }

    if (App::GetLogEnable()) {
        log_file_init();
        log_write("hello world v%s\n", APP_DISPLAY_VERSION);
    }

    // anything that can be async loaded should be placed in here in order
    // to halve load times.
    // rules:
    // - 1: cannot use romfs as its not thread-safe.
    // - 2: cannot use nvg code as its not thread-safe.
    // - 3: cannot be too slow that async takes longer than the main thread (ie, balance the load).
    // currrent load time is 60ms without logs, 90 with (down from 230ms).
    utils::Async async_init([this](){
        SCOPED_TIMESTAMP("App async load");

        {
            SCOPED_TIMESTAMP("config directory init");
            m_fs->CreateDirectoryRecursively("/config/sphaira");
            m_fs->CreateDirectory("/config/sphaira/assoc");
            m_fs->CreateDirectory("/config/sphaira/themes");
            m_fs->CreateDirectory("/config/sphaira/github");
            m_fs->CreateDirectory("/config/sphaira/i18n");
            m_fs->CreateDirectory("/config/sphaira/mount");

            if (!ini_haskey("steamgriddb", "api_key", CONFIG_PATH)) {
                ini_puts("steamgriddb", "api_key", "", CONFIG_PATH);
            }
        }

        {
            // delete old cached folders/files on startup.
            SCOPED_TIMESTAMP("old cache delete");
            m_fs->DeleteDirectoryRecursively("/switch/sphaira/cache/themezer"); // themezer icon/json cache.
            m_fs->DeleteFile("/switch/sphaira/cache/cache.json"); // old etag cache.
        }

        if (log_is_init()) {
            SCOPED_TIMESTAMP("fw log init");
            SetSysFirmwareVersion fw_version{};
            setsysInitialize();
            ON_SCOPE_EXIT(setsysExit());
            setsysGetFirmwareVersion(&fw_version);

            log_write("[version] platform: %s\n", fw_version.platform);
            log_write("[version] version_hash: %s\n", fw_version.version_hash);
            log_write("[version] display_version: %s\n", fw_version.display_version);
            log_write("[version] display_title: %s\n", fw_version.display_title);

            splInitialize();
            ON_SCOPE_EXIT(splExit());

            u64 out{};
            splGetConfig((SplConfigItem)65000, &out);
            log_write("[ams] version: %lu.%lu.%lu\n", (out >> 56) & 0xFF, (out >> 48) & 0xFF, (out >> 40) & 0xFF);
            log_write("[ams] target version: %lu.%lu.%lu\n", (out >> 24) & 0xFF, (out >> 16) & 0xFF, (out >> 8) & 0xFF);
            log_write("[ams] key gen: %lu\n", (out >> 32) & 0xFF);

            splGetConfig((SplConfigItem)65003, &out);
            log_write("[ams] hash: %lx\n", out);

            splGetConfig((SplConfigItem)65010, &out);
            log_write("[ams] usb 3.0 enabled: %lu\n", out);
        }

        // get emummc config.
        {
            SCOPED_TIMESTAMP("emummc detect init");
            alignas(0x1000) AmsEmummcPaths paths{};
            SecmonArgs args{};
            args.X[0] = 0xF0000404; /* smcAmsGetEmunandConfig */
            args.X[1] = 0; /* EXO_EMUMMC_MMC_NAND*/
            args.X[2] = (u64)&paths; /* out path */
            svcCallSecureMonitor(&args);
            m_emummc_paths = paths;

            log_write("[emummc] enabled: %u\n", App::IsEmummc());
            if (App::IsEmummc()) {
                log_write("[emummc] file based path: %s\n", m_emummc_paths.file_based_path);
                log_write("[emummc] nintendo path: %s\n", m_emummc_paths.nintendo);
            }
        }

#ifdef ENABLE_LIBHAZE
        if (App::GetMtpEnable()) {
            SCOPED_TIMESTAMP("mtp init");
            libhaze::Init();
        }
#endif // ENABLE_LIBHAZE

#ifdef ENABLE_FTPSRV
        if (App::GetFtpEnable()) {
            SCOPED_TIMESTAMP("ftp init");
            ftpsrv::Init();
        }
#endif // ENABLE_FTPSRV

        if (App::GetNxlinkEnable()) {
            SCOPED_TIMESTAMP("nxlink init");
            nxlinkInitialize(nxlink_callback);
        }

#ifdef ENABLE_LIBUSBHSFS
        if (App::GetHddEnable()) {
            SCOPED_TIMESTAMP("hdd init");
            ApplyHddMountFlags();
            usbHsFsInitialize(1);
        }
#endif // ENABLE_LIBUSBHSFS

#ifdef ENABLE_LIBUSBDVD
        {
            SCOPED_TIMESTAMP("usbdvd init");
            if (R_FAILED(usbdvd::MountAll())) {
                log_write("[USBDVD] failed to mount\n");
            }
        }
#endif // ENABLE_LIBUSBDVD

        {
            SCOPED_TIMESTAMP("curl init");
            curl::Init();
        }

        // this has to come after curl init as it inits curl global.
        {
            SCOPED_TIMESTAMP("vfs init");
            devoptab::MountVfsAll();
        }

        #ifdef ENABLE_DEVOPTAB_HTTP
        {
            SCOPED_TIMESTAMP("http init");
            devoptab::MountHttpAll();
        }
        #endif // ENABLE_DEVOPTAB_HTTP

        #ifdef ENABLE_DEVOPTAB_WEBDAV
        {
            SCOPED_TIMESTAMP("webdav init");
            devoptab::MountWebdavAll();
        }
        #endif // ENABLE_DEVOPTAB_WEBDAV

        #ifdef ENABLE_DEVOPTAB_FTP
        {
            SCOPED_TIMESTAMP("ftp init");
            devoptab::MountFtpAll();
        }
        #endif // ENABLE_DEVOPTAB_FTP

        #ifdef ENABLE_DEVOPTAB_SFTP
        {
            SCOPED_TIMESTAMP("sftp init");
            devoptab::MountSftpAll();
        }
        #endif // ENABLE_DEVOPTAB_SFTP

        #ifdef ENABLE_DEVOPTAB_NFS
        {
            SCOPED_TIMESTAMP("nfs init");
            devoptab::MountNfsAll();
        }
        #endif // ENABLE_DEVOPTAB_NFS

        #ifdef ENABLE_DEVOPTAB_SMB2
        {
            SCOPED_TIMESTAMP("smb init");
            devoptab::MountSmb2All();
        }
        #endif // ENABLE_DEVOPTAB_SMB2

        {
            SCOPED_TIMESTAMP("game init");
            devoptab::MountGameAll();
        }

        {
            SCOPED_TIMESTAMP("fatfs init");
            devoptab::MountFatfsAll();
        }

        {
            SCOPED_TIMESTAMP("mounts init");
            devoptab::MountInternalMounts();
        }

        {
            SCOPED_TIMESTAMP("timestamp init");
            // ini_putl(GetExePath(), "timestamp", m_start_timestamp, App::PLAYLOG_PATH);
        }

        {
            SCOPED_TIMESTAMP("HID init");
            hidInitializeTouchScreen();
            hidInitializeGesture();
            hidInitializeKeyboard();
            hidInitializeMouse();

            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            // padInitializeDefault(&m_pad);
            padInitializeAny(&m_pad);

            m_keyboard.Init(KEYBOARD_BUTTON_MAP);
        }

        {
            SCOPED_TIMESTAMP("loader init");
            const auto loader_info_size = envGetLoaderInfoSize();
            if (loader_info_size) {
                if (loader_info_size >= 8 && !std::memcmp(envGetLoaderInfo(), "sphaira", 7)) {
                    log_write("launching from sphaira created forwarder\n");
                    m_is_launched_via_sphaira_forwader = true;
                } else {
                    log_write("launching from unknown forwader: %.*s size: %zu\n", (int)loader_info_size, envGetLoaderInfo(), loader_info_size);
                }
            } else {
                log_write("not launching from forwarder\n");
            }
        }
    });

    std::string language_name;
    {
        SCOPED_TIMESTAMP("i18n init");
        language_name = i18n::init(GetLanguage());
    }

    if (App::GetLogEnable()) {
        App::Notify("Warning! Logs are enabled, Sphaira will run slowly!"_i18n);
    }

#ifdef USE_NVJPG
    {
        SCOPED_TIMESTAMP("nvjpg init");
        // this has to be init before deko3d.
        nj::initialize();
        m_decoder.initialize();
    }
#endif

    // get current size of the framebuffer
    {
        SCOPED_TIMESTAMP("nvg init");
        const auto fb = GetFrameBufferSize();
        s_width = fb.size.x;
        s_height = fb.size.y;
        m_scale = fb.scale;

        // Create the deko3d device
        this->device = dk::DeviceMaker{}
            .setCbDebug(deko3d_error_cb)
            .create();

        // Create the main queue
        this->queue = dk::QueueMaker{this->device}
            .setFlags(DkQueueFlags_Graphics)
            .create();

        // Create the memory pools
        this->pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16*1024*1024);
        this->pool_code.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 128*1024);
        this->pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 1*1024*1024);

        // Create the static command buffer and feed it freshly allocated memory
        this->cmdbuf = dk::CmdBufMaker{this->device}.create();
        const CMemPool::Handle cmdmem = this->pool_data->allocate(this->StaticCmdSize);
        this->cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

        // Create the framebuffer resources
        this->createFramebufferResources();

        this->renderer.emplace(s_width, s_height, this->device, this->queue, *this->pool_images, *this->pool_code, *this->pool_data);
        this->vg = nvgCreateDk(&*this->renderer, NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    }

    // not sure if these are meant to be deleted or not...
    {
        SCOPED_TIMESTAMP("font init");
        PlSharedFontType standard_font_type = PlSharedFontType_Standard;
        std::vector<PlSharedFontType> lang_fonts = {
            PlSharedFontType_ChineseSimplified,
            PlSharedFontType_ExtChineseSimplified,
            PlSharedFontType_ChineseTraditional,
            PlSharedFontType_Standard,
        };
        if (language_name == "zh-Hans")
        {
            standard_font_type = PlSharedFontType_ChineseSimplified;
            std::erase(lang_fonts, PlSharedFontType_ChineseSimplified);
        }
        else if (language_name == "zh-Hant")
        {
            standard_font_type = PlSharedFontType_ChineseTraditional;
            std::erase(lang_fonts, PlSharedFontType_ChineseTraditional);
        }
        else
        {
            std::erase(lang_fonts, PlSharedFontType_Standard);
        }
        lang_fonts.push_back(PlSharedFontType_KO);

        PlFontData font_standard, font_extended, font_lang;
        plGetSharedFontByType(&font_standard, standard_font_type);
        plGetSharedFontByType(&font_extended, PlSharedFontType_NintendoExt);

        auto standard_font = nvgCreateFontMem(this->vg, "Standard", (unsigned char*)font_standard.address, font_standard.size, 0);
        auto extended_font = nvgCreateFontMem(this->vg, "Extended", (unsigned char*)font_extended.address, font_extended.size, 0);
        nvgAddFallbackFontId(this->vg, standard_font, extended_font);

        for (auto type : lang_fonts) {
            if (R_SUCCEEDED(plGetSharedFontByType(&font_lang, type))) {
                char name[32];
                std::snprintf(name, sizeof(name), "Lang_%u", font_lang.type);
                auto lang_font = nvgCreateFontMem(this->vg, name, (unsigned char*)font_lang.address, font_lang.size, 0);
                nvgAddFallbackFontId(this->vg, standard_font, lang_font);
            } else {
                log_write("failed plGetSharedFontByType(%d)\n", type);
            }
        }
    }

    {
        SCOPED_TIMESTAMP("hook init");
        appletHook(&m_appletHookCookie, appplet_hook_calback, this);
    }

    // load default image
    {
        SCOPED_TIMESTAMP("load default image");
        m_default_image = nvgCreateImageMem(vg, 0, DEFAULT_IMAGE_DATA, std::size(DEFAULT_IMAGE_DATA));
    }

    // disable audio in applet mode with a suspended application due to audren fatal.
    // see: https://github.com/ITotalJustice/sphaira/issues/92
    if (IsAppletWithSuspendedApp()) {
        App::Notify("Audio disabled due to suspended game"_i18n);
    } else {
        SCOPED_TIMESTAMP("audio init");
        if (R_FAILED(audio::Init())) {
            log_write("[AUDIO] failed to init\n");
        }
    }

    {
        SCOPED_TIMESTAMP("theme init");
        ScanThemeEntries();

        // try and load previous theme, default to previous version otherwise.
        fs::FsPath theme_path = m_theme_path.Get();
        ThemeMeta theme_meta;
        if (R_SUCCEEDED(romfsInit())) {
            ON_SCOPE_EXIT(romfsExit());
            if (!LoadThemeMeta(theme_path, theme_meta)) {
                log_write("failed to load meta using default\n");
                theme_path = DEFAULT_THEME_PATH;
                LoadThemeMeta(theme_path, theme_meta);
            }
        }
        log_write("loading theme from: %s\n", theme_meta.ini_path.s);
        LoadTheme(theme_meta);

        // find theme index using the path of the theme.ini
        for (u64 i = 0; i < m_theme_meta_entries.size(); i++) {
            if (m_theme.meta.ini_path == m_theme_meta_entries[i].ini_path) {
                m_theme_index = i;
                break;
            }
        }
    }
}

void App::PlaySoundEffect(SoundEffect effect) {
    audio::PlaySoundEffect(effect);
}

void App::DisplayThemeOptions(bool left_side) {
    ui::SidebarEntryArray::Items theme_items{};
    const auto theme_meta = App::GetThemeMetaList();
    for (auto& p : theme_meta) {
        theme_items.emplace_back(p.name);
    }

    auto options = std::make_unique<ui::Sidebar>("Theme Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<ui::SidebarEntryArray>("Select Theme"_i18n, theme_items, [](s64& index_out){
        App::SetTheme(index_out);
    }, App::GetThemeIndex(), "Customise the look of Sphaira by changing the theme"_i18n);

    options->Add<ui::SidebarEntryBool>("Music"_i18n, App::GetThemeMusicEnable(), [](bool& enable){
        App::SetThemeMusicEnable(enable);
    },  i18n::get("bgm_enable_info",
            "Enable background music.\n"
            "Each theme can have it's own music file. "
            "If a theme does not set a music file, the default music is loaded instead (if it exists)."
        )
    );

    options->Add<ui::SidebarEntryBool>("Show IP address"_i18n, App::GetApp()->m_show_ip_addr,
        i18n::get("display_ip_info",
            "Shows the IP address, WiFi strength, and connection type in all menus, including Applet Mode."
        )
    );

    // todo: add file picker for music here.
    // todo: add array to audio which has the list of supported extensions.
    auto remove_music = options->Add<ui::SidebarEntryCallback>("Remove Background Music"_i18n, [](){
        g_app->m_default_music.Set("");
        audio::CloseSong(&g_app->m_background_music);
    },  "Removes the background music file"_i18n);

    remove_music->Depends([](){
        return !g_app->m_default_music.Get().empty();
    }, "No background music file is set"_i18n);
}

void App::DisplayNetworkOptions(bool left_side) {

}

void App::DisplayMenuOptions(bool left_side) {
    auto options = std::make_unique<ui::Sidebar>("Menus"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    for (auto& e : ui::menu::main::GetMenuMenuEntries()) {
        if (e.name == g_app->m_center_menu.Get()) {
            continue;
        } else if (e.name == g_app->m_left_menu.Get()) {
            continue;
        } else if (e.name == g_app->m_right_menu.Get()) {
            continue;
        }

        auto entry = options->Add<ui::SidebarEntryCallback>(i18n::get(e.title), [e](){
            App::Push(e.func(ui::menu::MenuFlag_None));
        }, i18n::get(e.info));

        if (e.IsInstall()) {
            entry->Depends(App::GetInstallEnable, i18n::get("enable_install_info", App::INSTALL_DEPENDS_STR), App::ShowEnableInstallPrompt);
        }
    }

    if (App::IsApplication()) {
        options->Add<ui::SidebarEntryCallback>("Web"_i18n, [](){
            // add some default entries, will use a config file soon so users can set their own.
            ui::PopupList::Items items;
            items.emplace_back("https://lite.duckduckgo.com/lite");
            items.emplace_back("https://dns.switchbru.com");
            items.emplace_back("https://gbatemp.net");
            items.emplace_back("https://github.com/ITotalJustice/sphaira/wiki");
            items.emplace_back("Enter custom URL"_i18n);

            App::Push<ui::PopupList>(
                "Select URL"_i18n, items, [items](auto op_index){
                    if (op_index) {
                        const auto index = *op_index;
                        if (index == items.size() - 1) {
                            std::string out;
                            if (R_SUCCEEDED(swkbd::ShowText(out, "Enter URL"_i18n.c_str(), "https://")) && !out.empty()) {
                                WebShow(out);
                            }
                        } else {
                            WebShow(items[index]);
                        }
                    }
                }
            );
        },
        i18n::get("web_browser_info",
            "Launch the built-in web browser.\n\n"
            "NOTE: The browser is very limted, some websites will fail to load and there's a 30 minute timeout which closes the browser"
        ));
    }
}

void App::DisplayAdvancedOptions(bool left_side) {
    auto options = std::make_unique<ui::Sidebar>("Advanced Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    ui::SidebarEntryArray::Items text_scroll_speed_items;
    text_scroll_speed_items.push_back("Slow"_i18n);
    text_scroll_speed_items.push_back("Normal"_i18n);
    text_scroll_speed_items.push_back("Fast"_i18n);

    std::vector<std::string> menu_names;
    ui::SidebarEntryArray::Items menu_items;
    for (auto& e : ui::menu::main::GetMenuMenuEntries()) {
        if (!e.IsShortcut()) {
            continue;
        }

        menu_names.emplace_back(e.name);
        menu_items.push_back(i18n::get(e.name));
    }

    options->Add<ui::SidebarEntryBool>("Logging"_i18n, App::GetLogEnable(), [](bool& enable){
        App::SetLogEnable(enable);
    }, "Logs to /config/sphaira/log.txt"_i18n);

    options->Add<ui::SidebarEntryBool>("Replace hbmenu on exit"_i18n, App::GetReplaceHbmenuEnable(), [](bool& enable){
        App::SetReplaceHbmenuEnable(enable);
    },  i18n::get("hbmenu_replace_info",
            "When enabled, it replaces /hbmenu.nro with Sphaira, creating a backup of hbmenu to /switch/hbmenu.nro\n\n"
            "Disabling will give you the option to restore hbmenu."));

    options->Add<ui::SidebarEntryCallback>("Add / modify mounts"_i18n, [](){
        devoptab::DisplayDevoptabSideBar();
    },  i18n::get("mount_options_info",
            "Create, modify, delete network mounts (HTTP, FTP, SFTP, SMB, NFS).\n"
            "Mount options only require a URL and Name be set, with other fields being optional, such as port, user, pass etc.\n\n"
            "Any changes made will require restarting Sphaira to take effect."));

    options->Add<ui::SidebarEntryBool>("Boost CPU during transfer"_i18n, App::GetApp()->m_progress_boost_mode,
        i18n::get("transfer_boost_info",
            "Enables boost mode during transfers which can improve transfer speed. "
            "This sets the CPU to 1785mhz and lowers the GPU 76mhz"));

    options->Add<ui::SidebarEntryArray>("Text scroll speed"_i18n, text_scroll_speed_items, [](s64& index_out){
        App::SetTextScrollSpeed(index_out);
    }, App::GetTextScrollSpeed(), "Change how fast the scrolling text updates"_i18n);

    options->Add<ui::SidebarEntryArray>("Set center menu"_i18n, menu_items, [menu_names](s64& index_out){
        const auto e = menu_names[index_out];
        if (g_app->m_center_menu.Get() != e) {
            // swap menus around.
            if (g_app->m_left_menu.Get() == e) {
                g_app->m_left_menu.Set(g_app->m_left_menu.Get());
            } else if (g_app->m_right_menu.Get() == e) {
                g_app->m_right_menu.Set(g_app->m_left_menu.Get());
            }
            g_app->m_center_menu.Set(e);

            App::Push<ui::OptionBox>(
                "Press OK to restart Sphaira"_i18n, "OK"_i18n, [](auto){
                    App::ExitRestart();
                }
            );
        }
    }, i18n::get(g_app->m_center_menu.Get()), "Set the menu that appears on the center tab."_i18n);


    options->Add<ui::SidebarEntryArray>("Set left-side menu"_i18n, menu_items, [menu_names](s64& index_out){
        const auto e = menu_names[index_out];
        if (g_app->m_left_menu.Get() != e) {
            // swap menus around.
            if (g_app->m_center_menu.Get() == e) {
                g_app->m_center_menu.Set(g_app->m_left_menu.Get());
            } else if (g_app->m_right_menu.Get() == e) {
                g_app->m_right_menu.Set(g_app->m_left_menu.Get());
            }
            g_app->m_left_menu.Set(e);

            App::Push<ui::OptionBox>(
                "Press OK to restart Sphaira"_i18n, "OK"_i18n, [](auto){
                    App::ExitRestart();
                }
            );
        }
    }, i18n::get(g_app->m_left_menu.Get()), "Set the menu that appears on the left tab."_i18n);

    options->Add<ui::SidebarEntryArray>("Set right-side menu"_i18n, menu_items, [menu_names](s64& index_out){
        const auto e = menu_names[index_out];
        if (g_app->m_right_menu.Get() != e) {
            // swap menus around.
            if (g_app->m_center_menu.Get() == e) {
                g_app->m_center_menu.Set(g_app->m_right_menu.Get());
            } else if (g_app->m_left_menu.Get() == e) {
                g_app->m_left_menu.Set(g_app->m_right_menu.Get());
            }
            g_app->m_right_menu.Set(e);

            App::Push<ui::OptionBox>(
                "Press OK to restart Sphaira"_i18n, "OK"_i18n, [](auto){
                    App::ExitRestart();
                }
            );
        }
    }, i18n::get(g_app->m_right_menu.Get()), "Set the menu that appears on the right tab."_i18n);

    options->Add<ui::SidebarEntryCallback>("Install options"_i18n, [left_side](){
        App::DisplayInstallOptions(left_side);
    },  i18n::get("install_options_info",
            "Change the install options.\n"
            "You can enable installing from here."));

    options->Add<ui::SidebarEntryCallback>("Export options"_i18n, [left_side](){
        App::DisplayDumpOptions(left_side);
    },  "Change the export options."_i18n);
}

void App::DisplayInstallOptions(bool left_side) {
    auto options = std::make_unique<ui::Sidebar>("Install Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    ui::SidebarEntryArray::Items install_items;
    install_items.push_back("System memory"_i18n);
    install_items.push_back("microSD card"_i18n);

    options->Add<ui::SidebarEntryBool>("Enable sysmmc"_i18n, App::GetInstallSysmmcEnable(), [](bool& enable){
        ShowEnableInstallPromptOption(g_app->m_install_sysmmc, enable);
    }, "Enables installing whilst in sysMMC mode."_i18n);

    options->Add<ui::SidebarEntryBool>("Enable emummc"_i18n, App::GetInstallEmummcEnable(), [](bool& enable){
        ShowEnableInstallPromptOption(g_app->m_install_emummc, enable);
    }, "Enables installing whilst in emuMMC mode."_i18n);

    options->Add<ui::SidebarEntryArray>("Install location"_i18n, install_items, [](s64& index_out){
        App::SetInstallSdEnable(index_out);
    }, (s64)App::GetInstallSdEnable());

    options->Add<ui::SidebarEntryBool>("Allow downgrade"_i18n, App::GetApp()->m_allow_downgrade,
        "Allows for installing title updates that are lower than the currently installed update."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip if already installed"_i18n, App::GetApp()->m_skip_if_already_installed,
        "Skips installing titles / ncas if they're already installed."_i18n);

    options->Add<ui::SidebarEntryBool>("Ticket only"_i18n, App::GetApp()->m_ticket_only,
        "Installs tickets only, useful if the title was already installed however the tickets were missing or corrupted."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip base"_i18n, App::GetApp()->m_skip_base,
        "Skips installing the base application."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip patch"_i18n, App::GetApp()->m_skip_patch,
        "Skips installing updates."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip dlc"_i18n, App::GetApp()->m_skip_addon,
        "Skips installing DLC."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip data patch"_i18n, App::GetApp()->m_skip_data_patch,
        "Skips installing DLC update (data patch)."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip ticket"_i18n, App::GetApp()->m_skip_ticket,
        "Skips installing tickets, not recommended."_i18n);

    options->Add<ui::SidebarEntryBool>("Skip NCA hash verify"_i18n, App::GetApp()->m_skip_nca_hash_verify,
        i18n::get("skip_nca_info",
            "Enables the option to skip sha256 verification. This is a hash over the entire NCA. "
            "It is used to verify that the NCA is valid / not corrupted. "
            "You may have seen the option for \"checking for corrupted data\" when a corrupted game is installed. "
            "That check performs various hash checks, including the hash over the NCA.\n\n"
            "It is recommended to keep this disabled."));

    options->Add<ui::SidebarEntryBool>("Skip RSA header verify"_i18n, App::GetApp()->m_skip_rsa_header_fixed_key_verify,
        i18n::get("nca_verify_info",
            "Enables the option to skip RSA NCA fixed key verification. "
            "This is a hash over the NCA header. It is used to verify that the header has not been modified. "
            "The header is signed by nintendo, thus it cannot be forged, and is reliable to detect modified NCA headers (such as NSP/XCI converts).\n\n"
            "It is recommended to keep this disabled, unless you need to install nsp/xci converts."));

    options->Add<ui::SidebarEntryBool>("Skip RSA NPDM verify"_i18n, App::GetApp()->m_skip_rsa_npdm_fixed_key_verify,
        i18n::get("npdm_verify_info",
            "Enables the option to skip RSA NPDM fixed key verification.\n\n"
            "Currently, this option is stubbed (not implemented)."));

    options->Add<ui::SidebarEntryBool>("Ignore distribution bit"_i18n, App::GetApp()->m_ignore_distribution_bit,
        i18n::get("nca_distbit_info",
            "If set, it will ignore the distribution bit in the NCA header. "
            "The distribution bit is used to signify whether a NCA is Eshop or GameCard. "
            "You cannot (normally) launch install games that have the distruction bit set to GameCard.\n\n"
            "It is recommended to keep this disabled."));

    options->Add<ui::SidebarEntryBool>("Convert to common ticket"_i18n, App::GetApp()->m_convert_to_common_ticket,
        i18n::get("ticket_convert_info",
            "[Requires keys] Converts personalised tickets to common (fake) tickets.\n\n"
            "It is recommended to keep this enabled."));

    options->Add<ui::SidebarEntryBool>("Convert to standard crypto"_i18n, App::GetApp()->m_convert_to_standard_crypto,
        i18n::get("titlekey_crypto_info",
            "[Requires keys] Converts titlekey to standard crypto, also known as \"ticketless\".\n\n"
            "It is recommended to keep this disabled."));

    options->Add<ui::SidebarEntryBool>("Lower master key"_i18n, App::GetApp()->m_lower_master_key,
        i18n::get("keyarea_crypto_info",
            "[Requires keys] Encrypts the keak (key area key) with master key 0, which allows the game to be launched on every fw. "
            "Implicitly performs standard crypto.\n\n"
            "Do note that just because the game can be launched on any fw (as it can be decrypted), doesn't mean it will work. It is strongly recommened to update your firmware and Atmosphere version in order to play the game, rather than enabling this option.\n\n"
            "It is recommended to keep this disabled."));

    options->Add<ui::SidebarEntryBool>("Lower system version"_i18n, App::GetApp()->m_lower_system_version,
        i18n::get("cnmt_fw_info",
            "Sets the system_firmware field in the cnmt extended header to 0. "
            "Note: if the master key is higher than fw version, the game still won't launch as the fw won't have the key to decrypt keak (see above).\n\n"
            "It is recommended to keep this disabled."));
}

void App::DisplayDumpOptions(bool left_side) {
    auto options = std::make_unique<ui::Sidebar>("Export Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    ui::SidebarEntryArray::Items nsz_level_items;
    for (auto& e : NSZ_COMPRESS_LEVEL_OPTIONS) {
        nsz_level_items.emplace_back(i18n::get(e.name));
    }

    ui::SidebarEntryArray::Items nsz_thread_items;
    for (auto& e : NSZ_COMPRESS_THREAD_OPTIONS) {
        nsz_thread_items.emplace_back(i18n::get(e.name));
    }

    ui::SidebarEntryArray::Items nsz_block_items;
    for (auto& e : NSZ_COMPRESS_BLOCK_OPTIONS) {
        nsz_block_items.emplace_back(i18n::get(e.name));
    }

    options->Add<ui::SidebarEntryBool>(
        "Fix export filenames"_i18n, App::GetApp()->m_dump_fix_filenames,
        i18n::get("fix_export_filenames_info",
            "Uses the English title for export filenames and falls back to the Title ID when unavailable. "
            "Disable this to preserve localized and Unicode titles on destinations that support them."
        )
    );
    options->Add<ui::SidebarEntryBool>(
        "Created nested folder"_i18n, App::GetApp()->m_dump_app_folder,
        i18n::get("game_folder_info",
            "Creates a folder using the name of the game.\n"
            "For example, /name/name.xci\n"
            "Disabling this would use /name.xci"
        )
    );
    options->Add<ui::SidebarEntryBool>(
        "Append folder with .xci"_i18n, App::GetApp()->m_dump_append_folder_with_xci,
        i18n::get("xci_folder_info",
            "XCI dumps will name the folder with the .xci extension.\n"
            "For example, /name.xci/name.xci\n\n"
            "Some devices only function is the xci folder is named exactly the same as the xci."
        )
    );
    options->Add<ui::SidebarEntryBool>(
        "Trim XCI"_i18n, App::GetApp()->m_dump_trim_xci,
        "Removes the unused data at the end of the XCI, making the output smaller."_i18n
    );
    options->Add<ui::SidebarEntryBool>(
        "Label trimmed XCI"_i18n, App::GetApp()->m_dump_label_trim_xci,
        "Names the trimmed xci.\n"
        "For example, /name/name (trimmed).xci"_i18n
    );
    options->Add<ui::SidebarEntryBool>(
        "Convert to common ticket"_i18n, App::GetApp()->m_dump_convert_to_common_ticket,
        "Converts personalised ticket to a fake common ticket."_i18n
    );

    options->Add<ui::SidebarEntryArray>("NSZ level"_i18n, nsz_level_items, [](s64& index_out){
        App::GetApp()->m_nsz_compress_level.Set(index_out);
    }, App::GetApp()->m_nsz_compress_level.Get(),
        i18n::get("compress_level_info",
            "Sets the compression level used when exporting to NSZ.\n\n"
            "NOTE: The switch CPU is not very fast, and setting the value too high can "
            "result in exporting taking a very long time for very little gain in size.\n\n"
            "It is recommended to set this value to 3."
        )
    );

    options->Add<ui::SidebarEntryArray>("NSZ threads"_i18n, nsz_thread_items, [](s64& index_out){
        App::GetApp()->m_nsz_compress_threads.Set(index_out);
    }, App::GetApp()->m_nsz_compress_threads.Get(),
        i18n::get("compress_threads_info",
            "Sets the number of threads used when compression the NCA.\n\n"
            "A value less than 3 allows for another thread to run freely, such as read/write threads. "
            "However in my testing, a value of 3 was usually the most performant.\n"
            "A value of 0 will use no threads and should only be used for testing as it is always slower.\n\n"
            "It is recommended to set this value between 1-3."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "NSZ long distance mode"_i18n, App::GetApp()->m_nsz_compress_ldm,
        "Enables \"Long Distance Mode\" which can reduce the output size at the cost of speed."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "NSZ block compression"_i18n, App::GetApp()->m_nsz_compress_block,
        i18n::get("block_compress_info",
            "Enables block compression, which compresses the NCA into blocks (at the cost of compression ratio) "
            "which allows for random access, allowing the NCZ to be mounted as a file system.\n\n"
            "NOTE: Sphaira does not yet support mounting NCZ as a file system, but it will be added evntually."
        )
    );

    auto block_size_option = options->Add<ui::SidebarEntryArray>("NSZ block size"_i18n, nsz_block_items, [](s64& index_out){
        App::GetApp()->m_nsz_compress_block_exponent.Set(index_out);
    }, App::GetApp()->m_nsz_compress_block_exponent.Get(),
        "Sets the size of each block. The smaller the size, the faster the random access is at the cost of compression ratio."_i18n
    );
    block_size_option->Depends(App::GetApp()->m_nsz_compress_block, "NSZ block compression is disabled."_i18n);
}

void App::DisplayFtpOptions(bool left_side) {
    // todo: prompt on exit to restart ftp server if options were changed.
    auto options = std::make_unique<ui::Sidebar>("FTP Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->SetOnExitWhenChanged([](){
        if (App::GetFtpEnable()) {
            App::Notify("Restarting FTP server..."_i18n);
            App::SetFtpEnable(false);
            App::SetFtpEnable(true);
        }
    });

    options->Add<ui::SidebarEntryBool>("Enable"_i18n, App::GetFtpEnable(), [](bool& enable){
        App::SetFtpEnable(enable);
    },  "Enable FTP server to run in the background."_i18n);

    options->Add<ui::SidebarEntryTextInput>(
        "Port"_i18n, App::GetApp()->m_ftp_port.Get(), "", "", 1, 5,
        "Opens the FTP server on this port."_i18n,
        [](auto* input){
            App::GetApp()->m_ftp_port.Set(input->GetNumValue());
        }
    );

    options->Add<ui::SidebarEntryBool>(
        "Anon"_i18n, App::GetApp()->m_ftp_anon,
        i18n::get("login_require_info",
            "Allows you to login without setting a username and password.\n"
            "If disabled, you must set a user name and password below!"
        )
    );

    options->Add<ui::SidebarEntryTextInput>(
        "User"_i18n, App::GetApp()->m_ftp_user.Get(), "", "", -1, 64,
        "Sets the username, must be set if anon is disabled."_i18n,
        [](auto* input){
            App::GetApp()->m_ftp_user.Set(input->GetValue());
        }
    );

    options->Add<ui::SidebarEntryTextInput>(
        "Pass"_i18n, App::GetApp()->m_ftp_pass.Get(), "", "", -1, 64,
        "Sets the password, must be set if anon is disabled."_i18n,
        [](auto* input){
            App::GetApp()->m_ftp_pass.Set(input->GetValue());
        }
    );

    options->Add<ui::SidebarEntryBool>(
        "Show album"_i18n, App::GetApp()->m_ftp_show_album,
        "Shows the microSD card album folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show Atmosphere contents"_i18n, App::GetApp()->m_ftp_show_ams_contents,
        "Shows the shortcut for the /atmosphere/contents folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show bis storage"_i18n, App::GetApp()->m_ftp_show_bis_storage,
        i18n::get("bis_contents_info",
            "Shows the bis folder which contains the following:\n"
            "- BootPartition1Root.bin\n"
            "- BootPartition2Root.bin\n"
            "- UserDataRoot.bin\n"
            "- BootConfigAndPackage2Part1.bin\n"
            "- BootConfigAndPackage2Part2.bin\n"
            "- BootConfigAndPackage2Part3.bin\n"
            "- BootConfigAndPackage2Part4.bin\n"
            "- BootConfigAndPackage2Part5.bin\n"
            "- BootConfigAndPackage2Part6.bin\n"
            "- CalibrationFile.bin\n"
            "- SafeMode.bin\n"
            "- User.bin\n"
            "- System.bin\n"
            "- SystemProperEncryption.bin"
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show bis file systems"_i18n, App::GetApp()->m_ftp_show_bis_fs,
        i18n::get("bis_fs_info",
            "Shows the following bis file systems:\n"
            "- bis_calibration_file\n"
            "- bis_safe_mode\n"
            "- bis_user\n"
            "- bis_system"
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show system contents"_i18n, App::GetApp()->m_ftp_show_content_system,
        "Shows the system contents folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show user contents"_i18n, App::GetApp()->m_ftp_show_content_user,
        "Shows the user contents folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show microSD contents"_i18n, App::GetApp()->m_ftp_show_content_sd,
        i18n::get("microsd_contents_info",
            "Shows the microSD contents folder.\n\n"
            "NOTE: This is not the normal microSD card storage, it is instead "
            "the location where NCA's are stored. The normal microSD card is always mounted."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show games"_i18n, App::GetApp()->m_ftp_show_games,
        i18n::get("games_ftp_info",
            "Shows the games folder.\n\n"
            "This folder contains all of your installed games, allowing you to create "
            "backups over FTP!\n\n"
            "NOTE: This folder is read-only. You cannot delete games over FTP."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show install"_i18n, App::GetApp()->m_ftp_show_install,
        i18n::get("install_ftp_info",
            "Shows the install folder.\n\n"
            "This folder is used for installing games via FTP.\n\n"
            "NOTE: You must open the \"FTP Install\" menu when trying to install a game!"
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show mounts"_i18n, App::GetApp()->m_ftp_show_mounts,
        i18n::get("mounts_ftp_info",
            "Shows the mounts folder.\n\n"
            "This folder is contains all of the mounts added to Sphaira, allowing you to acces them over FTP!\n"
            "For example, you can access your SMB, WebDav or other FTP mounts over FTP."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show switch"_i18n, App::GetApp()->m_ftp_show_switch,
        i18n::get("homebrew_folder_info",
            "Shows the shortcut for the /switch folder."
            "This is the folder that contains all your homebrew (NRO's)."
        )
    );
}

void App::DisplayMtpOptions(bool left_side) {
    // todo: prompt on exit to restart ftp server if options were changed.
    auto options = std::make_unique<ui::Sidebar>("MTP Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->SetOnExitWhenChanged([](){
        if (App::GetMtpEnable()) {
            App::Notify("Restarting MTP server..."_i18n);
            App::SetMtpEnable(false);
            App::SetMtpEnable(true);
        }
    });

    options->Add<ui::SidebarEntryBool>("Enable"_i18n, App::GetMtpEnable(), [](bool& enable){
        App::SetMtpEnable(enable);
    },  "Enable MTP server to run in the background."_i18n);

    // not sure if i want to expose this to users yet.
    // its also stubbed currently.
    #if 0
    options->Add<ui::SidebarEntryBool>(
        "Pre-allocate file"_i18n, App::GetApp()->m_mtp_allocate_file,
        i18n::get("prealloc_info",
            "Enables pre-allocating the file size before writing.\n"
            "This speeds up file writes, however, this can cause timeouts if all these conditions are met:\n"
            "- using Windows\n"
            "- using emuMMC\n"
            "- transferring a large file (>1GB)\n\n"
            "This option should be left enabled, however if you use the above and experience timeouts, "
            "then try again with this option disabled."
        )
    );
    #endif

    options->Add<ui::SidebarEntryBool>(
        "Show album"_i18n, App::GetApp()->m_mtp_show_album,
        "Shows the microSD card album folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show microSD contents"_i18n, App::GetApp()->m_mtp_show_content_sd,
        i18n::get("microsd_contents_info",
            "Shows the microSD contents folder.\n\n"
            "NOTE: This is not the normal microSD card storage, it is instead "
            "the location where NCA's are stored. The normal microSD card is always mounted."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show system contents"_i18n, App::GetApp()->m_mtp_show_content_system,
        "Shows the system contents folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show user contents"_i18n, App::GetApp()->m_mtp_show_content_user,
        "Shows the user contents folder."_i18n
    );

    options->Add<ui::SidebarEntryBool>(
        "Show games"_i18n, App::GetApp()->m_mtp_show_games,
        i18n::get("games_mtp_info",
            "Shows the games folder.\n\n"
            "This folder contains all of your installed games, allowing you to create "
            "backups over MTP!\n\n"
            "NOTE: This folder is read-only. You cannot delete games over MTP."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show install"_i18n, App::GetApp()->m_mtp_show_install,
        i18n::get("install_mtp_info",
            "Shows the install folder.\n\n"
            "This folder is used for installing games via MTP.\n\n"
            "NOTE: You must open the \"MTP Install\" menu when trying to install a game!"
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show mounts"_i18n, App::GetApp()->m_mtp_show_mounts,
        i18n::get("mounts_mtp_info",
            "Shows the mounts folder.\n\n"
            "This folder is contains all of the mounts added to Sphaira, allowing you to acces them over MTP!\n"
            "For example, you can access your SMB, WebDav and FTP mounts over MTP."
        )
    );

    options->Add<ui::SidebarEntryBool>(
        "Show DevNull"_i18n, App::GetApp()->m_mtp_show_speedtest,
        i18n::get("usb_benchmark_info",
            "Shows the DevNull (Speed Test) folder.\n\n"
            "This folder is used for benchmarking USB uploads. "
            "This ia virtual folder, nothing is actally written to disk."
        )
    );
}

void App::DisplayHddOptions(bool left_side) {
    auto options = std::make_unique<ui::Sidebar>("HDD Options"_i18n, left_side ? ui::Sidebar::Side::LEFT : ui::Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<ui::SidebarEntryBool>("Enable"_i18n, App::GetHddEnable(), [](bool& enable){
        App::SetHddEnable(enable);
    },  i18n::get("mount_hdd_info",
            "Enable mounting of connected USB/HDD devices. "
            "Connected devices can be used in the FileBrowser, as well as a backup location when dumping games and saves."
        )
    );

    options->Add<ui::SidebarEntryBool>("HDD write protect"_i18n, App::GetWriteProtect(), [](bool& enable){
        App::SetWriteProtect(enable);
    },  "Makes the connected HDD read-only."_i18n);

#ifdef ENABLE_NTFS
    options->Add<ui::SidebarEntryBool>("Show system files"_i18n, App::GetHddShowSystemFiles(), [](bool& enable){
        App::SetHddShowSystemFiles(enable);
    },  i18n::get("hdd_show_system_files_info",
            "NTFS only. Shows entries that Windows marked as system files, such as $Recycle.Bin. "
            "Leave this off unless you know what you are doing."
        )
    );

    options->Add<ui::SidebarEntryBool>("Mount hibernated NTFS"_i18n, App::GetHddIgnoreHibernation(), [](bool& enable){
        App::SetHddIgnoreHibernation(enable);
    },  i18n::get("hdd_ignore_hibernation_info",
            "NTFS only. Mounts a drive that Windows left in a hibernated state (fast startup). "
            "WARNING: the saved Windows session on that drive is lost."
        )
    );
#endif // ENABLE_NTFS
}

void App::ShowEnableInstallPrompt() {
    // warn the user the dangers of installing.
    App::Push<ui::OptionBox>(
        "Installing is disabled, enable now?"_i18n,
        "Back"_i18n, "Enable"_i18n, 0, [](auto op_index){
            if (op_index && *op_index) {
                // get the install option based on sysmmc/emummc.
                auto& option = IsEmummc() ? g_app->m_install_emummc : g_app->m_install_sysmmc;

                // dummy ref.
                static bool enable{};
                enable = true;

                return ShowEnableInstallPromptOption(option, enable);
            }
        }
    );
}

void App::ShowEnableInstallPromptOption(option::OptionBool& option, bool& enable) {
    if (enable) {
        // warn the user the dangers of installing.
        App::Push<ui::OptionBox>(
            "WARNING: Installing apps will lead to a ban!"_i18n,
            "Back"_i18n, "Enable"_i18n, 0, [&option, &enable](auto op_index){
                if (op_index && *op_index) {
                    option.Set(true);
                    App::Notify("Installing enabled!"_i18n);
                } else {
                    enable = false;
                }
            }
        );
    } else {
        option.Set(false);
    }
}

App::~App() {
    // boost mode is disabled in userAppExit().
    App::SetBoostMode(true);

    log_write("starting to exit\n");
    {
        SCOPED_TIMESTAMP("TOTAL EXIT");
        appletUnhook(&m_appletHookCookie);

        // async exit as these threads sleep every 100ms.
        {
            SCOPED_TIMESTAMP("async signal");
#ifdef ENABLE_FTPSRV
            ftpsrv::ExitSignal();
#endif // ENABLE_FTPSRV
            nxlinkSignalExit();
            audio::ExitSignal();
            curl::ExitSignal();
        }

        // this has to be called before any cleanup to ensure the lifetime of
        // nvg is still active as some widgets may need to free images.
        // clear in reverse order as the widgets are a stack.
        {
            SCOPED_TIMESTAMP("widget exit");
            while (!m_widgets.empty()) {
                m_widgets.pop_back();
            }
        }

        utils::Async async_exit([this](){
            {
                SCOPED_TIMESTAMP("i18n_exit");
                i18n::exit();
            }

#ifdef ENABLE_LIBUSBDVD
            {
                SCOPED_TIMESTAMP("usbdvd_exit");
                usbdvd::UnmountAll();
            }
#endif // ENABLE_LIBUSBDVD

#ifdef ENABLE_LIBHAZE
            {
                SCOPED_TIMESTAMP("mtp exit");
                libhaze::Exit();
            }
#endif // ENABLE_LIBHAZE

#ifdef ENABLE_LIBUSBHSFS
            {
                SCOPED_TIMESTAMP("hdd exit");
                usbHsFsExit();
            }
#endif // ENABLE_LIBUSBHSFS

            // this has to come before curl exit as it uses curl global.
            {
                SCOPED_TIMESTAMP("devoptab exit");
                devoptab::UmountAllNeworkDevices();
            }

            // do these last as they were signalled to exit.
            {
                SCOPED_TIMESTAMP("audio_exit");
                audio::CloseSong(&m_background_music);
                audio::Exit();
            }

#ifdef ENABLE_FTPSRV
            {
                SCOPED_TIMESTAMP("ftp exit");
                ftpsrv::Exit();
            }
#endif // ENABLE_FTPSRV

            {
                SCOPED_TIMESTAMP("nxlink exit");
                nxlinkExit();
            }

            {
                SCOPED_TIMESTAMP("curl_exit");
                curl::Exit();
            }
        });

        // do not async close theme as it frees textures.
        {
            SCOPED_TIMESTAMP("theme exit");
            ini_puts("config", "theme", m_theme.meta.ini_path, CONFIG_PATH);
            CloseTheme();
        }

        {
            SCOPED_TIMESTAMP("destroy frame buffer resources");
            this->destroyFramebufferResources();
        }

        {
            SCOPED_TIMESTAMP("nvg exit");
            nvgDeleteImage(vg, m_default_image);
            nvgDeleteDk(this->vg);
            this->renderer.reset();

    #ifdef USE_NVJPG
            m_decoder.finalize();
            nj::finalize();
    #endif
        }

        // backup hbmenu if it is not sphaira
        {
            SCOPED_TIMESTAMP("nro copy main");
            if (App::GetReplaceHbmenuEnable() && !IsHbmenu()) {
                NacpStruct hbmenu_nacp;
                Result rc;

                // todo: don't read whole nacp, only the name.
                // todo: keep file open and use that as part of the file copy.
                if (R_SUCCEEDED(rc = nro_get_nacp("/hbmenu.nro", hbmenu_nacp)) && std::strcmp(NacpLanguageEntries(hbmenu_nacp)[0].name, "sphaira")) {
                    log_write("backing up hbmenu.nro\n");
                    if (R_FAILED(rc = m_fs->copy_entire_file("/switch/hbmenu.nro", "/hbmenu.nro"))) {
                        log_write("failed to backup  hbmenu.nro\n");
                    }
                } else {
                    log_write("not backing up\n");
                }

                if (R_FAILED(rc = m_fs->copy_entire_file("/hbmenu.nro", GetExePath()))) {
                    log_write("failed to copy entire file: %s 0x%X module: %u desc: %u\n", GetExePath().s, rc, R_MODULE(rc), R_DESCRIPTION(rc));
                } else {
                    log_write("success with copying over root file!\n");
                }
            } else if (IsHbmenu()) {
                // check we have a version that's newer than current.
                NacpStruct hbmenu_nacp;
                Result rc;

                // ensure that are still sphaira
                if (R_SUCCEEDED(rc = nro_get_nacp("/hbmenu.nro", hbmenu_nacp)) && !std::strcmp(NacpLanguageEntries(hbmenu_nacp)[0].name, "sphaira")) {
                    NacpStruct sphaira_nacp;
                    fs::FsPath sphaira_path = "/switch/sphaira/sphaira.nro";

                    rc = nro_get_nacp(sphaira_path, sphaira_nacp);
                    if (R_FAILED(rc) || std::strcmp(NacpLanguageEntries(sphaira_nacp)[0].name, "sphaira")) {
                        sphaira_path = "/switch/sphaira.nro";
                        rc = nro_get_nacp(sphaira_path, sphaira_nacp);
                    }

                    // found sphaira, now lets get compare version
                    if (R_SUCCEEDED(rc) && !std::strcmp(NacpLanguageEntries(sphaira_nacp)[0].name, "sphaira")) {
                        if (IsVersionNewer(hbmenu_nacp.display_version, sphaira_nacp.display_version)) {
                            if (R_FAILED(rc = m_fs->copy_entire_file(GetExePath(), sphaira_path))) {
                                log_write("failed to copy entire file: %s 0x%X module: %u desc: %u\n", sphaira_path.s, rc, R_MODULE(rc), R_DESCRIPTION(rc));
                            } else {
                                log_write("success with updating hbmenu!\n");
                            }
                        }
                    }
                } else {
                    log_write("no longer hbmenu!\n");
                }
            }
        }
    }

    if (App::GetLogEnable()) {
        log_write("closing log\n");
        log_file_exit();
    }
}

auto App::GetVersionFromString(const char* str) -> u32 {
    u32 major{}, minor{}, macro{};
    std::sscanf(str, "%u.%u.%u", &major, &minor, &macro);
    return MAKEHOSVERSION(major, minor, macro);
}

auto App::IsVersionNewer(const char* current, const char* new_version) -> u32 {
    return GetVersionFromString(current) < GetVersionFromString(new_version);
}

void App::createFramebufferResources() {
    this->swapchain = nullptr;

    // Create layout for the depth buffer
    dk::ImageLayout layout_depthbuffer;
    dk::ImageLayoutMaker{device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_S8)
        .setDimensions(s_width, s_height)
        .initialize(layout_depthbuffer);

    // Create the depth buffer
    this->depthBuffer_mem = this->pool_images->allocate(layout_depthbuffer.getSize(), layout_depthbuffer.getAlignment());
    this->depthBuffer.initialize(layout_depthbuffer, this->depthBuffer_mem.getMemBlock(), this->depthBuffer_mem.getOffset());

    // Create layout for the framebuffers
    dk::ImageLayout layout_framebuffer;
    dk::ImageLayoutMaker{device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(s_width, s_height)
        .initialize(layout_framebuffer);

    // Create the framebuffers
    std::array<DkImage const*, NumFramebuffers> fb_array;
    const u64 fb_size  = layout_framebuffer.getSize();
    const uint32_t fb_align = layout_framebuffer.getAlignment();
    for (unsigned i = 0; i < fb_array.size(); i++) {
        // Allocate a framebuffer
        this->framebuffers_mem[i] = pool_images->allocate(fb_size, fb_align);
        this->framebuffers[i].initialize(layout_framebuffer, framebuffers_mem[i].getMemBlock(), framebuffers_mem[i].getOffset());

        // Generate a command list that binds it
        dk::ImageView colorTarget{ framebuffers[i] }, depthTarget{ depthBuffer };
        this->cmdbuf.bindRenderTargets(&colorTarget, &depthTarget);
        this->framebuffer_cmdlists[i] = cmdbuf.finishList();

        // Fill in the array for use later by the swapchain creation code
        fb_array[i] = &framebuffers[i];
    }

    // Create the swapchain using the framebuffers
    this->swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

    // Generate the main rendering cmdlist
    this->recordStaticCommands();
}

void App::destroyFramebufferResources() {
    // Return early if we have nothing to destroy
    if (!this->swapchain) {
        return;
    }

    this->queue.waitIdle();
    this->cmdbuf.clear();
    swapchain.destroy();

    // Destroy the framebuffers
    for (unsigned i = 0; i < NumFramebuffers; i++) {
        framebuffers_mem[i].destroy();
    }

    // Destroy the depth buffer
    this->depthBuffer_mem.destroy();
}

void App::recordStaticCommands() {
    // Initialize state structs with deko3d defaults
    dk::RasterizerState rasterizerState;
    dk::ColorState colorState;
    dk::ColorWriteState colorWriteState;
    dk::BlendState blendState;

    // Configure the viewport and scissor
    this->cmdbuf.setViewports(0, { { 0.0f, 0.0f, (float)s_width, (float)s_height, 0.0f, 1.0f } });
    this->cmdbuf.setScissors(0, { { 0, 0, (u32)s_width, (u32)s_height } });

    // Clear the color and depth buffers
    this->cmdbuf.clearColor(0, DkColorMask_RGBA, 0.2f, 0.3f, 0.3f, 1.0f);
    this->cmdbuf.clearDepthStencil(true, 1.0f, 0xFF, 0);

    // Bind required state
    this->cmdbuf.bindRasterizerState(rasterizerState);
    this->cmdbuf.bindColorState(colorState);
    this->cmdbuf.bindColorWriteState(colorWriteState);

    this->render_cmdlist = this->cmdbuf.finishList();
}

} // namespace sphaira

#include "ui/menus/mtp_menu.hpp"
#include "usb/usbds.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include "haze_helper.hpp"

namespace sphaira::ui::menu::mtp {

Menu::Menu(u32 flags) : stream::Menu{"MTP Install"_i18n, flags} {
    m_was_mtp_enabled = libhaze::IsInit();
    if (!m_was_mtp_enabled) {
        log_write("[MTP] wasn't enabled, forcefully enabling\n");
        libhaze::Init();
    }

    UpdateSubHeading();

    libhaze::InitInstallMode(
        [this](const char* path){ return OnInstallStart(path); },
        [this](const void *buf, size_t size){ return OnInstallWrite(buf, size); },
        [this](){ return OnInstallClose(); }
    );
}

Menu::~Menu() {
    // unblocks any haze callback that is currently waiting on us and then
    // disables install mode. must happen before libhaze::Exit(), which joins
    // the haze thread.
    CancelInstallMode();

    if (!m_was_mtp_enabled) {
        log_write("[MTP] disabling on exit\n");
        libhaze::Exit();
    }
}

void Menu::UpdateSubHeading() {
    m_poll_ts.Update();

    UsbState state{UsbState_Detached};
    usbDsGetState(&state);

    UsbDeviceSpeed speed{(UsbDeviceSpeed)UsbDeviceSpeed_None};
    usbDsGetSpeed(&speed);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "State: %s | Speed: %s"_i18n.c_str(), i18n::get(GetUsbDsStateStr(state)).c_str(), i18n::get(GetUsbDsSpeedStr(speed)).c_str());
    SetSubHeading(buf);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    stream::Menu::Update(controller, touch);

    if (m_poll_ts.GetSeconds() >= 1) {
        UpdateSubHeading();
    }
}

void Menu::OnDisableInstallMode() {
    libhaze::DisableInstallMode();
}

bool Menu::IsInstallModeActive() const {
    return libhaze::IsInstallModeEnabled();
}

} // namespace sphaira::ui::menu::mtp

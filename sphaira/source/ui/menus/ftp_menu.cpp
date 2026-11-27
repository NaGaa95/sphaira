#include "ui/menus/ftp_menu.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include "ftpsrv_helper.hpp"

namespace sphaira::ui::menu::ftp {

Menu::Menu(u32 flags) : stream::Menu{"FTP Install"_i18n, flags} {
    m_was_ftp_enabled = App::GetFtpEnable();
    if (!m_was_ftp_enabled) {
        log_write("[FTP] wasn't enabled, forcefully enabling\n");
        App::SetFtpEnable(true);
    }

    ftpsrv::InitInstallMode(
        [this](const char* path){ return OnInstallStart(path); },
        [this](const void *buf, size_t size){ return OnInstallWrite(buf, size); },
        [this](){ (void)OnInstallClose(); }
    );

    SetListRect({485.f, GetY() + 1.f + 42.f, 720.f, 60.f});

    m_port = ftpsrv::GetPort();
    m_anon = ftpsrv::IsAnon();
    if (!m_anon) {
        m_user = ftpsrv::GetUser();
        m_pass = ftpsrv::GetPass();
    }
}

Menu::~Menu() {
    // unblocks any ftp callback that is currently waiting on us and then
    // disables install mode. must happen before the server is torn down.
    CancelInstallMode();

    if (!m_was_ftp_enabled) {
        log_write("[FTP] disabling on exit\n");
        App::SetFtpEnable(false);
    }
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    stream::Menu::Update(controller, touch);
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    stream::Menu::Draw(vg, theme);

    const auto pdata = GetPolledData();
    if (pdata.ip) {
        if (pdata.type == NifmInternetConnectionType_WiFi) {
            SetSubHeading("Connection Type: WiFi | Strength: "_i18n + std::to_string(pdata.strength));
        } else {
            SetSubHeading("Connection Type: Ethernet"_i18n);
        }
    } else {
        SetSubHeading("Connection Type: None"_i18n);
    }

    const float start_x = 80;
    const float font_size = 22;
    const float spacing = 33;
    float start_y = 125;
    float bounds[4];

    nvgFontSize(vg, font_size);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    // note: textbounds strips spaces...todo: use nvgTextGlyphPositions() instead.
    #define draw(key, ...) \
        gfx::textBounds(vg, start_x, start_y, bounds, key.c_str()); \
        gfx::drawTextArgs(vg, start_x, start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), key.c_str()); \
        gfx::drawTextArgs(vg, bounds[2], start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED), __VA_ARGS__); \
        start_y += spacing;

    if (pdata.ip) {
        draw("Host:"_i18n, " %u.%u.%u.%u", pdata.ip&0xFF, (pdata.ip>>8)&0xFF, (pdata.ip>>16)&0xFF, (pdata.ip>>24)&0xFF);
        draw("Port:"_i18n, " %u", m_port);
        if (!m_anon) {
            draw("Username:"_i18n, " %s", m_user);
            draw("Password:"_i18n, " %s", m_pass);
        }

        if (pdata.type == NifmInternetConnectionType_WiFi) {
            NifmNetworkProfileData profile{};
            if (R_SUCCEEDED(nifmGetCurrentNetworkProfile(&profile))) {
                const auto& settings = profile.wireless_setting_data;

                // convert to ascii string.
                char passphrase[sizeof(settings.passphrase) + 1]{};
                std::transform(std::cbegin(settings.passphrase), std::cend(settings.passphrase), std::begin(passphrase), toascii);

                draw("SSID:"_i18n, " %.*s", settings.ssid_len, settings.ssid);
                draw("Passphrase:"_i18n, " %s", passphrase);
            }
        }
    }

    #undef draw
}

void Menu::OnDisableInstallMode() {
    ftpsrv::DisableInstallMode();
}

bool Menu::IsInstallModeActive() const {
    return ftpsrv::IsInstallModeEnabled();
}

} // namespace sphaira::ui::menu::ftp

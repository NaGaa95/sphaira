#include "ui/menus/album_share_menu.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"

#include <cstdio>

namespace sphaira::ui::menu::album {
namespace {

constexpr float TEXT_X = 90;
constexpr float TEXT_Y = 190;

constexpr float QR_X = 820;
constexpr float QR_Y = 170;
constexpr float QR_MAX_SIZE = 300;

} // namespace

ShareMenu::ShareMenu(albumsrv::Items&& items, u16 port)
: MenuBase{"Album"_i18n, MenuFlag_None} {
    this->SetAction(Button::B, Action{"Back"_i18n, [this](){
        SetPop();
    }});

    SetTitleSubHeading("Browse from phone"_i18n);
    SetSubHeading("The album is only shared whilst this screen is open."_i18n);

    m_server = std::make_unique<albumsrv::Server>(std::move(items), port);
    SetAddress(GetPolledData().ip);
}

ShareMenu::~ShareMenu() {
}

void ShareMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    // the console may not have been on the network when this screen opened.
    const auto pdata = GetPolledData();
    if (pdata.ip != m_ip) {
        SetAddress(pdata.ip);
    }
}

void ShareMenu::SetAddress(u32 ip) {
    m_ip = ip;
    m_url.clear();
    m_qr = {};

    if (!ip) {
        return;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u:%u",
        ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, (unsigned)m_server->GetPort());
    m_url = buf;

    // the code carries the pin as well as the address, so that scanning it is
    // all that a phone has to do.
    const auto link = m_url + "/?p=" + m_server->GetPin();
    if (!m_qr.Encode(link)) {
        log_write("[ALBUM] failed to encode qr code for: %s\n", link.c_str());
    }
}

void ShareMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    float y = TEXT_Y;
    const auto line = [&](float size, ThemeEntryID id, const char* str) {
        gfx::drawTextArgs(vg, TEXT_X, y, size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(id), "%s", str);
        y += size + 14.f;
    };

    if (m_server->IsFailed()) {
        line(26.f, ThemeEntryID_ERROR, "Failed to start the server"_i18n.c_str());
        line(20.f, ThemeEntryID_TEXT_INFO, "Something else may already be using the port."_i18n.c_str());
        return;
    }

    if (!m_ip) {
        line(26.f, ThemeEntryID_ERROR, "No network connection"_i18n.c_str());
        return;
    }

    line(20.f, ThemeEntryID_TEXT_INFO, "Open this address on a phone or PC"_i18n.c_str());
    line(30.f, ThemeEntryID_TEXT_SELECTED, m_url.c_str());

    y += 16.f;
    line(20.f, ThemeEntryID_TEXT_INFO, "PIN"_i18n.c_str());
    line(30.f, ThemeEntryID_TEXT_SELECTED, m_server->GetPin().c_str());

    y += 16.f;
    const auto requests = m_server->GetRequestCount();
    if (requests) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: %u", "Requests"_i18n.c_str(), requests);
        line(20.f, ThemeEntryID_TEXT, buf);
    } else {
        line(20.f, ThemeEntryID_TEXT_INFO, "Waiting for a connection"_i18n.c_str());
    }

    if (m_server->IsLockedOut()) {
        line(20.f, ThemeEntryID_ERROR, "Too many wrong PINs, close and re-open to try again"_i18n.c_str());
    }

    const auto size = gfx::drawQrCode(vg, QR_X, QR_Y, QR_MAX_SIZE, m_qr);
    if (size > 0.F) {
        gfx::drawTextArgs(vg, QR_X + size / 2, QR_Y + size + 12, 18, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", "Scan to open the album"_i18n.c_str());
    }
}

} // namespace sphaira::ui::menu::album

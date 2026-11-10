#include "app.hpp"
#include "log.hpp"
#include "ui/menus/menu_base.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"

namespace sphaira::ui::menu {

auto MenuBase::GetPolledData(bool force_refresh) -> PolledData {
    static PolledData data{};
    static TimeStamp timestamp{};
    static bool has_init = false;

    if (!has_init) {
        has_init = true;
        force_refresh = true;
    }

    // update every second, do this in Draw because Update() isn't called if it
    // doesn't have focus.
    if (force_refresh || timestamp.GetSeconds() >= 1) {
        data.tm = {};
        data.type = {};
        data.status = {};
        data.strength = {};
        data.ip = {};
        // avoid divide by zero if getting the size fails, for whatever reason.
        data.sd_free = 1;
        data.sd_total = 1;
        data.emmc_free = 1;
        data.emmc_total = 1;

        const auto t = std::time(NULL);
        localtime_r(&t, &data.tm);
        nifmGetInternetConnectionStatus(&data.type, &data.strength, &data.status);
        nifmGetCurrentIpAddress(&data.ip);

        App::GetSdSize(&data.sd_free, &data.sd_total);
        App::GetEmmcSize(&data.emmc_free, &data.emmc_total);

        timestamp.Update();
    }

    return data;
}

MenuBase::MenuBase(const std::string& title, u32 flags) : m_title{title}, m_flags{flags} {
    // this->SetParent(this);
    this->SetPos(30, 87, 1220 - 30, 646 - 87);
    SetAction(Button::START, Action{App::Exit});
}

MenuBase::~MenuBase() {
}

void MenuBase::Update(Controller* controller, TouchInfo* touch) {
    Widget::Update(controller, touch);
}

void MenuBase::Draw(NVGcontext* vg, Theme* theme) {
    DrawElement(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, ThemeEntryID_BACKGROUND);
    Widget::Draw(vg, theme);

    const auto pdata = GetPolledData();

    const float start_y = 70;
    const float font_size = 20;
    const float spacing = 30;

    float start_x = 1220;
    float bounds[4];

    nvgFontSize(vg, font_size);

    #define STORAGE_BAR_W   180
    #define STORAGE_BAR_H   8

    const auto rounding = 2;
    const auto storage_font = 19;
    const auto storage_y = start_y - 30;
    auto storage_x = start_x - STORAGE_BAR_W;

    gfx::drawTextArgs(vg, storage_x, storage_y, storage_font, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "System %.1f GB"_i18n.c_str(), pdata.emmc_free / 1024.0 / 1024.0 / 1024.0);
    // gfx::drawTextArgs(vg, storage_x, storage_y, storage_font, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "eMMC %.1f GB"_i18n.c_str(), pdata.emmc_free / 1024.0 / 1024.0 / 1024.0);
    #if 0
    Vec4 prog_bar{storage_x, storage_y + 24, STORAGE_BAR_W, STORAGE_BAR_H};
    gfx::drawRect(vg, prog_bar, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), rounding);
    gfx::drawRect(vg, prog_bar.x, prog_bar.y, ((double)pdata.emmc_free / (double)pdata.emmc_total) * prog_bar.w, prog_bar.h, theme->GetColour(ThemeEntryID_PROGRESSBAR), rounding);
    #else
    gfx::drawRect(vg, storage_x, storage_y + 24, STORAGE_BAR_W, STORAGE_BAR_H, theme->GetColour(ThemeEntryID_TEXT_INFO), rounding);
    gfx::drawRect(vg, storage_x + 1, storage_y + 24 + 1, STORAGE_BAR_W - 2, STORAGE_BAR_H - 2, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), rounding);
    gfx::drawRect(vg, storage_x + 2, storage_y + 24 + 2, STORAGE_BAR_W - (((double)pdata.emmc_free / (double)pdata.emmc_total) * STORAGE_BAR_W) - 4, STORAGE_BAR_H - 4, theme->GetColour(ThemeEntryID_TEXT_INFO), rounding);
    #endif

    storage_x -= (STORAGE_BAR_W + spacing);
    gfx::drawTextArgs(vg, storage_x, storage_y, storage_font, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "microSD %.1f GB"_i18n.c_str(), pdata.sd_free / 1024.0 / 1024.0 / 1024.0);
    gfx::drawRect(vg, storage_x, storage_y + 24, STORAGE_BAR_W, STORAGE_BAR_H, theme->GetColour(ThemeEntryID_TEXT_INFO), rounding);
    gfx::drawRect(vg, storage_x + 1, storage_y + 24 + 1, STORAGE_BAR_W - 2, STORAGE_BAR_H - 2, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), rounding);
    gfx::drawRect(vg, storage_x + 2, storage_y + 24 + 2, STORAGE_BAR_W - (((double)pdata.sd_free / (double)pdata.sd_total) * STORAGE_BAR_W) - 4, STORAGE_BAR_H - 4, theme->GetColour(ThemeEntryID_TEXT_INFO), rounding);
    start_x -= (STORAGE_BAR_W + spacing) * 2;

    const auto status_right = start_x;
    auto status_content_left = status_right;
    const auto draw_status = [&](ThemeEntryID colour, const std::string& text, float y) {
        gfx::drawText(vg, status_right, y, font_size, theme->GetColour(colour), text.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        gfx::textBounds(vg, 0, 0, bounds, text.c_str());
        status_content_left = std::min(status_content_left, status_right - (bounds[2] - bounds[0]));
    };

    if (!App::IsApplication()) {
        draw_status(ThemeEntryID_ERROR, "[Applet Mode]"_i18n, start_y - 28.f);
    }

    if (App::GetApp()->m_show_ip_addr.Get()) {
        if (pdata.ip) {
            char ip_buf[32];
            std::snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u", pdata.ip & 0xFF, (pdata.ip >> 8) & 0xFF, (pdata.ip >> 16) & 0xFF, (pdata.ip >> 24) & 0xFF);

            char type_buf[32];
            if (pdata.type == NifmInternetConnectionType_WiFi) {
                std::snprintf(type_buf, sizeof(type_buf), "Wi-Fi %.0f%%"_i18n.c_str(), ((float)pdata.strength / 3.F) * 100);
            } else if (pdata.type == NifmInternetConnectionType_Ethernet) {
                std::snprintf(type_buf, sizeof(type_buf), "Ethernet"_i18n.c_str());
            } else {
                std::snprintf(type_buf, sizeof(type_buf), "Unknown"_i18n.c_str());
            }

            draw_status(ThemeEntryID_TEXT, std::string{type_buf} + " | " + ip_buf, start_y);
        } else {
            draw_status(ThemeEntryID_TEXT, "No Internet"_i18n, start_y);
        }
    }

    gfx::drawRect(vg, 30.f, 86.f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawRect(vg, 30.f, 646.0f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));

    constexpr float header_left = 74.f;
    const auto text_w = SCREEN_WIDTH / 2 - 30;
    const auto header_right = std::min<float>(text_w, status_content_left - 15.f);
    const auto header_width = std::max(0.f, header_right - header_left);

    float title_width = header_width;
    float title_sub_x = header_right;
    float title_sub_width = 0.f;
    if (!m_title_sub_heading.empty() && header_width > 0.f) {
        nvgFontSize(vg, 28.f);
        gfx::textBounds(vg, 0, 0, bounds, m_title.c_str());
        const auto measured_title_width = std::max(0.f, bounds[2] - bounds[0]);
        const auto max_title_width = std::max(0.f, header_width * 0.55f);
        title_width = std::min(measured_title_width + 6.f, max_title_width);
        title_sub_x = header_left + title_width + 10.f;
        title_sub_width = std::max(0.f, header_right - title_sub_x);
    }

    m_scroll_title.Draw(vg, m_title != "Homebrew", header_left, start_y, title_width, 28.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, theme->GetColour(ThemeEntryID_TEXT), m_title);
    m_scroll_title_sub_heading.Draw(vg, true, title_sub_x, start_y, title_sub_width, 16.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, theme->GetColour(ThemeEntryID_TEXT_INFO), m_title_sub_heading);
    m_scroll_title_warning.Draw(vg, true, title_sub_x, start_y - 24.f, title_sub_width, 16.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, theme->GetColour(ThemeEntryID_ERROR), m_title_warning);
    m_scroll_sub_heading.Draw(vg, true, 80, 675, text_w - 160, 18, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), m_sub_heading.c_str());
}

} // namespace sphaira::ui::menu

#include "app.hpp"
#include "ui/menus/grid_menu_base.hpp"
#include "ui/nvg_util.hpp"

#include <cmath>

namespace sphaira::ui::menu::grid {

Vec4 Menu::DrawEntry(NVGcontext* vg, Theme* theme, int layout, const Vec4& v, bool selected, int image, const char* name, const char* author, const char* version) {
    return DrawEntry(vg, theme, true, layout, v, selected, image, name, author, version);
}

Vec4 Menu::DrawEntryNoImage(NVGcontext* vg, Theme* theme, int layout, const Vec4& v, bool selected, const char* name, const char* author, const char* version) {
    return DrawEntry(vg, theme, false, layout, v, selected, 0, name, author, version);
}

Vec4 Menu::DrawEntry(NVGcontext* vg, Theme* theme, bool draw_image, int layout, const Vec4& v, bool selected, int image, const char* name, const char* author, const char* version) {
    const auto& [x, y, w, h] = v;

    auto text_id = ThemeEntryID_TEXT;
    auto info_id = ThemeEntryID_TEXT_INFO;
    if (selected) {
        text_id = info_id = ThemeEntryID_TEXT_SELECTED;
        gfx::drawRectOutline(vg, theme, 4.f, v);
    } else {
        DrawElement(v, ThemeEntryID_GRID);
    }

    Vec4 image_v = v;

    if (layout == LayoutType_GridDetail) {
        image_v.x += 20;
        image_v.y += 20;
        image_v.w = 115;
        image_v.h = 115;

        const auto text_off = 148;
        const auto text_x = x + text_off;
        const auto text_clip_w = w - 30.f - text_off;
        const float font_size = 18;
        m_scroll_name.Draw(vg, selected, text_x, y + 45, text_clip_w, font_size, NVG_ALIGN_LEFT, theme->GetColour(text_id), name);
        m_scroll_author.Draw(vg, selected, text_x, y + 80, text_clip_w, font_size, NVG_ALIGN_LEFT, theme->GetColour(info_id), author);
        m_scroll_version.Draw(vg, selected, text_x, y + 115, text_clip_w, font_size, NVG_ALIGN_LEFT, theme->GetColour(info_id), version);
    } else {
        if (selected) {
            gfx::drawAppLable(vg, theme, m_scroll_name, x, y, w, name);
        }
    }

    if (draw_image) {
        if (image > 0) {
            gfx::drawImage(vg, image_v, image, 5);
        } else {
            // https://www.mathopenref.com/arcradius.html
            auto spinner = image_v;
            spinner.w /= 2;
            spinner.h /= 2;
            spinner.x += (image_v.w / 2);
            spinner.y += (image_v.h / 2);

            const auto rad = (spinner.h / 2) + (std::powf(spinner.w, 2) / (spinner.h * 8));
            gfx::drawSpinner(vg, theme, spinner.x, spinner.y, rad, armTicksToNs(armGetSystemTick()) / 1e+9);
        }
    }

    return image_v;
}

void Menu::OnLayoutChange(std::unique_ptr<List>& list, int layout) {
    m_scroll_name.Reset();
    m_scroll_author.Reset();
    m_scroll_version.Reset();

    switch (layout) {
        case LayoutType_List: {
            const Vec2 pad{14, 14};
            const Vec4 v{106, 194, 256, 256};
            list = std::make_unique<List>(1, 4, m_pos, v, pad);
            list->SetLayout(List::Layout::HOME);
        }   break;

        case LayoutType_Grid: {
            const Vec2 pad{10, 10};
            const Vec4 v{93, 186, 174, 174};
            list = std::make_unique<List>(6, 6*2, m_pos, v, pad);
        }   break;

        case LayoutType_GridDetail: {
            const Vec2 pad{10, 10};
            const Vec4 v{75, 110, 370, 155};
            list = std::make_unique<List>(3, 3*3, m_pos, v, pad);
        }   break;
    }
}

} // namespace sphaira::ui::menu::grid

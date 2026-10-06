#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"
#include <algorithm>

namespace sphaira::ui {

PopupList::PopupList(const std::string& title, const Items& items, std::string& index_str_ref, s64& index_ref)
: PopupList{title, items, Callback{}, index_ref}  {

    m_callback = [&index_str_ref, &index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = *op_idx;
            index_str_ref = m_items[index_ref];
        }
    };
}

PopupList::PopupList(const std::string& title, const Items& items, std::string& index_ref)
: PopupList{title, items, Callback{}}  {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index_ref);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
        if (m_index >= 6) {
            m_list->SetYoff((m_index - 5) * m_list->GetMaxY());
        }
    }

    m_callback = [&index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = m_items[*op_idx];
        }
    };
}

PopupList::PopupList(const std::string& title, const Items& items, s64& index_ref)
: PopupList{title, items, Callback{}, index_ref}  {

    m_callback = [&index_ref, this](auto op_idx) {
        if (op_idx) {
            index_ref = *op_idx;
        }
    };
}

PopupList::PopupList(const std::string& title, const Items& items, const Callback& cb, const std::string& index)
: PopupList{title, items, cb, 0} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        SetIndex(std::distance(m_items.cbegin(), it));
        if (m_index >= 6) {
            m_list->SetYoff((m_index - 5) * m_list->GetMaxY());
        }
    }
}

PopupList::PopupList(const std::string& title, const Items& items, const Callback& cb, s64 index)
: m_title{title}
, m_items{items}
, m_callback{cb}
, m_index{index} {
    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            // a greyed-out row isn't a choice: the list stays up.
            if (m_disabled && m_disabled(m_index)) {
                return;
            }
            if (m_callback) {
                m_callback(m_index);
            }
            SetPop();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    m_starting_index = m_index;

    m_pos.w = SCREEN_WIDTH;
    const float a = std::min(370.f, (60.f * static_cast<float>(m_items.size())));
    m_pos.h = 80.f + 140.f + a;
    m_pos.y = SCREEN_HEIGHT - m_pos.h;
    m_line_top = m_pos.y + 70.f;
    m_line_bottom = SCREEN_HEIGHT - 73.f;

    Vec4 v{m_block};
    v.y = m_line_top + 1.f + 42.f;
    const Vec4 pos{0, m_line_top, SCREEN_WIDTH, m_line_bottom - m_line_top};
    m_list = std::make_unique<List>(1, 6, pos, v);
    m_list->SetScrollBarPos(1250, m_line_top + 20, m_line_bottom - m_line_top - 40);

    if (m_index >= 6) {
        m_list->SetYoff((m_index - 5) * m_list->GetMaxY());
    }
}

auto PopupList::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
        SetIndex(i);
        if (touch) {
            FireAction(Button::A);
        }
    });
}

auto PopupList::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::dimBackground(vg);
    gfx::drawRect(vg, m_pos, theme->GetColour(ThemeEntryID_POPUP));
    m_scroll_title.Draw(vg, true, m_pos.x + m_title_pos.x, m_pos.y + m_title_pos.y,
        m_pos.w - m_title_pos.x * 2.f, 24.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT), m_title);
    gfx::drawRect(vg, 30.f, m_line_top, m_line_width, 1.f, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawRect(vg, 30.f, m_line_bottom, m_line_width, 1.f, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawTextArgs(vg, 80, 675, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%zu / %zu", m_index + 1, m_items.size());

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        auto colour = ThemeEntryID_TEXT;
        const auto selected = m_index == i;
        if (selected) {
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_items.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        if (m_starting_index == i) {
            colour = ThemeEntryID_TEXT_SELECTED;
            gfx::drawText(vg, x + w - m_text_xoffset, y + (h / 2.f), 20.f, "\uE14B", NULL, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(colour));
        }

        if (m_disabled && m_disabled(i)) {
            colour = ThemeEntryID_TEXT_INFO;
        }

        const auto text_x = x + m_text_xoffset;
        const auto text_clip_w = w - 60.f - m_text_xoffset;
        m_scroll_text.Draw(vg, selected, text_x, y + (h / 2.f), text_clip_w, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(colour), m_items[i]);
    });

    Widget::Draw(vg, theme);
}

auto PopupList::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto PopupList::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

void PopupList::SetIndex(s64 index) {
    m_index = index;
}

} // namespace sphaira::ui

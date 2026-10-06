#include "ui/popup_multi_select.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"
#include <algorithm>
#include <cstdio>

namespace sphaira::ui {

PopupMultiSelect::PopupMultiSelect(const std::string& title, const Items& items, std::vector<u8>& selected, const Callback& cb)
: m_title{title}
, m_items{items}
, m_selected{selected}
, m_callback{cb} {
    // A ticks rather than settling the list, so B is what closes the popup - and
    // leaves the ticks as they stand, every press having already been acted on.
    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            Toggle(m_index);
        }}),
        std::make_pair(Button::X, Action{"All"_i18n, [this](){
            SetAll(true);
        }}),
        std::make_pair(Button::Y, Action{"None"_i18n, [this](){
            SetAll(false);
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    // the caller sizes this to its own list; a shorter one would be indexed off
    // its end by the rows below it.
    m_selected.resize(m_items.size());
    for (size_t i = 0; i < m_items.size(); i++) {
        if (m_items[i].disabled) {
            m_selected[i] = 0;
        }
    }

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
}

auto PopupMultiSelect::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
        m_index = i;
        if (touch) {
            FireAction(Button::A);
        }
    });
}

auto PopupMultiSelect::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::dimBackground(vg);
    gfx::drawRect(vg, m_pos, theme->GetColour(ThemeEntryID_POPUP));

    // how many are ticked sits beside the title, since that is what the list is
    // for; the row counter below still says where in it the highlight is.
    char count[32];
    std::snprintf(count, sizeof(count), "%ld / %zu", GetSelectedCount(), m_items.size());

    constexpr float count_w = 120.f;
    m_scroll_title.Draw(vg, true, m_pos.x + m_title_pos.x, m_pos.y + m_title_pos.y,
        m_pos.w - m_title_pos.x * 2.f - count_w, 24.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT), m_title);
    gfx::drawTextArgs(vg, m_pos.w - m_title_pos.x, m_pos.y + m_title_pos.y + 4.f, 20.f,
        NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", count);

    gfx::drawRect(vg, 30.f, m_line_top, m_line_width, 1.f, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawRect(vg, 30.f, m_line_bottom, m_line_width, 1.f, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawTextArgs(vg, 80, 675, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%ld / %zu", m_index + 1, m_items.size());

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto focused = m_index == i;
        const auto ticked = IsSelected(i);
        const auto mid = y + (h / 2.f);

        if (focused) {
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != m_items.size() - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        // the tick keeps its place on every row, so a name is cut where the next
        // row's is whether or not this one is ticked.
        auto right = x + w - m_text_xoffset;
        if (ticked) {
            gfx::drawText(vg, right, mid, 20.f, "", nullptr, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
        }
        right -= m_tick_width;

        const auto& item = m_items[i];
        if (!item.note.empty()) {
            constexpr float note_size = 18.f;
            nvgFontSize(vg, note_size);
            const auto note_w = nvgTextBounds(vg, 0, 0, item.note.c_str(), nullptr, nullptr);
            gfx::drawTextArgs(vg, right, mid, note_size, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", item.note.c_str());
            right -= note_w + 20.f;
        }

        // a long name is cut short of whatever the row carries after it, and
        // scrolls while it is the row A would tick.
        const auto text_x = x + m_text_xoffset;
        auto colour = ticked ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT;
        if (item.disabled) {
            colour = ThemeEntryID_TEXT_INFO;
        }
        m_scroll_text.Draw(vg, focused, text_x, mid, std::max(0.f, right - text_x), 20.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(colour), item.name);
    });

    Widget::Draw(vg, theme);
}

auto PopupMultiSelect::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto PopupMultiSelect::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

void PopupMultiSelect::Toggle(s64 index) {
    if (index < 0 || index >= static_cast<s64>(m_selected.size()) || m_items[index].disabled) {
        return;
    }

    m_selected[index] ^= 1;
    OnChanged();
}

void PopupMultiSelect::SetAll(bool selected) {
    for (size_t i = 0; i < m_selected.size(); i++) {
        m_selected[i] = selected && !m_items[i].disabled;
    }

    OnChanged();
}

auto PopupMultiSelect::GetSelectedCount() const -> s64 {
    return std::count(m_selected.cbegin(), m_selected.cend(), 1);
}

auto PopupMultiSelect::IsSelected(s64 index) const -> bool {
    return index >= 0 && index < static_cast<s64>(m_selected.size()) && m_selected[index];
}

void PopupMultiSelect::OnChanged() {
    App::PlaySoundEffect(SoundEffect::Focus);
    if (m_callback) {
        m_callback();
    }
}

} // namespace sphaira::ui

#include "ui/sidebar.hpp"
#include "ui/menus/file_picker.hpp"
#include "app.hpp"
#include "ui/popup_list.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include "swkbd.hpp"
#include <algorithm>

namespace sphaira::ui {
namespace {

auto DistanceBetweenY(const Vec4& va, const Vec4& vb) -> Vec4 {
    return Vec4{
        va.x, va.y,
        va.w, vb.y - va.y
    };
}

} // namespace

SidebarEntryBase::SidebarEntryBase(const std::string& title, const std::string& info)
: m_title{title}, m_info{info} {

}

void SidebarEntryBase::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    // draw spacers or highlight box if in focus (selected)
    if (HasFocus()) {
        gfx::drawRectOutline(vg, theme, 4.f, m_pos);

        const auto& info = IsEnabled() ? m_info : m_depends_info;

        if (!info.empty()) {
            // reset clip here as the box will draw oob.
            nvgSave(vg);
            nvgScissor(vg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
            ON_SCOPE_EXIT(nvgRestore(vg));

            Vec4 info_box{};
            info_box.y = 86;
            info_box.w = 400;

            if (left) {
                info_box.x = root_pos.x + root_pos.w + 10;
            } else {
                info_box.x = root_pos.x - info_box.w - 10;
            }

            const float info_pad = 30;
            const float title_font_size = 18;
            const float info_font_size = 18;
            const float pad_after_title = title_font_size + info_pad;
            const float x = info_box.x + info_pad;
            const auto end_w = info_box.w - info_pad * 2;

            float bounds[4];
            nvgFontSize(vg, info_font_size);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgTextLineHeight(vg, 1.7);
            nvgTextBoxBounds(vg, 0, 0, end_w, info.c_str(), nullptr, bounds);
            info_box.h = pad_after_title + info_pad * 2 + bounds[3] - bounds[1];

            gfx::drawRect(vg, info_box, theme->GetColour(ThemeEntryID_SIDEBAR), 5);

            float y = info_box.y + info_pad;
            m_scolling_info_title.Draw(vg, true, x, y, end_w, title_font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), m_title.c_str());

            y += pad_after_title;
            gfx::drawTextBox(vg, x, y, info_font_size, end_w, theme->GetColour(ThemeEntryID_TEXT), info.c_str());
        }
    }
}

auto SidebarEntryBase::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
}

auto SidebarEntryBase::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    m_scolling_title.Reset();
    m_scolling_info_title.Reset();
    m_scolling_value.Reset();
}

void SidebarEntryBase::DrawEntry(NVGcontext* vg, Theme* theme, const std::string& left, const std::string& right, bool use_selected) {
    const auto colour_id = IsEnabled() ? ThemeEntryID_TEXT : ThemeEntryID_TEXT_INFO;

    constexpr float font_size = 20.f;
    constexpr float padding = 15.f;
    constexpr float column_gap = 20.f;
    const float ypos = m_pos.y + (m_pos.h / 2.f);
    const float label_x = m_pos.x + padding;
    const float content_width = std::max(0.f, m_pos.w - padding * 2.f);

    // Entries without a value use the full row. Long translated labels remain
    // clipped to the sidebar and scroll while focused.
    if (right.empty()) {
        m_scolling_title.Draw(vg, HasFocus(), label_x, ypos, content_width, font_size,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(colour_id), left);
        return;
    }

    float bounds[4];
    nvgFontSize(vg, font_size);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgTextBounds(vg, 0, 0, right.c_str(), nullptr, bounds);
    const float value_text_width = std::max(0.f, bounds[2] - bounds[0]);

    // Reserve a dedicated value column so On/Off and other values can never be
    // covered by a long label. Values are capped at 45% of the available row.
    const float value_width = std::clamp(value_text_width, 48.f, content_width * 0.45f);
    const float value_x = m_pos.x + m_pos.w - padding - value_width;
    const float label_width = std::max(0.f, value_x - column_gap - label_x);

    m_scolling_title.Draw(vg, HasFocus(), label_x, ypos, label_width, font_size,
        NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(colour_id), left);

    const auto value_id = use_selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT;
    const float value_draw_x = value_text_width < value_width
        ? value_x + value_width - value_text_width
        : value_x;
    const float value_clip_width = m_pos.x + m_pos.w - padding - value_draw_x;
    m_scolling_value.Draw(vg, HasFocus(), value_draw_x, ypos, value_clip_width, font_size,
        NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(value_id), right);
}

SidebarEntryBool::SidebarEntryBool(const std::string& title, bool option, const Callback& cb, const std::string& info, const std::string& true_str, const std::string& false_str)
: SidebarEntryBase{title, info}
, m_option{option}
, m_callback{cb}
, m_true_str{true_str}
, m_false_str{false_str} {

    if (m_true_str == "On") {
        m_true_str = i18n::get(m_true_str);
    }
    if (m_false_str == "Off") {
        m_false_str = i18n::get(m_false_str);
    }

    SetAction(Button::A, Action{"OK"_i18n, [this](){
        if (!IsEnabled()) {
            DependsClick();
        } else {
            m_option ^= 1;
            m_callback(m_option);
            SetDirty();
        } }
    });
}

SidebarEntryBool::SidebarEntryBool(const std::string& title, bool& option, const std::string& info, const std::string& true_str, const std::string& false_str)
: SidebarEntryBool{title, option, Callback{}, info, true_str, false_str} {
    m_callback = [this, &option](bool&){
        option ^= 1;
        SetDirty();
    };
}

SidebarEntryBool::SidebarEntryBool(const std::string& title, option::OptionBool& option, const Callback& cb, const std::string& info, const std::string& true_str, const std::string& false_str)
: SidebarEntryBool{title, option.Get(), Callback{}, info, true_str, false_str} {
    m_callback = [this, &option, cb](bool& v_out){
        if (cb) {
            cb(v_out);
        }
        option.Set(v_out);
        SetDirty();
    };
}

SidebarEntryBool::SidebarEntryBool(const std::string& title, option::OptionBool& option, const std::string& info, const std::string& true_str, const std::string& false_str)
: SidebarEntryBool{title, option, Callback{}, info, true_str, false_str} {
}

void SidebarEntryBool::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    SidebarEntryBase::Draw(vg, theme, root_pos, left);
    SidebarEntryBase::DrawEntry(vg, theme, m_title, m_option ? m_true_str : m_false_str, m_option);
}

SidebarEntrySlider::SidebarEntrySlider(const std::string& title, float value, float min, float max, int steps, const Callback& cb, const std::string& info)
: SidebarEntryBase{title, info}
, m_value{value}
, m_min{min}
, m_max{max}
, m_steps{steps}
, m_callback{cb} {
    SetAction(Button::LEFT, Action{[this](){
        if (!IsEnabled()) {
            DependsClick();
        } else {
            m_value = std::clamp(m_value - m_inc, m_min, m_max);
            SetDirty();
            // m_callback(m_option);
        } }
    });
    SetAction(Button::RIGHT, Action{[this](){
        if (!IsEnabled()) {
            DependsClick();
        } else {
            m_value = std::clamp(m_value + m_inc, m_min, m_max);
            SetDirty();
            // m_callback(m_option);
        } }
    });

    m_duration = m_max - m_min;
    m_inc = m_duration / (float)(m_steps);
}

void SidebarEntrySlider::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    SidebarEntryBase::Draw(vg, theme, root_pos, left);

    const float barh = 7;
    const Vec4 bar{m_pos.x + 15.f, m_pos.y + (m_pos.h / 2.f) - barh / 2, m_pos.w - 15.f * 2, barh};

    gfx::drawRect(vg, bar, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), 3);
    auto inner = bar;
    inner.w *= m_value / m_duration;
    gfx::drawRect(vg, inner, theme->GetColour(ThemeEntryID_PROGRESSBAR), 3);

    for (int i = 0; i <= m_steps; i++) {
        const auto loop = m_inc * (float)i;
        const auto marker = Vec4{bar.x + (bar.w * loop / m_duration), bar.y - 4.f, 3.f, bar.h + 8.f};
        gfx::drawRect(vg, marker, theme->GetColour(ThemeEntryID_TEXT_INFO));
    }
}

SidebarEntryCallback::SidebarEntryCallback(const std::string& title, const Callback& cb, bool pop_on_click, const std::string& info)
: SidebarEntryBase{title, info}
, m_callback{cb}
, m_pop_on_click{pop_on_click} {
    SetAction(Button::A, Action{"OK"_i18n, [this](){
        if (!IsEnabled()) {
            DependsClick();
        } else {
            m_callback();
            if (m_pop_on_click) {
                SetPop();
            }
        }}
    });
}

SidebarEntryCallback::SidebarEntryCallback(const std::string& title, const Callback& cb, const std::string& info)
: SidebarEntryCallback{title, cb, false, info} {

}

void SidebarEntryCallback::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    SidebarEntryBase::Draw(vg, theme, root_pos, left);
    SidebarEntryBase::DrawEntry(vg, theme, m_title, {}, false);
}

SidebarEntryArray::SidebarEntryArray(const std::string& title, const Items& items, std::string& index, const std::string& info)
: SidebarEntryArray{title, items, Callback{}, 0, info} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
    }

    m_list_callback = [&index, this]() {
        auto list = std::make_unique<PopupList>(m_title, m_items, index, m_index);
        list->SetDisabled(m_disabled);
        App::Push(std::move(list));

        SetDirty();
    };
}

SidebarEntryArray::SidebarEntryArray(const std::string& title, const Items& items, const Callback& cb, const std::string& index, const std::string& info)
: SidebarEntryArray{title, items, cb, 0, info} {

    const auto it = std::find(m_items.cbegin(), m_items.cend(), index);
    if (it != m_items.cend()) {
        m_index = std::distance(m_items.cbegin(), it);
    }
}

SidebarEntryArray::SidebarEntryArray(const std::string& title, const Items& items, const Callback& cb, s64 index, const std::string& info)
: SidebarEntryBase{title, info}
, m_items{items}
, m_callback{cb}
, m_index{index} {

    m_list_callback = [this]() {
        auto list = std::make_unique<PopupList>(m_title, m_items, [this](auto op_idx){
            if (op_idx) {
                m_index = *op_idx;
                m_callback(m_index);
            }
        }, m_index);
        list->SetDisabled(m_disabled);
        App::Push(std::move(list));
    };

    SetAction(Button::A, Action{"OK"_i18n, [this](){
        if (!IsEnabled()) {
            DependsClick();
        } else {
            // m_callback(m_index);
            m_list_callback();
            SetDirty();
        }}
    });
}

void SidebarEntryArray::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    SidebarEntryBase::Draw(vg, theme, root_pos, left);
    SidebarEntryBase::DrawEntry(vg, theme, m_title, m_items[m_index], true);
}

SidebarEntryTextBase::SidebarEntryTextBase(const std::string& title, const std::string& value, const Callback& cb, const std::string& info)
: SidebarEntryBase{title, info}
, m_value{value}
, m_callback{cb} {
    SetAction(Button::A, Action{"OK"_i18n, [this](){
        if (m_callback) {
            m_callback();
            SetDirty();
        }
    }});
}

void SidebarEntryTextBase::Draw(NVGcontext* vg, Theme* theme, const Vec4& root_pos, bool left) {
    SidebarEntryBase::Draw(vg, theme, root_pos, left);
    SidebarEntryBase::DrawEntry(vg, theme, m_title, m_value, true);
}

SidebarEntryTextInput::SidebarEntryTextInput(const std::string& title, const std::string& value, const std::string& header, const std::string& guide, s64 len_min, s64 len_max, const std::string& info, const Callback& callback)
: SidebarEntryTextBase{title, value, {}, info}
, m_header{header.empty() ? title : header}
, m_guide{guide.empty() ? title : guide}
, m_len_min{len_min}
, m_len_max{len_max}
, m_callback{callback} {

    SetCallback([this](){
        std::string out;
        if (R_SUCCEEDED(swkbd::ShowText(out, m_header.c_str(), m_guide.c_str(), GetValue().c_str(), m_len_min, m_len_max))) {
            SetValue(out);

            if (m_callback) {
                m_callback(this);
            }
        }
    });
}

SidebarEntryTextInput::SidebarEntryTextInput(const std::string& title, s64 value, const std::string& header, const std::string& guide, s64 len_min, s64 len_max, const std::string& info, const Callback& callback)
: SidebarEntryTextInput{title, std::to_string(value), header, guide, len_min, len_max, info, callback} {
    SetCallback([this](){
        s64 out = std::stoul(GetValue());
        if (R_SUCCEEDED(swkbd::ShowNumPad(out, m_header.c_str(), m_guide.c_str(), GetValue().c_str(), m_len_min, m_len_max))) {
            SetValue(std::to_string(out));

            if (m_callback) {
                m_callback(this);
            }
        }
    });
}

SidebarEntryFilePicker::SidebarEntryFilePicker(const std::string& title, const std::string& value, const std::vector<std::string>& filter, const std::string& info)
: SidebarEntryTextBase{title, value, {}, info}, m_filter{filter} {

    SetCallback([this](){
        App::Push<menu::filebrowser::picker::Menu>(
            [this](const fs::FsPath& path) {
                SetValue(path);
                SetDirty();
                return true;
            },
            m_filter
        );
    });
}

Sidebar::Sidebar(const std::string& title, Side side, float width)
: Sidebar{title, "", side, width} {
}

Sidebar::Sidebar(const std::string& title, const std::string& sub, Side side, float width)
: m_title{title}
, m_sub{sub}
, m_side{side} {
    switch (m_side) {
        case Side::LEFT:
            SetPos(Vec4{0.f, 0.f, width, SCREEN_HEIGHT});
            break;

        case Side::RIGHT:
            SetPos(Vec4{SCREEN_WIDTH - width, 0.f, width, SCREEN_HEIGHT});
            break;
    }

    // setup top and bottom bar
    m_top_bar = Vec4{m_pos.x + 15.f, 86.f, m_pos.w - 30.f, 1.f};
    m_bottom_bar = Vec4{m_pos.x + 15.f, 646.f, m_pos.w - 30.f, 1.f};
    m_title_pos = Vec2{m_pos.x + 30.f, m_pos.y + 40.f};
    m_base_pos = Vec4{GetX() + 30.f, GetY() + 170.f, m_pos.w - (30.f * 2.f), 70.f};

    // set button positions
    SetUiButtonPos({m_pos.x + m_pos.w - 60.f, 675});

    const Vec4 pos = DistanceBetweenY(m_top_bar, m_bottom_bar);
    m_list = std::make_unique<List>(1, 6, pos, m_base_pos);
    m_list->SetScrollBarPos(GetX() + GetW() - 20, m_base_pos.y - 10, pos.h - m_base_pos.y + 48);
}

Sidebar::~Sidebar() {
    if (m_on_exit_when_changed) {
        for (const auto& item : m_items) {
            if (item->IsDirty()) {
                m_on_exit_when_changed();
                break;
            }
        }
    }
}

auto Sidebar::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);

    // pop if we have no more entries.
    if (m_items.empty()) {
        App::Notify("Closing empty sidebar"_i18n);
        SetPop();
        return;
    }

    // if touched out of bounds, pop the sidebar and all widgets below it.
    if (touch->is_clicked && !touch->in_range(GetPos())) {
        App::PopToMenu();
    } else {
        m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
            SetIndex(i);
            if (touch) {
                FireAction(Button::A);
            }
        });
    }

    if (m_items[m_index]->ShouldPop()) {
        SetPop();
    }
}

auto Sidebar::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::drawRect(vg, m_pos, theme->GetColour(ThemeEntryID_SIDEBAR));
    constexpr float header_padding = 30.f;
    constexpr float header_gap = 16.f;
    const auto header_right = m_pos.x + m_pos.w - header_padding;
    auto title_width = std::max(0.f, header_right - m_title_pos.x);
    if (!m_sub.empty()) {
        float bounds[4]{};
        nvgFontSize(vg, 16.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgTextBounds(vg, 0.f, 0.f, m_sub.c_str(), nullptr, bounds);
        const auto available_width = std::max(0.f, header_right - m_title_pos.x);
        const auto sub_width = std::min(std::max(0.f, bounds[2] - bounds[0]), available_width * 0.4f);
        const auto sub_x = header_right - sub_width;
        title_width = std::max(0.f, sub_x - header_gap - m_title_pos.x);
        m_scroll_sub.Draw(vg, true, sub_x, m_title_pos.y + 10.f, sub_width, 16.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), m_sub);
    }
    m_scroll_title.Draw(vg, true, m_title_pos.x, m_title_pos.y, title_width, m_title_size,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), m_title);
    gfx::drawRect(vg, m_top_bar, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawRect(vg, m_bottom_bar, theme->GetColour(ThemeEntryID_LINE));
    gfx::drawTextArgs(vg, m_pos.x + 30, 675, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%zu / %zu", m_index + 1, m_items.size());

    Widget::Draw(vg, theme);

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;

        if (i != m_items.size() - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        m_items[i]->SetY(y);
        m_items[i]->Draw(vg, theme, m_pos, m_side == Side::LEFT);
    });
}

auto Sidebar::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto Sidebar::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

auto Sidebar::Add(std::unique_ptr<SidebarEntryBase>&& _entry) -> SidebarEntryBase* {
    auto& entry = m_items.emplace_back(std::forward<decltype(_entry)>(_entry));
    entry->SetPos(m_base_pos);

    // give focus to first entry.
    if (m_items.size() == 1) {
        entry->OnFocusGained();
        SetupButtons();
    }

    return entry.get();
}

void Sidebar::SetDefaultEntry(const SidebarEntryBase* entry) {
    for (s64 i = 0; i < static_cast<s64>(m_items.size()); i++) {
        if (m_items[i].get() == entry) {
            SetIndex(i);
            break;
        }
    }
}

void Sidebar::SetIndex(s64 index) {
    // if we moved
    if (m_index != index) {
        m_items[m_index]->OnFocusLost();
        m_index = index;
        m_items[m_index]->OnFocusGained();
        SetupButtons();
    }
}

void Sidebar::SetupButtons() {
    RemoveActions();

    // add entry actions
    for (const auto& [button, action] : m_items[m_index]->GetActions()) {
        SetAction(button, action);
    }

    // add default actions, overriding if needed.
    this->SetActions(
        // each item has it's own Action, but we take over B
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    // disable jump page if the item is using left/right buttons.
    if (HasAction(Button::LEFT) || HasAction(Button::RIGHT)) {
        m_list->SetPageJump(false);
    } else {
        m_list->SetPageJump(true);
    }
}

} // namespace sphaira::ui

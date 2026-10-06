#include "ui/widget.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "log.hpp"

namespace sphaira::ui {

uiButton::uiButton(Button button, const std::string& button_str, const std::string& action_str)
: m_button{button}
, m_button_str{button_str}
, m_action_str{action_str} {

}

uiButton::uiButton(Button button, const std::string& action_str)
: uiButton{button, gfx::getButton(button), action_str} {

}

auto uiButton::Draw(NVGcontext* vg, Theme* theme) -> void {
    // enable to see button region
    // gfx::drawRect(vg, m_pos, gfx::Colour::RED);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, theme->GetColour(ThemeEntryID_TEXT));
    nvgFontSize(vg, 20);
    nvgText(vg, m_hint_pos.x, m_hint_pos.y, m_action_str.c_str(), nullptr);
    nvgFontSize(vg, 26);
    nvgText(vg, m_button_pos.x, m_button_pos.y, m_button_str.c_str(), nullptr);
}

void Widget::Update(Controller* controller, TouchInfo* touch) {
    for (const auto& [button, action] : m_actions) {
        if ((action.m_type & ActionType::DOWN) && controller->GotDown(button)) {
            if (static_cast<u64>(button) & static_cast<u64>(Button::ANY_BUTTON)) {
                App::PlaySoundEffect(SoundEffect::Focus);
            }
            action.Invoke(true);
            break;
        }
        else if ((action.m_type & ActionType::UP) && controller->GotUp(button)) {
            action.Invoke(false);
            break;
        }
        else if ((action.m_type & ActionType::HELD) && controller->GotHeld(button)) {
            action.Invoke(true);
            break;
        }
    }

    auto draw_actions = GetUiButtons();
    for (auto& e : draw_actions) {
        if (touch->is_clicked && touch->in_range(e.GetPos())) {
            log_write("got click: %s\n", e.m_action_str.c_str());
            FireAction(e.m_button);
            break;
        }
    }
}

void Widget::Draw(NVGcontext* vg, Theme* theme) {
    auto draw_actions = GetUiButtons();

    for (auto& e : draw_actions) {
        e.Draw(vg, theme);
    }
}

auto Widget::HasAction(Button button) const -> bool {
    return m_actions.contains(button);
}

void Widget::SetAction(Button button, const Action& action) {
    m_actions.insert_or_assign(button, action);
}

void Widget::RemoveAction(Button button) {
    if (auto it = m_actions.find(button); it != m_actions.end()) {
        m_actions.erase(it);
    }
}

auto Widget::FireAction(Button b, u8 type) -> bool {
    for (const auto& [button, action] : m_actions) {
        if (button == b && (action.m_type & type)) {
            App::PlaySoundEffect(SoundEffect::Focus);
            action.Invoke(true);
            return true;
        }
    }
    return false;
}

void Widget::SetupUiButtons(uiButtons& buttons, const Vec2& button_pos) {
    auto vg = App::GetVg();
    auto [x, y] = button_pos;

    float bounds[4]{};
    for (auto& e : buttons) {
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);

        nvgFontSize(vg, 20.f);
        nvgTextBounds(vg, x, y, e.m_action_str.c_str(), nullptr, bounds);
        auto len = bounds[2] - bounds[0];
        e.m_hint_pos = {x, y, len, 20};

        x -= len + 8.f;
        nvgFontSize(vg, 26.f);
        nvgTextBounds(vg, x, y - 7.f, e.m_button_str.c_str(), nullptr, bounds);
        len = bounds[2] - bounds[0];
        e.m_button_pos = {x, y - 4.f, len, 26};
        x -= len + 34.f;

        e.SetPos(e.m_button_pos);
        e.SetX(e.GetX() - 40);
        e.SetW(e.m_hint_pos.x - e.m_button_pos.x + len + 25);
        e.SetY(e.GetY() - 18);
        e.SetH(26 + 18 * 2);
    }
}

auto Widget::GetUiButtons(const Actions& actions, const Vec2& button_pos) -> uiButtons {
    uiButtons draw_actions;
    draw_actions.reserve(actions.size());

    const std::pair<Button, Button> swap_buttons[] = {
        {Button::L, Button::R},
        {Button::L2, Button::R2},
    };

    // build array
    for (const auto& [button, action] : actions) {
        if (action.IsHidden() || action.m_hint.empty()) {
            continue;
        }

        uiButton ui_button{button, action.m_hint};

        // a ZL with no hint of its own beside a ZR that has one steps the same
        // thing the other way, so both glyphs are drawn in front of that hint.
        if (button == Button::R2) {
            if (const auto it = actions.find(Button::L2); it != actions.end() && it->second.m_hint.empty()) {
                ui_button.m_button_str = std::string{gfx::getButton(Button::L2)} + " " + gfx::getButton(Button::R2);
            }
        }

        bool should_swap = false;
        for (auto [left, right] : swap_buttons) {
            if (button == right && draw_actions.size() && draw_actions.back().m_button == left) {
                const auto s = draw_actions.back();
                draw_actions.back().m_button = button;
                draw_actions.back().m_button_str = gfx::getButton(button);
                draw_actions.back().m_action_str = action.m_hint;
                draw_actions.emplace_back(s);
                should_swap = true;
                break;
            }
        }

        if (!should_swap) {
            draw_actions.emplace_back(ui_button);
        }
    }

    // setup positions.
    SetupUiButtons(draw_actions, button_pos);

    return draw_actions;
}

auto Widget::GetUiButtons() const -> uiButtons {
    return GetUiButtons(m_actions, m_button_pos);
}

} // namespace sphaira::ui

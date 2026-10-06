#pragma once

#include "ui/widget.hpp"
#include "ui/scrolling_text.hpp"
#include "ui/list.hpp"
#include <optional>

namespace sphaira::ui {

class PopupList final : public Widget {
public:
    using Items = std::vector<std::string>;
    using Callback = std::function<void(std::optional<s64>)>;
    using DisabledCallback = std::function<bool(s64 index)>;

public:
    explicit PopupList(const std::string& title, const Items& items, const Callback& cb, s64 index = 0);
    PopupList(const std::string& title, const Items& items, const Callback& cb, const std::string& index);
    PopupList(const std::string& title, const Items& items, std::string& index_str_ref, s64& index);
    PopupList(const std::string& title, const Items& items, std::string& index_ref);
    PopupList(const std::string& title, const Items& items, s64& index_ref);

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

    // greys out the rows this returns true for, which A then leaves alone. asked
    // as the list is drawn, so it follows whatever it reads at the time.
    void SetDisabled(const DisabledCallback& disabled) {
        m_disabled = disabled;
    }

private:
    void SetIndex(s64 index);

private:
    static constexpr Vec2 m_title_pos{70.f, 28.f};
    static constexpr Vec4 m_block{280.f, 110.f, SCREEN_HEIGHT, 60.f};
    static constexpr float m_text_xoffset{15.f};
    static constexpr float m_line_width{1220.f};

    const std::string m_title;
    const Items m_items;
    Callback m_callback{};
    DisabledCallback m_disabled{};
    s64 m_index{}; // index in list array
    s64 m_starting_index{};

    std::unique_ptr<List> m_list{};
    ScrollingText m_scroll_title{};
    ScrollingText m_scroll_text{};

    float m_yoff{};
    float m_line_top{};
    float m_line_bottom{};
};

} // namespace sphaira::ui

#pragma once

#include "ui/widget.hpp"
#include "ui/scrolling_text.hpp"
#include "ui/list.hpp"
#include <string>
#include <vector>

namespace sphaira::ui {

// a PopupList whose rows are each a choice of their own: A ticks the highlighted
// row, X and Y take all and none, and B leaves. its rows are wider than a
// PopupList's, because what is picked here is named at length.
class PopupMultiSelect final : public Widget {
public:
    // a note is drawn after the name in the info colour. a disabled row is greyed
    // out and never ticked, by A, by X or by the caller's own vector.
    struct Item {
        std::string name{};
        std::string note{};
        bool disabled{};
    };

    using Items = std::vector<Item>;
    // run on every change, for a caller that draws its own count of what is ticked.
    using Callback = std::function<void()>;

public:
    // `selected` is one flag per item, ticked in place. it belongs to the caller
    // and has to outlive the popup, the way a PopupList's index reference does.
    PopupMultiSelect(const std::string& title, const Items& items, std::vector<u8>& selected, const Callback& cb = {});

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

private:
    void Toggle(s64 index);
    void SetAll(bool selected);
    auto GetSelectedCount() const -> s64;
    auto IsSelected(s64 index) const -> bool;
    void OnChanged();

private:
    static constexpr Vec2 m_title_pos{70.f, 28.f};
    // centred like a PopupList's rows and half as wide again, which is what a
    // name with a tag after it needs.
    static constexpr Vec4 m_block{140.f, 110.f, 1000.f, 60.f};
    static constexpr float m_text_xoffset{15.f};
    static constexpr float m_line_width{1220.f};
    // the room the tick keeps at the end of a row, held whether or not the row
    // has one, so a name is cut at the same place either way.
    static constexpr float m_tick_width{40.f};

    const std::string m_title;
    const Items m_items;
    std::vector<u8>& m_selected;
    const Callback m_callback;
    s64 m_index{}; // the highlighted row, which A ticks.

    std::unique_ptr<List> m_list{};
    ScrollingText m_scroll_title{};
    ScrollingText m_scroll_text{};

    float m_line_top{};
    float m_line_bottom{};
};

} // namespace sphaira::ui

#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/scrolling_text.hpp"
#include "ui/list.hpp"
#include <string>
#include <memory>

namespace sphaira::ui::menu::grid {

enum LayoutType {
    LayoutType_List,
    LayoutType_Grid,
    LayoutType_GridDetail,
};

struct Menu : MenuBase {
    using MenuBase::MenuBase;

protected:
    void OnLayoutChange(std::unique_ptr<List>& list, int layout);
    Vec4 DrawEntry(NVGcontext* vg, Theme* theme, int layout, const Vec4& v, bool selected, int image, const char* name, const char* author, const char* version);
    // same as above but doesn't draw image and returns image dimension.
    Vec4 DrawEntryNoImage(NVGcontext* vg, Theme* theme, int layout, const Vec4& v, bool selected, const char* name, const char* author, const char* version);

private:
    Vec4 DrawEntry(NVGcontext* vg, Theme* theme, bool draw_image, int layout, const Vec4& v, bool selected, int image, const char* name, const char* author, const char* version);

private:
    ScrollingText m_scroll_name{};
    ScrollingText m_scroll_author{};
    ScrollingText m_scroll_version{};
};

} // namespace sphaira::ui::menu::grid

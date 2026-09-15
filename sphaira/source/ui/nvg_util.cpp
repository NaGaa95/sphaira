#include "ui/nvg_util.hpp"
#include "log.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <array>
#include <utility>
#include <algorithm>
#include <cmath>

namespace sphaira::ui::gfx {
namespace {

constexpr auto ALIGN_HOR = NVG_ALIGN_LEFT|NVG_ALIGN_CENTER|NVG_ALIGN_RIGHT;
constexpr auto ALIGN_VER = NVG_ALIGN_TOP|NVG_ALIGN_MIDDLE|NVG_ALIGN_BOTTOM|NVG_ALIGN_BASELINE;

constexpr std::array buttons = {
    std::pair{Button::A, "\uE0E0"},
    std::pair{Button::B, "\uE0E1"},
    std::pair{Button::X, "\uE0E2"},
    std::pair{Button::Y, "\uE0E3"},
    std::pair{Button::L, "\uE0E4"},
    std::pair{Button::R, "\uE0E5"},
    std::pair{Button::L2, "\uE0E6"},
    std::pair{Button::R2, "\uE0E7"},
    std::pair{Button::UP, "\uE0EB"},
    std::pair{Button::DOWN, "\uE0EC"},
    std::pair{Button::LEFT, "\uE0ED"},
    std::pair{Button::RIGHT, "\uE0EE"},
    std::pair{Button::START, "\uE0EF"},
    std::pair{Button::SELECT, "\uE0F0"},
    // std::pair{Button::LS, "\uE101"},
    // std::pair{Button::RS, "\uE102"},
    std::pair{Button::L3, "\uE104"},
    std::pair{Button::R3, "\uE105"},
};

// software based clipping, saves a few cpu cycles.
bool ClipRect(float x, float y) {
    return x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT;
}

bool ClipText(float x, float y, int align) {
    if ((!(align & ALIGN_HOR) || (align & NVG_ALIGN_LEFT)) && x >= SCREEN_WIDTH) {
        return true;
    }

    if ((!(align & ALIGN_VER) || (align & NVG_ALIGN_TOP)) && y >= SCREEN_HEIGHT) {
        return true;
    }

    return false;
}

// NEW ---------------------
void drawRectIntenal(NVGcontext* vg, const Vec4& v, const NVGcolor& c, float rounded) {
    if (ClipRect(v.x, v.y)) {
        return;
    }

    nvgBeginPath(vg);
    nvgRoundedRect(vg, v.x, v.y, v.w, v.h, rounded);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

void drawRectIntenal(NVGcontext* vg, const Vec4& v, const NVGpaint& p, float rounded) {
    if (ClipRect(v.x, v.y)) {
        return;
    }

    nvgBeginPath(vg);
    nvgRoundedRect(vg, v.x, v.y, v.w, v.h, rounded);
    nvgFillPaint(vg, p);
    nvgFill(vg);
}

void drawRectOutlineInternal(NVGcontext* vg, const Theme* theme, float size, const Vec4& v) {
    float gradientX, gradientY, color;
    getHighlightAnimation(&gradientX, &gradientY, &color);

    const auto strokeWidth = size;
    // const auto strokeWidth = 5.F;
    auto v2 = v;
    v2.x -= strokeWidth / 2.F;
    v2.y -= strokeWidth / 2.F;
    v2.w += strokeWidth;
    v2.h += strokeWidth;
    const auto corner_radius = 0.5F;

    const auto shadow_width = 2.F;
    const auto shadow_offset = 10.F;
    const auto shadow_feather = 10.F;
    const auto shadow_opacity = 128.F;

    // Shadow
    NVGpaint shadowPaint = nvgBoxGradient(vg,
        v2.x, v2.y + shadow_width,
        v2.w, v2.h,
        corner_radius * 2, shadow_feather,
        nvgRGBA(0, 0, 0, shadow_opacity * 1.f), nvgRGBA(0, 0, 0, 0));

    nvgBeginPath(vg);
    nvgRect(vg, v2.x - shadow_offset, v2.y - shadow_offset,
        v2.w + shadow_offset * 2, v2.h + shadow_offset * 3);
    nvgRoundedRect(vg, v2.x, v2.y, v2.w, v2.h, corner_radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadowPaint);
    nvgFill(vg);

    const auto color1 = theme->GetColour(ThemeEntryID_HIGHLIGHT_1);
    const auto color2 = theme->GetColour(ThemeEntryID_HIGHLIGHT_2);
    const auto borderColor = nvgRGBAf(color2.r, color2.g, color2.b, 0.5);
    const auto transparent = nvgRGBA(0, 0, 0, 0);

    const auto pulsationColor = nvgRGBAf((color * color1.r) + (1 - color) * color2.r,
        (color * color1.g) + (1 - color) * color2.g,
        (color * color1.b) + (1 - color) * color2.b,
        1.f);

    const auto border1Paint = nvgRadialGradient(vg,
        v2.x + gradientX * v2.w, v2.y + gradientY * v2.h,
        strokeWidth * 10, strokeWidth * 40,
        borderColor, transparent);

    const auto border2Paint = nvgRadialGradient(vg,
        v2.x + (1 - gradientX) * v2.w, v2.y + (1 - gradientY) * v2.h,
        strokeWidth * 10, strokeWidth * 40,
        borderColor, transparent);

    nvgBeginPath(vg);
    nvgStrokeColor(vg, pulsationColor);
    nvgStrokeWidth(vg, strokeWidth);
    nvgRoundedRect(vg, v2.x, v2.y, v2.w, v2.h, corner_radius);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgStrokePaint(vg, border1Paint);
    nvgStrokeWidth(vg, strokeWidth);
    nvgRoundedRect(vg, v2.x, v2.y, v2.w, v2.h, corner_radius);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgStrokePaint(vg, border2Paint);
    nvgStrokeWidth(vg, strokeWidth);
    nvgRoundedRect(vg, v2.x, v2.y, v2.w, v2.h, corner_radius);
    nvgStroke(vg);
}

void drawRectOutlineInternal(NVGcontext* vg, const Theme* theme, float size, const Vec4& v, const NVGcolor& c) {
    if (ClipRect(v.x, v.y)) {
        return;
    }

    const auto corner_radius = 0.5;
    drawRectOutlineInternal(vg, theme, size, v);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, v.x, v.y, v.w, v.h, corner_radius);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

void drawTextIntenal(NVGcontext* vg, const Vec2& v, float size, const char* str, const char* end, int align, const NVGcolor& c) {
    if (ClipText(v.x, v.y, align)) {
        return;
    }

    nvgBeginPath(vg);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, c);
    nvgText(vg, v.x, v.y, str, end);
}

void drawTriangleInternal(NVGcontext* vg, float aX, float aY, float bX, float bY, float cX, float cY, const NVGcolor& c) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, aX, aY);
    nvgLineTo(vg, bX, bY);
    nvgLineTo(vg, cX, cY);
    nvgFillColor(vg, c);
    nvgFill(vg);
}

void drawTriangleInternal(NVGcontext* vg, float aX, float aY, float bX, float bY, float cX, float cY, const NVGpaint& p) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, aX, aY);
    nvgLineTo(vg, bX, bY);
    nvgLineTo(vg, cX, cY);
    nvgFillPaint(vg, p);
    nvgFill(vg);
}

} // namespace

const char* getButton(const Button want) {
    for (auto& [key, val] : buttons) {
        if (key == want) {
            return val;
        }
    }
    std::unreachable();
}

void drawTextArgs(NVGcontext* vg, float x, float y, float size, int align, const NVGcolor& c, const char* str, ...) {
    std::va_list v;
    va_start(v, str);
    char buffer[0x100];
    std::vsnprintf(buffer, sizeof(buffer), str, v);
    va_end(v);
    drawText(vg, x, y, size, buffer, nullptr, align, c);
}

void drawImage(NVGcontext* vg, const Vec4& v, int texture, float rounded, float alpha) {
    const auto paint = nvgImagePattern(vg, v.x, v.y, v.w, v.h, 0, texture, alpha);
    drawRect(vg, v, paint, rounded);
}

void drawImage(NVGcontext* vg, float x, float y, float w, float h, int texture, float rounded, float alpha) {
    drawImage(vg, Vec4(x, y, w, h), texture, rounded, alpha);
}

void drawTextBox(NVGcontext* vg, float x, float y, float size, float bound, const NVGcolor& c, const char* str, int align, const char* end) {
    if (ClipText(x, y, align)) {
        return;
    }

    nvgBeginPath(vg);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, c);
    nvgTextBox(vg, x, y, bound, str, end);
}

void textBounds(NVGcontext* vg, float x, float y, float *bounds, const char* str) {
    nvgTextBounds(vg, x, y, str, nullptr, bounds);
}

void textBoundsArgs(NVGcontext* vg, float x, float y, float *bounds, const char* str, ...) {
    char buf[0x100];
    va_list v;
    va_start(v, str);
    std::vsnprintf(buf, sizeof(buf), str, v);
    va_end(v);
    textBounds(vg, x, y, bounds, buf);
}

// NEW-----------

void dimBackground(NVGcontext* vg) {
    drawRectIntenal(vg, {0.f,0.f,SCREEN_WIDTH,SCREEN_HEIGHT}, nvgRGBA(0, 0, 0, 180), false);
}

void drawRect(NVGcontext* vg, float x, float y, float w, float h, const NVGcolor& c, float rounded) {
    drawRectIntenal(vg, {x,y,w,h}, c, rounded);
}

void drawRect(NVGcontext* vg, const Vec4& v, const NVGcolor& c, float rounded) {
    drawRectIntenal(vg, v, c, rounded);
}

void drawRect(NVGcontext* vg, float x, float y, float w, float h, const NVGpaint& p, float rounded) {
    drawRectIntenal(vg, {x,y,w,h}, p, rounded);
}

void drawRect(NVGcontext* vg, const Vec4& v, const NVGpaint& p, float rounded) {
    drawRectIntenal(vg, v, p, rounded);
}

void drawRectOutline(NVGcontext* vg, const Theme* theme, float size, float x, float y, float w, float h) {
    drawRectOutlineInternal(vg, theme, size, {x,y,w,h}, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
}

void drawRectOutline(NVGcontext* vg, const Theme* theme, float size, const Vec4& v) {
    drawRectOutlineInternal(vg, theme, size, v, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
}

void drawText(NVGcontext* vg, float x, float y, float size, const char* str, const char* end, int align, const NVGcolor& c) {
    drawTextIntenal(vg, {x,y}, size, str, end, align, c);
}

void drawText(NVGcontext* vg, float x, float y, float size, const NVGcolor& c, const char* str, int align, const char* end) {
    drawTextIntenal(vg, {x,y}, size, str, end, align, c);
}

void drawText(NVGcontext* vg, const Vec2& v, float size, const char* str, const char* end, int align, const NVGcolor& c) {
    drawTextIntenal(vg, v, size, str, end, align, c);
}

void drawText(NVGcontext* vg, const Vec2& v, float size, const NVGcolor& c, const char* str, int align, const char* end) {
    drawTextIntenal(vg, v, size, str, end, align, c);
}

void drawScrollbar(NVGcontext* vg, const Theme* theme, float x, float y, float h, u32 index_off, u32 count, u32 max_per_page) {
    const s64 SCROLL = index_off;
    const s64 max_entry_display = max_per_page;
    const s64 entry_total = count;
    const float scc2 = 8.0;
    const float scw = 2.0;

    // only draw scrollbar if needed
    if (entry_total > max_entry_display) {
        const float sb_h = 1.f / (float)entry_total * h;
        const float sb_y = SCROLL;
        gfx::drawRect(vg, x, y, scc2, h, theme->GetColour(ThemeEntryID_SCROLLBAR_BACKGROUND), 5);
        gfx::drawRect(vg, x + scw, y + scw + sb_h * sb_y, scc2 - scw * 2, sb_h * float(max_entry_display) - scw * 2, theme->GetColour(ThemeEntryID_SCROLLBAR), 5);
    }
}

void drawScrollbar(NVGcontext* vg, const Theme* theme, u32 index_off, u32 count, u32 max_per_page) {
    drawScrollbar(vg, theme, SCREEN_WIDTH - 50, 100, SCREEN_HEIGHT-200, index_off, count, max_per_page);
}

void drawScrollbar2(NVGcontext* vg, const Theme* theme, float x, float y, float h, s64 index_off, s64 count, s64 row, s64 page) {
    // round up
    if (count % row) {
        count = count + (row - count % row);
    }

    const float scc2 = 6.0;
    const float scw = 2.0;

    // only draw scrollbar if needed
    if (count > page) {
        const float sb_h = 1.f / (float)count * h;
        const float sb_y = index_off;
        gfx::drawRect(vg, x, y, scc2, h, theme->GetColour(ThemeEntryID_SCROLLBAR_BACKGROUND), 5);
        gfx::drawRect(vg, x + scw, y + scw + sb_h * sb_y, scc2 - scw * 2, sb_h * float(page) - scw * 2, theme->GetColour(ThemeEntryID_SCROLLBAR), 5);
    }
}

void drawScrollbar2(NVGcontext* vg, const Theme* theme, s64 index_off, s64 count, s64 row, s64 page) {
    drawScrollbar2(vg, theme, SCREEN_WIDTH - 50, 100, SCREEN_HEIGHT-200, index_off, count, row, page);
}

void drawTriangle(NVGcontext* vg, float aX, float aY, float bX, float bY, float cX, float cY, const NVGcolor& c) {
    drawTriangleInternal(vg, aX, aY, bX, bY, cX, cY, c);
}

void drawTriangle(NVGcontext* vg, float aX, float aY, float bX, float bY, float cX, float cY, const NVGpaint& p) {
    drawTriangleInternal(vg, aX, aY, bX, bY, cX, cY, p);
}

void drawAppLable(NVGcontext* vg, const Theme* theme, ScrollingText& st, float x, float y, float w, const char* name) {
    // todo: no more 5am code
    const float max_box_w = 392.f;
    const float box_h = 48.f;
    // used for adjusting the position of the box.
    const float clip_pad = 25.f;
    const float clip_left = clip_pad;
    const float clip_right = 1220.f - clip_pad;
    const float text_pad = 25.f;
    const float font_size = 22.f;

    nvgTextAlign(vg, NVG_ALIGN_LEFT);
    nvgFontSize(vg, font_size);
    float bounds[4]{};
    nvgTextBounds(vg, 0, 0, name, NULL, bounds);

    const float trinaglex = x + (w / 2.f) - 9.f;
    const float trinagley = y - 14.f;
    const float center_x = x + (w / 2.f);
    const float y_offset = y - 62.f; // top of box
    const float text_width = bounds[2];
    float box_w = text_width + text_pad * 2;
    if (box_w > max_box_w) {
        box_w = max_box_w;
    }

    float box_x = center_x - (box_w / 2.f);
    if (box_x < clip_left) {
        box_x = clip_left;
    }
    if ((box_x + box_w) > clip_right) {
        // box_x -= ((box_x + box_w) - clip_right) / 2;
        box_x = (clip_right - box_w);
    }

    const float text_x = box_x + text_pad;
    const float text_y = y_offset + (box_h / 2.f);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, y_offset, box_w, box_h, 3.f);
    nvgFillColor(vg, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
    nvgFill(vg);

    drawTriangle(vg, trinaglex, trinagley, trinaglex + 18.f, trinagley, trinaglex + 9.f, trinagley + 12.f, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
    st.Draw(vg, true, text_x, text_y, box_w - text_pad * 2, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED), name);
}

// https://github.com/memononen/nanovg/blob/f93799c078fa11ed61c078c65a53914c8782c00b/example/demo.c#L500
void drawSpinner(NVGcontext* vg, const Theme* theme, float cx, float cy, float r, float t)
{
    float a0 = 0.0f + t*6;
    float a1 = NVG_PI + t*6;
    float r0 = r;
    float r1 = r * 0.75f;
    float ax,ay, bx,by;
    NVGpaint paint;

    nvgSave(vg);

    auto colourb = theme->GetColour(ThemeEntryID_PROGRESSBAR);
    colourb.a = 0.5;

    nvgBeginPath(vg);
    nvgArc(vg, cx,cy, r0, a0, a1, NVG_CW);
    nvgArc(vg, cx,cy, r1, a1, a0, NVG_CCW);
    nvgClosePath(vg);
    ax = cx + cosf(a0) * (r0+r1)*0.5f;
    ay = cy + sinf(a0) * (r0+r1)*0.5f;
    bx = cx + cosf(a1) * (r0+r1)*0.5f;
    by = cy + sinf(a1) * (r0+r1)*0.5f;
    paint = nvgLinearGradient(vg, ax,ay, bx,by, nvgRGBA(0,0,0,0), colourb);
    nvgFillPaint(vg, paint);
    nvgFill(vg);

    nvgRestore(vg);
}

auto drawQrCode(NVGcontext* vg, float x, float y, float max_size, const qr::Matrix& matrix) -> float {
    if (matrix.IsEmpty() || ClipRect(x, y)) {
        return 0.F;
    }

    // the quiet zone is part of the code, without it scanners struggle.
    constexpr int QUIET = 4;
    const auto count = matrix.GetSize();
    const auto total = count + QUIET * 2;

    // whole pixels per module, otherwise the edges land between pixels and
    // the whole thing turns into a blurry mess.
    const auto scale = std::max(1.F, std::floor(max_size / total));
    const auto size = scale * total;

    // deliberately not themed, a QR code has to be dark on light to scan.
    drawRect(vg, x, y, size, size, nvgRGB(0xFF, 0xFF, 0xFF), 4.F);

    // one path for the lot, merging each row into as few rects as possible.
    nvgBeginPath(vg);
    for (int my = 0; my < count; my++) {
        int run = 0;
        for (int mx = 0; mx <= count; mx++) {
            if (mx < count && matrix.IsDark(mx, my)) {
                run++;
                continue;
            }

            if (run) {
                nvgRect(vg, x + (QUIET + mx - run) * scale, y + (QUIET + my) * scale, run * scale, scale);
                run = 0;
            }
        }
    }

    nvgFillColor(vg, nvgRGB(0, 0, 0));
    nvgFill(vg);

    return size;
}

#define HIGHLIGHT_SPEED 350.0

static double highlightGradientX = 0;
static double highlightGradientY = 0;
static double highlightColor     = 0;

void updateHighlightAnimation() {
    const auto currentTime = svcGetSystemTick() * 10 / 192 / 1000;

    // Update variables
    highlightGradientX = (std::cos((double)currentTime / HIGHLIGHT_SPEED / 3.0) + 1.0) / 2.0;
    highlightGradientY = (std::sin((double)currentTime / HIGHLIGHT_SPEED / 3.0) + 1.0) / 2.0;
    highlightColor     = (std::sin((double)currentTime / HIGHLIGHT_SPEED * 2.0) + 1.0) / 2.0;
}

void getHighlightAnimation(float* gradientX, float* gradientY, float* color) {
    if (gradientX)
        *gradientX = (float)highlightGradientX;

    if (gradientY)
        *gradientY = (float)highlightGradientY;

    if (color)
        *color = (float)highlightColor;
}

} // namespace sphaira::ui::gfx

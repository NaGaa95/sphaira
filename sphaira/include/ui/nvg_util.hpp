#pragma once

#include "nanovg.h"
#include "qrcode.hpp"
#include "ui/types.hpp"
#include "ui/scrolling_text.hpp"

namespace sphaira::ui::gfx {

void drawImage(NVGcontext*, float x, float y, float w, float h, int texture, float rounded = 0.F, float alpha = 1.0F);
void drawImage(NVGcontext*, const Vec4& v, int texture, float rounded = 0.F, float alpha = 1.0F);

void dimBackground(NVGcontext*);

void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGcolor& c, float rounding = 0.F);
void drawRect(NVGcontext*, const Vec4& v, const NVGcolor& c, float rounding = 0.F);
void drawRect(NVGcontext*, float x, float y, float w, float h, const NVGpaint& p, float rounding = 0.F);
void drawRect(NVGcontext*, const Vec4& v, const NVGpaint& p, float rounding = 0.F);

void drawRectOutline(NVGcontext*, const Theme*, float size, float x, float y, float w, float h);
void drawRectOutline(NVGcontext*, const Theme*, float size, const Vec4& v);

void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGcolor& c);
void drawTriangle(NVGcontext*, float aX, float aY, float bX, float bY, float cX, float cY, const NVGpaint& p);

void drawText(NVGcontext*, float x, float y, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, float x, float y, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawText(NVGcontext*, const Vec2& v, float size, const char* str, const char* end, int align, const NVGcolor& c);
void drawText(NVGcontext*, const Vec2& v, float size, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);
void drawTextArgs(NVGcontext*, float x, float y, float size, int align, const NVGcolor& c, const char* str, ...) __attribute__ ((format (printf, 7, 8)));

void drawTextBox(NVGcontext*, float x, float y, float size, float bound, const NVGcolor& c, const char* str, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP, const char* end = nullptr);

void textBounds(NVGcontext*, float x, float y, float *bounds, const char* str);
void textBoundsArgs(NVGcontext*, float x, float y, float *bounds, const char* str, ...) __attribute__ ((format (printf, 5, 6)));

auto getButton(Button button) -> const char*;
void drawScrollbar(NVGcontext*, const Theme*, u32 index_off, u32 count, u32 max_per_page);
void drawScrollbar(NVGcontext*, const Theme*, float x, float y, float h, u32 index_off, u32 count, u32 max_per_page);

void drawScrollbar2(NVGcontext*, const Theme*, float x, float y, float h, s64 index_off, s64 count, s64 row, s64 page);
void drawScrollbar2(NVGcontext*, const Theme*, s64 index_off, s64 count, s64 row, s64 page);

void drawAppLable(NVGcontext* vg, const Theme*, ScrollingText& st, float x, float y, float w, const char* name);

void drawSpinner(NVGcontext* vg, const Theme*, float cx, float cy, float r, float t);

// draws the code, and the quiet zone around it, as large as it can within
// max_size. returns the size actually drawn, which is a whole number of
// modules and thus usually smaller than max_size.
auto drawQrCode(NVGcontext* vg, float x, float y, float max_size, const qr::Matrix& matrix) -> float;

void updateHighlightAnimation();
void getHighlightAnimation(float* gradientX, float* gradientY, float* color);

} // namespace sphaira::ui::gfx


#include "ui/menus/image_viewer.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"
#include "image.hpp"

namespace sphaira::ui::menu::imageview {
namespace {

} // namespace

Menu::Menu(fs::Fs* fs, const fs::FsPath& path) : m_path{path} {
    SetAction(Button::B, Action{[this](){
        SetPop();
    }});

    std::vector<u8> m_image_buf;
    const auto rc = fs->read_entire_file(path, m_image_buf);
    if (R_FAILED(rc)) {
        App::PushErrorBox(rc, "Failed to load image"_i18n);
        SetPop();
        return;
    }

    // try and load using nvjpg if possible.
    u32 flags = ImageFlag_None;
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) {
        flags = ImageFlag_JPEG;
    }

    Load(m_image_buf, flags);
}

Menu::Menu(std::span<const u8> data, u32 flags) {
    SetAction(Button::B, Action{[this](){
        SetPop();
    }});

    Load(data, flags);
}

void Menu::Load(std::span<const u8> data, u32 flags) {
    const auto result = ImageLoadFromMemory(data, flags);
    if (result.data.empty()) {
        SetPop();
        return;
    }

    m_image = nvgCreateImageRGBA(App::GetVg(), result.w, result.h, 0, result.data.data());
    if (m_image <= 0) {
        SetPop();
        return;
    }

    m_image_width = result.w;
    m_image_height = result.h;

    // scale to fit.
    const auto ws = SCREEN_WIDTH / m_image_width;
    const auto hs = SCREEN_HEIGHT / m_image_height;
    m_zoom = std::min(ws, hs);

    UpdateSize();
}

Menu::~Menu() {
    nvgDeleteImage(App::GetVg(), m_image);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    Widget::Update(controller, touch);

    const auto kdown = controller->m_kdown | controller->m_kheld;

    // pan support.
    constexpr auto max_pan = 10.f;
    constexpr auto max_panx = max_pan;// * (SCREEN_WIDTH / SCREEN_HEIGHT);
    constexpr auto max_pany = max_pan;

    if (controller->Got(kdown, Button::LS_LEFT)) {
        m_xoff += max_panx;
    }
    if (controller->Got(kdown, Button::LS_RIGHT)) {
        m_xoff -= max_panx;
    }
    if (controller->Got(kdown, Button::LS_UP)) {
        m_yoff += max_pany;
    }
    if (controller->Got(kdown, Button::LS_DOWN)) {
        m_yoff -= max_pany;
    }

    // zoom support, by 1% increments.
    constexpr auto max_zoom = 0.01f;
    if (controller->Got(kdown, Button::RS_UP)) {
        m_zoom += max_zoom;
    }
    if (controller->Got(kdown, Button::RS_DOWN)) {
        m_zoom -= max_zoom;
    }

    if (controller->Got(kdown, Button::LS_ANY) || controller->Got(kdown, Button::RS_ANY)) {
        UpdateSize();
    }
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    gfx::drawRect(vg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, nvgRGB(0, 0, 0));
    gfx::drawImage(vg, m_xoff + GetX(), m_yoff + GetY(), GetW(), GetH(), m_image);

    // todo: when pan/zoom, show image info to the screen.
    // todo: maybe show image info by default and option to hide it.
}

void Menu::UpdateSize() {
    m_zoom = std::clamp(m_zoom, 0.1f, 4.0f);

    // center pos.
    const auto cx = SCREEN_WIDTH / 2;
    const auto cy = SCREEN_HEIGHT / 2;

    // calc position and size.
    const auto w = m_image_width * m_zoom;
    const auto h = m_image_height * m_zoom;
    const auto x = cx - (w / 2);
    const auto y = cy - (h / 2);
    SetPos(x, y, w, h);

    // clip edges.
    if (SCREEN_HEIGHT >= h) {
        m_yoff = 0;
        // m_yoff = std::clamp(m_yoff, -y, +y);
    } else {
        m_yoff = std::clamp(m_yoff, (SCREEN_HEIGHT - h) - y, (h - SCREEN_HEIGHT) + y);
    }

    if (SCREEN_WIDTH >= w) {
        m_xoff = 0;
        // m_xoff = std::clamp(m_xoff, -x, +x);
    } else {
        m_xoff = std::clamp(m_xoff, (SCREEN_WIDTH - w) - x, (w - SCREEN_WIDTH) + x);
    }
}

} // namespace sphaira::ui::menu::imageview

#pragma once

#include "ui/widget.hpp"
#include "fs.hpp"
#include <functional>
#include <span>
#include <vector>

namespace sphaira::ui::menu::imageview {

struct Menu final : Widget {
    // hands back the image before or after this one, where there is one.
    // direction is -1 or +1.
    using NavigateCallback = std::function<bool(int direction, std::vector<u8>& out)>;

    Menu(fs::Fs* fs, const fs::FsPath& path);
    // for an image that is already in memory, ie. one handed over by the
    // album, which has no path to read back from and pass navigate to allow
    // stepping through a list without leaving the viewer.
    Menu(std::span<const u8> data, u32 flags, const NavigateCallback& navigate = nullptr);
    ~Menu();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

    auto IsMenu() const -> bool override {
        return true;
    }

    void UpdateSize();

private:
    auto Load(std::span<const u8> data, u32 flags) -> bool;
    void Navigate(int direction);

private:
    const fs::FsPath m_path;
    const NavigateCallback m_navigate{};
    u32 m_flags{};
    int m_image{};
    float m_image_width{};
    float m_image_height{};

    // for zoom, 0.1 - 1.0
    float m_zoom{1};

    // for pan.
    float m_xoff{};
    float m_yoff{};
};

} // namespace sphaira::ui::menu::imageview

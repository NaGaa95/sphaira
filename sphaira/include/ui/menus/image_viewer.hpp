#pragma once

#include "ui/widget.hpp"
#include "fs.hpp"
#include <span>
#include <vector>

namespace sphaira::ui::menu::imageview {

struct Menu final : Widget {
    Menu(fs::Fs* fs, const fs::FsPath& path);
    // for an image that is already in memory, ie. one handed over by the
    // album, which has no path to read back from.
    Menu(std::span<const u8> data, u32 flags);
    ~Menu();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

    auto IsMenu() const -> bool override {
        return true;
    }

    void UpdateSize();

private:
    void Load(std::span<const u8> data, u32 flags);

private:
    const fs::FsPath m_path;
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

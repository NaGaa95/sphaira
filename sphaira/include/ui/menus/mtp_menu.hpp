#pragma once

#include "ui/menus/install_stream_menu_base.hpp"

namespace sphaira::ui::menu::mtp {

struct Menu final : stream::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "MTP"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void OnDisableInstallMode() override;
    bool IsInstallModeActive() const override;

private:
    void UpdateSubHeading();

private:
    TimeStamp m_poll_ts{};
    bool m_was_mtp_enabled{};
};

} // namespace sphaira::ui::menu::mtp

#pragma once

#include "album_server.hpp"
#include "qrcode.hpp"
#include "ui/menus/menu_base.hpp"

#include <memory>
#include <string>

namespace sphaira::ui::menu::album {

// serves the album over http for as long as this screen is open, and shows the
// address, the pin, and a QR code that carries both.
struct ShareMenu final : MenuBase {
    ShareMenu(albumsrv::Items&& items, u16 port);
    ~ShareMenu();

    auto GetShortTitle() const -> const char* override { return "Album"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    void SetAddress(u32 ip);

private:
    std::unique_ptr<albumsrv::Server> m_server{};
    qr::Matrix m_qr{};
    std::string m_url{};
    u32 m_ip{};
};

} // namespace sphaira::ui::menu::album

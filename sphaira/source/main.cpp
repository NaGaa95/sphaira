#include <switch.h>
#include <memory>
#include "app.hpp"
#include "log.hpp"
#include "owo.hpp"
#include "platform.hpp"
#include "ui/menus/main_menu.hpp"
#include "utils/core.hpp"

int main(int argc, char** argv) {
    if (!argc || !argv) {
        return 1;
    }

    if (R_FAILED(sphaira::utils::SetCurrentThreadSafeAffinity())) {
        return 1;
    }

    auto app = std::make_unique<sphaira::App>(argv[0]);
    app->Push<sphaira::ui::menu::main::MainMenu>();
    app->Loop();
    return 0;
}

extern "C" {

void userAppInit(void) {
    sphaira::App::SetBoostMode(true);

    // https://github.com/mtheall/ftpd/blob/e27898f0c3101522311f330e82a324861e0e3f7e/source/switch/init.c#L31
    const SocketInitConfig socket_config_application = {
        .tcp_tx_buf_size = 1024 * 64,
        .tcp_rx_buf_size = 1024 * 64,
        .tcp_tx_buf_max_size = 1024 * 1024 * 4,
        .tcp_rx_buf_max_size = 1024 * 1024 * 4,
        .udp_tx_buf_size = 0x2400, // same as default
        .udp_rx_buf_size = 0xA500, // same as default
        .sb_efficiency = 8,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };

    const SocketInitConfig socket_config_applet = {
        .tcp_tx_buf_size = 1024 * 32,
        .tcp_rx_buf_size = 1024 * 64,
        .tcp_tx_buf_max_size = 1024 * 256,
        .tcp_rx_buf_max_size = 1024 * 256,
        .udp_tx_buf_size = 0x2400, // same as default
        .udp_rx_buf_size = 0xA500, // same as default
        .sb_efficiency = 4,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };

    const auto is_application = sphaira::App::IsApplication();

    const auto socket_config = is_application ? socket_config_application : socket_config_applet;

    Result rc;
    if (R_FAILED(rc = appletLockExit()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = socketInitialize(&socket_config)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = plInitialize(PlServiceType_User)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = nifmInitialize(NifmServiceType_User)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = accountInitialize(is_application ? AccountServiceType_Application : AccountServiceType_System)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = setInitialize()))
        diagAbortWithResult(rc);

    // optional on Switch 2, what uses them handles the failure.
    if (R_FAILED(rc = hidsysInitialize()) && !sphaira::platform::IsSwitch2)
        diagAbortWithResult(rc);
    if (R_FAILED(rc = ncmInitialize()) && !sphaira::platform::IsSwitch2)
        diagAbortWithResult(rc);

    // it doesn't matter if this fails.
    appletSetScreenShotPermission(AppletScreenShotPermission_Enable);

    log_nxlink_init();
}

void userAppExit(void) {
    log_nxlink_exit();

    ncmExit();
    hidsysExit();
    setExit();
    accountExit();
    nifmExit();
    plExit();
    socketExit();
    // NOTE (DMC): prevents exfat corruption.
    if (auto fs = fsdevGetDeviceFileSystem("sdmc:")) {
        fsFsCommit(fs);
    }

    sphaira::App::SetBoostMode(false);
    appletUnlockExit();

    if (sphaira::core_launch_pending()) {
        svcExitProcess();
    }
}

} // extern "C"

#pragma once

// what the platform this build is for supports. the Switch 2 build uses the
// ounce-devkit (libounce, drako3d) and runs under kaiHEN, inside a game process
// with that game's syscall policy and no atmosphère. what can't work there is
// off: the ui hides it and its entry points refuse it.
// features backed by an optional library are turned off in sphaira/CMakeLists.txt.

namespace sphaira::platform {

#if defined(__SWITCH2__)
inline constexpr bool IsSwitch2 = true;
#else
inline constexpr bool IsSwitch2 = false;
#endif

// nsp / nsz / xci / xcz installs from any source, they need sigpatches and the
// Switch 1 keys.
inline constexpr bool SupportsInstall = !IsSwitch2;
// forwarders and the 4 core launch, both install an nca built around hbl.
inline constexpr bool SupportsForwarders = !IsSwitch2;
// the console keys, spl refuses to derive them on Switch 2.
inline constexpr bool SupportsKeyDerivation = !IsSwitch2;
// rewriting an installed content meta record, not verified on Switch 2.
inline constexpr bool SupportsContentMetaEdit = !IsSwitch2;
// the game card menu and the device operator's game card commands, which
// differ on Switch 2: IsGameCardInserted ends the program's fs session there.
inline constexpr bool SupportsGameCard = !IsSwitch2;
// usb install, libounce has not adapted usb:ds yet.
inline constexpr bool SupportsUsbInstall = !IsSwitch2;
// Joy-Con 2 has no ir camera.
inline constexpr bool SupportsIrs = !IsSwitch2;
// themezer themes are for the Switch 1 home menu.
inline constexpr bool SupportsThemezer = !IsSwitch2;
// emummc and exosphère configs, and atmosphère's folders. svcCallSecureMonitor
// ends the process on Switch 2.
inline constexpr bool HasAtmosphere = !IsSwitch2;
// the calibration and safe mode partitions and the raw nand, fs locks the raw
// partitions on Switch 2.
inline constexpr bool SupportsAllNandPartitions = !IsSwitch2;
// the web applet only shows "This feature is not available" on Switch 2.
inline constexpr bool SupportsWebApplet = !IsSwitch2;
// the homebrew menu's abi warning.
inline constexpr bool ShowsAbiWarning = !IsSwitch2;
// render at the output resolution: 1080p handheld, the tv's (up to 4K) docked.
inline constexpr bool FollowsDisplayResolution = IsSwitch2;

} // namespace sphaira::platform

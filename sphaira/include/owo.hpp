#pragma once

#include <switch.h>
#include <string>
#include <vector>
#include "ui/progress_box.hpp"
#include "utils/core.hpp"

namespace sphaira {

// Values are the NPDM AddressSpaceType field, written straight into meta.flags.
// The 32-bit spaces start at 0x00200000 rather than 0x08000000, which is what
// lets homebrew map a fixed low image base; they cap total VA at 4 GiB.
enum class ForwarderAddressSpace : u8 {
    Bit32 = 0,
    Bit36 = 1,
    Bit32NoAlias = 2,
    Bit39 = 3,
};

enum class ForwarderSvcDebugMode : u8 {
    Automatic,
    Enabled,
    Disabled,
};

// Controls whether the forwarder gets a retail-range title id and
// AllowDebug so SaltyNX/FPSLocker treats it like a native game process.
// Automatic keeps the built-in path detections (currently NetherSX2);
// Enabled and Disabled force the choice for any NRO.
enum class ForwarderNativeGameMode : u8 {
    Automatic,
    Enabled,
    Disabled,
};

struct OwoConfig {
    std::string nro_path;
    std::string args{};
    std::string name{};
    std::string author{};
    NacpStruct nacp;
    std::vector<u8> icon;
    std::vector<u8> logo;
    std::vector<u8> gif;
    bool profile_selection{};
    ForwarderAddressSpace address_space{ForwarderAddressSpace::Bit36};
    CpuCoreMode core_mode{CpuCoreMode::Three};
    bool screenshot{true};
    bool video_capture{true};
    ForwarderNativeGameMode native_game_mode{ForwarderNativeGameMode::Automatic};
    ForwarderSvcDebugMode svc_debug_mode{ForwarderSvcDebugMode::Automatic};

    std::vector<u8> program_nca{};
};

auto install_forwarder(OwoConfig& config, NcmStorageId storage_id) -> Result;
auto install_forwarder(ui::ProgressBox* pbox, OwoConfig& config, NcmStorageId storage_id) -> Result;
auto prepare_core_launch(const std::string& nro_path, const std::string& args, CpuCoreMode core_mode) -> Result;
auto core_launch_pending() -> bool;

} // namespace sphaira

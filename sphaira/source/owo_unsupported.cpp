// owo for the Switch 2 build, which cannot install forwarders (no hbl).
#include "owo.hpp"
#include "defines.hpp"
#include "platform.hpp"

static_assert(!sphaira::platform::SupportsForwarders, "build owo.cpp instead");

namespace sphaira {

auto install_forwarder(ui::ProgressBox* pbox, OwoConfig& config, NcmStorageId storage_id) -> Result {
    R_THROW(Result_PlatformUnsupported);
}

auto install_forwarder(OwoConfig& config, NcmStorageId storage_id) -> Result {
    R_THROW(Result_PlatformUnsupported);
}

auto prepare_core_launch(const std::string& nro_path, const std::string& args, CpuCoreMode core_mode) -> Result {
    R_THROW(Result_PlatformUnsupported);
}

auto core_launch_pending() -> bool {
    return false;
}

} // namespace sphaira

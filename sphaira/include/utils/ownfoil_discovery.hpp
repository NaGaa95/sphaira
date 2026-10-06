#pragma once

#include <string>
#include <vector>
#include <stop_token>

namespace sphaira::ownfoil::discovery {

struct DiscoveredServer {
    std::string uid{};
    std::string name{};
    std::string version{};
    std::string local{};
    std::string remote{};
    bool is_public{};
};

// broadcasts an OWNFOIL_DISCOVER request on the LAN (UDP :8465) and collects
// replies for a bounded window (~1.5s). blocking: call from a worker thread.
auto Discover(std::stop_token token) -> std::vector<DiscoveredServer>;

} // namespace sphaira::ownfoil::discovery

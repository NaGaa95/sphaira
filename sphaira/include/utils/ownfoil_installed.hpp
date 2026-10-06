#pragma once

#include <string>
#include <vector>
#include <switch.h>

namespace sphaira::ownfoil::installed {

// one application this console holds.
struct Title {
    std::string id{};  // 16 uppercase hex digits, spelled the way the shop spells ids.
    u32 version{};     // the highest update installed for it, 0 when there is none.
};

// what this console holds, in the shape the shop's filters ask for it: ids as
// strings, since every use of them is a comparison against what the shop sends.
struct Inventory {
    std::vector<Title> titles{};
    std::vector<std::string> dlc_ids{};  // add-on content ids, same spelling.
};

// `content` also asks ns what is installed under each title, the only thing that
// fills in `version` and `dlc_ids`, at a query per title. call from a worker.
auto Scan(bool content) -> Inventory;

// 0 when no update is installed, and 0 when the scan didn't look.
auto UpdateVersionOf(const Inventory& inv, const std::string& title_id) -> u32;

auto TitleIds(const Inventory& inv) -> std::vector<std::string>;

// what this console holds of one game.
struct InstalledVersion {
    bool installed{};
    u32 version{};         // the newest installed: an update's, or 0 for the base game.
    std::string display{}; // what that version calls itself; empty when it can't be read.
};

// reads the game's own control data for its version string, so it costs a trip
// to storage on top of the ns query. call from a worker.
auto InstalledVersionOf(const std::string& title_id) -> InstalledVersion;

// the console only answers for an add-on through the game it belongs to.
auto HasDlc(const std::string& title_id, const std::string& dlc_id) -> bool;

// one query for every row on a game's page. call from a worker.
auto InstalledDlcIds(const std::string& title_id) -> std::vector<std::string>;

} // namespace sphaira::ownfoil::installed

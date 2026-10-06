#pragma once

#include "utils/ownfoil.hpp"
#include <string>
#include <vector>
#include <stop_token>
#include <switch.h>

namespace sphaira::ownfoil::api {

// what *this* caller may use right now: server support and the user's own
// permissions, not the server's feature list in the abstract.
struct Features {
    bool shop{};
    bool dumps_upload{};
    bool save_backup{};
    bool resumable_download{};
    bool resumable_upload{};
};

struct ServerInfo {
    std::string uid{}; // the server's own stable id, same value discovery reports.
    std::string name{};
    std::string version{}; // server software version.
    s64 protocol_version{}; // version of the api contract itself.
    std::string motd{};
    bool is_public{};
    // where the server says it can be reached from anywhere. an empty one means it
    // has nothing to say, not that a saved one should be forgotten. there is no
    // local counterpart: only discovery, which hears the shop on this lan, says that.
    std::string remote_address{};
    Features features{};
};

struct ConnectResult {
    bool success{};
    bool used_remote{}; // false = connected via local_address.
    bool reached{}; // the server answered, even if it answered with an error.
    bool refused{}; // the server itself turned the request down: bad credentials, no access.
    std::string error{};
    std::string base_url{}; // the root url that answered, with a trailing '/'.
    ServerInfo info{};
};

// one piece of a game's artwork. the shop's own copy needs the shop's
// credentials; the catalogue cdn it falls back to must never be handed them.
struct ShopImage {
    std::string url{}; // absolute; a local url is resolved against the shop root.
    bool local{};      // the shop serves these bytes itself.
};

// the download has no file name, so `extension` is the only thing saying which
// container is about to be read.
struct ShopDownload {
    std::string url{};       // absolute, resolved against the shop root.
    std::string extension{}; // nsp, nsz, xci or xcz; empty when the shop didn't say.
};

// a card lives as long as the page it arrived on, and a page can outlast a
// switch to another category, so it records its own kind.
enum class AppType {
    Base,
    Update,
    Dlc,
};

// one browsable game in the shop's catalog.
struct ShopApp {
    AppType type{AppType::Base};
    std::string app_id{};
    // the game this belongs to, the app id itself for a base game. an update
    // reaches its game's page only through it.
    std::string title_id{};
    std::string name{};
    std::string publisher{};
    std::string game_name{}; // a dlc's game, named on the dlc's page.
    // what installing would leave on the console: the newest update the shop
    // holds, falling back to the base game.
    std::string version{};
    ShopImage icon{};
    ShopImage banner{};
    // a dlc card's own file: the shop answers with no apps under a dlc's id, so
    // its page can't ask for this itself. empty on other cards.
    ShopDownload download{};
};

// the handshake: an OPTIONS request to the server root, with Basic Auth when
// credentials are configured. tries local_address first, then remote_address.
auto Connect(const Config& config, std::stop_token token) -> ConnectResult;

// which slice of the shop a page is drawn from. the console-crossed ones are
// narrowed by the shop, not by filtering a fetched page: a client-side filter
// would leave the counter lying and the pages full of holes.
enum class Category : s64 {
    All,      // every base game the shop holds.
    NewGames, // base games this console hasn't installed.
    Updates,  // updates newer than the one installed for an installed game.
    Dlc,      // add-ons for installed games that aren't installed.
    Search,   // base games matching what the user typed.
};

// what to ask the catalog for: which slice of it, in what order.
struct CatalogQuery {
    s64 page{1};      // 1-based, as the shop counts them.
    s64 page_size{};  // capped at 1000 by the server.
    Category category{Category::All};
    // the console's side of the category: New games excludes `app_ids`, Updates
    // asks for exactly them, DLC looks under `title_ids` and excludes `app_ids`.
    std::vector<std::string> app_ids{};
    std::vector<std::string> title_ids{};
    // the bare term as typed, for Search only; the shop adds the wildcards.
    std::string search{};
    // an OrderField interpolated straight into the query document, so it comes
    // from a fixed table and never from anything typed in.
    const char* order_field{"NAME"};
    bool descending{};
};

// fetches one page, and `total` for the whole category, which the counter draws.
auto FetchApps(const std::string& base_url, const Config& config, std::stop_token token, const CatalogQuery& query, std::vector<ShopApp>& out, s64& total, std::string& error) -> bool;

// the newest update the shop can serve for one title.
struct ShopUpdate {
    std::string app_id{};
    std::string title_id{};
    s64 version{}; // the number nintendo counts in, which is what ns reports too.
};

// asks which of `title_ids` the shop holds an update for, and how new it is.
// Updates is a threshold per title, which no filter expresses, so it is settled
// here first and the page query then asks for exactly the app ids it returned.
// ids and versions only: metadata for a title that is up to date is wasted.
auto FetchUpdates(const std::string& base_url, const Config& config, std::stop_token token, const std::vector<std::string>& title_ids, std::vector<ShopUpdate>& out, std::string& error) -> bool;

// one dlc a game's page lists, with what its row draws.
struct ShopContent {
    std::string app_id{};
    std::string name{};
    std::string intro{};
    ShopImage banner{};
    s64 version{}; // the newest the shop holds, which is what `download` is.
    ShopDownload download{};
};

// one version the shop can serve: the base game at 0, then one per update it
// holds. the install menu's version picker lists these.
struct ShopVersion {
    s64 version{};         // the number nintendo counts in; 0 is the base game.
    std::string display{}; // what that version's own file reports, empty until the shop has read one.
    ShopDownload download{};
};

// what a game or dlc page draws beyond what its card already carried.
struct ShopTitle {
    // also what a page opened from a game's add-on row draws, having no card.
    std::string name{};
    std::string publisher{};
    ShopImage banner{};
    ShopImage full_banner{}; // the same banner at viewer size.
    std::string intro{};
    std::string description{};
    std::string release_date{}; // as the catalogue spells it: yyyymmdd.
    std::string genre{};        // the catalogue's categories, joined.
    std::string players{};
    std::string rating{};       // a bare age, which only its region turns into a rating.
    std::string region{};
    s64 size{};                 // install size as the catalogue reports it, 0 when it doesn't.
    s64 download_size{};        // the base game's own file, for a title the catalogue doesn't size.
    std::vector<ShopImage> screenshots{};
    std::vector<ShopImage> full_screenshots{}; // same order, viewer size.
    // the newest version the catalogue knows of, held or not, -1 when it knows
    // none. only a version the shop holds reports a string of its own, so this
    // one is known by its number and release date alone.
    s64 latest_version{-1};
    std::string latest_date{};
    // the newest version the shop holds, or 0 for the base game alone. -1 when
    // it holds no base game for the title, which is every dlc.
    s64 available_version{-1};
    std::string available_display{};
    // every version the shop can serve, oldest first. empty on a dlc's own page,
    // whose id answers with no apps at all.
    std::vector<ShopVersion> versions{};
    std::vector<ShopContent> dlc{}; // empty on a dlc's own page.
};

// `id` is a game's title id, or a dlc's own app id; the shop answers the same
// way for both.
auto FetchTitle(const std::string& base_url, const Config& config, std::stop_token token, const std::string& id, ShopTitle& out, std::string& error) -> bool;

} // namespace sphaira::ownfoil::api

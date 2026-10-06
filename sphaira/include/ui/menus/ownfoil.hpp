#pragma once

#include "ui/menus/grid_menu_base.hpp"
#include "ui/list.hpp"
#include "ui/scrolling_text.hpp"
#include "option.hpp"
#include "utils/ownfoil.hpp"
#include "utils/ownfoil_discovery.hpp"
#include "utils/ownfoil_api.hpp"
#include "utils/ownfoil_installed.hpp"
#include "utils/thread.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <optional>
#include <stop_token>

namespace sphaira::ui::menu::ownfoil {

struct Menu final : grid::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Ownfoil"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    enum class Mode {
        ServerList,
        Home,
    };

    // all three draw the same fetched data, so cycling costs nothing.
    enum ViewType {
        ViewType_Icon,
        ViewType_Banner,
        ViewType_Detail,
        ViewType_Count,
    };

    enum class ImageState { None, Progress, Done, Failed };

    struct LazyImage {
        LazyImage() = default;
        LazyImage(const LazyImage&) = delete;
        auto operator=(const LazyImage&) -> LazyImage& = delete;
        // movable so the entry vector can grow; the moved-from half must not
        // then delete the texture the new owner holds.
        LazyImage(LazyImage&& rhs) noexcept;
        auto operator=(LazyImage&& rhs) noexcept -> LazyImage&;
        ~LazyImage();
        int image{};
        bool tried_cache{};
        bool cached{};
        ImageState state{ImageState::None};
    };

    // only the image the current view draws is ever requested.
    struct Entry {
        sphaira::ownfoil::api::ShopApp app{};
        LazyImage icon{};
        LazyImage banner{};
    };

    // one row of the server list: a saved entry, a server discovered on the
    // network, or a saved entry that discovery also just answered for.
    struct Candidate {
        sphaira::ownfoil::Config config{};
        bool saved{};          // already present in ownfoil.ini.
        std::string version{}; // from discovery, empty when not seen this scan.
        bool is_public{};      // only meaningful when discovery saw it.
    };

private:
    void SetMode(Mode mode);
    void ReloadSaved();
    void RebuildCandidates();
    void StartDiscovery();
    void OnServerSelected(s64 index);
    void ConnectTo(const sphaira::ownfoil::Config& config);
    void ShowOptions();
    // the left panel Y brings up: which slice of the shop to show.
    void ShowCategories();
    // `index` is the Search category's own place in CATEGORIES.
    void ShowSearch(s64 index);
    // the server name and the category on screen, drawn above the grid.
    void SetCatalogTitle();
    void OnViewChange();
    void CycleView();
    // fetches `page` (0-based) in the background, leaving the page on screen up.
    void LoadPage(s64 page);
    void ApplyPage(s64 page, std::vector<sphaira::ownfoil::api::ShopApp>& apps, s64 total);
    auto GetPageMax() -> s64;
    auto BuildQuery(s64 page) -> sphaira::ownfoil::api::CatalogQuery;
    // fills in what only this console can answer. runs on the fetch worker: it
    // scans the console and can ask the shop a question of its own, returning
    // false with `error` set when that fails.
    auto PrepareQuery(sphaira::ownfoil::api::CatalogQuery& query, const std::string& base_url, const sphaira::ownfoil::Config& config, std::stop_token token, std::string& error) -> bool;
    void DrawHome(NVGcontext* vg, Theme* theme);
    void FreeEntries();
    // kicks off a cache read or download when there is no texture yet; `budget`
    // caps the work done per frame.
    auto GetImage(Entry& e, bool banner, int& budget) -> int;
    void OpenSelected();
    void EditSelected();
    auto FindAction(Button button) const -> std::optional<Action>;
    void RestoreAction(Button button, std::optional<Action>& prev);
    void DeleteSelected();

    void DrawDiscoverButton(NVGcontext* vg, Theme* theme) const;

    auto FindDiscovered(const std::string& uid) const -> const sphaira::ownfoil::discovery::DiscoveredServer*;

private:
    static constexpr inline const char* INI_SECTION = "ownfoil";

    Mode m_mode{Mode::ServerList};
    bool m_did_initial_setup{};

    sphaira::ownfoil::Configs m_saved{};
    std::vector<sphaira::ownfoil::discovery::DiscoveredServer> m_discovered{};
    std::vector<Candidate> m_candidates{};

    s64 m_index{};
    std::unique_ptr<List> m_list{};

    // the discover button sits below the list and shares its focus: down off the
    // last row moves onto it, up moves back.
    bool m_focus_discover{};
    bool m_discovering{};
    std::unique_ptr<utils::Async> m_discovery{};

    // no dialog for a connect: the list's place shows what is being connected to,
    // and B cancels it through m_connect_stop.
    bool m_connecting{};
    std::string m_connecting_name{};
    std::stop_source m_connect_stop{};
    std::unique_ptr<utils::Async> m_connect_async{};

    // the fetch worker's own: the scan is far too slow for the main thread. two
    // workers can briefly overlap - a page fetch takes as long as its request to
    // notice it was abandoned - so the set is guarded. declared ahead of
    // m_loader, whose destructor joins the worker still reading it.
    std::mutex m_installed_mutex{};
    sphaira::ownfoil::installed::Inventory m_installed{};
    bool m_installed_scanned{};
    bool m_installed_content{}; // the scan also walked each title's content.
    std::vector<std::string> m_outdated{};
    // the SignalInstalled count when the console was last scanned (guarded with
    // it) and when the page was last asked for (main thread only). either
    // falling behind means what the console holds has changed under it.
    u32 m_installs_scanned{};
    u32 m_installs_shown{};

    std::vector<sphaira::ownfoil::api::ShopApp> m_fetched{};
    std::string m_fetch_error{};
    std::vector<Entry> m_entries{};
    s64 m_entry_index{};
    // m_total is every title the category holds, not this page's worth, and comes
    // back with each page.
    s64 m_page{};
    s64 m_total{};
    // everything the page on screen has in flight - its fetch and its artwork -
    // so leaving the page abandons all of it rather than letting it finish into a
    // grid that has moved on.
    std::stop_source m_page_stop{};
    bool m_loading{};
    // the one request that arrived mid-fetch; only the newest is worth keeping,
    // since each builds its query when it is finally issued.
    std::optional<s64> m_pending_page{};
    std::unique_ptr<utils::Async> m_loader{};
    std::string m_base_url{};
    // artwork the shop serves itself is gated like the catalog, so drawing needs
    // its credentials.
    sphaira::ownfoil::Config m_config{};
    ScrollingText m_banner_name{};
    ScrollingText m_banner_publisher{};
    // '-' and Y belong to MainMenu while this menu is a tab; borrowed for the
    // home view and handed back on the way out.
    std::optional<Action> m_prev_select{};
    std::optional<Action> m_prev_y{};
    // an index into CATEGORIES, deliberately not persisted unlike the options
    // beside it: a shop opens on New games every time it is connected to.
    s64 m_category{};
    std::string m_search{}; // not persisted either, and cleared on connect.

    option::OptionLong m_view{INI_SECTION, "view", ViewType_Icon};
    option::OptionLong m_page_size{INI_SECTION, "page_size", 24};
    option::OptionLong m_sort{INI_SECTION, "sort", 0};
    option::OptionBool m_sort_desc{INI_SECTION, "sort_desc", false};
};

// an install changed what this console holds, so the catalog asks it again
// rather than go on listing what it found before.
void SignalInstalled();

} // namespace sphaira::ui::menu::ownfoil

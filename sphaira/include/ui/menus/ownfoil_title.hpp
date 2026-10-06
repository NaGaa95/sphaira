#pragma once

#include "ui/menus/menu_base.hpp"
#include "utils/ownfoil.hpp"
#include "utils/ownfoil_api.hpp"
#include "utils/ownfoil_installed.hpp"
#include "utils/thread.hpp"
#include "fs.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <stop_token>

namespace sphaira::ui::menu::ownfoil {

// shared by the catalog and the pages it opens, so artwork the grid already
// fetched is read back rather than fetched again.
inline constexpr auto CACHE_PATH = "/switch/sphaira/cache/ownfoil";

// keyed on the url, not the app id: two shops can answer with different art for
// one game, and a shop swaps a hotlink for its own copy as the download lands.
// both ends mint content-addressed names, so a changed url means changed bytes.
auto BuildImageCache(const std::string& app_id, const std::string& url, const char* kind) -> fs::FsPath;

// what a page is opened with, so it can draw the name and artwork its card
// already carried on its first frame rather than wait on the shop.
struct TitlePage {
    std::string id{}; // a game's title id, or a dlc's own app id.
    bool dlc{};
    // on a dlc's page, the only way to ask the console whether it is installed.
    std::string game_id{};
    std::string name{};
    std::string publisher{};
    std::string game_name{}; // a dlc's game.
    sphaira::ownfoil::api::ShopImage banner{};
    // drawn beside the game a dlc needs, and in place of a missing banner.
    sphaira::ownfoil::api::ShopImage icon{};
    // a dlc's own file: the shop answers with no apps under a dlc's id, so this
    // only ever comes from whatever opened the page.
    sphaira::ownfoil::api::ShopDownload download{};
};

// the page's images edge to edge at the size the shop keeps them. pushed over the
// page, which outlives it, so its smaller copies stand in while a full one loads.
struct ScreenshotViewer final : Widget {
    ScreenshotViewer(const sphaira::ownfoil::Config& config, const std::string& id, std::vector<sphaira::ownfoil::api::ShopImage> images, std::vector<int> previews, s64 index);
    ~ScreenshotViewer();

    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

    auto IsMenu() const -> bool override {
        return true;
    }

private:
    void Show(s64 index);

    const sphaira::ownfoil::Config m_config;
    const std::string m_id;
    const std::vector<sphaira::ownfoil::api::ShopImage> m_images;
    // the page's own textures, 0 where none landed: not this viewer's to delete.
    const std::vector<int> m_previews;
    s64 m_index{-1};
    int m_image{}; // the full-size texture for m_index, 0 until it lands.

    // stopped when the image changes, abandoning the load left behind.
    std::stop_source m_stop{};
    std::unique_ptr<utils::Async> m_loader{};
};

// one game or dlc, pushed over the catalog. the right column - the name, the
// facts, the versions - never moves; the left is a page that scrolls under it:
// the images, the tagline over the description, then a row per dlc.
struct TitleMenu final : MenuBase {
    TitleMenu(const sphaira::ownfoil::Config& config, const std::string& base_url, const TitlePage& page);
    ~TitleMenu();

    auto GetShortTitle() const -> const char* override { return "Game"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    // how long a page stays hidden waiting to open whole, from the press. on the
    // console a cached page is ready in ~70-100ms and one downloading its banner
    // in ~160-210ms, so this lets the second open whole too; a slower shop opens
    // at this and fills in, since a longer wait reads as the press being ignored.
    static constexpr u64 OPEN_WAIT_MS = 220;

    // one row of the fact list, its label already translated.
    struct Fact {
        std::string label{};
        std::string value{};
        bool group{}; // starts a group of its own, set a little apart.
    };

    // which of A's two jobs is bound right now: a highlighted row opens its own
    // page, anything else the install menu - which waits for the shop, being a
    // choice of what the shop holds.
    enum class MainAction { None, Install, View };

    // one of the game's dlc, as its row draws it.
    struct Row {
        std::string name{};    // without the game's name in front.
        std::string tagline{}; // flattened onto one line.
        bool installed{};
        int image{};           // its banner, 0 until the decode lands.
    };

    // the shop's once it has answered, the card's until then - too small to draw,
    // but enough to say whether there is a banner to make room for.
    auto GetBanner() const -> const sphaira::ownfoil::api::ShopImage&;
    auto GetName() const -> const std::string&;
    auto GetPublisher() const -> const std::string&;
    // the banner, when there is one, then the screenshots; 0 until a decode lands.
    auto GetImageCount() const -> s64;
    auto GetImage(s64 index) const -> int;
    void SetImageIndex(s64 index);
    // built when the shop answers and again when the console does.
    void BuildFacts();
    // either answer can be the last to land, so both run it.
    void MarkInstalled();
    // run from a worker, when the page opens and after an install.
    void LoadInstalled(std::stop_token token);

    // measures where each part of the page starts, at the width it is drawn at.
    void Layout();
    auto GetImagesBottom() const -> float;
    auto GetMaxScroll() const -> float;
    void ScrollTo(float y);
    // whether the page is still up at its images, which left, right and Y act on.
    auto ImagesInView() const -> bool;
    auto GetRowTop(s64 index) const -> float;
    auto RowInView(s64 index) const -> bool;
    auto FirstRowInView() const -> s64; // -1 when none is wholly on screen.
    auto GetRowRect(s64 index) const -> Vec4;
    // down and up: the page a part at a time, then the rows one at a time.
    void StepDown();
    void StepUp();
    void SetFocus(s64 index); // -1 is no row; a highlighted row is scrolled to.
    // set every frame, outside any action - replacing an Action from inside its
    // own callback would destroy the running std::function.
    void UpdateActions();
    // A off the rows. what the menu is set to lives on the page rather than in the
    // sidebar, so reopening it comes back to the same choices.
    void ShowInstallOptions();
    void Install();
    // one on/off entry per dlc, over the install menu. the row it was opened from
    // is handed in, since it counts what is ticked.
    void ShowAddons(const std::function<void()>& changed);
    auto GetAddonCount() const -> std::string;
    // what Install greys out on, re-asked every frame the menu is drawn.
    auto HasSomethingToInstall() const -> bool;
    // Update or Downgrade when the only thing chosen is a version newer or older
    // than the console's, Install for anything else.
    auto GetInstallLabel() const -> std::string;
    // what the console holds and newer, plus an older update while downgrades are
    // allowed; anything else is greyed out in the picker.
    auto IsVersionOffered(s64 version) const -> bool;
    void OpenDlc(s64 index);
    void OpenViewer();

    void DrawPage(NVGcontext* vg, Theme* theme);
    void DrawColumn(NVGcontext* vg, Theme* theme);

private:
    const sphaira::ownfoil::Config m_config;
    const std::string m_base_url;
    const TitlePage m_page;

    // what OPEN_WAIT_MS counts from, and what the log's timings measure against.
    const TimeStamp m_opened{};

    sphaira::ownfoil::api::ShopTitle m_title{};
    bool m_loaded{}; // the shop has answered for m_title.
    bool m_failed{}; // the shop won't answer: its error has been shown instead.
    // what this console holds, answered on a thread of its own, before the shop
    // or after it.
    bool m_console_loaded{};
    sphaira::ownfoil::installed::InstalledVersion m_installed{};
    std::vector<std::string> m_installed_dlc{};
    bool m_dlc_installed{};
    std::vector<Fact> m_facts{};
    std::string m_intro{}; // the tagline, flattened: it heads the description.
    std::vector<Row> m_rows{};

    int m_banner{};
    int m_icon{};
    // the artwork the page opens on has landed, or failed to: its own banner,
    // fetched once the shop names it, or the card's icon standing in for one.
    bool m_artwork_done{};
    std::vector<int> m_screenshots{};
    s64 m_image_index{};

    // the page's parts, where 0 is the top of the big image.
    float m_text_y{};
    float m_intro_h{};
    float m_desc_y{};
    float m_desc_h{};
    float m_rows_y{};
    float m_content_h{};

    float m_scroll{};        // how far the page is scrolled, as drawn.
    float m_scroll_target{}; // where it is gliding to.
    bool m_stick_scroll{};   // the stick sent it there, which glides at its own pace.
    bool m_dragging{};       // a drag that began on the page is moving it.
    float m_drag_from{};     // where the page was when that drag began.
    s64 m_focus{-1};
    std::string m_counter{}; // the subheading, set only when it changes.
    MainAction m_action{MainAction::None};

    // an index into the title's own version list, the newest until changed.
    s64 m_install_version{};
    // one per add-on row, sized when the menu is first opened - which is also what
    // says the defaults have been shown.
    std::vector<u8> m_install_dlc{};

    // stopped on the way out, ahead of the joins, so nothing still in flight is
    // waited on or run against a page that has gone.
    std::stop_source m_stop{};
    std::unique_ptr<utils::Async> m_loader{};
    // the console's answer, then the card's icon off the sd card: neither is a
    // reason to hold the shop's request up.
    std::unique_ptr<utils::Async> m_console{};
};

} // namespace sphaira::ui::menu::ownfoil

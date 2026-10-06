#include "ui/menus/ownfoil.hpp"
#include "ui/menus/ownfoil_title.hpp"
#include "ui/nvg_util.hpp"
#include "ui/sidebar.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"

#include "app.hpp"
#include "image.hpp"
#include "swkbd.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "defines.hpp"
#include "evman.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <utility>

namespace sphaira::ui::menu::ownfoil {
namespace {

// bumped by every install from a shop page. read by the fetch worker too.
std::atomic<u32> g_installs{};

// the list stops short of the menu body to leave room for the discover button,
// which is why it pages 4 rows rather than filling the area.
constexpr Vec4 LIST_POS{30.f, 87.f, 1190.f, 425.f};
constexpr Vec4 LIST_ITEM{75.f, 110.f, 1130.f, 90.f};
constexpr Vec2 LIST_PAD{0.f, 12.f};
constexpr Vec4 DISCOVER_BUTTON{75.f, 530.f, 1130.f, 60.f};

// the banner view has no equivalent in grid::Menu, so it lays itself out, in
// the shape themezer uses for its pack cards.
constexpr Vec4 BANNER_ITEM{75.f, 110.f, 350.f, 250.f};
constexpr Vec2 BANNER_PAD{10.f, 10.f};
constexpr float BANNER_W = 320.f;
constexpr float BANNER_H = 180.f;

// every page is a round trip and a screenful of artwork, so the choices stay
// modest at the low end.
constexpr s64 PAGE_SIZES[] = {12, 24, 48, 96};

// the title row draws the term back, so the cap keeps the header from scrolling.
constexpr s64 SEARCH_MAX = 64;

// what "Sort" offers. the shop's OrderField has more members, but VERSION is the
// app's own version (0 on every retail base game) and SIZE/DOWNLOAD_COUNT aren't
// app fields at all.
struct SortMode {
    const char* field; // the shop's own OrderField name.
    const char* name;  // shown in the sidebar, translated at use.
};

// a bullet keeps the title row's parts apart without reading as a range.
constexpr const char* SEPARATOR = "  \u2022  ";

constexpr SortMode SORT_MODES[] = {
    {"NAME", "Name"},
    {"ADDED_AT", "Date added"},
    {"RELEASE_DATE", "Release date"},
};

// the first three are the shop's catalog crossed with what this console holds,
// which is why they cost a scan of it; the last two only ask the shop.
struct CategoryMode {
    sphaira::ownfoil::api::Category category;
    const char* name; // shown in the panel, translated at use.
};

// the first is what every connect opens on: what the shop can add to this
// console is the reason to open it.
constexpr CategoryMode CATEGORIES[] = {
    {sphaira::ownfoil::api::Category::NewGames, "New games"},
    {sphaira::ownfoil::api::Category::Updates, "Updates"},
    {sphaira::ownfoil::api::Category::Dlc, "DLC"},
    {sphaira::ownfoil::api::Category::All, "All games"},
    {sphaira::ownfoil::api::Category::Search, "Search"},
};

// looked up once: the '-' action is re-asserted every frame the catalog is up.
auto GetViewActionHint() -> const std::string& {
    static const std::string hint = "View"_i18n;
    return hint;
}

// and Y, for the same reason.
auto GetCategoryActionHint() -> const std::string& {
    static const std::string hint = "Show"_i18n;
    return hint;
}

} // namespace

Menu::LazyImage::LazyImage(LazyImage&& rhs) noexcept {
    *this = std::move(rhs);
}

auto Menu::LazyImage::operator=(LazyImage&& rhs) noexcept -> LazyImage& {
    std::swap(image, rhs.image);
    std::swap(tried_cache, rhs.tried_cache);
    std::swap(cached, rhs.cached);
    std::swap(state, rhs.state);
    return *this;
}

Menu::LazyImage::~LazyImage() {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

Menu::Menu(u32 flags) : grid::Menu{"Ownfoil"_i18n, flags} {
    // B leaves the menu from the catalog too: the server list is reached from
    // the catalog's options, not by backing out.
    SetAction(Button::B, Action{"Back"_i18n, [this]{
        // backing out of a connect leaves the list up, not the menu.
        if (m_connecting) {
            m_connect_stop.request_stop();
            m_connecting = false;
            return;
        }
        SetPop();
    }});

    SetAction(Button::A, Action{"Select"_i18n, [this]{
        if (m_mode == Mode::Home) {
            OpenSelected();
            return;
        }

        if (m_mode != Mode::ServerList || m_connecting) {
            return;
        }

        if (m_focus_discover) {
            StartDiscovery();
        } else {
            OnServerSelected(m_index);
        }
    }});

    SetAction(Button::X, Action{"Options"_i18n, [this]{
        if (m_connecting) {
            return;
        }
        ShowOptions();
    }});

    m_list = std::make_unique<List>(1, 4, LIST_POS, LIST_ITEM, LIST_PAD);

    if (m_view.Get() < 0 || m_view.Get() >= ViewType_Count) {
        m_view.Set(ViewType_Icon);
    }

    // a hand-edited ini must not leave the sidebar pointing past the list, nor
    // BuildQuery indexing past SORT_MODES.
    if (m_sort.Get() < 0 || m_sort.Get() >= static_cast<s64>(std::size(SORT_MODES))) {
        m_sort.Set(0);
    }

    SetMode(Mode::ServerList);
}

Menu::~Menu() {
    // ~Async blocks on the worker and runs before the base Object dtor that would
    // cancel it, so the ~1.5s discovery window is cut short here rather than
    // waited out; m_page_stop drops the artwork still queued for the last page.
    m_stop_source.request_stop();
    m_page_stop.request_stop();
    m_connect_stop.request_stop();
    m_connect_async.reset();
}

void Menu::SetMode(Mode mode) {
    const auto was_home = m_mode == Mode::Home;
    m_mode = mode;

    if (mode == Mode::Home) {
        // browsing a shop, the header names it rather than the menu.
        SetTitle(m_config.name);

        // '-' and Y open MainMenu's options and menu picker while this menu is a
        // tab, so remember whatever held them to hand back.
        if (!was_home) {
            m_prev_select = FindAction(Button::SELECT);
            m_prev_y = FindAction(Button::Y);
        }
        SetAction(Button::Y, Action{GetCategoryActionHint(), [this]{
            ShowCategories();
        }});
        SetAction(Button::SELECT, Action{GetViewActionHint(), [this]{
            CycleView();
        }});
        // no hint on ZL: the bar draws it beside ZR, in front of the one hint.
        SetAction(Button::L2, Action{[this]{
            if (m_page > 0) {
                LoadPage(m_page - 1);
            }
        }});
        SetAction(Button::R2, Action{"Page"_i18n, [this]{
            if (m_page + 1 < GetPageMax()) {
                LoadPage(m_page + 1);
            }
        }});
        OnViewChange();
    } else {
        if (was_home) {
            FreeEntries();
            m_pending_page.reset();
            RestoreAction(Button::SELECT, m_prev_select);
            RestoreAction(Button::Y, m_prev_y);
            RemoveAction(Button::L2);
            RemoveAction(Button::R2);
            SetTitle("Ownfoil"_i18n);
            SetTitleSubHeading("");
            SetSubHeading("");
            m_list = std::make_unique<List>(1, 4, LIST_POS, LIST_ITEM, LIST_PAD);
        }
    }
}

auto Menu::FindAction(Button button) const -> std::optional<Action> {
    if (const auto it = m_actions.find(button); it != m_actions.end()) {
        return it->second;
    }
    return {};
}

void Menu::RestoreAction(Button button, std::optional<Action>& prev) {
    if (prev) {
        SetAction(button, *prev);
    } else {
        RemoveAction(button);
    }
    prev.reset();
}

void Menu::FreeEntries() {
    // more than tidiness: a queued download returns at once on a stopped token,
    // which hands the four download threads to the page arriving rather than
    // making it wait out a screenful of artwork nobody will see.
    m_page_stop.request_stop();
    m_page_stop = std::stop_source{};
    m_loading = false;
    m_entries.clear();
    m_entry_index = 0;
}

void Menu::CycleView() {
    m_view.Set((m_view.Get() + 1) % ViewType_Count);
    OnViewChange();
}

void Menu::OnViewChange() {
    m_entry_index = 0;
    m_banner_name.Reset();
    m_banner_publisher.Reset();

    switch (m_view.Get()) {
        case ViewType_Icon:
            grid::Menu::OnLayoutChange(m_list, grid::LayoutType_Grid);
            break;

        case ViewType_Detail:
            grid::Menu::OnLayoutChange(m_list, grid::LayoutType_GridDetail);
            break;

        // grid::Menu knows nothing of this one, so it is built here.
        case ViewType_Banner:
            m_list = std::make_unique<List>(3, 6, m_pos, BANNER_ITEM, BANNER_PAD);
            break;
    }

    // each view hands back a fresh List, which claims L2/R2 to scroll its own
    // rows - here those are the shop's pages, so the press would do both.
    m_list->SetPageJump(false);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    // an install changes what the console-crossed categories list, so the page
    // on screen is asked for again.
    if (m_mode == Mode::Home && m_installs_shown != g_installs) {
        m_installs_shown = g_installs;
        const auto category = CATEGORIES[m_category].category;
        if (category != sphaira::ownfoil::api::Category::All && category != sphaira::ownfoil::api::Category::Search) {
            LoadPage(m_page);
        }
    }

    // MainMenu copies its own actions onto the current tab after OnFocusGained,
    // so '-' and Y have to be reclaimed every frame the catalog owns them.
    if (m_mode == Mode::Home) {
        const auto it = m_actions.find(Button::SELECT);
        if (it == m_actions.end() || it->second.m_hint != GetViewActionHint()) {
            SetAction(Button::SELECT, Action{GetViewActionHint(), [this]{
                CycleView();
            }});
        }

        const auto y = m_actions.find(Button::Y);
        if (y == m_actions.end() || y->second.m_hint != GetCategoryActionHint()) {
            SetAction(Button::Y, Action{GetCategoryActionHint(), [this]{
                ShowCategories();
            }});
        }
    }

    MenuBase::Update(controller, touch);

    if (m_mode == Mode::Home) {
        if (!m_entries.empty()) {
            m_list->OnUpdate(controller, touch, m_entry_index, m_entries.size(), [this](bool is_touch, s64 i) {
                // a tap on the highlighted card opens it, any other only moves
                // the highlight.
                if (is_touch && m_entry_index == i) {
                    FireAction(Button::A);
                } else {
                    m_entry_index = i;
                }
            });
        }
        return;
    }

    if (m_mode != Mode::ServerList || m_connecting) {
        return;
    }

    // taken before the list, whose touch bounds are the whole menu body.
    if (touch->is_clicked && touch->in_range(DISCOVER_BUTTON)) {
        m_focus_discover = true;
        FireAction(Button::A);
        return;
    }

    // with nothing in the list, the button is the only thing there is to focus.
    if (m_candidates.empty()) {
        return;
    }

    if (m_focus_discover) {
        if (controller->GotDown(Button::UP)) {
            App::PlaySoundEffect(SoundEffect::Focus);
            m_focus_discover = false;
            return;
        }

        // the list is kept out of the d-pad's way while the button has focus but
        // still sees touch, so a tap can pull focus back onto a row.
        if (!touch->is_clicked && !touch->is_scroll && !touch->is_end) {
            return;
        }
    } else if (controller->GotDown(Button::DOWN) && m_index == static_cast<s64>(m_candidates.size()) - 1) {
        App::PlaySoundEffect(SoundEffect::Focus);
        m_focus_discover = true;
        return;
    }

    m_list->OnUpdate(controller, touch, m_index, m_candidates.size(), [this](bool is_touch, s64 i) {
        // a tap on the already-selected row selects it, unless that row didn't
        // have focus to begin with - then the tap only takes it back.
        if (is_touch && !m_focus_discover && m_index == i) {
            FireAction(Button::A);
        } else {
            m_focus_discover = false;
            m_index = i;
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    const auto pdata = GetPolledData();
    if (!pdata.ip) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", "Network access required"_i18n.c_str());
        return;
    }

    if (m_mode == Mode::Home) {
        DrawHome(vg, theme);
        return;
    }

    if (m_connecting) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, LIST_POS.y + LIST_POS.h / 2.f, 24.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s",
            (i18n::Reorder("Connecting to ", m_connecting_name) + "...").c_str());
        return;
    }

    if (m_candidates.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, LIST_POS.y + LIST_POS.h / 2.f, 24.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s",
            "No Ownfoil servers saved.\n\nPress X to add one by hand, or search the network below."_i18n.c_str());
    } else {
        m_list->Draw(vg, theme, m_candidates.size(), [this](NVGcontext* vg, Theme* theme, const Vec4& v, s64 index) {
            const auto& [x, y, w, h] = v;
            const auto& candidate = m_candidates[index];
            const auto& config = candidate.config;
            const auto selected = !m_focus_discover && index == m_index;

            if (selected) {
                gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
            }

            const auto text_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT;
            const auto info_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT_INFO;

            // only what is known: a version comes from a scan, and an entry can
            // have either address or both.
            std::string sub{};
            const auto append = [&sub](const std::string& part) {
                if (!sub.empty()) {
                    sub += SEPARATOR;
                }
                sub += part;
            };
            if (!candidate.version.empty()) {
                append("v" + candidate.version);
            }
            if (!config.local_address.empty()) {
                append("Local"_i18n + " (" + config.local_address + ")");
            }
            if (!config.remote_address.empty()) {
                append("Remote"_i18n + " (" + config.remote_address + ")");
            }

            gfx::drawTextArgs(vg, x + 20.f, y + h / 2.f - 16.f, 22.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s", config.name.c_str());
            gfx::drawTextArgs(vg, x + 20.f, y + h / 2.f + 16.f, 16.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(info_id), "%s", sub.c_str());

            // anything the list shows that isn't saved yet came out of a scan.
            if (!candidate.saved) {
                const Vec4 tag{x + w - 90.f, y + h / 2.f - 14.f, 70.f, 28.f};
                gfx::drawRect(vg, tag, theme->GetColour(ThemeEntryID_HIGHLIGHT_1), 5.f);
                gfx::drawTextArgs(vg, tag.x + tag.w / 2.f, tag.y + tag.h / 2.f, 18.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_BACKGROUND), "%s", "New"_i18n.c_str());
            }
        });
    }

    DrawDiscoverButton(vg, theme);
}

void Menu::DrawHome(NVGcontext* vg, Theme* theme) {
    if (m_entries.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 30.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", "Empty..."_i18n.c_str());
        return;
    }

    // capped so a page of fresh artwork doesn't stall the frame on io or gpu.
    int budget = 2;
    const auto view = m_view.Get();

    m_list->Draw(vg, theme, m_entries.size(), [this, view, &budget](NVGcontext* vg, Theme* theme, const Vec4& v, s64 index) {
        const auto& [x, y, w, h] = v;
        auto& e = m_entries[index];
        const auto selected = index == m_entry_index;

        if (view == ViewType_Banner) {
            if (selected) {
                gfx::drawRectOutline(vg, theme, 4.f, v);
            } else {
                DrawElement(v, ThemeEntryID_GRID);
            }

            const auto text_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT;
            const auto info_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT_INFO;
            const auto xoff = (BANNER_ITEM.w - BANNER_W) / 2.f;
            const auto image = GetImage(e, true, budget);

            gfx::drawImage(vg, x + xoff, y, BANNER_W, BANNER_H, image ? image : App::GetDefaultImage(), 5.f);
            m_banner_name.Draw(vg, selected, x + xoff, y + BANNER_H + 20.f, w - 30.f - xoff, 18.f, NVG_ALIGN_LEFT, theme->GetColour(text_id), e.app.name.c_str());
            m_banner_publisher.Draw(vg, selected, x + xoff, y + BANNER_H + 55.f, w - 30.f - xoff, 18.f, NVG_ALIGN_LEFT, theme->GetColour(info_id), e.app.publisher.c_str());
            return;
        }

        const auto layout = view == ViewType_Detail ? grid::LayoutType_GridDetail : grid::LayoutType_Grid;
        DrawEntry(vg, theme, layout, v, selected, GetImage(e, false, budget), e.app.name.c_str(), e.app.publisher.c_str(), e.app.version.c_str());
    });
}

auto Menu::GetImage(Entry& e, bool banner, int& budget) -> int {
    auto& image = banner ? e.banner : e.icon;
    const auto& src = banner ? e.app.banner : e.app.icon;
    const auto& url = src.url;

    if (image.image) {
        return image.image;
    }

    // a title the catalog has no artwork for is never going to gain any.
    if (url.empty()) {
        image.state = ImageState::Failed;
        return App::GetDefaultImage();
    }

    if (image.state == ImageState::Failed) {
        return App::GetDefaultImage();
    }

    // this runs per visible entry per frame, so bail before hashing a cache path
    // that won't be used.
    if (image.state == ImageState::Progress) {
        return 0;
    }

    if (budget <= 0) {
        return 0;
    }

    const auto path = BuildImageCache(e.app.app_id, url, banner ? "banner" : "icon");

    const auto load = [&]() -> bool {
        const auto data = ImageLoadFromFile(path, ImageFlag_JPEG);
        if (data.data.empty()) {
            return false;
        }
        image.image = nvgCreateImageRGBA(App::GetVg(), data.w, data.h, 0, data.data.data());
        return image.image != 0;
    };

    // whatever is already on the sd card is worth a look before the network.
    if (!image.tried_cache) {
        image.tried_cache = true;
        image.cached = load();
        if (image.cached) {
            // only a real decode is worth charging against the frame's budget.
            budget--;
            return image.image;
        }
    }

    switch (image.state) {
        case ImageState::None: {
            image.state = ImageState::Progress;
            const auto index = &e - m_entries.data();

            // the shop gates its own artwork like its catalogue, so its copies
            // need credentials; a hotlink points at a cdn that must not get them.
            const auto user = src.local ? m_config.user : "";
            const auto pass = src.local ? m_config.pass : "";

            const auto queued = curl::Api().ToFileAsync(
                curl::Url{url},
                curl::Path{path},
                curl::UserPass{user, pass},
                curl::PreemptiveAuth{src.local},
                curl::Flags{curl::Flag_Cache},
                curl::StopToken{m_page_stop.get_token()},
                curl::OnComplete{[this, index, banner](auto& result) {
                    // entries never move while a page is up, and leaving one stops
                    // the token this carries, so the index names the same title.
                    auto& entry = m_entries[index];
                    auto& done = banner ? entry.banner : entry.icon;
                    done.state = result.success ? ImageState::Done : ImageState::Failed;
                }}
            );

            if (!queued) {
                image.state = ImageState::Failed;
            }
        }   break;

        case ImageState::Done: {
            if (load()) {
                budget--;
            } else {
                image.state = ImageState::Failed;
            }
        }   break;

        case ImageState::Progress:
        case ImageState::Failed:
            break;
    }

    return image.image;
}

void Menu::DrawDiscoverButton(NVGcontext* vg, Theme* theme) const {
    gfx::drawRect(vg, DISCOVER_BUTTON, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND), 5.f);

    if (m_focus_discover) {
        gfx::drawRectOutline(vg, theme, 4.f, DISCOVER_BUTTON);
    }

    // greyed out for the duration of the scan: it can't be started twice.
    const auto colour_id = m_discovering ? ThemeEntryID_TEXT_INFO : (m_focus_discover ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT);
    const auto text = m_discovering ? "Searching the network..."_i18n : "Discover local servers"_i18n;

    gfx::drawTextArgs(vg, DISCOVER_BUTTON.x + DISCOVER_BUTTON.w / 2.f, DISCOVER_BUTTON.y + DISCOVER_BUTTON.h / 2.f, 22.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(colour_id), "%s", text.c_str());
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();

    // this runs back from every popup the menu pushes, so the list is not
    // reloaded here: every path that writes an entry calls ReloadSaved itself.
    if (m_did_initial_setup) {
        return;
    }
    m_did_initial_setup = true;
    ReloadSaved();

    if (!GetPolledData().ip) {
        return;
    }

    // a single saved server needs no picking; the list stays one X press away.
    if (m_saved.size() == 1) {
        ConnectTo(m_saved[0]);
    }
}

void Menu::ReloadSaved() {
    sphaira::ownfoil::LoadConfigs(m_saved);
    RebuildCandidates();
}

void Menu::StartDiscovery() {
    if (m_discovering) {
        return;
    }

    if (!GetPolledData().ip) {
        return;
    }

    m_discovering = true;

    // owned by the menu rather than fired and forgotten: ~Async joins, and a
    // thread can't join itself.
    m_discovery = std::make_unique<utils::Async>([this](){
        // qualified from the root: this file's own namespace otherwise wins.
        auto found = sphaira::ownfoil::discovery::Discover(this->GetToken());

        evman::push(evman::CallbackEventData{
            [this, found = std::move(found)]() mutable {
                m_discovered = std::move(found);
                m_discovering = false;
                RebuildCandidates();
            },
            this->GetToken()
        }, false);
    });
}

void Menu::RebuildCandidates() {
    m_candidates.clear();

    for (const auto& config : m_saved) {
        Candidate candidate{};
        candidate.config = config;
        candidate.saved = true;

        // the case the handshake can't cover: a local-only shop whose ip moved is
        // unreachable, so no handshake ever happens to correct the entry.
        // discovery's fresher address gets the connect through, which saves it.
        if (const auto d = FindDiscovered(config.uid)) {
            candidate.version = d->version;
            candidate.is_public = d->is_public;
            if (!d->local.empty()) {
                candidate.config.local_address = d->local;
            }
            if (!d->remote.empty()) {
                candidate.config.remote_address = d->remote;
            }
        }

        m_candidates.emplace_back(std::move(candidate));
    }

    for (const auto& d : m_discovered) {
        // matched on the server-issued uid alone, so a hand-added entry still
        // holding a placeholder is listed a second time until its first connect.
        if (sphaira::ownfoil::FindByUid(m_saved, d.uid)) {
            continue;
        }

        Candidate candidate{};
        candidate.config.uid = d.uid;
        candidate.config.name = d.name;
        candidate.config.local_address = d.local;
        candidate.config.remote_address = d.remote;
        candidate.version = d.version;
        candidate.is_public = d.is_public;
        m_candidates.emplace_back(std::move(candidate));
    }

    if (m_index >= static_cast<s64>(m_candidates.size())) {
        m_index = 0;
    }

    // an empty list has nothing to focus, so the button is all there is.
    if (m_candidates.empty()) {
        m_focus_discover = true;
    }
}

auto Menu::FindDiscovered(const std::string& uid) const -> const sphaira::ownfoil::discovery::DiscoveredServer* {
    if (uid.empty()) {
        return nullptr;
    }

    const auto it = std::find_if(m_discovered.begin(), m_discovered.end(), [&](const auto& d) {
        return d.uid == uid;
    });
    return it == m_discovered.end() ? nullptr : &*it;
}

void Menu::OnServerSelected(s64 index) {
    if (index < 0 || index >= static_cast<s64>(m_candidates.size())) {
        return;
    }

    const auto candidate = m_candidates[index];
    const auto config = candidate.config;

    // already a saved location: connect straight away.
    if (candidate.saved) {
        ConnectTo(config);
        return;
    }

    // a discovered server not saved yet: say whether it needs a login before
    // asking whether to set one up.
    const auto message = candidate.is_public
        ? "This shop is public. Set up a login anyway?"_i18n
        : "This shop is private. Set up a login now?"_i18n;

    App::Push<OptionBox>(message, "No"_i18n, "Yes"_i18n, 1, [this, config](std::optional<s64> op_index) {
        if (op_index && *op_index) {
            App::Push<sphaira::ownfoil::OwnfoilForm>(config, [this](const sphaira::ownfoil::Config& saved) {
                ReloadSaved();
                ConnectTo(saved);
            });
        } else {
            // SaveConfig assigns the uid, so it needs a mutable copy.
            auto to_save = config;
            sphaira::ownfoil::SaveConfig(to_save);
            ReloadSaved();
            ConnectTo(to_save);
        }
    });
}

void Menu::ConnectTo(const sphaira::ownfoil::Config& config) {
    if (m_connecting) {
        return;
    }

    // opening a shop asks what it can add to this console, so that is what it
    // opens on rather than wherever the last one was left.
    m_category = 0;
    m_search.clear();
    m_installs_shown = g_installs;

    // read here rather than on the worker: option::Get() fills its cache on the
    // first call, which has no business happening off the main thread.
    const auto query = BuildQuery(0);

    // B cancels this connect alone, and its result is dropped with it. a
    // cancelled worker may still be unwinding when the next connect replaces it,
    // which joins it, but curl notices a stop within a progress tick.
    m_connect_stop = {};
    const auto token = m_connect_stop.get_token();

    m_connecting = true;
    m_connecting_name = config.name;

    // owned by the menu rather than fired and forgotten: ~Async joins, and a
    // thread can't join itself.
    m_connect_async = std::make_unique<utils::Async>([this, config, query, token](){
        auto connect_result = sphaira::ownfoil::api::Connect(config, token);
        auto connected_config = config;
        bool saved_changed{};
        std::vector<sphaira::ownfoil::api::ShopApp> fetched;
        std::string fetch_error;
        s64 total{};
        std::string base_url;

        if (connect_result.success) {
            // the server owns its identity and its name; empty values are
            // ignored, so a server reporting none can't erase what discovery
            // gave.
            const auto& info = connect_result.info;
            if (!info.uid.empty()) {
                connected_config.uid = info.uid;
            }
            if (!info.name.empty()) {
                connected_config.name = info.name;
            }
            // no local address: that is discovery's alone (RebuildCandidates).
            if (!info.remote_address.empty()) {
                connected_config.remote_address = info.remote_address;
            }
            // so the next launch goes straight to the address that just worked.
            connected_config.resolved_url = connect_result.base_url;

            // written here, on the worker: minIni rewrites the whole file once
            // per key, so saving an entry is half a dozen SD writes and they
            // have no business landing on the main thread as the grid appears.
            if (connected_config.uid != config.uid || connected_config.name != config.name
                || connected_config.remote_address != config.remote_address
                || connected_config.resolved_url != config.resolved_url) {
                // the old uid re-keys the entry: a placeholder promoted on first
                // contact, or an entry re-pointed at a different server.
                sphaira::ownfoil::SaveConfig(connected_config, config.uid);
                saved_changed = true;
            }

            // the first page rides the same worker, so the grid appears full.
            if (info.features.shop) {
                base_url = connect_result.base_url;
                // made here rather than in the constructor, which runs at startup
                // for every user whether or not they ever open this tab.
                fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);
                auto page_query = query;
                if (PrepareQuery(page_query, base_url, config, token, fetch_error)) {
                    sphaira::ownfoil::api::FetchApps(base_url, config, token, page_query, fetched, total, fetch_error);
                }
            }
        }

        evman::push(evman::CallbackEventData{
            [this, config, connect_result = std::move(connect_result), connected_config = std::move(connected_config),
                saved_changed, fetched = std::move(fetched), fetch_error = std::move(fetch_error), total, base_url = std::move(base_url)]() mutable {
                m_connecting = false;

                if (!connect_result.success) {
                    App::Notify(config.name + ": " + connect_result.error);
                    return;
                }

                const auto& info = connect_result.info;
                if (!info.motd.empty()) {
                    App::Notify(info.name + ": " + info.motd);
                }
                App::Notify(info.name + (connect_result.used_remote ? ": connected via remote address"_i18n : ": connected via local network"_i18n));

                // the entry on disk moved under the list, so pick the change up.
                if (saved_changed) {
                    ReloadSaved();
                }

                m_config = connected_config;

                // the handshake says what this caller may actually do; without
                // shop access there is no catalog to show.
                if (!info.features.shop) {
                    App::Notify(info.name + ": " + "This account has no shop access"_i18n);
                    return;
                }

                if (!fetch_error.empty()) {
                    App::Notify(info.name + ": " + fetch_error);
                    return;
                }

                m_base_url = base_url;
                SetMode(Mode::Home);
                ApplyPage(0, fetched, total);
            },
            token
        }, false);
    });
}

auto Menu::GetPageMax() -> s64 {
    const auto size = m_page_size.Get();
    if (m_total <= 0 || size <= 0) {
        return 1;
    }
    return (m_total + size - 1) / size;
}

auto Menu::BuildQuery(s64 page) -> sphaira::ownfoil::api::CatalogQuery {
    auto sort = m_sort.Get();
    if (sort < 0 || sort >= static_cast<s64>(std::size(SORT_MODES))) {
        sort = 0;
    }

    sphaira::ownfoil::api::CatalogQuery query{};
    // the shop pages from 1; m_page counts from 0 like every index here.
    query.page = page + 1;
    query.page_size = m_page_size.Get();
    query.order_field = SORT_MODES[sort].field;
    query.descending = m_sort_desc.Get();
    // the id lists this leaves empty are the worker's to fill, since the console
    // has to be scanned for them.
    query.category = CATEGORIES[m_category].category;
    query.search = m_search;
    return query;
}

auto Menu::PrepareQuery(sphaira::ownfoil::api::CatalogQuery& query, const std::string& base_url, const sphaira::ownfoil::Config& config, std::stop_token token, std::string& error) -> bool {
    using Category = sphaira::ownfoil::api::Category;

    // neither asks anything about this console, so the shop answers both alone.
    if (query.category == Category::All || query.category == Category::Search) {
        return true;
    }

    std::scoped_lock lock{m_installed_mutex};

    // the scan describes the console, so it outlives any one page: done once, and
    // again only when a category needs more of it, or an install changed it.
    // walking each title's content costs an ns query per title.
    const auto content = query.category != Category::NewGames;
    const auto installs = g_installs.load();
    const auto rescan = installs != m_installs_scanned;
    if (!m_installed_scanned || rescan || (content && !m_installed_content)) {
        m_installed = sphaira::ownfoil::installed::Scan(content);
        m_installed_scanned = true;
        m_installed_content = content;
        m_installs_scanned = installs;
    }

    switch (query.category) {
        case Category::All:
        case Category::Search:
            break;

        case Category::NewGames:
            query.app_ids = sphaira::ownfoil::installed::TitleIds(m_installed);
            break;

        case Category::Dlc:
            query.title_ids = sphaira::ownfoil::installed::TitleIds(m_installed);
            query.app_ids = m_installed.dlc_ids;
            break;

        case Category::Updates: {
            // a round trip whose answer is the same for every page, so it is
            // asked on page 1 - where the category is entered and a changed sort
            // lands - and after an install.
            if (query.page <= 1 || rescan) {
                m_outdated.clear();

                std::vector<sphaira::ownfoil::api::ShopUpdate> updates;
                if (!sphaira::ownfoil::api::FetchUpdates(base_url, config, token, sphaira::ownfoil::installed::TitleIds(m_installed), updates, error)) {
                    return false;
                }

                for (const auto& update : updates) {
                    if (update.version > sphaira::ownfoil::installed::UpdateVersionOf(m_installed, update.title_id)) {
                        m_outdated.emplace_back(update.app_id);
                    }
                }
            }

            query.app_ids = m_outdated;
        }   break;
    }

    return true;
}

void Menu::LoadPage(s64 page) {
    // the page on screen stays up until the next lands, with no modal: a fetch is
    // a few tens of milliseconds, and a dialog that appears and vanishes inside
    // that reads as slower than none. a request arriving meanwhile is held rather
    // than dropped, since `Order` can be flipped faster than the shop answers.
    if (m_loading) {
        m_pending_page = page;
        return;
    }

    // option::Get() fills its cache on first call, so the query is built here
    // rather than on the worker.
    const auto query = BuildQuery(page);
    const auto config = m_config;
    const auto base_url = m_base_url;
    // the page on screen owns the fetch that would replace it, so leaving it
    // abandons the request.
    const auto token = m_page_stop.get_token();

    m_loading = true;

    // owned by the menu rather than fired and forgotten: ~Async joins, and a
    // thread can't join itself.
    m_loader = std::make_unique<utils::Async>([this, config, base_url, query, page, token](){
        // the result travels with the event rather than through a member: no
        // dialog is holding the main thread off one this time.
        std::vector<sphaira::ownfoil::api::ShopApp> apps;
        s64 total{};
        std::string error;
        auto page_query = query;
        if (PrepareQuery(page_query, base_url, config, token, error)) {
            sphaira::ownfoil::api::FetchApps(base_url, config, token, page_query, apps, total, error);
        }

        evman::push(evman::CallbackEventData{
            [this, page, total, apps = std::move(apps), error = std::move(error)]() mutable {
                if (!error.empty()) {
                    m_loading = false;
                    App::Notify(m_config.name + ": " + error);
                } else {
                    // ApplyPage clears the flag by way of FreeEntries.
                    ApplyPage(page, apps, total);
                }

                // whatever was asked for mid-flight, issued now against the
                // options as they stand.
                if (m_pending_page) {
                    const auto next = *m_pending_page;
                    m_pending_page.reset();
                    LoadPage(next);
                }
            },
            token
        }, false);
    });
}

void Menu::ApplyPage(s64 page, std::vector<sphaira::ownfoil::api::ShopApp>& apps, s64 total) {
    m_page = page;
    m_total = total;

    FreeEntries();
    m_entries.reserve(apps.size());
    for (auto& app : apps) {
        Entry entry{};
        entry.app = std::move(app);
        m_entries.emplace_back(std::move(entry));
    }
    apps.clear();

    // a fresh page starts at the top.
    m_entry_index = 0;
    m_list->SetYoff(0);
    m_banner_name.Reset();
    m_banner_publisher.Reset();

    char subheading[64];
    std::snprintf(subheading, sizeof(subheading), "Page %ld / %ld"_i18n.c_str(),
        m_page + 1, GetPageMax());
    SetSubHeading(subheading);

    // the count belongs beside what it counts, in the title row.
    SetCatalogTitle();
}

void Menu::SetCatalogTitle() {
    // what is shown and how much of it, beside the shop's name. what orders it is
    // left to the sidebar: the row is bounded by the status text to its right,
    // and a third part is what tips it into scrolling.
    auto title = i18n::get(CATEGORIES[m_category].name);

    // "Search" alone says nothing about what is on screen, so the term shares the
    // same slot rather than taking another.
    if (CATEGORIES[m_category].category == sphaira::ownfoil::api::Category::Search && !m_search.empty()) {
        title += ": " + m_search;
    }

    // until a page answers, "0 titles" would read as a claim about the shop
    // rather than about the wait.
    if (m_total > 0) {
        char total[64];
        std::snprintf(total, sizeof(total), "%ld titles"_i18n.c_str(), m_total);
        title += SEPARATOR;
        title += total;
    }

    SetTitleSubHeading(title);
}

void Menu::ShowCategories() {
    // a panel down the left rather than an options entry: this is the one choice
    // that says what the whole screen is about, and is made often enough for a
    // button of its own.
    auto panel = std::make_unique<Sidebar>("Show"_i18n, i18n::get(CATEGORIES[m_category].name), Sidebar::Side::LEFT);
    ON_SCOPE_EXIT(App::Push(std::move(panel)));

    for (s64 i = 0; i < static_cast<s64>(std::size(CATEGORIES)); i++) {
        const auto& mode = CATEGORIES[i];
        panel->Add<SidebarEntryCallback>(i18n::get(mode.name), [this, i](){
            // Search says nothing on its own, so picking it always asks for a
            // term - re-picking it is how a different search is started.
            if (CATEGORIES[i].category == sphaira::ownfoil::api::Category::Search) {
                ShowSearch(i);
                return;
            }

            if (i == m_category) {
                return;
            }

            m_category = i;
            SetCatalogTitle();
            // a different catalog, so there is no page of the old one to stay on.
            LoadPage(0);
        }, true);
    }
}

void Menu::ShowSearch(s64 index) {
    std::string term;

    // the term on screen is the initial text: narrowing a search that came back
    // too wide is far more common than starting an unrelated one.
    if (R_FAILED(swkbd::ShowText(term, "Search"_i18n.c_str(), "Name or id"_i18n.c_str(), m_search.c_str(), 1, SEARCH_MAX)) || term.empty()) {
        // backing out of the keyboard leaves the catalog that was up, up.
        return;
    }

    // the same term against the same catalog would come back the same.
    if (index == m_category && term == m_search) {
        return;
    }

    m_search = term;
    m_category = index;
    SetCatalogTitle();
    LoadPage(0);
}

void Menu::ShowOptions() {
    auto options = std::make_unique<Sidebar>("Options"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    if (m_mode == Mode::ServerList) {
        options->Add<SidebarEntryCallback>("Add server"_i18n, [this](){
            App::Push<sphaira::ownfoil::OwnfoilForm>(sphaira::ownfoil::Config{}, [this](const auto&){
                ReloadSaved();
            });
        }, true, "Add a server."_i18n);

        // not on Y: MainMenu claims it for its menu picker while this is a tab.
        if (!m_focus_discover && m_index < static_cast<s64>(m_candidates.size())) {
            options->Add<SidebarEntryCallback>("Edit server"_i18n, [this](){
                EditSelected();
            }, true, "Change the selected server's name, addresses or login."_i18n);
        }

        if (!m_focus_discover && m_index < static_cast<s64>(m_candidates.size()) && m_candidates[m_index].saved) {
            options->Add<SidebarEntryCallback>("Delete server"_i18n, [this](){
                DeleteSelected();
            }, true, "Forget the selected server."_i18n);
        }
    } else {
        SidebarEntryArray::Items view_items;
        view_items.push_back("Icon"_i18n);
        view_items.push_back("Banner"_i18n);
        view_items.push_back("Detail"_i18n);

        options->Add<SidebarEntryArray>("View"_i18n, view_items, [this](s64& index_out){
            m_view.Set(index_out);
            OnViewChange();
        }, m_view.Get(), "Change titles view. Also cycled with -."_i18n);

        SidebarEntryArray::Items page_items;
        s64 page_index{};
        for (size_t i = 0; i < std::size(PAGE_SIZES); i++) {
            page_items.emplace_back(std::to_string(PAGE_SIZES[i]));
            if (PAGE_SIZES[i] == m_page_size.Get()) {
                page_index = static_cast<s64>(i);
            }
        }

        options->Add<SidebarEntryArray>("Per page"_i18n, page_items, [this](s64& index_out){
            const auto size = PAGE_SIZES[index_out];
            if (size == m_page_size.Get()) {
                return;
            }
            m_page_size.Set(size);
            // the page boundaries have moved, so there is no page to stay on.
            LoadPage(0);
        }, page_index, "How many titles displayed per page."_i18n);

        SidebarEntryArray::Items sort_items;
        for (const auto& mode : SORT_MODES) {
            sort_items.emplace_back(i18n::get(mode.name));
        }

        // the shop does the sorting, and a different ordering is a different
        // catalog, so either of these refetches from page 1.
        options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
            if (index_out == m_sort.Get()) {
                return;
            }
            m_sort.Set(index_out);
            LoadPage(0);
        }, m_sort.Get());

        // two states, so A flips it in place rather than opening a picker. stored
        // here, not through the OptionBool overload, which runs its callback
        // before the store and would refetch the old order.
        options->Add<SidebarEntryBool>("Order"_i18n, m_sort_desc.Get(), [this](bool& descending){
            m_sort_desc.Set(descending);
            LoadPage(0);
        }, "", "Descending"_i18n, "Ascending"_i18n);

        // last: it leaves the catalog, where everything above adjusts it.
        options->Add<SidebarEntryCallback>("Servers list"_i18n, [this](){
            RebuildCandidates();
            SetMode(Mode::ServerList);
        }, true, "Return to the server list."_i18n);
    }

    // on both lists: every shop's artwork shares the one cache.
    options->Add<SidebarEntryCallback>("Clear cache"_i18n, [](){
        App::Push<OptionBox>(
            "Remove all cached artwork from the SD card?"_i18n,
            "No"_i18n, "Yes"_i18n, 0, [](std::optional<s64> op_index) {
                if (!op_index || !*op_index) {
                    return;
                }

                // one delete however much is cached, but still an sd write not to
                // wait on from the main thread.
                App::Push<ProgressBox>(0, "Clear cache"_i18n, "Ownfoil"_i18n, [](ProgressBox*) -> Result {
                    const auto rc = fs::FsNativeSd().DeleteDirectoryRecursively(CACHE_PATH);
                    // nothing cached yet is already clear.
                    R_UNLESS(R_SUCCEEDED(rc) || rc == FsError_PathNotFound, rc);
                    R_SUCCEED();
                }, [](Result rc) {
                    if (R_SUCCEEDED(App::PushErrorBox(rc, "Failed to clear the cache."_i18n))) {
                        App::Notify("Cache cleared."_i18n);
                    }
                });
            }
        );
    }, true, "Remove all cached shop artwork from the SD card."_i18n);
}

void Menu::OpenSelected() {
    if (m_entry_index < 0 || m_entry_index >= static_cast<s64>(m_entries.size())) {
        return;
    }

    using AppType = sphaira::ownfoil::api::AppType;
    const auto& app = m_entries[m_entry_index].app;

    // an update has no page of its own, so its card opens its game's - which is
    // whose name, publisher and artwork the card already carries.
    TitlePage page{};
    page.dlc = app.type == AppType::Dlc;
    page.id = page.dlc ? app.app_id : app.title_id;
    page.game_id = app.title_id;
    page.name = app.name;
    page.publisher = app.publisher;
    page.game_name = app.game_name;
    page.banner = app.banner;
    page.icon = app.icon;
    page.download = app.download;

    App::Push<TitleMenu>(m_config, m_base_url, page);
}

void Menu::EditSelected() {
    // the discover button has focus, or there is nothing saved to edit yet.
    if (m_focus_discover || m_index >= static_cast<s64>(m_candidates.size())) {
        return;
    }

    // a discovered row that was never saved is edited as what it will become: the
    // form writes the entry, addresses and all, on its first save.
    App::Push<sphaira::ownfoil::OwnfoilForm>(m_candidates[m_index].config, [this](const auto&){
        ReloadSaved();
    });
}

void Menu::DeleteSelected() {
    if (m_focus_discover || m_index >= static_cast<s64>(m_candidates.size())) {
        return;
    }

    const auto& candidate = m_candidates[m_index];
    if (!candidate.saved) {
        return;
    }

    const auto uid = candidate.config.uid;
    App::Push<OptionBox>(
        i18n::Reorder("Delete ", candidate.config.name) + '?',
        "No"_i18n, "Yes"_i18n, 0, [this, uid](std::optional<s64> op_index) {
            if (op_index && *op_index) {
                sphaira::ownfoil::DeleteConfig(uid);
                ReloadSaved();
            }
        }
    );
}

void SignalInstalled() {
    g_installs++;
}

} // namespace sphaira::ui::menu::ownfoil

#include "ui/menus/ownfoil_title.hpp"
#include "ui/menus/ownfoil.hpp"
#include "ui/nvg_util.hpp"
#include "ui/sidebar.hpp"
#include "ui/popup_multi_select.hpp"
#include "ui/progress_box.hpp"
#include "yati/yati.hpp"
#include "yati/source/http.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "image.hpp"
#include "download.hpp"
#include "evman.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <utility>

namespace sphaira::ui::menu::ownfoil {
namespace {

namespace api = sphaira::ownfoil::api;
namespace installed = sphaira::ownfoil::installed;

// the left column is a page of its own. unscrolled its top is drawn at PAGE_Y;
// scrolled, it runs up under the heading's line and down to the bottom bar's.
constexpr float PAGE_X = 54.f;
constexpr float PAGE_Y = 104.f;
constexpr float PAGE_W = 720.f;
constexpr float CLIP_Y = 87.f;
constexpr float CLIP_BOTTOM = 646.f;
constexpr float VIEW_H = CLIP_BOTTOM - PAGE_Y;
// a highlight's outline is drawn outside what it surrounds.
constexpr float CLIP_BLEED = 10.f;
// below the last thing on the page, so it doesn't end flush with the bar.
constexpr float PAGE_PAD = 24.f;

// a little under the width of the old image, which leaves room below the
// thumbnails for the tagline to show: that is what says the page goes on.
constexpr float IMAGE_H = 405.f;
constexpr float IMAGE_ROUNDING = 6.f;
constexpr float ICON_SIZE = 256.f;

constexpr float THUMB_GAP_Y = 16.f;
constexpr float THUMB_W = 94.f;
constexpr float THUMB_H = 53.f;
constexpr float THUMB_GAP = 10.f;
// as many as fit under the big image.
constexpr s64 THUMB_COUNT = 7;

constexpr float SECTION_GAP = 30.f;
constexpr float INTRO_SIZE = 24.f;
constexpr float INTRO_LINE = 32.f;
constexpr float INTRO_GAP = 12.f;
constexpr float TEXT_SIZE = 19.f;
constexpr float TEXT_LINE = 30.f;

constexpr float HEADING_SIZE = 24.f;
constexpr float HEADING_H = 44.f;
constexpr float DLC_ROW_H = 92.f;
constexpr float DLC_IMAGE_W = 128.f;
constexpr float DLC_IMAGE_H = 72.f;

// a press moves the page a third of a screen; held, the right stick moves it a
// step every frame.
constexpr float SCROLL_STEP = 180.f;
constexpr float STICK_STEP = 10.f;
// how much of the way to its target the page glides each frame: the stick moves
// the target every frame, a press moves it once and glides there more slowly.
constexpr float STICK_EASE = 0.3f;
constexpr float STEP_EASE = 0.12f;
constexpr float SCROLLBAR_X = 788.f;
constexpr float SCROLLBAR_H = 526.f;

constexpr float COLUMN_X = 806.f;
constexpr float COLUMN_Y = 104.f;
constexpr float COLUMN_W = 420.f;
// short enough that a two-line name and all eight rows fit, one of them a value
// wrapped onto a second line.
constexpr float ROW_H = 38.f;
constexpr float VALUE_SIZE = 18.f;
constexpr float VALUE_LINE = 22.f;
constexpr float GROUP_GAP = 18.f;
// room for the longest label, "Available version"; the value gets the rest.
constexpr float LABEL_W = 150.f;

// the regions whose ratings are pegi ages. anywhere else titledb's number is
// shown as a bare age: naming the wrong board would be worse than naming none.
constexpr const char* PEGI_REGIONS[] = {
    "AT", "BE", "CH", "CZ", "DE", "DK", "ES", "FI", "FR", "GB",
    "GR", "HU", "IE", "IT", "NL", "NO", "PL", "PT", "SE", "SK",
};

constexpr const char* MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

// the catalogue spells a release date yyyymmdd, and the version history spells
// one yyyy-mm-dd. either reads as "7 Aug 2018"; anything else is shown as sent.
auto FormatDate(const std::string& date) -> std::string {
    std::string digits;
    for (const auto c : date) {
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (c != '-') {
            return date;
        }
    }

    if (digits.size() != 8) {
        return date;
    }

    const auto year = std::atoi(digits.substr(0, 4).c_str());
    const auto month = std::atoi(digits.substr(4, 2).c_str());
    const auto day = std::atoi(digits.substr(6, 2).c_str());
    if (month < 1 || month > 12) {
        return date;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d %s %d", day, i18n::get(MONTHS[month - 1]).c_str(), year);
    return buf;
}

// what the game calls itself with the patch level beside it, the version over
// 65536 as ownfoil and the meta menu both spell it. `name` is empty for a version
// the shop doesn't hold, which has no string anywhere.
auto FormatVersion(const std::string& name, s64 version) -> std::string {
    char level[24];
    std::snprintf(level, sizeof(level), "v%ld", version >> 16);
    return name.empty() ? std::string{level} : name + " (" + level + ")";
}

// the base game by name, being the one version with no patch level to tell it
// apart, and every update the way the version rows beside it are written.
auto FormatInstallVersion(const api::ShopVersion& version) -> std::string {
    if (version.version > 0) {
        return FormatVersion(version.display, version.version);
    }

    const auto base = "Base"_i18n;
    return version.display.empty() ? base : base + " (" + version.display + ")";
}

auto FormatRating(const std::string& rating, const std::string& region) -> std::string {
    if (rating.empty()) {
        return {};
    }

    for (const auto pegi : PEGI_REGIONS) {
        if (region == pegi) {
            return "PEGI " + rating;
        }
    }

    return rating + "+";
}

// a dlc's name usually leads with its game's, which the page already says, so
// "Dead Cells: The Bad Seed" is listed as "The Bad Seed".
auto StripGameName(const std::string& name, const std::string& game) -> std::string {
    if (game.empty() || name.size() <= game.size() || name.compare(0, game.size(), game) != 0) {
        return name;
    }

    constexpr std::string_view en_dash{"–"};
    std::string_view rest{name};
    rest.remove_prefix(game.size());
    const auto joined = rest.size();

    while (!rest.empty()) {
        if (rest.front() == ' ' || rest.front() == ':' || rest.front() == '-') {
            rest.remove_prefix(1);
        } else if (rest.starts_with(en_dash)) {
            rest.remove_prefix(en_dash.size());
        } else {
            break;
        }
    }

    // nothing joined them, so the game's name was only the start of a word.
    if (rest.empty() || rest.size() == joined) {
        return name;
    }
    return std::string{rest};
}

// the shop spells an id as 16 hex digits, and yati compares the number.
auto ParseId(const std::string& id) -> u64 {
    return std::strtoull(id.c_str(), nullptr, 16);
}

auto IsSpace(char c) -> bool {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

// the catalogue breaks a tagline wherever the eshop's narrow box wanted it and
// pads it with spaces; as a heading or a row it reads as one run of text.
auto Flatten(const std::string& text) -> std::string {
    std::string out;
    for (const auto c : text) {
        if (!IsSpace(c)) {
            out += c;
        } else if (!out.empty() && out.back() != ' ') {
            out += ' ';
        }
    }

    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// runs on the page's worker, so it decodes with stb rather than the hardware jpeg
// decoder: that is one instance shared with the main thread, and unguarded.
auto FetchImage(const sphaira::ownfoil::Config& config, const std::string& id, const api::ShopImage& image, const char* kind, std::stop_token token) -> ImageResult {
    if (image.url.empty()) {
        return {};
    }

    const auto path = BuildImageCache(id, image.url, kind);

    // the cache is keyed on the url, and a url names its bytes, so whatever is
    // already there is the image - unless it is a download that never finished.
    if (fs::FsNativeSd().FileExists(path)) {
        auto cached = ImageLoadFromFile(path, ImageFlag_None);
        if (!cached.data.empty()) {
            return cached;
        }
    }

    // the shop gates its own copies like its catalogue; a hotlink points at a cdn
    // that must not get the shop's credentials.
    const auto result = curl::Api().ToFile(
        curl::Url{image.url},
        curl::Path{path},
        curl::UserPass{image.local ? config.user : "", image.local ? config.pass : ""},
        curl::PreemptiveAuth{image.local},
        curl::StopToken{token}
    );

    if (!result.success) {
        return {};
    }

    return ImageLoadFromFile(path, ImageFlag_None);
}

auto CreateTexture(const ImageResult& image) -> int {
    if (image.data.empty()) {
        return 0;
    }
    return nvgCreateImageRGBA(App::GetVg(), image.w, image.h, 0, image.data.data());
}

void DeleteTexture(int image) {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

} // namespace

auto BuildImageCache(const std::string& app_id, const std::string& url, const char* kind) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s_%s_%016lx.jpg", CACHE_PATH, app_id.c_str(), kind, std::hash<std::string>{}(url));
    return path;
}

TitleMenu::TitleMenu(const sphaira::ownfoil::Config& config, const std::string& base_url, const TitlePage& page)
: MenuBase{config.name, MenuFlag_None}
, m_config{config}
, m_base_url{base_url}
, m_page{page} {
    // the heading row names the shop, as the catalog's does. the title's own
    // name heads the right column instead: a long one scrolled in the heading.
    SetAction(Button::B, Action{"Back"_i18n, [this]{
        SetPop();
    }});

    Layout();

    // pushed out of sight: the catalog stays on screen until the page can open
    // whole, which Update decides.
    SetHidden(true);

    const auto token = m_stop.get_token();
    // everything the page's threads find is only put on the page from the main
    // thread, which is the one nanovg belongs to.
    const auto send = [token](std::function<void()>&& callback) {
        evman::push(evman::CallbackEventData{std::move(callback), token}, false);
    };
    // the log says how long after opening each part landed.
    const auto opened = m_opened;
    // a card with no banner opens a page with none either, which opens on the
    // card's icon instead.
    const auto icon_for_banner = m_page.banner.url.empty();

    // owned by the page rather than fired and forgotten: ~Async joins, and a
    // thread can't join itself.
    m_loader = std::make_unique<utils::Async>([this, token, send, opened, icon_for_banner](){
        api::ShopTitle title{};
        std::string error;
        if (!api::FetchTitle(m_base_url, m_config, token, m_page.id, title, error)) {
            if (!token.stop_requested()) {
                send([this, error]() {
                    App::Notify(m_page.name + ": " + error);
                    // nothing more is coming from the shop for the page to wait on.
                    m_failed = true;
                    m_artwork_done = true;
                });
            }
            return;
        }
        log_write("[OWNFOIL] shop answered for %s after %zums\n", m_page.id.c_str(), opened.GetMs());

        // the worker keeps its own copy of what it still has to fetch, since the
        // title itself moves to the main thread. the page's own banner is CLIENT,
        // the box its big image is drawn at; a card's THUMB only stands in.
        const auto banner = title.banner.url.empty() ? m_page.banner : title.banner;
        const auto screenshots = title.screenshots;
        const auto dlc = title.dlc;

        send([this, title = std::move(title)]() mutable {
            m_title = std::move(title);
            m_screenshots.resize(m_title.screenshots.size());

            // blank lines after the text would only lengthen the page.
            auto& description = m_title.description;
            while (!description.empty() && IsSpace(description.back())) {
                description.pop_back();
            }
            m_intro = Flatten(m_title.intro);

            m_rows.clear();
            for (const auto& content : m_title.dlc) {
                Row row{};
                row.name = StripGameName(content.name, GetName());
                row.tagline = Flatten(content.intro);
                m_rows.emplace_back(std::move(row));
            }
            MarkInstalled();

            // the newest: what a shop is opened to be asked for.
            m_install_version = std::max<s64>(0, static_cast<s64>(m_title.versions.size()) - 1);

            m_loaded = true;
            BuildFacts();
            SetImageIndex(m_image_index);
            Layout();
        });

        // ahead of the screenshots rather than beside them: fetched with them, an
        // uncached banner took ~150ms to download and ~107ms to decode, twice what
        // it takes alone. sent even when nothing was read, since the page waits.
        if (!banner.url.empty()) {
            auto image = FetchImage(m_config, m_page.id, banner, "banner", token);
            if (!image.data.empty()) {
                log_write("[OWNFOIL] banner for %s decoded after %zums\n", m_page.id.c_str(), opened.GetMs());
            }
            send([this, image = std::move(image)]() {
                if (!image.data.empty()) {
                    m_banner = CreateTexture(image);
                }
                m_artwork_done = true;
            });
        }

        // in order, so the thumbnails fill in left to right. these queue behind
        // the title above, so the slot each one lands in already exists.
        for (size_t i = 0; i < screenshots.size() && !token.stop_requested(); i++) {
            auto image = FetchImage(m_config, m_page.id, screenshots[i], "screenshot", token);
            if (image.data.empty()) {
                continue;
            }

            send([this, i, image = std::move(image)]() {
                if (i < m_screenshots.size() && !m_screenshots[i]) {
                    m_screenshots[i] = CreateTexture(image);
                }
            });
        }

        // last, since the rows are the furthest down the page. cached under the
        // same name a dlc card uses, so one the catalog has shown is read back.
        for (size_t i = 0; i < dlc.size() && !token.stop_requested(); i++) {
            auto image = FetchImage(m_config, dlc[i].app_id, dlc[i].banner, "banner", token);
            if (image.data.empty()) {
                continue;
            }

            send([this, i, image = std::move(image)]() {
                if (i < m_rows.size() && !m_rows[i].image) {
                    m_rows[i].image = CreateTexture(image);
                }
            });
        }
    });

    // the installed lookup mounts the game's control nca, so it runs beside the
    // shop's request rather than after it. the card's icon follows it here.
    m_console = std::make_unique<utils::Async>([this, token, send, icon_for_banner](){
        LoadInstalled(token);

        // a page with no banner opens on the card's icon instead, which the grid
        // has usually fetched. sent even when nothing was read, as above.
        if (icon_for_banner) {
            auto icon = FetchImage(m_config, m_page.id, m_page.icon, "icon", token);
            send([this, icon = std::move(icon)]() {
                if (!icon.data.empty()) {
                    m_icon = CreateTexture(icon);
                }
                m_artwork_done = true;
            });
            return;
        }

        // a dlc names its game beside this icon, which the page doesn't wait for:
        // it can be a download, and on the console one held a page to OPEN_WAIT_MS.
        if (m_page.dlc) {
            if (auto image = FetchImage(m_config, m_page.id, m_page.icon, "icon", token); !image.data.empty()) {
                send([this, image = std::move(image)]() {
                    m_icon = CreateTexture(image);
                });
            }
        }
    });
}

TitleMenu::~TitleMenu() {
    // abandon whatever the threads are still waiting on, then wait for them -
    // before the textures they were filling in go.
    m_stop.request_stop();
    m_loader.reset();
    m_console.reset();

    DeleteTexture(m_banner);
    DeleteTexture(m_icon);
    for (const auto image : m_screenshots) {
        DeleteTexture(image);
    }
    for (const auto& row : m_rows) {
        DeleteTexture(row.image);
    }
}

void TitleMenu::Update(Controller* controller, TouchInfo* touch) {
    // the page opens whole: hidden until the shop, the console and its artwork have
    // answered, or OPEN_WAIT_MS has passed. presses meanwhile are swallowed.
    if (IsHidden()) {
        const auto ready = (m_loaded || m_failed) && m_console_loaded && m_artwork_done;
        if (!ready && m_opened.GetMs() < OPEN_WAIT_MS) {
            return;
        }

        log_write("[OWNFOIL] page for %s shown after %zums%s\n", m_page.id.c_str(), m_opened.GetMs(), ready ? "" : " (waited out)");
        SetHidden(false);
    }

    MenuBase::Update(controller, touch);

    // the right stick moves the page freely. the d-pad and left stick move it a
    // part at a time, onto the rows, and - while the images are up - through them.
    if (controller->GotHeld(Button::RS_DOWN)) {
        m_stick_scroll = true;
        ScrollTo(m_scroll_target + STICK_STEP);
    } else if (controller->GotHeld(Button::RS_UP)) {
        m_stick_scroll = true;
        ScrollTo(m_scroll_target - STICK_STEP);
    } else if (controller->GotDown(Button::DPAD_DOWN | Button::LS_DOWN)) {
        StepDown();
    } else if (controller->GotDown(Button::DPAD_UP | Button::LS_UP)) {
        StepUp();
    } else if (m_focus < 0 && ImagesInView() && controller->GotDown(Button::DPAD_LEFT | Button::LS_LEFT)) {
        if (m_image_index > 0) {
            App::PlaySoundEffect(SoundEffect::Scroll);
            SetImageIndex(m_image_index - 1);
        }
    } else if (m_focus < 0 && ImagesInView() && controller->GotDown(Button::DPAD_RIGHT | Button::LS_RIGHT)) {
        if (m_image_index + 1 < GetImageCount()) {
            App::PlaySoundEffect(SoundEffect::Scroll);
            SetImageIndex(m_image_index + 1);
        }
    } else if (touch->is_scroll && (m_dragging || (touch->initial.x >= PAGE_X && touch->initial.x <= PAGE_X + PAGE_W && touch->initial.y >= CLIP_Y && touch->initial.y <= CLIP_BOTTOM))) {
        // a drag that starts on the page moves it with the finger, the way a
        // list's does: from where it was when the drag began, and without a glide.
        if (!m_dragging) {
            m_dragging = true;
            m_drag_from = m_scroll_target;
        }
        ScrollTo(m_drag_from + static_cast<float>(touch->initial.y) - static_cast<float>(touch->cur.y));
        m_scroll = m_scroll_target;
    } else if (touch->is_clicked && touch->in_range(Vec4{PAGE_X, CLIP_Y, PAGE_W, CLIP_BOTTOM - CLIP_Y})) {
        // a tap on the highlighted row opens it, like A does.
        for (s64 i = 0; i < static_cast<s64>(m_rows.size()); i++) {
            if (touch->in_range(GetRowRect(i))) {
                if (i == m_focus) {
                    OpenDlc(i);
                } else {
                    SetFocus(i);
                }
                break;
            }
        }
    }

    if (!touch->is_scroll) {
        m_dragging = false;
    }

    // a row the stick or a drag has scrolled away is no longer the one A acts on.
    if (m_focus >= 0 && !RowInView(m_focus)) {
        SetFocus(-1);
    }

    if (m_scroll != m_scroll_target) {
        m_scroll += (m_scroll_target - m_scroll) * (m_stick_scroll ? STICK_EASE : STEP_EASE);
        if (std::abs(m_scroll_target - m_scroll) < 1.f) {
            m_scroll = m_scroll_target;
        }
    }

    UpdateActions();
}

auto TitleMenu::GetBanner() const -> const api::ShopImage& {
    return m_title.banner.url.empty() ? m_page.banner : m_title.banner;
}

auto TitleMenu::GetName() const -> const std::string& {
    return m_title.name.empty() ? m_page.name : m_title.name;
}

auto TitleMenu::GetPublisher() const -> const std::string& {
    return m_title.publisher.empty() ? m_page.publisher : m_title.publisher;
}

auto TitleMenu::GetImageCount() const -> s64 {
    return (GetBanner().url.empty() ? 0 : 1) + static_cast<s64>(m_screenshots.size());
}

auto TitleMenu::GetImage(s64 index) const -> int {
    if (!GetBanner().url.empty()) {
        if (index == 0) {
            return m_banner;
        }
        index--;
    }

    if (index < 0 || index >= static_cast<s64>(m_screenshots.size())) {
        return 0;
    }
    return m_screenshots[index];
}

void TitleMenu::SetImageIndex(s64 index) {
    m_image_index = std::clamp<s64>(index, 0, std::max<s64>(0, GetImageCount() - 1));
}

void TitleMenu::BuildFacts() {
    m_facts.clear();

    // a row the shop has nothing for is left out rather than drawn empty.
    const auto add = [this](const char* label, const std::string& value) {
        if (!value.empty()) {
            m_facts.emplace_back(Fact{i18n::get(label), value});
        }
    };

    const auto& t = m_title;

    if (!t.release_date.empty()) {
        add("Release date", FormatDate(t.release_date));
    }
    add("Genre", t.genre);
    add("Players", t.players);
    add("Age rating", FormatRating(t.rating, t.region));

    // the catalogue gives every dlc the same placeholder size, so a dlc page
    // has no size worth drawing; a game it doesn't size is given its file's.
    if (!m_page.dlc) {
        if (t.size > 0) {
            add("Required space", utils::formatSizeStorage(t.size));
        } else if (t.download_size > 0) {
            add("Download size", utils::formatSizeStorage(t.download_size));
        }
    }

    // what the console holds opens the group, ahead of what there is to get. it can
    // answer after the shop, so until it has the row holds its place empty rather
    // than moving the rows under it. a dlc has no version string, only a yes or no.
    std::string held{};
    if (m_console_loaded && m_page.dlc) {
        held = m_dlc_installed ? "Yes"_i18n : std::string{"-"};
    } else if (m_console_loaded) {
        // installed but unreadable is still installed: the patch level stands
        // in for the string rather than the row claiming nothing is there.
        held = "-";
        if (m_installed.installed) {
            held = m_installed.display.empty() ? FormatVersion({}, m_installed.version) : m_installed.display;
        }
    }
    m_facts.emplace_back(Fact{i18n::get(m_page.dlc ? "Installed" : "Installed version"), std::move(held), !m_facts.empty()});

    // the shop holds no base game under a dlc's id, so the rest are a game's alone.
    if (t.available_version >= 0) {
        const auto available = FormatVersion(t.available_display, t.available_version);

        // when the shop is behind, the newest version has no string of its own,
        // so its release date stands in for one.
        auto latest = available;
        if (t.latest_version > t.available_version) {
            latest = FormatVersion(FormatDate(t.latest_date), t.latest_version);
        }

        add("Available version", available);
        add("Latest version", latest);
    }
}

void TitleMenu::LoadInstalled(std::stop_token token) {
    installed::InstalledVersion version{};
    std::vector<std::string> installed_dlc;
    bool dlc_installed{};
    if (m_page.dlc) {
        dlc_installed = installed::HasDlc(m_page.game_id, m_page.id);
    } else {
        version = installed::InstalledVersionOf(m_page.game_id);
        // asked before the shop has said whether there is any dlc: one ns
        // query is cheaper than waiting to find out.
        installed_dlc = installed::InstalledDlcIds(m_page.game_id);
    }
    log_write("[OWNFOIL] console answered for %s after %zums\n", m_page.id.c_str(), m_opened.GetMs());

    evman::push(evman::CallbackEventData{[this, version = std::move(version), installed_dlc = std::move(installed_dlc), dlc_installed]() mutable {
        m_installed = std::move(version);
        m_installed_dlc = std::move(installed_dlc);
        m_dlc_installed = dlc_installed;
        m_console_loaded = true;
        MarkInstalled();

        // until the shop answers there are no facts for it to join.
        if (m_loaded) {
            BuildFacts();
        }
    }, token}, false);
}

void TitleMenu::MarkInstalled() {
    // the rows are built one for one from the title's dlc.
    for (size_t i = 0; i < m_rows.size() && i < m_title.dlc.size(); i++) {
        const auto& id = m_title.dlc[i].app_id;
        m_rows[i].installed = std::find(m_installed_dlc.begin(), m_installed_dlc.end(), id) != m_installed_dlc.end();
    }
}

void TitleMenu::Layout() {
    const auto vg = App::GetVg();
    const auto measure = [vg](const std::string& text, float size, float line) -> float {
        if (text.empty()) {
            return 0;
        }

        nvgSave(vg);
        nvgFontSize(vg, size);
        nvgTextLineHeight(vg, line / size);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        float bounds[4]{};
        nvgTextBoxBounds(vg, 0, 0, PAGE_W, text.c_str(), nullptr, bounds);
        nvgRestore(vg);
        return bounds[3] - bounds[1];
    };

    auto y = GetImagesBottom();

    // the tagline heads the description, in place of a heading of its own.
    m_intro_h = measure(m_intro, INTRO_SIZE, INTRO_LINE);
    m_desc_h = measure(m_title.description, TEXT_SIZE, TEXT_LINE);
    if (m_intro_h > 0 || m_desc_h > 0) {
        m_text_y = y + SECTION_GAP;
        m_desc_y = m_text_y + m_intro_h + (m_intro_h > 0 && m_desc_h > 0 ? INTRO_GAP : 0);
        y = m_desc_y + m_desc_h;
    }

    if (!m_rows.empty()) {
        m_rows_y = y + SECTION_GAP;
        y = m_rows_y + HEADING_H + m_rows.size() * DLC_ROW_H;
    }

    m_content_h = y;
    ScrollTo(m_scroll_target);
    m_scroll = std::min(m_scroll, m_scroll_target);
}

auto TitleMenu::GetImagesBottom() const -> float {
    return GetImageCount() > 1 ? IMAGE_H + THUMB_GAP_Y + THUMB_H : IMAGE_H;
}

auto TitleMenu::GetMaxScroll() const -> float {
    return std::max(0.f, m_content_h + PAGE_PAD - VIEW_H);
}

void TitleMenu::ScrollTo(float y) {
    m_scroll_target = std::clamp(y, 0.f, GetMaxScroll());
}

auto TitleMenu::ImagesInView() const -> bool {
    return m_scroll_target < IMAGE_H / 2.f;
}

auto TitleMenu::GetRowTop(s64 index) const -> float {
    return m_rows_y + HEADING_H + index * DLC_ROW_H;
}

auto TitleMenu::RowInView(s64 index) const -> bool {
    const auto top = GetRowTop(index);
    return top >= m_scroll_target && top + DLC_ROW_H <= m_scroll_target + VIEW_H;
}

auto TitleMenu::FirstRowInView() const -> s64 {
    for (s64 i = 0; i < static_cast<s64>(m_rows.size()); i++) {
        if (RowInView(i)) {
            return i;
        }
    }
    return -1;
}

auto TitleMenu::GetRowRect(s64 index) const -> Vec4 {
    return Vec4{PAGE_X, PAGE_Y - m_scroll + GetRowTop(index), PAGE_W, DLC_ROW_H};
}

void TitleMenu::StepDown() {
    if (m_focus >= 0) {
        if (m_focus + 1 < static_cast<s64>(m_rows.size())) {
            App::PlaySoundEffect(SoundEffect::Scroll);
            SetFocus(m_focus + 1);
        }
        return;
    }

    // the rows take the highlight as soon as one is wholly on screen - the first
    // of them, wherever the stick left the page. at the bottom of the page the
    // last row always is.
    if (const auto first = FirstRowInView(); first >= 0) {
        App::PlaySoundEffect(SoundEffect::Scroll);
        SetFocus(first);
        return;
    }

    // a step at a time, until the page can go no further.
    const auto target = std::min(m_scroll_target + SCROLL_STEP, GetMaxScroll());
    if (target > m_scroll_target) {
        App::PlaySoundEffect(SoundEffect::Scroll);
        m_stick_scroll = false;
        ScrollTo(target);
    }
}

void TitleMenu::StepUp() {
    if (m_focus > 0) {
        App::PlaySoundEffect(SoundEffect::Scroll);
        SetFocus(m_focus - 1);
        return;
    }

    // up off the first row is back to the text, which is already on screen.
    if (m_focus == 0) {
        App::PlaySoundEffect(SoundEffect::Scroll);
        SetFocus(-1);
        return;
    }

    // the stick left the page past the first row, so up takes the highlight at the
    // first on screen and walks up from there.
    if (const auto first = FirstRowInView(); first > 0) {
        App::PlaySoundEffect(SoundEffect::Scroll);
        SetFocus(first);
        return;
    }

    if (m_scroll_target <= 0) {
        return;
    }

    App::PlaySoundEffect(SoundEffect::Scroll);
    m_stick_scroll = false;
    ScrollTo(m_scroll_target - SCROLL_STEP);
}

void TitleMenu::SetFocus(s64 index) {
    m_focus = index;
    if (index < 0) {
        return;
    }

    // the first row brings its heading on screen with it.
    const auto top = index == 0 ? m_rows_y : GetRowTop(index);
    const auto bottom = GetRowTop(index) + DLC_ROW_H;
    m_stick_scroll = false;
    // a row below comes up with the page's padding under it: flush with the bar,
    // its outline would be cut off.
    if (top < m_scroll_target) {
        ScrollTo(top);
    } else if (bottom + PAGE_PAD > m_scroll_target + VIEW_H) {
        ScrollTo(bottom + PAGE_PAD - VIEW_H);
    }
}

void TitleMenu::UpdateActions() {
    const auto focused = m_focus >= 0;

    // A follows what is on screen: a highlighted row opens that dlc's page, and
    // anything else the install menu, which waits on the shop's answer.
    const auto action = focused ? MainAction::View : (m_loaded ? MainAction::Install : MainAction::None);
    if (action != m_action) {
        m_action = action;
        switch (action) {
            case MainAction::None:
                RemoveAction(Button::A);
                break;

            case MainAction::Install:
                SetAction(Button::A, Action{"Install"_i18n, [this]{
                    ShowInstallOptions();
                }});
                break;

            case MainAction::View:
                SetAction(Button::A, Action{"Open"_i18n, [this]{
                    OpenDlc(m_focus);
                }});
                break;
        }
    }

    // the viewer needs the full-size copies, which only the shop's answer names.
    const auto images = m_loaded && GetImageCount() > 0 && ImagesInView() && !focused;
    if (images != HasAction(Button::Y)) {
        if (images) {
            SetAction(Button::Y, Action{"Full screen"_i18n, [this]{
                OpenViewer();
            }});
        } else {
            RemoveAction(Button::Y);
        }
    }

    // the counter is for whatever left and right, or up and down, step through.
    char counter[32]{};
    if (focused && m_rows.size() > 1) {
        std::snprintf(counter, sizeof(counter), "%ld / %zu", m_focus + 1, m_rows.size());
    } else if (!focused && ImagesInView() && GetImageCount() > 1) {
        std::snprintf(counter, sizeof(counter), "%ld / %ld", m_image_index + 1, GetImageCount());
    }

    if (m_counter != counter) {
        m_counter = counter;
        SetSubHeading(m_counter);
    }
}

void TitleMenu::ShowInstallOptions() {
    auto options = std::make_unique<Sidebar>("Install"_i18n, GetName(), Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    options->Add<SidebarEntryCallback>("Install options"_i18n, [](){
        App::DisplayInstallOptions(false);
    });

    // Install names what it would do and the rows above change that, but they are
    // added first, so they reach it through this, filled in once it exists.
    const auto install = std::make_shared<SidebarEntryCallback*>();
    const auto relabel = [this, install](){
        if (*install) {
            (*install)->SetTitle(GetInstallLabel());
        }
    };

    // the shop answers with no apps at all under a dlc's own id, so a dlc's page
    // has nothing to choose between: it installs the one thing the shop holds.
    if (!m_title.versions.empty()) {
        SidebarEntryArray::Items items;
        for (const auto& version : m_title.versions) {
            auto label = FormatInstallVersion(version);
            // the one the console holds: picking it installs nothing.
            if (m_console_loaded && m_installed.installed && version.version == m_installed.version) {
                label += " • " + "Installed"_i18n;
            }
            items.emplace_back(std::move(label));
        }

        const auto picker = options->Add<SidebarEntryArray>("Version"_i18n, items, [this, relabel](s64& index){
            m_install_version = index;
            relabel();
        }, m_install_version);

        // asked as the list is drawn, so turning Allow downgrade on under Install
        // options lights the older updates up the next time the list is opened.
        picker->SetDisabled([this](s64 index){
            return index >= 0 && index < static_cast<s64>(m_title.versions.size()) && !IsVersionOffered(m_title.versions[index].version);
        });
    }

    // every add-on the console hasn't got starts ticked, which is what a shop is
    // opened for; after that they are the user's to choose. the console can answer
    // after the menu was first opened, so the installed half is cleared each time
    // and only the defaults are settled once.
    if (!m_rows.empty()) {
        const auto first = m_install_dlc.size() != m_rows.size();
        m_install_dlc.resize(m_rows.size());
        for (size_t i = 0; i < m_rows.size(); i++) {
            if (m_rows[i].installed) {
                m_install_dlc[i] = 0;
            } else if (first) {
                m_install_dlc[i] = 1;
            }
        }

        const auto addons = options->Add<SidebarEntryTextBase>("Add-on content"_i18n, GetAddonCount(), SidebarEntryTextBase::Callback{});
        addons->SetCallback([this, addons, relabel](){
            ShowAddons([this, addons, relabel](){
                addons->SetValue(GetAddonCount());
                relabel();
            });
        });
    }

    *install = options->Add<SidebarEntryCallback>(GetInstallLabel(), [this](){
        Install();
    }, "Download and install selection."_i18n);

    // asked as the menu draws, so the version, the add-ons and Allow downgrade all
    // move it. the reason is set once, so it holds for every version it greys on.
    (*install)->Depends([this](){
        return HasSomethingToInstall();
    }, m_page.dlc
        ? "This add-on is already installed."_i18n
        : "Nothing to install: the console already has this version or a newer one, and no add-on is ticked."_i18n);

    // the menu opens on Install, which is what it is opened for: everything
    // above it only qualifies that press.
    options->SetDefaultEntry(*install);
}

void TitleMenu::Install() {
    if (!App::GetInstallEnable()) {
        App::ShowEnableInstallPrompt();
        return;
    }

    // one file can carry several of the chosen apps, so they are gathered by
    // download: each file is fetched once and installs only what was chosen of it.
    struct Target {
        api::ShopDownload download{};
        std::vector<u64> ids{};
    };
    std::vector<Target> targets;
    bool missing{};

    const auto add = [&targets, &missing](const api::ShopDownload& download, u64 id) {
        // the download serves no file name, so without the extension there is
        // no telling which container to read.
        if (download.url.empty() || download.extension.empty()) {
            missing = true;
            return;
        }

        auto it = std::find_if(targets.begin(), targets.end(), [&download](const auto& e) {
            return e.download.url == download.url;
        });
        if (it == targets.end()) {
            it = targets.emplace(targets.end(), Target{download});
        }
        it->ids.emplace_back(id);
    };

    if (m_page.dlc) {
        add(m_page.download, ParseId(m_page.id));
    } else {
        if (m_install_version >= 0 && m_install_version < static_cast<s64>(m_title.versions.size())) {
            const auto game_id = ParseId(m_page.game_id);
            const auto& chosen = m_title.versions[m_install_version];

            // a game the console hasn't got needs its base game under any update.
            const auto& base = m_title.versions.front();
            if (!m_installed.installed && base.version == 0) {
                add(base.download, game_id);
            }

            // an update's id is its game's with 0x800 set. the installed version
            // puts nothing new on the console: chosen, it installs the add-ons alone.
            if (chosen.version > 0 && !(m_installed.installed && chosen.version == m_installed.version)) {
                add(chosen.download, game_id ^ 0x800);
            }
        }

        for (size_t i = 0; i < m_install_dlc.size() && i < m_title.dlc.size(); i++) {
            if (m_install_dlc[i]) {
                add(m_title.dlc[i].download, ParseId(m_title.dlc[i].app_id));
            }
        }
    }

    if (missing) {
        App::Notify(GetName() + ": " + "The shop didn't say how to download this"_i18n);
        return;
    }

    if (targets.empty()) {
        return;
    }

    App::PopToMenu();
    App::Push<ProgressBox>(0, "Installing "_i18n, GetName(), [config = m_config, targets](ProgressBox* pbox) -> Result {
        for (const auto& target : targets) {
            yati::source::Http source{target.download.url, config.user, config.pass, pbox->GetToken()};

            yati::ConfigOverride config_override{};
            config_override.title_ids = target.ids;

            // yati picks the container by the path's extension.
            const auto path = "download." + target.download.extension;
            R_TRY(yati::InstallFromSource(pbox, &source, path, config_override));
        }

        R_SUCCEED();
    }, [this](Result rc) {
        App::PushErrorBox(rc, "Install failed!"_i18n);
        if (R_SUCCEEDED(rc)) {
            App::Notify(i18n::Reorder("Installed ", GetName()));
        }

        const auto token = m_stop.get_token();
        m_console = std::make_unique<utils::Async>([this, token](){
            LoadInstalled(token);
        });
    }, ProgressBoxOption::ScreenToggle);
}

void TitleMenu::ShowAddons(const std::function<void()>& changed) {
    PopupMultiSelect::Items items;
    for (const auto& row : m_rows) {
        // the name the page's own row carries, and the console's answer about it:
        // one it already holds is listed, greyed out, rather than offered.
        items.emplace_back(PopupMultiSelect::Item{row.name, row.installed ? "Installed"_i18n : std::string{}, row.installed});
    }

    // the popup ticks the page's own vector in place and calls back on every
    // change, so the row behind it keeps its count while the popup is still up.
    App::Push<PopupMultiSelect>("Add-on content"_i18n, items, m_install_dlc, changed);
}

auto TitleMenu::GetInstallLabel() const -> std::string {
    // a dlc, an add-on, or a game the console hasn't got is always an install.
    const auto ticked = std::find(m_install_dlc.begin(), m_install_dlc.end(), 1) != m_install_dlc.end();
    if (m_page.dlc || ticked || !m_installed.installed || m_install_version < 0 || m_install_version >= static_cast<s64>(m_title.versions.size())) {
        return "Install"_i18n;
    }

    // the chosen version alone. the installed one installs nothing, and is left
    // called Install for the greyed-out entry to explain.
    const auto chosen = m_title.versions[m_install_version].version;
    if (chosen > m_installed.version) {
        return "Update"_i18n;
    }
    if (chosen < m_installed.version) {
        return "Downgrade"_i18n;
    }
    return "Install"_i18n;
}

auto TitleMenu::HasSomethingToInstall() const -> bool {
    // a dlc's page installs the one thing: the dlc.
    if (m_page.dlc) {
        return !m_dlc_installed;
    }

    // installed add-ons are never ticked, so any tick is something new.
    if (std::find(m_install_dlc.begin(), m_install_dlc.end(), 1) != m_install_dlc.end()) {
        return true;
    }

    if (m_install_version < 0 || m_install_version >= static_cast<s64>(m_title.versions.size())) {
        return false;
    }

    if (!m_installed.installed) {
        return true;
    }

    // the installed version is offered - it is what installs add-ons alone -
    // but puts nothing new on the console by itself.
    const auto chosen = m_title.versions[m_install_version].version;
    return chosen != m_installed.version && IsVersionOffered(chosen);
}

auto TitleMenu::IsVersionOffered(s64 version) const -> bool {
    // a game the console hasn't got - or hasn't answered for yet - takes any
    // version, and so does any version from the installed one up.
    if (!m_installed.installed || version >= m_installed.version) {
        return true;
    }

    // yati only treats an older *update* as a downgrade; the base game under an
    // installed update is skipped outright by "Skip if already installed".
    return version > 0 && App::GetApp()->m_allow_downgrade.Get();
}

auto TitleMenu::GetAddonCount() const -> std::string {
    const auto selected = std::count(m_install_dlc.begin(), m_install_dlc.end(), 1);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu / %zu", static_cast<size_t>(selected), m_rows.size());
    return buf;
}

void TitleMenu::OpenDlc(s64 index) {
    if (index < 0 || index >= static_cast<s64>(m_title.dlc.size())) {
        return;
    }
    const auto& dlc = m_title.dlc[index];

    // no card stands behind this page, so it opens on what the row knows and
    // fills in the rest when the shop answers.
    TitlePage page{};
    page.id = dlc.app_id;
    page.dlc = true;
    page.game_id = m_page.game_id;
    page.name = dlc.name;
    page.publisher = GetPublisher();
    page.game_name = GetName();
    page.banner = dlc.banner;
    page.icon = m_page.icon;
    page.download = dlc.download;

    App::Push<TitleMenu>(m_config, m_base_url, page);
}

void TitleMenu::OpenViewer() {
    // the same images the page steps through, in the same order, each at full
    // size where the shop keeps a bigger copy and as the page has it where not.
    std::vector<api::ShopImage> images;
    std::vector<int> previews;

    if (!GetBanner().url.empty()) {
        images.emplace_back(m_title.full_banner.url.empty() ? GetBanner() : m_title.full_banner);
        previews.emplace_back(m_banner);
    }

    for (size_t i = 0; i < m_screenshots.size() && i < m_title.screenshots.size(); i++) {
        const auto full = i < m_title.full_screenshots.size() ? m_title.full_screenshots[i] : api::ShopImage{};
        images.emplace_back(full.url.empty() ? m_title.screenshots[i] : full);
        previews.emplace_back(m_screenshots[i]);
    }

    if (images.empty()) {
        return;
    }

    App::Push<ScreenshotViewer>(m_config, m_page.id, std::move(images), std::move(previews), m_image_index);
}

void TitleMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);
    DrawPage(vg, theme);
    DrawColumn(vg, theme);
}

void TitleMenu::DrawPage(NVGcontext* vg, Theme* theme) {
    // drawn at its place on the page less the scroll, clipped to the space between
    // the heading's line and the bottom bar, and skipped when wholly outside it.
    const auto top = PAGE_Y - m_scroll;
    const auto on_screen = [top](float y, float h) {
        return top + y + h > CLIP_Y && top + y < CLIP_BOTTOM;
    };

    nvgSave(vg);
    nvgIntersectScissor(vg, PAGE_X - CLIP_BLEED, CLIP_Y, PAGE_W + CLIP_BLEED * 2.f, CLIP_BOTTOM - CLIP_Y);

    const auto count = GetImageCount();
    const Vec4 image_pos{PAGE_X, top, PAGE_W, IMAGE_H};

    if (on_screen(0, GetImagesBottom())) {
        if (count > 0) {
            // an image still on its way holds the space it will fill.
            if (const auto image = GetImage(m_image_index)) {
                gfx::drawImage(vg, image_pos, image, IMAGE_ROUNDING);
            } else {
                gfx::drawRect(vg, image_pos, theme->GetColour(ThemeEntryID_GRID), IMAGE_ROUNDING);
            }

            if (count > 1) {
                // the strip scrolls to keep the current image in view once there
                // are more than fit.
                const auto first = std::clamp<s64>(m_image_index - THUMB_COUNT / 2, 0, std::max<s64>(0, count - THUMB_COUNT));
                const auto last = std::min(count, first + THUMB_COUNT);

                for (s64 i = first; i < last; i++) {
                    const Vec4 v{PAGE_X + (i - first) * (THUMB_W + THUMB_GAP), top + IMAGE_H + THUMB_GAP_Y, THUMB_W, THUMB_H};
                    const auto current = i == m_image_index;

                    // first, as the catalog draws it: the outline lays a shadow
                    // over the rect it surrounds, which would blank the thumbnail.
                    if (current) {
                        gfx::drawRectOutline(vg, theme, 3.f, v);
                    }

                    if (const auto thumb = GetImage(i)) {
                        gfx::drawImage(vg, v, thumb, 4.f, current ? 1.f : 0.5f);
                    } else {
                        gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_GRID), 4.f);
                    }
                }
            }
        } else {
            // nothing to show but an icon: the game's own, where the banner would be.
            gfx::drawRect(vg, image_pos, theme->GetColour(ThemeEntryID_GRID), IMAGE_ROUNDING);
            const Vec4 v{image_pos.x + (image_pos.w - ICON_SIZE) / 2.f, image_pos.y + (image_pos.h - ICON_SIZE) / 2.f, ICON_SIZE, ICON_SIZE};
            gfx::drawImage(vg, v, m_icon ? m_icon : App::GetDefaultImage(), 14.f);
        }
    }

    if (!m_intro.empty() && on_screen(m_text_y, m_intro_h)) {
        nvgSave(vg);
        nvgTextLineHeight(vg, INTRO_LINE / INTRO_SIZE);
        gfx::drawTextBox(vg, PAGE_X, top + m_text_y, INTRO_SIZE, PAGE_W, theme->GetColour(ThemeEntryID_TEXT), m_intro.c_str());
        nvgRestore(vg);
    }

    if (!m_title.description.empty() && on_screen(m_desc_y, m_desc_h)) {
        nvgSave(vg);
        nvgTextLineHeight(vg, TEXT_LINE / TEXT_SIZE);
        gfx::drawTextBox(vg, PAGE_X, top + m_desc_y, TEXT_SIZE, PAGE_W, theme->GetColour(ThemeEntryID_TEXT), m_title.description.c_str());
        nvgRestore(vg);
    }

    const auto rows = static_cast<s64>(m_rows.size());
    if (rows > 0 && on_screen(m_rows_y, HEADING_H + rows * DLC_ROW_H)) {
        const auto heading = "Add-on content"_i18n;
        const auto heading_mid = top + m_rows_y + (HEADING_H - INTRO_GAP) / 2.f;

        nvgFontSize(vg, HEADING_SIZE);
        const auto heading_w = nvgTextBounds(vg, 0, 0, heading.c_str(), nullptr, nullptr);
        gfx::drawTextArgs(vg, PAGE_X, heading_mid, HEADING_SIZE, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), "%s", heading.c_str());
        gfx::drawTextArgs(vg, PAGE_X + heading_w + 12.f, heading_mid, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%ld", rows);

        for (s64 i = 0; i < rows; i++) {
            const auto v = GetRowRect(i);
            if (v.y + v.h <= CLIP_Y || v.y >= CLIP_BOTTOM) {
                continue;
            }

            const auto& row = m_rows[i];
            const auto selected = i == m_focus;

            // the highlight goes down before what is drawn on it.
            if (selected) {
                gfx::drawRect(vg, v, theme->GetColour(ThemeEntryID_SELECTED_BACKGROUND));
                gfx::drawRectOutline(vg, theme, 4.f, v);
            } else {
                gfx::drawRect(vg, v.x, v.y + v.h - 1.f, v.w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }

            const Vec4 art{v.x + 6.f, v.y + (v.h - DLC_IMAGE_H) / 2.f, DLC_IMAGE_W, DLC_IMAGE_H};
            if (row.image) {
                gfx::drawImage(vg, art, row.image, 4.f);
            } else {
                gfx::drawRect(vg, art, theme->GetColour(ThemeEntryID_GRID), 4.f);
            }

            const auto text_x = art.x + art.w + 18.f;
            auto text_right = v.x + v.w - 16.f;
            const auto mid = v.y + v.h / 2.f;

            // tagged the way the server list tags a server nobody has saved yet.
            if (row.installed) {
                const auto tag = "Installed"_i18n;
                nvgFontSize(vg, 16.f);
                const auto tag_w = nvgTextBounds(vg, 0, 0, tag.c_str(), nullptr, nullptr) + 20.f;
                const Vec4 t{text_right - tag_w, mid - 14.f, tag_w, 28.f};
                gfx::drawRect(vg, t, theme->GetColour(ThemeEntryID_HIGHLIGHT_1), 5.f);
                gfx::drawTextArgs(vg, t.x + t.w / 2.f, t.y + t.h / 2.f, 16.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_BACKGROUND), "%s", tag.c_str());
                text_right = t.x - 16.f;
            }

            const auto text_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT;
            const auto info_id = selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT_INFO;

            // a long name or tagline is cut short of the tag.
            nvgSave(vg);
            nvgIntersectScissor(vg, text_x, v.y, std::max(0.f, text_right - text_x), v.h);
            if (row.tagline.empty()) {
                gfx::drawTextArgs(vg, text_x, mid, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s", row.name.c_str());
            } else {
                gfx::drawTextArgs(vg, text_x, mid - 12.f, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s", row.name.c_str());
                gfx::drawTextArgs(vg, text_x, mid + 14.f, 16.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(info_id), "%s", row.tagline.c_str());
            }
            nvgRestore(vg);
        }
    }

    nvgRestore(vg);

    // a bar beside a page that runs longer than the screen, showing how long it
    // is and where in it the view is.
    if (const auto max = GetMaxScroll(); max > 0) {
        const auto bar_h = std::max(24.f, SCROLLBAR_H * VIEW_H / (m_content_h + PAGE_PAD));
        const auto bar_y = PAGE_Y + (SCROLLBAR_H - bar_h) * std::clamp(m_scroll / max, 0.f, 1.f);
        gfx::drawRect(vg, SCROLLBAR_X, PAGE_Y, 4.f, SCROLLBAR_H, theme->GetColour(ThemeEntryID_SCROLLBAR_BACKGROUND), 2.f);
        gfx::drawRect(vg, SCROLLBAR_X, bar_y, 4.f, bar_h, theme->GetColour(ThemeEntryID_SCROLLBAR), 2.f);
    }
}

void TitleMenu::DrawColumn(NVGcontext* vg, Theme* theme) {
    auto y = COLUMN_Y;

    // the name wraps onto a second line rather than scrolling, and is cut there.
    {
        constexpr float size = 28.f;
        constexpr float line = 34.f;
        constexpr float max_h = line * 2.f;
        const auto& name = GetName();

        nvgSave(vg);
        nvgFontSize(vg, size);
        nvgTextLineHeight(vg, line / size);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        float bounds[4]{};
        nvgTextBoxBounds(vg, COLUMN_X, y, COLUMN_W, name.c_str(), nullptr, bounds);

        nvgIntersectScissor(vg, COLUMN_X, y, COLUMN_W, max_h);
        gfx::drawTextBox(vg, COLUMN_X, y, size, COLUMN_W, theme->GetColour(ThemeEntryID_TEXT), name.c_str());
        nvgRestore(vg);

        y += std::min(bounds[3] - bounds[1], max_h) + 4.f;
    }

    if (const auto& publisher = GetPublisher(); !publisher.empty()) {
        gfx::drawTextArgs(vg, COLUMN_X, y, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", publisher.c_str());
        y += 26.f;
    }

    y += 20.f;

    if (m_page.dlc && !m_page.game_name.empty()) {
        constexpr float icon_size = 44.f;
        gfx::drawImage(vg, COLUMN_X, y, icon_size, icon_size, m_icon ? m_icon : App::GetDefaultImage(), 6.f);
        gfx::drawTextArgs(vg, COLUMN_X + icon_size + 12.f, y + icon_size / 2.f, 17.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", i18n::Reorder("Requires ", m_page.game_name).c_str());
        y += icon_size + 16.f;
    }

    for (const auto& fact : m_facts) {
        if (fact.group) {
            y += GROUP_GAP;
        }

        // a value too long for its row - a genre list, say - wraps onto a second
        // line, and the row grows to hold it. past that it is cut.
        const auto value_x = COLUMN_X + LABEL_W;
        const auto value_w = COLUMN_W - LABEL_W;
        nvgFontSize(vg, VALUE_SIZE);
        const auto wraps = nvgTextBounds(vg, 0, 0, fact.value.c_str(), nullptr, nullptr) > value_w;
        const auto row_h = wraps ? ROW_H + VALUE_LINE : ROW_H;

        const auto mid = y + row_h / 2.f;
        gfx::drawTextArgs(vg, COLUMN_X, mid, 17.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", fact.label.c_str());

        nvgSave(vg);
        if (wraps) {
            nvgIntersectScissor(vg, value_x, mid - VALUE_LINE, value_w, VALUE_LINE * 2.f);
            nvgTextLineHeight(vg, VALUE_LINE / VALUE_SIZE);
            gfx::drawTextBox(vg, value_x, mid - VALUE_LINE, VALUE_SIZE, value_w, theme->GetColour(ThemeEntryID_TEXT), fact.value.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, nullptr);
        } else {
            gfx::drawTextArgs(vg, COLUMN_X + COLUMN_W, mid, VALUE_SIZE, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), "%s", fact.value.c_str());
        }
        nvgRestore(vg);

        y += row_h;
        gfx::drawRect(vg, COLUMN_X, y - 1.f, COLUMN_W, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
    }
}

ScreenshotViewer::ScreenshotViewer(const sphaira::ownfoil::Config& config, const std::string& id, std::vector<api::ShopImage> images, std::vector<int> previews, s64 index)
: m_config{config}
, m_id{id}
, m_images{std::move(images)}
, m_previews{std::move(previews)} {
    SetAction(Button::B, Action{"Close"_i18n, [this]{
        SetPop();
    }});

    Show(index);
}

void ScreenshotViewer::Update(Controller* controller, TouchInfo* touch) {
    Widget::Update(controller, touch);

    // left and right step through the images, as they do on the page.
    const auto index = m_index;
    if (controller->GotDown(Button::DPAD_LEFT | Button::LS_LEFT)) {
        Show(m_index - 1);
    } else if (controller->GotDown(Button::DPAD_RIGHT | Button::LS_RIGHT)) {
        Show(m_index + 1);
    }

    if (m_index != index) {
        App::PlaySoundEffect(SoundEffect::Scroll);
    }
}

ScreenshotViewer::~ScreenshotViewer() {
    m_stop.request_stop();
    m_loader.reset();
    DeleteTexture(m_image);
}

void ScreenshotViewer::Show(s64 index) {
    const auto count = static_cast<s64>(m_images.size());
    index = std::clamp<s64>(index, 0, std::max<s64>(0, count - 1));
    if (index == m_index || index >= count) {
        return;
    }
    m_index = index;
    log_write("[OWNFOIL] viewer showing image %ld of %ld\n", m_index + 1, count);

    // whatever was loading belongs to the image being left. its request stops
    // on the token, so the join is short.
    m_stop.request_stop();
    m_loader.reset();
    m_stop = std::stop_source{};
    DeleteTexture(m_image);
    m_image = 0;

    const auto token = m_stop.get_token();
    const auto image = m_images[m_index];
    m_loader = std::make_unique<utils::Async>([this, token, image, index](){
        auto data = FetchImage(m_config, m_id, image, "screen", token);
        if (data.data.empty() || token.stop_requested()) {
            return;
        }

        // the shop fits its own copies to the screen, but a hotlink is whatever the
        // eshop serves - a 1920x1080 banner, say - so it is fitted here rather than
        // uploaded at a size nothing will draw.
        if (data.w > SCREEN_WIDTH || data.h > SCREEN_HEIGHT) {
            const auto scale = std::min(static_cast<float>(SCREEN_WIDTH) / data.w, static_cast<float>(SCREEN_HEIGHT) / data.h);
            auto fitted = ImageResize(data.data, data.w, data.h, static_cast<int>(data.w * scale), static_cast<int>(data.h * scale));
            if (!fitted.data.empty()) {
                data = std::move(fitted);
            }
        }

        evman::push(evman::CallbackEventData{[this, index, data = std::move(data)]() {
            if (index == m_index && !m_image) {
                m_image = CreateTexture(data);
            }
        }, token}, false);
    });
}

void ScreenshotViewer::Draw(NVGcontext* vg, Theme* theme) {
    gfx::drawRect(vg, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, nvgRGB(0, 0, 0));

    // the page's own copy stands in until the full one lands.
    const auto image = m_image ? m_image : (m_index >= 0 && m_index < static_cast<s64>(m_previews.size()) ? m_previews[m_index] : 0);
    if (image) {
        int w{}, h{};
        nvgImageSize(vg, image, &w, &h);
        if (w > 0 && h > 0) {
            const auto scale = std::min(static_cast<float>(SCREEN_WIDTH) / w, static_cast<float>(SCREEN_HEIGHT) / h);
            const auto dw = w * scale;
            const auto dh = h * scale;
            gfx::drawImage(vg, (SCREEN_WIDTH - dw) / 2.f, (SCREEN_HEIGHT - dh) / 2.f, dw, dh, image);
        }
    }

    // a shade under the hints, so they read over any image.
    constexpr float shade_h = 110.f;
    const auto shade = nvgLinearGradient(vg, 0, SCREEN_HEIGHT - shade_h, 0, SCREEN_HEIGHT, nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 200));
    gfx::drawRect(vg, 0, SCREEN_HEIGHT - shade_h, SCREEN_WIDTH, shade_h, shade);

    if (m_images.size() > 1) {
        gfx::drawTextArgs(vg, 80.f, 675.f, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%ld / %zu", m_index + 1, m_images.size());
    }

    Widget::Draw(vg, theme);
}

} // namespace sphaira::ui::menu::ownfoil

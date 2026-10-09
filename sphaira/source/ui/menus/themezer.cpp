#include "ui/menus/themezer.hpp"
#include "ui/menus/ghdl.hpp"
#include "ui/progress_box.hpp"
#include "ui/option_box.hpp"
#include "ui/sidebar.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "download.hpp"
#include "ui/nvg_util.hpp"
#include "swkbd.hpp"
#include "i18n.hpp"
#include "threaded_file_transfer.hpp"
#include "image.hpp"
#include "title_info.hpp"
#include "nro.hpp"

#include <minIni.h>
#include <stb_image.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <ranges>
#include <utility>
#include <yyjson.h>
#include "yyjson_helper.hpp"

namespace sphaira::ui::menu::themezer {
namespace {

// format is /themes/sphaira/Theme Name by Author/theme_name-type.nxtheme
constexpr fs::FsPath THEME_FOLDER{"/themes/sphaira/"};
constexpr auto CACHE_PATH = "/switch/sphaira/cache/themezer";
constexpr auto API_URL = "https://api.themezer.net/graphql";

constexpr const char* NRO_URL = "https://github.com/exelix11/SwitchThemeInjector";

constexpr const char* NRO_PATHS[]{
    "/switch/NXThemesInstaller.nro",
    "/switch/NXThemesInstaller/NXThemesInstaller.nro",
    "/switch/Switch_themes_Installer/NXThemesInstaller.nro",
};

constexpr const char* REQUEST_SORT[]{
    "DOWNLOADS",
    "UPDATED",
    "SAVES",
    "CREATED"
};

constexpr const char* REQUEST_ORDER[]{
    "DESC",
    "ASC"
};

// https://api.themezer.net/?query=query($nsfw:Boolean,$target:String,$page:Int,$limit:Int,$sort:String,$order:String,$query:String,$creators:[String!]){themeList(nsfw:$nsfw,target:$target,page:$page,limit:$limit,sort:$sort,order:$order,query:$query,creators:$creators){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}&variables={"nsfw":false,"target":null,"page":1,"limit":10,"sort":"updated","order":"desc","query":null,"creators":["695065006068334622"]}
// https://api.themezer.net/?query=query($nsfw:Boolean,$page:Int,$limit:Int,$sort:String,$order:String,$query:String,$creators:[String!]){packList(nsfw:$nsfw,page:$page,limit:$limit,sort:$sort,order:$order,query:$query,creators:$creators){id,creator{id,display_name},details{name,description},last_updated,dl_count,like_count,themes{id,creator{display_name},details{name,description},last_updated,dl_count,like_count,target,preview{original,thumb}}}}&variables={"nsfw":false,"page":1,"limit":10,"sort":"updated","order":"desc","query":null,"creators":["695065006068334622"]}

auto GetNroPath() -> const char* {
    fs::FsNativeSd fs;
    for (auto& path : NRO_PATHS) {
        if (fs.FileExists(path)) {
            return path;
        }
    }

    return nullptr;
}

auto HasNro() -> bool {
    return GetNroPath() != nullptr;
}

auto JsonEscape(std::string_view input) -> std::string {
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const auto raw : input) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    output += "\\u00";
                    output += HEX[c >> 4];
                    output += HEX[c & 0x0F];
                } else {
                    output += raw;
                }
                break;
        }
    }
    return output;
}

auto BuildGraphqlUrl(std::string_view query, std::string_view variables) -> std::string {
    return std::string{API_URL} + "?query=" + curl::EscapeString(std::string{query})
        + "&variables=" + curl::EscapeString(std::string{variables});
}

auto apiBuildUrlListInternal(const Config& e, bool) -> std::string {
    static constexpr std::string_view query =
        "query($includeNSFW:Boolean!,$paginationArgs:PaginationInput,$sort:ItemSort,$order:SortOrder,$query:String){"
        "switch{packs(includeNSFW:$includeNSFW,paginationArgs:$paginationArgs,sort:$sort,order:$order,query:$query){"
        "nodes{hexId creator{username} name collagePreview{jpgHdUrl jpgThumbUrl} themes{hexId creator{username} name target screenshotPreview{jpgThumbUrl} downloadUrl}}"
        "pageInfo{itemCount limit page pageCount}}}}";

    const auto sort_index = std::min<u32>(e.sort_index, std::size(REQUEST_SORT) - 1);
    const auto order_index = std::min<u32>(e.order_index, std::size(REQUEST_ORDER) - 1);
    std::string variables = "{\"includeNSFW\":" + std::string{e.nsfw ? "true" : "false"}
        + ",\"paginationArgs\":{\"page\":" + std::to_string(e.page)
        + ",\"limit\":" + std::to_string(e.limit) + "}"
        + ",\"sort\":\"" + REQUEST_SORT[sort_index] + "\""
        + ",\"order\":\"" + REQUEST_ORDER[order_index] + "\"";
    if (!e.query.empty()) {
        variables += ",\"query\":\"" + JsonEscape(e.query) + "\"";
    }
    variables += '}';
    return BuildGraphqlUrl(query, variables);
}

auto apiBuildUrlDownloadInternal(const std::string& id, bool is_pack) -> std::string {
    if (!is_pack) {
        return {};
    }
    static constexpr std::string_view query =
        "query($hexId:Hexadecimal!){switch{pack(hexId:$hexId){downloadUrl}}}";
    return BuildGraphqlUrl(query, "{\"hexId\":\"" + JsonEscape(id) + "\"}");
}

auto apiBuildUrlListPacks(const Config& e) -> std::string {
    return apiBuildUrlListInternal(e, true);
}

auto apiBuildListPacksCache(const Config& e) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%u_page.json", CACHE_PATH, e.page);
    return path;
}

auto apiBuildIconCache(std::string_view id) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%.*s_thumb.jpg", CACHE_PATH, static_cast<int>(id.size()), id.data());
    return path;
}

auto loadThemeImage(std::string_view id, Preview& preview) -> bool {
    auto& image = preview.lazy_image;

    // already have the image
    if (image.image) {
        // log_write("warning, tried to load image: %s when already loaded\n", path.c_str());
        return true;
    }
    auto vg = App::GetVg();

    const auto path = apiBuildIconCache(id);
    TimeStamp ts;
    const auto data = ImageLoadFromFile(path, ImageFlag_JPEG);
    if (!data.data.empty()) {
        image.w = data.w;
        image.h = data.h;
        image.image = nvgCreateImageRGBA(vg, data.w, data.h, 0, data.data.data());
        log_write("\t[image load] time taken: %.2fs %zums\n", ts.GetSecondsD(), ts.GetMs());
    }

    if (!image.image) {
        log_write("failed to load image from file: %s\n", path.s);
        return false;
    } else {
        // log_write("loaded image from file: %s\n", path);
        return true;
    }
}

void from_json(yyjson_val* json, Creator& e) {
    if (!json || !yyjson_is_obj(json)) return;
    if (const auto username = yyjson_obj_get(json, "username"); username && yyjson_is_str(username)) {
        e.display_name = yyjson_get_str(username);
        e.id = e.display_name;
    }
}

void from_json(yyjson_val* json, Details& e) {
    if (!json) return;
    if (yyjson_is_str(json)) {
        e.name = yyjson_get_str(json);
    } else if (yyjson_is_obj(json)) {
        if (const auto name = yyjson_obj_get(json, "name"); name && yyjson_is_str(name)) {
            e.name = yyjson_get_str(name);
        }
    }
}

void from_json(yyjson_val* json, Preview& e) {
    if (!json || !yyjson_is_obj(json)) return;
    auto thumb = yyjson_obj_get(json, "jpgThumbUrl");
    if (!thumb || !yyjson_is_str(thumb)) {
        thumb = yyjson_obj_get(json, "jpgHdUrl");
    }
    if (thumb && yyjson_is_str(thumb) && yyjson_get_str(thumb)) {
        e.thumb = yyjson_get_str(thumb);
    }
}

void from_json(yyjson_val* json, ThemeEntry& e) {
    if (!json || !yyjson_is_obj(json)) return;
    if (const auto id = yyjson_obj_get(json, "hexId"); id && yyjson_is_str(id)) {
        e.id = yyjson_get_str(id);
    }
    from_json(yyjson_obj_get(json, "screenshotPreview"), e.preview);
}

void from_json(yyjson_val* json, PackListEntry& e) {
    if (!json || !yyjson_is_obj(json)) return;
    if (const auto id = yyjson_obj_get(json, "hexId"); id && yyjson_is_str(id)) {
        e.id = yyjson_get_str(id);
    }
    from_json(yyjson_obj_get(json, "creator"), e.creator);
    from_json(yyjson_obj_get(json, "name"), e.details);
    from_json(yyjson_obj_get(json, "collagePreview"), e.preview);

    const auto themes = yyjson_obj_get(json, "themes");
    if (themes && yyjson_is_arr(themes)) {
        const auto count = std::min<std::size_t>(yyjson_arr_size(themes), 64);
        e.themes.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            from_json(yyjson_arr_get(themes, i), e.themes[i]);
        }
    }
}

void from_json(yyjson_val* json, Pagination& e) {
    if (!json || !yyjson_is_obj(json)) return;
    const auto set_uint = [json](const char* key, u64& output) {
        const auto value = yyjson_obj_get(json, key);
        if (value && yyjson_is_uint(value)) output = yyjson_get_uint(value);
    };
    set_uint("page", e.page);
    set_uint("limit", e.limit);
    set_uint("pageCount", e.page_count);
    set_uint("itemCount", e.item_count);
}

auto from_json(const std::vector<u8>& data, DownloadPack& e) -> bool {
    auto document = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    R_UNLESS(document, false);
    ON_SCOPE_EXIT(yyjson_doc_free(document));
    auto node = yyjson_doc_get_root(document);
    node = node ? yyjson_obj_get(node, "data") : nullptr;
    node = node ? yyjson_obj_get(node, "switch") : nullptr;
    node = node ? yyjson_obj_get(node, "pack") : nullptr;
    const auto url = node ? yyjson_obj_get(node, "downloadUrl") : nullptr;
    R_UNLESS(url && yyjson_is_str(url), false);
    e.filename = "theme_pack.zip";
    e.url = yyjson_get_str(url);
    e.mimetype = "application/zip";
    return !e.url.empty();
}

auto from_json(const fs::FsPath& path, PackList& e) -> bool {
    auto document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    R_UNLESS(document, false);
    ON_SCOPE_EXIT(yyjson_doc_free(document));
    auto node = yyjson_doc_get_root(document);
    node = node ? yyjson_obj_get(node, "data") : nullptr;
    node = node ? yyjson_obj_get(node, "switch") : nullptr;
    node = node ? yyjson_obj_get(node, "packs") : nullptr;
    R_UNLESS(node && yyjson_is_obj(node), false);

    const auto nodes = yyjson_obj_get(node, "nodes");
    const auto page_info = yyjson_obj_get(node, "pageInfo");
    R_UNLESS(nodes && yyjson_is_arr(nodes) && page_info && yyjson_is_obj(page_info), false);
    const auto count = std::min<std::size_t>(yyjson_arr_size(nodes), 64);
    e.packList.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        from_json(yyjson_arr_get(nodes, i), e.packList[i]);
    }
    from_json(page_info, e.pagination);
    return e.pagination.page_count > 0 && e.pagination.page_count <= 1000;
}

struct ThemeInstallRequest {
    std::string id;
    std::string name;
    std::string author;
};

auto InstallTheme(ProgressBox* pbox, const ThemeInstallRequest& request, std::vector<std::string>& nxtheme_paths) -> Result {
    static const fs::FsPath zip_out{"/switch/sphaira/cache/themezer/temp.zip"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    DownloadPack download_pack;

    // 1. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", request.name));
        log_write("starting download\n");

        const auto url = apiBuildUrlDownloadInternal(request.id, true);
        log_write("using url: %s\n", url.c_str());
        const auto result = curl::Api().ToMemory(
            curl::Url{url},
            curl::Header{{ "User-Agent", "themezer-nx" }},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        if (!result.success || result.data.empty()) {
            log_write("error with download: %s\n", url.c_str());
            R_THROW(Result_ThemezerFailedToDownloadThemeMeta);
        }

        R_UNLESS(from_json(result.data, download_pack), Result_ThemezerFailedToDownloadThemeMeta);
    }

    // 2. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", request.name));
        log_write("starting download: %s\n", download_pack.url.c_str());

        const auto result = curl::Api().ToFile(
            curl::Url{download_pack.url},
            curl::Path{zip_out},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        R_UNLESS(result.success, Result_ThemezerFailedToDownloadTheme);
    }

    ON_SCOPE_EXIT(fs.DeleteFile(zip_out));

    // replace invalid characters in the name.
    fs::FsPath name_buf{request.name};
    title::utilsReplaceIllegalCharacters(name_buf, false);

    // replace invalid characters in the author.
    fs::FsPath author_buf{request.author};
    title::utilsReplaceIllegalCharacters(author_buf, false);

    // create directories.
    fs::FsPath dir_path;
    std::snprintf(dir_path, sizeof(dir_path), "%s/%s - By %s", THEME_FOLDER.s, name_buf.s, author_buf.s);
    fs.CreateDirectoryRecursively(dir_path);

    // 3. extract the zip
    nxtheme_paths.clear();
    if (!pbox->ShouldExit()) {
        R_TRY(thread::TransferUnzipAll(pbox, zip_out, &fs, dir_path, [&nxtheme_paths](const fs::FsPath& name, fs::FsPath& path){
            // just in case theme packs start adding invalid entries.
            if (!path.ends_with(".nxtheme")) {
                return false;
            }

            // store path for later.
            nxtheme_paths.emplace_back(path);
            return true;
        }));
    }

    // ensure that we actually downloaded the theme.
    // todo: add new error for this.
    R_UNLESS(!nxtheme_paths.empty(), Result_ThemezerFailedToDownloadTheme);

    log_write("finished install :)\n");
    R_SUCCEED();
}

auto PromptThemeInstall(const std::vector<std::string>& nxtheme_paths) -> void {
    if (!HasNro()) {
        return;
    }

    App::Push<OptionBox>(
        "Theme downloaded, install now?"_i18n,
        "Back"_i18n, "Install"_i18n, 1, [nxtheme_paths](auto op_index){
            if (op_index && *op_index) {
                std::string args;

                for (const auto& path : nxtheme_paths) {
                    if (!args.empty()) {
                        args += ' ';
                    }
                    args += nro_add_arg_file(path);
                }

                log_write("themezer nro: %s\n", GetNroPath());
                log_write("themezer args: %s\n", args.c_str());

                const auto rc = nro_launch(GetNroPath(), args);
                App::PushErrorBox(rc, "Failed to launch NXthemes_Installer.nro"_i18n);
            }
        }
    );
}

} // namespace

LazyImage::LazyImage(LazyImage&& other) noexcept {
    *this = std::move(other);
}

auto LazyImage::operator=(LazyImage&& other) noexcept -> LazyImage& {
    if (this == &other) {
        return *this;
    }
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
    image = std::exchange(other.image, 0);
    w = other.w;
    h = other.h;
    tried_cache = other.tried_cache;
    cached = other.cached;
    state = other.state;
    return *this;
}

LazyImage::~LazyImage() {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

Menu::Menu(u32 flags) : MenuBase{"Themezer"_i18n, flags} {
    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);
    if (m_sort.Get() < 0 || m_sort.Get() >= static_cast<long>(std::size(REQUEST_SORT))) m_sort.Set(0);
    if (m_order.Get() < 0 || m_order.Get() >= static_cast<long>(std::size(REQUEST_ORDER))) m_order.Set(0);

    SetAction(Button::B, Action{"Back"_i18n, [this]{
        // if search is valid, then we are in search mode, return back to normal.
        if (!m_search.empty()) {
            m_search.clear();
            InvalidateAllPages();
        } else {
            SetPop();
        }
    }});

    this->SetActions(
        std::make_pair(Button::A, Action{"Download"_i18n, [this](){
            App::Push<OptionBox>(
                "Download theme?"_i18n,
                "Back"_i18n, "Download"_i18n, 1, [this](auto op_index){
                    if (op_index && *op_index) {
                        if (m_page_index < 0 || m_page_index >= static_cast<s64>(m_pages.size())) {
                            return;
                        }
                        const auto& page = m_pages[m_page_index];
                        if (page.m_ready == PageLoadState::Done && m_index >= 0 && m_index < static_cast<s64>(page.m_packList.size())) {
                            const auto& entry = page.m_packList[m_index];
                            const auto image = entry.preview.lazy_image.image;
                            auto request = std::make_shared<ThemeInstallRequest>(ThemeInstallRequest{
                                .id = entry.id,
                                .name = entry.details.name,
                                .author = entry.creator.display_name,
                            });
                            auto installed_paths = std::make_shared<std::vector<std::string>>();
                            App::Push<ProgressBox>(image, "Downloading "_i18n, request->name, [request, installed_paths](auto pbox) -> Result {
                                return InstallTheme(pbox, *request, *installed_paths);
                            }, [request, installed_paths](Result rc){
                                App::PushErrorBox(rc, "Failed to download theme"_i18n);

                                if (R_SUCCEEDED(rc)) {
                                    App::Notify(i18n::Reorder("Downloaded ", request->name));
                                    PromptThemeInstall(*installed_paths);
                                }
                            });
                        }
                    }
                }
            );
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            DisplayOptions();
        }}),
        std::make_pair(Button::R2, Action{"Next"_i18n, [this](){
            m_page_index++;
            if (m_page_index >= m_page_index_max) {
                m_page_index = m_page_index_max - 1;
            } else {
                PackListDownload();
            }
        }}),
        std::make_pair(Button::L2, Action{"Prev"_i18n, [this](){
            if (m_page_index) {
                m_page_index--;
                PackListDownload();
            }
        }})
    );

    const Vec4 v{75, 110, 350, 250};
    const Vec2 pad{10, 10};
    m_list = std::make_unique<List>(3, 6, m_pos, v, pad);

    m_page_index = 0;
    m_pages.resize(1);
    PackListDownload();
}

Menu::~Menu() {

}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (m_pages.empty() || m_page_index < 0 || m_page_index >= static_cast<s64>(m_pages.size())) {
        return;
    }

    const auto& page = m_pages[m_page_index];
    if (page.m_ready != PageLoadState::Done) {
        return;
    }

    m_list->OnUpdate(controller, touch, m_index, page.m_packList.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_pages.empty() || m_page_index < 0 || m_page_index >= static_cast<s64>(m_pages.size())) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty!"_i18n.c_str());
        return;
    }

    auto& page = m_pages[m_page_index];

    switch (page.m_ready) {
        case PageLoadState::None:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Not Ready..."_i18n.c_str());
            return;
        case PageLoadState::Loading:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Loading"_i18n.c_str());
            return;
        case PageLoadState::Done:
            break;
        case PageLoadState::Error:
            gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Error loading page!"_i18n.c_str());
            return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 1;
    int image_load_count = 0;

    m_list->Draw(vg, theme, page.m_packList.size(), [this, &page, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        auto& e = page.m_packList[pos];

        auto text_id = ThemeEntryID_TEXT;
        const auto selected = pos == m_index;
        if (selected) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            DrawElement(x, y, w, h, ThemeEntryID_GRID);
        }

        const float xoff = (350 - 320) / 2;

        // Prefer the pack collage returned by the current API, with the first
        // theme screenshot as a compatibility fallback.
        Preview* preview = &e.preview;
        std::string_view preview_id = e.id;
        if (preview->thumb.empty() && !e.themes.empty()) {
            preview = &e.themes[0].preview;
            preview_id = e.themes[0].id;
        }

        if (!preview->thumb.empty() && !preview_id.empty()) {
            auto& image = preview->lazy_image;

            // try and load cached image.
            if (image_load_count < image_load_max && !image.image && !image.tried_cache) {
                image.tried_cache = true;
                image.cached = loadThemeImage(preview_id, *preview);
                if (image.cached) {
                    image_load_count++;
                }
            }

            if (!image.image || image.cached) {
                switch (image.state) {
                    case ImageDownloadState::None: {
                        const auto path = apiBuildIconCache(preview_id);
                        log_write("downloading theme!: %s\n", path.s);

                        const auto url = preview->thumb;
                        log_write("downloading url: %s\n", url.c_str());
                        const auto pack_id = e.id;
                        const auto generation = m_generation;
                        image.state = ImageDownloadState::Progress;
                        const auto queued = curl::Api().ToFileAsync(
                            curl::Url{url},
                            curl::Path{path},
                            curl::Flags{curl::Flag_Cache},
                            curl::StopToken{this->GetToken()},
                            curl::OnComplete{[this, pack_id, generation](auto& result) {
                                if (generation != m_generation) {
                                    return;
                                }
                                PackListEntry* found{};
                                for (auto& candidate_page : m_pages) {
                                    const auto it = std::ranges::find_if(candidate_page.m_packList, [&pack_id](const PackListEntry& pack){
                                        return pack.id == pack_id;
                                    });
                                    if (it != candidate_page.m_packList.end()) {
                                        found = &*it;
                                        break;
                                    }
                                }
                                if (!found) {
                                    return;
                                }
                                Preview* completed_preview = &found->preview;
                                if (completed_preview->thumb.empty() && !found->themes.empty()) {
                                    completed_preview = &found->themes[0].preview;
                                }
                                auto& completed_image = completed_preview->lazy_image;
                                if (result.success) {
                                    completed_image.state = ImageDownloadState::Done;
                                    // data hasn't changed
                                    if (result.code == 304) {
                                        completed_image.cached = false;
                                    } else if (completed_image.image) {
                                        nvgDeleteImage(App::GetVg(), completed_image.image);
                                        completed_image.image = 0;
                                        completed_image.cached = false;
                                    }
                                } else {
                                    completed_image.state = ImageDownloadState::Failed;
                                    log_write("failed to download image\n");
                                }
                            }
                        });
                        if (!queued) {
                            image.state = ImageDownloadState::Failed;
                        }
                    }   break;
                    case ImageDownloadState::Progress: {

                    }   break;
                    case ImageDownloadState::Done: {
                        image.cached = false;
                        if (!loadThemeImage(preview_id, *preview)) {
                            image.state = ImageDownloadState::Failed;
                        } else {
                            image_load_count++;
                        }
                    }   break;
                    case ImageDownloadState::Failed: {
                    }   break;
                }
            }

            gfx::drawImage(vg, x + xoff, y, 320, 180, image.image ? image.image : App::GetDefaultImage(), 5);
        }

        const auto text_x = x + xoff;
        const auto text_clip_w = w - 30.f - xoff;
        const float font_size = 18;
        m_scroll_name.Draw(vg, selected, text_x, y + 180 + 20, text_clip_w, font_size, NVG_ALIGN_LEFT, theme->GetColour(text_id), e.details.name.c_str());
        m_scroll_author.Draw(vg, selected, text_x, y + 180 + 55, text_clip_w, font_size, NVG_ALIGN_LEFT, theme->GetColour(text_id), e.creator.display_name.c_str());
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_checked_for_nro) {
        m_checked_for_nro = true;

        // check if we have the nro, if not, then prompt the user to download from the appstore.
        if (!HasNro()) {
            App::Push<OptionBox>(
                "NXthemes_Installer.nro not found, download now?"_i18n,
                "Back"_i18n, "Download"_i18n, 1, [this](auto op_index){
                    if (op_index && *op_index) {
                        const gh::AssetEntry asset{
                            .name = "NXThemesInstaller.nro",
                            // same path as appstore
                            .path = "/switch/NXThemesInstaller/NXThemesInstaller.nro",
                        };

                        gh::Download(NRO_URL, asset, "latest");
                    }
                }
            );
        }
    }
}

void Menu::InvalidateAllPages() {
    ++m_generation;
    m_pages.clear();
    m_pages.resize(1);
    m_page_index = 0;
    PackListDownload();
}

void Menu::PackListDownload() {
    if (m_pages.empty() || m_page_index < 0 || m_page_index >= static_cast<s64>(m_pages.size())) {
        return;
    }
    const auto page_index = m_page_index + 1;
    const auto generation = m_generation;
    char subheading[128];
    std::snprintf(subheading, sizeof(subheading), "Page %zu / %zu"_i18n.c_str(), m_page_index+1, m_page_index_max);
    SetSubHeading(subheading);

    m_index = 0;
    m_list->SetYoff(0);

    // already downloaded
    if (m_pages[m_page_index].m_ready != PageLoadState::None) {
        return;
    }
    m_pages[m_page_index].m_ready = PageLoadState::Loading;

    Config config;
    config.page = page_index;
    config.SetQuery(m_search);
    config.sort_index = m_sort.Get();
    config.order_index = m_order.Get();
    config.nsfw = m_nsfw.Get();
    const auto packList_url = apiBuildUrlListPacks(config);
    const auto packlist_path = apiBuildListPacksCache(config);

    log_write("\npackList_url: %s\n\n", packList_url.c_str());

    const auto queued = curl::Api().ToFileAsync(
        curl::Url{packList_url},
        curl::Path{packlist_path},
        curl::Flags{curl::Flag_Cache},
        curl::Header{{ "User-Agent", "themezer-nx" }},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this, page_index, generation](auto& result){
            if (generation != m_generation) {
                return;
            }
            App::SetBoostMode(true);
            ON_SCOPE_EXIT(App::SetBoostMode(false));

            log_write("got themezer data\n");
            if (!result.success) {
                if (page_index > m_pages.size()) {
                    return;
                }
                auto& page = m_pages[page_index-1];
                page.m_ready = PageLoadState::Error;
                log_write("failed to get themezer data...\n");
                return;
            }

            PackList a;
            if (!from_json(result.path, a) || page_index > a.pagination.page_count) {
                if (page_index <= m_pages.size()) {
                    m_pages[page_index - 1].m_ready = PageLoadState::Error;
                }
                return;
            }

            std::erase_if(a.packList, [](const PackListEntry& entry){
                return entry.id.empty() || entry.details.name.empty();
            });

            m_pages.resize(a.pagination.page_count);
            auto& page = m_pages[page_index-1];

            page.m_packList = std::move(a.packList);
            page.m_pagination = a.pagination;
            page.m_ready = PageLoadState::Done;
            m_page_index_max = a.pagination.page_count;

            char subheading[128];
            std::snprintf(subheading, sizeof(subheading), "Page %zu / %zu"_i18n.c_str(), m_page_index+1, m_page_index_max);
            SetSubHeading(subheading);

            log_write("a.pagination.page: %zu\n", a.pagination.page);
            log_write("a.pagination.page_count: %zu\n", a.pagination.page_count);
        }
    });

    if (!queued) {
        m_pages[m_page_index].m_ready = PageLoadState::Error;
    }
}

void Menu::DisplayOptions() {
    auto options = std::make_unique<Sidebar>("Themezer Options"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    SidebarEntryArray::Items sort_items;
    sort_items.push_back("Downloads"_i18n);
    sort_items.push_back("Updated"_i18n);
    sort_items.push_back("Saves"_i18n);
    sort_items.push_back("Created"_i18n);

    SidebarEntryArray::Items order_items;
    order_items.push_back("Descending (down)"_i18n);
    order_items.push_back("Ascending (Up)"_i18n);

    options->Add<SidebarEntryBool>("Nsfw"_i18n, m_nsfw.Get(), [this](bool& v_out){
        m_nsfw.Set(v_out);
        InvalidateAllPages();
    });

    options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this, sort_items](s64& index_out){
        if (m_sort.Get() != index_out) {
            m_sort.Set(index_out);
            InvalidateAllPages();
        }
    }, m_sort.Get());

    options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this, order_items](s64& index_out){
        if (m_order.Get() != index_out) {
            m_order.Set(index_out);
            InvalidateAllPages();
        }
    }, m_order.Get());

    options->Add<SidebarEntryCallback>("Page"_i18n, [this](){
        s64 out;
        if (R_SUCCEEDED(swkbd::ShowNumPad(out, "Enter Page Number"_i18n.c_str(), nullptr, nullptr, 1, 3))) {
            if (out >= 1 && out <= m_page_index_max) {
                m_page_index = out - 1;
                PackListDownload();
            } else {
                log_write("invalid page number\n");
                App::Notify("Bad Page"_i18n);
            }
        }
    });

    options->Add<SidebarEntryCallback>("Search"_i18n, [this](){
        std::string out;
        if (R_SUCCEEDED(swkbd::ShowText(out)) && !out.empty()) {
            m_search = out;
            // PackListDownload();
            InvalidateAllPages();
        }
    });

    if (HasNro()) {
        options->Add<SidebarEntryCallback>("Launch NXthemes_Installer.nro"_i18n, [](){
            const auto rc = nro_launch(GetNroPath());
            App::PushErrorBox(rc, "Failed to launch NXthemes_Installer.nro"_i18n);
        });
    }
}

} // namespace sphaira::ui::menu::themezer

#include "ui/menus/appstore.hpp"
#include "ui/menus/homebrew.hpp"
#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/progress_box.hpp"
#include "ui/option_box.hpp"

#include "download.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "app.hpp"
#include "ui/nvg_util.hpp"
#include "fs.hpp"
#include "yyjson_helper.hpp"
#include "swkbd.hpp"
#include "i18n.hpp"
#include "hasher.hpp"
#include "threaded_file_transfer.hpp"
#include "nro.hpp"
#include "nacp_compat.hpp"
#include "web.hpp"
#include "minizip_helper.hpp"

#include "utils/utils.hpp"

#include <minIni.h>
#include <string>
#include <cstring>
#include <yyjson.h>
#include <stb_image.h>
#include <minizip/unzip.h>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <utility>

namespace sphaira::ui::menu::appstore {
namespace {

constexpr fs::FsPath CACHE_PATH{"/switch/sphaira/cache/appstore"};
constexpr fs::FsPath STORE_CONFIG_PATH{"/config/sphaira/appstoreAPI.ini"};
constexpr auto STORE_CONFIG_SECTION = "STORE";
constexpr auto STORE_CONFIG_SECTION_LEGACY = "appstore";
constexpr auto OFFICIAL_STORE_NAME = "HB App Store";
constexpr auto OFFICIAL_STORE_URL = "https://switch.cdn.fortheusers.org";
// the store the menu starts with: the HB App Store lists Switch 1 homebrew,
// brewstore's catalog Switch 2 homebrew.
constexpr auto BREWSTORE_NAME = "Brewstore";
constexpr auto BREWSTORE_URL = "https://raw.githubusercontent.com/HerXayah/brewstore-repo/main/catalog.json";
constexpr auto BUILTIN_STORE_NAME = platform::IsSwitch2 ? BREWSTORE_NAME : OFFICIAL_STORE_NAME;
constexpr auto BUILTIN_STORE_URL = platform::IsSwitch2 ? BREWSTORE_URL : OFFICIAL_STORE_URL;
constexpr std::size_t MAX_STORE_SOURCES = 32;
constexpr std::size_t MAX_STORE_PACKAGES = 20000;
constexpr std::size_t MAX_CATALOG_INCLUDES = 512;
constexpr auto URL_POST_FEEDBACK = "http://switchbru.com/appstore/feedback";
constexpr auto URL_GET_FEEDACK = "http://switchbru.com/appstore/feedback";

constexpr const u8 UPDATE_IMAGE_DATA[]{
    #embed <icons/UPDATE.png>
};

constexpr const u8 GET_IMAGE_DATA[]{
    #embed <icons/GET.png>
};

constexpr const u8 LOCAL_IMAGE_DATA[]{
    #embed <icons/LOCAL.png>
};

constexpr const u8 INSTALLED_IMAGE_DATA[]{
    #embed <icons/INSTALLED.png>
};

constexpr const char* FILTER_STR[] = {
    "All",
    "Games",
    "Emulators",
    "Tools",
    "Advanced",
    "Themes",
    "Legacy",
    "Misc",
};

constexpr const char* SORT_STR[] = {
    "Updated",
    "Downloads",
    "Size",
    "Alphabetical",
};

constexpr const char* ORDER_STR[] = {
    "Desc",
    "Asc",
};

auto HashStore(std::string_view value) -> u64 {
    u64 hash = 1469598103934665603ULL;
    for (const auto c : value) {
        hash ^= static_cast<u8>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

auto IsRemoteUrl(std::string_view value) -> bool {
    return value.starts_with("https://") || value.starts_with("http://");
}

auto StartsWithCi(std::string_view value, std::string_view prefix) -> bool {
    return value.size() >= prefix.size() && !strncasecmp(value.data(), prefix.data(), prefix.size());
}

auto EndsWithCi(std::string_view value, std::string_view suffix) -> bool {
    return value.size() >= suffix.size() && !strncasecmp(value.data() + value.size() - suffix.size(), suffix.data(), suffix.size());
}

auto TrimStoreUrl(std::string value) -> std::string {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::ranges::find_if(value, [](char c){ return !std::isspace(static_cast<unsigned char>(c)); });
    value.erase(value.begin(), first);
    while (value.size() > 8 && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

auto StoreBaseUrl(std::string url) -> std::string {
    url = TrimStoreUrl(std::move(url));
    constexpr std::string_view suffix{"/repo.json"};
    if (url.size() >= suffix.size() && !strcasecmp(url.c_str() + url.size() - suffix.size(), suffix.data())) {
        url.resize(url.size() - suffix.size());
    } else if (url.size() >= 5 && !strcasecmp(url.c_str() + url.size() - 5, ".json")) {
        // A source may point directly to an arbitrarily named catalog. Resolve
        // relative package resources beside that catalog, not below the JSON
        // filename itself.
        const auto scheme = url.find("://");
        const auto slash = url.find_last_of('/');
        if (slash != std::string::npos && (scheme == std::string::npos || slash > scheme + 2)) {
            url.resize(slash);
        }
    }
    return url;
}

auto BuildRepoUrl(const StoreSource& source) -> std::string {
    const auto url = TrimStoreUrl(source.url);
    if (url.ends_with(".json")) {
        return url;
    }
    return url + "/repo.json";
}

auto StoreCachePath(u64 store_id) -> fs::FsPath {
    fs::FsPath out;
    std::snprintf(out, sizeof(out), "%s/stores/%016llx", CACHE_PATH.s, static_cast<unsigned long long>(store_id));
    return out;
}

auto IncludeCachePath(const fs::FsPath& store_cache, std::string_view url) -> fs::FsPath {
    fs::FsPath out;
    std::snprintf(out, sizeof(out), "%s/includes/%016llx.json", store_cache.s, static_cast<unsigned long long>(HashStore(url)));
    return out;
}

// a brewstore catalog is {"include": [links]}, each link a manifest with a
// "packages" array. relative links are resolved beside the catalog.
auto ReadCatalogIncludes(const fs::FsPath& path, std::string_view catalog_url, std::vector<std::string>& out) -> bool {
    auto document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    R_UNLESS(document, false);
    ON_SCOPE_EXIT(yyjson_doc_free(document));

    const auto root = yyjson_doc_get_root(document);
    const auto include = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "include") : nullptr;
    R_UNLESS(include && yyjson_is_arr(include), false);

    const auto base = StoreBaseUrl(std::string{catalog_url});
    out.clear();
    size_t index{}, max{};
    yyjson_val* link{};
    yyjson_arr_foreach(include, index, max, link) {
        const auto value = yyjson_get_str(link);
        if (!value || !value[0] || out.size() >= MAX_CATALOG_INCLUDES) {
            continue;
        }

        std::string_view url{value};
        if (IsRemoteUrl(url)) {
            out.emplace_back(url);
        } else {
            while (url.starts_with("./") || url.starts_with('/')) {
                url.remove_prefix(url.front() == '.' ? 2 : 1);
            }
            out.emplace_back(base + "/" + std::string{url});
        }
    }
    return true;
}

auto ResolveResourceUrl(const Entry& entry, std::string_view resource, std::string_view fallback) -> std::string {
    if (resource.empty()) {
        resource = fallback;
    }
    if (IsRemoteUrl(resource)) {
        return std::string{resource};
    }

    if (!resource.empty() && resource.front() == '/') {
        const auto scheme = entry.store_base.find("://");
        const auto slash = scheme == std::string::npos ? std::string::npos : entry.store_base.find('/', scheme + 3);
        if (slash != std::string::npos) {
            return entry.store_base.substr(0, slash) + std::string{resource};
        }
    }

    return entry.store_base + (resource.empty() || resource.front() == '/' ? "" : "/") + std::string{resource};
}

auto BuildIconUrl(const Entry& e) -> std::string {
    return ResolveResourceUrl(e, e.icon_url, "packages/" + e.name + "/icon.png");
}

auto BuildBannerUrl(const Entry& e) -> std::string {
    return ResolveResourceUrl(e, e.banner_url, "packages/" + e.name + "/screen.png");
}

auto BuildManifestUrl(const Entry& e) -> std::string {
    return ResolveResourceUrl(e, e.manifest_url, "packages/" + e.name + "/manifest.install");
}

auto BuildZipUrl(const Entry& e) -> std::string {
    const auto& resource = !e.download_url.empty() ? e.download_url : (!e.zip_url.empty() ? e.zip_url : e.resource_url);
    return ResolveResourceUrl(e, resource, "zips/" + e.name + ".zip");
}

auto BuildIconCachePath(const Entry& e) -> fs::FsPath {
    fs::FsPath out;
    std::snprintf(out, sizeof(out), "%s/stores/%016llx/icons/%s.png", CACHE_PATH.s, static_cast<unsigned long long>(e.store_id), e.name.c_str());
    return out;
}

auto BuildBannerCachePath(const Entry& e) -> fs::FsPath {
    fs::FsPath out;
    std::snprintf(out, sizeof(out), "%s/stores/%016llx/banners/%s.png", CACHE_PATH.s, static_cast<unsigned long long>(e.store_id), e.name.c_str());
    return out;
}

#if 0
auto BuildScreensCachePath(const Entry& e, u8 num) -> fs::FsPath {
    fs::FsPath out;
    std::snprintf(out, sizeof(out), "%s/screens/%s%u.png", CACHE_PATH, e.name.c_str(), num+1);
    return out;
}
#endif

// use appstore path in order to maintain compat with appstore
auto BuildPackageCachePath(const Entry& e) -> fs::FsPath {
    if (e.store_id == HashStore(OFFICIAL_STORE_URL)) {
        return "/switch/appstore/.get/packages/" + e.name;
    }

    fs::FsPath out;
    std::snprintf(out, sizeof(out), "/switch/appstore/.get/stores/%016llx/packages/%s", static_cast<unsigned long long>(e.store_id), e.name.c_str());
    return out;
}

auto BuildInfoCachePath(const Entry& e) -> fs::FsPath {
    return BuildPackageCachePath(e) + "/info.json";
}

auto BuildManifestCachePath(const Entry& e) -> fs::FsPath {
    return BuildPackageCachePath(e) + "/manifest.install";
}

auto BuildFeedbackCachePath(const Entry& e) -> fs::FsPath {
    return BuildPackageCachePath(e) + "/feedback.json";
}

void from_json(yyjson_val* json, Entry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(category);
        JSON_SET_STR(binary);
        JSON_SET_STR(updated);
        JSON_SET_STR(name);
        JSON_SET_STR(license);
        JSON_SET_STR(title);
        JSON_SET_STR(url);
        JSON_SET_STR(description);
        JSON_SET_STR(author);
        JSON_SET_STR(changelog);
        JSON_SET_UINT(screens);
        JSON_SET_UINT(extracted);
        JSON_SET_STR(version);
        JSON_SET_UINT(filesize);
        JSON_SET_STR(details);
        JSON_SET_UINT(app_dls);
        JSON_SET_STR(md5);
        JSON_SET_STR(sha256);
        JSON_SET_STR(icon_url);
        JSON_SET_STR(banner_url);
        JSON_SET_STR(manifest_url);
        JSON_SET_STR(download_url);
        JSON_SET_STR(zip_url);
        JSON_SET_STR(resource_url);
        JSON_SET_STR(release);
        JSON_SET_STR(asset);
        JSON_SET_STR(target);
        JSON_SET_UINT(published);

        case cexprHash("icon"): {
            if (yyjson_is_str(val)) {
                e.icon_url = yyjson_get_str(val);
            }
        } break;
        case cexprHash("marker"): {
            if (yyjson_is_str(val)) {
                std::string_view marker{yyjson_get_str(val)};
                if (marker.starts_with("sdmc:")) {
                    marker.remove_prefix(5);
                }
                e.marker = marker;
            }
        } break;
        case cexprHash("stripPrefix"): {
            if (yyjson_is_str(val)) {
                e.strip_prefix = yyjson_get_str(val);
            }
        } break;
    );
}

auto IsSafePackageName(std::string_view name) -> bool {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        return false;
    }
    return std::ranges::none_of(name, [](char c){
        const auto value = static_cast<unsigned char>(c);
        return value < 0x20 || c == '/' || c == '\\' || c == ':';
    });
}

auto IsSafeManifestPath(std::string_view path) -> bool {
    if (path.empty() || path.size() >= PATH_MAX || path.find_first_of("\\:\r\n") != std::string_view::npos) {
        return false;
    }

    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return !path.empty()
        && path != ".."
        && !path.starts_with("../")
        && !path.ends_with("/..")
        && path.find("/../") == std::string_view::npos;
}

auto IsSupportedReleaseAsset(std::string_view name) -> bool {
    return EndsWithCi(name, ".nro") || EndsWithCi(name, ".zip");
}

auto from_json(const fs::FsPath& path, std::vector<appstore::Entry>& entries) -> bool {
    auto document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    R_UNLESS(document, false);
    ON_SCOPE_EXIT(yyjson_doc_free(document));

    const auto root = yyjson_doc_get_root(document);
    R_UNLESS(root && yyjson_is_obj(root), false);
    const auto packages = yyjson_obj_get(root, "packages");
    R_UNLESS(packages && yyjson_is_arr(packages), false);

    const auto count = yyjson_arr_size(packages);
    R_UNLESS(count <= MAX_STORE_PACKAGES, false);
    const auto schema = yyjson_obj_get(root, "schema_version");
    if (schema) {
        const auto declared_count = yyjson_obj_get(root, "count");
        R_UNLESS(yyjson_is_uint(schema) && yyjson_get_uint(schema) == 1, false);
        R_UNLESS(yyjson_is_uint(declared_count) && yyjson_get_uint(declared_count) == count, false);
    }

    entries.clear();
    entries.reserve(count);
    size_t index{}, max{};
    yyjson_val* package{};
    yyjson_arr_foreach(packages, index, max, package) {
        Entry entry;
        from_json(package, entry);
        if (schema) {
            const auto tag = yyjson_get_str(yyjson_obj_get(package, "release_tag"));
            const auto valid_hash = entry.sha256.size() == 64 && std::ranges::all_of(entry.sha256, [](unsigned char c){
                return std::isxdigit(c);
            });
            if (!tag || !*tag || !entry.download_url.starts_with("https://") ||
                !valid_hash || !IsSafePackageName(entry.asset) || !IsSupportedReleaseAsset(entry.asset)) {
                continue;
            }
            entry.release_tag = tag;
            entry.release_asset = true;
        }

        if (!entry.marker.starts_with('/') || !IsSafeManifestPath(entry.marker)) {
            entry.marker.clear();
        } else if (EndsWithCi(entry.marker, ".nro")) {
            entry.binary = entry.marker;
        }
        entries.emplace_back(std::move(entry));
    }
    return true;
}

auto ParseManifest(std::span<const char> view) -> ManifestEntries {
    ManifestEntries entries;
    // auto view = std::string_view{manifest_data.data(), manifest_data.size()};

    for (const auto line : std::views::split(view, '\n')) {
        if (line.size() <= 3) {
            continue;
        }

        ManifestEntry entry{};
        entry.command = line[0];
        if (entry.command != 'E' && entry.command != 'U' && entry.command != 'G') {
            continue;
        }

        auto path = std::string_view{line.data() + 3, line.size() - 3};
        if (!path.empty() && path.back() == '\r') {
            path.remove_suffix(1);
        }
        if (!IsSafeManifestPath(path)) {
            log_write("[APPSTORE] rejected unsafe manifest path\n");
            continue;
        }

        std::memcpy(entry.path.s, path.data(), path.size());
        entry.path.s[path.size()] = '\0';
        entries.emplace_back(entry);
    }

    return entries;
}

auto LoadAndParseManifest(const Entry& e) -> ManifestEntries {
    const auto path = BuildManifestCachePath(e);

    std::vector<u8> data;
    if (R_FAILED(fs::FsNativeSd().read_entire_file(path, data))) {
        return {};
    }

    return ParseManifest(std::span{(const char*)data.data(), data.size()});
}

auto EntryLoadImageData(std::span<const u8> image_buf, LazyImage& image) -> bool {
    // already have the image
    if (image.image) {
        // log_write("warning, tried to load image: %s when already loaded\n", path);
        return true;
    }
    if (image_buf.empty() || image_buf.size() > 8 * 1024 * 1024) {
        return false;
    }

    int probe_w{}, probe_h{}, probe_channels{};
    if (!stbi_info_from_memory(image_buf.data(), image_buf.size(), &probe_w, &probe_h, &probe_channels)
        || probe_w <= 0 || probe_h <= 0 || probe_w > 4096 || probe_h > 4096) {
        return false;
    }

    auto vg = App::GetVg();

    int channels_in_file;
    auto buf = stbi_load_from_memory(image_buf.data(), image_buf.size(), &image.w, &image.h, &channels_in_file, 4);
    if (buf) {
        ON_SCOPE_EXIT(stbi_image_free(buf));
        std::memcpy(image.first_pixel, buf, sizeof(image.first_pixel));
        image.image = nvgCreateImageRGBA(vg, image.w, image.h, 0, buf);
    }

    return image.image;
}

auto EntryLoadImageFile(fs::Fs& fs, const fs::FsPath& path, LazyImage& image) -> bool {
    // already have the image
    if (image.image) {
        // log_write("warning, tried to load image: %s when already loaded\n", path);
        return true;
    }

    fs::File file;
    s64 file_size{};
    if (R_FAILED(fs.OpenFile(path, FsOpenMode_Read, &file))
        || R_FAILED(file.GetSize(&file_size))
        || file_size <= 0
        || file_size > 8 * 1024 * 1024) {
        log_write("invalid image file size: %s\n", path.s);
        return false;
    }
    file.Close();

    std::vector<u8> image_buf;
    if (R_FAILED(fs.read_entire_file(path, image_buf))) {
        log_write("failed to load image from file: %s\n", path.s);
    } else {
        EntryLoadImageData(image_buf, image);
    }

    if (!image.image) {
        log_write("failed to load image from file: %s\n", path.s);
        return false;
    } else {
        // log_write("loaded image from file: %s\n", path);
        return true;
    }
}

auto EntryLoadImageFile(const fs::FsPath& path, LazyImage& image) -> bool {
    if (!strncasecmp("romfs:/", path, 7)) {
        fs::FsStdio fs;
        return EntryLoadImageFile(fs, path, image);
    } else {
        fs::FsNativeSd fs;
        return EntryLoadImageFile(fs, path, image);
    }
}

void DrawIcon(NVGcontext* vg, const LazyImage& l, const LazyImage& d, float x, float y, float w, float h, bool rounded = true, float scale = 1.0) {
    const auto& i = l.image ? l : d;

    const float iw = (float)i.w / scale;
    const float ih = (float)i.h / scale;
    float ix = x;
    float iy = y;
    bool rounded_image = rounded;

    if (w > iw) {
        ix = x + abs((w - iw) / 2);
    } else if (w < iw) {
        ix = x - abs((w - iw) / 2);
    }
    if (h > ih) {
        iy = y + abs((h - ih) / 2);
    } else if (h < ih) {
        iy = y - abs((h - ih) / 2);
    }

    bool crop = false;
    if (iw < w || ih < h) {
        rounded_image = false;
        gfx::drawRect(vg, x, y, w, h, nvgRGB(i.first_pixel[0], i.first_pixel[1], i.first_pixel[2]), rounded ? 5 : 0);
    }
    if (iw > w || ih > h) {
        crop = true;
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, w, h);
    }

    gfx::drawImage(vg, ix, iy, iw, ih, i.image, rounded_image ? 5 : 0);
    if (crop) {
        nvgRestore(vg);
    }
}

void DrawIcon(NVGcontext* vg, const LazyImage& l, const LazyImage& d, const Vec4& vec, bool rounded = true, float scale = 1.0) {
    DrawIcon(vg, l, d, vec.x, vec.y, vec.w, vec.h, rounded, scale);
}

auto AppDlToStr(u32 value) -> std::string {
    auto str = std::to_string(value);
    u32 inc = 3;
    for (u32 i = inc; i < str.size(); i += inc) {
        str.insert(str.cend() - i , ',');
        inc++;
    }
    return str;
}

void ReadFromInfoJson(Entry& e) {
    const auto info_path = BuildInfoCachePath(e);

    yyjson_read_err err;
    auto doc = yyjson_read_file(info_path, YYJSON_READ_NOFLAG, nullptr, &err);
    if (doc) {
        const auto root = yyjson_doc_get_root(doc);
        const auto version = yyjson_obj_get(root, "version");
        if (version && yyjson_is_str(version)) {
            const auto installed_version = yyjson_get_str(version);
            if (installed_version && !std::strcmp(installed_version, e.version.c_str())) {
                e.status = EntryStatus::Installed;
            } else {
                e.status = EntryStatus::Update;
                log_write("info.json said %s needs update: %s vs %s\n", e.name.c_str(), installed_version ? installed_version : "", e.version.c_str());
            }
        }
        // log_write("got info for: %s\n", e.name.c_str());
        yyjson_doc_free(doc);
    }
}

// this ignores ShouldExit() as leaving somthing in a half
// deleted state is a bad idea :)
auto UninstallApp(ProgressBox* pbox, const Entry& entry) -> Result {
    const auto manifest = LoadAndParseManifest(entry);
    fs::FsNativeSd fs;

    if (manifest.empty()) {
        if (!entry.binary.empty()) {
            R_TRY(fs.DeleteFile(entry.binary));
        }
    } else {
        for (auto& e : manifest) {
            pbox->NewTransfer(e.path);

            const auto safe_buf = fs::AppendPath("/", e.path);
            // this will handle read only files, ie, hbmenu.nro
            if (R_FAILED(fs.DeleteFile(safe_buf))) {
                log_write("failed to delete file: %s\n", safe_buf.s);
            } else {
                log_write("deleted file: %s\n", safe_buf.s);
                svcSleepThread(1e+5);
                // todo: delete empty directories!
                // fs::delete_directory(safe_buf);
            }
        }
    }

    // the folders a release asset filled, once empty, deepest first. only
    // under /switch: the app's own folders.
    if (entry.release_asset) {
        std::vector<std::string> folders;
        for (const auto& e : manifest) {
            std::string_view folder{e.path.s};
            for (auto slash = folder.rfind('/'); slash != std::string_view::npos; slash = folder.rfind('/')) {
                folder = folder.substr(0, slash);
                if (!StartsWithCi(folder, "switch/")) {
                    break;
                }
                folders.emplace_back(folder);
            }
        }
        std::ranges::sort(folders);
        folders.erase(std::ranges::unique(folders).begin(), folders.end());
        std::ranges::stable_sort(folders, [](const auto& a, const auto& b){ return a.size() > b.size(); });
        for (const auto& folder : folders) {
            fs.DeleteDirectory(fs::AppendPath("/", folder));
        }
    }

    // remove directory, this will also delete manifest and info
    const auto dir = BuildPackageCachePath(entry);
    pbox->NewTransfer(i18n::Reorder("Removing ", dir.toString()));
    if (R_FAILED(fs.DeleteDirectoryRecursively(dir))) {
        log_write("failed to delete folder: %s\n", dir.s);
    } else {
        log_write("deleted: %s\n", dir.s);
    }

    R_SUCCEED();
}

// this is called by ProgressBox on a seperate thread
// it has 4 main steps
// 1. download the zip
// 2. md5 check the zip
// 3. parse manifest and unzip everything to placeholder
// 4. move everything from placeholder to normal location
auto InstallApp(ProgressBox* pbox, const Entry& entry) -> Result {
    static const fs::FsPath zip_out{"/switch/sphaira/cache/appstore/temp.zip"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    // check if we can download the entire zip to mem for faster download / extract times.
    // Repository sizes are expressed in KiB. Stream unknown or >=300 MiB
    // archives to the SD card so a malformed third-party entry cannot exhaust
    // application memory.
    const auto file_download = App::IsApplet() || !entry.filesize || entry.filesize >= 300 * 1024;
    curl::ApiResult api_result{};

    // 1. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", entry.title));
        log_write("starting download\n");

        const auto url = BuildZipUrl(entry);
        curl::Api api{
            curl::Url{url},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        };

        if (file_download) {
            api.SetOption(curl::Path{zip_out});
            api_result = curl::ToFile(api);
        } else {
            api_result = curl::ToMemory(api);
        }

        R_UNLESS(api_result.success, Result_AppstoreFailedZipDownload);
    }

    ON_SCOPE_EXIT(fs.DeleteFile(zip_out));

    // 2. Verify the strongest checksum supplied by the repository. Custom
    // direct-resource entries may omit checksums, in which case ZIP CRCs are
    // still verified during extraction.
    const auto hash_type = entry.sha256.size() == 64 ? hash::Type::Sha256 : hash::Type::Md5;
    const auto& expected_hash = hash_type == hash::Type::Sha256 ? entry.sha256 : entry.md5;
    if (!pbox->ShouldExit() && (expected_hash.size() == 32 || expected_hash.size() == 64)) {
        pbox->NewTransfer(hash_type == hash::Type::Sha256 ? "Checking SHA-256"_i18n : "Checking MD5"_i18n);
        log_write("starting package hash check\n");

        std::string hash_out;
        if (file_download) {
            R_TRY(hash::Hash(pbox, hash_type, &fs, zip_out, hash_out));
        } else {
            R_TRY(hash::Hash(pbox, hash_type, api_result.data, hash_out));
        }

        if (hash_out.size() != expected_hash.size() || strncasecmp(hash_out.data(), expected_hash.data(), expected_hash.size())) {
            log_write("bad package hash\n");
            R_THROW(Result_AppstoreFailedMd5);
        }
    }

    mz::MzSpan mz_span{api_result.data};
    zlib_filefunc64_def file_func;
    if (!file_download) {
        mz::FileFuncSpan(&mz_span, &file_func);
    } else {
        mz::FileFuncStdio(&file_func);
    }

    // 3. extract the zip
    if (!pbox->ShouldExit()) {
        auto zfile = unzOpen2_64(zip_out, &file_func);
        R_UNLESS(zfile, Result_UnzOpen2_64);
        ON_SCOPE_EXIT(unzClose(zfile));

        // get manifest
        if (UNZ_END_OF_LIST_OF_FILE == unzLocateFile(zfile, "manifest.install", 0)) {
            log_write("failed to find manifest.install\n");
            R_THROW(Result_UnzLocateFile);
        }

        ManifestEntries new_manifest;
        const auto old_manifest = LoadAndParseManifest(entry);
        {
            if (UNZ_OK != unzOpenCurrentFile(zfile)) {
                log_write("failed to open current file\n");
                R_THROW(Result_UnzOpenCurrentFile);
            }
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            if (UNZ_OK != unzGetCurrentFileInfo64(zfile, &info, 0, 0, 0, 0, 0, 0)) {
                log_write("failed to get current info\n");
                R_THROW(Result_UnzGetGlobalInfo64);
            }

            if (!info.uncompressed_size || info.uncompressed_size > 2 * 1024 * 1024) {
                log_write("manifest has invalid size\n");
                R_THROW(Result_AppstoreFailedParseManifest);
            }

            std::vector<char> manifest_data(info.uncompressed_size);
            if ((int)info.uncompressed_size != unzReadCurrentFile(zfile, manifest_data.data(), manifest_data.size())) {
                log_write("failed to read manifest file\n");
                R_THROW(Result_UnzReadCurrentFile);
            }

            new_manifest = ParseManifest(manifest_data);
            if (new_manifest.empty()) {
                log_write("manifest is empty!\n");
                R_THROW(Result_AppstoreFailedParseManifest);
            }
        }

        const auto unzip_to = [&](const fs::FsPath& inzip, const fs::FsPath& output) -> Result {
            pbox->NewTransfer(inzip);

            if (UNZ_END_OF_LIST_OF_FILE == unzLocateFile(zfile, inzip, 0)) {
                log_write("failed to find %s\n", inzip.s);
                R_THROW(Result_UnzLocateFile);
            }

            if (UNZ_OK != unzOpenCurrentFile(zfile)) {
                log_write("failed to open current file\n");
                R_THROW(Result_UnzOpenCurrentFile);
            }
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            if (UNZ_OK != unzGetCurrentFileInfo64(zfile, &info, 0, 0, 0, 0, 0, 0)) {
                log_write("failed to get current info\n");
                R_THROW(Result_UnzGetCurrentFileInfo64);
            }

            auto path = output;
            if (path[0] != '/') {
                path = fs::AppendPath("/", path);
            }

            return thread::TransferUnzip(pbox, zfile, &fs, path, info.uncompressed_size, info.crc);
        };

        // unzip manifest, info and all entries.
        TimeStamp ts;
        #if 1
        R_TRY(unzip_to("info.json", BuildInfoCachePath(entry)));
        R_TRY(unzip_to("manifest.install", BuildManifestCachePath(entry)));
        #endif

        R_TRY(thread::TransferUnzipAll(pbox, zfile, &fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
            const auto it = std::ranges::find_if(new_manifest, [&name](auto& e){
                return !strcasecmp(name, e.path);
            });

            if (it == new_manifest.end()) [[unlikely]] {
                return false;
            }

            pbox->NewTransfer(it->path);

            switch (it->command) {
                case 'E': // both are the same?
                case 'U':
                    return true;

                case 'G': // checks if file exists, if not, extract
                    return !fs.FileExists(fs::AppendPath("/", it->path));

                default:
                    log_write("bad command: %c\n", it->command);
                    return false;
            }
        }));

        log_write("\n\t[APPSTORE] finished extract new, time taken: %.2fs %zums\n\n", ts.GetSecondsD(), ts.GetMs());

        // finally finally, remove files no longer in the manifest
        for (auto& old_entry : old_manifest) {
            bool found = false;
            for (auto& new_entry : new_manifest) {
                if (!strcasecmp(old_entry.path, new_entry.path)) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                const auto safe_buf = fs::AppendPath("/", old_entry.path);
                // std::strcat(safe_buf, old_entry.path);
                if (R_FAILED(fs.DeleteFile(safe_buf))) {
                    log_write("failed to delete: %s\n", safe_buf.s);
                } else {
                    log_write("deleted file: %s\n", safe_buf.s);
                    svcSleepThread(1e+5);
                }
            }
        }
    }

    log_write("finished install :)\n");
    R_SUCCEED();
}

// case-insensitive version of str.find()
auto FindCaseInsensitive(std::string_view base, std::string_view term) -> bool {
    const auto it = std::search(base.cbegin(), base.cend(), term.cbegin(), term.cend(), [](char a, char b){
        return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
    });
    return it != base.cend();
}

// the .nro an install left, to launch it: the least nested one, the app
// rather than, say, RetroArch's cores.
auto FindInstalledNro(const Entry& e) -> std::string {
    const auto manifest = LoadAndParseManifest(e);
    std::string_view nro;
    for (const auto& file : manifest) {
        const std::string_view path{file.path.s};
        if (EndsWithCi(path, ".nro") && (nro.empty() || std::ranges::count(path, '/') < std::ranges::count(nro, '/'))) {
            nro = path;
        }
    }
    return nro.empty() ? std::string{} : fs::AppendPath("/", nro).toString();
}

struct ReleaseAsset {
    std::string name;
    std::string url;
    std::string sha256; // GitHub lists it, GitLab doesn't
};

// as brewstore picks it: the asset whose name ends like the package's asset,
// or else the first .nro, or else the first zip that isn't debug symbols or
// sources.
auto PickReleaseAsset(const Entry& e, const std::vector<ReleaseAsset>& assets) -> const ReleaseAsset* {
    const ReleaseAsset* zip{};
    for (const auto& asset : assets) {
        if (!e.asset.empty()) {
            if (EndsWithCi(asset.name, e.asset)) {
                return &asset;
            }
        } else if (EndsWithCi(asset.name, ".nro")) {
            return &asset;
        } else if (!zip && EndsWithCi(asset.name, ".zip") && !FindCaseInsensitive(asset.name, "debug") &&
                   !FindCaseInsensitive(asset.name, "symbols") && !FindCaseInsensitive(asset.name, "source")) {
            zip = &asset;
        }
    }
    return zip;
}

// the releases API of https://github.com/<owner>/<repo> or of
// https://gitlab.com/<group>/<project>, newest release first.
auto ReleasesApiUrl(std::string_view url, std::string& out, bool& gitlab) -> bool {
    while (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    if (url.ends_with(".git")) {
        url.remove_suffix(4);
    }

    constexpr std::string_view github_prefix{"https://github.com/"};
    constexpr std::string_view gitlab_prefix{"https://gitlab.com/"};
    gitlab = url.starts_with(gitlab_prefix);
    if (!gitlab && !url.starts_with(github_prefix)) {
        return false;
    }

    const auto path = url.substr(gitlab ? gitlab_prefix.size() : github_prefix.size());
    if (path.empty() || !IsSafeManifestPath(path) || path.find('/') == std::string_view::npos ||
        (!gitlab && path.find('/') != path.rfind('/'))) {
        return false;
    }
    if (!std::ranges::all_of(path, [](char c){ return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/'; })) {
        return false;
    }

    if (gitlab) {
        std::string project;
        for (const auto c : path) {
            project += c == '/' ? std::string{"%2F"} : std::string{c};
        }
        out = "https://gitlab.com/api/v4/projects/" + project + "/releases?per_page=10";
    } else {
        out = "https://api.github.com/repos/" + std::string{path} + "/releases?per_page=10";
    }
    return true;
}

auto ResolveReleaseAsset(const Entry& e, ReleaseAsset& out) -> Result {
    if (!e.release_tag.empty()) {
        out = {e.asset, e.download_url, e.sha256};
        R_SUCCEED();
    }

    std::string api;
    bool gitlab{};
    R_UNLESS(ReleasesApiUrl(e.url, api, gitlab), Result_AppstoreNoReleaseAsset);

    const auto result = curl::Api().ToMemory(
        curl::Url{api},
        curl::Header{
            { "Accept", gitlab ? "application/json" : "application/vnd.github+json" },
        }
    );
    R_UNLESS(result.success, Result_AppstoreFailedZipDownload);

    auto document = yyjson_read(reinterpret_cast<const char*>(result.data.data()), result.data.size(), YYJSON_READ_NOFLAG);
    R_UNLESS(document, Result_AppstoreNoReleaseAsset);
    ON_SCOPE_EXIT(yyjson_doc_free(document));

    const auto releases = yyjson_doc_get_root(document);
    R_UNLESS(releases && yyjson_is_arr(releases), Result_AppstoreNoReleaseAsset);

    // each release's assets, newest release first.
    std::vector<std::vector<ReleaseAsset>> release_assets;
    size_t index{}, max{};
    yyjson_val* release{};
    yyjson_arr_foreach(releases, index, max, release) {
        const auto tag = yyjson_get_str(yyjson_obj_get(release, "tag_name"));
        if (e.release != "latest" && (!tag || e.release != tag)) {
            continue;
        }

        // GitHub: assets[].browser_download_url, GitLab: assets.links[].url.
        auto list = yyjson_obj_get(release, "assets");
        if (gitlab) {
            list = yyjson_obj_get(list, "links");
        }

        auto& assets = release_assets.emplace_back();
        size_t asset_index{}, asset_max{};
        yyjson_val* asset{};
        yyjson_arr_foreach(list, asset_index, asset_max, asset) {
            const auto name = yyjson_get_str(yyjson_obj_get(asset, "name"));
            auto link = yyjson_get_str(yyjson_obj_get(asset, gitlab ? "direct_asset_url" : "browser_download_url"));
            if (!link && gitlab) {
                link = yyjson_get_str(yyjson_obj_get(asset, "url"));
            }
            if (!name || !link || !IsSafePackageName(name) || !std::string_view{link}.starts_with("https://")) {
                continue;
            }

            const auto digest_str = yyjson_get_str(yyjson_obj_get(asset, "digest"));
            std::string_view digest{digest_str ? digest_str : ""};
            if (digest.starts_with("sha256:")) {
                digest.remove_prefix(7);
            }
            assets.emplace_back(name, link, std::string{digest});
        }
    }

    // the asset the catalog lists, even when a newer release came out since:
    // the store shows that version, and offers the next one once the catalog
    // has it.
    if (e.sha256.size() == 64) {
        for (const auto& assets : release_assets) {
            for (const auto& asset : assets) {
                if (!strcasecmp(asset.sha256.c_str(), e.sha256.c_str())) {
                    out = asset;
                    R_SUCCEED();
                }
            }
        }
    }

    for (const auto& assets : release_assets) {
        if (const auto picked = PickReleaseAsset(e, assets)) {
            out = *picked;
            R_SUCCEED();
        }
    }

    R_THROW(Result_AppstoreNoReleaseAsset);
}

auto ReadZipNames(const fs::FsPath& path, std::vector<std::string>& out) -> Result {
    zlib_filefunc64_def file_func;
    mz::FileFuncStdio(&file_func);
    auto zfile = unzOpen2_64(path, &file_func);
    R_UNLESS(zfile, Result_UnzOpen2_64);
    ON_SCOPE_EXIT(unzClose(zfile));

    for (auto rc = unzGoToFirstFile(zfile); rc == UNZ_OK; rc = unzGoToNextFile(zfile)) {
        fs::FsPath name;
        R_UNLESS(UNZ_OK == unzGetCurrentFileInfo64(zfile, nullptr, name, sizeof(name), nullptr, 0, nullptr, 0), Result_UnzGetCurrentFileInfo64);
        out.emplace_back(name.s);
    }
    R_SUCCEED();
}

// how a release zip is laid out. one folder holding everything is dropped,
// as brewstore does. a zip with a switch folder mirrors the sd card: that
// folder goes to the sd card's root, and loose files (readmes) are left out.
struct ZipLayout {
    std::string strip;
    bool sd_root{};
};

auto GetZipLayout(const std::vector<std::string>& names) -> ZipLayout {
    ZipLayout layout;

    std::string_view root;
    for (const std::string_view name : names) {
        const auto slash = name.find('/');
        if (slash == std::string_view::npos || !slash || (!root.empty() && name.substr(0, slash) != root)) {
            root = {};
            break;
        }
        root = name.substr(0, slash);
    }
    if (!root.empty() && !(root.size() == 6 && StartsWithCi(root, "switch"))) {
        layout.strip = std::string{root} + "/";
    }

    layout.sd_root = std::ranges::any_of(names, [&layout](std::string_view name){
        return StartsWithCi(name.substr(std::min(layout.strip.size(), name.size())), "switch/");
    });
    return layout;
}

// where a brewstore package goes: its target, /switch/<name> otherwise.
auto ReleaseTarget(const Entry& e, fs::FsPath& out) -> bool {
    if (e.target.empty()) {
        out = fs::AppendPath("/switch", e.name);
        return true;
    }

    std::string_view target{e.target};
    if (target.starts_with("sdmc:")) {
        target.remove_prefix(5);
    }
    while (target.size() > 1 && target.ends_with('/')) {
        target.remove_suffix(1);
    }
    if (target != "/" && (!target.starts_with('/') || !IsSafeManifestPath(target))) {
        return false;
    }

    out = target;
    return true;
}

// a brewstore package is a release asset rather than an HB App Store zip: a
// .nro put in its folder, or a zip extracted there. its files are listed in
// manifest.install and its version in info.json, as for the HB App Store, so
// updating and removing it work the same.
auto InstallReleaseApp(ProgressBox* pbox, const Entry& entry) -> Result {
    static const fs::FsPath asset_out{"/switch/sphaira/cache/appstore/temp.asset"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    fs::FsPath target;
    R_UNLESS(ReleaseTarget(entry, target), Result_AppstoreFailedParseManifest);

    if (entry.release_tag.empty()) {
        pbox->NewTransfer("Downloading json"_i18n);
    }
    ReleaseAsset asset;
    R_TRY(ResolveReleaseAsset(entry, asset));
    R_TRY(pbox->ShouldExitResult());

    R_UNLESS(IsSafePackageName(asset.name) && IsSupportedReleaseAsset(asset.name), Result_AppstoreNoReleaseAsset);
    const auto zip = EndsWithCi(asset.name, ".zip");
    R_UNLESS(zip || std::strcmp(target, "/") ||
        (!entry.release_tag.empty() && entry.marker == "/" + asset.name), Result_AppstoreFailedParseManifest);

    fs.DeleteFile(asset_out);
    ON_SCOPE_EXIT(fs.DeleteFile(asset_out));

    pbox->NewTransfer(i18n::Reorder("Downloading ", entry.title));
    const auto download = curl::Api().ToFile(
        curl::Url{asset.url},
        curl::Path{asset_out},
        curl::OnProgress{pbox->OnDownloadProgressCallback()}
    );
    R_UNLESS(download.success, Result_AppstoreFailedZipDownload);
    R_TRY(pbox->ShouldExitResult());

    // the catalog's sha256 is the asset's.
    if (entry.sha256.size() == 64) {
        pbox->NewTransfer("Checking SHA-256"_i18n);
        std::string hash;
        R_TRY(hash::Hash(pbox, hash::Type::Sha256, &fs, asset_out, hash));
        R_UNLESS(!strcasecmp(hash.c_str(), entry.sha256.c_str()), Result_AppstoreFailedMd5);
    }

    // the files, from the sd card's root, as manifest.install lists them.
    std::vector<std::string> files;
    if (zip) {
        ZipLayout layout;
        if (!entry.strip_prefix.empty()) {
            R_UNLESS(!entry.strip_prefix.starts_with('/') && IsSafeManifestPath(entry.strip_prefix),
                Result_AppstoreFailedParseManifest);
            layout.strip = entry.strip_prefix;
            if (!layout.strip.ends_with('/')) {
                layout.strip += '/';
            }
        } else if (std::strcmp(target, "/")) {
            std::vector<std::string> names;
            R_TRY(ReadZipNames(asset_out, names));
            layout = GetZipLayout(names);
        }
        const fs::FsPath base = layout.sd_root ? "/" : target.s;

        R_TRY(thread::TransferUnzipAll(pbox, asset_out, &fs, base, [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
            std::string_view file{name.s};
            if (!file.starts_with(layout.strip)) {
                return false;
            }
            file.remove_prefix(layout.strip.size());
            if ((layout.sd_root && !StartsWithCi(file, "switch/")) || !IsSafeManifestPath(file) ||
                file.starts_with('/') || file.size() + std::strlen(base) + 1 >= sizeof(path.s)) {
                return false;
            }

            path = fs::AppendPath(base, file);
            if (!file.ends_with('/')) {
                files.emplace_back(path.s + 1);
            }
            return true;
        }));
    } else {
        const auto path = fs::AppendPath(target, asset.name);
        fs.CreateDirectoryRecursively(target);
        fs.DeleteFile(path);
        R_TRY(fs.RenameFile(asset_out, path));
        files.emplace_back(path.s + 1);
    }
    R_UNLESS(!files.empty(), Result_AppstoreFailedParseManifest);

    // what the previous version had and this one doesn't.
    for (const auto& old_file : LoadAndParseManifest(entry)) {
        if (std::ranges::none_of(files, [&old_file](const auto& file){ return !strcasecmp(file.c_str(), old_file.path); })) {
            fs.DeleteFile(fs::AppendPath("/", old_file.path));
        }
    }

    fs.CreateDirectoryRecursively(BuildPackageCachePath(entry));

    std::string manifest;
    for (const auto& file : files) {
        manifest += "U: " + file + "\n";
    }
    R_TRY(fs.write_entire_file(BuildManifestCachePath(entry), {reinterpret_cast<const u8*>(manifest.data()), manifest.size()}));

    auto info = yyjson_mut_doc_new(nullptr);
    R_UNLESS(info, Result_AppstoreFailedParseManifest);
    ON_SCOPE_EXIT(yyjson_mut_doc_free(info));
    const auto root = yyjson_mut_obj(info);
    yyjson_mut_doc_set_root(info, root);
    yyjson_mut_obj_add_strcpy(info, root, "version", entry.version.c_str());

    size_t info_size{};
    const auto info_json = yyjson_mut_write(info, YYJSON_WRITE_NOFLAG, &info_size);
    R_UNLESS(info_json, Result_AppstoreFailedParseManifest);
    ON_SCOPE_EXIT(std::free(info_json));
    R_TRY(fs.write_entire_file(BuildInfoCachePath(entry), {reinterpret_cast<const u8*>(info_json), info_size}));

    log_write("[APPSTORE] installed %s from %s\n", entry.name.c_str(), asset.name.c_str());
    R_SUCCEED();
}

} // namespace

EntryMenu::EntryMenu(Entry& entry, const LazyImage& default_icon, Menu& menu)
: MenuBase{entry.title, MenuFlag_None}
, m_entry{entry}
, m_default_icon{default_icon}
, m_menu{menu} {
    this->SetActions(
        std::make_pair(Button::DPAD_DOWN | Button::RS_DOWN, Action{[this](){
            if (m_index < (m_options.size() - 1)) {
                SetIndex(m_index + 1);
                App::PlaySoundEffect(SoundEffect::Focus);
            }
        }}),
        std::make_pair(Button::DPAD_UP | Button::RS_UP, Action{[this](){
            if (m_index != 0) {
                SetIndex(m_index - 1);
                App::PlaySoundEffect(SoundEffect::Focus);
            }
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            options->Add<SidebarEntryCallback>("More by Author"_i18n, [this](){
                m_menu.SetAuthor();
                SetPop();
            }, true);

            // the feedback goes to the HB App Store's server.
            if (!m_entry.release_asset) {
                options->Add<SidebarEntryCallback>("Leave Feedback"_i18n, [this](){
                    std::string out;
                    std::string header = "Leave feedback for " + m_entry.title;
                    if (R_SUCCEEDED(swkbd::ShowText(out, header.c_str())) && !out.empty()) {
                        const auto post = "name=" "switch_user" "&package=" + m_entry.name + "&message=" + out;
                        const auto file = BuildFeedbackCachePath(m_entry);

                        curl::Api().ToAsync(
                            curl::Url{URL_POST_FEEDBACK},
                            curl::Path{file},
                            curl::Fields{post},
                            curl::StopToken{this->GetToken()},
                            curl::OnComplete{[](auto& result){
                                if (result.success) {
                                    log_write("got feedback!\n");
                                } else {
                                    log_write("failed to send feedback :(");
                                }
                            }
                        });
                    }
                }, true);
            }

            if (platform::SupportsWebApplet && App::IsApplication() && !m_entry.url.empty()) {
                options->Add<SidebarEntryCallback>("Visit Website"_i18n, [this](){
                    WebShow(m_entry.url);
                });
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::L2, Action{"Files"_i18n, [this](){
            m_show_file_list ^= 1;

            if (m_show_file_list && !m_manifest_list && m_file_list_state == ImageDownloadState::None) {
                m_file_list_state = ImageDownloadState::Progress;
                const auto path = BuildManifestCachePath(m_entry);
                std::vector<u8> data;

                if (R_SUCCEEDED(fs::FsNativeSd().read_entire_file(path, data))) {
                    m_file_list_state = ImageDownloadState::Done;
                    data.push_back('\0');
                    m_manifest_list = std::make_unique<ScrollableText>((const char*)data.data(), 0, 374, 250, 768, 18);
                } else if (m_entry.release_asset) {
                    // a release asset's files are only known once installed.
                    m_show_file_list = false;
                    m_file_list_state = ImageDownloadState::None;
                } else {
                    curl::Api().ToMemoryAsync(
                        curl::Url{BuildManifestUrl(m_entry)},
                        curl::StopToken{this->GetToken()},
                        curl::OnComplete{[this](auto& result){
                            if (result.success) {
                                m_file_list_state = ImageDownloadState::Done;
                                result.data.push_back('\0');
                                m_manifest_list = std::make_unique<ScrollableText>((const char*)result.data.data(), 0, 374, 250, 768, 18);
                            } else {
                                m_file_list_state = ImageDownloadState::Failed;
                            }
                        }}
                    );
                }
            }
        }})
    );

    SetTitleSubHeading("by "_i18n + m_entry.author);

    m_details = std::make_unique<ScrollableText>(m_entry.details, 0, 374, 250, 768, 18);
    m_changelog = std::make_unique<ScrollableText>(m_entry.changelog, 0, 374, 250, 768, 18);

    m_show_changlog ^= 1;
    ShowChangelogAction();

    const auto path = BuildBannerCachePath(m_entry);
    const auto url = BuildBannerUrl(m_entry);
    m_banner.cached = EntryLoadImageFile(path, m_banner);

    // race condition if we pop the widget before the download completes
    // brewstore packages have no banner.
    if (!m_entry.release_asset || !m_entry.banner_url.empty()) {
        curl::Api().ToFileAsync(
            curl::Url{url},
            curl::Path{path},
            curl::Flags{curl::Flag_Cache},
            curl::StopToken{this->GetToken()},
            curl::OnComplete{[this, path](auto& result){
                if (result.success) {
                    if (result.code == 304) {
                        m_banner.cached = false;
                    } else {
                        EntryLoadImageFile(path, m_banner);
                    }
                }
            }
        });
    }

    SetSubHeading(m_entry.binary);
    SetSubHeading(m_entry.description);
    UpdateOptions();

    // todo: see Draw()
    // const Vec4 v{75, 110, 370, 155};
    // const Vec2 pad{10, 10};
    // m_list = std::make_unique<List>(3, 3, v, pad);
}

EntryMenu::~EntryMenu() {
}

void EntryMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (m_show_file_list) {
        if (m_manifest_list) {
            m_manifest_list->Update(controller, touch);
        }
    } else {
        m_detail_changelog->Update(controller, touch);
    }
}

void EntryMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    constexpr Vec4 line_vec(30, 86, 1220, 646);
    constexpr Vec4 banner_vec(70, line_vec.y + 20, 848.f, 208.f);
    constexpr Vec4 icon_vec(968, line_vec.y + 30, 256, 150);
    constexpr Vec4 grid_vec(icon_vec.x - 50, line_vec.y + 1, line_vec.w, line_vec.h - line_vec.y - 1);

    // nvgSave(vg);
    // nvgScissor(vg, line_vec.x, line_vec.y, line_vec.w - line_vec.x, line_vec.h - line_vec.y); // clip
    // ON_SCOPE_EXIT(nvgRestore(vg));

    gfx::drawRect(vg, grid_vec, theme->GetColour(ThemeEntryID_GRID));
    DrawIcon(vg, m_banner, m_entry.image.image ? m_entry.image : m_default_icon, banner_vec, false);
    DrawIcon(vg, m_entry.image, m_default_icon, icon_vec);

    constexpr float text_start_x = icon_vec.x;// - 10;
    float text_start_y = 218 + line_vec.y;
    const float text_inc_y = 32;
    const float font_size = 20;

    gfx::drawTextArgs(vg, text_start_x, text_start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "version: %s"_i18n.c_str(), m_entry.version.c_str());
    text_start_y += text_inc_y;
    gfx::drawTextArgs(vg, text_start_x, text_start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "updated: %s"_i18n.c_str(), m_entry.updated.c_str());
    text_start_y += text_inc_y;
    gfx::drawTextArgs(vg, text_start_x, text_start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "category: %s"_i18n.c_str(), i18n::get(m_entry.category).c_str());
    text_start_y += text_inc_y;
    gfx::drawTextArgs(vg, text_start_x, text_start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "extracted: %s"_i18n.c_str(), utils::formatSizeStorage(m_entry.extracted).c_str());
    text_start_y += text_inc_y;
    gfx::drawTextArgs(vg, text_start_x, text_start_y, font_size, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "app_dls: %s"_i18n.c_str(), AppDlToStr(m_entry.app_dls).c_str());
    text_start_y += text_inc_y;

    // todo: rewrite this mess and use list
    constexpr float mm = 0;//20;
    constexpr Vec4 block{968.f + mm, 110.f, 256.f - mm*2, 60.f};
    const float x = block.x;
    float y = 1.f + text_start_y + (text_inc_y * 3) ;
    const float h = block.h;
    const float w = block.w;

    for (s32 i = m_options.size() - 1; i >= 0; i--) {
        const auto& option = m_options[i];
        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, Vec4{x, y, w, h});
        }

        gfx::drawTextArgs(vg, x + w / 2, y + h / 2, 22, NVG_ALIGN_MIDDLE | NVG_ALIGN_CENTER, theme->GetColour(text_id), option.display_text.c_str());
        y -= block.h + 18;
    }

    if (m_show_file_list) {
        if (m_manifest_list) {
            m_manifest_list->Draw(vg, theme);
        } else if (m_file_list_state == ImageDownloadState::Progress) {
            gfx::drawText(vg, 110, 374, 18, theme->GetColour(ThemeEntryID_TEXT), "Loading..."_i18n.c_str());
        } else if (m_file_list_state == ImageDownloadState::Failed) {
            gfx::drawText(vg, 110, 374, 18, theme->GetColour(ThemeEntryID_TEXT), "Failed to download manifest"_i18n.c_str());
        }
    } else {
        m_detail_changelog->Draw(vg, theme);
    }
}

void EntryMenu::ShowChangelogAction() {
    std::function<void()> func = std::bind(&EntryMenu::ShowChangelogAction, this);
    m_show_changlog ^= 1;
    m_show_file_list = false;

    if (m_show_changlog) {
        SetAction(Button::L, Action{"Details"_i18n, func});
        m_detail_changelog = m_changelog.get();
    } else {
        SetAction(Button::L, Action{"Changelog"_i18n, func});
        m_detail_changelog = m_details.get();
    }
}

void EntryMenu::UpdateOptions() {
    const auto launch = [this](){
        nro_launch(m_entry.binary);
    };

    const auto install = [this](){
        App::Push<ProgressBox>(m_entry.image.image, "Downloading "_i18n, m_entry.title, [this](auto pbox){
            return m_entry.release_asset ? InstallReleaseApp(pbox, m_entry) : InstallApp(pbox, m_entry);
        }, [this](Result rc){
            homebrew::SignalChange();
            App::PushErrorBox(rc, "Failed to install app"_i18n);

            if (R_SUCCEEDED(rc)) {
                App::Notify(i18n::Reorder("Downloaded ", m_entry.title));
                if (m_entry.release_asset && m_entry.binary.empty()) {
                    m_entry.binary = FindInstalledNro(m_entry);
                }
                m_entry.status = EntryStatus::Installed;
                m_menu.SetDirty();
                UpdateOptions();
            }
        });
    };

    const auto uninstall = [this](){
        App::Push<ProgressBox>(m_entry.image.image, "Uninstalling "_i18n, m_entry.title, [this](auto pbox){
            return UninstallApp(pbox, m_entry);
        }, [this](Result rc){
            homebrew::SignalChange();
            App::PushErrorBox(rc, "Failed to remove app"_i18n);

            if (R_SUCCEEDED(rc)) {
                App::Notify(i18n::Reorder("Removed ", m_entry.title));
                m_entry.status = EntryStatus::Get;
                m_menu.SetDirty();
                UpdateOptions();
            }
        });
    };

    const Option install_option{"Install"_i18n, install};
    const Option update_option{"Update"_i18n, install};
    const Option launch_option{"Launch"_i18n, launch};
    const Option remove_option{"Remove"_i18n, i18n::Reorder("Completely remove ", m_entry.title) + '?', uninstall};

    m_options.clear();
    switch (m_entry.status) {
        case EntryStatus::Get:
            m_options.emplace_back(install_option);
            break;
        case EntryStatus::Installed:
            if (!m_entry.binary.empty() && m_entry.binary != "none") {
                m_options.emplace_back(launch_option);
            }
            m_options.emplace_back(remove_option);
            break;
        case EntryStatus::Local:
            if (!m_entry.binary.empty() && m_entry.binary != "none") {
                m_options.emplace_back(launch_option);
            }
            m_options.emplace_back(update_option);
            break;
        case EntryStatus::Update:
            m_options.emplace_back(update_option);
            m_options.emplace_back(remove_option);
            break;
    }

    SetIndex(0);
}

void EntryMenu::SetIndex(s64 index) {
    m_index = index;
    const auto option = m_options[m_index];
    if (option.confirm_text.empty()) {
        SetAction(Button::A, Action{option.display_text, option.func});
    } else {
        SetAction(Button::A, Action{option.display_text, [this, option](){
            App::Push<OptionBox>(option.confirm_text, "No"_i18n, "Yes"_i18n, 1, [this, option](auto op_index){
                if (op_index && *op_index) {
                    option.func();
                }
            });
        }});
    }
}

Menu::Menu(u32 flags) : grid::Menu{"AppStore"_i18n, flags} {
    fs::FsNativeSd fs;
    fs.CreateDirectoryRecursively(CACHE_PATH);
    fs.CreateDirectoryRecursively("/config/sphaira");
    LoadStoreSources();
    if (m_filter.Get() < 0 || m_filter.Get() >= Filter_MAX) m_filter.Set(Filter_All);
    if (m_sort.Get() < 0 || m_sort.Get() > SortType_Alphabetical) m_sort.Set(SortType_Updated);
    if (m_order.Get() < 0 || m_order.Get() > OrderType_Ascending) m_order.Set(OrderType_Descending);
    if (m_layout.Get() < 0 || m_layout.Get() > LayoutType::LayoutType_GridDetail) m_layout.Set(LayoutType::LayoutType_GridDetail);

    this->SetActions(
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            if (m_is_author) {
                m_is_author = false;
                if (m_is_search) {
                    SetSearch(m_search_term);
                } else {
                    SetFilter();
                }

                SetIndex(m_entry_author_jump_back);
                if (m_entry_author_jump_back >= 9) {
                    m_list->SetYoff((((m_entry_author_jump_back - 9) + 3) / 3) * m_list->GetMaxY());
                } else {
                    m_list->SetYoff(0);
                }
            } else if (m_is_search) {
                m_is_search = false;
                SetFilter();
                SetIndex(m_entry_search_jump_back);
                if (m_entry_search_jump_back >= 9) {
                    m_list->SetYoff(0);
                    m_list->SetYoff((((m_entry_search_jump_back - 9) + 3) / 3) * m_list->GetMaxY());
                } else {
                    m_list->SetYoff(0);
                }
            } else {
                SetPop();
            }
        }}),
        std::make_pair(Button::A, Action{"Info"_i18n, [this](){
            if (m_entries_current.empty()) {
                // log_write("pushing A when empty: size: %zu count: %zu\n", repo_json.size(), m_entries_current.size());
                return;
            }
            App::Push<EntryMenu>(m_entries[m_entries_current[m_index]], m_default_image, *this);
        }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            auto options = std::make_unique<Sidebar>("AppStore Options"_i18n, Sidebar::Side::RIGHT);
            ON_SCOPE_EXIT(App::Push(std::move(options)));

            SidebarEntryArray::Items filter_items;
            filter_items.push_back("All"_i18n);
            filter_items.push_back("Games"_i18n);
            filter_items.push_back("Emulators"_i18n);
            filter_items.push_back("Tools"_i18n);
            filter_items.push_back("Advanced"_i18n);
            filter_items.push_back("Themes"_i18n);
            filter_items.push_back("Legacy"_i18n);
            filter_items.push_back("Misc"_i18n);

            SidebarEntryArray::Items sort_items;
            sort_items.push_back("Updated"_i18n);
            sort_items.push_back("Downloads"_i18n);
            sort_items.push_back("Size"_i18n);
            sort_items.push_back("Alphabetical"_i18n);

            SidebarEntryArray::Items order_items;
            order_items.push_back("Descending"_i18n);
            order_items.push_back("Ascending"_i18n);

            SidebarEntryArray::Items layout_items;
            layout_items.push_back("List"_i18n);
            layout_items.push_back("Icon"_i18n);
            layout_items.push_back("Grid"_i18n);

            options->Add<SidebarEntryCallback>("Store Source"_i18n, [this](){
                ShowStoreSelector();
            }, true, "Current store: "_i18n + m_store_sources[m_active_store].name);

            if (m_active_store > 0) {
                options->Add<SidebarEntryCallback>("Delete Current Store"_i18n, [this](){
                    DeleteActiveStore();
                }, true);
            }

            options->Add<SidebarEntryArray>("Filter"_i18n, filter_items, [this](s64& index_out){
                m_filter.Set(index_out);
                SetFilter();
            }, m_filter.Get());

            options->Add<SidebarEntryArray>("Sort"_i18n, sort_items, [this](s64& index_out){
                m_sort.Set(index_out);
                SortAndFindLastFile();
            }, m_sort.Get());

            options->Add<SidebarEntryArray>("Order"_i18n, order_items, [this](s64& index_out){
                m_order.Set(index_out);
                SortAndFindLastFile();
            }, m_order.Get());

            options->Add<SidebarEntryArray>("Layout"_i18n, layout_items, [this](s64& index_out){
                m_layout.Set(index_out);
                OnLayoutChange();
            }, m_layout.Get());

            options->Add<SidebarEntryCallback>("Search"_i18n, [this](){
                std::string out;
                if (R_SUCCEEDED(swkbd::ShowText(out, "Search for app"_i18n.c_str())) && !out.empty()) {
                    SetSearch(out);
                    log_write("got %s\n", out.c_str());
                }
            });
        }})
    );

    OnLayoutChange();
    StartStoreDownload();
}

void Menu::LoadStoreSources() {
    m_store_sources.clear();
    m_store_sources.push_back({BUILTIN_STORE_NAME, BUILTIN_STORE_URL});

    for (std::size_t i = 1; i < MAX_STORE_SOURCES; ++i) {
        char name_key[32]{};
        char url_key[32]{};
        char name[128]{};
        char url[PATH_MAX]{};
        std::snprintf(name_key, sizeof(name_key), "store_%zu_name", i);
        std::snprintf(url_key, sizeof(url_key), "store_%zu_url", i);
        ini_gets(STORE_CONFIG_SECTION, name_key, "", name, sizeof(name), STORE_CONFIG_PATH);
        ini_gets(STORE_CONFIG_SECTION, url_key, "", url, sizeof(url), STORE_CONFIG_PATH);
        if (!name[0] || !url[0]) {
            ini_gets(STORE_CONFIG_SECTION_LEGACY, name_key, "", name, sizeof(name), STORE_CONFIG_PATH);
            ini_gets(STORE_CONFIG_SECTION_LEGACY, url_key, "", url, sizeof(url), STORE_CONFIG_PATH);
        }

        auto normalized_url = TrimStoreUrl(url);
        if (name[0] && IsRemoteUrl(normalized_url)) {
            m_store_sources.push_back({name, std::move(normalized_url)});
        }
    }

    auto saved = ini_getl(STORE_CONFIG_SECTION, "store_active", -1, STORE_CONFIG_PATH);
    if (saved < 0) {
        saved = ini_getl(STORE_CONFIG_SECTION_LEGACY, "store_active", 0, STORE_CONFIG_PATH);
    }
    m_active_store = std::clamp<s64>(saved, 0, m_store_sources.size() - 1);
    SaveStoreSources();
}

void Menu::SaveStoreSources() {
    ini_putl(STORE_CONFIG_SECTION, "store_active", m_active_store, STORE_CONFIG_PATH);

    for (std::size_t i = 0; i < MAX_STORE_SOURCES; ++i) {
        char name_key[32]{};
        char url_key[32]{};
        std::snprintf(name_key, sizeof(name_key), "store_%zu_name", i);
        std::snprintf(url_key, sizeof(url_key), "store_%zu_url", i);

        if (i < m_store_sources.size()) {
            ini_puts(STORE_CONFIG_SECTION, name_key, m_store_sources[i].name.c_str(), STORE_CONFIG_PATH);
            ini_puts(STORE_CONFIG_SECTION, url_key, m_store_sources[i].url.c_str(), STORE_CONFIG_PATH);
        } else {
            ini_puts(STORE_CONFIG_SECTION, name_key, "", STORE_CONFIG_PATH);
            ini_puts(STORE_CONFIG_SECTION, url_key, "", STORE_CONFIG_PATH);
        }
    }
}

void Menu::ShowStoreSelector() {
    PopupList::Items items;
    items.reserve(m_store_sources.size() + 1);
    for (const auto& source : m_store_sources) {
        items.push_back(source.name);
    }
    items.push_back("Add Custom Store..."_i18n);

    App::Push<PopupList>("Select store source"_i18n, items, [this](auto index){
        if (!index) {
            return;
        }
        if (*index == static_cast<s64>(m_store_sources.size())) {
            AddStoreSource();
        } else {
            SwitchStore(*index);
        }
    }, m_active_store);
}

void Menu::AddStoreSource() {
    if (m_store_sources.size() >= MAX_STORE_SOURCES) {
        App::Notify("The store source limit has been reached"_i18n);
        return;
    }

    std::string name;
    if (R_FAILED(swkbd::ShowText(name, "Store Name"_i18n.c_str(), "Enter a name for the store"_i18n.c_str(), nullptr, 1, 96)) || name.empty()) {
        return;
    }

    std::string url;
    if (R_FAILED(swkbd::ShowText(url, "Store URL"_i18n.c_str(), "Enter custom store API (URL)"_i18n.c_str(), "https://", 8, PATH_MAX - 1))) {
        return;
    }
    url = TrimStoreUrl(std::move(url));
    if (!IsRemoteUrl(url) || url.find_first_of("\r\n\t ") != std::string::npos) {
        App::Notify("Store URL must be a valid HTTP or HTTPS address"_i18n);
        return;
    }

    m_store_sources.push_back({std::move(name), std::move(url)});
    SwitchStore(m_store_sources.size() - 1);
}

void Menu::DeleteActiveStore() {
    if (m_active_store <= 0 || m_active_store >= static_cast<s64>(m_store_sources.size())) {
        return;
    }

    const auto name = m_store_sources[m_active_store].name;
    App::Push<OptionBox>(
        i18n::Reorder("Delete custom store ", "'" + name + "'") + "?",
        "Back"_i18n,
        "Delete"_i18n,
        0,
        [this](auto index){
            if (!index || *index != 1 || m_active_store <= 0) {
                return;
            }
            m_store_sources.erase(m_store_sources.begin() + m_active_store);
            m_active_store = 0;
            SaveStoreSources();
            StartStoreDownload();
        }
    );
}

void Menu::SwitchStore(s64 index) {
    if (index < 0 || index >= static_cast<s64>(m_store_sources.size())) {
        return;
    }
    m_active_store = index;
    SaveStoreSources();
    StartStoreDownload();
    App::Notify("Selected store: "_i18n + m_store_sources[m_active_store].name);
}

void Menu::StartStoreDownload() {
    const auto generation = ++m_store_generation;
    const auto& source = m_store_sources[m_active_store];
    const auto store_id = HashStore(StoreBaseUrl(source.url));

    m_entries.clear();
    for (auto& index : m_entries_index) {
        index.clear();
    }
    m_entries_index_author.clear();
    m_entries_index_search.clear();
    m_entries_current = {};
    m_index = 0;
    m_is_search = false;
    m_is_author = false;
    m_dirty = false;
    m_repo_download_state = ImageDownloadState::Progress;
    SetTitleSubHeading(source.name);
    SetSubHeading("Loading store catalog..."_i18n);

    fs::FsNativeSd fs;
    const auto store_cache = StoreCachePath(store_id);
    fs.CreateDirectoryRecursively(store_cache + "/icons");
    fs.CreateDirectoryRecursively(store_cache + "/banners");
    fs.CreateDirectoryRecursively(store_cache + "/includes");
    const auto repo_url = BuildRepoUrl(source);
    m_repo_path = store_cache + (repo_url == BREWSTORE_URL ? "/catalog.json" : "/repo.json");

    const auto queued = curl::Api().ToFileAsync(
        curl::Url{repo_url},
        curl::Path{m_repo_path},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this, generation, repo_url, store_cache](auto& result){
            if (generation != m_store_generation) {
                return;
            }

            const auto has_cache = fs::FsNativeSd().FileExists(m_repo_path);
            if (result.success || has_cache) {
                // a brewstore catalog lists its manifests, they come next.
                std::vector<std::string> includes;
                if (ReadCatalogIncludes(m_repo_path, repo_url, includes)) {
                    DownloadCatalogIncludes(generation, store_cache, includes);
                } else {
                    OnCatalogReady();
                }
            } else {
                m_repo_download_state = ImageDownloadState::Failed;
                SetSubHeading("Failed to load store catalog"_i18n);
            }
        }}
    );

    if (!queued) {
        m_repo_download_state = ImageDownloadState::Failed;
        SetSubHeading("Failed to queue store download"_i18n);
    }
}

// the manifests a brewstore catalog lists, each cached beside repo.json. one
// that fails leaves its packages out (or uses the cached copy).
void Menu::DownloadCatalogIncludes(u64 generation, const fs::FsPath& store_cache, const std::vector<std::string>& urls) {
    auto remaining = std::make_shared<std::size_t>(urls.size());
    for (const auto& url : urls) {
        const auto queued = curl::Api().ToFileAsync(
            curl::Url{url},
            curl::Path{IncludeCachePath(store_cache, url)},
            curl::Flags{curl::Flag_Cache},
            curl::StopToken{this->GetToken()},
            curl::OnComplete{[this, generation, remaining](auto&){
                if (generation == m_store_generation && !--*remaining) {
                    OnCatalogReady();
                }
            }}
        );
        if (!queued) {
            --*remaining;
        }
    }

    if (!*remaining) {
        OnCatalogReady();
    }
}

void Menu::OnCatalogReady() {
    m_repo_download_state = ImageDownloadState::Done;
    if (HasFocus()) {
        ScanHomebrew();
    }
}

Menu::~Menu() {

}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries_current.size(), [this](bool touch, auto i) {
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

    if (m_entries.empty()) {
        const auto message = m_repo_download_state == ImageDownloadState::Failed
            ? "Failed to load store catalog"_i18n
            : (m_repo_download_state == ImageDownloadState::Done ? "This store has no packages"_i18n : "Loading..."_i18n);
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", message.c_str());
        return;
    }

    if (m_entries_current.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty!"_i18n.c_str());
        return;
    }

    // max images per frame, in order to not hit io / gpu too hard.
    const int image_load_max = 1;
    int image_load_count = 0;

    m_list->Draw(vg, theme, m_entries_current.size(), [this, &image_load_count](auto* vg, auto* theme, auto v, auto pos) {
        const auto& [x, y, w, h] = v;
        const auto index = m_entries_current[pos];
        auto& e = m_entries[index];
        auto& image = e.image;

        // try and load cached image.
        if (image_load_count < image_load_max && !image.image && !image.tried_cache) {
            image.tried_cache = true;
            image.cached = EntryLoadImageFile(BuildIconCachePath(e), image);
            if (image.cached) {
                image_load_count++;
            }
        }

        // lazy load image
        if (!image.image || image.cached) {
            switch (image.state) {
                case ImageDownloadState::None: {
                    const auto path = BuildIconCachePath(e);
                    const auto url = BuildIconUrl(e);
                    const auto entry_name = e.name;
                    const auto generation = m_store_generation;
                    image.state = ImageDownloadState::Progress;
                    const auto queued = curl::Api().ToFileAsync(
                        curl::Url{url},
                        curl::Path{path},
                        curl::Flags{curl::Flag_Cache},
                        curl::StopToken{this->GetToken()},
                        curl::OnComplete{[this, entry_name, generation](auto& result) {
                            if (generation != m_store_generation) {
                                return;
                            }
                            const auto it = std::ranges::find_if(m_entries, [&entry_name](const Entry& entry){
                                return entry.name == entry_name;
                            });
                            if (it == m_entries.end()) {
                                return;
                            }
                            auto& completed_image = it->image;
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
                    if (image_load_count < image_load_max) {
                        image.cached = false;
                        if (!EntryLoadImageFile(BuildIconCachePath(e), e.image)) {
                            image.state = ImageDownloadState::Failed;
                        } else {
                            image_load_count++;
                        }
                    }
                }   break;
                case ImageDownloadState::Failed: {
                }   break;
            }
        }

        const auto selected = pos == m_index;
        const auto image_vec = DrawEntryNoImage(vg, theme, m_layout.Get(), v, selected, e.title.c_str(), e.author.c_str(), e.version.c_str());

        const auto image_scale = 256.0 / image_vec.w;
        DrawIcon(vg, e.image, m_default_image, image_vec.x, image_vec.y, image_vec.w, image_vec.h, true, image_scale);
        // gfx::drawImage(vg, x + 20, y + 20, image_size, image_size_h, image.image ? image.image : m_default_image);

        // todo: fix position on non-grid layout.
        float i_size = 22;
        switch (e.status) {
            case EntryStatus::Get:
                gfx::drawImage(vg, x + w - 30.f, y + 110, i_size, i_size, m_get.image, 20);
                break;
            case EntryStatus::Installed:
                gfx::drawImage(vg, x + w - 30.f, y + 110, i_size, i_size, m_installed.image, 20);
                break;
            case EntryStatus::Local:
                gfx::drawImage(vg, x + w - 30.f, y + 110, i_size, i_size, m_local.image, 20);
                break;
            case EntryStatus::Update:
                gfx::drawImage(vg, x + w - 30.f, y + 110, i_size, i_size, m_update.image, 20);
                break;
        }
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    // log_write("saying we got focus base: size: %zu count: %zu\n", repo_json.size(), m_entries.size());

    if (!m_default_image.image) {
        EntryLoadImageData(App::GetDefaultImageData(), m_default_image);
        EntryLoadImageData(UPDATE_IMAGE_DATA, m_update);
        EntryLoadImageData(GET_IMAGE_DATA, m_get);
        EntryLoadImageData(LOCAL_IMAGE_DATA, m_local);
        EntryLoadImageData(INSTALLED_IMAGE_DATA, m_installed);
    }

    if (m_entries.empty()) {
        // log_write("got focus with empty size: size: %zu count: %zu\n", repo_json.size(), m_entries.size());

        if (m_repo_download_state == ImageDownloadState::Done) {
            // log_write("is done: size: %zu count: %zu\n", repo_json.size(), m_entries.size());
            ScanHomebrew();
        }
    } else {
        if (m_dirty && !m_entries_current.empty()) {
            m_dirty = false;
            const auto& current_entry = m_entries[m_entries_current[m_index]];
            Sort();

            for (u32 i = 0; i < m_entries_current.size(); i++) {
                if (current_entry.name == m_entries[m_entries_current[i]].name) {
                    const auto index = i;
                    const auto row = m_list->GetRow();
                    const auto page = m_list->GetPage();
                    // guesstimate where the position is
                    if (index >= page) {
                        m_list->SetYoff((((index - page) + row) / row) * m_list->GetMaxY());
                    } else {
                        m_list->SetYoff(0);
                    }
                    SetIndex(i);
                    break;
                }
            }
        }
    }
}

void Menu::SetIndex(s64 index) {
    if (m_entries_current.empty()) {
        m_index = 0;
        SetSubHeading("0 / 0");
        return;
    }

    m_index = std::clamp<s64>(index, 0, m_entries_current.size() - 1);
    if (!m_index) {
        m_list->SetYoff(0);
    }

    this->SetSubHeading(std::to_string(m_index + 1) + " / " + std::to_string(m_entries_current.size()));
}

void Menu::ScanHomebrew() {
    App::SetBoostMode(true);
    ON_SCOPE_EXIT(App::SetBoostMode(false));

    m_entries.clear();
    for (auto& index : m_entries_index) {
        index.clear();
    }
    m_entries_index_author.clear();
    m_entries_index_search.clear();
    m_entries_current = {};

    const auto& source = m_store_sources[m_active_store];
    const auto store_base = StoreBaseUrl(source.url);
    const auto store_id = HashStore(store_base);

    // a brewstore catalog: its manifests' packages, all release assets.
    std::vector<std::string> includes;
    if (ReadCatalogIncludes(m_repo_path, BuildRepoUrl(source), includes)) {
        const auto store_cache = StoreCachePath(store_id);
        for (const auto& url : includes) {
            std::vector<Entry> packages;
            if (!from_json(IncludeCachePath(store_cache, url), packages)) {
                continue;
            }
            for (auto& package : packages) {
                if (package.release.empty()) {
                    package.release = "latest";
                }
                if (m_entries.size() < MAX_STORE_PACKAGES) {
                    m_entries.emplace_back(std::move(package));
                }
            }
        }
    } else if (!from_json(m_repo_path, m_entries)) {
        m_repo_download_state = ImageDownloadState::Failed;
        SetSubHeading("The selected store returned an invalid catalog"_i18n);
        return;
    }

    std::erase_if(m_entries, [](const Entry& entry){ return !IsSafePackageName(entry.name); });
    for (auto& entry : m_entries) {
        entry.store_base = store_base;
        entry.store_id = store_id;
        if (entry.title.empty()) {
            entry.title = entry.name;
        }
        if (entry.author.empty()) {
            entry.author = "Unknown"_i18n;
        }

        entry.release_asset |= !entry.release.empty();
        if (entry.release_asset) {
            if (entry.details.empty()) {
                entry.details = entry.description;
            }
            if (entry.published) {
                const auto published = static_cast<time_t>(entry.published);
                struct tm date{};
                if (gmtime_r(&published, &date)) {
                    char updated[16];
                    std::snprintf(updated, sizeof(updated), "%02d/%02d/%04d", date.tm_mday, date.tm_mon + 1, date.tm_year + 1900);
                    entry.updated = updated;
                }
            }
        }
    }

    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult())) {
        log_write("failed to open sd card in appstore scan\n");
        return;
    }

    // Pre-allocate once; retaining this capacity makes source reloads and
    // filter changes substantially cheaper.
    for (auto& index : m_entries_index) {
        index.reserve(m_entries.size());
    }

    for (u32 i = 0; i < m_entries.size(); i++) {
        auto& e = m_entries[i];

        m_entries_index[Filter_All].push_back(i);

        if (e.category == std::string_view{"game"}) {
            m_entries_index[Filter_Games].push_back(i);
        } else if (e.category == std::string_view{"emu"}) {
            m_entries_index[Filter_Emulators].push_back(i);
        } else if (e.category == std::string_view{"tool"}) {
            m_entries_index[Filter_Tools].push_back(i);
        } else if (e.category == std::string_view{"advanced"}) {
            m_entries_index[Filter_Advanced].push_back(i);
        } else if (e.category == std::string_view{"theme"}) {
            m_entries_index[Filter_Themes].push_back(i);
        } else if (e.category == std::string_view{"legacy"}) {
            m_entries_index[Filter_Legacy].push_back(i);
        } else {
            m_entries_index[Filter_Misc].push_back(i);
        }

        unsigned day{}, month{}, year{};
        if (std::sscanf(e.updated.c_str(), "%u/%u/%u", &day, &month, &year) == 3) {
            e.updated_num = day + month * 100 + year * 10000;
        } else {
            e.updated_num = 0;
        }

        e.status = EntryStatus::Get;
        // if binary is present, check for it, if not avalible, report as not installed
        // if there is not a binary path, then we have to trust the info.json
        // this can result in applications being shown as installed even though they
        // are deleted, this includes sys-modules.
        const auto& marker = e.marker.empty() ? e.binary : e.marker;
        if (marker.empty() || marker == "none") {
            ReadFromInfoJson(e);
        } else {
            if (fs.FileExists(marker)) {
                // first check the info.json
                ReadFromInfoJson(e);
                // if we get here, this means that we have the file, but not the .info file
                // report the file as locally installed to match hb-appstore.
                if (e.status == EntryStatus::Get) {
                    // filter out some apps.
                    bool filtered{};

                    // ignore hbmenu if it was replaced with sphaira.
                    if (e.name == "hbmenu") {
                        NacpStruct nacp;
                        if (R_SUCCEEDED(nro_get_nacp(e.binary, nacp))) {
                            filtered = std::strcmp(NacpLanguageEntries(nacp)[0].name, "nx-hbmenu");
                        }
                    }
                    // ignore single retroarch core.
                    else if (e.name == "snes9x_2010") {
                        filtered = true;
                    }
                    // todo: filter
                    // - sys-clk

                    if (!filtered) {
                        e.status = EntryStatus::Local;
                    } else {
                        log_write("filtered: %s path: %s\n", e.name.c_str(), e.binary.c_str());
                    }
                }
            }
        }

        // the .nro an install of a release asset left, to launch it.
        if (e.release_asset && e.binary.empty() && e.status != EntryStatus::Get) {
            e.binary = FindInstalledNro(e);
        }

        // images are lazy loaded. most brewstore packages have no icon.
        e.image.state = e.release_asset && e.icon_url.empty() ? ImageDownloadState::Failed : ImageDownloadState::None;
        e.image.image = 0;
    }

    SetFilter();
    m_repo_download_state = ImageDownloadState::Done;
    SetSubHeading(m_entries.empty() ? "This store has no packages"_i18n : "");
}

void Menu::Sort() {
    // log_write("doing sort: size: %zu count: %zu\n", repo_json.size(), m_entries.size());

    const auto sort = m_sort.Get();
    const auto order = m_order.Get();
    const auto filter = m_filter.Get();

    // returns true if lhs should be before rhs
    const auto sorter = [this, sort, order](EntryMini _lhs, EntryMini _rhs) -> bool {
        const auto& lhs = m_entries[_lhs];
        const auto& rhs = m_entries[_rhs];

        // fallback to name compare if the updated num is the same
        if (lhs.status == EntryStatus::Update && !(rhs.status == EntryStatus::Update)) {
            return true;
        } else if (!(lhs.status == EntryStatus::Update) && rhs.status == EntryStatus::Update) {
            return false;
        } else if (lhs.status == EntryStatus::Installed && !(rhs.status == EntryStatus::Installed)) {
            return true;
        } else if (!(lhs.status == EntryStatus::Installed) && rhs.status == EntryStatus::Installed) {
            return false;
        } else if (lhs.status == EntryStatus::Local && !(rhs.status == EntryStatus::Local)) {
            return true;
        } else if (!(lhs.status == EntryStatus::Local) && rhs.status == EntryStatus::Local) {
            return false;
        } else {
            switch (sort) {
                case SortType_Updated: {
                    if (lhs.updated_num == rhs.updated_num) {
                        return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                    } else if (order == OrderType_Descending) {
                        return lhs.updated_num > rhs.updated_num;
                    } else {
                        return lhs.updated_num < rhs.updated_num;
                    }
                } break;
                case SortType_Downloads: {
                    if (lhs.app_dls == rhs.app_dls) {
                        return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                    } else if (order == OrderType_Descending) {
                        return lhs.app_dls > rhs.app_dls;
                    } else {
                        return lhs.app_dls < rhs.app_dls;
                    }
                } break;
                case SortType_Size: {
                    if (lhs.extracted == rhs.extracted) {
                        return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                    } else if (order == OrderType_Descending) {
                        return lhs.extracted > rhs.extracted;
                    } else {
                        return lhs.extracted < rhs.extracted;
                    }
                } break;
                case SortType_Alphabetical: {
                    if (order == OrderType_Descending) {
                        return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                    } else {
                        return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) > 0;
                    }
                } break;
            }

            std::unreachable();
        }
    };


    char subheader[128]{};
    std::snprintf(subheader, sizeof(subheader), "Filter: %s | Sort: %s | Order: %s"_i18n.c_str(), i18n::get(FILTER_STR[filter]).c_str(), i18n::get(SORT_STR[sort]).c_str(), i18n::get(ORDER_STR[order]).c_str());
    SetTitleSubHeading(subheader);

    std::sort(m_entries_current.begin(), m_entries_current.end(), sorter);
}

void Menu::SortAndFindLastFile() {
    if (m_entries_current.empty()) {
        Sort();
        SetIndex(0);
        return;
    }

    const auto name = GetEntry().name;
    Sort();
    SetIndex(0);

    s64 index = -1;
    for (u64 i = 0; i < m_entries_current.size(); i++) {
        if (name == GetEntry(i).name) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        const auto row = m_list->GetRow();
        const auto page = m_list->GetPage();
        // guesstimate where the position is
        if (index >= page) {
            m_list->SetYoff((((index - page) + row) / row) * m_list->GetMaxY());
        } else {
            m_list->SetYoff(0);
        }
        SetIndex(index);
    }
}

void Menu::SetFilter() {
    m_is_search = false;
    m_is_author = false;

    const auto filter = std::clamp<long>(m_filter.Get(), Filter_All, Filter_MAX - 1);
    m_entries_current = m_entries_index[filter];
    SetIndex(0);
    Sort();
}

void Menu::SetSearch(const std::string& term) {
    if (!m_is_search) {
        m_entry_search_jump_back = m_index;
    }

    m_search_term = term;
    m_entries_index_search.clear();
    const auto query = m_search_term;

    for (u64 i = 0; i < m_entries.size(); i++) {
        const auto& e = m_entries[i];
        if (FindCaseInsensitive(e.title, query) || FindCaseInsensitive(e.author, query) || FindCaseInsensitive(e.description, query)) {
            m_entries_index_search.emplace_back(i);
        }
    }

    m_is_search = true;
    m_entries_current = m_entries_index_search;
    SetIndex(0);
    Sort();
}

void Menu::SetAuthor() {
    if (m_entries_current.empty()) {
        return;
    }

    if (!m_is_author) {
        m_entry_author_jump_back = m_index;
    }

    m_author_term = m_entries[m_entries_current[m_index]].author;
    m_entries_index_author.clear();
    const auto query = m_author_term;

    for (u64 i = 0; i < m_entries.size(); i++) {
        const auto& e = m_entries[i];
        if (FindCaseInsensitive(e.author, query)) {
            m_entries_index_author.emplace_back(i);
        }
    }

    m_is_author = true;
    m_entries_current = m_entries_index_author;
    SetIndex(0);
    Sort();
}

void Menu::OnLayoutChange() {
    m_index = 0;
    grid::Menu::OnLayoutChange(m_list, m_layout.Get());
}

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
    std::memcpy(first_pixel, other.first_pixel, sizeof(first_pixel));
    return *this;
}

LazyImage::~LazyImage() {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

} // namespace sphaira::ui::menu::appstore

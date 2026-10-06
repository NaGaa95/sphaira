#include "utils/ownfoil_api.hpp"

#include "download.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "i18n.hpp"

#include <yyjson.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sphaira::ownfoil::api {
namespace {

// every address ends up as the bare root url of its server.
auto WithScheme(const char* scheme, const std::string& address) -> std::string {
    auto url = scheme + address;
    if (!url.ends_with("/")) {
        url += "/";
    }
    return url;
}

// a supplied address rarely carries a scheme, so it is guessed from its shape:
// an ip, a port or a dotless name is a lan shop and plain http, while a domain
// tries https first, since basic auth has no business going out in the clear.
auto BuildUrls(const std::string& address) -> std::vector<std::string> {
    if (address.starts_with("http://") || address.starts_with("https://")) {
        return {WithScheme("", address)};
    }

    const auto host = address.substr(0, address.find('/'));
    const auto is_ip_literal = !host.empty() && ((host.front() >= '0' && host.front() <= '9') || host.front() == '[');

    // the last colon is a port unless it belongs inside a [::1] style address.
    const auto bracket = host.rfind(']');
    const auto colon = host.rfind(':');
    const auto has_port = colon != std::string::npos && (bracket == std::string::npos || colon > bracket);

    // a dotless or .local name is a machine here, not a certificated domain.
    const auto is_lan_name = host.find('.') == std::string::npos || host.ends_with(".local");

    if (is_ip_literal || has_port || is_lan_name) {
        return {WithScheme("http://", address)};
    }
    return {WithScheme("https://", address), WithScheme("http://", address)};
}

// the url that answered last time, as the entry remembers it: probing costs a
// full connect timeout per address that isn't there.
struct Resolved {
    std::string url{};
    bool used_remote{};
};

// honoured only while the entry's own addresses still produce it, so it can only
// reorder candidates the probe would try anyway - a re-pointed entry can never
// reach the machine it used to name.
auto ResolvedFrom(const Config& config) -> Resolved {
    if (config.resolved_url.empty()) {
        return {};
    }

    for (const auto used_remote : {false, true}) {
        const auto& address = used_remote ? config.remote_address : config.local_address;
        if (address.empty()) {
            continue;
        }
        for (const auto& url : BuildUrls(address)) {
            if (url == config.resolved_url) {
                return {config.resolved_url, used_remote};
            }
        }
    }

    return {};
}

// a toast draws one line, so a multi-line server message is folded into one.
auto Flatten(const std::string& str) -> std::string {
    std::string out;
    out.reserve(str.size());

    for (const auto c : str) {
        const auto ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (ch == ' ' && (out.empty() || out.back() == ' ')) {
            continue;
        }
        out += ch;
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }

    return out;
}

auto GetBool(yyjson_val* obj, const char* key) -> bool {
    const auto v = yyjson_obj_get(obj, key);
    return v && yyjson_is_bool(v) && yyjson_get_bool(v);
}

// empty when missing or null, which the shop's catalogue fields often are.
auto GetString(yyjson_val* obj, const char* key) -> std::string {
    const auto v = yyjson_obj_get(obj, key);
    return v && yyjson_is_str(v) ? yyjson_get_str(v) : "";
}

// a non-200 carries the server's own {"error": "..."}, but a reverse proxy in
// front of it answers for itself, so anything unparseable yields nothing.
auto ParseError(const std::vector<u8>& data) -> std::string {
    if (data.empty()) {
        return {};
    }

    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    if (!doc) {
        return {};
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        return {};
    }

    if (const auto v = yyjson_obj_get(root, "error"); v && yyjson_is_str(v)) {
        return Flatten(yyjson_get_str(v));
    }

    return {};
}

auto ParseHandshake(const std::vector<u8>& data, ConnectResult& out) -> bool {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    if (!doc) {
        out.error = "Could not parse the server's response"_i18n;
        return false;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        out.error = "Could not parse the server's response"_i18n;
        return false;
    }

    if (const auto v = yyjson_obj_get(root, "uid"); v && yyjson_is_str(v)) {
        out.info.uid = yyjson_get_str(v);
    }

    const auto name = yyjson_obj_get(root, "name");
    if (!name || !yyjson_is_str(name)) {
        out.error = "This does not look like an Ownfoil server"_i18n;
        return false;
    }
    out.info.name = yyjson_get_str(name);

    if (const auto v = yyjson_obj_get(root, "version"); v && yyjson_is_str(v)) {
        out.info.version = yyjson_get_str(v);
    }
    if (const auto v = yyjson_obj_get(root, "protocol_version"); v && yyjson_is_int(v)) {
        out.info.protocol_version = yyjson_get_sint(v);
    }
    if (const auto v = yyjson_obj_get(root, "motd"); v && yyjson_is_str(v)) {
        out.info.motd = yyjson_get_str(v);
    }
    out.info.is_public = GetBool(root, "public");

    // a shop that moved says so here, which is the only way a console off its
    // network can hear about it.
    if (const auto v = yyjson_obj_get(root, "remote"); v && yyjson_is_str(v)) {
        out.info.remote_address = yyjson_get_str(v);
    }

    if (const auto features = yyjson_obj_get(root, "features"); features && yyjson_is_obj(features)) {
        out.info.features.shop = GetBool(features, "shop");
        out.info.features.dumps_upload = GetBool(features, "dumps_upload");
        out.info.features.save_backup = GetBool(features, "save_backup");
        out.info.features.resumable_download = GetBool(features, "resumable_download");
        out.info.features.resumable_upload = GetBool(features, "resumable_upload");
    }

    return true;
}

// tries a single url, filling `out` either way.
auto TryConnect(const std::string& url, const Config& config, std::stop_token token, ConnectResult& out) -> bool {
    log_write("[OWNFOIL] handshake with %s\n", url.c_str());

    const auto result = curl::Api().ToMemory(
        curl::Url{url},
        curl::CustomRequest{"OPTIONS"},
        curl::UserPass{config.user, config.pass},
        curl::PreemptiveAuth{true},
        // an error status carries the server's own explanation, worth more than
        // the status alone - and with it kept, only an unreachable server fails
        // the transfer, so the status says whether the handshake worked.
        curl::Flags{curl::Flag_KeepErrorBody},
        curl::StopToken{token}
    );

    if (!result.success) {
        out.error = "Could not reach the server"_i18n;
        return false;
    }
    out.reached = true;

    if (result.code != 200) {
        out.error = ParseError(result.data);

        // a refusal the server phrased itself settles the matter: every address
        // and scheme leads to that same server and the same answer.
        out.refused = !out.error.empty() || result.code == 401 || result.code == 403;

        if (out.error.empty()) {
            if (result.code == 401 || result.code == 403) {
                out.error = "Incorrect username or password"_i18n;
            } else {
                out.error = "Server returned an error"_i18n + " (" + std::to_string(result.code) + ")";
            }
        }
        return false;
    }

    if (!ParseHandshake(result.data, out)) {
        return false;
    }

    out.base_url = url;
    out.success = true;
    return true;
}

// tries one address, over each scheme it could plausibly be served on.
auto TryAddress(const std::string& address, const Config& config, std::stop_token token, ConnectResult& out) -> bool {
    for (const auto& url : BuildUrls(address)) {
        ConnectResult attempt{};
        if (TryConnect(url, config, token, attempt)) {
            out = std::move(attempt);
            return true;
        }

        // once something has answered, its reply is the account of what went
        // wrong; trying another scheme only buries it.
        const auto reached = attempt.reached;
        if (reached || !out.reached) {
            out = std::move(attempt);
        }
        if (reached) {
            break;
        }
    }

    return false;
}

// the shop returns its own stored copies as a path relative to its root, so
// they only become fetchable joined onto the address that answered.
auto ParseImage(yyjson_val* obj, const std::string& base_url, ShopImage& out) -> void {
    if (!obj || !yyjson_is_obj(obj)) {
        return;
    }

    const auto url = yyjson_obj_get(obj, "url");
    if (!url || !yyjson_is_str(url) || !*yyjson_get_str(url)) {
        return;
    }

    const auto local = yyjson_obj_get(obj, "local");
    out.local = local && yyjson_is_bool(local) && yyjson_get_bool(local);
    out.url = yyjson_get_str(url);

    // base_url already carries its trailing '/'.
    if (out.local && out.url.front() == '/') {
        out.url = base_url + out.url.substr(1);
    }
}

// the same, for the Image stored under `key`.
auto ParseImage(yyjson_val* src, const char* key, const std::string& base_url, ShopImage& out) -> void {
    ParseImage(yyjson_obj_get(src, key), base_url, out);
}

// posts one graphql document. FAILONERROR turns an error status into a failed
// transfer, so the status is the thing to read first either way.
auto PostQuery(const std::string& url, const Config& config, std::stop_token token, const char* body, std::vector<u8>& data, std::string& error) -> bool {
    auto result = curl::Api().ToMemory(
        curl::Url{url},
        curl::Fields{body},
        curl::Header{{"Content-Type", "application/json"}},
        curl::UserPass{config.user, config.pass},
        curl::PreemptiveAuth{true},
        curl::Flags{curl::Flag_KeepErrorBody},
        curl::StopToken{token}
    );

    if (token.stop_requested()) {
        return false;
    }

    if (result.code && result.code != 200) {
        error = ParseError(result.data);
        if (error.empty()) {
            if (result.code == 401 || result.code == 403) {
                error = "Incorrect username or password"_i18n;
            } else {
                error = "Server returned an error"_i18n + " (" + std::to_string(result.code) + ")";
            }
        }
        return false;
    }

    if (!result.success) {
        error = "Could not reach the server"_i18n;
        return false;
    }

    data = std::move(result.data);
    return true;
}

// posts one document to the shop's graphql endpoint. `variables` is bare,
// un-braced `"key":value` json, as `IdArray`/`JsonString` produce.
auto FetchGraphql(const std::string& base_url, const Config& config, std::stop_token token, const std::string& query, const std::string& variables, std::vector<u8>& data, std::string& error) -> bool {
    const auto url = base_url + "api/graphql";
    const auto body = "{\"query\":\"" + query + "\",\"variables\":{" + variables + "}}";
    return PostQuery(url, config, token, body.c_str(), data, error);
}

// graphql answers its own failures with a 200, so every reply goes through this
// first and reports whatever the server put in `errors`.
auto OpenData(yyjson_doc* doc, std::string& error) -> yyjson_val* {
    const auto root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root || !yyjson_is_obj(root)) {
        error = "Could not parse the server's response"_i18n;
        return nullptr;
    }

    if (const auto errors = yyjson_obj_get(root, "errors"); errors && yyjson_is_arr(errors) && yyjson_arr_size(errors)) {
        const auto first = yyjson_arr_get_first(errors);
        const auto message = first ? yyjson_obj_get(first, "message") : nullptr;
        error = message && yyjson_is_str(message) ? Flatten(yyjson_get_str(message)) : "The server rejected the request"_i18n;
        return nullptr;
    }

    const auto data_obj = yyjson_obj_get(root, "data");
    if (!data_obj || !yyjson_is_obj(data_obj)) {
        error = "Unexpected response from the server"_i18n;
        return nullptr;
    }

    return data_obj;
}

// unwraps the {data:{apps:{items}}} envelope a page arrives in.
auto OpenApps(yyjson_doc* doc, s64& total, std::string& error) -> yyjson_val* {
    const auto data_obj = OpenData(doc, error);
    if (!data_obj) {
        return nullptr;
    }

    const auto conn = yyjson_obj_get(data_obj, "apps");
    const auto items = conn && yyjson_is_obj(conn) ? yyjson_obj_get(conn, "items") : nullptr;
    if (!items || !yyjson_is_arr(items)) {
        error = "Unexpected response from the server"_i18n;
        return nullptr;
    }

    if (const auto v = yyjson_obj_get(conn, "total"); v && yyjson_is_int(v)) {
        total = yyjson_get_sint(v);
    }

    return items;
}

// the newest update the shop can serve, falling back to what the base file
// reports. either can be absent until metadata extraction has run, and a title
// naming no version is left blank: the raw number is 0 on nearly every game.
auto ParseVersion(yyjson_val* item) -> std::string {
    for (const auto src : {yyjson_obj_get(item, "latestOwnedVersion"), item}) {
        if (!src || !yyjson_is_obj(src)) {
            continue;
        }
        if (const auto v = yyjson_obj_get(src, "displayVersion"); v && yyjson_is_str(v) && *yyjson_get_str(v)) {
            return "v" + std::string{yyjson_get_str(v)};
        }
    }
    return {};
}

// a path under the shop's own root, as its images are, so joined onto it.
auto ParseDownload(yyjson_val* item, const std::string& base_url) -> ShopDownload {
    ShopDownload out{};
    out.url = GetString(item, "downloadUrl");
    out.extension = GetString(item, "downloadExtension");
    if (!out.url.empty() && out.url.front() == '/') {
        out.url = base_url + out.url.substr(1);
    }
    return out;
}

// reads one page of the apps connection into `out`.
auto ParseApps(const std::vector<u8>& data, const std::string& base_url, AppType type, std::vector<ShopApp>& out, s64& total, std::string& error) -> bool {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto items = OpenApps(doc, total, error);
    if (!items) {
        return false;
    }

    size_t idx, max;
    yyjson_val* item;
    yyjson_arr_foreach(items, idx, max, item) {
        if (!yyjson_is_obj(item)) {
            continue;
        }

        ShopApp app{};
        app.type = type;
        app.app_id = GetString(item, "appId");
        // only update and dlc cards ask: a base game's app id is its title id.
        app.title_id = GetString(item, "titleId");
        if (app.title_id.empty()) {
            app.title_id = app.app_id;
        }
        app.version = ParseVersion(item);
        // only a dlc card asks for it.
        app.download = ParseDownload(item, base_url);

        // `titledb` is the app's own catalogue row, `title` the game's. a base
        // game's app id *is* its title id, so that query asks for only the
        // first; updates and dlc need both, each filling what the other lacks.
        for (const auto key : {"titledb", "title"}) {
            const auto src = yyjson_obj_get(item, key);
            if (!src || !yyjson_is_obj(src)) {
                continue;
            }

            if (app.name.empty()) {
                if (const auto v = yyjson_obj_get(src, "name"); v && yyjson_is_str(v)) {
                    app.name = yyjson_get_str(v);
                }
            }
            if (app.publisher.empty()) {
                if (const auto v = yyjson_obj_get(src, "publisher"); v && yyjson_is_str(v)) {
                    app.publisher = yyjson_get_str(v);
                }
            }
            if (app.icon.url.empty()) {
                ParseImage(src, "icon", base_url, app.icon);
            }
            if (app.banner.url.empty()) {
                ParseImage(src, "banner", base_url, app.banner);
            }
        }

        // the dlc's own name won above, so its game's is kept apart for the page
        // that says what the dlc needs.
        if (type == AppType::Dlc) {
            if (const auto game = yyjson_obj_get(item, "title"); game && yyjson_is_obj(game)) {
                app.game_name = GetString(game, "name");
            }
        }

        if (app.name.empty()) {
            app.name = app.app_id;
        }

        out.emplace_back(std::move(app));
    }

    return true;
}

// reads the id-and-version rows the update pre-pass asks for.
auto ParseUpdates(const std::vector<u8>& data, std::vector<ShopUpdate>& out, s64& total, std::string& error) -> bool {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto items = OpenApps(doc, total, error);
    if (!items) {
        return false;
    }

    size_t idx, max;
    yyjson_val* item;
    yyjson_arr_foreach(items, idx, max, item) {
        if (!yyjson_is_obj(item)) {
            continue;
        }

        ShopUpdate update{};
        if (const auto v = yyjson_obj_get(item, "appId"); v && yyjson_is_str(v)) {
            update.app_id = yyjson_get_str(v);
        }
        if (const auto v = yyjson_obj_get(item, "titleId"); v && yyjson_is_str(v)) {
            update.title_id = yyjson_get_str(v);
        }
        if (const auto v = yyjson_obj_get(item, "appVersion"); v && yyjson_is_int(v)) {
            update.version = yyjson_get_sint(v);
        }

        out.emplace_back(std::move(update));
    }

    return true;
}

// reads the one title a page asks for.
auto ParseTitle(const std::vector<u8>& data, const std::string& base_url, ShopTitle& out, std::string& error) -> bool {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    const auto data_obj = OpenData(doc, error);
    if (!data_obj) {
        return false;
    }

    const auto title = yyjson_obj_get(data_obj, "title");
    if (!title || !yyjson_is_obj(title)) {
        error = "The shop has no details for this title"_i18n;
        return false;
    }

    out.name = GetString(title, "name");
    out.publisher = GetString(title, "publisher");
    ParseImage(title, "banner", base_url, out.banner);
    out.intro = GetString(title, "intro");
    out.description = GetString(title, "description");
    out.release_date = GetString(title, "releaseDate");
    out.players = GetString(title, "numberOfPlayers");
    out.rating = GetString(title, "rating");
    out.region = GetString(title, "region");
    // a string in the catalogue, not reliably numeric: unparseable means no size.
    out.size = std::strtoll(GetString(title, "size").c_str(), nullptr, 10);

    size_t idx, max;
    yyjson_val* val;

    if (const auto categories = yyjson_obj_get(title, "category"); categories && yyjson_is_arr(categories)) {
        yyjson_arr_foreach(categories, idx, max, val) {
            if (!yyjson_is_str(val)) {
                continue;
            }
            if (!out.genre.empty()) {
                out.genre += ", ";
            }
            out.genre += yyjson_get_str(val);
        }
    }

    ParseImage(title, "fullBanner", base_url, out.full_banner);

    const auto parse_images = [&](const char* key, std::vector<ShopImage>& images) {
        const auto arr = yyjson_obj_get(title, key);
        if (!arr || !yyjson_is_arr(arr)) {
            return;
        }

        size_t i, n;
        yyjson_val* element;
        yyjson_arr_foreach(arr, i, n, element) {
            ShopImage image{};
            ParseImage(element, base_url, image);
            if (!image.url.empty()) {
                images.emplace_back(std::move(image));
            }
        }
    };

    parse_images("screenshots", out.screenshots);
    parse_images("full", out.full_screenshots);

    // ascending, so the last is the newest the catalogue knows of.
    if (const auto versions = yyjson_obj_get(title, "availableVersions"); versions && yyjson_is_arr(versions)) {
        if (const auto last = yyjson_arr_get_last(versions); last && yyjson_is_obj(last)) {
            if (const auto v = yyjson_obj_get(last, "version"); v && yyjson_is_int(v)) {
                out.latest_version = yyjson_get_sint(v);
                out.latest_date = GetString(last, "releaseDate");
            }
        }
    }

    if (const auto base = yyjson_obj_get(title, "base"); base && yyjson_is_arr(base)) {
        yyjson_arr_foreach(base, idx, max, val) {
            if (!yyjson_is_obj(val)) {
                continue;
            }

            if (const auto v = yyjson_obj_get(val, "downloadSize"); v && yyjson_is_int(v)) {
                out.download_size = yyjson_get_sint(v);
            }

            // the base game is version 0, all the shop offers until it holds an
            // update, and the first thing the version picker lists.
            out.available_version = 0;
            out.available_display = GetString(val, "displayVersion");
            // the shop answers with a row per file, so a base game cataloged
            // twice - an original and a compressed copy - is still one version.
            if (out.versions.empty()) {
                out.versions.emplace_back(ShopVersion{0, out.available_display, ParseDownload(val, base_url)});
            }
            if (const auto owned = yyjson_obj_get(val, "latestOwnedVersion"); owned && yyjson_is_obj(owned)) {
                if (const auto v = yyjson_obj_get(owned, "version"); v && yyjson_is_int(v)) {
                    out.available_version = yyjson_get_sint(v);
                    out.available_display = GetString(owned, "displayVersion");
                }
            }
        }
    }

    // what the install menu offers a choice of: the base game at version 0,
    // then each update the shop holds. the shop returns a row per version it
    // holds, so a version held as two files arrives twice and is listed once.
    if (const auto updates = yyjson_obj_get(title, "updates"); updates && yyjson_is_arr(updates)) {
        yyjson_arr_foreach(updates, idx, max, val) {
            if (!yyjson_is_obj(val)) {
                continue;
            }

            const auto v = yyjson_obj_get(val, "appVersion");
            if (!v || !yyjson_is_int(v)) {
                continue;
            }

            const auto version = yyjson_get_sint(v);
            const auto seen = std::any_of(out.versions.begin(), out.versions.end(), [version](const auto& e) {
                return e.version == version;
            });
            if (!seen) {
                out.versions.emplace_back(ShopVersion{version, GetString(val, "displayVersion"), ParseDownload(val, base_url)});
            }
        }
    }

    // the shop answers in no particular order; the picker offers oldest first.
    std::sort(out.versions.begin(), out.versions.end(), [](const auto& a, const auto& b) {
        return a.version < b.version;
    });

    // a row per version held, so a dlc held at two arrives twice and is listed
    // once, with the newer to install.
    if (const auto dlc = yyjson_obj_get(title, "dlc"); dlc && yyjson_is_arr(dlc)) {
        yyjson_arr_foreach(dlc, idx, max, val) {
            if (!yyjson_is_obj(val)) {
                continue;
            }

            ShopContent content{};
            content.app_id = GetString(val, "appId");
            if (content.app_id.empty()) {
                continue;
            }

            if (const auto v = yyjson_obj_get(val, "appVersion"); v && yyjson_is_int(v)) {
                content.version = yyjson_get_sint(v);
            }
            content.download = ParseDownload(val, base_url);

            const auto seen = std::find_if(out.dlc.begin(), out.dlc.end(), [&](const auto& e) {
                return e.app_id == content.app_id;
            });
            if (seen != out.dlc.end()) {
                if (content.version > seen->version) {
                    seen->version = content.version;
                    seen->download = std::move(content.download);
                }
                continue;
            }

            if (const auto db = yyjson_obj_get(val, "titledb"); db && yyjson_is_obj(db)) {
                content.name = GetString(db, "name");
                content.intro = GetString(db, "intro");
                ParseImage(db, "banner", base_url, content.banner);
            }
            if (content.name.empty()) {
                content.name = content.app_id;
            }

            out.dlc.emplace_back(std::move(content));
        }
    }

    return true;
}

// every id was formatted here from a u64, so there is nothing to escape.
auto IdArray(const std::vector<std::string>& ids) -> std::string {
    std::string out{"["};
    for (const auto& id : ids) {
        if (out.size() > 1) {
            out += ',';
        }
        out += '"';
        out += id;
        out += '"';
    }
    out += ']';
    return out;
}

// text somebody typed, so escaped rather than merely quoted. json takes utf-8
// raw, leaving the quote, the backslash and the control characters.
auto JsonString(const std::string& str) -> std::string {
    std::string out{"\""};
    for (const auto c : str) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", u);
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// what a card draws, per category, and nothing beyond it. a base game's own
// catalogue row carries the lot; an update's app id has no row, so its game's
// fills in, and `titleId` is what opens that game's page; a dlc has a row but no
// icon in it, and carries its game's `titleId` for the installed check plus its
// own download, which the shop won't answer for under a dlc's id. artwork is
// THUMB, the size a card draws it at.
constexpr const char* BASE_FIELDS = "appId displayVersion latestOwnedVersion{displayVersion} titledb{name publisher icon(size:THUMB){url local} banner(size:THUMB){url local}}";
constexpr const char* UPDATE_FIELDS = "appId titleId displayVersion title{name publisher icon(size:THUMB){url local} banner(size:THUMB){url local}}";
constexpr const char* DLC_FIELDS = "appId titleId displayVersion downloadUrl downloadExtension titledb{name publisher banner(size:THUMB){url local}} title{name icon(size:THUMB){url local}}";

// what a page draws beyond its card, at CLIENT for the page and SCREEN for the
// viewer. `availableVersions` is the catalogue's ascending list, so its last is
// the newest version there is, while the base app's `latestOwnedVersion` is the
// newest the shop holds - and only a held version reports a display string, which
// is why both are asked. base, updates and dlc are separate aliases, each asking
// for what is read of it: a row per update for the install menu's picker, a dlc's
// row fields at THUMB, and a download apiece for Install.
// note this needs a shop with the aliased-nested-selection fix: an older one
// hydrates every alias with the first's fields and the dlc arrive nameless.
constexpr const char* TITLE_QUERY = "query($id:ID!){title(titleId:$id){name publisher intro description releaseDate category numberOfPlayers size rating region banner(size:CLIENT){url local} fullBanner:banner(size:SCREEN){url local} availableVersions{version releaseDate} screenshots(size:CLIENT){url local} full:screenshots(size:SCREEN){url local} base:apps(owned:true,appType:[BASE]){displayVersion downloadSize downloadUrl downloadExtension latestOwnedVersion{version displayVersion}} updates:apps(owned:true,appType:[UPDATE]){appVersion displayVersion downloadUrl downloadExtension} dlc:apps(owned:true,appType:[DLC]){appId appVersion downloadUrl downloadExtension titledb{name intro banner(size:THUMB){url local}}}}}";

// the update pre-pass: ids and versions only, for exactly the title ids asked.
constexpr const char* UPDATES_QUERY = "query($page:Int!,$pageSize:Int!,$titleIds:[String!]){apps(owned:true,appType:[UPDATE],groupByAppId:true,filter:{titleId:{in:$titleIds}},page:$page,pageSize:$pageSize){total items{appId titleId appVersion}}}";

// one shape for every category, with the content type, fields and filter each
// needs. the ids ride as variables, not in the document, so the shop's parser
// and validation caches keep hitting however long the console's library gets.
// fills `variables` with this page's, in `FetchGraphql`'s bare-json shape.
auto BuildPageQuery(const CatalogQuery& query, std::string& variables) -> std::string {
    const char* app_type{"BASE"};
    const char* fields{BASE_FIELDS};
    std::string params{"$page:Int!,$pageSize:Int!"};
    // the arguments this category adds to the apps() call, spliced in whole.
    std::string args{};
    variables = "\"page\":" + std::to_string(query.page) + ",\"pageSize\":" + std::to_string(query.page_size);

    switch (query.category) {
        case Category::All:
            break;

        case Category::NewGames:
            params += ",$appIds:[String!]";
            args = ",filter:{appId:{notIn:$appIds}}";
            variables += ",\"appIds\":" + IdArray(query.app_ids);
            break;

        case Category::Updates:
            app_type = "UPDATE";
            fields = UPDATE_FIELDS;
            params += ",$appIds:[String!]";
            args = ",filter:{appId:{in:$appIds}}";
            variables += ",\"appIds\":" + IdArray(query.app_ids);
            break;

        case Category::Dlc:
            app_type = "DLC";
            fields = DLC_FIELDS;
            params += ",$titleIds:[String!],$appIds:[String!]";
            args = ",filter:{titleId:{in:$titleIds},appId:{notIn:$appIds}}";
            variables += ",\"titleIds\":" + IdArray(query.title_ids) + ",\"appIds\":" + IdArray(query.app_ids);
            break;

        // an argument of its own, not a filter field: it is an OR across the
        // name and either id, which a filter's all-AND fields can't express.
        case Category::Search:
            params += ",$search:String";
            args = ",search:$search";
            variables += ",\"search\":" + JsonString(query.search);
            break;
    }

    // `total` is cheap enough for every page to ask: a HAVING would make the
    // server count a grouped query as a derived table, but `owned:true` is
    // filtered before the grouping, so it is a flat COUNT(DISTINCT appId).
    return "query(" + params + "){apps(owned:true,appType:[" + app_type + "],groupByAppId:true" + args
        + ",orderBy:{field:" + query.order_field + ",direction:" + (query.descending ? "DESC" : "ASC")
        + "},page:$page,pageSize:$pageSize){total items{" + fields + "}}}";
}

} // namespace

auto Connect(const Config& config, std::stop_token token) -> ConnectResult {
    ConnectResult out{};
    out.error = "No address configured"_i18n;

    // whatever answered last time goes first, so a reconnect costs one request
    // rather than a walk down every address and scheme again.
    if (const auto resolved = ResolvedFrom(config); !resolved.url.empty()) {
        ConnectResult attempt{};
        if (TryConnect(resolved.url, config, token, attempt)) {
            attempt.used_remote = resolved.used_remote;
            return attempt;
        }

        if (attempt.refused) {
            return attempt;
        }
    }

    if (!config.local_address.empty() && TryAddress(config.local_address, config, token, out)) {
        out.used_remote = false;
        return out;
    }

    // the remote address is the same shop and would answer the same way, while
    // whatever proxies it may answer less helpfully and bury the reason.
    if (out.refused) {
        return out;
    }

    if (!config.remote_address.empty()) {
        ConnectResult remote_attempt{};
        if (TryAddress(config.remote_address, config, token, remote_attempt)) {
            remote_attempt.used_remote = true;
            return remote_attempt;
        }

        // whichever address got an answer explains the failure better than the
        // one that stayed silent.
        if (remote_attempt.reached || !out.reached) {
            return remote_attempt;
        }
    }

    return out;
}

auto FetchApps(const std::string& base_url, const Config& config, std::stop_token token, const CatalogQuery& query, std::vector<ShopApp>& out, s64& total, std::string& error) -> bool {
    out.clear();
    total = 0;

    // an empty list is no constraint to the shop, so a category narrowed by one
    // the console can't supply would answer with the whole catalog rather than
    // the nothing it is; an empty search term matches everything the same way.
    // New games is the exception: with nothing installed, every game is new.
    if ((query.category == Category::Updates && query.app_ids.empty())
        || (query.category == Category::Dlc && query.title_ids.empty())
        || (query.category == Category::Search && query.search.empty())) {
        return true;
    }

    std::string variables;
    const auto doc = BuildPageQuery(query, variables);

    log_write("[OWNFOIL] fetching apps page %d (%d per page, category %d, %s %s)\n",
        static_cast<int>(query.page), static_cast<int>(query.page_size),
        static_cast<int>(query.category), query.order_field, query.descending ? "desc" : "asc");

    std::vector<u8> data;
    if (!FetchGraphql(base_url, config, token, doc, variables, data, error)) {
        return false;
    }

    // each category asks for one kind of content, so the query says what every
    // card on the page is.
    const auto type = query.category == Category::Updates ? AppType::Update
        : query.category == Category::Dlc ? AppType::Dlc
        : AppType::Base;
    return ParseApps(data, base_url, type, out, total, error);
}

auto FetchUpdates(const std::string& base_url, const Config& config, std::stop_token token, const std::vector<std::string>& title_ids, std::vector<ShopUpdate>& out, std::string& error) -> bool {
    out.clear();

    // as above: asking with an empty list would name every update in the shop.
    if (title_ids.empty()) {
        return true;
    }

    const auto ids = IdArray(title_ids);

    // bounded by the console's library rather than the shop, but pageSize is
    // capped server-side all the same, so it is paged.
    static constexpr s64 PAGE_SIZE = 1000;

    for (s64 page = 1; ; page++) {
        const auto variables = "\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(PAGE_SIZE) + ",\"titleIds\":" + ids;

        std::vector<u8> data;
        if (!FetchGraphql(base_url, config, token, UPDATES_QUERY, variables, data, error)) {
            return false;
        }

        const auto before = out.size();
        s64 total{};
        if (!ParseUpdates(data, out, total, error)) {
            return false;
        }

        // a page that added nothing would loop forever on a total that
        // disagrees with what the shop is sending.
        if (out.size() == before || static_cast<s64>(out.size()) >= total) {
            break;
        }
    }

    log_write("[OWNFOIL] shop has updates for %zu of %zu installed titles\n", out.size(), title_ids.size());
    return true;
}

auto FetchTitle(const std::string& base_url, const Config& config, std::stop_token token, const std::string& id, ShopTitle& out, std::string& error) -> bool {
    out = {};

    log_write("[OWNFOIL] fetching title %s\n", id.c_str());

    // the id rides as a variable, so every page asks the one document the shop's
    // caches already hold.
    const auto variables = "\"id\":" + JsonString(id);

    std::vector<u8> data;
    if (!FetchGraphql(base_url, config, token, TITLE_QUERY, variables, data, error)) {
        return false;
    }

    return ParseTitle(data, base_url, out, error);
}

} // namespace sphaira::ownfoil::api

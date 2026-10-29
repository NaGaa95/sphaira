#include "i18n.hpp"
#include "fs.hpp"
#include "log.hpp"
#include <yyjson.h>
#include <vector>
#include <unordered_map>

namespace sphaira::i18n {
namespace {

std::vector<u8> g_i18n_data{};
yyjson_doc* json{};
yyjson_val* root{};
std::unordered_map<std::string, std::string> g_tr_cache{};
Mutex g_mutex{};
std::string g_language_code{"en"};

static WordOrder g_word_order = WordOrder::PhraseName;

static WordOrder DetectWordOrder(const std::string& lang) {
    // SOV Language.
    if (lang == "ja" || lang == "ko" || lang == "tr")
        return WordOrder::NamePhrase;
    
    // Default: SVO Language.
    return WordOrder::PhraseName;
}

static std::string get_internal(std::string_view str, std::string_view fallback) {
    SCOPED_MUTEX(&g_mutex);

    const std::string kkey{str.data(), str.length()};

    if (auto it = g_tr_cache.find(kkey); it != g_tr_cache.end()) {
        return it->second;
    }

    // add default entry
    const auto it = g_tr_cache.emplace(kkey, std::string{fallback}).first;

    if (!json || !root) {
        log_write("no json or root\n");
        return std::string{fallback};
    }

    yyjson_val* node = yyjson_obj_getn(root, str.data(), str.length());
    if (!node && str != fallback) {
        node = yyjson_obj_getn(root, fallback.data(), fallback.length());
        if (node) {
            log_write("\tfallback-key matched: [%s]\n", std::string(fallback).c_str());
        }
    }

    if (!node) {
        log_write("\tfailed to find key: [%s]\n", kkey.c_str());
        return std::string{fallback};
    }

    std::string ret;

    // key > string
    if (const char* val = yyjson_get_str(node)) {
        size_t len = yyjson_get_len(node);
        if (len) {
            ret.assign(val, len);
        }
    }

    // key > array of strings (multi-line)
    if (ret.empty() && yyjson_is_arr(node)) {
        size_t idx, max;
        yyjson_val* elem;
        yyjson_arr_foreach(node, idx, max, elem) {
            if (idx) ret.push_back('\n');

            if (yyjson_is_str(elem)) {
                const char* s = yyjson_get_str(elem);
                size_t len = yyjson_get_len(elem);
                if (s && len) ret.append(s, len);
            }
        }
    }

    if (ret.empty()) {
        log_write("\tfailed to get value: [%s]\n", kkey.c_str());
        ret = std::string{fallback};
    }

    // update entry in cache
    g_tr_cache.insert_or_assign(it, kkey, ret);
    return ret;
}

static std::string get_internal(std::string_view str) {
    return get_internal(str, str);
}

} // namespace

std::string init(long index) {
    SCOPED_MUTEX(&g_mutex);

    g_tr_cache.clear();
    R_TRY_RESULT(romfsInit(), "");
    ON_SCOPE_EXIT( romfsExit() );

    u64 languageCode;
    SetLanguage setLanguage = SetLanguage_ENGB;
    std::string lang_name = "en";

    switch (index) {
        case 0: // auto
            if (R_SUCCEEDED(setGetSystemLanguage(&languageCode))) {
                setMakeLanguage(languageCode, &setLanguage);
            }
            break;

        case 1: setLanguage = SetLanguage_ENGB; break; // "English"
        case 2: setLanguage = SetLanguage_JA; break; // "Japanese"
        case 3: setLanguage = SetLanguage_FR; break; // "French"
        case 4: setLanguage = SetLanguage_DE; break; // "German"
        case 5: setLanguage = SetLanguage_IT; break; // "Italian"
        case 6: setLanguage = SetLanguage_ES; break; // "Spanish"
        case 7: setLanguage = SetLanguage_ZHCN; break; // "Chinese (Simplified)"
        case 8: setLanguage = SetLanguage_KO; break; // "Korean"
        case 9: setLanguage = SetLanguage_NL; break; // "Dutch"
        case 10: setLanguage = SetLanguage_PT; break; // "Portuguese"
        case 11: setLanguage = SetLanguage_RU; break; // "Russian"
        case 12: setLanguage = SetLanguage_ZHTW; break; // "Chinese (Traditional)"
        case 13: lang_name = "se"; break; // "Swedish"
        case 14: lang_name = "vi"; break; // "Vietnamese"
        case 15: lang_name = "uk"; break; // "Ukrainian"
        case 16: lang_name = "tr"; break; // "Turkish"
    }

    switch (setLanguage) {
        case SetLanguage_JA: lang_name = "ja"; break;
        case SetLanguage_FR: lang_name = "fr"; break;
        case SetLanguage_DE: lang_name = "de"; break;
        case SetLanguage_IT: lang_name = "it"; break;
        case SetLanguage_ES: lang_name = "es"; break;
        case SetLanguage_ZHCN: lang_name = "zh-Hans"; break;
        case SetLanguage_KO: lang_name = "ko"; break;
        case SetLanguage_NL: lang_name = "nl"; break;
        case SetLanguage_PT: lang_name = "pt"; break;
        case SetLanguage_RU: lang_name = "ru"; break;
        case SetLanguage_ZHTW: lang_name = "zh-Hant"; break;
        case SetLanguage_ZHHANS: lang_name = "zh-Hans"; break;
        case SetLanguage_ZHHANT: lang_name = "zh-Hant"; break;
        default: break;
    }

    g_word_order = DetectWordOrder(lang_name);
    g_language_code = lang_name;

    const fs::FsPath sdmc_path = "/config/sphaira/i18n/" + lang_name + ".json";
    const fs::FsPath romfs_path = "romfs:/i18n/" + lang_name + ".json";
    fs::FsPath path = sdmc_path;

    // try and load override translation first
    Result rc = fs::FsNativeSd().read_entire_file(path, g_i18n_data);
    if (R_FAILED(rc)) {
        path = romfs_path;
        rc = fs::FsStdio().read_entire_file(path, g_i18n_data);
    }

    if (R_SUCCEEDED(rc)) {
        json = yyjson_read((const char*)g_i18n_data.data(), g_i18n_data.size(), YYJSON_READ_ALLOW_TRAILING_COMMAS|YYJSON_READ_ALLOW_COMMENTS|YYJSON_READ_ALLOW_INVALID_UNICODE);
        if (json) {
            root = yyjson_doc_get_root(json);
            if (root) {
                log_write("opened json: %s\n", path.s);
                return lang_name;
            } else {
                log_write("failed to find root\n");
            }
        } else {
            log_write("failed open json\n");
        }
    } else {
        log_write("failed to read file\n");
    }

    return "";
}

void exit() {
    SCOPED_MUTEX(&g_mutex);

    if (json) {
        yyjson_doc_free(json);
        json = nullptr;
        root = nullptr;
    }
    g_i18n_data.clear();
}

std::string get(std::string_view str) {
    return get_internal(str);
}

std::string get(std::string_view str, std::string_view fallback) {
    return get_internal(str, fallback);
}

std::string GetLanguageCode() {
    SCOPED_MUTEX(&g_mutex);
    return g_language_code;
}

// Reorders sentence structure based on locale.
WordOrder GetWordOrder() {
    return g_word_order;
}

bool WordOrderLocale() {
    return g_word_order == WordOrder::NamePhrase;
}

std::string Reorder(std::string_view phrase, std::string_view name) {
    std::string p = i18n::get(phrase);
    std::string out;
    out.reserve(phrase.length() + name.length());

    if (g_word_order == WordOrder::NamePhrase) {
        out.append(name);
        out.append(p);
    } else {
        out.append(p);
        out.append(name);
    }
    return out;
}

} // namespace sphaira::i18n

namespace literals {

std::string operator""_i18n(const char* str, size_t len) {
    return sphaira::i18n::get({str, len});
}

} // namespace literals

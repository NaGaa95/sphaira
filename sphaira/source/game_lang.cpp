#include "game_lang.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "title_info.hpp"

#include <minIni.h>

#include <strings.h>

namespace sphaira::game_lang {
namespace {

constexpr const char* INI_SECTION = "override_config";
constexpr const char* INI_KEY = "override_language";

} // namespace

s64 Get(u64 app_id) {
    const auto path = fs::AppendPath(title::GetContentsPath(app_id), "config.ini");

    char buf[32]{};
    if (ini_gets(INI_SECTION, INI_KEY, "", buf, sizeof(buf), path) < 0) {
        return 0;
    }

    // empty value or unknown code -> system default.
    // atmosphere ignores invalid codes too.
    for (size_t i = 0; i < LANGUAGE_COUNT; i++) {
        if (!strcasecmp(buf, LANGUAGES[i].code)) {
            return static_cast<s64>(i) + 1;
        }
    }

    return 0;
}

Result Set(u64 app_id, s64 index) {
    R_UNLESS(index >= 0 && index <= static_cast<s64>(LANGUAGE_COUNT), Result_GameLangBadIndex);

    const auto dir = title::GetContentsPath(app_id);
    const auto path = fs::AppendPath(dir, "config.ini");

    if (index == 0) {
        // nothing to remove if the config doesn't exist.
        if (!fs::FileExists(path)) {
            R_SUCCEED();
        }

        // passing NULL as value deletes the key.
        R_UNLESS(ini_puts(INI_SECTION, INI_KEY, NULL, path), Result_FsStdioFailedToWrite);
        log_write("[GAMELANG] removed language override for %016lX\n", app_id);
        R_SUCCEED();
    }

    // ensure /atmosphere/contents/<tid>/ exists before writing the config.
    R_TRY(fs::FsNativeSd().CreateDirectoryRecursively(dir));

    R_UNLESS(ini_puts(INI_SECTION, INI_KEY, LANGUAGES[index - 1].code, path), Result_FsStdioFailedToWrite);
    log_write("[GAMELANG] set language override for %016lX: %s\n", app_id, LANGUAGES[index - 1].code);
    R_SUCCEED();
}

} // namespace sphaira::game_lang

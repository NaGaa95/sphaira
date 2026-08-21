#pragma once

#include <switch.h>

#include <iterator>

namespace sphaira::game_lang {

// language entry, ordered by SetLanguage / NacpStruct.supported_language_flag
// bit index. code is the value passed to atmospheres per-title config.ini:
// /atmosphere/contents/<tid>/config.ini -> [override_config] override_language.
struct Language {
    const char* code; // "ja", "en-US", ...
    const char* name; // display name, i18n key.
};

constexpr inline Language LANGUAGES[]{
    {"ja",     "Japanese"},
    {"en-US",  "English"},
    {"fr",     "French"},
    {"de",     "German"},
    {"it",     "Italian"},
    {"es",     "Spanish"},
    {"zh-CN",  "Chinese (Simplified)"},
    {"ko",     "Korean"},
    {"nl",     "Dutch"},
    {"pt",     "Portuguese"},
    {"ru",     "Russian"},
    {"zh-TW",  "Chinese (Traditional)"},
    {"en-GB",  "British English"},
    {"fr-CA",  "Canadian French"},
    {"es-419", "Latin American Spanish"},
    {"zh-Hans", "Simplified Chinese"},
    {"zh-Hant", "Traditional Chinese"},
    {"pt-BR",  "Portuguese (Brazil)"},
};

// index 0 is reserved for "system default" (no override).
constexpr inline size_t LANGUAGE_COUNT = std::size(LANGUAGES);

// returns the index into LANGUAGES + 1 for the forced language,
// or 0 if no (valid) override is set.
s64 Get(u64 app_id);

// sets the forced launch language for app_id.
// index 0 removes the override, otherwise 1..LANGUAGE_COUNT.
Result Set(u64 app_id, s64 index);

} // namespace sphaira::game_lang

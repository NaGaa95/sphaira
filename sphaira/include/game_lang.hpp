#pragma once

#include <switch.h>

#include <iterator>

namespace sphaira::game_lang {

// NACP language bits differ from SetLanguage order.
struct Language {
    const char* code; // "ja", "en-US", ...
    const char* name; // display name, i18n key.
    u32 supported_language_flag;
};

constexpr inline Language LANGUAGES[]{
    {"ja",     "Japanese", BIT(2)},
    {"en-US",  "English", BIT(0)},
    {"fr",     "French", BIT(3)},
    {"de",     "German", BIT(4)},
    {"it",     "Italian", BIT(7)},
    {"es",     "Spanish", BIT(6)},
    {"zh-CN",  "Chinese (Simplified)", BIT(14)},
    {"ko",     "Korean", BIT(12)},
    {"nl",     "Dutch", BIT(8)},
    {"pt",     "Portuguese", BIT(10)},
    {"ru",     "Russian", BIT(11)},
    {"zh-TW",  "Chinese (Traditional)", BIT(13)},
    {"en-GB",  "British English", BIT(1)},
    {"fr-CA",  "Canadian French", BIT(9)},
    {"es-419", "Latin American Spanish", BIT(5)},
    {"zh-Hans", "Simplified Chinese", BIT(14)},
    {"zh-Hant", "Traditional Chinese", BIT(13)},
    {"pt-BR",  "Portuguese (Brazil)", BIT(15)},
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

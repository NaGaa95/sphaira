#pragma once

#include <string>
#include <string_view>

namespace sphaira::i18n {

enum class WordOrder {
    PhraseName,  // default: SVO (English, French, German, etc.)
    NamePhrase   // SOV (Japanese, Korean, Turkish)
};

std::string init(long index);
void exit();

std::string get(std::string_view str);
std::string get(std::string_view str, std::string_view fallback);

// Returns the language file that is currently loaded (for example "en" or
// "zh-CN").  Unlike the configured language index, this also resolves the
// console language when the user selected Automatic.
std::string GetLanguageCode();

WordOrder GetWordOrder();
bool WordOrderLocale();

std::string Reorder(std::string_view phrase, std::string_view name);

} // namespace sphaira::i18n

inline namespace literals {

std::string operator""_i18n(const char* str, size_t len);

} // namespace literals

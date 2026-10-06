#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sphaira::qr {

// only what the menus need: byte mode, ecc level M, versions 1-10.
// that is plenty for a url, whilst keeping the tables small.
constexpr int MAX_VERSION = 10;
constexpr int MAX_MODULES = 17 + 4 * MAX_VERSION;
constexpr int MAX_PAYLOAD = 213; // byte mode capacity at v10 / ecc M.

struct Matrix {
    // encodes data, returns false (and empties the matrix) if it doesn't fit.
    auto Encode(std::string_view data) -> bool;

    auto IsEmpty() const -> bool {
        return !m_size;
    }

    // width / height of the code, in modules, excluding the quiet zone.
    auto GetSize() const -> int {
        return m_size;
    }

    auto IsDark(int x, int y) const -> bool {
        return m_modules[y * MAX_MODULES + x];
    }

private:
    int m_size{};
    std::array<std::uint8_t, MAX_MODULES * MAX_MODULES> m_modules{};
};

} // namespace sphaira::qr

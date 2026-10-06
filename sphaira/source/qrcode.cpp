#include "qrcode.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// QR code generation, as per ISO/IEC 18004.
//
// scope is deliberately narrow: byte mode, ecc level M, versions 1-10, which
// covers every address the network menus need to show. everything is sized off
// MAX_VERSION so nothing here allocates.
namespace sphaira::qr {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// ecc level M, indexed by version - 1.
struct VersionInfo {
    u16 data_codewords; // total data codewords across every block.
    u8 ec_codewords;    // ec codewords per block.
    u8 blocks;
};

constexpr VersionInfo VERSIONS[MAX_VERSION] = {
    {  16, 10, 1 },
    {  28, 16, 1 },
    {  44, 26, 1 },
    {  64, 18, 2 },
    {  86, 24, 2 },
    { 108, 16, 4 },
    { 124, 18, 4 },
    { 154, 22, 4 },
    { 182, 22, 5 },
    { 216, 26, 5 },
};

// centre coordinates of the alignment patterns, indexed by version - 1.
constexpr u8 ALIGNMENT[MAX_VERSION][3] = {
    { 0,  0,  0 },
    { 6, 18,  0 },
    { 6, 22,  0 },
    { 6, 26,  0 },
    { 6, 30,  0 },
    { 6, 34,  0 },
    { 6, 22, 38 },
    { 6, 24, 42 },
    { 6, 26, 46 },
    { 6, 28, 50 },
};

// bits left over after the last codeword, indexed by version - 1.
constexpr u8 REMAINDER_BITS[MAX_VERSION] = {
    0, 7, 7, 7, 7, 7, 0, 0, 0, 0,
};

constexpr int MAX_CODEWORDS = 346; // total codewords at v10.
constexpr int MAX_EC_CODEWORDS = 26;

constexpr auto GetVersion(int version) -> const VersionInfo& {
    return VERSIONS[version - 1];
}

constexpr auto GetModuleCount(int version) -> int {
    return 17 + 4 * version;
}

// byte mode capacity, in bytes.
constexpr auto GetCapacity(int version) -> int {
    const auto count_bits = version < 10 ? 8 : 16;
    return (GetVersion(version).data_codewords * 8 - (4 + count_bits)) / 8;
}

// GF(256) arithmetic, as used by Reed-Solomon.
struct GaloisField {
    std::array<u8, 256> exp{};
    std::array<u8, 256> log{};
};

consteval auto MakeGaloisField() -> GaloisField {
    GaloisField gf{};

    u32 x = 1;
    for (int i = 0; i < 255; i++) {
        gf.exp[i] = x;
        gf.log[x] = i;

        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11D; // primitive polynomial.
        }
    }

    // wraps around, saves a modulo in the multiply.
    gf.exp[255] = gf.exp[0];
    return gf;
}

constexpr auto GF = MakeGaloisField();

constexpr auto GfMul(u8 a, u8 b) -> u8 {
    if (!a || !b) {
        return 0;
    }
    return GF.exp[(GF.log[a] + GF.log[b]) % 255];
}

// builds the generator polynomial of the given degree.
void MakeGenerator(int degree, u8* out) {
    std::memset(out, 0, degree);
    out[degree - 1] = 1;

    u8 root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = GfMul(out[j], root);
            if (j + 1 < degree) {
                out[j] ^= out[j + 1];
            }
        }
        root = GfMul(root, 0x02);
    }
}

// remainder of data divided by the generator, ie. the ec codewords.
void MakeEcCodewords(const u8* data, int len, const u8* gen, int degree, u8* out) {
    std::memset(out, 0, degree);

    for (int i = 0; i < len; i++) {
        const u8 factor = data[i] ^ out[0];
        std::memmove(out, out + 1, degree - 1);
        out[degree - 1] = 0;

        for (int j = 0; j < degree; j++) {
            out[j] ^= GfMul(gen[j], factor);
        }
    }
}

// bit stream.
struct BitBuffer {
    void Append(u32 value, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            const auto bit = (value >> i) & 1;
            if (bit) {
                data[len / 8] |= 0x80 >> (len % 8);
            }
            len++;
        }
    }

    std::array<u8, MAX_CODEWORDS> data{};
    int len{};
};

// matrix construction.
struct Builder {
    explicit Builder(int version) : m_version{version}, m_size{GetModuleCount(version)} {}

    void DrawFunctionPatterns();
    void DrawCodewords(const u8* data, int total_bits);
    void ApplyBestMask();
    void CopyTo(std::uint8_t* out) const;

    auto GetSize() const -> int {
        return m_size;
    }

private:
    auto Get(int x, int y) const -> bool {
        return m_modules[y * MAX_MODULES + x];
    }

    void Set(int x, int y, bool dark) {
        m_modules[y * MAX_MODULES + x] = dark;
    }

    void SetFunction(int x, int y, bool dark) {
        Set(x, y, dark);
        m_function[y * MAX_MODULES + x] = true;
    }

    auto IsFunction(int x, int y) const -> bool {
        return m_function[y * MAX_MODULES + x];
    }

    void DrawFinder(int x, int y);
    void DrawAlignment(int x, int y);
    void DrawFormatBits(int mask);
    void DrawVersionBits();
    void ApplyMask(int mask);
    auto GetPenalty() const -> u32;

    const int m_version;
    const int m_size;
    std::array<u8, MAX_MODULES * MAX_MODULES> m_modules{};
    std::array<u8, MAX_MODULES * MAX_MODULES> m_function{};
};

void Builder::DrawFinder(int x, int y) {
    // the 7x7 pattern plus the separator ring around it.
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            const auto xx = x + dx;
            const auto yy = y + dy;
            if (xx < 0 || xx >= m_size || yy < 0 || yy >= m_size) {
                continue;
            }

            const auto dist = std::max(std::abs(dx), std::abs(dy));
            SetFunction(xx, yy, dist != 2 && dist != 4);
        }
    }
}

void Builder::DrawAlignment(int x, int y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            const auto dist = std::max(std::abs(dx), std::abs(dy));
            SetFunction(x + dx, y + dy, dist != 1);
        }
    }
}

void Builder::DrawFunctionPatterns() {
    // timing patterns, drawn first, the finders overwrite their ends.
    for (int i = 0; i < m_size; i++) {
        SetFunction(6, i, !(i % 2));
        SetFunction(i, 6, !(i % 2));
    }

    DrawFinder(3, 3);
    DrawFinder(m_size - 4, 3);
    DrawFinder(3, m_size - 4);

    // alignment patterns, skipping the three that clash with the finders.
    const auto& align = ALIGNMENT[m_version - 1];
    int count = 0;
    while (count < 3 && align[count]) {
        count++;
    }

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < count; j++) {
            const auto skip =
                (!i && !j) ||
                (!i && j == count - 1) ||
                (i == count - 1 && !j);

            if (!skip) {
                DrawAlignment(align[j], align[i]);
            }
        }
    }

    // reserve the format / version areas, the values are filled in later.
    DrawFormatBits(0);
    DrawVersionBits();
}

void Builder::DrawFormatBits(int mask) {
    // 5 data bits (ecc level M is 0b00) with a 10 bit bch code, then masked.
    const u32 data = 0b00 << 3 | mask;
    u32 rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }

    const u32 bits = ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
    const auto bit = [bits](int i) -> bool {
        return (bits >> i) & 1;
    };

    // first copy, around the top left finder.
    for (int i = 0; i <= 5; i++) {
        SetFunction(8, i, bit(i));
    }
    SetFunction(8, 7, bit(6));
    SetFunction(8, 8, bit(7));
    SetFunction(7, 8, bit(8));
    for (int i = 9; i < 15; i++) {
        SetFunction(14 - i, 8, bit(i));
    }

    // second copy, split between the other two finders.
    for (int i = 0; i < 8; i++) {
        SetFunction(m_size - 1 - i, 8, bit(i));
    }
    for (int i = 8; i < 15; i++) {
        SetFunction(8, m_size - 15 + i, bit(i));
    }

    SetFunction(8, m_size - 8, true); // always dark.
}

void Builder::DrawVersionBits() {
    if (m_version < 7) {
        return;
    }

    // 6 data bits with an 18 bit bch code.
    u32 rem = m_version;
    for (int i = 0; i < 12; i++) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    }

    const u32 bits = ((u32)m_version << 12) | (rem & 0xFFF);

    for (int i = 0; i < 18; i++) {
        const auto bit = (bits >> i) & 1;
        const auto a = m_size - 11 + i % 3;
        const auto b = i / 3;
        SetFunction(a, b, bit);
        SetFunction(b, a, bit);
    }
}

void Builder::DrawCodewords(const u8* data, int total_bits) {
    int i = 0;

    // two module wide columns, right to left, zig-zagging up then down.
    for (int right = m_size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5; // the vertical timing pattern isn't part of a column.
        }

        for (int vert = 0; vert < m_size; vert++) {
            for (int j = 0; j < 2; j++) {
                const auto x = right - j;
                const auto upward = !((right + 1) & 2);
                const auto y = upward ? m_size - 1 - vert : vert;

                if (!IsFunction(x, y) && i < total_bits) {
                    Set(x, y, (data[i / 8] >> (7 - (i % 8))) & 1);
                    i++;
                }
            }
        }
    }
}

void Builder::ApplyMask(int mask) {
    for (int y = 0; y < m_size; y++) {
        for (int x = 0; x < m_size; x++) {
            if (IsFunction(x, y)) {
                continue;
            }

            bool invert{};
            switch (mask) {
                case 0: invert = !((x + y) % 2); break;
                case 1: invert = !(y % 2); break;
                case 2: invert = !(x % 3); break;
                case 3: invert = !((x + y) % 3); break;
                case 4: invert = !((x / 3 + y / 2) % 2); break;
                case 5: invert = !(x * y % 2 + x * y % 3); break;
                case 6: invert = !((x * y % 2 + x * y % 3) % 2); break;
                case 7: invert = !((x * y % 3 + (x + y) % 2) % 2); break;
            }

            if (invert) {
                Set(x, y, !Get(x, y));
            }
        }
    }
}

auto Builder::GetPenalty() const -> u32 {
    constexpr u32 N1 = 3, N2 = 3, N3 = 40, N4 = 10;
    // 1011101 followed (or preceded) by four light modules.
    constexpr bool FINDER[11] = { true, false, true, true, true, false, true, false, false, false, false };

    u32 score{};

    const auto run_and_finder = [&](auto get) {
        for (int i = 0; i < m_size; i++) {
            int run = 1;
            for (int j = 1; j < m_size; j++) {
                if (get(i, j) == get(i, j - 1)) {
                    run++;
                    if (run == 5) {
                        score += N1;
                    } else if (run > 5) {
                        score += 1;
                    }
                } else {
                    run = 1;
                }
            }

            for (int j = 0; j + 11 <= m_size; j++) {
                bool forward = true, backward = true;
                for (int k = 0; k < 11; k++) {
                    forward = forward && get(i, j + k) == FINDER[k];
                    backward = backward && get(i, j + k) == FINDER[10 - k];
                }

                if (forward) {
                    score += N3;
                }
                if (backward) {
                    score += N3;
                }
            }
        }
    };

    run_and_finder([this](int y, int x) { return Get(x, y); });
    run_and_finder([this](int x, int y) { return Get(x, y); });

    // blocks of the same colour.
    for (int y = 0; y + 1 < m_size; y++) {
        for (int x = 0; x + 1 < m_size; x++) {
            const auto c = Get(x, y);
            if (c == Get(x + 1, y) && c == Get(x, y + 1) && c == Get(x + 1, y + 1)) {
                score += N2;
            }
        }
    }

    // proportion of dark modules, the further from 50% the worse.
    u32 dark{};
    for (int y = 0; y < m_size; y++) {
        for (int x = 0; x < m_size; x++) {
            dark += Get(x, y);
        }
    }

    const auto total = (u32)m_size * m_size;
    const auto percent = dark * 100 / total;
    const auto deviation = percent > 50 ? percent - 50 : 50 - percent;
    score += (deviation / 5) * N4;

    return score;
}

void Builder::ApplyBestMask() {
    int best_mask{};
    u32 best_penalty = ~0U;

    for (int mask = 0; mask < 8; mask++) {
        ApplyMask(mask);
        DrawFormatBits(mask);

        const auto penalty = GetPenalty();
        if (penalty < best_penalty) {
            best_penalty = penalty;
            best_mask = mask;
        }

        ApplyMask(mask); // xor is its own inverse.
    }

    ApplyMask(best_mask);
    DrawFormatBits(best_mask);
}

void Builder::CopyTo(std::uint8_t* out) const {
    std::memcpy(out, m_modules.data(), m_modules.size());
}

} // namespace

auto Matrix::Encode(std::string_view data) -> bool {
    m_size = 0;
    m_modules = {};

    if (data.size() > (std::size_t)MAX_PAYLOAD) {
        return false;
    }

    // smallest version the payload fits in.
    int version = 1;
    while (version <= MAX_VERSION && (int)data.size() > GetCapacity(version)) {
        version++;
    }

    if (version > MAX_VERSION) {
        return false;
    }

    const auto& info = GetVersion(version);

    // mode indicator, character count, then the payload itself.
    BitBuffer bb{};
    bb.Append(0b0100, 4);
    bb.Append((u32)data.size(), version < 10 ? 8 : 16);
    for (const auto c : data) {
        bb.Append((u8)c, 8);
    }

    // terminator, then pad out to a whole number of codewords.
    const auto capacity_bits = info.data_codewords * 8;
    bb.Append(0, std::min(4, capacity_bits - bb.len));
    bb.Append(0, (8 - bb.len % 8) % 8);

    for (bool second = false; bb.len < capacity_bits; second = !second) {
        bb.Append(second ? 0x11 : 0xEC, 8);
    }

    // split into blocks and compute the ec codewords for each.
    const auto short_blocks = info.blocks - info.data_codewords % info.blocks;
    const auto short_len = info.data_codewords / info.blocks;

    std::array<u8, MAX_EC_CODEWORDS> generator{};
    MakeGenerator(info.ec_codewords, generator.data());

    std::array<std::array<u8, MAX_EC_CODEWORDS>, 5> ec{};
    std::array<const u8*, 5> block{};
    std::array<int, 5> block_len{};

    int offset = 0;
    for (int i = 0; i < info.blocks; i++) {
        block_len[i] = short_len + (i < short_blocks ? 0 : 1);
        block[i] = bb.data.data() + offset;
        offset += block_len[i];

        MakeEcCodewords(block[i], block_len[i], generator.data(), info.ec_codewords, ec[i].data());
    }

    // interleave, data codewords first then the ec codewords.
    BitBuffer interleaved{};
    for (int i = 0; i <= short_len; i++) {
        for (int j = 0; j < info.blocks; j++) {
            if (i < block_len[j]) {
                interleaved.Append(block[j][i], 8);
            }
        }
    }

    for (int i = 0; i < info.ec_codewords; i++) {
        for (int j = 0; j < info.blocks; j++) {
            interleaved.Append(ec[j][i], 8);
        }
    }

    interleaved.Append(0, REMAINDER_BITS[version - 1]);

    Builder builder{version};
    builder.DrawFunctionPatterns();
    builder.DrawCodewords(interleaved.data.data(), interleaved.len);
    builder.ApplyBestMask();
    builder.CopyTo(m_modules.data());

    m_size = builder.GetSize();
    return true;
}

} // namespace sphaira::qr

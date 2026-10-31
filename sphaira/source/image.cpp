#include "image.hpp"

// disable warnings for stb
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Warray-bounds="
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop

#include "app.hpp"
#include "log.hpp"
#ifdef USE_NVJPG
#include <nvjpg.hpp>
#endif
#include <cstring>
#include <limits>

namespace sphaira {
namespace {

constexpr int BPP = 4;
constexpr int ICON_SIZE = 256;
constexpr int ICON_MAX_DIMENSION = 1024;
constexpr size_t ICON_MAX_PIXELS = static_cast<size_t>(ICON_MAX_DIMENSION) * ICON_MAX_DIMENSION;

struct ImageLimits {
    int max_dimension{std::numeric_limits<int>::max()};
    size_t max_pixels{std::numeric_limits<size_t>::max()};
};

auto GetImageSize(int x, int y, size_t bpp, size_t& size) -> bool {
    if (x <= 0 || y <= 0 || !bpp) {
        return false;
    }

    const auto width = static_cast<size_t>(x);
    const auto height = static_cast<size_t>(y);
    if (width > std::numeric_limits<size_t>::max() / height) {
        return false;
    }

    const auto pixels = width * height;
    if (pixels > std::numeric_limits<size_t>::max() / bpp) {
        return false;
    }

    size = pixels * bpp;
    return true;
}

auto ImageDimensionsAllowed(int x, int y, const ImageLimits& limits, size_t& size) -> bool {
    if (!GetImageSize(x, y, BPP, size)) {
        return false;
    }

    return x <= limits.max_dimension && y <= limits.max_dimension && size / BPP <= limits.max_pixels;
}

auto ImageLoadInternal(stbi_uc* image_data, int x, int y, const ImageLimits& limits) -> ImageResult {
    size_t size{};
    if (!image_data || !ImageDimensionsAllowed(x, y, limits, size)) {
        stbi_image_free(image_data);
        log_write("failed image load\n");
        return {};
    }

    ImageResult result{};
    result.data.resize(size);
    result.w = x;
    result.h = y;
    std::memcpy(result.data.data(), image_data, result.data.size());
    stbi_image_free(image_data);
    return result;
}

#ifdef USE_NVJPG
auto ImageLoadInternal(nj::Image&& image, const ImageLimits& limits) -> ImageResult {
    if (!image.is_valid() || image.parse()) {
        log_write("[NVJPG] failed to parse image\n");
        return {};
    }

    if (image.width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<size_t>(std::numeric_limits<int>::max())) {
        log_write("[NVJPG] invalid image dimensions\n");
        return {};
    }

    size_t decoded_size{};
    if (!ImageDimensionsAllowed(static_cast<int>(image.width), static_cast<int>(image.height), limits, decoded_size)) {
        log_write("[NVJPG] image dimensions exceed limit\n");
        return {};
    }

    nj::Surface surf{image.width, image.height};
    if (surf.allocate()) {
        log_write("[NVJPG] failed to allocate surf\n");
        return {};
    }

    if (R_FAILED(App::GetApp()->m_decoder.render(image, surf, 255))) {
        log_write("[NVJPG] failed to render\n");
        return {};
    }

    if (R_FAILED(App::GetApp()->m_decoder.wait(surf))) {
        log_write("[NVJPG] failed to wait\n");
        return {};
    }

    ImageResult result{};
    result.w = static_cast<int>(surf.width);
    result.h = static_cast<int>(surf.height);
    size_t result_size{};
    if (!GetImageSize(result.w, result.h, surf.get_bpp(), result_size)) {
        log_write("[NVJPG] invalid surface dimensions\n");
        return {};
    }
    result.data.resize(result_size);
    // std::printf("[NVJPG] w: %zu h: %zu bpp: %u pitch: %zu size: %zu size2: %u\n", surf.width, surf.height, surf.get_bpp(), surf.pitch, surf.size(), 256*256*4);

    if (surf.width * surf.get_bpp() == surf.pitch) [[likely]] {
        std::memcpy(result.data.data(), surf.data(), result.data.size());
    } else {
        for (size_t i = 0; i < surf.height; i++) {
            const auto src_pitch = surf.pitch;
            const auto dst_pitch = surf.width * surf.get_bpp();
            std::memcpy(result.data.data() + i * dst_pitch, surf.data() + i * src_pitch, dst_pitch);
        }
    }

    return result;
}
#endif

auto ImageLoadFromMemoryInternal(std::span<const u8> data, u32 flags, const ImageLimits& limits) -> ImageResult {
    if (data.empty() || data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        log_write("failed image load\n");
        return {};
    }

    const auto data_size = static_cast<int>(data.size());
    int x{}, y{}, channels{};
    if (!stbi_info_from_memory(data.data(), data_size, &x, &y, &channels)) {
        log_write("failed image info\n");
        return {};
    }

    size_t decoded_size{};
    if (!ImageDimensionsAllowed(x, y, limits, decoded_size)) {
        log_write("image dimensions exceed limit: %dx%d\n", x, y);
        return {};
    }

#ifdef USE_NVJPG
    if (flags & ImageFlag_JPEG) {
        auto shared_vec = std::make_shared<std::vector<u8>>(data.size());
        std::memcpy(shared_vec->data(), data.data(), shared_vec->size());
        auto result = ImageLoadInternal(nj::Image{shared_vec}, limits);
        if (!result.data.empty()) {
            return result;
        }
    }
#else
    (void)flags;
#endif

    x = y = channels = 0;
    auto image_data = stbi_load_from_memory(data.data(), data_size, &x, &y, &channels, BPP);
    return ImageLoadInternal(image_data, x, y, limits);
}

// 5x7 bitmap glyphs, MSB is the leftmost pixel. Only the characters used
// by the icon labels (eg. "32-BIT NA", "39-BIT") are defined.
struct LabelGlyph {
    char ch;
    u8 rows[7];
};

constexpr LabelGlyph LABEL_GLYPHS[] = {
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'-', {0, 0, 0, 0b01110, 0, 0, 0}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

auto FindLabelGlyph(char ch) -> const LabelGlyph* {
    for (const auto& glyph : LABEL_GLYPHS) {
        if (glyph.ch == ch) {
            return &glyph;
        }
    }
    return nullptr;
}

} // namespace

auto ImageLoadFromMemory(std::span<const u8> data, u32 flags) -> ImageResult {
    return ImageLoadFromMemoryInternal(data, flags, {});
}

auto ImageLoadFromFile(const fs::FsPath& file, u32 flags) -> ImageResult {
#ifdef USE_NVJPG
    if (flags & ImageFlag_JPEG) {
        // don't make const as it prevents RTO.
        auto result = ImageLoadInternal(nj::Image{file}, {});
        // if it failed, try again but without using oss-jpg.
        return result.data.empty() ? ImageLoadFromFile(file, 0) : result;
    }
    else
#endif
    {
        int x{}, y{}, channels{};
        if (!stbi_info(file, &x, &y, &channels)) {
            log_write("failed image info\n");
            return {};
        }

        size_t decoded_size{};
        if (!ImageDimensionsAllowed(x, y, {}, decoded_size)) {
            log_write("invalid image dimensions: %dx%d\n", x, y);
            return {};
        }

        x = y = channels = 0;
        auto image_data = stbi_load(file, &x, &y, &channels, BPP);
        return ImageLoadInternal(image_data, x, y, {});
    }
}

auto ImageResize(std::span<const u8> data, int inx, int iny, int outx, int outy) -> ImageResult {
    size_t input_size{}, output_size{};
    if (!GetImageSize(inx, iny, BPP, input_size) || !GetImageSize(outx, outy, BPP, output_size) ||
        data.size() < input_size || inx > std::numeric_limits<int>::max() / BPP ||
        outx > std::numeric_limits<int>::max() / BPP) {
        log_write("invalid resize dimensions\n");
        return {};
    }

    log_write("doing resize inx: %d iny: %d outx: %d outy: %d\n", inx, iny, outx, outy);
    std::vector<u8> resized_data(output_size);

    // if (stbir_resize_uint8(data.data(), inx, iny, inx * BPP, resized_data.data(), outx, outy, outx*BPP, BPP)) {
    if (stbir_resize_uint8_linear(data.data(), inx, iny, inx * BPP, resized_data.data(), outx, outy, outx*BPP, (stbir_pixel_layout)BPP)) {
        log_write("did resize\n");
        return { resized_data, outx, outy };
    }
    log_write("failed resize\n");
    return {};
}

auto ImageConvertToJpg(std::span<const u8> data, int x, int y) -> ImageResult {
    size_t input_size{};
    if (!GetImageSize(x, y, BPP, input_size) || data.size() < input_size) {
        log_write("invalid jpg dimensions\n");
        return {};
    }

    std::vector<u8> out;
    out.reserve(input_size);
    log_write("doing jpeg convert\n");

    const auto cb = [](void *context, void *data, int size) -> void {
        auto buf = static_cast<std::vector<u8>*>(context);
        const auto offset = buf->size();
        buf->resize(offset + size);
        std::memcpy(buf->data() + offset, data, size);
    };

    if (stbi_write_jpg_to_func(cb, &out, x, y, 4, data.data(), 93)) {
        // out.shrink_to_fit();
        log_write("did jpg convert\n");
        return { out, x, y };
    }

    log_write("failed jpg convert\n");
    return {};
}

auto ImageLoadIcon(std::span<const u8> data) -> ImageResult {
    const auto flags = data.size() >= 2 && data[0] == 0xFF && data[1] == 0xD8
        ? ImageFlag_JPEG : ImageFlag_None;
    auto image = ImageLoadFromMemoryInternal(data, flags, {ICON_MAX_DIMENSION, ICON_MAX_PIXELS});
    if (image.data.empty()) {
        return {};
    }

    if (image.w != ICON_SIZE || image.h != ICON_SIZE) {
        image = ImageResize(image.data, image.w, image.h, ICON_SIZE, ICON_SIZE);
    }
    return image;
}

auto ImageNormalizeIcon(std::span<const u8> data) -> std::vector<u8> {
    auto image = ImageLoadIcon(data);
    if (image.data.empty()) {
        return {};
    }

    return ImageConvertToJpg(image.data, image.w, image.h).data;
}

auto ImageIconWithLabel(std::span<const u8> data, std::string_view label) -> std::vector<u8> {
    if (label.empty()) {
        return {};
    }
    for (const auto ch : label) {
        if (!FindLabelGlyph(ch)) {
            log_write("unsupported icon label character: %c\n", ch);
            return {};
        }
    }

    // Decode like ImageLoadIcon, but never through NVJPG: this runs on the
    // install worker thread, away from the GPU decoder.
    auto image = ImageLoadFromMemoryInternal(data, ImageFlag_None, {ICON_MAX_DIMENSION, ICON_MAX_PIXELS});
    if (image.data.empty()) {
        return {};
    }
    if (image.w != ICON_SIZE || image.h != ICON_SIZE) {
        image = ImageResize(image.data, image.w, image.h, ICON_SIZE, ICON_SIZE);
        if (image.data.empty()) {
            return {};
        }
    }

    constexpr int GLYPH_W = 5;
    constexpr int GLYPH_H = 7;
    constexpr int ADVANCE = 6; // glyph width + one spacing column, pre-scale.

    int scale = 4;
    const auto text_width = [&label](int s) {
        return static_cast<int>(label.size()) * ADVANCE * s - s;
    };
    while (scale > 1 && text_width(scale) > ICON_SIZE - 16) {
        scale--;
    }

    const int text_w = text_width(scale);
    const int text_h = GLYPH_H * scale;
    const int band_h = text_h + 16;
    const int band_y = ICON_SIZE - band_h;

    // Darken the band so the label stays readable over any artwork.
    for (int y = band_y; y < ICON_SIZE; y++) {
        for (int x = 0; x < ICON_SIZE; x++) {
            auto* px = &image.data[(y * ICON_SIZE + x) * BPP];
            px[0] = static_cast<u8>(px[0] * 70 / 255);
            px[1] = static_cast<u8>(px[1] * 70 / 255);
            px[2] = static_cast<u8>(px[2] * 70 / 255);
            px[3] = 255;
        }
    }

    int cx = (ICON_SIZE - text_w) / 2;
    const int cy = band_y + (band_h - text_h) / 2;
    for (const auto ch : label) {
        const auto* glyph = FindLabelGlyph(ch);
        for (int row = 0; row < GLYPH_H; row++) {
            for (int col = 0; col < GLYPH_W; col++) {
                if (!(glyph->rows[row] & (1 << (GLYPH_W - 1 - col)))) {
                    continue;
                }
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        auto* px = &image.data[((cy + row * scale + dy) * ICON_SIZE + cx + col * scale + dx) * BPP];
                        px[0] = px[1] = px[2] = px[3] = 255;
                    }
                }
            }
        }
        cx += ADVANCE * scale;
    }

    return ImageConvertToJpg(image.data, image.w, image.h).data;
}

} // namespace sphaira

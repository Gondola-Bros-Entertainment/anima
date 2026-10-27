// Vector atlases: Linea drawings rendered at a scale and packed into one sprite sheet, each frame inside a border that
// its spread color fills.
#include "../detail/dilate.hpp"
#include "../detail/memory_image.hpp"
#include <algorithm>
#include <anima/vector.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anima {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
bool positive(double value) { return std::isfinite(value) && value > 0; }
static_assert(max_image_edge == 8192 && VectorAtlas::max_sheet_edge == 8192, "The messages state these limits");
constexpr char size_message[] = "Vector art must render at most 8192 pixels along each axis";
constexpr char fit_message[] = "Vector art must fit one sheet of 8192 by 8192 texels";

void validate_art(const VectorArt &art) {
    require(positive(art.size.x) && positive(art.size.y), "Vector art size must be finite and positive");
    require(positive(art.drawing.width) && positive(art.drawing.height),
            "Vector art canvas must be finite and positive");
}
void validate_scale(float scale) { require(positive(scale), "Vector art scale must be finite and positive"); }
// The pixels that @p units logical units cover at @p scale: the nearest whole number, halves away from zero, at least
// 1, or 0 when that exceeds max_image_edge. The product of two floats is exact in double.
std::uint32_t pixels(float units, float scale) {
    const auto rounded = std::max(1., std::round(double(units) * double(scale)));
    return rounded <= double(max_image_edge) ? std::uint32_t(rounded) : 0;
}

// A frame and the border around it, and where packing puts its top left corner.
struct Cell {
    std::uint32_t width{}, height{};
    std::uint64_t x{}, y{};
};
// Packs @p cells into shelves at most @p shelf_width wide, from the top: each shelf takes the cells that fit from the
// left, tallest first, then widest, then in art order, and is as tall as its first. Returns the width and height that
// the cells span.
std::pair<std::uint64_t, std::uint64_t> pack(std::vector<Cell> &cells, std::uint64_t shelf_width) {
    std::vector<std::size_t> order(cells.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, std::ranges::greater{},
                             [&](std::size_t i) { return std::pair(cells[i].height, cells[i].width); });
    std::uint64_t x = 0, y = 0, shelf = 0, width = 0;
    for (const auto i : order) {
        auto &cell = cells[i];
        if (x + cell.width > shelf_width) {
            y += shelf;
            x = shelf = 0;
        }
        cell.x = x;
        cell.y = y;
        x += cell.width;
        shelf = std::max<std::uint64_t>(shelf, cell.height);
        width = std::max(width, x);
    }
    return {width, y + shelf};
}

// The cells of @p art at @p scale, each a frame and its border, packed, and the width and height they span; or, when
// they do not fit one sheet, why not.
struct Packing {
    std::vector<Cell> cells;
    std::pair<std::uint64_t, std::uint64_t> extent{};
    const char *misfit = nullptr;
};
Packing pack_sheet(std::span<const VectorArt> art, float scale) {
    constexpr auto border = VectorAtlas::padding;
    Packing packing;
    packing.cells.reserve(art.size());
    std::uint64_t area = 0, widest = 0;
    for (const auto &drawing : art) {
        const auto width = pixels(drawing.size.x, scale), height = pixels(drawing.size.y, scale);
        if (!width || !height) {
            packing.misfit = size_message;
            return packing;
        }
        const Cell cell{width + 2 * border, height + 2 * border, 0, 0};
        packing.cells.push_back(cell);
        area += std::uint64_t{cell.width} * cell.height;
        widest = std::max<std::uint64_t>(widest, cell.width);
    }
    if (widest > VectorAtlas::max_sheet_edge) {
        packing.misfit = fit_message;
        return packing;
    }
    // Shelves about as wide as a square of the cells' area, or as wide as a sheet may be when that leaves it too tall.
    // The square root is correctly rounded, so the width is the same on every platform.
    const auto square = std::uint64_t(std::ceil(std::sqrt(double(area))));
    packing.extent = pack(packing.cells, std::clamp<std::uint64_t>(square, widest, VectorAtlas::max_sheet_edge));
    if (packing.extent.second > VectorAtlas::max_sheet_edge)
        packing.extent = pack(packing.cells, VectorAtlas::max_sheet_edge);
    if (packing.extent.second > VectorAtlas::max_sheet_edge)
        packing.misfit = fit_message;
    return packing;
}

// The sheet of @p art at @p scale; see VectorAtlas.
std::shared_ptr<const SpriteSheet> render_sheet(const std::vector<VectorArt> &art, float scale) {
    validate_scale(scale);
    constexpr auto border = VectorAtlas::padding;
    const Packing packing = pack_sheet(art, scale);
    if (packing.misfit)
        throw std::invalid_argument(packing.misfit);
    const auto &cells = packing.cells;
    const auto extent = packing.extent;

    auto image = std::make_shared<Image>();
    image->width = std::uint32_t(extent.first);
    image->height = std::uint32_t(extent.second);
    image->rgba.assign(std::size_t{image->width} * image->height * 4, 0);
    const auto texel = [&](std::uint64_t x, std::uint64_t y) { return std::size_t(y * image->width + x) * 4; };
    std::vector<SpriteFrame> frames;
    frames.reserve(art.size());
    std::vector<std::uint8_t> covered;
    for (std::size_t i = 0; i < art.size(); ++i) {
        const auto &cell = cells[i];
        const SpriteFrame frame{std::uint32_t(cell.x) + border, std::uint32_t(cell.y) + border, cell.width - 2 * border,
                                cell.height - 2 * border};
        frames.push_back(frame);
        const auto rendered = render_vector_art(art[i], scale);
        const std::size_t row_bytes = std::size_t{frame.width} * 4;
        for (std::uint32_t row = 0; row < frame.height; ++row)
            std::copy_n(rendered.rgba.begin() + std::ptrdiff_t(row * row_bytes), row_bytes,
                        image->rgba.begin() + std::ptrdiff_t(texel(frame.x, frame.y + row)));
        // The cell's texels row by row, of which those that are not transparent spread their color over the rest.
        const auto at = [&](std::uint32_t index) {
            return texel(cell.x + index % cell.width, cell.y + index / cell.width);
        };
        covered.assign(std::size_t{cell.width} * cell.height, 0);
        for (std::uint32_t index = 0; index < covered.size(); ++index)
            covered[index] = image->rgba[at(index) + 3] != 0;
        detail::dilate(cell.width, cell.height, covered, [&](std::uint32_t from, std::uint32_t to) {
            std::copy_n(image->rgba.begin() + std::ptrdiff_t(at(from)), 3,
                        image->rgba.begin() + std::ptrdiff_t(at(to)));
        });
    }
    Texture texture{std::move(image), {}, TextureEncoding::srgb};
    texture.sampler = {Filter::linear, Filter::linear, Filter::linear, Wrap::clamp, Wrap::clamp, false};
    return std::make_shared<const SpriteSheet>(SpriteSheetDefinition{std::move(texture), std::move(frames)});
}
// The name of drawing @p i of @p art, for sorting and searching indices by name.
auto name_of(const std::vector<VectorArt> &art) {
    return [&art](std::size_t i) { return std::string_view(art[i].name); };
}
} // namespace

Image render_vector_art(const VectorArt &art, float scale) {
    validate_art(art);
    validate_scale(scale);
    const auto width = pixels(art.size.x, scale), height = pixels(art.size.y, scale);
    require(width && height, size_message);
    auto rendered = linea::render(art.drawing, int(width), int(height));
    Image result;
    result.width = width;
    result.height = height;
    result.rgba = std::move(rendered.rgba);
    return result;
}

VectorAtlas::VectorAtlas(std::vector<VectorArt> art, float scale) : art_(std::move(art)) {
    require(!art_.empty(), "Vector atlas needs at least one drawing");
    for (const auto &drawing : art_) {
        if (!detail::memory_image_name(drawing.name))
            throw std::invalid_argument("Invalid vector art name: " + drawing.name);
        validate_art(drawing);
    }
    by_name_.resize(art_.size());
    std::iota(by_name_.begin(), by_name_.end(), std::size_t{0});
    std::ranges::sort(by_name_, {}, name_of(art_));
    if (const auto duplicate = std::ranges::adjacent_find(by_name_, {}, name_of(art_)); duplicate != by_name_.end())
        throw std::invalid_argument("Duplicate vector art name: " + art_[*duplicate].name);
    sheet_ = render_sheet(art_, scale);
    scale_ = scale;
}

bool VectorAtlas::fits(std::span<const VectorArt> art, float scale) {
    for (const auto &drawing : art)
        validate_art(drawing);
    validate_scale(scale);
    return !pack_sheet(art, scale).misfit;
}

void VectorAtlas::set_scale(float scale) {
    if (scale == scale_)
        return;
    sheet_ = render_sheet(art_, scale);
    scale_ = scale;
}

std::uint32_t VectorAtlas::frame(std::string_view name) const {
    const auto found = std::ranges::lower_bound(by_name_, name, {}, name_of(art_));
    if (found == by_name_.end() || art_[*found].name != name)
        throw std::out_of_range("Missing vector art: " + std::string(name));
    // The sheet holds a frame for each drawing within 8192 by 8192 texels, so the index fits.
    return std::uint32_t(*found);
}
} // namespace anima

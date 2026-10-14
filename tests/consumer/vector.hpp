#pragma once
// An application's art described as Linea drawings through the public anima::vector target alone: an atlas renders it
// into one sheet at a display scale and again at another, finds each frame by name at both, and spreads each frame's
// color into the border around it.
#include <anima/vector.hpp>
#include <cstdint>
#include <linea/Shapes.hpp>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vector_consumer {
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline linea::Layer filled(linea::Path path, linea::Color color) {
    linea::Layer layer;
    layer.path = std::move(path);
    layer.fill = color;
    return layer;
}
// A red coin and a blue gem of one size, each one color wherever it covers, and a flag of two colors under a
// translucent stripe. Sizes are multiples of 4 logical units, whole pixels at every display scale in quarters.
inline std::vector<anima::VectorArt> art() {
    std::vector<anima::VectorArt> result;
    result.push_back(
        {"art/coin", linea::Drawing{12, 12, {filled(linea::circle({6, 6}, 5.5), {255, 0, 0, 255})}}, {12, 12}});
    result.push_back(
        {"art/gem",
         linea::Drawing{12, 12, {filled(linea::star({6, 6.5}, 5.5, 2.5, 5, -std::numbers::pi / 2), {0, 0, 255, 255})}},
         {12, 12}});
    result.push_back({"art/flag",
                      linea::Drawing{16,
                                     8,
                                     {filled(linea::rectangle({1, 1}, {8, 7}), {255, 200, 0, 255}),
                                      filled(linea::rectangle({8, 1}, {15, 7}), {0, 160, 80, 255}),
                                      filled(linea::rectangle({1, 3}, {15, 5}), {255, 255, 255, 128})}},
                      {16, 8}});
    return result;
}
inline void run() {
    anima::VectorAtlas atlas(art(), 1.5F);
    const auto first = atlas.sheet();
    require(atlas.frame("art/coin") == 0 && atlas.frame("art/gem") == 1 && atlas.frame("art/flag") == 2,
            "Vector art frames are not in art order");
    const auto &frames = first->definition().frames;
    require(frames.size() == 3 && frames[0].width == 18 && frames[2].width == 24 && frames[2].height == 12,
            "Vector art did not render at its size times the scale");
    // The coin's border holds its red, with alpha 0.
    const auto &image = *first->definition().texture.image;
    const auto border = (std::size_t{frames[0].y} * image.width + frames[0].x - anima::VectorAtlas::padding) * 4;
    require(image.rgba[border] == 255 && image.rgba[border + 1] == 0 && image.rgba[border + 2] == 0 &&
                image.rgba[border + 3] == 0,
            "A frame's color did not spread into its border");
    atlas.set_scale(3);
    require(atlas.sheet() != first && atlas.sheet()->definition().frames[2].width == 48 && atlas.frame("art/flag") == 2,
            "A new scale did not render a new sheet with the same frames");
    const auto icon = anima::render_vector_art(atlas.art()[0], 2);
    require(icon.width == 24 && icon.height == 24 && icon.rgba.size() == std::size_t{24} * 24 * 4,
            "Vector art did not render alone at its size times the scale");
}
} // namespace vector_consumer

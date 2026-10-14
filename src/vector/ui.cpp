// Vector art as UI images: each drawing of an atlas rendered at the UI's dp ratio and registered by name.
#include "../detail/memory_image.hpp"
#include <anima/vector_ui.hpp>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace anima {
void set_ui_images(UiContext &ui, const VectorAtlas &atlas) {
    const auto scale = ui.context().GetDensityIndependentPixelRatio();
    std::vector<UiImage> images;
    images.reserve(atlas.art().size());
    for (const auto &art : atlas.art()) {
        auto rendered = render_vector_art(art, scale);
        if (!detail::measured_dp(rendered.width, scale) || !detail::measured_dp(rendered.height, scale))
            throw std::invalid_argument("Invalid UI image scale");
        images.push_back({rendered.width, rendered.height, std::move(rendered.rgba), scale});
    }
    for (std::size_t i = 0; i < images.size(); ++i)
        ui.set_image(atlas.art()[i].name, std::move(images[i]));
}
} // namespace anima

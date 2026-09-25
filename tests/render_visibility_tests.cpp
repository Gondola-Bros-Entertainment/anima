#include <anima/assets/render_visibility.hpp>
#include <iostream>
#include <limits>
#include <random>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
anima::RenderBounds box(anima::Vec3 low, anima::Vec3 high) { return {low, high, true}; }
// Independent homogeneous-corner oracle: reject only if every corner lies
// beyond the same clip plane. No plane extraction or perspective divide.
bool reference(const anima::Mat4 &view, const anima::RenderBounds &bounds) {
    bool outside[6]{true, true, true, true, true, true};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const double point[]{corner & 1 ? bounds.maximum.x : bounds.minimum.x,
                             corner & 2 ? bounds.maximum.y : bounds.minimum.y,
                             corner & 4 ? bounds.maximum.z : bounds.minimum.z, 1};
        double clip[4]{};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column)
                clip[row] += view[column * 4 + row] * point[column];
        const double distances[]{clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1],
                                 clip[3] - clip[1], clip[2],           clip[3] - clip[2]};
        for (unsigned plane = 0; plane < 6; ++plane)
            outside[plane] &= distances[plane] < 0;
    }
    return std::none_of(std::begin(outside), std::end(outside), [](bool value) { return value; });
}
} // namespace
int main() {
    try {
        const anima::RenderFrustum unit(anima::identity());
        require(unit.intersects(box({-.2F, -.2F, .2F}, {.2F, .2F, .8F})), "Interior box rejected");
        for (const auto bounds : {box({-3, -.2F, .2F}, {-2, .2F, .8F}), box({2, -.2F, .2F}, {3, .2F, .8F}),
                                  box({-.2F, -3, .2F}, {.2F, -2, .8F}), box({-.2F, 2, .2F}, {.2F, 3, .8F}),
                                  box({-.2F, -.2F, -2}, {.2F, .2F, -1}), box({-.2F, -.2F, 2}, {.2F, .2F, 3})})
            require(!unit.intersects(bounds), "Box outside a clip plane retained");
        for (const auto bounds :
             {box({-2, -.2F, .2F}, {-1, .2F, .8F}), box({1, -.2F, .2F}, {2, .2F, .8F}),
              box({-.2F, -2, .2F}, {.2F, -1, .8F}), box({-.2F, 1, .2F}, {.2F, 2, .8F}),
              box({-.2F, -.2F, -1}, {.2F, .2F, 0}), box({-.2F, -.2F, 1}, {.2F, .2F, 2}),
              box({-100, -100, -100}, {100, 100, 100}), box({-2, -.1F, .5F}, {2, .1F, .5F}), box({0, 0, 0}, {0, 0, 0})})
            require(unit.intersects(bounds), "Contact, spanning or enclosing box rejected");
        const auto projection = anima::perspective(1.5F, .5F, 20);
        const anima::RenderFrustum perspective(projection);
        require(perspective.intersects(box({-.1F, -.1F, -2}, {.1F, .1F, -1})), "Perspective interior rejected");
        require(!perspective.intersects(box({-.1F, -.1F, 1}, {.1F, .1F, 2})), "Box behind eye retained");
        require(perspective.intersects(box({-1, -1, -1}, {1, 1, 1})), "Eye/near-plane crossing rejected");
        require(!perspective.intersects(box({-.1F, -.1F, -22}, {.1F, .1F, -21})), "Beyond far plane retained");
        auto infinite = projection;
        infinite[10] = -1;
        infinite[14] = -.5F;
        require(anima::RenderFrustum(infinite).intersects(box({-1, -1, -1e8F}, {1, 1, -1e7F})),
                "Infinite far projection clipped distance");
        auto reverse = projection;
        for (unsigned column = 0; column < 4; ++column)
            reverse[column * 4 + 2] = projection[column * 4 + 3] - projection[column * 4 + 2];
        require(anima::RenderFrustum(reverse).intersects(box({-.1F, -.1F, -2}, {.1F, .1F, -1})),
                "Reverse depth interior rejected");
        require(!anima::RenderFrustum(reverse).intersects(box({-.1F, -.1F, -22}, {.1F, .1F, -21})),
                "Reverse depth far plane missed");
        require(unit.intersects({}) && anima::RenderFrustum().intersects(box({1e10F, 0, 0}, {2e10F, 1, 1})),
                "Unknown view/bounds did not fail open");
        auto invalid = box({0, 0, 0}, {1, 1, 1});
        invalid.minimum.x = std::numeric_limits<float>::quiet_NaN();
        require(unit.intersects(invalid), "Nonfinite bounds did not fail open");
        require(unit.intersects(box({2, 0, 0}, {1, 1, 1})), "Unordered bounds did not fail open");
        auto invalid_view = anima::identity();
        invalid_view[0] = std::numeric_limits<float>::infinity();
        bool rejected = false;
        try {
            (void)anima::RenderFrustum(invalid_view);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "Nonfinite view accepted");

        // Separate float clip coordinates can touch even when their combined
        // plane is slightly negative in double precision. Its nearly cancelled
        // coefficients must not erase the original arithmetic's rounding margin.
        auto cancelling = anima::identity();
        cancelling[0] = -1e8F;
        cancelling[4] = 1;
        cancelling[12] = -2;
        cancelling[3] = 1e8F;
        const auto contact = box({1, 0, .5F}, {1, 0, .5F});
        volatile float clip_x = cancelling[0] * contact.minimum.x;
        clip_x = clip_x + cancelling[12];
        volatile float clip_w = cancelling[3] * contact.minimum.x;
        clip_w = clip_w + cancelling[15];
        require(clip_x + clip_w == 0, "Cancellation fixture did not touch the float clip plane");
        require(anima::RenderFrustum(cancelling).intersects(contact),
                "Float clip contact lost to camera-row cancellation");

        std::mt19937 random(7139);
        std::uniform_real_distribution<float> coordinate(-25, 25), size(0, .75F);
        const auto camera = anima::operator*(projection, anima::look_at({3, 4, 7}, {-2, 1, 0}));
        unsigned rejected_boxes = 0;
        for (const auto &view : {anima::identity(), projection, camera, infinite, reverse}) {
            const anima::RenderFrustum frustum(view);
            for (unsigned trial = 0; trial < 4000; ++trial) {
                const anima::Vec3 center{coordinate(random), coordinate(random), coordinate(random)};
                const anima::Vec3 extent{size(random), size(random), size(random)};
                const auto bounds = box(center - extent, center + extent);
                const bool visible = frustum.intersects(bounds);
                require(visible || !reference(view, bounds), "Culling rejected a homogeneous-oracle intersection");
                rejected_boxes += !visible;
            }
        }
        require(rejected_boxes > 10000, "Culling oracle coverage did not exercise rejection");
        // A large translation stresses cancellation; projection scaling must not
        // change visibility or create NaNs by normalizing a degenerate far plane.
        auto translated = anima::identity();
        translated[12] = -100000;
        const auto touching = box({100001, -.1F, .4F}, {100002, .1F, .6F});
        for (float scale : {1e-20F, 1.F, 1e20F}) {
            auto view = translated;
            for (auto &entry : view)
                entry *= scale;
            require(anima::RenderFrustum(view).intersects(touching), "Scaled large-coordinate contact rejected");
        }
        std::cout << "PASS Vulkan clip planes, conservative contact, projection variants and 20000 oracle boxes\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

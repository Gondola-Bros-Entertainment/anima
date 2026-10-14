#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace anima::detail {
// Gives each texel of a @p width by @p height region that @p covered marks 0 the values of the nearest texel that it
// marks nonzero, nearest by steps between edge neighbors, as bake_impostor() fills the texels of its frames that no
// surface covers and VectorAtlas the transparent texels of its frames and of the border around each. A breadth-first
// search from the marked texels, taken in row order, settles ties. @p covered holds one entry per texel, row by row,
// and marks each texel as it receives values, so it ends all nonzero unless it began all zero, when nothing changes.
// Takes time and memory proportional to the region's texels.
//
// @p copy(from, to) gives texel @p to, which @p covered marked 0, the values of texel @p from, both indices into
// @p covered; @p from already holds its final values.
template <class Copy>
void dilate(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> &covered, Copy &&copy) {
    std::deque<std::uint32_t> queue;
    for (std::uint32_t texel = 0; texel < width * height; ++texel)
        if (covered[texel])
            queue.push_back(texel);
    while (!queue.empty()) {
        const auto texel = queue.front();
        queue.pop_front();
        const auto s = texel % width, t = texel / width;
        const std::array<std::pair<long, long>, 4> steps{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
        for (const auto &[dx, dy] : steps) {
            const long x = long(s) + dx, y = long(t) + dy;
            if (x < 0 || y < 0 || x >= long(width) || y >= long(height))
                continue;
            const auto next = std::uint32_t(y) * width + std::uint32_t(x);
            if (covered[next])
                continue;
            covered[next] = 1;
            copy(texel, next);
            queue.push_back(next);
        }
    }
}
} // namespace anima::detail

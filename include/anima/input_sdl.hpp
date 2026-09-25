#pragma once
#include <anima/input.hpp>
#include <optional>
union SDL_Event;
namespace anima::input {
// Pure SDL 3.2+ event conversion; no SDL initialization, event pump, window or
// device ownership. Pass the nonzero window ID owned by this input context.
// Text/IME, mouse motion/wheel, raw joysticks and device opening stay external.
std::optional<Event> from_sdl(const SDL_Event &event, std::uint32_t window);
} // namespace anima::input

#pragma once
#include <anima/components.hpp>
#include <span>

namespace anima::detail {
// Throws `std::invalid_argument` when two of @p data have the same type. It needs no codecs, so staging checks it
// before ComponentCodecs::validate, which checks it too, can run.
void require_distinct_component_types(std::span<const ComponentData> data);
} // namespace anima::detail

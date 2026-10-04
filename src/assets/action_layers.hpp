#pragma once
#include <anima/assets/action_runtime.hpp>
namespace anima::detail {
// The layers of one action phase at a phase progress, as one MotionRuntime::evaluate applies them. Evaluating its
// world-only result again would fail below a collapsed joint, so a caller that solves contacts adds them to controls.
struct PhaseLayers {
    // The base pose, or the pose of the full-body layer's clip when that layer has weight 1.
    Pose source;
    // Every other layer above weight 0, in order; a full-body layer has an empty mask.
    MotionControls controls;
};
// Samples the layers of @p phase, an action phase decoded for @p motion, at @p progress over @p base.
PhaseLayers phase_layers(const MotionRuntime &motion, const ActionPhaseBinding &phase, double progress, Pose base);
} // namespace anima::detail

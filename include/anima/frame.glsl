// GLSL declarations of the frame block that every application shader reads, those of custom materials
// (anima/custom_material.glsl) and of sprites (anima/sprite.glsl) alike, as anima/custom_material.hpp documents it, with
// the helpers that read it and the declarations of fog.glsl and visibility.glsl, which it includes.
#ifndef ANIMA_FRAME_GLSL
#define ANIMA_FRAME_GLSL

layout(set = 0, binding = 0, std140) uniform AnimaFrame {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    // The eye with w 1, or in an orthographic view the unit direction toward the camera with w 0.
    vec4 viewOrigin;
    vec4 sunDirection;
    vec4 sunIrradiance;
    vec4 fillDirection;
    vec4 fillIrradiance;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 ambientSpecular;
    // The fog's color, and in w its density per unit at its height (fogShape.x), which fogShape.y's falloff varies.
    vec4 fog;
    // The scene target's width and height in pixels, the window's times the render scale, and their reciprocals.
    vec4 viewport;
    float time;
    // The fog's height, falloff, sky distance and phase asymmetry.
    vec4 fogShape;
    // The sunlight that the fog scatters, before its phase function: HeightFog::sun_scattering times sunIrradiance.
    vec4 fogSun;
}
animaFrame;

#include "fog.glsl"
#include "visibility.glsl"

// @p color at world position @p position as the standard material fogs it: seen from the eye through the
// environment's fog in a perspective view with fog, and unchanged otherwise.
vec3 animaFogged(vec3 color, vec3 position) {
    if (animaFrame.viewOrigin.w > 0.5 && animaFrame.fog.w > 0.0)
        color = animaFog(color, animaFrame.viewOrigin.xyz, position, animaFrame.fog, animaFrame.fogShape,
                         animaFrame.fogSun, animaFrame.sunDirection.xyz);
    return color;
}

// The world position that Vulkan depth @p depth shows at the framebuffer coordinates @p pixel, such as
// gl_FragCoord.xy.
vec3 animaWorldPosition(vec2 pixel, float depth) {
    vec4 world = animaFrame.inverseViewProjection * vec4(pixel * animaFrame.viewport.zw * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

#endif

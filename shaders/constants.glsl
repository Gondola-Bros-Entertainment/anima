// Constants that the renderer's shaders share; lighting.glsl and atmosphere.glsl include it. The public
// include/anima/fog.glsl, which custom materials compile without this directory, keeps its own pi and declares
// animaMaximumHalfFloat, the largest half float, at which the built-in shaders cap their colors.
#ifndef ANIMA_CONSTANTS_GLSL
#define ANIMA_CONSTANTS_GLSL

const float PI = 3.14159265359;

#endif

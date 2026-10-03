// The environment's uniform block, which the world's fragment shaders and the atmosphere's compute passes share.
layout(set = 1, binding = 0, std140) uniform EnvironmentData {
    // The sun's radiance at the ground (atmosphere_sunlight()), which lights surfaces and the fog.
    vec4 sunDirection, sunRadiance, fillDirection, fillRadiance;
    vec4 ambientSky, ambientGround, ambientSpecular;
    // The atmosphere's Rayleigh scattering per meter with its scale height in w, its Mie scattering with its scale
    // height, and its Mie absorption with the Mie phase function's asymmetry.
    vec4 atmosphereRayleigh, atmosphereMie, atmosphereMieAbsorption;
    vec4 fog, controls;
    // Takes a pixel's normalized device coordinates (x, y, 0, 1) to a vector along its ray from the eye in a
    // perspective view, formed relative to the eye so that its distance from the world's origin costs no precision.
    mat4 viewRays;
    vec4 viewOrigin;
    // The sun's shadow cascades fitted to the view: each one's view-projection,
    mat4 cascadeView[4];
    // the view depth where each one ends,
    vec4 cascadeEnd;
    // and its constant and slope biases in its normalized depth, and the world size of its texels.
    vec4 cascadeBias[4];
    // The point that view depths are measured from, with the depth where the first cascade begins in w.
    vec4 cascadeOrigin;
    // The unit view direction along which they are measured, with the share of each cascade that blends into the next
    // in w.
    vec4 cascadeAxis;
    // The number of cascades, none while they are disabled.
    vec4 cascades;
    mat4 detailShadowView;
    vec4 detailShadow; // enabled, inverse resolution, constant bias, slope bias
    // The fog's height, falloff, sky distance and phase asymmetry, and the sunlight it scatters (fog.glsl).
    vec4 fogShape, fogSun;
    // The atmosphere's ozone absorption per meter with the altitude of its peak; the ground's radius, the thickness,
    // sent apart from the radius whose float spacing would swallow it, the ozone layer's width and the sun's angular
    // radius; the ground's albedo with the eye's altitude; and
    // the sun's radiance above the atmosphere, with 1 in w while the atmosphere is enabled.
    vec4 atmosphereOzone, atmosphereShape, atmosphereGround, atmosphereSun;
}
environment;

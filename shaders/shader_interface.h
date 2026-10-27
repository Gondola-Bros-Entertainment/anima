// The numbers that the built-in shaders and the renderer must agree on: descriptor sets and bindings, vertex attribute
// locations, specialization constant IDs, what a draw's push constants pack, and the atmosphere's table sizes and
// workgroups. The shaders here include it, and so do src/desktop/vulkan_renderer.cpp and, for the vertex attributes
// that custom materials read and the sprite values that sprite shaders read, src/assets/custom_material.cpp, so it
// holds only macros, which GLSL's preprocessor and C++'s both read. A mismatch between same-typed bindings, attributes
// or specialization IDs passes validation, so neither side states these numbers itself. The public
// include/anima/custom_material.glsl and include/anima/sprite.glsl state their own.
#ifndef ANIMA_SHADER_INTERFACE_H
#define ANIMA_SHADER_INTERFACE_H

// Descriptor sets. Set 0 belongs to each pipeline layout: the material's in the world's pipelines, the image that the
// resolve pass and each bloom pass read, and the atmosphere's tables in its compute passes. Every layout that reads the
// environment binds it at set 1, the resource pipelines bind their poses at set 2, and the resolve pass binds the bloom
// chain's first level at set 2, as a scene input set.
#define ANIMA_SET_MATERIAL 0
#define ANIMA_SET_SCENE_INPUT 0
#define ANIMA_SET_ATMOSPHERE 0
#define ANIMA_SET_ENVIRONMENT 1
#define ANIMA_SET_POSES 2
#define ANIMA_SET_BLOOM 2
// The sprite pipelines' sets, built-in and sprite shaders' alike: the frame block that custom materials read too, then
// the sheet's texture.
#define ANIMA_SET_SPRITE_FRAME 0
#define ANIMA_SET_SPRITE_SHEET 1

// The material set (material.glsl): its textures in material_texture_count order, then its uniform block.
#define ANIMA_MATERIAL_BASE_COLOR 0
#define ANIMA_MATERIAL_NORMAL 1
#define ANIMA_MATERIAL_METALLIC_ROUGHNESS 2
#define ANIMA_MATERIAL_EMISSIVE 3
#define ANIMA_MATERIAL_OCCLUSION 4
#define ANIMA_MATERIAL_UNIFORM 5

// The scene input set: the scene target that resolve.frag displays and the bright pass reads, or the level of the bloom
// chain that a bloom pass or resolve.frag reads.
#define ANIMA_SCENE_INPUT_COLOR 0

// The environment set: its uniform block (environment_data.glsl), the depth maps of the shadow cascades and of the
// detail region (environment.glsl), and the atmosphere's sky view and transmittance tables, which the sky samples.
#define ANIMA_ENVIRONMENT_UNIFORM 0
#define ANIMA_ENVIRONMENT_CASCADE_DEPTH 1
#define ANIMA_ENVIRONMENT_DETAIL_SHADOW_DEPTH 2
#define ANIMA_ENVIRONMENT_SKY_VIEW 3
#define ANIMA_ENVIRONMENT_TRANSMITTANCE 4

// The atmosphere set of the compute passes: each table as a storage image to write, then the transmittance and
// multiple scattering tables to sample.
#define ANIMA_ATMOSPHERE_WRITE_TRANSMITTANCE 0
#define ANIMA_ATMOSPHERE_WRITE_MULTIPLE_SCATTERING 1
#define ANIMA_ATMOSPHERE_WRITE_SKY_VIEW 2
#define ANIMA_ATMOSPHERE_READ_TRANSMITTANCE 3
#define ANIMA_ATMOSPHERE_READ_MULTIPLE_SCATTERING 4

// The pose set: the frame's palettes, as matrices.
#define ANIMA_POSE_MATRICES 0

// The sprite sheet set: the sheet's texture.
#define ANIMA_SPRITE_SHEET_TEXTURE 0

// Vertex attribute locations (resource.vert): the SourceVertex fields, from the mesh's vertices, then rows 0 to 2 of
// the affine matrix of the placement being drawn, per instance. Custom materials read the same locations
// (anima/custom_material.hpp), which the renderer supplies from the same attribute descriptions.
#define ANIMA_ATTRIBUTE_POSITION 0
#define ANIMA_ATTRIBUTE_NORMAL 1
#define ANIMA_ATTRIBUTE_COLOR 2
#define ANIMA_ATTRIBUTE_UV 3
#define ANIMA_ATTRIBUTE_JOINTS 4
#define ANIMA_ATTRIBUTE_WEIGHTS 5
#define ANIMA_ATTRIBUTE_TANGENT 6
#define ANIMA_ATTRIBUTE_ALPHA 7
#define ANIMA_ATTRIBUTE_PLACEMENT_ROW0 8
#define ANIMA_ATTRIBUTE_PLACEMENT_ROW1 9
#define ANIMA_ATTRIBUTE_PLACEMENT_ROW2 10

// Sprite attribute locations (sprite.vert), all per instance: the position and rotation, the size and pivot, the
// frame's texture coordinates at its first column and row and past its last, the color and alpha, and the parameters.
#define ANIMA_SPRITE_PLACEMENT 0
#define ANIMA_SPRITE_SHAPE 1
#define ANIMA_SPRITE_FRAME 2
#define ANIMA_SPRITE_COLOR 3
#define ANIMA_SPRITE_PARAMETERS 4

// What sprite.vert passes to the fragment shader, which sprite shaders read (anima/sprite.glsl): the texture
// coordinates in the sheet, the color and alpha, the coordinates across the sprite from 0 to 1, and the parameters.
#define ANIMA_SPRITE_OUT_SHEET_UV 0
#define ANIMA_SPRITE_OUT_COLOR 1
#define ANIMA_SPRITE_OUT_UV 2
#define ANIMA_SPRITE_OUT_PARAMETERS 3

// Specialization constant IDs, which each shader stage numbers apart. In the fragment stage of the view's pipelines:
// mesh.frag's premultiplied output, the height fog's code (environment.glsl), mesh.frag's discards, the 2x2 shadow
// filter (environment.glsl) and the impostor's single frame (impostor_sample.glsl).
#define ANIMA_SPEC_BLENDED 0
#define ANIMA_SPEC_HEIGHT_FOG 1
#define ANIMA_SPEC_MAY_DISCARD 2
#define ANIMA_SPEC_BILINEAR_SHADOW_FILTER 3
#define ANIMA_SPEC_SINGLE_IMPOSTOR_FRAME 4
// impostor.vert's view along the sun, in the shadow pipeline.
#define ANIMA_SPEC_SHADOW_PASS 0
// resolve.frag's encoding to sRGB.
#define ANIMA_SPEC_ENCODE_SRGB 0
// bloom-downsample.frag's bright pass.
#define ANIMA_SPEC_BLOOM_BRIGHT_PASS 0

// A draw's push constants (resource.vert): the flags in indices.w, set with placements and with a visibility range.
#define ANIMA_DRAW_PLACED 1u
#define ANIMA_DRAW_RANGED 2u
// An impostor's indices.y: its frames per side in the bits of the mask, and its ImpostorLayout from the shift, with
// the hemisphere's and the sphere's values.
#define ANIMA_IMPOSTOR_COUNT_MASK 0xFFu
#define ANIMA_IMPOSTOR_LAYOUT_SHIFT 8
#define ANIMA_IMPOSTOR_HEMISPHERE 0u
#define ANIMA_IMPOSTOR_SPHERE 1u
// With placements or a visibility range, the draw's pose header in the pose set: the object's world matrix at
// indices.z, then its range's matrix at ANIMA_POSE_HEADER_RANGE past it, and then the palette, after the header's
// ANIMA_POSE_HEADER_MATRICES. custom_material.glsl reads the same header.
#define ANIMA_POSE_HEADER_RANGE 1u
#define ANIMA_POSE_HEADER_MATRICES 2

// The atmosphere's tables, in texels (atmosphere.glsl).
#define ANIMA_TRANSMITTANCE_WIDTH 256
#define ANIMA_TRANSMITTANCE_HEIGHT 64
#define ANIMA_MULTIPLE_SCATTERING_WIDTH 64
#define ANIMA_MULTIPLE_SCATTERING_HEIGHT 32
#define ANIMA_SKY_VIEW_WIDTH 192
#define ANIMA_SKY_VIEW_HEIGHT 108
// The edge of the square workgroups that write the transmittance and sky view tables, one invocation per texel.
#define ANIMA_ATMOSPHERE_GROUP_EDGE 8
// The invocations of the workgroup that writes each texel of the multiple scattering table.
#define ANIMA_SCATTERING_INVOCATIONS 64

#endif

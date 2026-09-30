// The one translation unit that compiles miniaudio's implementation, with stb_vorbis for Ogg Vorbis. Both come from
// a system include directory, so the project's warnings do not apply to the vendored code. The configuration macros
// come from the anima_audio_dependencies target, which gives every translation unit that includes miniaudio.h the
// same ones, and with them the same structure layouts.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
// stb_vorbis's implementation must follow miniaudio's.
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

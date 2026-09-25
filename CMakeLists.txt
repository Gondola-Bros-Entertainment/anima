cmake_minimum_required(VERSION 3.25)
project(Anima VERSION 0.1.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
option(ANIMA_BUILD_DESKTOP "Build the SDL3/Vulkan desktop library" ON)
option(ANIMA_BUILD_AUDIO_OUTPUT "Build optional SDL audio device output without graphics" OFF)
option(ANIMA_BUILD_INPUT_SDL "Build optional SDL event converter (headers only)" OFF)
option(ANIMA_BUILD_ASSETS "Build the headless asset/animation library" ON)
option(ANIMA_BUILD_PHYSICS2D "Build optional headless Box2D physics" OFF)
option(ANIMA_BUILD_PHYSICS "Build optional headless Jolt physics" OFF)
option(ANIMA_BUILD_UI_DOCUMENTS "Build headless RmlUi document API" OFF)
option(ANIMA_BUILD_UI "Build the optional RmlUi player-facing UI integration" OFF)
if(ANIMA_BUILD_UI AND NOT ANIMA_BUILD_DESKTOP)
    message(FATAL_ERROR "ANIMA_BUILD_UI requires ANIMA_BUILD_DESKTOP")
endif()
option(ANIMA_BUILD_TOOLS "Build Anima viewer and command-line tools" ${PROJECT_IS_TOP_LEVEL})
set(anima_tests_default OFF)
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    set(anima_tests_default ON)
endif()
option(ANIMA_BUILD_TESTS "Build and register Anima tests" ${anima_tests_default})
if(ANIMA_BUILD_TESTS)
    enable_testing()
endif()
include(cmake/AnimaRuntime.cmake)
option(ANIMA_FETCH_SDL "Fetch pinned SDL 3.4.2 instead of using an installed package" OFF)
option(ANIMA_WARNINGS_AS_ERRORS "Treat Anima compiler warnings as errors" OFF)
include(cmake/AnimaSanitizers.cmake)
option(ANIMA_BUILD_DOCS "Provide the optional local Doxygen reference target" OFF)
if(ANIMA_BUILD_DOCS)
    add_subdirectory(docs)
endif()

function(anima_target_defaults target)
    anima_enable_sanitizers(${target})
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(ANIMA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(ANIMA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(anima_core STATIC
    src/core/fixed_step.cpp
    src/core/transform.cpp
    src/core/heightfield.cpp
    src/core/triangle_query.cpp
    src/core/capabilities.cpp
    src/runtime/audio.cpp
    src/runtime/navigation.cpp
    src/runtime/input.cpp
)
add_library(anima::core ALIAS anima_core)
target_include_directories(anima_core PUBLIC include)
anima_target_defaults(anima_core)

if(ANIMA_BUILD_TOOLS)
    add_executable(anima_headless apps/headless/main.cpp)
    target_link_libraries(anima_headless PRIVATE anima::core)
    anima_target_defaults(anima_headless)
endif()

if(ANIMA_BUILD_ASSETS OR ANIMA_BUILD_UI)
    add_library(anima_image_dependencies STATIC src/assets/image.cpp)
    anima_enable_sanitizers(anima_image_dependencies)
    target_include_directories(anima_image_dependencies SYSTEM PUBLIC third_party/stb)
endif()

if(ANIMA_BUILD_ASSETS)
    add_library(anima_asset_dependencies STATIC src/assets/third_party.cpp)
    anima_enable_sanitizers(anima_asset_dependencies)
    target_include_directories(anima_asset_dependencies SYSTEM PUBLIC third_party/cgltf third_party/stb)
    target_compile_features(anima_asset_dependencies PRIVATE cxx_std_20)
    add_library(anima_assets STATIC
    src/assets/mesh_snapshot.cpp
    src/assets/animation.cpp
    src/assets/evaluation.cpp
    src/assets/manifest.cpp
    src/assets/preview.cpp
    src/assets/audio_scene.cpp
    src/assets/camera.cpp
    src/assets/lighting.cpp
    src/assets/input_scene.cpp
    src/assets/navigation_scene.cpp
    src/assets/scene_validation.cpp
    src/assets/scene.cpp
    src/assets/scene_set.cpp
    src/assets/components.cpp
    src/assets/prefab.cpp
    src/assets/mesh.cpp
    src/assets/terrain.cpp
    src/assets/animator.cpp
    src/assets/imports.cpp
    src/assets/render_visibility.cpp
    src/assets/texture_mips.cpp
    src/assets/material_textures.cpp
    src/assets/mesh_preparation.cpp
    src/assets/motion_runtime.cpp
    src/assets/action_runtime.cpp
    src/assets/interaction_runtime.cpp
    src/assets/actor_presentation.cpp
    src/assets/attachments.cpp
    src/assets/fitted.cpp
)
    add_library(anima::assets ALIAS anima_assets)
    target_include_directories(anima_assets PUBLIC include)
    target_include_directories(anima_assets SYSTEM PRIVATE third_party)
    target_link_libraries(anima_assets PUBLIC anima::core PRIVATE anima_asset_dependencies anima_image_dependencies)
    anima_target_defaults(anima_assets)
    if(ANIMA_BUILD_TOOLS)
        add_executable(anima_inspect apps/inspect/main.cpp)
        target_link_libraries(anima_inspect PRIVATE anima::assets)
        anima_target_defaults(anima_inspect)
    endif()
    if(ANIMA_BUILD_TESTS)
        add_executable(anima_image_decoder_tests tests/image_decoder_tests.cpp)
        target_include_directories(anima_image_decoder_tests SYSTEM PRIVATE third_party/stb)
        anima_target_defaults(anima_image_decoder_tests)
        add_test(NAME image_decoder COMMAND anima_image_decoder_tests)
        add_executable(anima_asset_tests tests/asset_tests.cpp)
        target_link_libraries(anima_asset_tests PRIVATE anima::assets)
        anima_target_defaults(anima_asset_tests)
        add_test(NAME asset_import COMMAND anima_asset_tests)
        add_executable(anima_animation_tests tests/animation_tests.cpp)
        target_link_libraries(anima_animation_tests PRIVATE anima::assets)
        anima_target_defaults(anima_animation_tests)
        add_test(NAME animation_runtime COMMAND anima_animation_tests)
        add_executable(anima_evaluation_tests tests/evaluation_tests.cpp)
        target_link_libraries(anima_evaluation_tests PRIVATE anima::assets)
        anima_target_defaults(anima_evaluation_tests)
        add_test(NAME animation_evaluation COMMAND anima_evaluation_tests)
        add_executable(anima_action_tests tests/action_tests.cpp)
        target_link_libraries(anima_action_tests PRIVATE anima::assets)
        anima_target_defaults(anima_action_tests)
        add_test(NAME animation_actions COMMAND anima_action_tests)
        add_executable(anima_interaction_tests tests/interaction_tests.cpp)
        target_link_libraries(anima_interaction_tests PRIVATE anima::assets)
        anima_target_defaults(anima_interaction_tests)
        add_test(NAME animation_interactions COMMAND anima_interaction_tests)
        add_executable(anima_scene_tests tests/scene_tests.cpp)
        target_link_libraries(anima_scene_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_tests)
        add_test(NAME scene_snapshots COMMAND anima_scene_tests)
        add_executable(anima_scene_validation_tests tests/scene_validation_tests.cpp)
        target_link_libraries(anima_scene_validation_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_validation_tests)
        add_test(NAME scene_validation COMMAND anima_scene_validation_tests)
        add_executable(anima_render_scene_tests tests/render_scene_tests.cpp)
        target_link_libraries(anima_render_scene_tests PRIVATE anima::assets)
        anima_target_defaults(anima_render_scene_tests)
        add_test(NAME render_scene COMMAND anima_render_scene_tests)
        add_executable(anima_scene_object_tests tests/scene_object_tests.cpp)
        target_link_libraries(anima_scene_object_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_object_tests)
        add_test(NAME scene_objects COMMAND anima_scene_object_tests)
        add_executable(anima_scene_lifecycle_tests tests/scene_lifecycle_tests.cpp)
        target_link_libraries(anima_scene_lifecycle_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_lifecycle_tests)
        add_test(NAME scene_lifecycle COMMAND anima_scene_lifecycle_tests)
        add_executable(anima_scene_reference_tests tests/scene_reference_tests.cpp)
        target_link_libraries(anima_scene_reference_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_reference_tests)
        add_test(NAME scene_references COMMAND anima_scene_reference_tests)
        add_executable(anima_scene_set_tests tests/scene_set_tests.cpp)
        target_link_libraries(anima_scene_set_tests PRIVATE anima::assets)
        anima_target_defaults(anima_scene_set_tests)
        add_test(NAME scene_set COMMAND anima_scene_set_tests)
        add_executable(anima_audio_scene_tests tests/audio_scene_tests.cpp)
        target_link_libraries(anima_audio_scene_tests PRIVATE anima::assets)
        anima_target_defaults(anima_audio_scene_tests)
        add_test(NAME audio_scene COMMAND anima_audio_scene_tests)
        add_executable(anima_camera_tests tests/camera_tests.cpp)
        target_link_libraries(anima_camera_tests PRIVATE anima::assets)
        anima_target_defaults(anima_camera_tests)
        add_test(NAME camera_views COMMAND anima_camera_tests)
        add_executable(anima_lighting_tests tests/lighting_tests.cpp)
        target_link_libraries(anima_lighting_tests PRIVATE anima::assets)
        anima_target_defaults(anima_lighting_tests)
        add_test(NAME lighting COMMAND anima_lighting_tests)
        add_executable(anima_input_scene_tests tests/input_scene_tests.cpp)
        target_link_libraries(anima_input_scene_tests PRIVATE anima::assets)
        anima_target_defaults(anima_input_scene_tests)
        add_test(NAME input_scene COMMAND anima_input_scene_tests)
        add_executable(anima_navigation_scene_tests tests/navigation_scene_tests.cpp)
        target_link_libraries(anima_navigation_scene_tests PRIVATE anima::assets)
        anima_target_defaults(anima_navigation_scene_tests)
        add_test(NAME navigation_scene COMMAND anima_navigation_scene_tests)
        add_executable(anima_import_tests tests/import_tests.cpp)
        target_link_libraries(anima_import_tests PRIVATE anima::assets)
        anima_target_defaults(anima_import_tests)
        add_test(NAME asset_reimport COMMAND anima_import_tests)
        add_executable(anima_render_visibility_tests tests/render_visibility_tests.cpp)
        target_link_libraries(anima_render_visibility_tests PRIVATE anima::assets)
        anima_target_defaults(anima_render_visibility_tests)
        add_test(NAME render_visibility COMMAND anima_render_visibility_tests)
        add_executable(anima_environment_tests tests/environment_tests.cpp)
        target_link_libraries(anima_environment_tests PRIVATE anima_assets)
        anima_target_defaults(anima_environment_tests)
        add_test(NAME environment_contract COMMAND anima_environment_tests)
        add_executable(anima_texture_mip_tests tests/texture_mip_tests.cpp)
        target_link_libraries(anima_texture_mip_tests PRIVATE anima::assets)
        anima_target_defaults(anima_texture_mip_tests)
        add_test(NAME texture_mips COMMAND anima_texture_mip_tests)
        set(ANIMA_TEST_MANIFEST "" CACHE FILEPATH "Optional model manifest for preview integration tests")
        if(ANIMA_TEST_MANIFEST)
            add_test(NAME asset_preview COMMAND anima_animation_tests "${ANIMA_TEST_MANIFEST}")
        endif()
        set(ANIMA_TEST_GLB "" CACHE FILEPATH "Optional exported GLB for integration tests")
        if(ANIMA_TEST_GLB)
            add_test(NAME exported_import COMMAND anima_asset_tests "${ANIMA_TEST_GLB}")
            add_test(NAME exported_instances COMMAND anima_scene_tests "${ANIMA_TEST_GLB}")
        endif()
    endif()
endif()

if(ANIMA_BUILD_TESTS)
    add_executable(anima_input_tests tests/input_tests.cpp)
    target_link_libraries(anima_input_tests PRIVATE anima::core)
    anima_target_defaults(anima_input_tests)
    add_test(NAME input_actions COMMAND anima_input_tests)
    add_executable(anima_navigation_tests tests/navigation_tests.cpp)
    target_link_libraries(anima_navigation_tests PRIVATE anima::core)
    anima_target_defaults(anima_navigation_tests)
    add_test(NAME navigation_queries COMMAND anima_navigation_tests)
    add_executable(anima_audio_tests tests/audio_tests.cpp)
    target_link_libraries(anima_audio_tests PRIVATE anima::core)
    anima_target_defaults(anima_audio_tests)
    add_test(NAME audio_mixer COMMAND anima_audio_tests)
    add_executable(anima_core_tests tests/core_tests.cpp)
    target_link_libraries(anima_core_tests PRIVATE anima::core)
    anima_target_defaults(anima_core_tests)
    add_test(NAME core_timing COMMAND anima_core_tests)
    add_executable(anima_heightfield_tests tests/heightfield_tests.cpp)
    target_link_libraries(anima_heightfield_tests PRIVATE anima::core)
    anima_target_defaults(anima_heightfield_tests)
    add_test(NAME heightfield COMMAND anima_heightfield_tests)
    add_executable(anima_triangle_query_tests tests/triangle_query_tests.cpp)
    target_link_libraries(anima_triangle_query_tests PRIVATE anima::core)
    anima_target_defaults(anima_triangle_query_tests)
    add_test(NAME triangle_query COMMAND anima_triangle_query_tests)
    set_tests_properties(heightfield PROPERTIES TIMEOUT 20)
    if(ANIMA_BUILD_TOOLS)
        add_test(NAME headless_smoke COMMAND anima_headless)
    endif()
endif()

if(ANIMA_BUILD_DESKTOP OR ANIMA_BUILD_AUDIO_OUTPUT)
    include(cmake/AnimaSdl.cmake)
endif()

if(ANIMA_BUILD_AUDIO_OUTPUT)
    add_library(anima_audio_output STATIC src/audio_output/audio_output.cpp)
    add_library(anima::audio_output ALIAS anima_audio_output)
    target_include_directories(anima_audio_output PUBLIC include)
    target_link_libraries(anima_audio_output PUBLIC anima::core PRIVATE SDL3::SDL3)
    anima_target_defaults(anima_audio_output)
    if(ANIMA_BUILD_TESTS)
        add_executable(anima_audio_output_tests tests/audio_output_tests.cpp)
        target_link_libraries(anima_audio_output_tests PRIVATE anima::audio_output SDL3::SDL3)
        anima_target_defaults(anima_audio_output_tests)
        anima_copy_sdl_runtime(anima_audio_output_tests)
        add_test(NAME audio_output COMMAND anima_audio_output_tests)
        set_tests_properties(audio_output PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 20)
    endif()
endif()

if(ANIMA_BUILD_DESKTOP)
    if(NOT WIN32 AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The desktop bootstrap targets Windows, macOS, and Linux only")
    endif()
    find_package(Vulkan 1.1 REQUIRED COMPONENTS glslc)
    # Vulkan 1.1 is the device baseline. Building also needs declarations for
    # optional presentation-lifetime extensions, even on devices without them.
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${Vulkan_INCLUDE_DIRS}")
    check_cxx_source_compiles([=[
        #include <vulkan/vulkan.h>
        #if !defined(VK_EXT_surface_maintenance1) || !defined(VK_EXT_swapchain_maintenance1)
        #error Missing Vulkan maintenance extension declarations
        #endif
        int main() {
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT features{};
            VkSwapchainPresentFenceInfoEXT fences{};
            const char* names[] = {VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                                   VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
            (void)features; (void)fences; (void)names;
            return 0;
        }
    ]=] ANIMA_VULKAN_MAINTENANCE_HEADERS)
    cmake_pop_check_state()
    if(NOT ANIMA_VULKAN_MAINTENANCE_HEADERS)
        message(FATAL_ERROR
            "Anima needs Vulkan SDK headers declaring VK_EXT_surface_maintenance1 "
            "and VK_EXT_swapchain_maintenance1. Install newer SDK headers; "
            "the runtime device baseline remains Vulkan 1.1.")
    endif()
    set(shader_outputs)
    set(shader_sources triangle.vert triangle.frag)
    if(ANIMA_BUILD_ASSETS)
        list(APPEND shader_sources mesh.frag resource.vert sky.vert sky.frag shadow-resource.vert shadow.frag resolve.frag)
    endif()
    if(ANIMA_BUILD_UI)
        list(APPEND shader_sources ui.vert ui.frag)
    endif()
    foreach(shader IN LISTS shader_sources)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${shader}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND Vulkan::glslc --target-env=vulkan1.1 -mfmt=c
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/environment.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.glsl"
            VERBATIM)
        list(APPEND shader_outputs "${output}")
    endforeach()

    add_library(anima_desktop STATIC src/desktop/vulkan_renderer.cpp ${shader_outputs})
    add_library(anima::desktop ALIAS anima_desktop)
    target_include_directories(anima_desktop PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(anima_desktop PUBLIC SDL3::SDL3 anima::core PRIVATE Vulkan::Vulkan)
    if(ANIMA_BUILD_ASSETS)
        target_link_libraries(anima_desktop PUBLIC anima::assets)
        target_compile_definitions(anima_desktop PUBLIC ANIMA_HAS_ASSETS=1)
    endif()
    anima_target_defaults(anima_desktop)
    if(ANIMA_BUILD_UI)
        target_compile_definitions(anima_desktop PRIVATE ANIMA_UI=1)
    endif()
    # Vulkan's structure initialization intentionally zeros trailing fields.
    if(NOT MSVC)
        target_compile_options(anima_desktop PRIVATE -Wno-missing-field-initializers)
    endif()

    if(ANIMA_BUILD_TOOLS)
        add_library(anima_viewer STATIC apps/desktop/viewer_application.cpp apps/desktop/viewer_options.cpp)
        target_link_libraries(anima_viewer PUBLIC anima::core anima::desktop)
        anima_target_defaults(anima_viewer)
        add_executable(anima apps/desktop/main.cpp)
        target_link_libraries(anima PRIVATE anima_viewer)
        anima_target_defaults(anima)
        anima_copy_sdl_runtime(anima)
        if(ANIMA_BUILD_TESTS)
            add_executable(anima_check tests/desktop/main.cpp)
            target_link_libraries(anima_check PRIVATE anima_viewer)
            anima_target_defaults(anima_check)
            anima_copy_sdl_runtime(anima_check)
            add_test(NAME desktop_no_scripted_mode COMMAND anima --smoke)
            set_tests_properties(desktop_no_scripted_mode PROPERTIES WILL_FAIL TRUE)
            add_test(NAME desktop_help COMMAND anima --help)
            add_test(NAME desktop_invalid_frames COMMAND anima --frames 0)
            set_tests_properties(desktop_invalid_frames PROPERTIES WILL_FAIL TRUE)
        endif()
    endif()
endif()

if(ANIMA_BUILD_INPUT_SDL)
    include(cmake/AnimaInput.cmake)
endif()

if(ANIMA_BUILD_PHYSICS2D)
    include(cmake/AnimaPhysics2d.cmake)
endif()

if(ANIMA_BUILD_PHYSICS)
    include(cmake/AnimaPhysics.cmake)
endif()

if(ANIMA_BUILD_TESTS AND ANIMA_BUILD_PHYSICS AND ANIMA_BUILD_PHYSICS2D)
    add_executable(anima_physics_backends_tests tests/physics_backends_tests.cpp)
    target_link_libraries(anima_physics_backends_tests PRIVATE anima::physics anima::physics2d)
    anima_target_defaults(anima_physics_backends_tests)
    add_test(NAME physics_backends COMMAND anima_physics_backends_tests)
    if(ANIMA_BUILD_ASSETS)
        add_executable(anima_runtime_tests tests/runtime_tests.cpp)
        target_link_libraries(anima_runtime_tests PRIVATE anima::physics_scene anima::physics2d_scene)
        anima_target_defaults(anima_runtime_tests)
        add_test(NAME runtime COMMAND anima_runtime_tests)
    endif()
endif()

if(ANIMA_BUILD_UI OR ANIMA_BUILD_UI_DOCUMENTS)
    include(cmake/AnimaUi.cmake)
endif()

if(ANIMA_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME repository COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_repository.py")
    set(anima_consumer_modes core)
    if(ANIMA_BUILD_AUDIO_OUTPUT)
        list(APPEND anima_consumer_modes audio_output)
    endif()
    if(ANIMA_BUILD_INPUT_SDL)
        list(APPEND anima_consumer_modes input_sdl)
    endif()
    if(ANIMA_BUILD_PHYSICS2D)
        list(APPEND anima_consumer_modes physics2d)
        if(ANIMA_BUILD_ASSETS)
            list(APPEND anima_consumer_modes physics2d_scene)
        endif()
    endif()
    if(ANIMA_BUILD_PHYSICS)
        list(APPEND anima_consumer_modes physics)
        if(ANIMA_BUILD_ASSETS)
            list(APPEND anima_consumer_modes physics_scene)
        endif()
    endif()
    if(ANIMA_BUILD_ASSETS)
        list(APPEND anima_consumer_modes assets)
    endif()
    if(ANIMA_BUILD_DESKTOP AND ANIMA_BUILD_ASSETS)
        list(APPEND anima_consumer_modes desktop)
    endif()
    if(ANIMA_BUILD_UI OR ANIMA_BUILD_UI_DOCUMENTS)
        list(APPEND anima_consumer_modes ui_documents)
    endif()
    if(ANIMA_BUILD_UI)
        list(APPEND anima_consumer_modes ui)
    endif()
    if(ANIMA_BUILD_ASSETS AND ANIMA_BUILD_PHYSICS AND ANIMA_BUILD_PHYSICS2D)
        list(APPEND anima_consumer_modes runtime)
    endif()
    set(anima_consumer_sdl_args)
    foreach(option STATIC SHARED VIDEO RENDER GPU CAMERA WAYLAND UNIX_CONSOLE_BUILD)
        if(DEFINED SDL_${option})
            list(APPEND anima_consumer_sdl_args "-DCONSUMER_SDL_${option}=${SDL_${option}}")
        endif()
    endforeach()
    function(anima_add_consumer_check name)
        add_test(NAME "${name}"
            COMMAND "${CMAKE_COMMAND}"
                ${ARGN}
                "-DANIMA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DCONSUMER_CONFIG=$<CONFIG>"
                "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
                "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DCONSUMER_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCONSUMER_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCONSUMER_SANITIZERS=${ANIMA_ENABLE_SANITIZERS}"
                "-DCONSUMER_MSVC_DEBUG_INFORMATION_FORMAT=${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}"
                "-DCONSUMER_DISABLE_PRECOMPILE_HEADERS=${CMAKE_DISABLE_PRECOMPILE_HEADERS}"
                "-DCONSUMER_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
                "-DCONSUMER_INPUT_SDL_HEADERS=${ANIMA_INPUT_SDL_INCLUDE_DIR}"
                "-DCONSUMER_SDL_DIR=${SDL3_DIR}"
                "-DCONSUMER_FETCH_SDL=${ANIMA_FETCH_SDL}"
                "-DCONSUMER_SDL_SOURCE=${anima_sdl_source}"
                ${anima_consumer_sdl_args}
                "-DCONSUMER_RMLUI_SOURCE=${anima_rmlui_source}"
                "-DCONSUMER_JOLT_SOURCE=${anima_jolt_source}"
                "-DCONSUMER_BOX2D_SOURCE=${anima_box2d_source}"
                "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
                "-DCONSUMER_TRIPLET=${VCPKG_TARGET_TRIPLET}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer/run.cmake")
    endfunction()
    set(anima_consumer_suite "${CMAKE_CURRENT_BINARY_DIR}/consumer-suite")
    string(JOIN "," anima_consumer_suite_modes ${anima_consumer_modes})
    anima_add_consumer_check(consumer_build
        "-DCONSUMER_BINARY_DIR=${anima_consumer_suite}"
        "-DCONSUMER_MODES=${anima_consumer_suite_modes}" -DCONSUMER_ACTION=build)
    set_tests_properties(consumer_build PROPERTIES
        FIXTURES_SETUP consumer_build TIMEOUT 1200 LABELS "consumer;consumer-build")
    set(anima_consumer_executable_dir "${anima_consumer_suite}/build")
    get_property(anima_consumer_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(anima_consumer_multi_config)
        string(APPEND anima_consumer_executable_dir "/$<CONFIG>")
    endif()
    foreach(mode IN LISTS anima_consumer_modes)
        anima_add_consumer_check(consumer_configure_${mode}
            "-DCONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-configure-${mode}"
            "-DCONSUMER_MODE=${mode}" -DCONSUMER_ACTION=configure)
        set_tests_properties(consumer_configure_${mode} PROPERTIES
            TIMEOUT 180 LABELS "consumer;consumer-configure")
        add_test(NAME consumer_${mode}
            COMMAND "${anima_consumer_executable_dir}/consumer_${mode}${CMAKE_EXECUTABLE_SUFFIX}")
        set_tests_properties(consumer_${mode} PROPERTIES
            FIXTURES_REQUIRED consumer_build TIMEOUT 60 LABELS "consumer;consumer-runtime")
        if(mode STREQUAL "audio_output")
            set_tests_properties(consumer_${mode} PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 20)
        endif()
    endforeach()
endif()

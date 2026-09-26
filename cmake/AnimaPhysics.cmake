# Jolt is private and fetched only for explicitly enabled physics consumers.
include(FetchContent)
function(anima_add_jolt)
    foreach(flag OVERRIDE_CXX_FLAGS INTERPROCEDURAL_OPTIMIZATION ENABLE_ALL_WARNINGS
                 USE_STATIC_MSVC_RUNTIME_LIBRARY DEBUG_RENDERER_IN_DEBUG_AND_RELEASE
                 DEBUG_RENDERER_IN_DISTRIBUTION PROFILER_IN_DEBUG_AND_RELEASE PROFILER_IN_DISTRIBUTION
                 ENABLE_OBJECT_STREAM ENABLE_INSTALL JPH_BUILD_SHARED_LIBS
                 JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
                 USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD)
        set(${flag} OFF)
    endforeach()
    # Keep the dependency's public ABI compatible with the exception/RTTI-enabled wrapper.
    set(CPP_EXCEPTIONS_ENABLED ON)
    set(CPP_RTTI_ENABLED ON)
    set(DOUBLE_PRECISION OFF)
    if(MSVC AND (ANIMA_ENABLE_SANITIZERS OR DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT))
        # CMake supplies the selected debug format. Jolt's extra /Zi would
        # override Embedded symbols and prevent safe compiler caching.
        set(GENERATE_DEBUG_SYMBOLS OFF)
    endif()
    FetchContent_Declare(jolt
        URL https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/e77f175595e64cb44218cc9d9d56fc365ad0e36a
        URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
        SOURCE_SUBDIR Build
        SYSTEM)
    FetchContent_MakeAvailable(jolt)
    anima_enable_sanitizers(Jolt)
    set(anima_jolt_source "${jolt_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
anima_add_jolt()
add_library(anima_physics STATIC src/physics/world.cpp)
add_library(anima::physics ALIAS anima_physics)
target_include_directories(anima_physics PUBLIC include)
target_link_libraries(anima_physics PUBLIC anima::core PRIVATE Jolt)
anima_target_defaults(anima_physics)
if(ANIMA_BUILD_ASSETS)
    add_library(anima_physics_scene STATIC src/physics/scene.cpp)
    add_library(anima::physics_scene ALIAS anima_physics_scene)
    target_include_directories(anima_physics_scene PUBLIC include)
    target_include_directories(anima_physics_scene SYSTEM PRIVATE third_party)
    target_link_libraries(anima_physics_scene PUBLIC anima::physics anima::assets)
    anima_target_defaults(anima_physics_scene)
endif()
if(ANIMA_BUILD_TESTS)
    add_executable(anima_physics_tests tests/physics_tests.cpp)
    target_link_libraries(anima_physics_tests PRIVATE anima::physics anima_test_main)
    anima_target_defaults(anima_physics_tests)
    add_test(NAME physics_world COMMAND anima_physics_tests)
    if(ANIMA_BUILD_ASSETS)
        add_executable(anima_physics_scene_tests tests/physics_scene_tests.cpp)
        target_link_libraries(anima_physics_scene_tests PRIVATE anima::physics_scene)
        anima_target_defaults(anima_physics_scene_tests)
        add_test(NAME physics_scene COMMAND anima_physics_scene_tests)
    endif()
endif()

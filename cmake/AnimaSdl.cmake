include_guard(GLOBAL)

# SDL for the desktop library. A build that fetches SDL also includes this file for the
# input event converter, which uses only the fetched headers.
if(ANIMA_FETCH_SDL)
    include(FetchContent)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG fa2c02bb6e21974a89ea9824bc53c9932abe5f9c # release-3.4.16
        GIT_PROGRESS TRUE
        SYSTEM)
    FetchContent_MakeAvailable(SDL3)
    foreach(target SDL3-static SDL3-shared)
        if(TARGET ${target})
            anima_enable_sanitizers(${target})
        endif()
    endforeach()
    set(anima_sdl_source "${sdl3_SOURCE_DIR}")
else()
    # Consumer runtime-copy helpers must also see the imported SDL targets.
    find_package(SDL3 3.2 REQUIRED CONFIG GLOBAL)
endif()

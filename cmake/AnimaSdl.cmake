include_guard(GLOBAL)

# Shared runtime discovery for independently selected desktop and audio output.
# The input event converter uses headers only and never includes this file.
if(ANIMA_FETCH_SDL)
    include(FetchContent)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 683181b47cfabd293e3ea409f838915b8297a4fd # release-3.4.2
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(anima_sdl_source "${sdl3_SOURCE_DIR}")
else()
    # Consumer runtime-copy helpers must also see the imported SDL targets.
    find_package(SDL3 3.2 REQUIRED CONFIG GLOBAL)
endif()

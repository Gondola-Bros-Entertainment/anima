include_guard(GLOBAL)

# Call after linking an executable to anima::desktop or anima::audio_output. This
# handles SDL's shared runtime on Windows; drivers and other dependencies are external.
function(anima_copy_sdl_runtime target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "anima_copy_sdl_runtime requires an existing executable: ${target}")
    endif()
    get_target_property(target_type "${target}" TYPE)
    if(NOT target_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "anima_copy_sdl_runtime expects an executable: ${target}")
    endif()
    get_target_property(already_added "${target}" ANIMA_SDL_RUNTIME_COPY)
    if(already_added)
        return()
    endif()
    if(WIN32 AND TARGET SDL3::SDL3)
        get_target_property(sdl_type SDL3::SDL3 TYPE)
        if(sdl_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET "${target}" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endif()
    set_target_properties("${target}" PROPERTIES ANIMA_SDL_RUNTIME_COPY TRUE)
endfunction()

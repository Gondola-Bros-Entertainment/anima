include_guard(GLOBAL)

option(ANIMA_ENABLE_SANITIZERS "Instrument Anima and source dependencies with ASan (and UBSan on Linux/macOS)" OFF)

function(anima_check_msvc_sanitizer_configuration)
    set(configurations "${CMAKE_BUILD_TYPE}")
    if(CMAKE_CONFIGURATION_TYPES)
        set(configurations "${CMAKE_CONFIGURATION_TYPES}")
    endif()
    if(NOT configurations)
        message(FATAL_ERROR "MSVC ASan requires an explicit build configuration; use -DCMAKE_BUILD_TYPE=RelWithDebInfo")
    endif()
    foreach(configuration IN LISTS configurations)
        string(TOUPPER "${configuration}" configuration_upper)
        if(configuration_upper STREQUAL "DEBUG")
            message(FATAL_ERROR
                "MSVC ASan does not support the default Debug /RTC checks. Use "
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo and, for Visual Studio or other "
                "multi-configuration generators, -DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo.")
        endif()
        foreach(language C CXX)
            set(flags "${CMAKE_${language}_FLAGS} ${CMAKE_${language}_FLAGS_${configuration_upper}}")
            if(flags MATCHES "(^|[ \t])[-/]RTC[^ \t]*([ \t]|$)" OR
               flags MATCHES "(^|[ \t])[-/]ZI([ \t]|$)")
                message(FATAL_ERROR "MSVC ASan is incompatible with /RTC and /ZI in ${language} ${configuration} flags; use Embedded debug information and disable runtime checks")
            endif()
        endforeach()
    endforeach()
endfunction()

function(anima_check_sanitizer_support)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        anima_check_msvc_sanitizer_configuration()
    elseif(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" OR
           NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "ANIMA_ENABLE_SANITIZERS requires native MSVC on Windows, or GCC/Clang on Linux/macOS")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    if(MSVC)
        set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
        if(CMAKE_CONFIGURATION_TYPES)
            list(GET CMAKE_CONFIGURATION_TYPES 0 CMAKE_TRY_COMPILE_CONFIGURATION)
        endif()
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
        set(CMAKE_REQUIRED_FLAGS "/fsanitize=address")
        set(CMAKE_REQUIRED_LINK_OPTIONS /INCREMENTAL:NO /DEBUG)
    else()
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
        set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
    endif()
    check_cxx_source_compiles("int main() { return 0; }" ANIMA_SANITIZERS_SUPPORTED)
    cmake_pop_check_state()
    if(NOT ANIMA_SANITIZERS_SUPPORTED)
        message(FATAL_ERROR "The selected compiler cannot compile and link the requested sanitizers; install its ASan runtime (and UBSan on Linux/macOS)")
    endif()
endfunction()

if(ANIMA_ENABLE_SANITIZERS)
    anima_check_sanitizer_support()
endif()

function(anima_enable_sanitizers target)
    if(NOT ANIMA_ENABLE_SANITIZERS)
        return()
    endif()
    if(MSVC)
        if(CMAKE_C_COMPILER_LOADED AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            message(FATAL_ERROR "Windows ASan also requires native MSVC for C dependencies")
        endif()
        anima_check_msvc_sanitizer_configuration()
        # Embedded symbols retain full debug information without a shared
        # compiler PDB, so each instrumented object can be cached independently.
        set_property(TARGET ${target} PROPERTY MSVC_DEBUG_INFORMATION_FORMAT Embedded)
        # SDL can include assembly sources; instrument only C and C++ units.
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>")
        # MSVC embeds ASan runtime-library directives in instrumented objects.
        # Its runtime is incompatible with incremental linking of final binaries.
        target_link_options(${target} PUBLIC /INCREMENTAL:NO /DEBUG)
        return()
    endif()
    if(CMAKE_C_COMPILER_LOADED AND NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "ANIMA_ENABLE_SANITIZERS also requires a GCC or Clang C compiler for C dependencies")
    endif()
    # Keep instrumentation private: opting into Anima's checks must not change
    # unrelated application compilation or third-party ABI definitions.
    target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    # Static libraries need the final executable to link the sanitizer runtimes.
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
endfunction()

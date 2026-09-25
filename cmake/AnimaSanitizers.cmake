include_guard(GLOBAL)

option(ANIMA_ENABLE_SANITIZERS "Instrument Anima and source dependencies with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(ANIMA_ENABLE_SANITIZERS)
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "ANIMA_ENABLE_SANITIZERS requires GCC or Clang on Linux/macOS")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
    check_cxx_source_compiles("int main() { return 0; }" ANIMA_SANITIZERS_SUPPORTED)
    cmake_pop_check_state()
    if(NOT ANIMA_SANITIZERS_SUPPORTED)
        message(FATAL_ERROR "The selected compiler cannot compile and link ASan/UBSan; install its sanitizer runtimes")
    endif()
endif()

function(anima_enable_sanitizers target)
    if(NOT ANIMA_ENABLE_SANITIZERS)
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

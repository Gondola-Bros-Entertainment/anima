# Linea is configured only for explicitly enabled vector art targets. anima::vector exposes it, since applications
# describe their art as Linea drawings; see third_party/linea. Anima follows Linea's main branch while the two are
# developed together. FETCHCONTENT_SOURCE_DIR_LINEA builds against a local checkout instead.
include(FetchContent)
function(anima_add_linea)
    set(BUILD_SHARED_LIBS OFF)
    set(LINEA_BUILD_TESTS OFF)
    set(LINEA_WARNINGS_AS_ERRORS OFF)
    FetchContent_Declare(linea
        GIT_REPOSITORY https://github.com/Gondola-Bros-Entertainment/linea.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(linea)
    anima_enable_sanitizers(linea)
    set(anima_linea_source "${linea_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
anima_add_linea()

add_library(anima_vector STATIC src/vector/atlas.cpp)
add_library(anima::vector ALIAS anima_vector)
target_include_directories(anima_vector PUBLIC include)
target_link_libraries(anima_vector PUBLIC anima::assets linea::linea)
anima_target_defaults(anima_vector)
if(ANIMA_BUILD_UI)
    add_library(anima_vector_ui STATIC src/vector/ui.cpp)
    add_library(anima::vector_ui ALIAS anima_vector_ui)
    target_include_directories(anima_vector_ui PUBLIC include)
    target_link_libraries(anima_vector_ui PUBLIC anima::vector anima::ui)
    anima_target_defaults(anima_vector_ui)
endif()

if(ANIMA_BUILD_TESTS)
    add_executable(anima_vector_tests tests/vector_tests.cpp)
    target_link_libraries(anima_vector_tests PRIVATE anima::vector anima_test_main)
    anima_target_defaults(anima_vector_tests)
    add_test(NAME vector_atlas COMMAND anima_vector_tests)
endif()

# Linea is configured only for explicitly enabled vector art targets. anima::vector exposes it, since applications
# describe their art as Linea drawings; see third_party/linea.
include(FetchContent)
function(anima_add_linea)
    set(BUILD_SHARED_LIBS OFF)
    set(LINEA_BUILD_TESTS OFF)
    set(LINEA_WARNINGS_AS_ERRORS OFF)
    FetchContent_Declare(linea
        URL https://codeload.github.com/Gondola-Bros-Entertainment/linea/tar.gz/16648e644c4864be81811ee765455a3397448ae1
        URL_HASH SHA256=f4655c2d15139f41ff030ff82e60d1b13472bb88527e8bb07b6861074ad0dffe
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

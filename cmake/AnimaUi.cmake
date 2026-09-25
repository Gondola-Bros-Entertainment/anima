# RmlUi/FreeType are configured only for explicitly enabled UI/document targets.
include(FetchContent)
function(anima_add_rmlui)
    set(BUILD_SHARED_LIBS OFF)
    set(RMLUI_SAMPLES OFF)
    set(RMLUI_TESTS OFF)
    set(RMLUI_SHELL OFF)
    set(RMLUI_LUA_BINDINGS OFF)
    set(RMLUI_SVG_PLUGIN OFF)
    set(RMLUI_LOTTIE_PLUGIN OFF)
    set(RMLUI_FONT_ENGINE freetype)
    set(RMLUI_THIRDPARTY_CONTAINERS OFF)
    set(RMLUI_MATRIX_ROW_MAJOR OFF)
    set(RMLUI_PRECOMPILED_HEADERS OFF)
    FetchContent_Declare(rmlui
        URL https://codeload.github.com/mikke89/RmlUi/tar.gz/ba95ffe8bfb6370efb2cdcca927eaad4710c5413
        URL_HASH SHA256=1541ef5577115e9368f8ed389b29f0925ef6572f326a33d378ea16c3cfa2cde8
        PATCH_COMMAND "${CMAKE_COMMAND}" "-DRMLUI_SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/RmlUiBoolString.cmake"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(rmlui)
    anima_enable_sanitizers(rmlui_core)
    set_target_properties(rmlui_debugger PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set(anima_rmlui_source "${rmlui_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
anima_add_rmlui()

add_library(anima_ui_documents STATIC src/ui/document.cpp)
add_library(anima::ui_documents ALIAS anima_ui_documents)
target_include_directories(anima_ui_documents PUBLIC include)
target_link_libraries(anima_ui_documents PRIVATE RmlUi::Core)
anima_target_defaults(anima_ui_documents)
if(ANIMA_BUILD_ASSETS)
    add_library(anima_ui_scene STATIC src/ui/scene.cpp)
    add_library(anima::ui_scene ALIAS anima_ui_scene)
    target_include_directories(anima_ui_scene PUBLIC include)
    target_include_directories(anima_ui_scene SYSTEM PRIVATE third_party)
    target_link_libraries(anima_ui_scene PUBLIC anima::ui_documents anima::assets)
    anima_target_defaults(anima_ui_scene)
endif()

if(ANIMA_BUILD_UI)
    # Upstream SDL key translation, cursor/clipboard and composition helpers retain
    # upstream licenses; no sample window or standalone renderer backend is included.
    add_library(anima_rmlui_sdl STATIC "${anima_rmlui_source}/Backends/RmlUi_Platform_SDL.cpp")
    anima_enable_sanitizers(anima_rmlui_sdl)
    target_include_directories(anima_rmlui_sdl SYSTEM PUBLIC "${anima_rmlui_source}/Backends")
    target_compile_definitions(anima_rmlui_sdl PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
    target_link_libraries(anima_rmlui_sdl PUBLIC RmlUi::Core SDL3::SDL3)

    add_library(anima_ui STATIC src/ui/context.cpp)
    add_library(anima::ui ALIAS anima_ui)
    target_include_directories(anima_ui PUBLIC include PRIVATE src/desktop)
    target_link_libraries(anima_ui PUBLIC anima::desktop anima::ui_documents RmlUi::Core PRIVATE anima_rmlui_sdl anima_image_dependencies)
    anima_target_defaults(anima_ui)

endif()

if(ANIMA_BUILD_TESTS)
    add_executable(anima_ui_document_tests tests/ui_document_tests.cpp)
    if(ANIMA_BUILD_ASSETS)
        target_link_libraries(anima_ui_document_tests PRIVATE anima::ui_scene RmlUi::Core)
        target_compile_definitions(anima_ui_document_tests PRIVATE TEST_UI_SCENE=1)
    else()
        target_link_libraries(anima_ui_document_tests PRIVATE anima::ui_documents RmlUi::Core)
    endif()
    anima_target_defaults(anima_ui_document_tests)
    add_test(NAME ui_documents COMMAND anima_ui_document_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/assets/controls.rml")
endif()

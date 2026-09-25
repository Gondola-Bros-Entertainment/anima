cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AnimaDocumentation LANGUAGES NONE)
endif()

# Also usable as a standalone documentation project: no engine dependencies,
# graphics SDK, downloads or compiler are needed to generate the reference.
find_package(Doxygen 1.10 REQUIRED)
get_filename_component(ANIMA_DOCUMENTATION_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ANIMA_DOCUMENTATION_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/reference")
configure_file(Doxyfile.in Doxyfile @ONLY)
add_custom_target(anima_docs
    COMMAND Doxygen::doxygen "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${ANIMA_DOCUMENTATION_SOURCE}"
    COMMENT "Generating Anima public C++ reference"
    VERBATIM)

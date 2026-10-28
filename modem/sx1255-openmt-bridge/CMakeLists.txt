# Minimum required CMake version -- matches OpenMT's own component
# CMakeLists.txt files (modem/, protocols/m17/, etc.) exactly, for
# consistency with that project's build system, which is what this
# bridge was built to plug into as a drop-in modem.

cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find ALSA -- the only external library dependency
# this project has (OpenMT's own CMakeLists.txt files use the same
# PkgConfig + pkg_check_modules pattern for mariadb; this mirrors that
# convention with the actual library this project needs instead).
find_package(PkgConfig REQUIRED)
pkg_check_modules(alsa REQUIRED IMPORTED_TARGET "alsa")
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(sx1255_bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# GNU extensions (usleep, strdup, etc.) are used throughout this
# project's sources -- equivalent to the Makefile's -std=gnu11.
add_compile_options(-Wall -Wextra)
add_compile_definitions(_GNU_SOURCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Define an executable target (sources added below)
add_executable(sx1255_bridge)

# Link ALSA plus the other system libraries this project needs:
# libm (math.h), libpthread (the playback thread + mutex),
# libutil (openpty())
target_link_libraries(sx1255_bridge PUBLIC PkgConfig::alsa PkgConfig::mariadb)
target_link_libraries(sx1255_bridge PRIVATE m pthread util)

# Link the source files -- one flat list, matching this project's own
# directory layout (hw/, dsp/, audio/, app/)
target_sources(sx1255_bridge PRIVATE
    hw/sx1255.c
    hw/gpio_line.c
    dsp/dsp.c
    audio/alsa_io.c
    app/bridge.c
    app/bridge_db.c
    ../../tools/bridge_common.c
    ../../tools/tools.c
    ../../tools/string_builder.c
    ../../tools/RingBuffer.c
)

# Specify the directories where header files are located
target_include_directories(sx1255_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hw
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools
)

# Change the output directory for the executable -- same pattern
# OpenMT's own CMakeLists.txt files use, so the built binary lands in
# the build directory rather than scattered under source directories.
set_target_properties(sx1255_bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

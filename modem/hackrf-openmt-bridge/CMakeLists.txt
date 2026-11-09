# hackrf-openmt-bridge
#
# Build:   cmake -B build && cmake --build build
# Needs:   libhackrf (libhackrf-dev), pkg-config, a C11 compiler.
#
# For testing without a HackRF, build against the built-in software HackRF:
#          cmake -B build-sim -DHRF_SIM=ON && cmake --build build-sim
cmake_minimum_required(VERSION 3.12)
project(hackrf-openmt-bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)
add_compile_definitions(_GNU_SOURCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(HRF_SIM "Use the software HackRF (hw/hrf_sim.c) instead of libhackrf" OFF)

# The settings (frequencies, gains, invert flags) are read from the OpenMT config database.  Turn this off
# only for the simulator tests on a machine without MariaDB: the command line values then stay.
option(HRF_DB "Read the settings from the OpenMT config database" ON)

add_executable(bridge
    app/bridge.c
    app/hrf_db.c
    ../../tools/rf_spectrum.c
    dsp/dsp.c
    dsp/hrf_frontend.c
)
target_include_directories(bridge PRIVATE hw dsp app ../../tools)
target_link_libraries(bridge PRIVATE m pthread util)

if(HRF_DB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")
    target_sources(bridge PRIVATE
        ../../tools/bridge_common.c
        ../../tools/tools.c
        ../../tools/string_builder.c
        ../../tools/RingBuffer.c)
    target_link_libraries(bridge PRIVATE PkgConfig::mariadb)
else()
    target_compile_definitions(bridge PRIVATE HRF_NO_DB)
endif()

# 64-bit atomics (the bridge uses them) are library calls on 32-bit ARM such as the Pi 2: libatomic.
include(CheckLibraryExists)
check_library_exists(atomic __atomic_load_8 "" HRF_HAVE_LIBATOMIC)
if(HRF_HAVE_LIBATOMIC)
    target_link_libraries(bridge PRIVATE atomic)
endif()

if(HRF_SIM)
    target_sources(bridge PRIVATE hw/hrf_sim.c)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HACKRF REQUIRED IMPORTED_TARGET libhackrf)
    target_sources(bridge PRIVATE hw/hrf_dev.c)
    target_link_libraries(bridge PRIVATE PkgConfig::HACKRF)
endif()

# Unit test of the sample-rate front end (no hardware needed).
add_executable(test_frontend test/test_frontend.c dsp/hrf_frontend.c)
target_include_directories(test_frontend PRIVATE dsp)
target_link_libraries(test_frontend PRIVATE m)

# Unit test of the RF spectrum server (UDP on localhost, no hardware needed).
add_executable(test_spectrum test/test_spectrum.c ../../tools/rf_spectrum.c)
target_include_directories(test_spectrum PRIVATE ../../tools)
target_link_libraries(test_spectrum PRIVATE m)

add_executable(test_iq test/test_iq.c ../../tools/rf_spectrum.c)
target_include_directories(test_iq PRIVATE ../../tools)
target_link_libraries(test_iq PRIVATE m)

set_target_properties(bridge test_frontend test_spectrum test_iq PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

enable_testing()
add_test(NAME frontend COMMAND test_frontend)
add_test(NAME spectrum COMMAND test_spectrum)
add_test(NAME iq COMMAND test_iq)

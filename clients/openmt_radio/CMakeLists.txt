cmake_minimum_required(VERSION 3.12)
project(openmt_radio C CXX)

# OpenMT Radio: a software radio client for Modem_Host.  The radio core (Modem_Host link, vocoder, NXDN, ...)
# is plain C++ and builds without Qt; the program with the window needs Qt 5.15 or Qt 6 (Widgets, Multimedia).

option(BUILD_GUI   "Build the Qt program (needs Qt Widgets and Multimedia)" ON)
option(BUILD_TESTS "Build the core tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OPENMT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
find_package(Threads REQUIRED)

# The repo's own protocol code is used as it is: one copy of each protocol, fixes carry over.
add_library(openmt_repo_c STATIC
    ${OPENMT_ROOT}/protocols/NXDN/nxdn_func.c
    ${OPENMT_ROOT}/protocols/NXDN/nxdn_dsp.c
    ${OPENMT_ROOT}/protocols/NXDN/ambe3000.c
    ${OPENMT_ROOT}/protocols/DMR/dmr_func.c
    ${OPENMT_ROOT}/protocols/P25/p25_func.c
    ${OPENMT_ROOT}/protocols/YSF/ysf_func.c
    ${OPENMT_ROOT}/protocols/YSF/ysf_dsp.c
    core/dstar_codec.c
    ${OPENMT_ROOT}/protocols/FM/fm_dsp.c
    ${OPENMT_ROOT}/tools/dsp_tools.c
    ${OPENMT_ROOT}/tools/fec_tools.c
)
target_include_directories(openmt_repo_c PUBLIC
    ${OPENMT_ROOT}/protocols/NXDN
    ${OPENMT_ROOT}/protocols/DMR
    ${OPENMT_ROOT}/protocols/P25
    ${OPENMT_ROOT}/protocols/YSF
    ${OPENMT_ROOT}/protocols/DSTAR
    ${OPENMT_ROOT}/protocols/FM
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${OPENMT_ROOT}/tools
)
target_link_libraries(openmt_repo_c PUBLIC m)

file(GLOB IMBE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imbe_vocoder/*.cc)
add_library(openmt_radio_core STATIC
    ${IMBE_SOURCES}
    core/modem_link.cpp
    core/null_vocoder.cpp
    core/ambe_vocoder.cpp
    core/resampler.cpp
    core/nxdn_mode.cpp
    core/dmr_mode.cpp
    core/p25_mode.cpp
    core/ysf_mode.cpp
    core/dstar_mode.cpp
    core/m17_mode.cpp
    core/fm_mode.cpp
    core/codec2_codec.cpp
    core/rf_monitor.cpp
    core/iq_monitor.cpp
    core/rf_spectrum.cpp
    ${OPENMT_ROOT}/protocols/m17/M17/M17Callsign.cpp
    ${OPENMT_ROOT}/protocols/m17/M17/M17FrameDecoder.cpp
    ${OPENMT_ROOT}/protocols/m17/M17/M17FrameEncoder.cpp
    ${OPENMT_ROOT}/protocols/m17/M17/M17LinkSetupFrame.cpp
    ${OPENMT_ROOT}/protocols/m17/M17/M17Golay.cpp
    core/radio_engine.cpp
)
target_include_directories(openmt_radio_core PUBLIC core third_party/imbe_vocoder ${OPENMT_ROOT}/protocols/m17 ${OPENMT_ROOT}/tools)
target_link_libraries(openmt_radio_core PUBLIC openmt_repo_c Threads::Threads ${CMAKE_DL_LIBS})

if (BUILD_TESTS)
    add_executable(spectrum_client_test tests/spectrum_client_test.cpp)
    target_link_libraries(spectrum_client_test PRIVATE openmt_radio_core)
    add_executable(monitor_test tests/monitor_test.cpp)
    target_link_libraries(monitor_test PRIVATE openmt_radio_core)
    add_executable(iq_monitor_test tests/iq_monitor_test.cpp)
    target_link_libraries(iq_monitor_test PRIVATE openmt_radio_core)
    add_executable(core_test tests/core_test.cpp)
    target_link_libraries(core_test PRIVATE openmt_radio_core)
    add_executable(dmr_interop_tool tests/dmr_interop_tool.cpp ${OPENMT_ROOT}/tools/test/sim_common.c)
    target_include_directories(dmr_interop_tool PRIVATE ${OPENMT_ROOT}/tools/test)
    target_link_libraries(dmr_interop_tool PRIVATE openmt_radio_core)
    add_executable(p25_interop_tool tests/p25_interop_tool.cpp)
    target_link_libraries(p25_interop_tool PRIVATE openmt_radio_core)
    add_executable(dstar_interop_tool tests/dstar_interop_tool.cpp)
    target_link_libraries(dstar_interop_tool PRIVATE openmt_radio_core)
    add_executable(m17_interop_tool tests/m17_interop_tool.cpp)
    target_link_libraries(m17_interop_tool PRIVATE openmt_radio_core)
    add_executable(fm_interop_tool tests/fm_interop_tool.cpp)
    target_link_libraries(fm_interop_tool PRIVATE openmt_radio_core)
    add_executable(ysf_interop_tool tests/ysf_interop_tool.cpp)
    target_link_libraries(ysf_interop_tool PRIVATE openmt_radio_core)
    enable_testing()
    add_test(NAME core_test COMMAND core_test)
    add_test(NAME monitor_test COMMAND monitor_test)
    add_test(NAME iq_monitor_test COMMAND iq_monitor_test)
    add_test(NAME spectrum_client_test COMMAND spectrum_client_test)
endif()

if (BUILD_GUI)
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets Multimedia QUIET)
    if (QT_FOUND)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia)
        set(CMAKE_AUTOMOC ON)
        add_executable(openmt_radio
            app/main.cpp
            app/mainwindow.cpp
            app/mainwindow.h
            app/audioio.cpp
            app/audioio.h
            app/radiocontroller.cpp
            app/radiocontroller.h
            app/monitorwidget.cpp
            app/monitorwidget.h
        )
        target_link_libraries(openmt_radio PRIVATE openmt_radio_core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Multimedia)
        if (QT_VERSION_MAJOR EQUAL 6)
            set_target_properties(openmt_radio PROPERTIES CXX_STANDARD 17)
        endif()
        install(TARGETS openmt_radio RUNTIME DESTINATION bin)
    else()
        message(STATUS "Qt Widgets and Multimedia not found: the radio core and its tests are built, the program is not")
    endif()
endif()

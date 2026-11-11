cmake_minimum_required(VERSION 3.12)
project(openmt_radio C CXX)

# OpenMT Radio: a software radio client for Modem_Host.  The radio core (Modem_Host link, vocoder, NXDN, ...)
# is plain C++ and builds without Qt; the program with the window needs Qt 5.15 or Qt 6 (Widgets, Multimedia).

option(BUILD_GUI   "Build the Qt program (needs Qt Widgets and Multimedia)" ON)
option(BUILD_TESTS "Build the core tests" ON)

# The monitor analyses signals on the window's timer: without optimisation (cmake with no build type) that is several times
# slower and the window can lag.  Optimised unless a build type is given (-DCMAKE_BUILD_TYPE=Debug for debugging).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

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
    core/remote_json.cpp
    core/remote_settings.cpp
)
target_include_directories(openmt_radio_core PUBLIC core third_party/imbe_vocoder ${OPENMT_ROOT}/protocols/m17 ${OPENMT_ROOT}/tools)
target_link_libraries(openmt_radio_core PUBLIC openmt_repo_c Threads::Threads ${CMAKE_DL_LIBS})

# The remote control server (WebSocket over TLS) needs OpenSSL.  Without it the program is built without the remote control.
option(BUILD_REMOTE "Build the remote control server (needs OpenSSL)" ON)
set(OPENMT_HAVE_REMOTE OFF)
if (BUILD_REMOTE)
    find_package(OpenSSL QUIET)
    if (OPENSSL_FOUND)
        # The web client (web/index.html) is built into the program as a C array, so the radio can serve it by itself
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html OPENMT_WEB_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," OPENMT_WEB_HEX "${OPENMT_WEB_HEX}")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/web_client_data.cpp
            "#include \"web_client.h\"\nnamespace openmt\n{\nstatic const unsigned char kPage[] = {${OPENMT_WEB_HEX}0x00};\n"
            "const std::string& webClientPage()\n{\n    static const std::string page((const char*)kPage, sizeof(kPage) - 1U);\n    return page;\n}\n}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
        target_sources(openmt_radio_core PRIVATE core/remote_server.cpp core/remote_control.cpp ${CMAKE_CURRENT_BINARY_DIR}/web_client_data.cpp)
        target_link_libraries(openmt_radio_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(openmt_radio_core PUBLIC OPENMT_REMOTE=1)
        set(OPENMT_HAVE_REMOTE ON)
    else()
        message(STATUS "OpenSSL not found: OpenMT Radio is built without the remote control (install libssl-dev to get it)")
    endif()
endif()

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
    add_executable(remote_json_test tests/remote_json_test.cpp)
    target_link_libraries(remote_json_test PRIVATE openmt_radio_core)
    enable_testing()
    add_test(NAME remote_json_test COMMAND remote_json_test)
    if (OPENMT_HAVE_REMOTE)
        add_executable(remote_test_server tests/remote_test_server.cpp)
        target_link_libraries(remote_test_server PRIVATE openmt_radio_core)
        add_executable(remote_control_server tests/remote_control_server.cpp)
        target_link_libraries(remote_control_server PRIVATE openmt_radio_core)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if (Python3_FOUND)
            add_test(NAME remote_protocol COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote_test.py $<TARGET_FILE:remote_test_server>)
            add_test(NAME remote_control COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/remote_control_test.py $<TARGET_FILE:remote_control_server>)
            add_test(NAME android_core COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/android_core_test.py $<TARGET_FILE:remote_control_server>)
            add_test(NAME web_client COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/web_client_test.py $<TARGET_FILE:remote_control_server>)
        endif()
    endif()
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
            app/remotebridge.cpp
            app/remotebridge.h
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

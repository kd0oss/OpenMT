cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(fm_gateway C)

# The GSM 06.10 codec of EchoLink: libgsm (Debian/Raspberry Pi OS: sudo apt install libgsm1-dev)
find_library(GSM_LIBRARY NAMES gsm REQUIRED)
find_path(GSM_INCLUDE_DIR NAMES gsm.h PATH_SUFFIXES gsm REQUIRED)

add_executable(fm_gateway)

target_link_libraries(fm_gateway PUBLIC PkgConfig::mariadb)
target_link_libraries(fm_gateway PRIVATE ${GSM_LIBRARY} m pthread)

target_sources(fm_gateway PRIVATE
    ./fm_gateway.c
    ./elink.c
    ../../../tools/string_builder.c
    ../../../tools/tools.c
)

target_include_directories(fm_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GSM_INCLUDE_DIR}
    ../../../tools
)

set_target_properties(fm_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

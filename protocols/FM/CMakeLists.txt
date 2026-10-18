cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(fm_host C)

add_executable(fm_host)

target_link_libraries(fm_host PUBLIC PkgConfig::mariadb)
target_link_libraries(fm_host PRIVATE m pthread)

target_sources(fm_host PRIVATE
    ./fm_host.c
    ./fm_dsp.c
    ../../tools/string_builder.c
    ../../tools/tools.c
)

target_include_directories(fm_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

set_target_properties(fm_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

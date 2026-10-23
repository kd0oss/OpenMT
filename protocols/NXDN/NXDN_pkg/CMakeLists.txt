cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(nxdn_host C)

add_executable(nxdn_host)

target_link_libraries(nxdn_host PUBLIC PkgConfig::mariadb)
target_link_libraries(nxdn_host PRIVATE m pthread)

target_sources(nxdn_host PRIVATE
    ./nxdn_host.c
    ./nxdn_func.c
    ./nxdn_dsp.c
    ../../tools/string_builder.c
    ../../tools/tools.c
    ../../tools/fec_tools.c
    ../../tools/dsp_tools.c
)

target_include_directories(nxdn_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

set_target_properties(nxdn_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

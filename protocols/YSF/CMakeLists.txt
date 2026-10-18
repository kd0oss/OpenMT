cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(ysf_host C)

add_executable(ysf_host)

target_link_libraries(ysf_host PUBLIC PkgConfig::mariadb)
target_link_libraries(ysf_host PRIVATE m pthread)

target_sources(ysf_host PRIVATE
    ./ysf_host.c
    ./ysf_func.c
    ./ysf_dsp.c
    ../../tools/string_builder.c
    ../../tools/tools.c
    ../../tools/dsp_tools.c
    ../../tools/ADF7021.c
)

target_include_directories(ysf_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

set_target_properties(ysf_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(ysf_gateway C)

add_executable(ysf_gateway)

target_link_libraries(ysf_gateway PUBLIC PkgConfig::mariadb)
target_link_libraries(ysf_gateway PRIVATE m pthread)

target_sources(ysf_gateway PRIVATE
    ./ysf_gateway.c
    ./ysf_reflist.c
    ./ysf_wiresx.c
    ../ysf_func.c
    ../../../tools/fec_tools.c
    ../../../tools/string_builder.c
    ../../../tools/tools.c
)

target_include_directories(ysf_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../tools
)

set_target_properties(ysf_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

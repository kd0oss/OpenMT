cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(nxdn_gateway C)

add_executable(nxdn_gateway)

target_link_libraries(nxdn_gateway PUBLIC PkgConfig::mariadb)
target_link_libraries(nxdn_gateway PRIVATE m pthread)

target_sources(nxdn_gateway PRIVATE
    ./nxdn_gateway.c
    ../../../tools/string_builder.c
    ../../../tools/tools.c
)

target_include_directories(nxdn_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../tools
)

set_target_properties(nxdn_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

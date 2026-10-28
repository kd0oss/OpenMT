cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(openmths_bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)
add_compile_definitions(_GNU_SOURCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(openmths_bridge)

target_link_libraries(openmths_bridge PUBLIC PkgConfig::mariadb)
target_link_libraries(openmths_bridge PRIVATE util pthread)

target_sources(openmths_bridge PRIVATE
    openmths_bridge.c
    ../../tools/bridge_common.c
    ../../tools/string_builder.c
    ../../tools/tools.c
    ../../tools/RingBuffer.c
)

target_include_directories(openmths_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

set_target_properties(openmths_bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

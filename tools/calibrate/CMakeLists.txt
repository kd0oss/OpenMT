cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(openmt_cal C)

add_executable(openmt_cal)

target_link_libraries(openmt_cal PUBLIC PkgConfig::mariadb)
target_link_libraries(openmt_cal PRIVATE m pthread)

target_sources(openmt_cal PRIVATE
    ./openmt_cal.c
    ../string_builder.c
    ../tools.c
)

target_include_directories(openmt_cal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ..
)

set_target_properties(openmt_cal PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

project(service_manager C)

add_executable(service_manager)

target_link_libraries(service_manager PUBLIC PkgConfig::mariadb)
target_link_libraries(service_manager PRIVATE m pthread)

target_sources(service_manager PRIVATE
    ./service_manager.c
    ../string_builder.c
    ../tools.c
)

target_include_directories(service_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ..
)

set_target_properties(service_manager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

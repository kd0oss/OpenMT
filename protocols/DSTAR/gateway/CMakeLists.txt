# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(DSTAR_Gateway C)

# Define an executable target (sources will be added later)
add_executable(DSTAR_Gateway)

# Link the MariaDB library to your executable
target_link_libraries(DSTAR_Gateway PUBLIC PkgConfig::mariadb)

# Link the source files from the src/ directory to the target
target_sources(DSTAR_Gateway PRIVATE
    ./dstargateway.c
    ../dstar_gps.c
    ../../../tools/tools.c
    ../../../tools/CCITTChecksumReverse.c
    ../../../tools/string_builder.c
    ../../../tools/RingBuffer.c
)

# Specify the directory where header files are located
target_include_directories(DSTAR_Gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../tools
)

# Optional: Change the output directory for the executable
set_target_properties(DSTAR_Gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

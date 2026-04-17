# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(dstar_gateway C)

# Define an executable target (sources will be added later)
add_executable(dstar_gateway)

# Link the MariaDB library to your executable
target_link_libraries(dstar_gateway PUBLIC PkgConfig::mariadb)

# Link the source files from the src/ directory to the target
target_sources(dstar_gateway PRIVATE
    ./dstargateway.c
    ../dstar_gps.c
    ../../../tools/tools.c
    ../../../tools/CCITTChecksumReverse.c
    ../../../tools/string_builder.c
    ../../../tools/RingBuffer.c
)

# Specify the directory where header files are located
target_include_directories(dstar_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../tools
)

# Optional: Change the output directory for the executable
set_target_properties(dstar_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

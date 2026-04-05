# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(Modem_Host C)

# Define an executable target (sources will be added later)
add_executable(Modem_Host)

# Link the MariaDB library to your executable
target_link_libraries(Modem_Host PUBLIC PkgConfig::mariadb)

# Link the source files from the src/ directory to the target
target_sources(Modem_Host PRIVATE
    ./Modem_Host.c
    ./mmdvm.c
    ../tools/string_builder.c
    ../tools/tools.c
    ../tools/RingBuffer.c
)

# Specify the directory where header files are located
target_include_directories(Modem_Host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../tools
)

# Optional: Change the output directory for the executable
set_target_properties(Modem_Host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(m17_gateway CXX C)

# Define an executable target (sources will be added later)
add_executable(m17_gateway)

# Link the MariaDB library to your executable
target_link_libraries(m17_gateway PUBLIC PkgConfig::mariadb)

# Link the source files from the src/ directory to the target
target_sources(m17_gateway PRIVATE
    ./m17gateway.cpp
    ../../../tools/tools.c
    ../../../tools/string_builder.c
    ../../../tools/RingBuffer.c
    ../M17/M17Callsign.cpp
    ../M17/M17DSP.cpp
    ../M17/M17FrameDecoder.cpp
    ../M17/M17FrameEncoder.cpp
    ../M17/M17LinkSetupFrame.cpp
    ../M17/M17Golay.cpp
)

# Specify the directory where header files are located
target_include_directories(m17_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../tools
    ..
)

# Optional: Change the output directory for the executable
set_target_properties(m17_gateway PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

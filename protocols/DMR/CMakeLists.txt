# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(dmr_host C)

#file(GLOB SOURCE_FILES "./DMR/*.cpp")

# Define an executable target (sources will be added later)
add_executable(dmr_host ${SOURCE_FILES})

# Link the MariaDB library to your executable
target_link_libraries(dmr_host PUBLIC PkgConfig::mariadb)
target_link_libraries(dmr_host PRIVATE VendorLib m)

# 1. Define the imported library target
# The type (STATIC or SHARED) should match the precompiled library type
add_library(VendorLib STATIC IMPORTED)

# 2. Set the properties of the imported library
set_target_properties(VendorLib PROPERTIES
    # Specify the location of the library file
    IMPORTED_LOCATION "/home/pi/OpenMT/dsp_lib/libCMSISDSP.a"
    # Specify the include directories for the library's headers
    INTERFACE_INCLUDE_DIRECTORIES "/home/pi/OpenMT/dsp_lib/Include"
)

# Link the source files from the src/ directory to the target
target_sources(dmr_host PRIVATE
    ./dmr_host.c
    ./dmr_func.c
    ../../tools/string_builder.c
    ../../tools/RingBuffer.c
    ../../tools/tools.c
    ../../tools/dsp_tools.c
    ../../tools/ADF7021.c
)

# Specify the directory where header files are located
target_include_directories(dmr_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

# Optional: Change the output directory for the executable
set_target_properties(dmr_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

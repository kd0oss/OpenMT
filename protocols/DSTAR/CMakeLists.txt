# Minimum required CMake version
cmake_minimum_required(VERSION 3.12)

# Use PkgConfig to find the mariadb client library
find_package(PkgConfig REQUIRED)
pkg_check_modules(mariadb REQUIRED IMPORTED_TARGET "mariadb")

# Project name and language
project(dstar_host C)

# Define an executable target (sources will be added later)
add_executable(dstar_host)

# Link the MariaDB library to your executable
target_link_libraries(dstar_host PUBLIC PkgConfig::mariadb)
target_link_libraries(dstar_host PRIVATE m)

# 1. Define the imported library target
# The type (STATIC or SHARED) should match the precompiled library type
#add_library(VendorLib) 

# 2. Set the properties of the imported library
#if(CMAKE_SIZEOF_VOID_P EQUAL 8)
#    message(STATUS "Configuring for 64-bit target architecture")
#    set_target_properties(VendorLib PROPERTIES
#        # Specify the location of the library file
#        IMPORTED_LOCATION "/home/pi/test/OpenMT/dsp_lib/lib_64/libCMSISDSP.a"
        # Specify the include directories for the library's headers
#        INTERFACE_INCLUDE_DIRECTORIES "/home/pi/OpenMT/dsp_lib/Include"
#    )
#else()
#        set_target_properties(VendorLib PROPERTIES
#        IMPORTED_LOCATION "/home/pi/test/OpenMT/dsp_lib/lib_32/libCMSISDSP.a"
        # Specify the include directories for the library's headers
#        INTERFACE_INCLUDE_DIRECTORIES "/home/pi/OpenMT/dsp_lib/Include"
#    )
#endif()

# Link the source files from the src/ directory to the target
target_sources(dstar_host PRIVATE
    ./dstar_host.c
    ./dstar_gps.c
    ../../tools/tools.c
    ../../tools/dsp_tools.c
    ../../tools/string_builder.c
    ../../tools/CCITTChecksumReverse.c
    ../../tools/ADF7021.c
    ../../tools/RingBuffer.c
)

# Specify the directory where header files are located
target_include_directories(dstar_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../tools
)

# Optional: Change the output directory for the executable
set_target_properties(dstar_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

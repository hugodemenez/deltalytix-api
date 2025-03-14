cmake_minimum_required(VERSION 3.10)
project(rapi_python)

# Enable verbose output during compilation
set(CMAKE_VERBOSE_MAKEFILE ON)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set basic compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -fno-lto")

# Add debug symbols in Release build
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Find Python with specific version
find_package(Python3 3.11 COMPONENTS Interpreter Development REQUIRED)

# Add pybind11
add_subdirectory(pybind11)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Add nlohmann/json
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Define Rithmic library paths - look in /usr/local/lib first, then builder/lib
set(RITHMIC_LIB_DIR "/usr/local/lib")
if(NOT EXISTS "${RITHMIC_LIB_DIR}/lib_api-optimize.a")
    set(RITHMIC_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/builder/lib")
endif()

set(RITHMIC_LIBS
    ${RITHMIC_LIB_DIR}/lib_api-optimize.a
    ${RITHMIC_LIB_DIR}/lib_apipoll-stubs-optimize.a
    ${RITHMIC_LIB_DIR}/lib_kit-optimize.a
    ${RITHMIC_LIB_DIR}/libRApiPlus-optimize.a
    ${RITHMIC_LIB_DIR}/libOmneStreamEngine-optimize.a
    ${RITHMIC_LIB_DIR}/libOmneEngine-optimize.a
    ${RITHMIC_LIB_DIR}/libOmneChannel-optimize.a
)

# Verify libraries exist
foreach(lib ${RITHMIC_LIBS})
    if(NOT EXISTS ${lib})
        message(FATAL_ERROR "Required library not found: ${lib}")
    endif()
endforeach()

# Include directories - look in /usr/local/include first, then builder/include
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include
    /usr/local/include
    ${CMAKE_CURRENT_SOURCE_DIR}/builder/include
    ${Python3_INCLUDE_DIRS}
    ${pybind11_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

# Link directories
link_directories(
    ${RITHMIC_LIB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp/lib
)

# Create the Python module
pybind11_add_module(rapi 
    cpp/src/rapi_wrapper.cpp
)

# Platform-specific libraries
set(PLATFORM_LIBS pthread dl z)
if(UNIX AND NOT APPLE)
    list(APPEND PLATFORM_LIBS rt)  # rt is only needed on Linux
endif()

# Link against the required libraries
target_link_libraries(rapi PRIVATE
    ${RITHMIC_LIBS}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${Python3_LIBRARIES}
    ${PLATFORM_LIBS}
    nlohmann_json::nlohmann_json
)

# Set RPATH for the target
set_target_properties(rapi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python
    PREFIX ""  # Remove 'lib' prefix on Unix systems
    SUFFIX ".so"  # Use .so extension on Unix systems
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${RITHMIC_LIB_DIR}"
)

# Print debug information
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "Python include dirs: ${Python3_INCLUDE_DIRS}")
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "Rithmic lib dir: ${RITHMIC_LIB_DIR}")
message(STATUS "Platform libs: ${PLATFORM_LIBS}")
message(STATUS "Output directory: ${CMAKE_CURRENT_SOURCE_DIR}/python") 
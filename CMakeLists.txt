cmake_minimum_required(VERSION 3.16)
project(http_server LANGUAGES CXX)

# -------- Build settings --------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if the user didn't specify a build type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Helpful warnings under GCC/Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -------- Sources --------
# IMPORTANT: only glob src/, never the repo root.
# The old monolithic server.cpp lives at the root for sentimental reasons
# and must not be compiled (it would cause duplicate-symbol link errors).
file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(http_server ${SERVER_SOURCES})

target_include_directories(http_server PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

# -------- SQLite --------
# Find the system-installed SQLite3 (libsqlite3-dev on Debian/Ubuntu).
find_package(SQLite3 REQUIRED)
target_link_libraries(http_server PRIVATE SQLite3::SQLite3)
# -------- Threads --------
find_package(Threads REQUIRED)
target_link_libraries(http_server PRIVATE Threads::Threads)

# -------- Install / output --------
set_target_properties(http_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "SQLite3 version:  ${SQLite3_VERSION}")

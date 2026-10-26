cmake_minimum_required(VERSION 3.16)
project(http_server LANGUAGES CXX)

include(CTest)

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
file(GLOB SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(http_server ${SERVER_SOURCES})

target_include_directories(http_server PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

# -------- SQLite --------
find_package(SQLite3 REQUIRED)
target_link_libraries(http_server PRIVATE SQLite3::SQLite3)
# -------- Threads --------
find_package(Threads REQUIRED)
target_link_libraries(http_server PRIVATE Threads::Threads)

# -------- Install / output --------
set_target_properties(http_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# -------- Tests --------
if(BUILD_TESTING)
    add_executable(http_parser_tests
        tests/http_parser_tests.cpp
        src/http_parser.cpp
    )
    target_include_directories(http_parser_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
    )
    add_test(NAME http_parser_tests COMMAND http_parser_tests)
    set_tests_properties(http_parser_tests PROPERTIES TIMEOUT 30)

    add_executable(http_resource_unit_tests
        tests/resource_tests.cpp
        src/thread_pool.cpp
    )
    target_include_directories(http_resource_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_link_libraries(http_resource_unit_tests PRIVATE Threads::Threads)
    add_test(NAME http_resource_unit_tests COMMAND http_resource_unit_tests)
    set_tests_properties(http_resource_unit_tests PROPERTIES TIMEOUT 30)

    add_executable(http_logger_unit_tests
        tests/logger_tests.cpp
        src/logger.cpp
    )
    target_include_directories(http_logger_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
    )
    target_compile_definitions(http_logger_unit_tests PRIVATE
        HTTP_SERVER_TESTING
    )
    target_link_libraries(http_logger_unit_tests PRIVATE
        SQLite3::SQLite3
        Threads::Threads
    )
    add_test(NAME http_logger_unit_tests COMMAND http_logger_unit_tests)
    set_tests_properties(http_logger_unit_tests PROPERTIES TIMEOUT 30)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME http_benchmark_parser_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/benchmark_parser_tests.py"
        )
        set_tests_properties(http_benchmark_parser_tests PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_protocol_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_protocol_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_protocol_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_resource_integration_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_resource_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_resource_integration_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_deadline_integration_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_deadline_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_deadline_integration_tests PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_shutdown_integration_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_shutdown_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_shutdown_integration_tests PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_logging_integration_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_logging_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_logging_integration_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_cache_integration_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_cache_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_cache_integration_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_transfer_logging_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_transfer_logging_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_transfer_logging_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_connection_handling_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_connection_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_connection_handling_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_robustness_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_robustness_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_robustness_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME http_async_logging_tests
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tests/http_async_logging_tests.py"
                    --binary "$<TARGET_FILE:http_server>"
        )
        set_tests_properties(http_async_logging_tests PROPERTIES
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    else()
        message(WARNING "Python3 not found; http_protocol_tests will not run")
    endif()
endif()

message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "SQLite3 version:  ${SQLite3_VERSION}")

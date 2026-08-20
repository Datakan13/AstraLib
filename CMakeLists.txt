cmake_minimum_required(VERSION 3.16)
project(AstraLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # lets IDE tooling find include paths

add_library(AstraLib INTERFACE)
target_include_directories(AstraLib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Sanitizer builds: assertion-based tests catch wrong VALUES, but only the
# sanitizers reliably catch data races (TSan) and lifetime/memory bugs (ASan).
# Usage: cmake -B build-tsan -DENABLE_TSAN=ON  (run tests with ASTRA_STRESS_SCALE=10)
# Note: gcc-11's TSan runtime can abort with "unexpected memory mapping" on
# 6.x kernels; run the binary via `setarch $(uname -m) -R <test>` if so.
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "TSan and ASan cannot be combined in one build")
endif()
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

option(BUILD_TESTS "Build AstraLib tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(BUILD_DEMO "Build AstraLib demos" ON)
if(BUILD_DEMO) 
    add_subdirectory(demo)
endif()
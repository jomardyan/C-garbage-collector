cmake_minimum_required(VERSION 3.20)
project(GCLib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(gc_lib
    src/GCManager.cpp
    src/RootScanner.cpp
)

set_target_properties(gc_lib PROPERTIES OUTPUT_NAME "GC-Lib")

target_include_directories(gc_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(gc_lib PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gc_demo
    src/main.cpp
)

target_link_libraries(gc_demo PRIVATE gc_lib)

if(MSVC)
    target_compile_options(gc_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gc_tests
    tests/gc_tests.cpp
)

target_link_libraries(gc_tests PRIVATE gc_lib)

if(MSVC)
    target_compile_options(gc_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME gc_tests COMMAND gc_tests)

cmake_minimum_required(VERSION 3.20)
project(GCLibConsumer LANGUAGES CXX)
find_package(GCLib 0.1 CONFIG REQUIRED)
add_executable(gc_package_smoke main.cpp)
target_link_libraries(gc_package_smoke PRIVATE GCLib::gc_lib)

# Compile installed headers in separate translation units so umbrella includes
# cannot conceal missing dependencies in a public header.
get_target_property(gc_installed_include_dirs GCLib::gc_lib INTERFACE_INCLUDE_DIRECTORIES)
list(GET gc_installed_include_dirs 0 gc_installed_include_dir)
if(NOT GC_SOURCE_DIR)
    get_filename_component(GC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
include("${GC_SOURCE_DIR}/cmake/AddHeaderChecks.cmake")
gc_add_header_checks(gc_installed_header_checks
    "${gc_installed_include_dir}" GCLib::gc_lib)

if(WIN32)
    get_target_property(gc_library_type GCLib::gc_lib TYPE)
    if(gc_library_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET gc_package_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:GCLib::gc_lib> $<TARGET_FILE_DIR:gc_package_smoke>)
    endif()
endif()
enable_testing()
add_test(NAME gc_package_smoke COMMAND gc_package_smoke)
set_tests_properties(gc_package_smoke PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0" TIMEOUT 30)

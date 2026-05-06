cmake_minimum_required(VERSION 3.20)
project(GCLib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

enable_testing()

# ---- Sanitizers -------------------------------------------------------------
option(GC_ENABLE_ASAN  "Build with AddressSanitizer + UBSan"  OFF)
option(GC_ENABLE_TSAN  "Build with ThreadSanitizer"           OFF)

function(gc_enable_sanitizers target sanitize_sources)
    if(GC_ENABLE_ASAN)
        if(${sanitize_sources})
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-address-use-after-scope)
        endif()
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    elseif(GC_ENABLE_TSAN)
        if(${sanitize_sources})
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        endif()
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

# ---- Library ----------------------------------------------------------------
add_library(gc_lib
    src/GCManager.cpp
    src/RootScanner.cpp
)

add_library(GCLib::gc_lib ALIAS gc_lib)

set_target_properties(gc_lib PROPERTIES OUTPUT_NAME "GC-Lib")

target_include_directories(gc_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(MSVC)
    target_compile_options(gc_lib PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

gc_enable_sanitizers(gc_lib TRUE)

# ---- Demo executable --------------------------------------------------------
add_executable(gc_demo src/main.cpp)
target_link_libraries(gc_demo PRIVATE gc_lib)
if(MSVC)
    target_compile_options(gc_demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

gc_enable_sanitizers(gc_demo FALSE)

# ---- Tests ------------------------------------------------------------------
add_executable(gc_tests tests/gc_tests.cpp)
target_link_libraries(gc_tests PRIVATE gc_lib)
if(MSVC)
    target_compile_options(gc_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(gc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gc_enable_sanitizers(gc_tests FALSE)

add_test(NAME gc_tests COMMAND gc_tests)
if(GC_ENABLE_ASAN)
    set_tests_properties(gc_tests PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0")
endif()

# ---- Examples ---------------------------------------------------------------
option(GC_BUILD_EXAMPLES "Build example programs" ON)

if(GC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- Install / export -------------------------------------------------------
install(TARGETS gc_lib
    EXPORT GCLibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT GCLibTargets
    FILE GCLibTargets.cmake
    NAMESPACE GCLib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GCLib
)

configure_package_config_file(
    cmake/GCLibConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GCLibConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GCLib
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/GCLibConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/GCLibConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/GCLibConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GCLib
)

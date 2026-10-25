cmake_minimum_required(VERSION 3.20)
project(GCLib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

set(GC_TOP_LEVEL OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(GC_TOP_LEVEL ON)
endif()
option(BUILD_TESTING "Build tests" ${GC_TOP_LEVEL})
option(GC_BUILD_DEMO "Build the demonstration executable" ${GC_TOP_LEVEL})
option(GC_BUILD_EXAMPLES "Build example programs" ${GC_TOP_LEVEL})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(CTest)
find_package(Threads REQUIRED)

# ---- Sanitizers -------------------------------------------------------------
option(GC_ENABLE_ASAN  "Build with AddressSanitizer + UBSan"  OFF)
option(GC_ENABLE_TSAN  "Build with ThreadSanitizer"           OFF)

if(GC_ENABLE_ASAN AND GC_ENABLE_TSAN)
    message(FATAL_ERROR "GC_ENABLE_ASAN and GC_ENABLE_TSAN cannot be enabled together")
endif()
if((GC_ENABLE_ASAN OR GC_ENABLE_TSAN) AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR "GC sanitizer builds require GCC or Clang")
endif()

function(gc_enable_sanitizers target)
    # Consumers must link the sanitizer runtime, including installed static builds.
    # PUBLIC compilation also instruments inline allocation and pointer helpers.
    get_target_property(target_type ${target} TYPE)
    if(target_type MATCHES "LIBRARY$")
        set(scope PUBLIC)
    else()
        set(scope PRIVATE)
    endif()
    if(GC_ENABLE_ASAN)
        target_compile_options(${target} ${scope}
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-address-use-after-scope)
        target_link_options(${target} ${scope} -fsanitize=address,undefined)
    elseif(GC_ENABLE_TSAN)
        target_compile_options(${target} ${scope}
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} ${scope} -fsanitize=thread)
    endif()
endfunction()

function(gc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ---- Library ----------------------------------------------------------------
add_library(gc_lib
    src/GCManager.cpp
    src/RootScanner.cpp
)

add_library(GCLib::gc_lib ALIAS gc_lib)

set_target_properties(gc_lib PROPERTIES
    OUTPUT_NAME "GC-Lib"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
target_compile_features(gc_lib PUBLIC cxx_std_20)
target_link_libraries(gc_lib PUBLIC Threads::Threads)

target_include_directories(gc_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

gc_enable_warnings(gc_lib)
gc_enable_sanitizers(gc_lib)

# ---- Demo executable --------------------------------------------------------
if(GC_BUILD_DEMO)
    add_executable(gc_demo src/main.cpp)
    target_link_libraries(gc_demo PRIVATE gc_lib)
    gc_enable_warnings(gc_demo)
endif()

# ---- Tests ------------------------------------------------------------------
if(BUILD_TESTING)
    function(gc_register_test target)
        gc_enable_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120)
        if(GC_ENABLE_ASAN)
            set_tests_properties(${target} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0")
        endif()
    endfunction()
    function(gc_add_test target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE gc_lib)
        gc_register_test(${target})
    endfunction()
    gc_add_test(gc_tests tests/gc_tests.cpp)
    gc_add_test(gc_core_regressions tests/core_regressions.cpp)
    gc_add_test(gc_handle_tests tests/handle_tests.cpp)
    gc_add_test(gc_scanner_tests tests/scanner_tests.cpp)
    # Link the subject directly so replacement operator new also intercepts its
    # bookkeeping allocations on platforms without DLL symbol interposition.
    add_executable(gc_allocation_failure_tests
        tests/allocation_failure_tests.cpp src/GCManager.cpp src/RootScanner.cpp)
    target_include_directories(gc_allocation_failure_tests PRIVATE include)
    target_compile_features(gc_allocation_failure_tests PRIVATE cxx_std_20)
    target_link_libraries(gc_allocation_failure_tests PRIVATE Threads::Threads)
    gc_enable_sanitizers(gc_allocation_failure_tests)
    gc_register_test(gc_allocation_failure_tests)

    add_test(NAME gc_package_consumer
        COMMAND ${CMAKE_COMMAND}
            -DGC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DGC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DGC_CONFIG=$<CONFIG>
            -DGC_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DGC_GENERATOR=${CMAKE_GENERATOR}
            -DGC_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -DGC_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestInstalledPackage.cmake)
    set_tests_properties(gc_package_consumer PROPERTIES TIMEOUT 120)
endif()

# ---- Examples ---------------------------------------------------------------
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

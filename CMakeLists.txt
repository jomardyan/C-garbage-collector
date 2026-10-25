cmake_minimum_required(VERSION 3.20)
project(GCLib VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

set(GC_TOP_LEVEL OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(GC_TOP_LEVEL ON)
endif()
option(BUILD_TESTING "Build tests" ${GC_TOP_LEVEL})
option(GC_BUILD_DEMO "Build the demonstration executable" ${GC_TOP_LEVEL})
option(GC_BUILD_EXAMPLES "Build example programs" ${GC_TOP_LEVEL})
option(GC_BUILD_BENCHMARKS "Build the optional correctness-checked benchmark CLI" OFF)

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

# Before 1.0, a minor release can change public object layouts. Give it a
# distinct shared-library ABI while retaining source-level package compatibility.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(GC_ABI_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
else()
    set(GC_ABI_VERSION "${PROJECT_VERSION_MAJOR}")
endif()
set_target_properties(gc_lib PROPERTIES
    OUTPUT_NAME "GC-Lib"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${GC_ABI_VERSION}
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
    function(gc_configure_test name labels)
        set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS "${labels}")
        if(GC_ENABLE_ASAN)
            set_tests_properties(${name} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=detect_stack_use_after_return=0")
        endif()
    endfunction()
    function(gc_register_test target)
        gc_enable_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
        if(ARGN)
            gc_configure_test(${target} "${ARGN}")
        else()
            gc_configure_test(${target} "unit;regression")
        endif()
    endfunction()
    function(gc_add_test target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE gc_lib)
        gc_register_test(${target} ${ARGN})
    endfunction()
    gc_add_test(gc_tests tests/gc_tests.cpp)
    gc_add_test(gc_core_regressions tests/core_regressions.cpp)
    gc_add_test(gc_handle_tests tests/handle_tests.cpp)
    gc_add_test(gc_scanner_tests tests/scanner_tests.cpp)
    gc_add_test(gc_array_feature_tests tests/array_feature_tests.cpp)
    gc_add_test(gc_diagnostic_tests tests/diagnostic_tests.cpp)
    gc_add_test(gc_graph_property_tests tests/graph_property_tests.cpp "unit;regression;stress")
    gc_add_test(gc_rooted_container_tests tests/rooted_container_tests.cpp)
    gc_add_test(gc_finalizer_contract_tests tests/finalizer_contract_tests.cpp)
    add_test(NAME gc_throwing_finalizer_contract
        COMMAND ${CMAKE_COMMAND}
            -DGC_FINALIZER_EXECUTABLE=$<TARGET_FILE:gc_finalizer_contract_tests>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/RunFinalizerContract.cmake)
    gc_configure_test(gc_throwing_finalizer_contract "integration;regression")

    include(cmake/AddHeaderChecks.cmake)
    gc_add_header_checks(gc_public_header_checks
        "${CMAKE_CURRENT_SOURCE_DIR}/include" gc_lib)
    # Link the subject directly so replacement operator new also intercepts its
    # bookkeeping allocations on platforms without DLL symbol interposition.
    add_executable(gc_allocation_failure_tests
        tests/allocation_failure_tests.cpp src/GCManager.cpp src/RootScanner.cpp)
    target_include_directories(gc_allocation_failure_tests PRIVATE include)
    target_compile_features(gc_allocation_failure_tests PRIVATE cxx_std_20)
    target_link_libraries(gc_allocation_failure_tests PRIVATE Threads::Threads)
    # This single-threaded test replaces operator new/delete to inject failures.
    # Clang's TSan runtime provides strong definitions of the same operators, so
    # keep this standalone executable uninstrumented in TSan builds. The library
    # and the other tests retain full TSan instrumentation; ASan still covers it.
    if(NOT GC_ENABLE_TSAN)
        gc_enable_sanitizers(gc_allocation_failure_tests)
    endif()
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
    gc_configure_test(gc_package_consumer "integration;package")
endif()

# ---- Examples ---------------------------------------------------------------
if(GC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- Executable smoke checks ------------------------------------------------
if(BUILD_TESTING)
    function(gc_add_smoke_test name target)
        # Example binaries live in examples/, while Windows puts the shared DLL
        # beside the library target. Set its search path only for the test process.
        if(WIN32 AND BUILD_SHARED_LIBS)
            add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND}
                    -DGC_EXECUTABLE=$<TARGET_FILE:${target}>
                    -DGC_LIBRARY_DIR=$<TARGET_FILE_DIR:gc_lib>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunExecutable.cmake)
        else()
            add_test(NAME ${name} COMMAND ${target})
        endif()
        gc_configure_test(${name} "integration;smoke")
    endfunction()
    if(GC_BUILD_DEMO)
        gc_add_smoke_test(gc_demo_smoke gc_demo)
    endif()
    if(GC_BUILD_EXAMPLES)
        foreach(example quick_start explicit_roots weak_arrays worker_threads collections_and_diagnostics)
            gc_add_smoke_test(gc_example_${example}_smoke gc_example_${example})
        endforeach()
    endif()
endif()

# ---- Optional benchmarks ----------------------------------------------------
if(GC_BUILD_BENCHMARKS)
    add_executable(gc_bench benchmarks/gc_bench.cpp)
    target_link_libraries(gc_bench PRIVATE gc_lib)
    gc_enable_warnings(gc_bench)
    if(BUILD_TESTING)
        foreach(mode smoke help invalid)
            add_test(NAME gc_bench_${mode}
                COMMAND ${CMAKE_COMMAND}
                    -DGC_BENCH_EXECUTABLE=$<TARGET_FILE:gc_bench>
                    -DGC_LIBRARY_DIR=$<TARGET_FILE_DIR:gc_lib>
                    -DGC_BENCH_MODE=${mode}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBenchmark.cmake)
            gc_configure_test(gc_bench_${mode} "integration;benchmark")
            set_tests_properties(gc_bench_${mode} PROPERTIES TIMEOUT 60)
        endforeach()
    endif()
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

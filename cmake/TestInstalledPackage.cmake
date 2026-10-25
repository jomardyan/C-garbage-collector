# Exercise the installed target without relying on the parent's language settings.
function(gc_run)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${errors}")
    endif()
endfunction()

set(prefix "${GC_BINARY_DIR}/package-smoke/install")
set(consumer_build "${GC_BINARY_DIR}/package-smoke/consumer")
gc_run("${CMAKE_COMMAND}" --install "${GC_BINARY_DIR}"
    --prefix "${prefix}" --config "${GC_CONFIG}")
set(generator_args -G "${GC_GENERATOR}")
if(GC_GENERATOR_PLATFORM)
    list(APPEND generator_args -A "${GC_GENERATOR_PLATFORM}")
endif()
if(GC_GENERATOR_TOOLSET)
    list(APPEND generator_args -T "${GC_GENERATOR_TOOLSET}")
endif()
gc_run("${CMAKE_COMMAND}"
    -S "${GC_SOURCE_DIR}/tests/package_consumer" -B "${consumer_build}"
    ${generator_args}
    "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DGC_SOURCE_DIR=${GC_SOURCE_DIR}"
    "-DCMAKE_CXX_COMPILER=${GC_CXX_COMPILER}")
gc_run("${CMAKE_COMMAND}" --build "${consumer_build}" --config "${GC_CONFIG}")
gc_run("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}"
    -C "${GC_CONFIG}" --output-on-failure)

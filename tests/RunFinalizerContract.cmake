if(NOT DEFINED GC_FINALIZER_EXECUTABLE OR NOT EXISTS "${GC_FINALIZER_EXECUTABLE}")
    message(FATAL_ERROR "GC_FINALIZER_EXECUTABLE must name the built finalizer contract test executable")
endif()

foreach(finalizer_mode IN ITEMS --throwing --throwing-collect --throwing-cleanup)
    execute_process(
        COMMAND "${GC_FINALIZER_EXECUTABLE}" "${finalizer_mode}"
        RESULT_VARIABLE finalizer_result
        OUTPUT_VARIABLE finalizer_stdout
        ERROR_VARIABLE finalizer_stderr
        TIMEOUT 30
    )

    # std::_Exit(73) in the terminate handler gives a stable exit status on
    # Windows, macOS, and Linux. Crashes, timeouts, or the other failure codes
    # must never count as the expected termination contract.
    if(NOT "${finalizer_result}" STREQUAL "73")
        message(FATAL_ERROR
            "Throwing finalizer (${finalizer_mode}) must reach std::terminate with exit code 73; got '${finalizer_result}'.\n"
            "stdout: ${finalizer_stdout}\nstderr: ${finalizer_stderr}")
    endif()
    message(STATUS "Throwing finalizer (${finalizer_mode}) reached its terminate handler (exit 73)")
endforeach()

function(gc_add_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE gc_lib)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    gc_enable_sanitizers(${target} FALSE)
endfunction()

gc_add_example(gc_example_quick_start quick_start.cpp)
gc_add_example(gc_example_explicit_roots explicit_roots.cpp)
gc_add_example(gc_example_weak_arrays weak_arrays.cpp)
gc_add_example(gc_example_worker_threads worker_threads.cpp)
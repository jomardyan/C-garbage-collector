function(gc_add_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE gc_lib)

    gc_enable_warnings(${target})
endfunction()

gc_add_example(gc_example_quick_start quick_start.cpp)
gc_add_example(gc_example_explicit_roots explicit_roots.cpp)
gc_add_example(gc_example_weak_arrays weak_arrays.cpp)
gc_add_example(gc_example_worker_threads worker_threads.cpp)
gc_add_example(gc_example_collections_and_diagnostics collections_and_diagnostics.cpp)

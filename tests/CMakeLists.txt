function(threadc_test name)
    add_executable(${name}_test ${name}_test.c)
    target_link_libraries(${name}_test PRIVATE threadc ctt::ctt)
    target_compile_options(${name}_test PRIVATE
        $<IF:$<BOOL:${MSVC}>,/W4,-Wall -Wextra -Wpedantic>)
    add_test(NAME ${name} COMMAND ${name}_test)
    # A broken lock or condition variable usually hangs rather than fails;
    # turn that into a failure instead of a CI job stuck for hours.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

threadc_test(smoke)
threadc_test(time)
threadc_test(thread)
threadc_test(mutex)
threadc_test(cond)

function(threadc_test name)
    add_executable(${name}_test ${name}_test.c)
    target_link_libraries(${name}_test PRIVATE threadc ctt::ctt)
    target_compile_options(${name}_test PRIVATE
        $<IF:$<BOOL:${MSVC}>,/W4,-Wall -Wextra -Wpedantic>)
    add_test(NAME ${name} COMMAND ${name}_test)
endfunction()

threadc_test(smoke)

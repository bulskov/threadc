add_library(threadc STATIC)
add_library(threadc::threadc ALIAS threadc)

# One backend per platform.  src/common/ (sema, queue, pool — no OS code)
# joins here once it exists.
if(WIN32)
    target_sources(threadc PRIVATE
        win32/threadc_win32.c
    )
else()
    target_sources(threadc PRIVATE
        posix/threadc_posix.c
    )
endif()

target_include_directories(threadc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>   # generated version.h
    $<INSTALL_INTERFACE:include>
)

# seqc propagates arena's include dirs.
target_link_libraries(threadc PUBLIC seqc)

if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(threadc PUBLIC Threads::Threads)
endif()

target_compile_options(threadc PRIVATE
    $<IF:$<BOOL:${MSVC}>,
        /W4,
        -Wall -Wextra -Wpedantic -Wno-unused-parameter>
)

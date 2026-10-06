cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" THREADC_VERSION)
project(threadc
    VERSION ${THREADC_VERSION}
    DESCRIPTION "Portable threads, locks and time for C11"
    HOMEPAGE_URL "https://github.com/bulskov/threadc"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Are we the top-level project, or pulled in via FetchContent/add_subdirectory?
# Tests, sanitizers and install rules are only on by default at top level, so
# consumers (e.g. fskim) don't build threadc's test suite.
if(DEFINED PROJECT_IS_TOP_LEVEL)
    set(THREADC_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
elseif(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(THREADC_TOP_LEVEL ON)
else()
    set(THREADC_TOP_LEVEL OFF)
endif()

option(THREADC_BUILD_TESTS "Build threadc's own test suite" ${THREADC_TOP_LEVEL})
option(THREADC_INSTALL "Generate install rules for threadc" ${THREADC_TOP_LEVEL})

# none | address (ASan + UBSan) | thread (TSan).  ASan and TSan cannot be
# combined, so this is a choice rather than two switches.
set(THREADC_SANITIZER "none" CACHE STRING "Sanitizer: none, address, thread")
set_property(CACHE THREADC_SANITIZER PROPERTY STRINGS none address thread)

if(THREADC_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(THREADC_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT THREADC_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "THREADC_SANITIZER must be none, address or thread")
endif()

# --- Dependencies ------------------------------------------------------------
# Fetched from source so they are built with the same compiler and flags
# (including sanitizers).  Dependency names are shared across the library
# family (arena, seqc, ctt, threadc, filec): FetchContent keeps the FIRST
# declaration of a name, so a parent project pins the versions for everyone.
#
# Build against local checkouts with the `dev` preset, or by hand:
#   -DFETCHCONTENT_SOURCE_DIR_SEQC=$HOME/repos/seqc
#   -DFETCHCONTENT_SOURCE_DIR_CTT=$HOME/repos/ctt
include(FetchContent)

set(THREADC_SEQC_GIT_REPOSITORY "https://github.com/bulskov/seqc.git"
    CACHE STRING "Clone URL for the seqc dependency")
set(THREADC_SEQC_GIT_TAG "v2.2.3"
    CACHE STRING "Tag or commit of the seqc dependency")
set(THREADC_CTT_GIT_TAG "v0.3.1"
    CACHE STRING "Tag or commit of the ctt dependency (tests only)")

FetchContent_Declare(seqc
    GIT_REPOSITORY ${THREADC_SEQC_GIT_REPOSITORY}
    GIT_TAG        ${THREADC_SEQC_GIT_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(seqc)

# --- Library -----------------------------------------------------------------
configure_file(
    include/threadc/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/threadc/version.h)

add_subdirectory(src)

# --- Tests -------------------------------------------------------------------
if(THREADC_BUILD_TESTS)
    FetchContent_Declare(ctt
        GIT_REPOSITORY https://github.com/bulskov/ctt.git
        GIT_TAG        ${THREADC_CTT_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(ctt)

    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install -----------------------------------------------------------------
if(THREADC_INSTALL)
    install(TARGETS threadc ARCHIVE DESTINATION lib)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/threadc
        DESTINATION include
        PATTERN "*.in" EXCLUDE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/threadc/version.h
        DESTINATION include/threadc)
endif()

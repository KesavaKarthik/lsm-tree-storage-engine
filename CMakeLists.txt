cmake_minimum_required(VERSION 3.16)

project(kvstore
    VERSION 0.1.0
    DESCRIPTION "A persistent key-value storage engine (Bitcask-style, evolving toward LSM)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++20, not -std=gnu++20.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # For clangd / clang-tidy.

# Single-config generators default to no build type, meaning no -g and no -O.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer (GCC/Clang only, not with ENABLE_SANITIZERS)" OFF)
option(KVSTORE_BUILD_TESTS "Build the kvstore test suite" ON)

# ASan and TSan instrument the same things differently and cannot coexist -- the
# combination either fails to link or reports nonsense. Separate builds, and
# saying so here beats finding out from a linker error.
if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR
        "ENABLE_SANITIZERS and ENABLE_TSAN are mutually exclusive. "
        "ThreadSanitizer needs its own build tree.")
endif()

# Warnings are applied per-target below, not globally, so GoogleTest's don't
# drown ours. MSVC has no -Wall/-Wextra; /W4 is the equivalent (/Wall there
# warns from inside system headers and is unusable).
if(MSVC)
    set(KVSTORE_WARNING_FLAGS /W4 /permissive-)
else()
    set(KVSTORE_WARNING_FLAGS -Wall -Wextra)
endif()

# Sanitizers go at directory scope, before the targets: ASan is whole-program,
# and a partly-instrumented heap gives false negatives.
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Without -fno-omit-frame-pointer the backtraces are useless.
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    elseif(MSVC)
        message(WARNING
            "ENABLE_SANITIZERS: MSVC provides AddressSanitizer only; "
            "UndefinedBehaviorSanitizer is not available on this toolchain. "
            "Run the sanitizer build under GCC or Clang for full coverage.")
        add_compile_options(/fsanitize=address /Zi)
        # MSVC ASan is incompatible with incremental linking and with /RTC.
        add_link_options(/INCREMENTAL:NO)
        string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endif()
endif()

# ThreadSanitizer. Its own option because Phase 3 put a thread on every
# connection, and ASan and UBSan find neither races nor lock-order problems --
# they are memory and UB tools. Without this there is no instrument that could
# disagree with the claim that LockedStore covers every path into the engine.
#
# Whole-program for the same reason ASan is: uninstrumented code is invisible to
# it, and a race with one end in an uninstrumented translation unit is a race it
# will not report.
if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
    else()
        message(WARNING "ENABLE_TSAN: ThreadSanitizer needs GCC or Clang; ignored.")
    endif()
endif()

# Phase 3 puts a thread on every connection. On GCC/Clang std::thread needs
# -pthread at both compile and link time, and omitting it fails in the worst
# possible way -- it links, and then std::thread throws at runtime or the
# process aborts inside pthread_create. MSVC needs nothing.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include(FetchContent)

# zlib, for crc32. Fetched rather than found for the same reason as GoogleTest:
# no system-wide install to arrange, and everyone gets the same version. We use
# exactly one function from it, but a hand-rolled crc32 is a checksum nobody
# else can verify a file against.
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

add_library(kvstore STATIC
    src/bitcask.cpp
    src/client.cpp
    src/compaction.cpp
    src/hint.cpp
    src/log_file.cpp
    src/locked_store.cpp
    src/log_scan.cpp
    src/memory_store.cpp
    src/platform_file.cpp
    src/platform_socket.cpp
    src/protocol.cpp
    src/record.cpp
    src/server.cpp
    src/socket.cpp
)
add_library(kvstore::kvstore ALIAS kvstore)

# PUBLIC: consumers get the include path by linking.
target_include_directories(kvstore
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src
)

# zlib 1.3.1 predates target-based include dirs, so we add them by hand.
# zconf.h is generated, hence the binary dir as well as the source dir.
target_include_directories(kvstore PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

# PRIVATE: zlib is an implementation detail. Nothing in our public headers
# mentions it, so nobody linking us needs to know it exists.
#
# ws2_32 is Winsock. PRIVATE is still correct for a static library -- CMake
# records it as a link-only interface requirement, so the executables that link
# kvstore pick it up without our headers pretending to export it.
#
# Threads is PUBLIC because it is not only a library: it carries -pthread as a
# *compile* option, and a consumer that starts a thread of its own needs the
# flag on its own translation units too.
target_link_libraries(kvstore
    PRIVATE zlibstatic
            $<$<PLATFORM_ID:Windows>:ws2_32>
    PUBLIC  Threads::Threads
)

# PRIVATE: our warning policy shouldn't leak into whoever links us.
target_compile_options(kvstore PRIVATE ${KVSTORE_WARNING_FLAGS})

target_compile_features(kvstore PUBLIC cxx_std_20)

# Executables. src/ is the library and nothing in it has a main(); apps/ is
# where the programs live, and each is thin enough to read in one sitting.
add_executable(kvstore_server apps/server_main.cpp)
target_link_libraries(kvstore_server PRIVATE kvstore::kvstore)
target_compile_options(kvstore_server PRIVATE ${KVSTORE_WARNING_FLAGS})

add_executable(kv_cli apps/cli_main.cpp)
target_link_libraries(kv_cli PRIVATE kvstore::kvstore)
target_compile_options(kv_cli PRIVATE ${KVSTORE_WARNING_FLAGS})
# The target name has an underscore because the rest of them do; the binary has
# a hyphen because that is what a command-line tool looks like.
set_target_properties(kv_cli PROPERTIES OUTPUT_NAME kv-cli)

if(KVSTORE_BUILD_TESTS)

    # Fetched at a pinned tag rather than find_package: a fresh checkout builds
    # with no system-wide install, and everyone tests the same version.
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # Match MSVC's CRT.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(kvstore_tests
        tests/bitcask_test.cpp
        tests/compaction_test.cpp
        tests/framing_test.cpp
        tests/hint_test.cpp
        tests/locked_store_test.cpp
        tests/memory_store_test.cpp
        tests/protocol_test.cpp
        tests/record_test.cpp
        tests/server_test.cpp
        tests/socket_test.cpp
    )

    target_link_libraries(kvstore_tests
        PRIVATE kvstore::kvstore
                GTest::gtest_main
    )

    # For kvstore_contract.hpp and temp_dir.hpp, shared between test files.
    target_include_directories(kvstore_tests PRIVATE tests)

    target_compile_options(kvstore_tests PRIVATE ${KVSTORE_WARNING_FLAGS})

    # Registers each TEST() with CTest separately, so failures name the case.
    # PRE_TEST because the default discovers by running the binary at build
    # time -- a binary that builds but can't start then fails the *build*.
    include(GoogleTest)
    gtest_discover_tests(kvstore_tests DISCOVERY_MODE PRE_TEST)
endif()

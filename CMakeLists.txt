cmake_minimum_required(VERSION 3.16)

project(memory_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MEMORY_POOL_BUILD_TESTS "Build memory pool tests" ON)
option(MEMORY_POOL_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)

find_package(Threads REQUIRED)

add_library(memory_pool STATIC
    src/CentralCache.cpp
    src/PageCache.cpp
    src/ThreadCache.cpp
)

target_include_directories(memory_pool
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(memory_pool
    PUBLIC
        Threads::Threads
)

target_compile_options(memory_pool
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(MEMORY_POOL_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(memory_pool PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(memory_pool PRIVATE -fsanitize=address)
endif()

if(MEMORY_POOL_BUILD_TESTS)
    enable_testing()

    add_executable(test_memory_pool
        test/test_memory_pool.cpp
    )

    target_link_libraries(test_memory_pool
        PRIVATE
            memory_pool
            Threads::Threads
    )

    target_compile_options(test_memory_pool
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    if(MEMORY_POOL_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_memory_pool PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(test_memory_pool PRIVATE -fsanitize=address)
    endif()

    add_test(NAME memory_pool_test COMMAND test_memory_pool)
endif()

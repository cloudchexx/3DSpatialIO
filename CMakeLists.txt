cmake_minimum_required(VERSION 3.16)
project(cc_3Dreader VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8 /wd4996)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(SOURCES
    src/main.cpp
    src/args.cpp
    src/file_io.cpp
    src/chunk_optimizer.cpp
    src/chunk_io.cpp
    src/profiler.cpp
)

add_executable(cc_3Dreader ${SOURCES})

target_include_directories(cc_3Dreader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── Unit tests (Phase 3) ──────────────────────────────────────
add_executable(test_chunk_optimizer
    tests/test_chunk_optimizer.cpp
    src/chunk_optimizer.cpp
)
target_include_directories(test_chunk_optimizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ─── Integration tests (Phase 4) ───────────────────────────────
add_executable(test_chunk_io
    tests/test_chunk_io.cpp
    src/chunk_optimizer.cpp
    src/chunk_io.cpp
)
target_include_directories(test_chunk_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()
add_test(NAME chunk_optimizer COMMAND test_chunk_optimizer)
add_test(NAME chunk_io       COMMAND test_chunk_io)

cmake_minimum_required(VERSION 3.20)
project(sp1_drum_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

# Shared strict warning flags for all pure C11 modules and host unit tests
add_library(sp1_strict_warnings INTERFACE)
target_compile_options(sp1_strict_warnings INTERFACE
    -Wall
    -Wextra
    -Werror
)

# Project include paths (portable pure C11 core & DSP headers live under src/)
set(SP1_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

function(sp1_add_host_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${SP1_REPO_ROOT}/src"
    )
    target_link_libraries(${target_name} PRIVATE sp1_strict_warnings m)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

sp1_add_host_test(test_smoke test_smoke.c)

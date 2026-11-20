cmake_minimum_required(VERSION 3.20)
project(sp1_drum_host_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Shared strict warning flags for all pure C11 modules, C++17 header checks, and host unit tests
add_library(sp1_strict_warnings INTERFACE)
target_compile_options(sp1_strict_warnings INTERFACE
    -Wall
    -Wextra
    -Werror
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

# Project include paths (portable pure C11 core, DSP, and HAL headers live under src/)
set(SP1_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Host HAL static library (pure C11 common logic + host WAV/MIDI backend)
add_library(sp1_hal_host STATIC
    "${SP1_REPO_ROOT}/src/hal/sp1_hal_common.c"
    "${SP1_REPO_ROOT}/src/hal/sp1_hal_host.c"
)
target_include_directories(sp1_hal_host PUBLIC
    "${SP1_REPO_ROOT}/src"
)
target_link_libraries(sp1_hal_host PRIVATE sp1_strict_warnings)

# Offline WAV + MIDI host simulator CLI
add_executable(sp1_sim
    "${SP1_REPO_ROOT}/sim/sp1_sim.c"
)
target_link_libraries(sp1_sim PRIVATE sp1_hal_host sp1_strict_warnings m)

function(sp1_add_host_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${SP1_REPO_ROOT}/src"
    )
    target_link_libraries(${target_name} PRIVATE sp1_hal_host sp1_strict_warnings m)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

sp1_add_host_test(test_smoke test_smoke.c)
sp1_add_host_test(test_hal_midi_queue test_hal_midi_queue.c)
sp1_add_host_test(test_hal_controls test_hal_controls.c)
sp1_add_host_test(test_hal_audio_sim test_hal_audio_sim.c)
sp1_add_host_test(test_hal_cpp_compat test_hal_cpp_compat.cpp)

add_test(
    NAME test_sim_cli
    COMMAND $<TARGET_FILE:sp1_sim>
            --wav "${CMAKE_CURRENT_BINARY_DIR}/sim_cli_smoke.wav"
            --midi-csv "${CMAKE_CURRENT_BINARY_DIR}/sim_cli_smoke.csv"
            --duration-ms 500
            --bpm 120
            --quiet
)

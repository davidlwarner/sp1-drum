cmake_minimum_required(VERSION 3.20.0)

# Custom board definitions live in boards/
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stem-player-bsp)

# ── BSP sources (board support — generally don't modify) ─────────
set(BSP_SOURCES
    subsys/system/ResetBreadcrumbs.cpp
    subsys/system/Watchdog.cpp
    subsys/power/PowerManager.cpp
    drivers/charger/charger_bq24232.c
    drivers/audio/cs42l42_codec.c
    drivers/audio/tas2505_codec.c
)

# ── App sources (your code here) ─────────────────────────────────
set(APP_SOURCES
    app/main.cpp
    app/AppStateMachine.cpp
    app/MidiController.cpp
    app/Leds.cpp
)

# ── Drum Machine Core, DSP & HAL sources (strict C11) ────────────
set(CORE_SOURCES
    src/core/euclid.c
    src/core/clock_sync.c
    src/core/sequencer.c
    src/core/ui_mapper.c
)

set(DSP_SOURCES
    src/dsp/drum_voices.c
    src/dsp/master_fx.c
)

set(HAL_SOURCES
    src/hal/sp1_hal_common.c
    src/hal/sp1_hal_zephyr.c
)

set_source_files_properties(${BSP_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-function"
)

set_source_files_properties(${CORE_SOURCES} ${DSP_SOURCES} ${HAL_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-std=c11;-Wall;-Wextra;-Werror"
)

target_sources(app PRIVATE ${BSP_SOURCES} ${APP_SOURCES} ${CORE_SOURCES} ${DSP_SOURCES} ${HAL_SOURCES})
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/subsys
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZEPHYR_BASE}/lib/midi2
)

# C++17, no exceptions/RTTI for embedded.
target_compile_features(app PRIVATE cxx_std_17)
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)

#pragma once

#include <cstddef>
#include <cstdint>
#include <zephyr/sys/reboot.h>

namespace core::reset_breadcrumbs {

struct Snapshot {
    uint32_t reset_cause = 0U;
    uint32_t resetreas = 0U;
    uint8_t gpregret = 0U;
    uint8_t gpregret2 = 0U;
    uint8_t retained_code = 0U;
    uint8_t retained_watchdog_channel = 0U;
    uint8_t retained_control_phase = 0U;
    bool reset_cause_valid = false;
    bool retained_valid = false;
    bool retained_watchdog_valid = false;
    bool retained_control_phase_valid = false;

    /* Retained previous-boot fatal crash breadcrumb (captured from __noinit SRAM) */
    bool retained_crash_valid = false;
    uint32_t crash_reason = 0U;
    uint32_t crash_pc = 0U;
    uint32_t crash_lr = 0U;
    uint32_t crash_xpsr = 0U;
    uint32_t crash_cfsr = 0U;
    uint32_t crash_hfsr = 0U;
    uint32_t crash_mmfar = 0U;
    uint32_t crash_bfar = 0U;
};

void capture_early();
Snapshot snapshot();
void clear_live_markers();
void mark(uint8_t code);
void mark_watchdog_expiry(uint8_t channel);
void mark_control_phase(uint8_t code);
void mark_software_reboot(int type);
bool has_boot_report();
void print_boot_report();

}  // namespace core::reset_breadcrumbs

#include "ResetBreadcrumbs.hpp"

#include <cstddef>
#include <cstring>
#include <type_traits>

#include <zephyr/arch/exception.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <nrf.h>

LOG_MODULE_REGISTER(reset_breadcrumbs, LOG_LEVEL_INF);

namespace core::reset_breadcrumbs {
namespace {

constexpr uint32_t kRetainedMagic = 0x53504252U;  // "SPBR"
constexpr uint32_t kCrashMagic = 0x53504352U;     // "SPCR"

struct RetainedCrash {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t xpsr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t crc32;
};

static_assert(sizeof(RetainedCrash) == 10U * sizeof(uint32_t),
              "RetainedCrash must have no implicit padding bytes");
static_assert(offsetof(RetainedCrash, crc32) == 9U * sizeof(uint32_t),
              "RetainedCrash::crc32 must be the final field");

struct RetainedBreadcrumb {
    uint32_t magic;
    uint8_t code;
    uint8_t watchdog_channel;
    uint8_t flags;
    uint8_t control_phase;
    uint32_t sequence;
    RetainedCrash crash;
};

static_assert(std::is_trivial_v<RetainedBreadcrumb>,
              "RetainedBreadcrumb must remain trivial to preserve __noinit SRAM across resets");

constexpr uint8_t kFlagProgressValid = BIT(0);
constexpr uint8_t kFlagWatchdogValid = BIT(1);
constexpr uint8_t kFlagControlPhaseValid = BIT(2);
constexpr uint8_t kFlagCrashValid = BIT(3);
constexpr uint8_t kFlagSoftRebootValid = BIT(4);

Snapshot g_snapshot{};
bool g_captured = false;
RetainedBreadcrumb g_retained __noinit;
volatile bool g_in_fatal_handler = false;

uint32_t compute_crash_crc32(const RetainedCrash& c) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&c);
    const std::size_t len = offsetof(RetainedCrash, crc32);
    uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0U; i < len; ++i) {
        crc ^= static_cast<uint32_t>(bytes[i]);
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
    }
    return ~crc;
}

const char* fatal_reason_str(uint32_t reason) {
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "CPU_EXCEPTION";
    case K_ERR_SPURIOUS_IRQ:
        return "SPURIOUS_IRQ";
    case K_ERR_STACK_CHK_FAIL:
        return "STACK_OVERFLOW";
    case K_ERR_KERNEL_OOPS:
        return "KERNEL_OOPS";
    case K_ERR_KERNEL_PANIC:
        return "KERNEL_PANIC";
    default:
        return "UNKNOWN";
    }
}

void print_crash_details(const Snapshot& s) {
    printk("\r\n================ PREVIOUS-BOOT CRASH BREADCRUMB ================\r\n");
    printk("Reason : %u (%s)\r\n",
           static_cast<unsigned int>(s.crash_reason),
           fatal_reason_str(s.crash_reason));
    printk("PC=0x%08x  LR=0x%08x  xPSR=0x%08x\r\n",
           static_cast<unsigned int>(s.crash_pc),
           static_cast<unsigned int>(s.crash_lr),
           static_cast<unsigned int>(s.crash_xpsr));
    printk("CFSR=0x%08x  HFSR=0x%08x  MMFAR=0x%08x  BFAR=0x%08x\r\n",
           static_cast<unsigned int>(s.crash_cfsr),
           static_cast<unsigned int>(s.crash_hfsr),
           static_cast<unsigned int>(s.crash_mmfar),
           static_cast<unsigned int>(s.crash_bfar));
    printk("================================================================\r\n");
}

int cmd_crash_report(const struct shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    const Snapshot& s = g_snapshot;
    shell_print(sh, "RESETREAS=0x%08x GPREGRET=0x%02x GPREGRET2=0x%02x",
                static_cast<unsigned int>(s.resetreas),
                static_cast<unsigned int>(s.gpregret),
                static_cast<unsigned int>(s.gpregret2));
    if (s.retained_watchdog_valid) {
        shell_warn(sh, "Previous boot watchdog expiry on channel %u",
                   static_cast<unsigned int>(s.retained_watchdog_channel));
    }
    if (s.retained_crash_valid) {
        print_crash_details(s);
    } else {
        shell_print(sh, "No retained fatal crash breadcrumb from previous boot.");
    }
    return 0;
}

SHELL_CMD_REGISTER(crash, NULL, "Print retained previous-boot crash breadcrumb and reset info",
                   cmd_crash_report);

void handle_fatal_error(unsigned int reason, const struct arch_esf* esf) {
    /* Keep the fatal handler minimal and atomic: IRQs are already locked by
     * Zephyr's fatal error path and kernel spinlocks may be held. Never unlock
     * IRQs, sleep, busy-wait, or call into logging/USB here—persist the fault
     * record directly into __noinit SRAM and cold-reboot immediately. */
    if (!g_in_fatal_handler) {
        g_in_fatal_handler = true;

        g_retained.magic = kRetainedMagic;
        g_retained.crash.magic = kCrashMagic;
        g_retained.crash.reason = static_cast<uint32_t>(reason);
        g_retained.crash.pc = (esf != nullptr) ? esf->basic.pc : 0U;
        g_retained.crash.lr = (esf != nullptr) ? esf->basic.lr : 0U;
        g_retained.crash.xpsr = (esf != nullptr) ? esf->basic.xpsr : 0U;
        g_retained.crash.cfsr = SCB->CFSR;
        g_retained.crash.hfsr = SCB->HFSR;
        g_retained.crash.mmfar = SCB->MMFAR;
        g_retained.crash.bfar = SCB->BFAR;
        g_retained.crash.crc32 = compute_crash_crc32(g_retained.crash);
        g_retained.flags |= kFlagCrashValid;
        ++g_retained.sequence;
        __DSB();
    }

    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
    }
}

}  // namespace

void capture_early() {
    if (g_captured) {
        return;
    }

    g_snapshot.resetreas = NRF_POWER->RESETREAS;
    g_snapshot.gpregret = static_cast<uint8_t>(NRF_POWER->GPREGRET & 0xFFU);
    g_snapshot.gpregret2 = static_cast<uint8_t>(NRF_POWER->GPREGRET2 & 0xFFU);
    g_snapshot.reset_cause_valid =
        hwinfo_get_reset_cause(&g_snapshot.reset_cause) == 0;
    if (g_retained.magic == kRetainedMagic) {
        g_snapshot.retained_code = g_retained.code;
        g_snapshot.retained_valid = (g_retained.flags & kFlagProgressValid) != 0U;
        g_snapshot.retained_watchdog_channel = g_retained.watchdog_channel;
        g_snapshot.retained_watchdog_valid = (g_retained.flags & kFlagWatchdogValid) != 0U;
        g_snapshot.retained_control_phase = g_retained.control_phase;
        g_snapshot.retained_control_phase_valid =
            (g_retained.flags & kFlagControlPhaseValid) != 0U;

        if ((g_retained.flags & kFlagCrashValid) != 0U &&
            g_retained.crash.magic == kCrashMagic &&
            g_retained.crash.crc32 == compute_crash_crc32(g_retained.crash)) {
            g_snapshot.retained_crash_valid = true;
            g_snapshot.crash_reason = g_retained.crash.reason;
            g_snapshot.crash_pc = g_retained.crash.pc;
            g_snapshot.crash_lr = g_retained.crash.lr;
            g_snapshot.crash_xpsr = g_retained.crash.xpsr;
            g_snapshot.crash_cfsr = g_retained.crash.cfsr;
            g_snapshot.crash_hfsr = g_retained.crash.hfsr;
            g_snapshot.crash_mmfar = g_retained.crash.mmfar;
            g_snapshot.crash_bfar = g_retained.crash.bfar;
        }
    }
    g_captured = true;
}

Snapshot snapshot() {
    return g_snapshot;
}

void clear_live_markers() {
    g_retained.magic = kRetainedMagic;
    g_retained.code = 0U;
    g_retained.watchdog_channel = 0U;
    g_retained.flags = 0U;
    g_retained.control_phase = 0U;
    std::memset(&g_retained.crash, 0, sizeof(g_retained.crash));
    ++g_retained.sequence;
}

void mark(uint8_t code) {
    g_retained.magic = kRetainedMagic;
    g_retained.code = code;
    g_retained.flags |= kFlagProgressValid;
    ++g_retained.sequence;
}

void mark_watchdog_expiry(uint8_t channel) {
    g_retained.magic = kRetainedMagic;
    g_retained.watchdog_channel = channel;
    g_retained.flags |= kFlagWatchdogValid;
    ++g_retained.sequence;
}

void mark_control_phase(uint8_t code) {
    g_retained.magic = kRetainedMagic;
    g_retained.control_phase = code;
    g_retained.flags |= kFlagControlPhaseValid;
    ++g_retained.sequence;
}

void mark_software_reboot(int type) {
    g_retained.magic = kRetainedMagic;
    g_retained.code = static_cast<uint8_t>(type & 0xFF);
    g_retained.flags |= kFlagSoftRebootValid;
    ++g_retained.sequence;
}

bool has_boot_report() {
    const Snapshot& s = g_snapshot;
    const bool hw_dog_or_lockup =
        (s.resetreas & (POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk)) != 0U;
    return s.retained_crash_valid || s.retained_watchdog_valid || hw_dog_or_lockup;
}

void print_boot_report() {
    if (!has_boot_report()) {
        return;
    }

    const Snapshot& s = g_snapshot;

    LOG_ERR("Previous-boot abnormal reset detected: RESETREAS=0x%08x GPREGRET=0x%02x code=0x%02x",
            static_cast<unsigned int>(s.resetreas),
            static_cast<unsigned int>(s.gpregret),
            static_cast<unsigned int>(s.retained_code));

    if (s.retained_watchdog_valid) {
        LOG_ERR("Previous-boot software watchdog expired on channel %u (phase=0x%02x)",
                static_cast<unsigned int>(s.retained_watchdog_channel),
                static_cast<unsigned int>(s.retained_control_phase));
    }

    if (s.retained_crash_valid) {
        LOG_ERR("Previous-boot fatal fault: reason=%u (%s) PC=0x%08x LR=0x%08x CFSR=0x%08x HFSR=0x%08x",
                static_cast<unsigned int>(s.crash_reason),
                fatal_reason_str(s.crash_reason),
                static_cast<unsigned int>(s.crash_pc),
                static_cast<unsigned int>(s.crash_lr),
                static_cast<unsigned int>(s.crash_cfsr),
                static_cast<unsigned int>(s.crash_hfsr));
        print_crash_details(s);
    }
}

}  // namespace core::reset_breadcrumbs

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf* esf) {
    core::reset_breadcrumbs::handle_fatal_error(reason, esf);
}

#include "ResetBreadcrumbs.hpp"

#include <cstring>
#include <type_traits>

#include <zephyr/kernel.h>
#include <zephyr/arch/exception.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/irq.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_wdt.h>
#include <nrf.h>

LOG_MODULE_REGISTER(reset_breadcrumbs, LOG_LEVEL_INF);

namespace core::reset_breadcrumbs {
namespace {

constexpr uint32_t kRetainedMagic = 0x53504252U;      // "SPBR"
constexpr uint32_t kCrashMagic = 0x53504352U;         // "SPCR"
constexpr uint32_t kSlot0Start = 0x00020000U;
constexpr uint32_t kSlot0End = 0x000FF000U;
constexpr uint32_t kSramStart = 0x20000000U;
constexpr uint32_t kSramEnd = 0x20040000U;
constexpr uint32_t kMaxStackScanWords = 96U;
constexpr uint32_t kFatalFlushTotalMs = 1500U;
constexpr uint32_t kFatalFlushStepMs = 50U;

struct RetainedCrash {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    uint32_t xpsr;
    uint32_t psp;
    uint32_t msp;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t backtrace[kMaxBacktraceFrames];
    uint8_t backtrace_len;
    char thread[kMaxThreadNameLen];
    uint32_t checksum;
};

struct RetainedBreadcrumb {
    uint32_t magic;
    uint8_t code;
    uint8_t watchdog_channel;
    uint8_t flags;
    uint8_t control_phase;
    uint32_t sequence;
    RetainedCrash crash;
};

constexpr uint8_t kFlagProgressValid = BIT(0);
constexpr uint8_t kFlagWatchdogValid = BIT(1);
constexpr uint8_t kFlagControlPhaseValid = BIT(2);
constexpr uint8_t kFlagCrashValid = BIT(3);
constexpr uint8_t kFlagSoftRebootValid = BIT(4);

Snapshot g_snapshot{};
bool g_captured = false;
RetainedBreadcrumb g_retained __noinit;
struct k_work_delayable g_boot_report_work;
bool g_boot_report_work_inited = false;
volatile bool g_in_fatal_handler = false;

uint32_t compute_crash_checksum(const RetainedCrash& c) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&c);
    const std::size_t len = offsetof(RetainedCrash, checksum);
    uint32_t h = 2166136261U;
    for (std::size_t i = 0U; i < len; ++i) {
        h ^= static_cast<uint32_t>(bytes[i]);
        h *= 16777619U;
    }
    return h;
}

void feed_hardware_wdt_all() {
    for (size_t i = 0; i < NRF_WDT_CHANNEL_NUMBER; ++i) {
        nrf_wdt_reload_request_set(NRF_WDT0, static_cast<nrf_wdt_rr_register_t>(i));
    }
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

bool is_valid_thumb_call_return_addr(uint32_t word) {
    if ((word & 1U) == 0U) {
        return false;
    }
    const uint32_t addr = word & ~1U;
    if (addr < (kSlot0Start + 4U) || addr >= kSlot0End) {
        return false;
    }

    const uint16_t hw_prev = *reinterpret_cast<const uint16_t*>(addr - 2U);
    /* 16-bit Thumb BLX <Rm>: 0100 0111 1xxx x000 */
    if ((hw_prev & 0xFF87U) == 0x4780U) {
        return true;
    }

    /* 32-bit Thumb-2 BL / BLX(imm): hw0=1111 0xxx xxxx xxxx, hw1=11x1 xxxx xxxx xxxx */
    const uint16_t hw0 = *reinterpret_cast<const uint16_t*>(addr - 4U);
    if ((hw0 & 0xF800U) == 0xF000U && (hw_prev & 0xD000U) == 0xD000U) {
        return true;
    }

    return false;
}

void capture_stack_backtrace(const struct arch_esf* esf,
                             uint32_t psp,
                             uint32_t msp,
                             struct k_thread* thread,
                             RetainedCrash& out) {
    out.backtrace_len = 0U;

    if (esf != nullptr) {
        const uint32_t lr = esf->basic.lr;
        if ((lr & 1U) != 0U && lr >= kSlot0Start && lr < kSlot0End) {
            out.backtrace[out.backtrace_len++] = lr;
        }
    }

    uint32_t sp = psp;
    if (sp < kSramStart || sp >= kSramEnd) {
        if (esf != nullptr) {
            sp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(esf));
        } else {
            sp = msp;
        }
    }

    if (sp < kSramStart || sp >= kSramEnd) {
        return;
    }

    uint32_t scan_start = (sp + 3U) & ~3U;
    uint32_t scan_end = kSramEnd;

#if defined(CONFIG_THREAD_STACK_INFO)
    if (thread != nullptr) {
        const uint32_t st_start = static_cast<uint32_t>(thread->stack_info.start);
        const uint32_t st_end = st_start + static_cast<uint32_t>(thread->stack_info.size);
        if (st_start >= kSramStart && st_end <= kSramEnd &&
            scan_start >= st_start && scan_start < st_end) {
            scan_end = st_end;
        }
    }
#else
    ARG_UNUSED(thread);
#endif

    const uint32_t max_end = scan_start + (kMaxStackScanWords * sizeof(uint32_t));
    if (scan_end > max_end) {
        scan_end = max_end;
    }

    for (uint32_t addr = scan_start;
         addr + sizeof(uint32_t) <= scan_end && out.backtrace_len < kMaxBacktraceFrames;
         addr += sizeof(uint32_t)) {
        const uint32_t word = *reinterpret_cast<const uint32_t*>(addr);
        if (!is_valid_thumb_call_return_addr(word)) {
            continue;
        }
        /* Avoid duplicating the immediately preceding recorded return address */
        if (out.backtrace_len > 0U && out.backtrace[out.backtrace_len - 1U] == word) {
            continue;
        }
        out.backtrace[out.backtrace_len++] = word;
    }
}

void print_crash_details(const char* banner,
                         uint32_t reason,
                         const char* thread_name,
                         uint32_t pc,
                         uint32_t lr,
                         uint32_t xpsr,
                         uint32_t psp,
                         uint32_t msp,
                         uint32_t cfsr,
                         uint32_t hfsr,
                         uint32_t mmfar,
                         uint32_t bfar,
                         uint32_t r0,
                         uint32_t r1,
                         uint32_t r2,
                         uint32_t r3,
                         uint32_t r12,
                         const uint32_t* bt,
                         uint8_t bt_len) {
    printk("\r\n================ %s ================\r\n", banner);
    printk("Reason : %u (%s) | Thread: %s\r\n",
           static_cast<unsigned int>(reason),
           fatal_reason_str(reason),
           (thread_name != nullptr && thread_name[0] != '\0') ? thread_name : "<unknown>");
    printk("PC=0x%08x  LR=0x%08x  xPSR=0x%08x\r\n",
           static_cast<unsigned int>(pc),
           static_cast<unsigned int>(lr),
           static_cast<unsigned int>(xpsr));
    printk("R0=0x%08x  R1=0x%08x  R2=0x%08x  R3=0x%08x  R12=0x%08x\r\n",
           static_cast<unsigned int>(r0),
           static_cast<unsigned int>(r1),
           static_cast<unsigned int>(r2),
           static_cast<unsigned int>(r3),
           static_cast<unsigned int>(r12));
    printk("PSP=0x%08x  MSP=0x%08x\r\n",
           static_cast<unsigned int>(psp),
           static_cast<unsigned int>(msp));
    printk("CFSR=0x%08x  HFSR=0x%08x  MMFAR=0x%08x  BFAR=0x%08x\r\n",
           static_cast<unsigned int>(cfsr),
           static_cast<unsigned int>(hfsr),
           static_cast<unsigned int>(mmfar),
           static_cast<unsigned int>(bfar));
    printk("Backtrace (%u frames, resolve with arm-zephyr-eabi-addr2line -e zephyr.elf):\r\n",
           static_cast<unsigned int>(bt_len + 1U));
    printk("  #00 PC 0x%08x\r\n", static_cast<unsigned int>(pc));
    for (uint8_t i = 0U; i < bt_len && i < kMaxBacktraceFrames; ++i) {
        printk("  #%02u LR 0x%08x\r\n",
               static_cast<unsigned int>(i + 1U),
               static_cast<unsigned int>(bt[i]));
    }
    printk("========================================================\r\n");
}

void boot_report_work_handler(struct k_work* work) {
    ARG_UNUSED(work);
    print_boot_report();
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
        print_crash_details("PREVIOUS-BOOT CRASH DUMP",
                            s.crash_reason,
                            s.crash_thread,
                            s.crash_pc,
                            s.crash_lr,
                            s.crash_xpsr,
                            s.crash_psp,
                            s.crash_msp,
                            s.crash_cfsr,
                            s.crash_hfsr,
                            s.crash_mmfar,
                            s.crash_bfar,
                            s.crash_r0,
                            s.crash_r1,
                            s.crash_r2,
                            s.crash_r3,
                            s.crash_r12,
                            s.crash_backtrace,
                            s.crash_backtrace_len);
    } else {
        shell_print(sh, "No retained fatal crash dump from previous boot.");
    }
    return 0;
}

SHELL_CMD_REGISTER(crash, NULL, "Print retained previous-boot crash dump and reset breadcrumbs",
                   cmd_crash_report);

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
            g_retained.crash.checksum == compute_crash_checksum(g_retained.crash)) {
            g_snapshot.retained_crash_valid = true;
            g_snapshot.crash_reason = g_retained.crash.reason;
            g_snapshot.crash_pc = g_retained.crash.pc;
            g_snapshot.crash_lr = g_retained.crash.lr;
            g_snapshot.crash_xpsr = g_retained.crash.xpsr;
            g_snapshot.crash_psp = g_retained.crash.psp;
            g_snapshot.crash_msp = g_retained.crash.msp;
            g_snapshot.crash_cfsr = g_retained.crash.cfsr;
            g_snapshot.crash_hfsr = g_retained.crash.hfsr;
            g_snapshot.crash_mmfar = g_retained.crash.mmfar;
            g_snapshot.crash_bfar = g_retained.crash.bfar;
            g_snapshot.crash_r0 = g_retained.crash.r0;
            g_snapshot.crash_r1 = g_retained.crash.r1;
            g_snapshot.crash_r2 = g_retained.crash.r2;
            g_snapshot.crash_r3 = g_retained.crash.r3;
            g_snapshot.crash_r12 = g_retained.crash.r12;
            g_snapshot.crash_backtrace_len =
                (g_retained.crash.backtrace_len <= kMaxBacktraceFrames)
                    ? g_retained.crash.backtrace_len
                    : static_cast<uint8_t>(kMaxBacktraceFrames);
            std::memcpy(g_snapshot.crash_backtrace,
                        g_retained.crash.backtrace,
                        sizeof(g_snapshot.crash_backtrace));
            std::memcpy(g_snapshot.crash_thread,
                        g_retained.crash.thread,
                        sizeof(g_snapshot.crash_thread));
            g_snapshot.crash_thread[kMaxThreadNameLen - 1U] = '\0';
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

void print_boot_report() {
    const Snapshot& s = g_snapshot;
    const bool hw_dog_or_lockup =
        (s.resetreas & (POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk)) != 0U;

    if (!s.retained_crash_valid && !s.retained_watchdog_valid && !hw_dog_or_lockup) {
        return;
    }

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
        LOG_ERR("Previous-boot fatal fault: reason=%u (%s) thread=%s PC=0x%08x LR=0x%08x CFSR=0x%08x HFSR=0x%08x",
                static_cast<unsigned int>(s.crash_reason),
                fatal_reason_str(s.crash_reason),
                s.crash_thread[0] != '\0' ? s.crash_thread : "<unknown>",
                static_cast<unsigned int>(s.crash_pc),
                static_cast<unsigned int>(s.crash_lr),
                static_cast<unsigned int>(s.crash_cfsr),
                static_cast<unsigned int>(s.crash_hfsr));
        print_crash_details("PREVIOUS-BOOT CRASH DUMP",
                            s.crash_reason,
                            s.crash_thread,
                            s.crash_pc,
                            s.crash_lr,
                            s.crash_xpsr,
                            s.crash_psp,
                            s.crash_msp,
                            s.crash_cfsr,
                            s.crash_hfsr,
                            s.crash_mmfar,
                            s.crash_bfar,
                            s.crash_r0,
                            s.crash_r1,
                            s.crash_r2,
                            s.crash_r3,
                            s.crash_r12,
                            s.crash_backtrace,
                            s.crash_backtrace_len);
    }
}

void schedule_boot_report(uint32_t delay_ms) {
    if (!g_boot_report_work_inited) {
        k_work_init_delayable(&g_boot_report_work, boot_report_work_handler);
        g_boot_report_work_inited = true;
    }
    k_work_reschedule(&g_boot_report_work, K_MSEC(delay_ms));
}

static_assert(std::is_trivial_v<RetainedBreadcrumb>,
              "RetainedBreadcrumb must remain trivial to preserve __noinit SRAM across resets");

void handle_fatal_error(unsigned int reason, const struct arch_esf* esf) {
    if (g_in_fatal_handler) {
        feed_hardware_wdt_all();
        sys_reboot(SYS_REBOOT_COLD);
        for (;;) {
        }
    }
    g_in_fatal_handler = true;

    feed_hardware_wdt_all();

    struct k_thread* const cur = k_current_get();
    const char* const tname = (cur != nullptr) ? k_thread_name_get(cur) : nullptr;

    uint32_t psp = __get_PSP();
    const uint32_t msp = __get_MSP();
#if defined(CONFIG_EXTRA_EXCEPTION_INFO)
    if (esf != nullptr && esf->extra_info.callee != nullptr && esf->extra_info.callee->psp != 0U) {
        psp = esf->extra_info.callee->psp;
    }
#endif

    RetainedCrash crash{};
    crash.magic = kCrashMagic;
    crash.reason = static_cast<uint32_t>(reason);
    crash.psp = psp;
    crash.msp = msp;
    crash.cfsr = SCB->CFSR;
    crash.hfsr = SCB->HFSR;
    crash.mmfar = SCB->MMFAR;
    crash.bfar = SCB->BFAR;
    if (esf != nullptr) {
        crash.pc = esf->basic.pc;
        crash.lr = esf->basic.lr;
        crash.xpsr = esf->basic.xpsr;
        crash.r0 = esf->basic.r0;
        crash.r1 = esf->basic.r1;
        crash.r2 = esf->basic.r2;
        crash.r3 = esf->basic.r3;
        crash.r12 = esf->basic.r12;
    }
    if (tname != nullptr) {
        std::strncpy(crash.thread, tname, kMaxThreadNameLen - 1U);
        crash.thread[kMaxThreadNameLen - 1U] = '\0';
    }
    capture_stack_backtrace(esf, psp, msp, cur, crash);
    crash.checksum = compute_crash_checksum(crash);

    /* Persist into __noinit SRAM while preserving prior progress/watchdog markers */
    g_retained.magic = kRetainedMagic;
    g_retained.crash = crash;
    g_retained.flags |= kFlagCrashValid;
    ++g_retained.sequence;

    print_crash_details("SP-1 FATAL CRASH DUMP",
                        crash.reason,
                        crash.thread,
                        crash.pc,
                        crash.lr,
                        crash.xpsr,
                        crash.psp,
                        crash.msp,
                        crash.cfsr,
                        crash.hfsr,
                        crash.mmfar,
                        crash.bfar,
                        crash.r0,
                        crash.r1,
                        crash.r2,
                        crash.r3,
                        crash.r12,
                        crash.backtrace,
                        crash.backtrace_len);

    LOG_PANIC();

    printk("Flushing USB-C CDC ACM console for %u ms before cold reboot...\r\n",
           static_cast<unsigned int>(kFatalFlushTotalMs));

    /* Briefly unlock IRQs during the 1,500 ms wait so the nRF52840 USBD
     * endpoint interrupt (and system workqueue when in thread context) can
     * drain the CDC ACM TX ring buffer completely to the host terminal. */
    feed_hardware_wdt_all();
    irq_unlock(0U);

    const uint32_t steps = kFatalFlushTotalMs / kFatalFlushStepMs;
    const bool in_isr = k_is_in_isr();
    for (uint32_t i = 0U; i < steps; ++i) {
        feed_hardware_wdt_all();
        if (in_isr) {
            k_busy_wait(kFatalFlushStepMs * 1000U);
        } else {
            k_msleep(static_cast<int32_t>(kFatalFlushStepMs));
        }
    }

    (void)irq_lock();
    feed_hardware_wdt_all();
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
    }
}

}  // namespace core::reset_breadcrumbs

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf* esf) {
    core::reset_breadcrumbs::handle_fatal_error(reason, esf);
}

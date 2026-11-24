#include "PowerManager.hpp"
#include "system/ResetBreadcrumbs.hpp"
#include "system/Watchdog.hpp"
#include "charger/charger_bq24232.h"
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <hal/nrf_temp.h>
#include <algorithm>
#include <cmath>

LOG_MODULE_REGISTER(power_manager, LOG_LEVEL_INF);

namespace {

constexpr int64_t kPowerPollIntervalMs = 50;
constexpr int32_t kBatteryEmptyMv = 3450;
constexpr int32_t kBatteryFullMv = 4180;
constexpr uint32_t kTempEventTimeoutUs = 1000U;

const struct device *const kChargerDev = DEVICE_DT_GET(DT_NODELABEL(bq24232));

bool charger_ready() {
    return device_is_ready(kChargerDev);
}

bool charger_set_enabled(bool enable) {
    return charger_charge_enable(kChargerDev, enable) == 0;
}

bool charger_sample_charge_active() {
    union charger_propval val = {};
    if (charger_get_prop(kChargerDev, CHARGER_PROP_STATUS, &val) < 0) {
        return false;
    }
    return val.status == CHARGER_STATUS_CHARGING;
}

float battery_level_from_mv(int32_t batt_mv) {
    const float normalized = static_cast<float>(batt_mv - kBatteryEmptyMv) /
                             static_cast<float>(kBatteryFullMv - kBatteryEmptyMv);
    return std::clamp(normalized, 0.0f, 1.0f);
}

int32_t sample_die_temperature_milli_c() {
    nrf_temp_event_clear(NRF_TEMP, NRF_TEMP_EVENT_DATARDY);
    nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_START);
    for (uint32_t elapsed = 0; elapsed < kTempEventTimeoutUs; ++elapsed) {
        if (nrf_temp_event_check(NRF_TEMP, NRF_TEMP_EVENT_DATARDY)) {
            nrf_temp_event_clear(NRF_TEMP, NRF_TEMP_EVENT_DATARDY);
            const int32_t quarter_c = nrf_temp_result_get(NRF_TEMP);
            nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_STOP);
            return quarter_c * 250;
        }
        k_busy_wait(1);
    }
    nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_STOP);
    return 25000;
}

}  // namespace

namespace power {

void PowerManager::init() {
    LOG_INF("Initializing Power Monitor (BQ24232)");

    core::reset_breadcrumbs::capture_early();
    uint32_t resetreas = core::reset_breadcrumbs::snapshot().resetreas;
    const uint32_t live_resetreas = NRF_POWER->RESETREAS;
    if (live_resetreas != 0U) {
        resetreas |= live_resetreas;
        NRF_POWER->RESETREAS = live_resetreas;
    }

    const bool reset_pin = (resetreas & POWER_RESETREAS_RESETPIN_Msk) != 0U;
    const bool dog_wdt   = (resetreas & POWER_RESETREAS_DOG_Msk) != 0U;
    const bool sreq_soft = (resetreas & POWER_RESETREAS_SREQ_Msk) != 0U;
    const bool lockup    = (resetreas & POWER_RESETREAS_LOCKUP_Msk) != 0U;
    const bool woke_gpio = (resetreas & POWER_RESETREAS_OFF_Msk) != 0U;
    const bool woke_vbus = (resetreas & POWER_RESETREAS_VBUS_Msk) != 0U;

    LOG_INF("Boot RESETREAS=0x%08x [DOG=%u(0x02:wdt) SREQ=%u(0x04:sys_reboot/fault) LOCKUP=%u(0x08:lockup) OFF=%u(0x10000:gpio) VBUS=%u RESETPIN=%u]",
            static_cast<unsigned int>(resetreas),
            dog_wdt ? 1U : 0U,
            sreq_soft ? 1U : 0U,
            lockup ? 1U : 0U,
            woke_gpio ? 1U : 0U,
            woke_vbus ? 1U : 0U,
            reset_pin ? 1U : 0U);

    if (dog_wdt) {
        LOG_WRN("Reset cause: DOG (0x00000002 = hardware watchdog timeout)");
    }
    if (sreq_soft) {
        LOG_WRN("Reset cause: SREQ (0x00000004 = sys_reboot / fault handler)");
    }
    if (lockup) {
        LOG_WRN("Reset cause: LOCKUP (0x00000008 = CPU hardfault lockup)");
    }
    woke_from_system_off_.store(woke_gpio, std::memory_order_relaxed);
    if (woke_gpio) {
        LOG_INF("Wakeup from System OFF by GPIO (RESETREAS=0x%08x)", resetreas);
    }

    if (gpio_is_ready_dt(&function_button_)) {
        (void)gpio_pin_configure_dt(&function_button_, GPIO_INPUT);
    }

    if (charger_ready()) {
        charger_enabled_state_.store(charger_set_enabled(true), std::memory_order_relaxed);
        LOG_INF("Charger enable asserted");
    } else {
        LOG_WRN("Charger driver not ready");
    }

    refresh_usb_power_state();
    charging_state_.store(charger_sample_charge_active(), std::memory_order_relaxed);
    die_temperature_milli_c_.store(sample_die_temperature_milli_c(), std::memory_order_relaxed);
    last_poll_ms_.store(0U, std::memory_order_relaxed);
}

bool PowerManager::woke_from_system_off() const {
    return woke_from_system_off_.load(std::memory_order_relaxed);
}

bool PowerManager::consume_system_off_gpio_wake() {
    const uint32_t resetreas = NRF_POWER->RESETREAS;
    if (resetreas != 0U) {
        NRF_POWER->RESETREAS = resetreas;
        if ((resetreas & POWER_RESETREAS_OFF_Msk) != 0U) {
            woke_from_system_off_.store(true, std::memory_order_relaxed);
        }
    }
    return woke_from_system_off_.exchange(false, std::memory_order_relaxed);
}

float PowerManager::get_battery_level() const {
    return battery_level_.load(std::memory_order_relaxed);
}

bool PowerManager::is_charging() const {
    return charging_state_.load(std::memory_order_relaxed);
}

bool PowerManager::is_external_power_present() const {
    return external_power_state_.load(std::memory_order_relaxed);
}

void PowerManager::refresh_usb_power_state() {
    const bool vbus_det = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
    bool charger_online = false;
    if (charger_ready()) {
        union charger_propval val = {};
        if (charger_get_prop(kChargerDev, CHARGER_PROP_ONLINE, &val) == 0) {
            charger_online = (val.online != CHARGER_ONLINE_OFFLINE);
        }
    }
    external_power_state_.store(vbus_det || charger_online, std::memory_order_relaxed);
}

void PowerManager::service_tick() {
    const uint32_t now_ms = k_uptime_get_32();
    const uint32_t last_ms = last_poll_ms_.load(std::memory_order_relaxed);
    if (last_ms != 0U && (now_ms - last_ms) < static_cast<uint32_t>(kPowerPollIntervalMs)) {
        return;
    }
    run_poll();
}

void PowerManager::feed_watchdog() const {
    core::watchdog::feed();
}

void PowerManager::run_poll() {
    refresh_usb_power_state();

    const bool charging_now = charger_sample_charge_active();
    const int32_t temp_x1000 = sample_die_temperature_milli_c();
    die_temperature_milli_c_.store(temp_x1000, std::memory_order_relaxed);

    // Charger thermal management: disable above 45C, normal below 15C.
    bool charger_enabled = charger_enabled_state_.load(std::memory_order_relaxed);
    if (temp_x1000 > 45000 && charger_enabled) {
        charger_enabled = !charger_set_enabled(false);
    } else if (temp_x1000 <= 14999 && !charger_enabled) {
        charger_enabled = charger_set_enabled(true);
    }
    charger_enabled_state_.store(charger_enabled, std::memory_order_relaxed);

    const bool prev_charging = charging_state_.exchange(charging_now, std::memory_order_relaxed);
    if (prev_charging != charging_now) {
        LOG_INF("Charging: %s", charging_now ? "yes" : "no");
    }

    last_poll_ms_.store(k_uptime_get_32(), std::memory_order_relaxed);
}

bool PowerManager::function_button_pressed() const {
    if (!gpio_is_ready_dt(&function_button_)) {
        return false;
    }
    const int value = gpio_pin_get_dt(&function_button_);
    return value > 0;
}

static void drive_pin_low(NRF_GPIO_Type* port, uint32_t pin) {
    port->OUTCLR = (1UL << pin);
    port->PIN_CNF[pin] =
        (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
    port->OUTCLR = (1UL << pin);
}

void PowerManager::prepare_for_system_off() {
    k_sleep(K_MSEC(2));

    // Hold external audio codecs in reset, cut 3.072 MHz osc, eMMC VCCQ, and BTN_COM ladder rail,
    // and keep BQ24232 nCE low so battery charging works in SYSTEM_OFF.
    drive_pin_low(NRF_P0, 9U);   // TAS2505 reset (active-low)
    drive_pin_low(NRF_P0, 15U);  // CS42L42 reset (active-low)
    drive_pin_low(NRF_P0, 13U);  // 3.072 MHz audio oscillator enable
    drive_pin_low(NRF_P0, 14U);  // eMMC VCCQ supply rail
    drive_pin_low(NRF_P0, 21U);  // BQ24232 nCE (low = charging enabled)
    drive_pin_low(NRF_P1, 10U);  // BTN_COM ladder & fader supply rail

    // Clear RESETREAS immediately before entering SYSTEM_OFF so stale flags never accumulate.
    NRF_POWER->RESETREAS = 0xFFFFFFFFU;

    // Arm Function button (P0.27 active-low with pull-up) as SENSE_Low SYSTEM_OFF wake source.
    if (gpio_is_ready_dt(&function_button_)) {
        (void)gpio_pin_configure_dt(&function_button_, GPIO_INPUT);
        (void)gpio_pin_interrupt_configure_dt(&function_button_, GPIO_INT_LEVEL_ACTIVE);
    }
    NRF_P0->PIN_CNF[27U] =
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos);
}

} // namespace power

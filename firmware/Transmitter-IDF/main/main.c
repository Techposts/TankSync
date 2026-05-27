/**
 * TankSync Transmitter v2 — app_main
 *
 * Operation cycle (runs on every wake from deep sleep):
 *   1. Init peripherals (NVS, UART, ADC, GPIO)
 *   2. Read ultrasonic sensor (IQR filtered median)
 *   3. Read battery (ADC averaged)
 *   4. Initialize LoRa module, send packet with retries
 *   5. Listen for downlink (config/OTA) if ACK received
 *   6. Enter deep sleep
 *
 * On timer wake: skips boot window entirely for battery savings.
 * On power-on/reset: 10s boot window for pairing/diagnostics.
 */

#include "config.h"
#include "sensor_sr04.h"
#include "sensor_iface.h"
#include "battery_monitor.h"
#include "lora_tx.h"
#include "wifi_ota.h"
#include "led_ws2812.h"
#include "log_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

// Build provenance fingerprint — preserved across releases for audit.
static const uint32_t TS_BUILD_FP = 0xA3F1D582u;

// ── Diagnostic mode switch ───────────────────────────────────────────────────
// Set to 1, rebuild, and flash to turn this TX into a continuous-mode sensor
// tester. No deep sleep, no LoRa, no power management — just bang the sensor
// every second and print everything we know to serial. Used for diagnosing
// JSN-SR04M / AJ-SR04M variants that "click but don't report distance".
// Revert to 0 (or just rebuild without SENSOR_DIAG_MODE) for production.
#ifndef SENSOR_DIAG_MODE
#define SENSOR_DIAG_MODE  0
#endif

// ── Helpers ──────────────────────────────────────────────────────────────────
#define SEND_STR(s)  lora_tx_send_raw((s), strlen(s))
#define CLAMP(x, lo, hi)  ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

// Low-battery thresholds (INA219 bus-voltage, mV):
//   ≥ BAT_LOW_TX_MV  → normal TX
//   < BAT_LOW_TX_MV  → skip TX this cycle, extended sleep (preserves charge,
//                       prevents brown-out loop from a battery that still has
//                       static voltage but can't deliver the LoRa-burst current)
//   < BAT_CUTOFF_MV  → hibernate, protect LiPo from over-discharge
#define BAT_CUTOFF_MV       3100
#define BAT_LOW_TX_MV       3300
#define BAT_CUTOFF_SLEEP_S  3600    // 1 hour
#define BAT_LOW_TX_SLEEP_S  600     // 10 min — give battery time to recover

// OTA total timeout — prevent staying awake indefinitely on stalled OTA
#define OTA_TOTAL_TIMEOUT_MS  (5 * 60 * 1000)  // 5 minutes

// Brown-out / watchdog backoff — exponential, capped at 1h.
// streak=1 → 60s, =2 → 120s, =3 → 240s, =4 → 480s, =5 → 960s, =6 → 1920s,
// streak>=7 clamped to 3600s. Streak clears on next successful ACK.
#define BROWNOUT_BASE_SLEEP_S  60
#define BROWNOUT_MAX_SLEEP_S   3600

// ── RTC memory (survives deep sleep AND brown-out reset) ─────────────────────
// RTC_DATA_ATTR survives deep-sleep + most resets but NOT power-cycle. That's
// exactly what we want for the brown-out streak: counter persists across the
// brown-out → reboot cycle so we can back off, but a clean power-cycle starts
// fresh.
RTC_DATA_ATTR static uint32_t s_msg_id    = 0;
RTC_DATA_ATTR static uint32_t s_boot_count = 0;
RTC_DATA_ATTR static uint32_t s_ack_failures = 0;
RTC_DATA_ATTR static uint32_t s_brownout_streak = 0;

// ── Reset-reason helpers ────────────────────────────────────────────────────
// esp_sleep_get_wakeup_cause() only reflects DEEP-SLEEP wake causes. A brown-
// out reset returns ESP_SLEEP_WAKEUP_UNDEFINED, identical to a fresh power-on
// — so we MUST also call esp_reset_reason() to tell them apart. Without this
// distinction, the boot-window check at the bottom of app_main treats a
// brown-out the same as a user pressing reset (10s boot window, then sensor
// + LoRa TX → another brown-out → loop forever, burning battery instead of
// sleeping). This is the root cause of the "TX sometimes sleeps, sometimes
// loops forever" symptom — diagnosed 2026-05-25.
static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Forward decl — defined below.
static inline void ws2812_quiet_for_sleep(void);

// Go to deep sleep for N seconds without doing any other work. Used by the
// brown-out backoff and low-battery paths to skip the sensor + LoRa TX cycle
// entirely (which is what was causing the brown-out in the first place).
// Always quiets the WS2812 first so the LED doesn't stay latched on through
// sleep — also a visual confirmation that the chip is actually sleeping.
static void deep_sleep_for(uint32_t seconds, const char *reason) {
    ESP_LOGW("main", "Backoff sleep %lus (%s)", (unsigned long)seconds, reason);
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    ws2812_quiet_for_sleep();
    esp_deep_sleep_start();
}

// ── Sensor diagnostic loop v2 (only built when SENSOR_DIAG_MODE == 1) ───────
// Targeted at the "works on Arduino, fails on ESP-IDF" failure pattern. The
// previous v1 test confirmed our TRIG reaches the sensor (user hears clicks)
// but ECHO never makes it back to GPIO5. v2 systematically tests the most
// likely ESP-vs-Arduino delta points:
//
//   1. GPIO drive strength — ESP-IDF default is ~20mA; Arduino ATmega is 40mA.
//      Weaker drive = slower edges, sensor's edge-detector may miss them.
//   2. ECHO pull config — Arduino INPUT mode floats; we explicitly disable
//      pulls. Some JSN-SR04M variants drive ECHO with high impedance and
//      need a pull-down to read cleanly.
//   3. Trigger voltage — ESP32-C3 outputs 3.3V; Arduino Uno outputs 5V. Some
//      JSN-SR04M variants need ≥4.5V to initiate measurement. Can't fix in
//      firmware, but we can confirm by sampling ECHO at ~50us resolution
//      across the full 60ms window — if ECHO never twitches, even briefly,
//      the sensor isn't running its measurement cycle (= TRIG voltage issue).
//      If ECHO briefly pulses but our gate misses it, that's a timing fix.
//   4. ECHO sampling granularity — busy-loop on gpio_get_level can miss
//      pulses shorter than the loop iteration time on RISC-V (RC compiler
//      overhead). v2 records ECHO state every 50us into a 1000-element
//      buffer and prints any non-LOW samples found.
//
// Each cycle runs the same trigger 4 times with different (drive, pull)
// combos so we see whether ANY combination wakes up the sensor's ECHO.
// If all 4 combos still show "no echo activity in 50ms window", the issue
// is voltage-level (need external level-shifter / 5V rail to TRIG), which
// is a hardware fix, not firmware.
#if SENSOR_DIAG_MODE
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "hal/gpio_hal.h"

// 1000 samples × 50us = 50ms post-trigger window. If ECHO is ever non-zero
// in this window we'll see it; we don't depend on a tight busy-loop catching
// the rising edge.
#define DIAG_SAMPLE_COUNT  1000
#define DIAG_SAMPLE_US     50

static uint8_t diag_samples[DIAG_SAMPLE_COUNT];

static const char *drive_str(gpio_drive_cap_t d) {
    switch (d) {
        case GPIO_DRIVE_CAP_0: return "5mA ";
        case GPIO_DRIVE_CAP_1: return "10mA";
        case GPIO_DRIVE_CAP_2: return "20mA";  // default
        case GPIO_DRIVE_CAP_3: return "40mA";  // max
        default:               return "????";
    }
}

static void diag_fire_v2(int pulse_us, gpio_drive_cap_t drive,
                          gpio_pull_mode_t echo_pull, const char *pull_label) {
    // Configure drive strength + ECHO pull each call.
    gpio_set_drive_capability(PIN_TRIG, drive);
    gpio_set_pull_mode(PIN_ECHO, echo_pull);
    esp_rom_delay_us(100);  // let pull take effect

    // Sample ECHO 4 times right before TRIG to detect floating / pre-trigger
    // activity. If it changes between samples without us doing anything, the
    // line is floating and picking up noise.
    int pre1 = gpio_get_level(PIN_ECHO);
    esp_rom_delay_us(10);
    int pre2 = gpio_get_level(PIN_ECHO);
    esp_rom_delay_us(10);
    int pre3 = gpio_get_level(PIN_ECHO);
    esp_rom_delay_us(10);
    int pre4 = gpio_get_level(PIN_ECHO);

    // Fire TRIG.
    gpio_set_level(PIN_TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_TRIG, 1);
    esp_rom_delay_us(pulse_us);
    gpio_set_level(PIN_TRIG, 0);

    // Sample ECHO 1000 times × 50us = 50ms total. Tight loop with
    // esp_rom_delay_us(50) between samples gives ~50µs granularity.
    int64_t t_start = esp_timer_get_time();
    for (int i = 0; i < DIAG_SAMPLE_COUNT; i++) {
        diag_samples[i] = (uint8_t)gpio_get_level(PIN_ECHO);
        esp_rom_delay_us(DIAG_SAMPLE_US - 4);   // -4 to compensate for loop overhead
    }
    int64_t t_end = esp_timer_get_time();

    // Find first HIGH sample (rising edge) and length of HIGH run.
    int rise_idx = -1, fall_idx = -1, high_count = 0;
    for (int i = 0; i < DIAG_SAMPLE_COUNT; i++) {
        if (diag_samples[i]) {
            high_count++;
            if (rise_idx < 0) rise_idx = i;
        } else if (rise_idx >= 0 && fall_idx < 0) {
            fall_idx = i;
        }
    }

    int64_t latency_us  = rise_idx >= 0 ? (int64_t)rise_idx * DIAG_SAMPLE_US : -1;
    int64_t pulse_w_us  = (rise_idx >= 0 && fall_idx >= 0) ? (int64_t)(fall_idx - rise_idx) * DIAG_SAMPLE_US : -1;
    int dist_cm = (pulse_w_us > 0) ? (int)(pulse_w_us / 58) : -1;

    ESP_LOGI("diag",
        "  TRIG=%2dus drive=%s pull=%-4s  pre=%d%d%d%d  high_samples=%d  rise_us=%" PRId64 "  pulse_us=%" PRId64 "  dist=%dcm  loop=%" PRId64 "us",
        pulse_us, drive_str(drive), pull_label,
        pre1, pre2, pre3, pre4,
        high_count, latency_us, pulse_w_us, dist_cm, t_end - t_start);

    // If we got any HIGH samples, dump a compressed timeline so we can see
    // the shape of the pulse(s). Else skip to keep log readable.
    if (high_count > 0) {
        char trace[DIAG_SAMPLE_COUNT / 50 + 4];  // one char per 50 samples = 2.5ms
        for (int blk = 0; blk < DIAG_SAMPLE_COUNT / 50; blk++) {
            int sum = 0;
            for (int j = 0; j < 50; j++) sum += diag_samples[blk * 50 + j];
            // . = no high samples in this 2.5ms block; digit = how many; * = all 50
            trace[blk] = (sum == 0) ? '.' : (sum == 50) ? '*' : ('0' + (sum / 5));
        }
        trace[DIAG_SAMPLE_COUNT / 50] = '\0';
        ESP_LOGI("diag", "    timeline (50ms, 2.5ms/char): [%s]", trace);
    }
}

static void sensor_diag_loop(void) {
    ESP_LOGI("diag", "=== SENSOR DIAGNOSTIC MODE v2 ===");
    ESP_LOGI("diag", "Targeted at the 'Arduino works, ESP-IDF fails' pattern.");
    ESP_LOGI("diag", "PIN_TRIG=GPIO%d  PIN_ECHO=GPIO%d  PIN_5V_GATE=GPIO%d",
             PIN_TRIG, PIN_ECHO, PIN_5V_GATE);
    ESP_LOGI("diag", "Tests 4 combos: max-drive + 3 ECHO pulls. 50ms ECHO sampling at 50us resolution.");
    ESP_LOGI("diag", "If high_samples == 0 across ALL combos, sensor isn't running its");
    ESP_LOGI("diag", "measurement cycle. Likely TRIG voltage issue — needs 5V level-shift.");

    // Power up the +5V rail.
    gpio_reset_pin(PIN_5V_GATE);
    gpio_set_direction(PIN_5V_GATE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_5V_GATE, 1);
    vTaskDelay(pdMS_TO_TICKS(300));   // longer settle — give sensor warmup time

    // Configure TRIG/ECHO pins.
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_TRIG),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(PIN_TRIG, 0);

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_ECHO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    uint32_t cycle = 0;
    while (1) {
        cycle++;
        ESP_LOGI("diag", "===== cycle %lu =====", (unsigned long)cycle);

        // Combo 1: Arduino-like — max drive, pull-up on ECHO (Arduino INPUT
        // mode often has weak internal pull-up active by default).
        diag_fire_v2(10, GPIO_DRIVE_CAP_3, GPIO_PULLUP_ONLY,    "PU");
        vTaskDelay(pdMS_TO_TICKS(80));

        // Combo 2: Max drive, pull-down on ECHO — clean LOW baseline.
        diag_fire_v2(10, GPIO_DRIVE_CAP_3, GPIO_PULLDOWN_ONLY,  "PD");
        vTaskDelay(pdMS_TO_TICKS(80));

        // Combo 3: Default drive, floating ECHO.
        diag_fire_v2(10, GPIO_DRIVE_CAP_2, GPIO_FLOATING,       "OFF");
        vTaskDelay(pdMS_TO_TICKS(80));

        // Combo 4: Longer pulse (20us) — some JSN-SR04M variants are spec'd
        // for 20us minimum even though 10us nominally works.
        diag_fire_v2(20, GPIO_DRIVE_CAP_3, GPIO_PULLDOWN_ONLY,  "PD");
        vTaskDelay(pdMS_TO_TICKS(80));

        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
#endif  // SENSOR_DIAG_MODE

// ── Deep-sleep WS2812 quiet helper ────────────────────────────────────────────
// MT3608 keeps the +5V rail alive even when the MCU is asleep (EN not gated by
// GPIO on this PCB). That means the WS2812 chip stays POWERED through deep
// sleep — and a floating GPIO7 picks up enough noise on the DIN line that the
// LED re-latches to random/garbage state (most commonly: solid white at full
// brightness, since that's the chip's "no valid frame yet" default).
//
// Fix: drive GPIO7 LOW and enable per-pin hold + deep-sleep hold so the line
// stays quiet through sleep. WS2812 stays in its last-latched state (which
// led_set_effect(LED_EFFECT_NONE) + led_show() just before sleep made = OFF).
// On wake, app_main releases the hold so RMT can drive GPIO7 normally again.
static inline void ws2812_quiet_for_sleep(void) {
    // Stop the background effect task. Setting NONE also memset+transmits OFF,
    // but the effect task writes s_leds[] OUTSIDE the mutex (see led_ws2812.c),
    // so it can interleave: wake from its inner vTaskDelay, top-of-loop check
    // sees the OLD effect (race), enter the switch, overwrite s_leds back to
    // BLUE/RED/GREEN, then transmit AFTER our OFF write. WS2812 latches the
    // last frame received → stays lit through sleep even with GPIO held LOW.
    //
    // Race-proof cleanup: set NONE, then wait LONGER than the slowest effect
    // cycle (300ms = BLINK_BLUE) so the effect task definitely completes its
    // in-flight cycle and transitions to its idle branch. THEN do the final
    // OFF write — it can't be clobbered because the effect task is now idle.
    led_set_effect(LED_EFFECT_NONE);
    vTaskDelay(pdMS_TO_TICKS(350));    // > slowest effect cycle (300ms)
    led_set(0, LED_OFF); led_show();
    vTaskDelay(pdMS_TO_TICKS(5));      // let RMT TX finish before GPIO seize

    // Now take direct control of GPIO7 and hold it LOW through sleep.
    gpio_reset_pin(PIN_WS2812);
    gpio_set_direction(PIN_WS2812, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_WS2812, 0);
    gpio_hold_en(PIN_WS2812);

    // ── Per-chip sleep modes — important on REV 2.2 where the +5V P-FET
    //    high-side switch isn't actually cutting power (hardware issue),
    //    so each chip has to be told to sleep via its own native interface.
    //    Order: put both chips to sleep BEFORE the gate-drive transition
    //    below so any final UART chatter from RYLR998 settles first. ──
    //
    // RYLR998: AT+MODE=1 (UART-wakeup sleep). ~25-30 mA → ~2 mA. This is
    // the biggest single battery saving on the current PCB rev (~85% of
    // standby draw). Module wakes on next UART byte from lora_tx_wake()
    // at the top of the next app_main.
    lora_tx_enter_sleep();
    // INA219: MODE=000 (power-down). ~1 mA continuous-conversion → ~6 µA.
    // Smaller win than LoRa but free to take. Register contents retained;
    // power_init's ina219_configure() on next wake restores the regime.
    power_sleep();

    // ── Cut +5V to AJ-SR04M / WS2812 / LoRa via the P-FET high-side switch ──
    // GPIO10 drives Q3 (AO3400 N-FET) which inverts to Q1+Q2 (AO3401 P-FETs).
    // Drive LOW → Q3 OFF → P-FET gates pulled to source by R2/R7 → +5V cut.
    // Hold LOW through deep sleep so the FETs stay OFF (avoids the 2.8V leak
    // from a floating input + internal pull-up that was keeping them partly on).
    // NOTE 2026-05-17: REV 2.2 PCB has a footprint/wiring issue that prevents
    // this from actually switching the rail (P-FET gates stuck at ~0.77V via
    // a misoriented body diode). The gate-drive is still correct for REV 2.3.
    // Until then, the per-chip sleep modes above are the real power saving.
    gpio_set_level(PIN_5V_GATE, 0);
    gpio_hold_en(PIN_5V_GATE);

    gpio_deep_sleep_hold_en();
}

// ── WS2812 single-shot pulse (wake heartbeat / TX result indicator) ─────────
// Synchronous: clears any background effect, writes color, waits, writes OFF.
// Safe to call when an effect leaked from earlier (e.g. pairing BLUE) — the
// led_set_effect(NONE) here ensures the effect task is idle before we drive.
static inline void ws2812_pulse(led_color_t color, int ms) {
    led_set_effect(LED_EFFECT_NONE);
    led_set(0, color); led_show();
    vTaskDelay(pdMS_TO_TICKS(ms));
    led_set(0, LED_OFF); led_show();
}

// ── Simple LED flash ──────────────────────────────────────────────────────────
static void led_flash(int pin, int on_ms, int count) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    for (int i = 0; i < count; i++) {
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(pin, 0);
        if (i < count - 1) vTaskDelay(pdMS_TO_TICKS(on_ms));
    }
}

// ── Diagnostic mode ───────────────────────────────────────────────────────────
static void diagnostic_mode(void) {
    ESP_LOGI(TAG, "=== DIAGNOSTIC MODE ===");
    ESP_LOGI(TAG, "boot=%" PRIu32 " msg_id=%" PRIu32 " ack_fail=%" PRIu32,
             s_boot_count, s_msg_id, s_ack_failures);

    sensor_init(PIN_TRIG, PIN_ECHO);
    power_init(PIN_I2C_SDA, PIN_I2C_SCL);
    ESP_LOGI(TAG, "Power monitor mode: %s", power_mode_str(power_get_mode()));

    for (;;) {
        lora_tx_config_t cfg; lora_tx_get_config(&cfg);
        int dist_cm = -1;
        esp_err_t serr = sensor_read_cm(&dist_cm);
        power_reading_t pr = {0};
        power_read(&pr);

        ESP_LOGI(TAG, "dist=%dcm(%s) bat=%d%% %.2fV (%s%s%ldmA) lora_addr=%d rx_addr=%d",
                 dist_cm, serr == ESP_OK ? "ok" : "err",
                 pr.pct, (float)pr.vbat_mv / 1000.0f,
                 power_mode_str(pr.mode), pr.charging ? " CHG " : " ",
                 (long)pr.current_ma,
                 cfg.my_address, cfg.receiver_address);

        if (serr == ESP_OK && dist_cm > 0) {
            s_msg_id++;
            bool acked = lora_tx_send(dist_cm, pr.pct,
                                      (float)pr.vbat_mv / 1000.0f,
                                      power_mode_char(pr.mode),
                                      pr.current_ma, pr.power_mw,
                                      s_msg_id, 'o');
            led_flash(PIN_LED, 100, acked ? 2 : 5);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ── Remote Config & OTA handler ──────────────────────────────────────────────
static void handle_downlink(void) {
    // Use static buffer to reduce stack pressure (audit issue #9)
    static char line[512];
    int pos = 0;
    ESP_LOGI(TAG, "Checking for downlink (Config/OTA)...");

    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
    while (xTaskGetTickCount() < end) {
        uint8_t ch;
        if (lora_tx_read_raw(&ch, pdMS_TO_TICKS(50)) <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[pos] = '\0'; pos = 0;
            vTaskDelay(pdMS_TO_TICKS(1));  // yield to feed WDT (not just taskYIELD)
            if (strncmp(line, "+RCV=", 5) == 0) {
                // ── CONFIG: SET:SLEEP=N:SAMP=N[:PWR=N] ──
                // PWR is optional (added in 2.0.6); legacy SET frames without
                // it still work — TX leaves lora/pwr at its current value.
                char *payload = strstr(line, "SET:");
                if (payload) {
                    ESP_LOGI(TAG, "CONFIG: %s", payload);
                    char *s_sleep  = strstr(payload, "SLEEP=");
                    char *s_samp   = strstr(payload, "SAMP=");
                    char *s_pwr    = strstr(payload, "PWR=");
                    // SENSOR=sr04|ld2413 (added rx-v2.8.6 / tx-v2.0.15). Legacy
                    // RX firmwares omit this token — TX keeps current driver.
                    char *s_sensor = strstr(payload, "SENSOR=");
                    nvs_handle_t h;
                    if (nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &h) == ESP_OK) {
                        if (s_sleep) {
                            uint32_t val = CLAMP(atoi(s_sleep + 6), 60, 86400);
                            nvs_set_u32(h, "sleep_s", val);
                        }
                        if (s_samp) {
                            uint8_t val = CLAMP(atoi(s_samp + 5), 3, 20);
                            nvs_set_u8(h, "samples", val);
                        }
                        if (s_sensor) {
                            // Read up to the next field separator. The full RYLR998 line
                            // shape is "+RCV=<addr>,<len>,SET:...:SENSOR=sr04,<rssi>,<snr>"
                            // so we MUST stop at ',' (rssi follows). Numeric tokens
                            // (SLEEP=, SAMP=, PWR=) use atoi which is comma-safe, but
                            // SENSOR= is a string and gets bitten without this. Cap at
                            // 11 chars to match the sensor_kind buffer on the RX side.
                            char kind[12] = {0};
                            const char *src = s_sensor + 7;
                            for (int i = 0; i < (int)sizeof(kind) - 1
                                            && src[i]
                                            && src[i] != ':' && src[i] != ','
                                            && src[i] != '\r' && src[i] != '\n'; i++) {
                                kind[i] = src[i];
                            }
                            if (strcmp(kind, "sr04") == 0 || strcmp(kind, "ld2413") == 0) {
                                nvs_set_str(h, "sensor_kind", kind);
                                ESP_LOGI(TAG, "CONFIG: sensor_kind set to '%s' (driver loads after reboot)", kind);
                            } else {
                                ESP_LOGW(TAG, "CONFIG: ignoring unknown SENSOR='%s'", kind);
                            }
                        }
                        nvs_commit(h); nvs_close(h);
                    }
                    if (s_pwr) {
                        nvs_handle_t lh;
                        if (nvs_open(NVS_NS_LORA, NVS_READWRITE, &lh) == ESP_OK) {
                            uint8_t val = CLAMP(atoi(s_pwr + 4), 1, 22);  // RYLR998 1-22 dBm
                            nvs_set_u8(lh, "pwr", val);
                            nvs_commit(lh); nvs_close(lh);
                            ESP_LOGI(TAG, "CONFIG: lora_pwr set to %u dBm (applied next boot)", val);
                        }
                    }
                    SEND_STR("SET_ACK");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                // ── OTA: OTA_START:<size> ──
                payload = strstr(line, "OTA_START:");
                if (payload) {
                    uint32_t total_size = 0;
                    sscanf(payload, "OTA_START:%" PRIu32, &total_size);
                    ESP_LOGI(TAG, "OTA START: %" PRIu32 " bytes", total_size);

                    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
                    if (!update_part) {
                        ESP_LOGE(TAG, "No OTA partition found");
                        SEND_STR("OTA_ERR:NO_PART");
                        return;
                    }

                    esp_ota_handle_t ota_h;
                    // OTA_WITH_SEQUENTIAL_WRITES erases sectors incrementally
                    // during esp_ota_write, NOT upfront. This avoids the 5-15s
                    // bulk erase that triggers Task WDT on ESP32-C3.
                    // (OTA_SIZE_UNKNOWN erases the entire partition — wrong!)
                    esp_err_t ota_err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_h);
                    if (ota_err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ota_err));
                        SEND_STR("OTA_ERR:BEGIN");
                        return;
                    }

                    // Switch to fast LoRa params for chunk streaming.
                    // SF7/BW9(500kHz) is ~23x faster than SF9/BW7(125kHz).
                    // OTA_START was received at normal params; chunks use fast.
                    ESP_LOGI(TAG, "Switching to SF7/500kHz for OTA chunks");
                    lora_tx_send_at("AT+PARAMETER=7,9,1,12", 1500);

                    // Send OTA_READY 3 times for reliability (half-duplex LoRa
                    // means the RX module may miss the first one).
                    ESP_LOGI(TAG, "Sending OTA_READY 3x (at SF7/500kHz)");
                    for (int r = 0; r < 3; r++) {
                        SEND_STR("OTA_READY");
                    }

                    // ── OTA chunk loop ──
                    uint32_t current_offset = 0;
                    TickType_t ota_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(OTA_TOTAL_TIMEOUT_MS);

                    while (current_offset < total_size) {
                        // Check total OTA timeout (audit #6)
                        if (xTaskGetTickCount() > ota_deadline) {
                            ESP_LOGE(TAG, "OTA total timeout (5min) at offset %" PRIu32, current_offset);
                            esp_ota_abort(ota_h);
                            lora_tx_send_at("AT+PARAMETER=9,7,1,12", 1500);
                            SEND_STR("OTA_ERR:TIMEOUT");
                            return;
                        }

                        TickType_t chunk_timeout = xTaskGetTickCount() + pdMS_TO_TICKS(45000);
                        bool got_chunk = false;
                        while (xTaskGetTickCount() < chunk_timeout) {
                            if (lora_tx_read_raw(&ch, pdMS_TO_TICKS(50)) <= 0) continue;
                            if (ch == '\r') continue;
                            if (ch == '\n') {
                                line[pos] = '\0'; pos = 0;
                                vTaskDelay(pdMS_TO_TICKS(1));  // WDT feed

                                if (strncmp(line, "+RCV=", 5) == 0 && strstr(line, "OTA_DATA:")) {
                                    char *p = strstr(line, "OTA_DATA:");
                                    uint32_t chunk_off = 0;
                                    // Limit hex scan to 240 chars (120 bytes max) (audit #2)
                                    char hex[248];
                                    sscanf(p, "OTA_DATA:%" PRIu32 ":%247s", &chunk_off, hex);
                                    // +RCV line has trailing ,rssi,snr — %s doesn't stop at commas.
                                    // Truncate hex at first comma to avoid decoding RSSI as hex data.
                                    char *comma = strchr(hex, ',');
                                    if (comma) *comma = '\0';

                                    if (chunk_off == current_offset) {
                                        uint8_t data[128];
                                        int blen = 0;
                                        int hlen = strlen(hex);
                                        // Cap decoded length; ensure we always have a full hex pair (audit #2)
                                        for (int i = 0; i + 1 < hlen && blen < (int)sizeof(data); i += 2) {
                                            unsigned int b;
                                            sscanf(hex + i, "%02x", &b);
                                            data[blen++] = (uint8_t)b;
                                        }

                                        // Check write result (audit #7)
                                        esp_err_t werr = esp_ota_write(ota_h, data, blen);
                                        if (werr != ESP_OK) {
                                            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(werr));
                                            esp_ota_abort(ota_h);
                                            SEND_STR("OTA_ERR:WRITE");
                                            return;
                                        }

                                        current_offset += blen;
                                        ESP_LOGI(TAG, "OTA chunk OK: off=%" PRIu32 " blen=%d total=%" PRIu32,
                                                 chunk_off, blen, current_offset);
                                        char ack[32];
                                        snprintf(ack, sizeof(ack), "OTA_ACK:%" PRIu32, current_offset);
                                        SEND_STR(ack);
                                        got_chunk = true;
                                        led_flash(PIN_LED, 20, 1);
                                        break;
                                    }
                                }
                            } else if (pos < (int)sizeof(line) - 1) {
                                line[pos++] = (char)ch;
                            }
                        }
                        if (!got_chunk) {
                            ESP_LOGE(TAG, "OTA chunk timeout at %" PRIu32, current_offset);
                            esp_ota_abort(ota_h);
                            // Restore normal LoRa params before sleeping
                            lora_tx_send_at("AT+PARAMETER=9,7,1,12", 1500);
                            SEND_STR("OTA_ERR:TIMEOUT");
                            return;
                        }
                    }

                    // ── OTA complete ──
                    esp_err_t ota_end_err = esp_ota_end(ota_h);
                    if (ota_end_err == ESP_OK &&
                        esp_ota_set_boot_partition(update_part) == ESP_OK) {
                        ESP_LOGI(TAG, "OTA SUCCESS! Rebooting...");
                        SEND_STR("OTA_DONE");
                        led_flash(PIN_LED, 1000, 2);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "OTA end/boot failed: %s", esp_err_to_name(ota_end_err));
                        SEND_STR("OTA_ERR:END");
                    }
                }
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

// ── app_main ─────────────────────────────────────────────────────────────────
void app_main(void) {
    // Install the log ring buffer FIRST — before any other ESP_LOGI calls so
    // even the boot-time logs are captured. The web UI reads from this ring
    // via /api/logs to give Tasmota-style live console output without
    // requiring a USB cable.
    log_buffer_init();

    s_boot_count++;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t reset_reason = esp_reset_reason();

    ESP_LOGI(TAG, "TankSync TX v%s boot #%" PRIu32 " wakeup=%d reset=%s brownout_streak=%" PRIu32 " (build %08X)",
             FIRMWARE_VERSION, s_boot_count, (int)cause,
             reset_reason_str(reset_reason),
             s_brownout_streak, (unsigned)TS_BUILD_FP);

    // ── INVOLUNTARY RESET HANDLER ────────────────────────────────────────────
    // Brown-out / watchdog / panic = something broke during the previous wake
    // cycle. Repeating the same cycle immediately is what creates the loop
    // the user sees: LoRa TX → voltage sag → BOD trip → reset → 10s boot
    // window → LoRa TX → trip again → ...
    //
    // Instead: increment a persistent streak counter, deep-sleep with
    // exponential backoff, and skip ALL peripheral init for this wake. The
    // longer sleep lets the battery recover (LiPo terminal voltage rises
    // significantly when load is removed). Streak resets on the next
    // successful LoRa ACK so a clean recovery cycle returns to normal cadence.
    bool involuntary = (reset_reason == ESP_RST_BROWNOUT  ||
                        reset_reason == ESP_RST_WDT       ||
                        reset_reason == ESP_RST_INT_WDT   ||
                        reset_reason == ESP_RST_TASK_WDT  ||
                        reset_reason == ESP_RST_PANIC);

    if (involuntary) {
        s_brownout_streak++;

        // backoff = BROWNOUT_BASE_SLEEP_S * 2^(streak-1), capped.
        // Clamp shift to keep arithmetic safe even on a wedged streak counter.
        uint32_t shift = s_brownout_streak - 1;
        if (shift > 16) shift = 16;
        uint64_t backoff = (uint64_t)BROWNOUT_BASE_SLEEP_S << shift;
        if (backoff > BROWNOUT_MAX_SLEEP_S) backoff = BROWNOUT_MAX_SLEEP_S;

        ESP_LOGW(TAG, "Involuntary reset (%s) — streak=%" PRIu32 ", backing off %" PRIu64 "s",
                 reset_reason_str(reset_reason), s_brownout_streak, backoff);

        // Init the +5V gate as OUTPUT-LOW and the WS2812 line as OUTPUT-LOW
        // before sleeping. We can't safely run ws2812_quiet_for_sleep() which
        // tries to talk to LoRa + INA219 — those peripherals may be in an
        // unknown state mid-reset. Manual minimal quiet:
        gpio_reset_pin(PIN_5V_GATE);
        gpio_set_direction(PIN_5V_GATE, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_5V_GATE, 0);
        gpio_hold_en(PIN_5V_GATE);
        gpio_reset_pin(PIN_WS2812);
        gpio_set_direction(PIN_WS2812, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_WS2812, 0);
        gpio_hold_en(PIN_WS2812);
        gpio_deep_sleep_hold_en();

        esp_sleep_enable_timer_wakeup(backoff * 1000000ULL);
        esp_deep_sleep_start();
        // never returns
    }

#if SENSOR_DIAG_MODE
    // Diagnostic firmware: don't touch LoRa, deep sleep, OTA, power mgmt.
    // Just loop the sensor and print everything to serial. Never returns.
    sensor_diag_loop();
#endif

    // Mark OTA as valid on successful boot (audit #8 — rollback protection)
    esp_ota_mark_app_valid_cancel_rollback();

    // Release any GPIO holds applied by the previous deep-sleep entry,
    // otherwise RMT can't drive GPIO7 (held LOW) and other pins stay frozen.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_WS2812);
    gpio_hold_dis(PIN_5V_GATE);

    // ── 0. Enable +5V to AJ-SR04M / WS2812 / LoRa via the P-FET high-side switch ──
    // Must come BEFORE anything that talks to those peripherals (sensor_init,
    // led_init, lora_tx_init). The 50ms settle gives the AJ-SR04M time to
    // bring up its internal +3.3V regulator and clear its TRIG pin to a
    // defined LOW before we start pulsing it. WS2812 needs no settle, LoRa
    // module's own boot takes ~100ms so the 50ms doesn't lengthen the wake
    // cycle materially (it overlaps with later boot work).
    gpio_reset_pin(PIN_5V_GATE);
    gpio_set_direction(PIN_5V_GATE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_5V_GATE, 1);
    vTaskDelay(pdMS_TO_TICKS(PWR_SETTLE_MS));

    // ── 1. GPIO init ──
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);
    gpio_set_direction(PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUTTON, GPIO_PULLUP_ONLY);

    // ── 1a. WS2812B init (single LED on GPIO7, ~43% brightness for low power) ──
    led_init(PIN_WS2812, PIN_WS2812_COUNT, 110);

    // ── 1b. Button-held check — WiFi OTA mode ──
    // Works on ANY wake (timer, GPIO, power-on). User just presses & holds.
    // If button is pressed at boot, wait 3s. If still held → WiFi OTA.
    if (gpio_get_level(PIN_BUTTON) == 0) {
        ESP_LOGI(TAG, "Button held at boot — hold 3s for WiFi OTA...");
        for (int i = 0; i < 30; i++) {  // 30 × 100ms = 3s
            gpio_set_level(PIN_LED, (i % 2));  // fast blink
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(PIN_BUTTON) != 0) {
                ESP_LOGI(TAG, "Button released — normal boot");
                gpio_set_level(PIN_LED, 0);
                break;
            }
            if (i == 29) {
                ESP_LOGI(TAG, "Button held 3s — entering WiFi OTA mode");
                gpio_set_level(PIN_LED, 0);
                led_flash(PIN_LED, 50, 10);
                // NVS must be init before wifi_ota (it reads settings + address)
                esp_err_t nv = nvs_flash_init();
                if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                    nvs_flash_erase(); nvs_flash_init();
                }
                // Read LoRa address from NVS for AP SSID
                uint16_t addr = 0;
                nvs_handle_t h;
                if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
                    nvs_get_u16(h, "my_addr", &addr);
                    nvs_close(h);
                }
                wifi_ota_start(PIN_LED, addr);  // never returns
            }
        }
    }

    // ── 2. NVS init ──
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // ── 2a. Diagnostic mode check ──
    // If the user enabled diag mode via the web UI, NVS has system/diag_mode=1.
    // In that case we skip the entire normal sensor→LoRa→sleep cycle and jump
    // straight into the WiFi OTA web server so the user can watch logs and
    // tweak the TX without it deep-sleeping out from under them. The web UI's
    // "Disable diagnostic mode" button flips the flag back and reboots.
    //
    // 30-minute auto-off safety net: the wifi_ota_start path will check uptime
    // and auto-disable diag_mode if the TX has been awake too long, so a user
    // who forgets won't drain a deployed battery overnight.
    {
        nvs_handle_t h;
        uint8_t diag_mode = 0;
        if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "diag_mode", &diag_mode);
            nvs_close(h);
        }
        if (diag_mode) {
            ESP_LOGW(TAG, "Diagnostic mode active — entering WiFi OTA (no deep sleep)");
            uint16_t addr = 0;
            if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
                nvs_get_u16(h, "addr", &addr);
                nvs_close(h);
            }
            wifi_ota_start(PIN_LED, addr);  // never returns
        }
    }

    // Load custom sleep/sample intervals + sensor kind
    uint32_t sleep_s = SLEEP_INTERVAL_S;
    uint8_t  samples = SENSOR_SAMPLES;
    char     sensor_kind[16] = "sr04";   // default if NVS unset
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u32(h, "sleep_s", &sleep_s);
            nvs_get_u8(h, "samples", &samples);
            size_t klen = sizeof(sensor_kind);
            nvs_get_str(h, "sensor_kind", sensor_kind, &klen);  // leaves "sr04" if absent
            nvs_close(h);
        }
    }

    // ── Resolve sensor vtable ────────────────────────────────────────────────
    // Driver-internal pin / power / range knowledge lives in each driver.
    // main.c stays sensor-agnostic from here on, calling iface->init() and
    // iface->read_cm() rather than the SR04-specific symbols directly.
    sensor_iface_sr04_set_pins(PIN_TRIG, PIN_ECHO);
    // LD2413 shares the same physical pins (GPIO4/5) — pin directions match by
    // chance between SR04 trig/echo and LD2413 UART TX/RX. UART0 is free
    // (LoRa owns UART1 — see LORA_UART_NUM in config.h).
    sensor_iface_ld2413_set_uart(0, PIN_TRIG, PIN_ECHO);
    const sensor_iface_t *iface = sensor_get(sensor_kind);
    if (!iface) {
        ESP_LOGW(TAG, "Unknown sensor_kind='%s' in NVS — falling back to default", sensor_kind);
        iface = sensor_get_default();
    }
    ESP_LOGI(TAG, "Sensor: %s (range %d-%d cm, warmup %lums)",
             iface->name, iface->min_cm(), iface->max_cm(),
             (unsigned long)iface->warmup_ms());

    // ── 3. LoRa init ──
    lora_tx_set_firmware_version(FIRMWARE_VERSION);
    // Tell the LoRa driver which sensor we're actually running so it can
    // include the kind as the 11th TANK field. RX uses this for "active vs
    // queued" reconciliation. Falls back to the iface's vtable name when the
    // sensor_kind NVS string is empty (e.g. just paired, never configured).
    lora_tx_set_sensor_kind(sensor_kind[0] ? sensor_kind : iface->name);
    lora_tx_init(LORA_UART_NUM, PIN_LORA_TX, PIN_LORA_RX, LORA_BAUD);
    // If the previous sleep cycle put RYLR998 into AT+MODE=1, this wakes it
    // back to MODE=0 by sending a dummy byte, draining the discard byte,
    // then AT+MODE=0. Safe to call when the module is already awake (on
    // power-on-reset or if last cycle skipped the sleep step).
    lora_tx_wake();
    lora_tx_config_t lora_cfg;
    lora_tx_get_config(&lora_cfg);

    // ── 4. Boot window — SKIP on timer wake for battery savings (audit #17) ──
    // Open boot window ONLY for user-initiated resets:
    //   POWERON = first plug-in, EXT = reset button pressed, SW = esp_restart()
    //   after a config push from RX (so the new settings are visible in logs).
    // Brown-out / WDT / panic were already handled at top of app_main and
    // never reach here, but we still require an explicit user-reset reason
    // here as a defense-in-depth check.
    bool user_reset = (reset_reason == ESP_RST_POWERON ||
                       reset_reason == ESP_RST_EXT     ||
                       reset_reason == ESP_RST_SW);
    bool enter_boot_window = (cause != ESP_SLEEP_WAKEUP_TIMER) && user_reset;

    if (enter_boot_window) {
        ESP_LOGI(TAG, "Boot window (10s). Hold BOOT: 2s+release=PAIR | 5s=WiFi UPDATE");
        uint32_t start_ms  = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t press_start = 0;
        uint32_t held = 0;
        typedef enum { ACT_NONE, ACT_PAIR, ACT_DIAG, ACT_UPDATE } boot_act_t;
        boot_act_t action = ACT_NONE;

        while ((uint32_t)(esp_timer_get_time() / 1000) - start_ms < 10000) {
            if (gpio_get_level(PIN_BUTTON) == 0) {
                if (press_start == 0) press_start = (uint32_t)(esp_timer_get_time() / 1000);
                held = (uint32_t)(esp_timer_get_time() / 1000) - press_start;
                // LED feedback: slow blink < 2s, fast blink 2-5s, solid 5-15s, rapid > 15s
                if (held < 2000)       gpio_set_level(PIN_LED, (esp_timer_get_time() / 500000) % 2);
                else if (held < 5000)  gpio_set_level(PIN_LED, (esp_timer_get_time() / 150000) % 2);
                else if (held < 15000) gpio_set_level(PIN_LED, 1);
                else                   gpio_set_level(PIN_LED, (esp_timer_get_time() / 50000) % 2);
                // Auto-trigger at 15s for factory reset, 5s for update
                if (held >= 15000) { action = ACT_DIAG; break; }  // reuse DIAG as FACTORY_RESET
                if (held >= 5000)  { action = ACT_UPDATE; break; }
            } else if (press_start > 0) {
                // Button released — decide based on hold duration
                if (held >= 2000)      action = ACT_PAIR;
                press_start = 0;
                held = 0;
                if (action != ACT_NONE) break;
            } else {
                gpio_set_level(PIN_LED, (esp_timer_get_time() / 500000) % 2);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        gpio_set_level(PIN_LED, 0);

        if (action == ACT_UPDATE) {
            ESP_LOGI(TAG, "Entering WiFi OTA update mode...");
            led_set_effect(LED_EFFECT_PULSE_CYAN);
            led_flash(PIN_LED, 50, 10);
            wifi_ota_start(PIN_LED, lora_cfg.my_address);  // never returns
        }

        if (action == ACT_DIAG) {
            // Factory reset — erase all NVS (pairing, settings, LoRa config)
            ESP_LOGW(TAG, "FACTORY RESET — erasing NVS...");
            led_set_effect(LED_EFFECT_BLINK_RED);
            led_flash(PIN_LED, 50, 20);
            nvs_flash_erase();
            ESP_LOGW(TAG, "NVS erased. Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();  // never returns
        }

        #define DO_PAIR() do { \
            led_set_effect(LED_EFFECT_BLINK_BLUE); \
            led_flash(PIN_LED, 50, 6); \
            if (lora_tx_enter_pairing()) { \
                led_set_effect(LED_EFFECT_BLINK_GREEN); \
                led_flash(PIN_LED, 500, 3); \
                vTaskDelay(pdMS_TO_TICKS(500)); \
                esp_restart(); \
            } else { \
                /* IMPORTANT: clear the BLUE effect we started above. \
                 * Without this, BLUE blink leaks through sensor-read+TX+sleep \
                 * and stays latched on the WS2812 through deep sleep. */ \
                led_set_effect(LED_EFFECT_NONE); \
                led_flash(PIN_LED, 200, 5); \
            } \
            while (gpio_get_level(PIN_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(10)); \
            press_start = 0; \
            lora_tx_get_config(&lora_cfg); \
        } while (0)

        if (action == ACT_PAIR) {
            DO_PAIR();
        }

        // Unpaired guard — stay awake blinking until paired
        if (lora_cfg.my_address == 0) {
            ESP_LOGW(TAG, "Unpaired — hold button 2s to pair");
            press_start = 0;
            while (lora_cfg.my_address == 0) {
                led_flash(PIN_LED, 200, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (gpio_get_level(PIN_BUTTON) == 0) {
                    if (press_start == 0) press_start = (uint32_t)(esp_timer_get_time() / 1000);
                    if ((uint32_t)(esp_timer_get_time() / 1000) - press_start >= 2000) {
                        DO_PAIR();
                    }
                } else {
                    press_start = 0;
                }
            }
        }
        #undef DO_PAIR
    } else {
        // Timer wake — check if unpaired (shouldn't happen, but safety guard)
        if (lora_cfg.my_address == 0) {
            ESP_LOGW(TAG, "Unpaired on timer wake — sleeping until manual reset");
            esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
            ws2812_quiet_for_sleep();
            esp_deep_sleep_start();
        }
    }

    // ── Wake heartbeat: amber pulse so user knows TX is alive ──
    // Visible as soon as the device wakes from sleep, before the LoRa TX
    // result indicator below. Two-beat pattern (amber → green/red) gives
    // a glanceable "alive + result" wake status without keeping the LED on.
    ws2812_pulse(LED_AMBER, 150);

    // ── 5. Read sensors ──
    iface->init();
    int dist_cm = -1;
    esp_err_t sensor_err = iface->read_cm(&dist_cm);
    char sensor_status = (sensor_err == ESP_OK && dist_cm > 0) ? 'o' : 'e';
    if (sensor_status == 'e') {
        ESP_LOGW(TAG, "Sensor read failed (err=0x%x, dist=%d) — TX will mark status='e' so RX can preserve last-known reading", sensor_err, dist_cm);
    }

    power_init(PIN_I2C_SDA, PIN_I2C_SCL);
    power_reading_t pr = {0};
    power_read(&pr);
    int bat_pct = pr.pct;
    uint32_t bat_mv = pr.vbat_mv > 0 ? pr.vbat_mv : 3700;
    float bat_v = (float)bat_mv / 1000.0f;
    ESP_LOGI(TAG, "Power: mode=%s vbat=%lumV (%d%%) I=%ldmA P=%ldmW %s",
             power_mode_str(pr.mode), (unsigned long)pr.vbat_mv, pr.pct,
             (long)pr.current_ma, (long)pr.power_mw,
             pr.charging ? "CHARGING" : "discharging");

    // ── 6. Battery-voltage gate (INA219 only — ADC path was removed 2026-05-16) ──
    // Two thresholds, both depending on a TRUSTWORTHY voltage reading.
    //
    // SANITY FLOOR — bat_mv must be > BAT_PRESENT_MIN_MV before we apply any
    // cutoff. A real LiPo never reads below ~2.0V even after irreversible
    // damage; anything lower means the INA219 is reporting garbage (no
    // battery in the circuit, USB-only bench, faulty wiring). Acting on
    // bogus readings caused tx-v2.0.15 to hibernate dev hardware on first
    // boot — bench TX with no battery reads vbat=36mV which would otherwise
    // trip BAT_CUTOFF immediately. Below the floor we log + skip the gate.
    //
    //   < BAT_CUTOFF_MV  → hibernate 1h. LiPo at this voltage is being
    //                      damaged by further discharge; transmit cycles
    //                      will brown out and waste what little charge
    //                      remains. Long sleep is the only safe action.
    //
    //   < BAT_LOW_TX_MV  → skip TX this cycle, sleep 10 min. Battery still
    //                      has some charge but rest voltage is already low
    //                      enough that the 22 dBm LoRa burst will likely
    //                      sag the rail past the C3's BOD level. Skipping
    //                      one cycle preserves the ability to send a final
    //                      warning packet later when voltage recovers
    //                      (LiPo terminal voltage rises after rest).
    #define BAT_PRESENT_MIN_MV  1000
    if (pr.mode == POWER_MODE_INA219 && bat_mv > BAT_PRESENT_MIN_MV) {
        if (bat_mv < BAT_CUTOFF_MV) {
            ESP_LOGW(TAG, "BAT CRITICAL %" PRIu32 "mV — hibernating %ds",
                     bat_mv, BAT_CUTOFF_SLEEP_S);
            led_flash(PIN_LED, 50, 10);    // rapid flash = critical
            deep_sleep_for(BAT_CUTOFF_SLEEP_S, "bat_critical");
        }
        if (bat_mv < BAT_LOW_TX_MV) {
            ESP_LOGW(TAG, "BAT LOW %" PRIu32 "mV — skip TX, sleeping %ds",
                     bat_mv, BAT_LOW_TX_SLEEP_S);
            led_flash(PIN_LED, 100, 3);    // 3 slow blinks = battery low warning
            deep_sleep_for(BAT_LOW_TX_SLEEP_S, "bat_low");
        }
    } else if (pr.mode == POWER_MODE_INA219 && bat_mv <= BAT_PRESENT_MIN_MV) {
        ESP_LOGW(TAG, "INA219 vbat=%" PRIu32 "mV looks like no battery / bench USB — skipping cutoff",
                 bat_mv);
    }

    // ── 7. Send TANK packet ──
    s_msg_id++;
    bool acked = lora_tx_send(dist_cm > 0 ? dist_cm : 0, bat_pct, bat_v,
                              power_mode_char(pr.mode),
                              pr.current_ma, pr.power_mw,
                              s_msg_id, sensor_status);

    // ── TX result indicator: green if ACK'd, red if no ACK ──
    // Second beat of the wake heartbeat. Visible at-a-glance from a few
    // metres away — much easier than counting onboard-LED blinks.
    ws2812_pulse(acked ? LED_GREEN : LED_RED, 200);

    if (acked) {
        s_ack_failures = 0;
        // Successful TX → the brown-out chain (if any) is broken. Reset the
        // streak so we go back to the configured sleep cadence instead of
        // the exponential-backoff schedule. If the next cycle browns out
        // again, the streak rebuilds from 1.
        if (s_brownout_streak != 0) {
            ESP_LOGI(TAG, "Brown-out streak cleared after successful ACK (was %" PRIu32 ")",
                     s_brownout_streak);
            s_brownout_streak = 0;
        }
        led_flash(PIN_LED, 80, 2);
        handle_downlink();
    } else {
        s_ack_failures++;
        led_flash(PIN_LED, 200, 3);
    }

    // ── 8. Deep sleep ──
    led_set_effect(LED_EFFECT_NONE);
    led_set(0, LED_OFF); led_show();   // Single WS2812 — only index 0 exists
    // Jitter prevents two TXs paired to the same hub from phase-locking and
    // colliding on every wake. 0-10s window is large enough to drift apart
    // within a few cycles even if both started simultaneously, but small
    // enough that the user-visible wake interval doesn't surprise them.
    uint32_t jitter_ms = esp_random() % 10000;
    uint64_t sleep_us  = (uint64_t)sleep_s * 1000000ULL + (uint64_t)jitter_ms * 1000ULL;
    ESP_LOGI(TAG, "Sleeping %" PRIu32 "s (+%" PRIu32 "ms anti-collision jitter)", sleep_s, jitter_ms);
    esp_sleep_enable_timer_wakeup(sleep_us);
    ws2812_quiet_for_sleep();
    esp_deep_sleep_start();
}

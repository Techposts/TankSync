/**
 * TankSync Transmitter v2 - Central Configuration
 * ESP32-C3 SuperMini + RYLR998 + AJ-SR04M ultrasonic
 *
 * Pin assignments match existing v1.0 hardware — same physical boards.
 */

#pragma once

// ============================================================================
// FIRMWARE
// ============================================================================
#define FIRMWARE_VERSION        "2.0.12"
#define FIRMWARE_TYPE           "transmitter"

// ============================================================================
// PINS - ESP32-C3 SuperMini (unchanged from v1.0 hardware)
// ============================================================================
#define PIN_LORA_TX             21      // UART1 TX → RYLR998 RXD
#define PIN_LORA_RX             20      // UART1 RX ← RYLR998 TXD
#define PIN_LED                 8       // On-board LED for ESP32-C3 SuperMini
#define PIN_WS2812              7       // WS2812B data (single LED)
#define PIN_WS2812_COUNT        1       // Number of WS2812B LEDs (single, low power)
#define PIN_TRIG                4       // AJ-SR04M TRIG
#define PIN_ECHO                5       // AJ-SR04M ECHO
#define PIN_BUTTON              9       // Boot/config button (INPUT_PULLUP)

// ── +5V high-side switch (REV 2.2 hardware) ──────────────────────────────────
// PIN_5V_GATE drives Q3 (AO3400 N-FET) via R3 (1kΩ series), whose drain
// inverts the signal onto the gates of Q1 + Q2 (AO3401 P-FETs). Q1 switches
// +3.3V to the RYLR998 LoRa module; Q2 switches +5V to the AJ-SR04M and
// WS2812B. R1 (100kΩ) pulls Q3 gate to GND so the FETs stay OFF when the
// MCU is in reset/sleep — but only if the pin is actively driven LOW (a
// floating input + ESP32's internal weak pull-up + ESD-diode leakage gives
// ~0.77V on the gate, enough to drag the P-FET gate node low and keep the
// loads partially on, which is what Ravi measured before this gate-drive
// was wired up in firmware).
//
// Wiring note (REV 2.2 as-built, 2026-05-17): GPIO10 was originally listed
// in the schematic plan to gate the MT3608 boost converter's EN pin — that
// route never made it to the PCB. GPIO10 IS routed to R3 → Q3 gate, which
// is its actually-used purpose on this board rev.
//
// Logic:  HIGH = wake-time, 5V loads enabled.  LOW = sleep, 5V loads off.
#define PIN_5V_GATE             10
#define PWR_SETTLE_MS           50      // AJ-SR04M takes ~30ms after V+ rises

// ============================================================================
// SENSOR - AJ-SR04M ultrasonic
// ============================================================================
#define SENSOR_TRIG_US          20      // Trigger pulse width (µs).
                                        // JSN-SR04T V3.0 datasheet specifies
                                        // 20 µs minimum for reliable operation
                                        // (vs HC-SR04's 10 µs). Especially
                                        // important in JSN Mode 4 (low-power
                                        // between-pings) where the analog
                                        // front-end is more sensitive to
                                        // marginal TRIG pulse widths.
#define SENSOR_TIMEOUT_US       30000   // Echo timeout = 5m (30000µs)
#define SENSOR_MIN_CM           5       // Reject readings below this
#define SENSOR_MAX_CM           400     // Reject readings above this
#define SENSOR_SAMPLES          5       // Readings per wake cycle; median used
#define SENSOR_SAMPLE_DELAY_MS  50      // Delay between samples

// Speed of sound factor: distance = echo_us / 58 (for cm)
#define SENSOR_US_TO_CM         58

// ============================================================================
// LORA - RYLR998
// ============================================================================
#define LORA_UART_NUM           UART_NUM_1
#define LORA_BAUD               115200
#define LORA_DEFAULT_FREQ       865000000   // 865 MHz
#define LORA_DEFAULT_NETID      6
#define LORA_DEFAULT_ADDR       1           // Transmitter address (unique per unit)
#define LORA_RECEIVER_ADDR      2           // Receiver's address
#define LORA_CMD_TIMEOUT_MS     1500
#define LORA_ACK_TIMEOUT_MS     3000        // Wait up to 3s for ACK
#define LORA_MAX_RETRIES        3           // Retry count if no ACK

// ============================================================================
// POWER MONITORING — INA219 over I²C (single mode as of 2026-05-16)
// ============================================================================
// INA219 in series with battery+ (0.1Ω shunt). I²C on GPIO1/2 at 0x40.
//   Bus voltage register   → battery voltage
//   Shunt voltage register → signed current
//                            (positive = discharging, negative = charging)
// NVS key "pwr_mode_ovr" accepts: auto / ina219 / disabled.
//
// Voltage-divider mode removed 2026-05-16. The PCB still has the optional
// 100k/100k divider on GPIO0 (drawing ~20µA from battery, harmless), but
// firmware ignores it entirely. To eliminate the parasitic draw, desolder
// the two divider resistors on the PCB — purely a hardware option, not
// required for firmware to work.

#define PIN_I2C_SDA             1               // GPIO1 → INA219 SDA
#define PIN_I2C_SCL             2               // GPIO2 → INA219 SCL
#define INA219_I2C_ADDR         0x40            // INA219 default (A0/A1 jumpers open)
#define INA219_I2C_FREQ_HZ      100000          // 100 kHz standard mode
// 0.1 Ω shunt → shunt_raw / 10 = current in mA (signed; +ve discharge, -ve charge)

// ============================================================================
// DEEP SLEEP
// ============================================================================
#define SLEEP_INTERVAL_S        30      // 30s default — RX-pushed config overrides via NVS at next wake
#define SLEEP_INTERVAL_US       ((uint64_t)SLEEP_INTERVAL_S * 1000000ULL)

// ============================================================================
// NVS NAMESPACES
// ============================================================================
#define NVS_NS_LORA             "lora"
#define NVS_NS_SYSTEM           "system"

// ============================================================================
// RTC MEMORY - survives deep sleep
// ============================================================================
// msg_id counter persisted in RTC memory (no NVS write needed on each wake)
// boot_count for diagnostics

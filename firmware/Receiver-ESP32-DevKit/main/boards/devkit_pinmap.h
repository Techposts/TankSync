// Pin map: ESP32 DevKit v1 (CP2102) — the original production hub board.
// Wiring source of truth: cloud/firmware/Receiver-ESP32-DevKit/BOARD.md and
// public/hardware/wiring.md. Keep both documents in sync if any pin moves.
#pragma once

#define PIN_LORA_RX             16      // UART2 RX ← RYLR998 TXD
#define PIN_LORA_TX             17      // UART2 TX → RYLR998 RXD
#define PIN_LED_DATA            13      // WS2812B data (GPIO2 is onboard LED on DevKit)
#define PIN_I2C_SDA             21      // OLED + INA219 SDA
#define PIN_I2C_SCL             22      // OLED + INA219 SCL
#define PIN_BUZZER              2       // Active 3-pin buzzer. Strapping pin: disconnect during USB flash. Shares with onboard LED — LED pulses with buzzer.

#define LORA_UART_NUM           UART_NUM_2  // ESP32 has UART2 (C3 only had UART0/1)

// Pin map: ESP32-S3 SuperMini (FH4R2 = 4 MB flash + 2 MB PSRAM, USB-Serial/JTAG).
// Onboard addressable WS2812 sits on GPIO48 — we reuse the same data pin so
// no external strip is required for first-boot status. USB D+/D- are fixed
// at GPIO19/20 by silicon — avoid those. Strapping pins GPIO0, GPIO45, GPIO46
// are also kept clear of peripheral roles.
#pragma once

#define PIN_LORA_RX             17      // UART1 RX ← RYLR998 TXD
#define PIN_LORA_TX             16      // UART1 TX → RYLR998 RXD
#define PIN_LED_DATA            48      // Onboard WS2812 on S3 SuperMini
#define PIN_I2C_SDA             8       // OLED + INA219 SDA
#define PIN_I2C_SCL             9       // OLED + INA219 SCL
#define PIN_BUZZER              4       // Active 3-pin buzzer. Free GPIO, no strapping or USB conflict.

#define LORA_UART_NUM           UART_NUM_1  // UART1 is the free general-purpose UART on S3

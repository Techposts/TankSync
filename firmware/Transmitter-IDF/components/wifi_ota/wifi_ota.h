/**
 * wifi_ota — WiFi AP mode for transmitter OTA + settings
 *
 * Activated by 8s button hold during boot window.
 * Creates open WiFi AP "TankSync-Update", serves web UI at 192.168.4.1.
 * User can upload firmware and edit device settings.
 * Auto-reboots after 5 minutes of inactivity.
 * This function never returns (reboots or times out → reboot).
 */

#pragma once
#include <stdint.h>

/**
 * Enter WiFi AP OTA mode. Does not return.
 * - Starts WiFi AP "TankSync-<addr>" (e.g. TankSync-851)
 * - Starts HTTP server with OTA upload + settings page at 192.168.4.1
 * - Reboots after successful OTA or 5-minute timeout
 *
 * @param led_gpio    GPIO number for status LED blink
 * @param device_addr LoRa address for AP SSID (0 = use "TankSync-Update")
 */
void wifi_ota_start(int led_gpio, uint16_t device_addr);

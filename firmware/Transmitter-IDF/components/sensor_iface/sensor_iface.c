/**
 * sensor_iface — vtable + resolver. See sensor_iface.h for design notes.
 */

#include "sensor_iface.h"
#include "sensor_sr04.h"
#include "sensor_ld2413.h"
#include <string.h>

// ── SR04 wrapper ─────────────────────────────────────────────────────────────
// Pins are stashed by sensor_iface_sr04_set_pins() before init runs. Defaults
// match main/config.h (PIN_TRIG=4, PIN_ECHO=5) so init still works on the
// canonical hardware if a caller forgets to configure them.
static int s_sr04_trig = 4;
static int s_sr04_echo = 5;

void sensor_iface_sr04_set_pins(int trig_pin, int echo_pin) {
    s_sr04_trig = trig_pin;
    s_sr04_echo = echo_pin;
}

static esp_err_t sr04_init(void)               { return sensor_init(s_sr04_trig, s_sr04_echo); }
static esp_err_t sr04_read_cm(int *out_cm)     { return sensor_read_cm(out_cm); }
static void      sr04_deinit(void)             { /* sensor_sr04 has no deinit; GPIOs reset in deep sleep */ }
static uint32_t  sr04_warmup_ms(void)          { return 50;  } // AJ-SR04M settle after V+ rises; matches PWR_SETTLE_MS
static int       sr04_min_cm(void)             { return 5;   }
static int       sr04_max_cm(void)             { return 400; }

static const sensor_iface_t SR04_IFACE = {
    .name      = "sr04",
    .init      = sr04_init,
    .read_cm   = sr04_read_cm,
    .deinit    = sr04_deinit,
    .warmup_ms = sr04_warmup_ms,
    .min_cm    = sr04_min_cm,
    .max_cm    = sr04_max_cm,
};

// ── LD2413 wrapper ───────────────────────────────────────────────────────────
// UART number + pins configured by sensor_iface_ld2413_set_uart() before init.
// Defaults match the canonical TX board: UART_NUM_0 free (LoRa owns UART_NUM_1),
// pins shared with SR04 (GPIO4 = sensor RX-side TX, GPIO5 = sensor TX-side RX).
static int s_ld2413_uart = 0;   // UART_NUM_0
static int s_ld2413_tx   = 4;
static int s_ld2413_rx   = 5;

void sensor_iface_ld2413_set_uart(int uart_num, int pin_tx, int pin_rx) {
    s_ld2413_uart = uart_num;
    s_ld2413_tx   = pin_tx;
    s_ld2413_rx   = pin_rx;
}

static esp_err_t ld2413_init_wrapper(void) {
    return ld2413_init((uart_port_t)s_ld2413_uart, s_ld2413_tx, s_ld2413_rx);
}
static esp_err_t ld2413_read_cm_wrapper(int *out_cm) { return ld2413_read_cm(out_cm); }
static void      ld2413_deinit_wrapper(void)         { ld2413_deinit(); }

// LD2413 needs the sensor's internal MCU to boot + run its first radar sweep
// before clean frames stream. Datasheet sec 9.4: response time = 2.4s @ 160ms
// reporting cycle. We give ~3s budget for the first valid frame; the actual
// wait amortizes against everything else in the wake cycle (LoRa init, boot
// window, etc.) because warmup is measured from power-on, not from init().
static uint32_t ld2413_warmup_ms(void) { return 3000; }
static int      ld2413_min_cm(void)    { return 15;   }
static int      ld2413_max_cm(void)    { return 1050; }

static const sensor_iface_t LD2413_IFACE = {
    .name      = "ld2413",
    .init      = ld2413_init_wrapper,
    .read_cm   = ld2413_read_cm_wrapper,
    .deinit    = ld2413_deinit_wrapper,
    .warmup_ms = ld2413_warmup_ms,
    .min_cm    = ld2413_min_cm,
    .max_cm    = ld2413_max_cm,
};

// ── Resolver ─────────────────────────────────────────────────────────────────

const sensor_iface_t *sensor_get(const char *kind) {
    if (!kind) return NULL;
    if (strcmp(kind, "sr04")   == 0) return &SR04_IFACE;
    if (strcmp(kind, "ld2413") == 0) return &LD2413_IFACE;
    return NULL;
}

const sensor_iface_t *sensor_get_default(void) {
    return &SR04_IFACE;
}

/**
 * LED Control Module
 */
#ifndef LEDS_H
#define LEDS_H

#include <Adafruit_NeoPixel.h>
#include "config.h"

extern Adafruit_NeoPixel leds;
extern Color ledColors[NUM_LEDS];
extern bool blinkState;

void initializeLeds();
void updateLedColors();
void safeLedShow();

#endif

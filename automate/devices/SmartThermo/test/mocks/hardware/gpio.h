// =====================================================================================
// test/mocks/hardware/gpio.h
// Host stand-in for the pico-sdk GPIO pad settings: the simulator records each pin's drive
// strength and checks a relay contact only closes on a pin set to 12 mA.
// =====================================================================================
#pragma once

#include <stdint.h>

enum gpio_drive_strength {
  GPIO_DRIVE_STRENGTH_2MA = 0,
  GPIO_DRIVE_STRENGTH_4MA = 1, // the RP2040's reset value
  GPIO_DRIVE_STRENGTH_8MA = 2,
  GPIO_DRIVE_STRENGTH_12MA = 3
};

void gpio_set_drive_strength(uint32_t gpio, enum gpio_drive_strength drive);

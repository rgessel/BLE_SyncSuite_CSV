/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

/* Defines */
#define BLINK_GPIO CONFIG_EXAMPLE_BLINK_GPIO

/* Public function declarations */
uint8_t get_led_state(void);
void led_on(void);
void led_off(void);
void led_init(void);

#endif // LED_H

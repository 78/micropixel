// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#define GPIO_NUM_NC -1
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_INTR_POSEDGE 1
#define GPIO_INTR_NEGEDGE 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define BIT64(a) (UINT64_C(1) << (a))
typedef struct {
    int mode;
    int intr_type;
    int pull_up_en;
    uint64_t pin_bit_mask;
} gpio_config_t;
static inline int gpio_config(const gpio_config_t* config) {
    (void)config;
    return 0;
}
static inline int gpio_reset_pin(int pin) {
    (void)pin;
    return 0;
}
static inline int gpio_isr_handler_remove(int pin) {
    (void)pin;
    return 0;
}
static inline int gpio_set_level(int pin, int level) {
    (void)pin;
    (void)level;
    return 0;
}

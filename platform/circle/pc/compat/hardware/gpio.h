#pragma once
#define GPIO_OUT 1
#define GPIO_IN 0
#define PICO_DEFAULT_LED_PIN 16
static inline void gpio_init(unsigned) { }
static inline void gpio_set_dir(unsigned, int) { }
static inline void gpio_put(unsigned, int) { }

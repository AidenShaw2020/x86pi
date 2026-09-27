#pragma once
#include <stdint.h>
#include <pico.h>
#include <stdbool.h>
typedef uint64_t absolute_time_t;
absolute_time_t make_timeout_time_ms(uint32_t);
bool time_reached(absolute_time_t);

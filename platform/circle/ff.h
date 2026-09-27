#pragma once
/* Make the Circle FatFs compatibility header visible to transitive shared
 * headers as a real project-relative file.  Circle's dependency generator
 * otherwise records the bare name `ff.h`, which GNU make cannot resolve. */
#include "../../build-rpi3-deps/circle/addon/fatfs/ff.h"

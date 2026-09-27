#pragma once
#define __mem_fence() __asm__ __volatile__("dmb ish" ::: "memory")
#ifndef __dmb
#define __dmb() __mem_fence()
#endif

#ifndef JIT_H
#define JIT_H

#include "pd_api.h"

/* 6809 dynamic recompiler (work in progress).
 *
 * M1: prove the device can emit ARM Thumb code at runtime and execute it. */
void jit_selftest(PlaydateAPI *playdate);

#endif /* JIT_H */

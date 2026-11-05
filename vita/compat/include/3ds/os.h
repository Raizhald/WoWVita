// os.h — the part of libctru's <3ds/os.h> the game uses, for the Vita / PC compatibility layer.
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSCLOCK_SOC       (16756991)
#define SYSCLOCK_SYS       (SYSCLOCK_SOC * 2)
#define SYSCLOCK_SDMMC     (SYSCLOCK_SYS * 2)
#define SYSCLOCK_ARM9      (SYSCLOCK_SYS * 4)
#define SYSCLOCK_ARM11     (SYSCLOCK_ARM9 * 2)
#define SYSCLOCK_ARM11_NEW (SYSCLOCK_ARM11 * 3)
#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)
#define CPU_TICKS_PER_USEC (SYSCLOCK_ARM11 / 1000000.0)

typedef enum {
    MEMREGION_ALL = 0,
    MEMREGION_APPLICATION = 1,
    MEMREGION_SYSTEM = 2,
    MEMREGION_BASE = 3,
} MemRegion;

u64 osGetTime(void);                 // milliseconds (monotonic here)
void osSetSpeedupEnable(bool enable);
u8 osGetWifiStrength(void);           // 0..3
s64 osGetMemRegionSize(MemRegion region);
s64 osGetMemRegionUsed(MemRegion region);
s64 osGetMemRegionFree(MemRegion region);
static inline bool osIsNew3DS(void) { return true; }

#ifdef __cplusplus
}
#endif

// 3ds.h — libctru replacement for running the WoW3DS client on the PS Vita (and on a PC for tests).
//
// The enums, structs and macros come from libctru (zlib licence, see platform/LICENSE.libctru.md); everything that
// talks to the 3DS hardware or OS is re-implemented in platform/src on top of VitaSDK / POSIX.
#pragma once

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/os.h>
#include <3ds/gpu/enums.h>
#include <3ds/gpu/registers.h>
#include <3ds/gpu/gx.h>
#include <3ds/services/hid.h>
#include <3ds/ndsp/ndsp.h>
#include <3ds/ndsp/channel.h>
#include <3ds/applets/swkbd.h>
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------ svc / kernel
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define CUR_THREAD_HANDLE  0xFFFF8000

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef enum { USERBREAK_PANIC = 0, USERBREAK_ASSERT = 1, USERBREAK_USER = 2 } UserBreakType;
typedef enum {
    MEMOP_FREE = 1, MEMOP_RESERVE = 2, MEMOP_ALLOC = 3, MEMOP_MAP = 4, MEMOP_UNMAP = 5, MEMOP_PROT = 6,
    MEMOP_REGION_APP = 0x100, MEMOP_REGION_SYSTEM = 0x200, MEMOP_REGION_BASE = 0x300,
    MEMOP_LINEAR_FLAG = 0x10000, MEMOP_ALLOC_LINEAR = MEMOP_LINEAR_FLAG | MEMOP_ALLOC,
} MemOp;
typedef enum { MEMPERM_READ = 1, MEMPERM_WRITE = 2, MEMPERM_EXECUTE = 4, MEMPERM_READWRITE = 3 } MemPerm;
typedef enum {
    RESLIMIT_PRIORITY = 0, RESLIMIT_COMMIT = 1, RESLIMIT_THREAD = 2, RESLIMIT_EVENT = 3, RESLIMIT_MUTEX = 4,
    RESLIMIT_SEMAPHORE = 5, RESLIMIT_TIMER = 6, RESLIMIT_SHAREDMEMORY = 7, RESLIMIT_ADDRESSARBITER = 8,
    RESLIMIT_CPUTIME = 9, RESLIMIT_BIT = 0x80000000,
} ResourceLimitType;

u64 svcGetSystemTick(void);                       // SYSCLOCK_ARM11 ticks
void svcSleepThread(s64 ns);
void svcBreak(UserBreakType breakReason);
Result svcGetThreadPriority(s32* out, Handle handle);
Result svcSetThreadPriority(Handle thread, s32 prio);
Result svcGetThreadId(u32* out, Handle handle);
Result svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
Result svcGetResourceLimit(Handle* resourceLimit, Handle process);
Result svcGetResourceLimitLimitValues(s64* values, Handle resourceLimit, ResourceLimitType* names, s32 nameCount);
Result svcGetResourceLimitCurrentValues(s64* values, Handle resourceLimit, ResourceLimitType* names, s32 nameCount);
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size);
Result svcCloseHandle(Handle handle);
Result svcOutputDebugString(const char* str, s32 length);

// ------------------------------------------------------------------ threads
typedef struct Thread_tag* Thread;
Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id, bool detached);
Handle threadGetHandle(Thread thread);
int threadGetExitCode(Thread thread);
void threadFree(Thread thread);
Result threadJoin(Thread thread, u64 timeout_ns);
Thread threadGetCurrent(void);
void threadExit(int rc) __attribute__((noreturn));

typedef struct ERRF_ExceptionInfo { u32 type, fsr, far_, fpexc, fpinst, fpinst2; } ERRF_ExceptionInfo;
typedef void (*ExceptionHandler)(ERRF_ExceptionInfo* excep, CpuRegisters* regs);
#define RUN_HANDLER_ON_FAULTING_STACK ((void*)1)
#define WRITE_DATA_TO_HANDLER_STACK NULL
#define WRITE_DATA_TO_FAULTING_STACK ((void*)1)
void threadOnException(ExceptionHandler handler, void* stack_top, void* exception_data);

// ------------------------------------------------------------------ synchronisation (pthread underneath)
typedef struct { pthread_mutex_t m; int ok; } LightLock;
typedef struct { pthread_mutex_t m; int ok; } RecursiveLock;
typedef struct { pthread_mutex_t m; pthread_cond_t c; int state; ResetType type; int ok; } LightEvent;
typedef struct { pthread_mutex_t m; pthread_cond_t c; s32 count; s16 max; int ok; } LightSemaphore;

void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
int LightLock_TryLock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);
void RecursiveLock_Init(RecursiveLock* lock);
void RecursiveLock_Lock(RecursiveLock* lock);
int RecursiveLock_TryLock(RecursiveLock* lock);
void RecursiveLock_Unlock(RecursiveLock* lock);
void LightEvent_Init(LightEvent* event, ResetType reset_type);
void LightEvent_Clear(LightEvent* event);
void LightEvent_Pulse(LightEvent* event);
void LightEvent_Signal(LightEvent* event);
int LightEvent_TryWait(LightEvent* event);
void LightEvent_Wait(LightEvent* event);
int LightEvent_WaitTimeout(LightEvent* event, s64 timeout_ns);   // 0 signalled, 1 timed out
void LightSemaphore_Init(LightSemaphore* semaphore, s16 initial_count, s16 max_count);
void LightSemaphore_Acquire(LightSemaphore* semaphore, s32 count);
int LightSemaphore_TryAcquire(LightSemaphore* semaphore, s32 count);
void LightSemaphore_Release(LightSemaphore* semaphore, s32 count);

// ------------------------------------------------------------------ memory
void* linearAlloc(size_t size);
void* linearMemAlign(size_t size, size_t alignment);
void* linearRealloc(void* mem, size_t size);
size_t linearGetSize(void* mem);
void linearFree(void* mem);
u32 linearSpaceFree(void);
void* vramAlloc(size_t size);
void* vramMemAlign(size_t size, size_t alignment);
void vramFree(void* mem);
u32 vramSpaceFree(void);
static inline u32 osConvertVirtToPhys(const void* vaddr) { return (u32)(uintptr_t)vaddr; }

// GPU-visible memory is ordinary memory here; a flush tells the compatibility layer a texture's data changed.
Result GSPGPU_FlushDataCache(const void* adr, u32 size);
Result GSPGPU_InvalidateDataCache(const void* adr, u32 size);
Result DSP_FlushDataCache(const void* address, u32 size);
Result DSP_InvalidateDataCache(const void* address, u32 size);

// ------------------------------------------------------------------ gfx
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
#define GSP_SCREEN_WIDTH        240
#define GSP_SCREEN_HEIGHT_TOP   400
#define GSP_SCREEN_HEIGHT_TOP_2X 800
#define GSP_SCREEN_HEIGHT_BOTTOM 320
void gfxInitDefault(void);
void gfxExit(void);
void gfxSet3D(bool enable);
bool gfxIs3D(void);
void gfxSetWide(bool enable);
bool gfxIsWide(void);
void gfxSwapBuffers(void);
void gfxFlushBuffers(void);
void gspWaitForVBlank(void);

// ------------------------------------------------------------------ apt / misc services
typedef enum { APTHOOK_ONSUSPEND = 0, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP, APTHOOK_ONWAKEUP, APTHOOK_ONEXIT } APT_HookType;
bool aptMainLoop(void);
Result APT_SetAppCpuTimeLimit(u32 percent);
Result APT_GetAppCpuTimeLimit(u32* percent);
void aptSetHomeAllowed(bool allowed);
Result romfsInit(void);
Result romfsExit(void);
static inline Result romfsMountSelf(const char* name) { (void)name; return romfsInit(); }

// irrst (C-stick on the New 3DS)
void hidCstickRead(circlePosition* pos);

// ------------------------------------------------------------------ shaders (GLSL programs made by platform/tools/pica2glsl.py)
typedef struct { s8 index; const char* name; } CompatUniform;
typedef struct DVLE_s {
    const char* source;        // GLSL vertex shader
    int numUniforms;
    const CompatUniform* uniforms;
    int numInputs;             // vertex attributes a0..a(n-1)
    u32 outMask;               // varyings written: bit n texcoord n, bit 8 colour
    int numRegs;               // float uniform registers used (c[0..numRegs-1])
    u32 id;
} DVLE_s;
typedef struct { u32 numDVLE; DVLE_s* DVLE; } DVLB_s;
typedef struct { DVLE_s* dvle; } shaderInstance_s;
typedef struct {
    shaderInstance_s* vertexShader;
    shaderInstance_s* geometryShader;
    u32 geoShaderInputPermutation[2];
    u8 geoShaderInputStride;
} shaderProgram_s;

DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize);
void DVLB_Free(DVLB_s* dvlb);
Result shaderProgramInit(shaderProgram_s* sp);
Result shaderProgramFree(shaderProgram_s* sp);
Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle);
s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name);

// ------------------------------------------------------------------ console, environment, GSP events
typedef struct PrintConsole { int consoleWidth, consoleHeight; } PrintConsole;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console);
PrintConsole* consoleSelect(PrintConsole* console);
void consoleClear(void);
bool envIsHomebrew(void);
Result APT_CheckNew3DS(bool* out);
float osGet3DSliderState(void);

typedef enum {
    GSPGPU_EVENT_PSC0 = 0, GSPGPU_EVENT_PSC1, GSPGPU_EVENT_VBlank0, GSPGPU_EVENT_VBlank1, GSPGPU_EVENT_PPF,
    GSPGPU_EVENT_P3D, GSPGPU_EVENT_DMA, GSPGPU_EVENT_MAX,
} GSPGPU_Event;
void gxCmdQueueInterrupt(GSPGPU_Event irq);
void gspWaitForAnyEvent(void);

#define W3D_COMPAT 1   // game code can test for the Vita / PC compatibility layer

#ifdef __cplusplus
}
#endif

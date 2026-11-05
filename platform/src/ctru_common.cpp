// ctru_common.cpp — libctru system services on POSIX / VitaSDK: time, threads, locks, memory, gfx/apt stand-ins.
#include <3ds.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <mutex>
#include <unistd.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

extern "C" {

// Symbols the 3DS build gets from heaps.cpp / libctru (read by main.cpp and the perf log).
u32 __ctru_heap = 0, __ctru_heap_size = 0, __ctru_linear_heap = 0, __ctru_linear_heap_size = 0;

// ------------------------------------------------------------------ time
static u64 monoUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000;
#endif
}

u64 svcGetSystemTick(void) {
    return (u64)((double)monoUs() * (SYSCLOCK_ARM11 / 1000000.0));
}

u64 osGetTime(void) { return monoUs() / 1000; }

void svcSleepThread(s64 ns) {
    if (ns <= 0) {
#if defined(__vita__)
        sceKernelDelayThread(0);
#else
        sched_yield();
#endif
        return;
    }
#if defined(__vita__)
    sceKernelDelayThread((SceUInt)(ns / 1000));
#else
    timespec ts{(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, nullptr);
#endif
}

void svcBreak(UserBreakType breakReason) {
    fprintf(stderr, "svcBreak(%d)\n", (int)breakReason);
    fflush(stderr);
    abort();
}

Result svcGetThreadPriority(s32* out, Handle handle) {
    (void)handle;
    if (out) *out = 0x30;
    return 0;
}
Result svcSetThreadPriority(Handle thread, s32 prio) { (void)thread; (void)prio; return 0; }
Result svcGetThreadId(u32* out, Handle handle) {
    (void)handle;
    if (out) *out = (u32)(uintptr_t)pthread_self();
    return 0;
}
Result svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm) {
    (void)addr_out; (void)addr0; (void)addr1; (void)size; (void)op; (void)perm;
    return -1;
}
Result svcGetResourceLimit(Handle* resourceLimit, Handle process) {
    (void)process;
    if (resourceLimit) *resourceLimit = 1;
    return 0;
}
static s64 resLimit(ResourceLimitType t, bool current) {
    switch (t) {
        case RESLIMIT_COMMIT: return current ? 64ll << 20 : 256ll << 20;
        case RESLIMIT_CPUTIME: return current ? 30 : 80;
        case RESLIMIT_THREAD: return current ? 8 : 64;
        default: return current ? 0 : 64;
    }
}
Result svcGetResourceLimitLimitValues(s64* values, Handle resourceLimit, ResourceLimitType* names, s32 nameCount) {
    (void)resourceLimit;
    for (s32 i = 0; i < nameCount; ++i) values[i] = resLimit(names[i], false);
    return 0;
}
Result svcGetResourceLimitCurrentValues(s64* values, Handle resourceLimit, ResourceLimitType* names, s32 nameCount) {
    (void)resourceLimit;
    for (s32 i = 0; i < nameCount; ++i) values[i] = resLimit(names[i], true);
    return 0;
}
// The game flushes buffers that the GPU reads: textures it filled itself are re-uploaded after a flush.
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size) {
    (void)process;
#if defined(__vita__)
    GSPGPU_FlushDataCache((const void*)(uintptr_t)addr, size);
#else
    (void)addr; (void)size;   // a 32-bit address of a 64-bit pointer: nothing to look up (textures flush by GSPGPU too)
#endif
    return 0;
}
Result svcCloseHandle(Handle handle) { (void)handle; return 0; }
Result svcOutputDebugString(const char* str, s32 length) {
    fwrite(str, 1, (size_t)length, stderr);
    return 0;
}

// ------------------------------------------------------------------ threads
struct Thread_tag {
    pthread_t th;
    ThreadFunc entry;
    void* arg;
    bool detached;
    bool joined = false;
    std::atomic<bool> finished{false};
    int rc = 0;
};

static void* threadTrampoline(void* p) {
    Thread_tag* t = (Thread_tag*)p;
    t->entry(t->arg);
    t->finished = true;
    if (t->detached) delete t;
    return nullptr;
}

Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id, bool detached) {
    (void)prio; (void)core_id;
    Thread_tag* t = new Thread_tag();
    t->entry = entrypoint;
    t->arg = arg;
    t->detached = detached;
    pthread_attr_t a;
    pthread_attr_init(&a);
    // 3DS stacks are sized tightly (16-32 KB); the Vita's newlib and the PC's libc want more headroom.
    size_t ss = stack_size < (256u << 10) ? (256u << 10) : stack_size;
    pthread_attr_setstacksize(&a, ss);
    if (detached) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    const int e = pthread_create(&t->th, &a, threadTrampoline, t);
    pthread_attr_destroy(&a);
    if (e) {
        delete t;
        return nullptr;
    }
    return t;
}

Handle threadGetHandle(Thread thread) { return thread ? 1 : 0; }
int threadGetExitCode(Thread thread) { return thread ? thread->rc : 0; }
// libctru: threadJoin waits, threadFree releases (a thread that is still running gets detached and frees itself).
void threadFree(Thread thread) {
    if (!thread || thread->detached) return;
    if (!thread->joined) {
        if (!thread->finished) {
            thread->detached = true;   // threadTrampoline deletes it when it ends
            pthread_detach(thread->th);
            return;
        }
        pthread_join(thread->th, nullptr);
    }
    delete thread;
}
Result threadJoin(Thread thread, u64 timeout_ns) {
    if (!thread) return -1;
    if (timeout_ns != U64_MAX && !thread->finished) {   // wait up to the timeout, then report (like svcWaitSynchronization)
        const u64 end = monoUs() + timeout_ns / 1000;
        while (!thread->finished && monoUs() < end) svcSleepThread(1000000);
        if (!thread->finished) return (Result)0x09401BFE;   // timeout
    }
    if (!thread->joined) pthread_join(thread->th, nullptr);
    thread->joined = true;
    return 0;
}
Thread threadGetCurrent(void) { return nullptr; }
void threadExit(int rc) {
    (void)rc;
    pthread_exit(nullptr);
}
void threadOnException(ExceptionHandler handler, void* stack_top, void* exception_data) {
    (void)handler; (void)stack_top; (void)exception_data;
}

// ------------------------------------------------------------------ synchronisation
static void initOnce(pthread_mutex_t* m, int* ok, bool recursive) {
    if (*ok) return;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (recursive) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    *ok = 1;
}

void LightLock_Init(LightLock* lock) { lock->ok = 0; initOnce(&lock->m, &lock->ok, false); }
void LightLock_Lock(LightLock* lock) { if (!lock->ok) LightLock_Init(lock); pthread_mutex_lock(&lock->m); }
int LightLock_TryLock(LightLock* lock) { if (!lock->ok) LightLock_Init(lock); return pthread_mutex_trylock(&lock->m) == 0 ? 0 : 1; }
void LightLock_Unlock(LightLock* lock) { pthread_mutex_unlock(&lock->m); }
void RecursiveLock_Init(RecursiveLock* lock) { lock->ok = 0; initOnce(&lock->m, &lock->ok, true); }
void RecursiveLock_Lock(RecursiveLock* lock) { if (!lock->ok) RecursiveLock_Init(lock); pthread_mutex_lock(&lock->m); }
int RecursiveLock_TryLock(RecursiveLock* lock) { if (!lock->ok) RecursiveLock_Init(lock); return pthread_mutex_trylock(&lock->m) == 0 ? 0 : 1; }
void RecursiveLock_Unlock(RecursiveLock* lock) { pthread_mutex_unlock(&lock->m); }

void LightEvent_Init(LightEvent* event, ResetType reset_type) {
    pthread_mutex_init(&event->m, nullptr);
    pthread_cond_init(&event->c, nullptr);
    event->state = 0;
    event->type = reset_type;
    event->ok = 1;
}
void LightEvent_Clear(LightEvent* event) {
    pthread_mutex_lock(&event->m);
    event->state = 0;
    pthread_mutex_unlock(&event->m);
}
void LightEvent_Pulse(LightEvent* event) {
    pthread_mutex_lock(&event->m);
    pthread_cond_broadcast(&event->c);
    pthread_mutex_unlock(&event->m);
}
void LightEvent_Signal(LightEvent* event) {
    pthread_mutex_lock(&event->m);
    event->state = 1;
    if (event->type == RESET_ONESHOT) pthread_cond_signal(&event->c);
    else pthread_cond_broadcast(&event->c);
    pthread_mutex_unlock(&event->m);
}
int LightEvent_TryWait(LightEvent* event) {
    pthread_mutex_lock(&event->m);
    const int s = event->state;
    if (s && event->type == RESET_ONESHOT) event->state = 0;
    pthread_mutex_unlock(&event->m);
    return s ? 1 : 0;
}
void LightEvent_Wait(LightEvent* event) {
    pthread_mutex_lock(&event->m);
    while (!event->state) pthread_cond_wait(&event->c, &event->m);
    if (event->type == RESET_ONESHOT) event->state = 0;
    pthread_mutex_unlock(&event->m);
}
int LightEvent_WaitTimeout(LightEvent* event, s64 timeout_ns) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const s64 ns = (s64)ts.tv_nsec + (timeout_ns > 0 ? timeout_ns : 0);
    ts.tv_sec += (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    pthread_mutex_lock(&event->m);
    int rc = 0;
    while (!event->state && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&event->c, &event->m, &ts);
    const int signalled = event->state;
    if (signalled && event->type == RESET_ONESHOT) event->state = 0;
    pthread_mutex_unlock(&event->m);
    return signalled ? 0 : 1;
}

void LightSemaphore_Init(LightSemaphore* s, s16 initial_count, s16 max_count) {
    pthread_mutex_init(&s->m, nullptr);
    pthread_cond_init(&s->c, nullptr);
    s->count = initial_count;
    s->max = max_count;
    s->ok = 1;
}
void LightSemaphore_Acquire(LightSemaphore* s, s32 count) {
    pthread_mutex_lock(&s->m);
    while (s->count < count) pthread_cond_wait(&s->c, &s->m);
    s->count -= count;
    pthread_mutex_unlock(&s->m);
}
int LightSemaphore_TryAcquire(LightSemaphore* s, s32 count) {
    pthread_mutex_lock(&s->m);
    const bool ok = s->count >= count;
    if (ok) s->count -= count;
    pthread_mutex_unlock(&s->m);
    return ok ? 0 : 1;
}
void LightSemaphore_Release(LightSemaphore* s, s32 count) {
    pthread_mutex_lock(&s->m);
    s->count += count;
    if (s->count > s->max) s->count = s->max;
    pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
}

// ------------------------------------------------------------------ memory
// "Linear" and VRAM memory are ordinary heap memory here. The game sizes its caches by linearSpaceFree /
// vramSpaceFree, so they report a budget (larger than the 3DS had, well inside what the Vita can give).
static std::mutex g_memLock;
static size_t g_linearUsed = 0, g_vramUsed = 0;
static const size_t kLinearBudget = 160u << 20, kVramBudget = 64u << 20;

struct AllocHead { u32 magic; u32 size; u32 kind; u32 offset; };
static const u32 kMagic = 0x4C494E45;   // "LINE"

static void* allocKind(size_t size, size_t align, u32 kind) {
    if (align < 128) align = 128;
    const size_t pad = (sizeof(AllocHead) + align - 1) / align * align;
    u8* raw = (u8*)malloc(size + pad + align);
    if (!raw) return nullptr;
    uintptr_t p = ((uintptr_t)raw + pad + align - 1) & ~(uintptr_t)(align - 1);
    AllocHead* h = (AllocHead*)(p - sizeof(AllocHead));
    h->magic = kMagic;
    h->size = (u32)size;
    h->kind = kind;
    h->offset = (u32)(p - (uintptr_t)raw);
    std::lock_guard<std::mutex> g(g_memLock);
    (kind ? g_vramUsed : g_linearUsed) += size;
    return (void*)p;
}

static AllocHead* headOf(void* mem) {
    if (!mem) return nullptr;
    AllocHead* h = (AllocHead*)((u8*)mem - sizeof(AllocHead));
    return h->magic == kMagic ? h : nullptr;
}

void* linearAlloc(size_t size) { return allocKind(size, 0x80, 0); }
void* linearMemAlign(size_t size, size_t alignment) { return allocKind(size, alignment, 0); }
size_t linearGetSize(void* mem) {
    AllocHead* h = headOf(mem);
    return h ? h->size : 0;
}
void linearFree(void* mem) {
    AllocHead* h = headOf(mem);
    if (!h) return;
    {
        std::lock_guard<std::mutex> g(g_memLock);
        (h->kind ? g_vramUsed : g_linearUsed) -= h->size;
    }
    h->magic = 0;
    free((u8*)mem - h->offset);
}
void* linearRealloc(void* mem, size_t size) {
    if (!mem) return linearAlloc(size);
    AllocHead* h = headOf(mem);
    if (!h) return nullptr;
    void* n = allocKind(size, 0x80, h->kind);
    if (!n) return nullptr;
    memcpy(n, mem, h->size < size ? h->size : size);
    linearFree(mem);
    return n;
}
u32 linearSpaceFree(void) {
    std::lock_guard<std::mutex> g(g_memLock);
    return g_linearUsed >= kLinearBudget ? 0 : (u32)(kLinearBudget - g_linearUsed);
}
void* vramAlloc(size_t size) { return allocKind(size, 0x80, 1); }
void* vramMemAlign(size_t size, size_t alignment) { return allocKind(size, alignment, 1); }
void vramFree(void* mem) { linearFree(mem); }
u32 vramSpaceFree(void) {
    std::lock_guard<std::mutex> g(g_memLock);
    return g_vramUsed >= kVramBudget ? 0 : (u32)(kVramBudget - g_vramUsed);
}

s64 osGetMemRegionSize(MemRegion region) {
    (void)region;
    return 256ll << 20;
}
s64 osGetMemRegionUsed(MemRegion region) {
    (void)region;
    std::lock_guard<std::mutex> g(g_memLock);
    return (s64)(g_linearUsed + g_vramUsed) + (48ll << 20);
}
s64 osGetMemRegionFree(MemRegion region) { return osGetMemRegionSize(region) - osGetMemRegionUsed(region); }
void osSetSpeedupEnable(bool enable) { (void)enable; }
u8 osGetWifiStrength(void) { return 3; }

Result DSP_FlushDataCache(const void* address, u32 size) { (void)address; (void)size; return 0; }
Result DSP_InvalidateDataCache(const void* address, u32 size) { (void)address; (void)size; return 0; }

// ------------------------------------------------------------------ gfx / apt
static bool g_3d = false, g_wide = false;
void gfxInitDefault(void) {}
void gfxExit(void) {}
void gfxSet3D(bool enable) { g_3d = enable; }
bool gfxIs3D(void) { return g_3d; }
void gfxSetWide(bool enable) { g_wide = enable; }
bool gfxIsWide(void) { return g_wide; }
void gfxSwapBuffers(void) {}
void gfxFlushBuffers(void) {}
void gspWaitForVBlank(void) { svcSleepThread(16666666); }
void gxCmdQueueInterrupt(GSPGPU_Event irq) { (void)irq; }
void gspWaitForAnyEvent(void) { svcSleepThread(1000000); }

// consoleInit / consoleSelect / consoleClear: console.cpp
bool envIsHomebrew(void) { return true; }
Result APT_CheckNew3DS(bool* out) {
    if (out) *out = true;
    return 0;
}
float osGet3DSliderState(void) { return 0.0f; }

Result APT_SetAppCpuTimeLimit(u32 percent) { (void)percent; return 0; }
Result APT_GetAppCpuTimeLimit(u32* percent) {
    if (percent) *percent = 30;
    return 0;
}
void aptSetHomeAllowed(bool allowed) { (void)allowed; }
Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }

}  // extern "C"

// crash.cpp (the 3DS exception handler) is not built here
void crashInstall() {}
void crashCheckCmdBuf() {}

// render.cpp's early GPU kick reaches into citro3d's context (its GX queue). An empty queue fails the game's own
// layout check, so the kick stays off: vitaGL / GL submit work as they go anyway.
extern "C" {
gxCmdQueue_s __C3D_Context = {};
void gxCmdQueueRun(gxCmdQueue_s* queue) { (void)queue; }
void gxCmdQueueStop(gxCmdQueue_s* queue) { (void)queue; }
void gxCmdQueueClear(gxCmdQueue_s* queue) { (void)queue; }
}

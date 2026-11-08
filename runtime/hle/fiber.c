// nn::os fibers on host fibers. A guest fiber switches stacks by hand (nn::os::detail::FiberImplByHorizon), which a
// recompiled program can't follow: its guest functions are host functions, and the host call stack would stay behind
// when only the guest sp moved. So the SDK's public fiber API is host code: each guest FiberType gets a host fiber
// (its own host stack) and its own Cpu (its own guest registers), and SwitchToFiber is a host fiber switch. A fiber's
// registers stay in its Cpu while it is switched out; nothing is copied. A fiber resumed on another guest thread takes
// that thread's TLS (tpidr / tpidrro) and KThread.
//
// FiberFunction is FiberType *(*)(void *arg): when it returns, the fiber ends and the returned fiber runs next.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../hle.h"
#include "../kernel.h"

#ifndef _WIN32
Cpu *fiber_current_cpu(void) { return NULL; }  // (no fibers on this host yet)
#else
#define FIBER_HOST_STACK (16u << 20)  // reserved (committed as used): recompiled code is stack hungry

typedef struct HostFiber {
    uint64_t guest;               // the guest FiberType
    void *host;                   // the host fiber
    uint64_t func, arg, stack_top;
    Cpu cpu;                      // this fiber's guest registers
    struct HostFiber *next;
} HostFiber;

static pthread_mutex_t g_fibers_lock = PTHREAD_MUTEX_INITIALIZER;
static HostFiber *g_fibers;

static __thread HostFiber *t_cur;  // the fiber this thread runs (NULL: the thread's own context)
static __thread void *t_host;      // this thread as a host fiber (ConvertThreadToFiber), once it switched

// (thread-local accessors that the compiler can't move across a fiber switch: after one, this code may run on another
// thread with other thread-locals)
__attribute__((noinline)) static HostFiber *cur_get(void) { return t_cur; }
__attribute__((noinline)) static void cur_set(HostFiber *f) { t_cur = f; }
__attribute__((noinline)) static void *thread_host(void) {
    if (!t_host) {
        t_host = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        if (!t_host) t_host = GetCurrentFiber();  // (already a fiber)
    }
    return t_host;
}

Cpu *fiber_current_cpu(void) {
    HostFiber *f = cur_get();
    return f ? &f->cpu : NULL;
}

static HostFiber *find(uint64_t guest) {
    pthread_mutex_lock(&g_fibers_lock);
    HostFiber *f = g_fibers;
    while (f && f->guest != guest) f = f->next;
    pthread_mutex_unlock(&g_fibers_lock);
    return f;
}

// This fiber's Cpu belongs to whichever guest thread runs it now: that thread's TLS and KThread, which notes the
// registers it runs on (KThread.active: the hang report's frames).
static void adopt_thread(Cpu *c) {
    Cpu *tc = kernel_thread_cpu();
    if (!tc) return;
    ((KThread *)tc->thread)->active = tc == c ? NULL : c;
    if (tc == c) return;
    c->tpidr = tc->tpidr;
    c->tpidrro = tc->tpidrro;
    c->thread = tc->thread;
}

static void switch_to(Cpu *c, uint64_t guest) {
    HostFiber *to = guest ? find(guest) : NULL;
    if (guest && !to) FATAL("[fiber] SwitchToFiber(%llx): not an initialized fiber\n", (unsigned long long)guest);
    void *self = thread_host();  // (the thread's own context must be a host fiber before it can switch away)
    HostFiber *from = cur_get();
    if (from == to) return;
    cur_set(to);
    SwitchToFiber(to ? to->host : self);
    adopt_thread(c);  // (resumed: maybe on another thread)
}

static void WINAPI fiber_main(void *p) {
    HostFiber *f = p;
    Cpu *c = &f->cpu;
    adopt_thread(c);
    c->x[0] = f->arg;
    c->sp = f->stack_top & ~15ull;
    guest_call(c, f->func);
    uint64_t next = c->x[0];
    if (!next) FATAL("[fiber] fiber %llx ended without a next fiber\n", (unsigned long long)f->guest);
    for (;;) switch_to(c, next);  // (a host fiber must never return: that would end the thread)
}

// void nn::os::InitializeFiber(FiberType*, FiberFunction, void* arg, void* stack, size_t stack_size, int flags)
HLE_FUNC("_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", fiber_initialize) {
    uint64_t guest = c->x[0];
    HostFiber *f = find(guest);
    if (f) {
        if (f == cur_get()) FATAL("[fiber] InitializeFiber on the running fiber %llx\n", (unsigned long long)guest);
        DeleteFiber(f->host);
    } else {
        f = calloc(1, sizeof *f);
        f->guest = guest;
        pthread_mutex_lock(&g_fibers_lock);
        f->next = g_fibers;
        g_fibers = f;
        pthread_mutex_unlock(&g_fibers_lock);
    }
    f->func = c->x[1];
    f->arg = c->x[2];
    f->stack_top = c->x[3] + c->x[4];
    memset(&f->cpu, 0, sizeof f->cpu);
    f->cpu.excl_addr = ~0ull;
    f->cpu.fpcr = c->fpcr;
    f->cpu.fpsr = c->fpsr;
    f->host = CreateFiberEx(64 << 10, FIBER_HOST_STACK, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->host) FATAL("[fiber] CreateFiberEx failed (%lu)\n", GetLastError());
    static int nlog;
    if (nlog++ < 8)
        LOG("[fiber] init %llx func %llx arg %llx stack %llx+%llx flags %llx\n", (unsigned long long)guest,
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
            (unsigned long long)c->x[4], (unsigned long long)c->x[5]);
}

// void nn::os::FinalizeFiber(FiberType*)
HLE_FUNC("_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", fiber_finalize) {
    uint64_t guest = c->x[0];
    pthread_mutex_lock(&g_fibers_lock);
    HostFiber **pp = &g_fibers;
    while (*pp && (*pp)->guest != guest) pp = &(*pp)->next;
    HostFiber *f = *pp;
    if (f) *pp = f->next;
    pthread_mutex_unlock(&g_fibers_lock);
    if (!f) return;
    if (f == cur_get()) FATAL("[fiber] FinalizeFiber on the running fiber %llx\n", (unsigned long long)guest);
    DeleteFiber(f->host);
    free(f);
}

// void nn::os::SwitchToFiber(FiberType*)
HLE_FUNC("_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", fiber_switch) { switch_to(c, c->x[0]); }

// FiberType* nn::os::GetCurrentFiber()
HLE_FUNC("_ZN2nn2os15GetCurrentFiberEv", fiber_current) {
    HostFiber *f = cur_get();
    c->x[0] = f ? f->guest : 0;
}
#endif

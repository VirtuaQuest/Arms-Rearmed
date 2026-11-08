// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"
#include "../settings.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c, runtime/nvn/pace.c)
volatile uint64_t g_vsync_ns;     // plat_now_ns() of the latest (stored after the count: count >= the vsync it's from)
uint64_t g_native_window;

// The display's vsync period. MUA3 waits for two vsyncs a frame (present_pace, interval 2) and keeps game time by the
// clock, so the frame rate setting (frame_rate 30 / 60 / 120; LARECOMP_FPS overrides it) runs the display at twice
// that; its own 30 fps frame limiter is shortened to match (runtime/kernel.c, SleepThread) and its time step follows
// (game_hooks.c game_frame_rate). (test) LARECOMP_VSYNC_HZ=<hz>: any rate from the start; LARECOMP_FPS_AT=<frame>:<hz>:
// that rate from that presented frame on (objects.c: a scripted route gets through the loads at the game's own rate).
volatile uint64_t g_vsync_period = 16666667;
int vi_frame_rate(void) {  // 30, 60 or 120: what the display period gives (two vsyncs a frame)
    uint64_t p = g_vsync_period;
    return p <= 4200000 ? 120 : p <= 8400000 ? 60 : 30;
}
void vi_apply_fps(void) {
    const char *e = getenv("LARECOMP_FPS");
    int fps = e ? atoi(e) : g_settings.frame_rate;
    fps = fps >= 120 ? 120 : fps >= 60 ? 60 : 30;
    g_vsync_period = 500000000ull / (uint64_t)fps;
    const char *hz = getenv("LARECOMP_VSYNC_HZ");
    if (hz && atoi(hz) >= 30 && atoi(hz) <= 480) g_vsync_period = 1000000000ull / (uint64_t)atoi(hz);
    LOG("[vi] display at %.0f Hz (%d fps)\n", 1e9 / (double)g_vsync_period, vi_frame_rate());
}
static void *vsync_thread(void *arg) {
    (void)arg;
    vi_apply_fps();
    uint64_t period = g_vsync_period;
    plat_thread_realtime(period);
    plat_thread_perf_core(PLAT_PERF_VSYNC);
    uint64_t next = plat_now_ns();
    for (;;) {
        period = __atomic_load_n(&g_vsync_period, __ATOMIC_RELAXED);
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            // (~100 ms late every frame: macOS coalescing the timers of a thread that lost its real-time policy while
            // the app isn't frontmost - checked and set again here; the window's focus says whether it was frontmost)
            static int warned;
            int rt = plat_thread_realtime_check(period);
            extern bool ui_window_focused(void);
            if (warned++ < 40) LOG("[vi] vsync timer late by %llums (window focused %d, real-time policy %s)\n", (unsigned long long)((now - next) / 1000000),
                                    ui_window_focused(), rt == 0 ? "holds" : rt == 1 ? "was gone: set again" : "unknown");
            next = now;  // don't burst-signal to catch up
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_vsync_ns, now, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }
// (the renderer fits the game's image to the window itself: runtime/nvn/output_scale.c)
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling_mode) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }

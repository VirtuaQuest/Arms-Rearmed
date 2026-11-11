// Game hooks: the places where the shared runtime (input, renderer, file system, frame pacing) asks the game-specific
// code something. In LAS Recomp these were Link's Awakening features (warp, mirror mode, widescreen camera, pots, pets,
// save editor, extra flags); for MUA3 they answer "nothing special" until a MUA3 counterpart is written.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../hle.h"

// ---- game state
int game_in_play(void) { return 0; }  // (not located yet: the menu's save editor and LED health stay off)
bool world_on_screen(void) { return false; }
const char *game_level_name(void) { return ""; }
uint64_t game_camera_zone(void) { return 0; }
int cheats_link_health(int *max) { if (max) *max = 0; return -1; }

// ---- frame rate: the game's own frame limiter (main thread, f_7000839000) sleeps until its frame start + 1 / the
// target rate at [frame object + 0x98] (30.0); the frame object is a global, its logic frame count at +0x68
#define FRAME_OBJ 0x70045a7158ull
bool game_frame_limiter_sleep(uint64_t lr) { return lr == 0x70008395b4ull; }  // (its SleepThread: LARECOMP_LOG_LIMITER)

// The game's time step (f_7000839000): dt = max(measured frame time, its frame interval's step), at most 1/30 s, where
// the step comes from a table (0x70019f33d8): 1/60 s for frame interval 1, 1/30 s for 2 ([[0x7002188a00] + 0x60];
// MUA3 runs with 2). So above 30 fps with interval 2 every frame still advanced 1/30 s (double speed at 60). Above 30
// the frame rate setting makes the interval 1 every frame (present_pace, runtime/nvn/objects.c), and at 120 the
// table's interval-1 step 1/120 s: then dt is the measured time at any frame rate up to the setting's. The limiter's
// target rate becomes the setting's too.
void game_frame_rate(int fps) {
    // ARMS: nothing. The addresses below are MUA3's (its time step table, frame object and present interval); in
    // ARMS's memory they are other data, and this ran at every present (it rewrote a 60.0f there to 30.0f). ARMS
    // presents every vsync at 60 Hz and steps its fights once a frame (runtime/hle/vi.c).
    (void)fps;
    if (fps) return;
    uint64_t p = *(volatile uint64_t *)(uintptr_t)0x7002188a00ull;
    volatile float *step1 = (volatile float *)(uintptr_t)0x70019f33d8ull;
    float want = fps >= 120 ? 1.0f / 120 : 1.0f / 60;
    if (*step1 != want && (*step1 == 1.0f / 60 || *step1 == 1.0f / 120)) *step1 = want;
    volatile float *target = (volatile float *)(uintptr_t)(FRAME_OBJ + 0x98);
    if (*target != (float)fps && (*target == 30.0f || *target == 60.0f || *target == 120.0f)) *target = (float)fps;
    if (p < 0x7000000000ull || p >= 0x8000000000ull) return;
    volatile uint32_t *interval = (volatile uint32_t *)(uintptr_t)(p + 0x60);
    static uint32_t saved;
    if (fps > 30 && *interval == 2) saved = 2, *interval = 1;
    else if (fps <= 30 && saved && *interval == 1) *interval = saved, saved = 0;
    // LARECOMP_LOG_GAMETIME=1: game time per second of real time - its dt ([p + 0x18]) times the logic frames since
    // the last present (the logic thread can run more frames than are presented)
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_LOG_GAMETIME") != NULL;
    if (log) {
        static double sum;
        static uint64_t t0, presents, logic, last_count;
        uint64_t now = plat_now_ns(), count = *(volatile uint64_t *)(uintptr_t)(FRAME_OBJ + 0x68);
        if (!t0) t0 = now, last_count = count;
        sum += *(volatile float *)(uintptr_t)(p + 0x18) * (double)(count - last_count);
        logic += count - last_count, last_count = count, presents++;
        if (now - t0 >= 4000000000ull) {
            uint64_t w = *(volatile uint64_t *)(uintptr_t)(FRAME_OBJ + 0x50);  // (the world: its step count at 0x1e566c)
            LOG("[gametime] %.2f s of game time in %.2f s (%llu logic frames, %llu presented, interval %u, target %.0f, "
                "steps %u)\n", sum, (now - t0) / 1e9, (unsigned long long)logic, (unsigned long long)presents, *interval,
                *target, w >> 32 == 0x72 ? *(volatile uint32_t *)(uintptr_t)(w + 0x1e566c) : 0u);
            sum = 0, presents = logic = 0, t0 = now;
        }
    }
}

// ---- version, for the log's [info] line and the shader caches' tag (runtime/nvn/shader_prep.c): the Makefile's
// VERSION (with -dev in a source build)
#ifndef ARMS_VERSION
#define ARMS_VERSION "dev"
#endif
const char *larecomp_version(void) { return ARMS_VERSION; }

// ---- input (runtime/hle/input.c)
bool mirror_input(void) { return false; }
uint64_t pets_menu_input(uint64_t b, int32_t st[4]) { (void)st; return b; }
uint64_t tracker_map_input(uint64_t b, int32_t st[4]) { (void)st; return b; }
void input_note_rstick(int32_t ry) { (void)ry; }
int32_t g_stick_lx, g_stick_ly;
uint64_t g_stick_freeze_until;
int32_t g_test_lx, g_test_ly;
uint64_t g_test_stick_until;
uint64_t g_test_buttons, g_test_buttons_until;
char g_window_shot_request[256];

// ---- file system (runtime/services/fs.c)
void romfs_file_read_hook(const char *path) { (void)path; }
void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path, (void)addr, (void)off, (void)size;
}
void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path, (void)addr, (void)off, (void)size;
}
bool flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path, (void)addr, (void)off, (void)size;
    return true;  // (false fails the save write)
}
void save_editor_frame(uint64_t frame) { (void)frame; }

// ---- renderer (runtime/nvn/vulkan): per-draw tints and the widescreen camera
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) {
    (void)kind, (void)ui, (void)ui_size, (void)tint;
}
bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size) { (void)ui, (void)ui_size; return false; }
bool mirror_draw(bool composite) { (void)composite; return false; }
bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) { (void)sw, (void)sh, (void)w, (void)h; return false; }
void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h) {
    (void)composite, (void)sw, (void)sh, (void)w, (void)h;
}
bool pets_hide_item_cursor(void) { return false; }
bool pot_glow_active(void) { return false; }
bool pot_level_active(void) { return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m, (void)tint, (void)mesh; return false; }
bool widescreen_allowed(void) { return false; }
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
void widescreen_frame_end(uint64_t frame) { (void)frame; }
void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw, double *vh) {
    (void)frame, (void)shape, (void)vx, (void)vy, (void)vw, (void)vh;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;  // the whole frame, the viewport as it is
}
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }
void widescreen_set_factor(float f) { (void)f; }
bool widescreen_ui_pane_partial(const float *u, uint64_t size) { (void)u, (void)size; return false; }

// ARMS widescreen (Video tab, Aspect ratio = Unlocked): a window wider than 16:9 (21:9, 32:9) shows more of the scene
// instead of a stretched picture.
//
// The renderer already draws scaled render targets wider than the game asks for when widescreen is allowed
// (runtime/nvn/output_scale.c: allocation scale ax = h x shape, ay = h, shape = window aspect / 16:9). What makes the
// wider targets a wider *view* is the camera's frustum. ARMS keeps its scene camera in constant buffer slot 0 (a
// sead/agl view block, rows of 4 floats: 0-2 View, 3-6 ViewProj, 7-10 Proj, 11-13 InvView, 21-34 the previous frame's
// copies, 42-49 the shadow cascades); a scene camera is recognised by its projection's last row (row 10) being
// (0, 0, -1, 0). For draws into the scene's widened targets with such a camera, the vertex shader's clip position is
// scaled after the game's own transform (runtime/nvn/maxwell/translate.c: gl_Position.xy *= rscale[0].xy; exec.c
// sets rscale[0] per draw): x by 1 / shape, so the frustum widens by the window's shape at the game's own vertical
// field of view (taller windows: y by shape, the horizontal view kept). The camera block itself is left as the game
// wrote it - scaling its projection made the fighters black: their shading reads the projection for more than the
// position. Shadow cameras (orthographic), the small targets the game renders data into and everything drawn into the
// presented image (the 2D UI) keep the game's own transform.
//
// The 2D UI stays in a centred 16:9 box (exec.c: draws into the presented image or the game's UI layer get a uniform
// scale); the composites that read the scene fill the window.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

static float g_shape = 1.0f;  // window aspect / 16:9 while the camera is widened (device.c, every frame), else 1

bool widescreen_allowed(void) { return g_settings.aspect == ASPECT_UNLOCKED; }
void widescreen_set_factor(float f) { g_shape = f > 0.01f ? f : 1.0f; }

// A draw's camera block (constant buffer slot 0): when it's a perspective scene camera and the window is widened, the
// clip-space scales for x and y (*kx, *ky: left at 1 otherwise). true: scaled.
bool widescreen_scene_camera(const float *block, uint64_t size, float *kx, float *ky) {
    static int nocam = -1;  // (test) LARECOMP_WIDE_NOCAM=1: the targets widen, the frustum stays the game's (stretched)
    if (nocam < 0) nocam = getenv("LARECOMP_WIDE_NOCAM") != NULL;
    if (nocam || !widescreen_allowed() || fabsf(g_shape - 1.0f) < 0.005f || !block || size < 11 * 16) return false;
    const float *p = block + 7 * 4;  // Proj, rows 7-10
    if (p[12] != 0.0f || p[13] != 0.0f || p[14] != -1.0f || p[15] != 0.0f || !(p[0] > 0.0f) || !(p[5] > 0.0f)) return false;
    if (g_shape > 1.0f) *kx = 1.0f / g_shape;
    else *ky = g_shape;
    return true;
}

// A 2D UI draw's pane (its vertex constant buffer slot 0: nn::ui2d's per-pane block - rows 0-3 the projection, 4-6
// the pane's model-view, row 33 the pane's rectangle w, h, left, top in the 1280x720 layout): 1 when it covers the
// whole 16:9 screen at the layout's own size and scale (a layout backdrop: character select's swirl), 5 when it
// covers it otherwise (a full-screen picture: the strap screen's, the title's), 2 when it spans its whole width only
// (a band), 3 / 4 when it reaches the left / right edge only, 0 otherwise (or not a ui2d pane).
int widescreen_ui_pane(const float *u, uint64_t size) {
    if (!u || size < 34 * 16 || u[12] != 0.0f || u[13] != 0.0f || u[14] != -1.0f || u[15] != 0.0f) return 0;
    const float *mv = u + 16, *r = u + 33 * 4;
    if (!(r[0] > 0.0f) || !(r[1] > 0.0f)) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int c = 0; c < 4; c++) {
        float px = r[2] + (c & 1 ? r[0] : 0), py = r[3] - (c & 2 ? r[1] : 0);
        float v[4] = {mv[0] * px + mv[1] * py + mv[3], mv[4] * px + mv[5] * py + mv[7], mv[8] * px + mv[9] * py + mv[11], 1};
        float cl[4];
        for (int i = 0; i < 4; i++) cl[i] = u[i * 4] * v[0] + u[i * 4 + 1] * v[1] + u[i * 4 + 2] * v[2] + u[i * 4 + 3];
        if (!(cl[3] > 1e-6f)) return 0;
        float nx = cl[0] / cl[3], ny = cl[1] / cl[3];
        x0 = fminf(x0, nx), x1 = fmaxf(x1, nx), y0 = fminf(y0, ny), y1 = fmaxf(y1, ny);
    }
    if (x0 <= -0.98f && x1 >= 0.98f && y0 <= -0.98f && y1 >= 0.98f) {  // (the layout's own backdrop size at scale 1: 1)
        bool native = fabsf(r[0] - 1280) < 1 && fabsf(r[1] - 720) < 1 && fabsf(mv[0] - 1) < 0.01f && fabsf(mv[5] - 1) < 0.01f;
        return native ? 1 : 5;
    }
    if (x0 <= -0.98f && x1 >= 0.98f) return 2;
    if (x0 <= -0.98f && x1 > -0.98f) return 3;  // (it reaches the left edge)
    if (x1 >= 0.98f && x0 < 0.98f) return 4;    // (the right edge)
    return 0;
}

// ---- the rest of LAS Recomp's widescreen hooks (Link's Awakening's camera, backdrops, effects): nothing for ARMS
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
// (test) LARECOMP_ASPECT_AT=<frame>:<unlocked|original>,...: the Aspect ratio setting changed at those frames, as the
// menu would (a change while the game runs)
void widescreen_frame_end(uint64_t frame) {
    const char *e = getenv("LARECOMP_ASPECT_AT");
    for (const char *p = e; p && *p;) {
        char *q;
        long at = strtol(p, &q, 10);
        if (*q != ':') break;
        bool unlocked = !strncmp(q + 1, "unlocked", 8);
        if ((long)frame == at) {
            g_settings.aspect = unlocked ? ASPECT_UNLOCKED : ASPECT_ORIGINAL;
            LOG("[widescreen] frame %llu: aspect -> %s (test)\n", (unsigned long long)frame, unlocked ? "unlocked" : "original");
        }
        p = strchr(q, ',');
        if (p) p++;
    }
    // (test) LARECOMP_RES_AT=<frame>:<match|dynamic|1..4>,...: Internal resolution changed at those frames
    e = getenv("LARECOMP_RES_AT");
    for (const char *p = e; p && *p;) {
        char *q;
        long at = strtol(p, &q, 10);
        if (*q != ':') break;
        if ((long)frame == at) {
            const char *v = q + 1;
            g_settings.res_mode = !strncmp(v, "match", 5) ? RES_MATCH : !strncmp(v, "dynamic", 7) ? RES_DYNAMIC : atoi(v);
            LOG("[widescreen] frame %llu: resolution -> %d (test)\n", (unsigned long long)frame, g_settings.res_mode);
        }
        p = strchr(q, ',');
        if (p) p++;
    }
}
void widescreen_present_crop(uint64_t frame, double shape, float crop[4], double *vx, double *vy, double *vw, double *vh) {
    (void)frame, (void)shape, (void)vx, (void)vy, (void)vw, (void)vh;
    crop[0] = crop[1] = 0, crop[2] = crop[3] = 1;  // the whole frame, the viewport as it is
}
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u, (void)size; }
bool widescreen_ui_pane_partial(const float *u, uint64_t size) { (void)u, (void)size; return false; }

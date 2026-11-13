// Button prompts in the controller's own style, and the Hero Mode HUD icon hidden: edits made to the game's own UI
// textures as they are uploaded (ideas from the GameBanana mods "XBOX UI", "PS4 UI Buttons" and "No Hero Mode
// Indicator", credited in README.md; none of their files are used and no game art ships with the recomp).
//
// The prompt glyphs are 48x48 layout textures (BtnA_00, BtnL_01, ... in the UI archives' __Combined.bntx: BC5
// masks, BC3 / ASTC colour versions) and the pictogram font's sheet (pictogram.bffnt: RGBA8, the inline icons of
// message text). They are recognised by content: the user's own UI archives are read once and each texture the game
// uploads is compared with the known ones (same size, same bytes). A known glyph keeps the game's button shape and
// colours; only its label is redrawn: the old label (the pixels inside the shape that differ from the body colour)
// is painted over with the body colour and the new label (Xbox letters, PlayStation symbols, drawn here with a
// system font / simple geometry) composited in its place. Changed 4x4 blocks are re-encoded to the original format.
//
// Labels follow the controller bindings (runtime/hle/bindings.c; by position unless remapped): each Switch button's
// glyph names the controller button now driving it. Defaults: Switch A (east) = Xbox B / circle, B (south) = Xbox A /
// cross, X (north) = Y / triangle, Y (west) = X / square; L / R = LB / RB, L1 / R1; ZL / ZR = LT / RT, L2 / R2;
// Plus / Minus = Menu / View, Options / Create. A button remapped on a Nintendo-style pad gets Nintendo letters; one
// moved to a stick click / the d-pad gets LS / RS (L3 / R3) or an arrow. Stick glyphs stay. (The keyboard has no
// glyphs of its own: keyboard bindings don't change the prompts.)
#include "texpatch.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <SDL3/SDL_keyboard.h>  // (SDL_GetScancodeName: ARMS's key cap icons)
#include <SDL3/SDL_scancode.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../rando.h"  // (no seeds: the stubs)
#include "../settings.h"
#include "astc.h"
#include "recolour.h"
#include "skin_face.h"
#include "tegra.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"

enum { K_BC5, K_BC3, K_ASTC4, K_RGBA8, K_BC1, K_BC4, K_BC7 };
enum { R_A = 1, R_B, R_X, R_Y, R_L, R_R, R_ZL, R_ZR, R_PLUS, R_MINUS, R_HERO, R_SHEET, R_BOTTLE, R_BOTTLE_MODEL, R_FAIRY_MODEL,
       R_ITEM_CURSOR, R_TUNIC, R_TUNIC_RED, R_TUNIC_BLUE, R_HEART, R_POWDER_MODEL, R_DGN_MAP, R_MAP_MODEL, R_POWER_PIECE_MODEL,
       R_SKIN, R_ARMS_SHEET, R_BRAND };
// ARMS's icon font glyphs beyond the buttons R_A..R_MINUS: the sticks (moved: the keyboard's movement keys) and their
// presses (ARMS: guard is the left stick's press)
enum { AR_LSTICK = 100, AR_RSTICK, AR_LS_PRESS, AR_RS_PRESS };
// STYLE_KB: the keyboard / mouse bindings, drawn as key caps and a mouse (ARMS's icon font: runtime/nvn/texpatch.c
// arms_sheet_apply)
enum { STYLE_NINTENDO, STYLE_XBOX, STYLE_PS, STYLE_KB };
// labels: R_A..R_MINUS = the button that role is on by default; or these
enum { L_LS = 32, L_RS, L_DUP, L_DDOWN, L_DLEFT, L_DRIGHT, L_OTHER };

typedef struct {
    uint8_t role, kind, srgb, bhl;  // bhl: log2 of the block height (GOBs) it is stored with
    uint16_t w, h, levels;
    uint32_t size;
    uint8_t *data;  // the texture's (block-linear) bytes as stored in the game files
    char name[32];
} Ref;

static Ref g_refs[96];
static int g_nrefs;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

// ------------------------------------------------------------------ game files (SARC / BNTX / BFFNT)
static uint8_t *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len ? *len : 1);
    if (fread(d, 1, *len, f) != *len) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static uint8_t *read_all(const char *rel, size_t *len) {  // (the game's own files, never a seed's)
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs", rel);
    return read_file(p, len);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool g_scanning_link;  // (Playerlink.bfres: its face maps are Link's skin)

static int role_of(const char *name) {
    // cosmetics: Link's skin (Cosmetics tab "Skin color"): his face's maps (eyes / mouth expressions, ears; the body
    // map's arms and hands are recoloured with the tunic map, tunic_apply)
    if (g_scanning_link && (!strncmp(name, "MI_eye_alb", 10) || !strncmp(name, "MI_mouth_alb", 12) ||
                            !strncmp(name, "MI_ear_alb", 10)))
        return R_SKIN;
    static const struct { const char *prefix; int role; } m[] = {
        {"BtnA_", R_A}, {"BtnB_", R_B}, {"BtnX_", R_X}, {"BtnY_", R_Y}, {"BtnL_", R_L}, {"BtnR_", R_R},
        {"BtnZL_", R_ZL}, {"BtnZR_", R_ZR}, {"BtnPlus_", R_PLUS}, {"BtnMinus_", R_MINUS},
        {"Spicy_00^t", R_HERO},  // Ld_Spicy (StartUp.arc): the HUD's Hero Mode icon (the file select uses Spicy_00^f)
        // the empty bottle's item icon and its 3D model's colour map (ItemBottle.bfres): identified only, never edited
        // (the gold Escape Bottle's draws are tinted: runtime/hle/escape.c)
        {"ItemBottle_00^w", R_BOTTLE}, {"MI_bottle_alb", R_BOTTLE_MODEL},
        // the fairy's colour map (ItemFairy.bfres): boss / mini-boss souls are a fairy drawn evil red (runtime/hle/souls.c)
        {"MI_fairy_alb", R_FAIRY_MODEL},
        // cosmetics: Link's tunics (recoloured on upload: green, and with a colour chosen the red / blue too, so the
        // Color Dungeon's tunics keep it - user 2026-10-11; the red map is also the recolour's mask)
        // and the hearts of Cm_HeartGauge (HUD, file select): Hp_00 = an empty heart (its own draw); each quarter of
        // a full one is one draw of Hp_01 (fill) + Hp_02 (frame, coloured by the layout's material) through the
        // Hp_04 mask; HeartUp = the life-up flash. Their draws are tinted.
        {"MI_body_alb.0", R_TUNIC}, {"MI_body_alb.1", R_TUNIC_RED}, {"MI_body_alb.2", R_TUNIC_BLUE},
        // the Magic Powder sack's colour map (ItemMagicPowder.bfres; after the tunic's: same name prefix): held up as
        // the Bomb Bag, tinted bomb blue then (runtime/hle/escape.c bomb_bag_tint)
        {"MI_body_alb", R_POWDER_MODEL},
        // the Hint Sheet (runtime/hle/hint_sheet.c): the dungeon map's UI icon (drawn in place of the first bottle's,
        // texpatch_hint_icon) and its 3D model's colour map (ItemDungeonMap.bfres: Tarin's hand-over, tinted)
        {"DgnMap_00^q", R_DGN_MAP}, {"MI_dungeonMap_alb", R_MAP_MODEL},
        // the Piece of Power's colour map (ItemPowerPiece.bfres): Triforce pieces are one drawn gold (runtime/hle/triforce.c)
        {"MI_powerPiece_alb", R_POWER_PIECE_MODEL},
        {"Hp_00^w", R_HEART}, {"Hp_01^w", R_HEART}, {"Hp_02^w", R_HEART}, {"HeartUp^w", R_HEART},
        // the Items page's cursor brackets: identified only (hidden while the companion card has the cursor, pets.c)
        {"ItemCursor_", R_ITEM_CURSOR},
    };
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++)
        if (!strncmp(name, m[i].prefix, strlen(m[i].prefix))) return m[i].role;
    return 0;
}

static void add_ref(int role, int kind, int srgb, uint32_t w, uint32_t h, uint32_t levels, const uint8_t *data, uint32_t size,
                    const char *name, uint32_t bhl) {
    for (int i = 0; i < g_nrefs; i++)  // (the archives share copies of the same textures)
        if (g_refs[i].w == w && g_refs[i].h == h && g_refs[i].size == size && !memcmp(g_refs[i].data, data, size)) return;
    if (g_nrefs == (int)(sizeof g_refs / sizeof *g_refs)) return;
    Ref *r = &g_refs[g_nrefs++];
    *r = (Ref){(uint8_t)role, (uint8_t)kind, (uint8_t)srgb, (uint8_t)bhl, (uint16_t)w, (uint16_t)h, (uint16_t)levels, size,
               malloc(size), ""};
    memcpy(r->data, data, size);
    strlcpy(r->name, name, sizeof r->name);
}

// Textures of a BNTX (`b` .. `b + len`): the glyph ones (or, sheet = true, its only one: a font sheet).
static void scan_bntx(const uint8_t *b, size_t len, bool sheet) {
    if (len < 0x40 || memcmp(b, "BNTX", 4) || memcmp(b + 0x20, "NX  ", 4)) return;
    uint32_t count = rd32(b + 0x24);
    uint64_t infos = rd64(b + 0x28);
    for (uint32_t i = 0; i < count && infos + 8 * i + 8 <= len; i++) {
        uint64_t brti = rd64(b + infos + 8 * i);
        if (brti + 0x90 > len || memcmp(b + brti, "BRTI", 4)) continue;
        const uint8_t *t = b + brti + 16;
        uint64_t name_at = rd64(t + 80), ptrs = rd64(t + 96);
        char name[64] = "";
        if (name_at + 2 < len) {
            uint16_t n = rd16(b + name_at);
            if (n < sizeof name && name_at + 2 + n <= len) { memcpy(name, b + name_at + 2, n); name[n] = 0; }
        }
        int role = sheet ? R_SHEET : role_of(name);
        if (!role || ptrs + 8 > len) continue;
        uint32_t fmt = rd32(t + 12), w = rd32(t + 20), h = rd32(t + 24), size = rd32(t + 64), levels = rd16(t + 6);
        uint64_t data = rd64(b + ptrs);
        if (data + size > len) continue;
        int kind = (fmt >> 8) == 0x1e ? K_BC5 : (fmt >> 8) == 0x1c ? K_BC3 : (fmt >> 8) == 0x2d ? K_ASTC4 :
                   (fmt >> 8) == 0x0b ? K_RGBA8 : (fmt >> 8) == 0x1a ? K_BC1 : -1;
        if (kind < 0 || (sheet && kind != K_RGBA8)) continue;
        add_ref(role, kind, (fmt & 0xff) == 6, w, h, levels, b + data, size, name, rd32(t + 36) & 7);
    }
}

static void scan_sarc(const char *rel) {
    size_t len;
    uint8_t *d = read_all(rel, &len);
    if (!d) return;
    if (len > 0x20 && !memcmp(d, "SARC", 4) && !memcmp(d + 0x14, "SFAT", 4)) {
        uint32_t data_off = rd32(d + 0xC);
        uint16_t n = rd16(d + 0x1A);
        const uint8_t *ent = d + 0x20, *names = d + 0x20 + 16 * n + 8;  // (SFNT header: 8 bytes)
        for (uint16_t i = 0; i < n && ent + 16 * (i + 1) <= d + len; i++) {
            uint32_t attr = rd32(ent + 16 * i + 4), s = rd32(ent + 16 * i + 8), e = rd32(ent + 16 * i + 12);
            const char *nm = (attr >> 24) ? (const char *)names + (attr & 0xffffff) * 4 : "";
            if ((const uint8_t *)nm >= d + len || strcmp(nm, "timg/__Combined.bntx") || data_off + e > len) continue;
            scan_bntx(d + data_off + s, e - s, false);
        }
    }
    free(d);
}

// ------------------------------------------------------------------ ARMS: the button icons' font (ARMSIconFont.bffnt)
// ARMS draws its button prompts as characters of an icon font, U+E0A0.. (the Switch's NintendoExt layout: outline
// set E0A0.., filled set E0E0..; the game tints them - the yellow footer, the orange L / R). Its sheet is one BC4
// mask 512x1024 (cells 43x49 in 11 columns, rows from the bottom, each stored upside down) in Font/BmpFont_<region>.szs
// (Yaz0 > SARC > ARMSIconFont.bffnt). The game files are read in place (romfs_file_find: no extracted romfs/).
typedef struct {
    uint16_t cw, ch, cols;  // cell size (the font's cell + 1) and cells a row
    int n;
    uint16_t code[256], glyph[256];
} ArmsFont;
static ArmsFont g_arms_font[8];
static int g_arms_nfonts;
static int8_t g_arms_font_of_ref[96];  // (g_refs index -> g_arms_font index, or -1)

static uint8_t *game_file(const char *rel, size_t *len) {
    extern bool romfs_file_find(const char *rel, uint64_t *off, uint64_t *size);  // runtime/services/fs.c
    extern ssize_t romfs_image_read(void *dst, uint64_t n, uint64_t off);
    uint64_t off, size;
    if (!romfs_file_find(rel, &off, &size)) return read_all(rel, len);  // (an extracted romfs/ folder)
    if (size > (64u << 20)) return NULL;
    uint8_t *d = malloc(size ? size : 1);
    if (romfs_image_read(d, size, off) != (ssize_t)size) { free(d); return NULL; }
    *len = size;
    return d;
}

static uint8_t *yaz0_decode(const uint8_t *d, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(d, "Yaz0", 4)) return NULL;
    size_t size = (size_t)d[4] << 24 | (size_t)d[5] << 16 | (size_t)d[6] << 8 | d[7], i = 16, n = 0;
    if (size > (256u << 20)) return NULL;
    uint8_t *o = malloc(size ? size : 1);
    while (n < size && i < len) {
        uint8_t code = d[i++];
        for (int b = 0; b < 8 && n < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= len) goto bad;
                o[n++] = d[i++];
                continue;
            }
            if (i + 1 >= len) goto bad;
            size_t b1 = d[i], b2 = d[i + 1], dist = ((b1 & 0xF) << 8 | b2) + 1, cnt = b1 >> 4;
            i += 2;
            if (!cnt) {
                if (i >= len) goto bad;
                cnt = (size_t)d[i++] + 0x12;
            } else {
                cnt += 2;
            }
            if (dist > n) goto bad;
            for (size_t k = 0; k < cnt && n < size; k++, n++) o[n] = o[n - dist];
        }
    }
    if (n < size) goto bad;
    *out_len = size;
    return o;
bad:
    free(o);
    return NULL;
}

// A file of a (little-endian) SARC by name, or NULL
static const uint8_t *sarc_file(const uint8_t *d, size_t len, const char *want, uint32_t *flen) {
    if (len < 0x20 || memcmp(d, "SARC", 4) || memcmp(d + 0x14, "SFAT", 4)) return NULL;
    uint32_t data_off = rd32(d + 0xC);
    uint16_t n = rd16(d + 0x1A);
    const uint8_t *ent = d + 0x20, *names = d + 0x20 + 16 * (size_t)n + 8;  // (SFNT header: 8 bytes)
    if ((size_t)(names - d) > len) return NULL;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t attr = rd32(ent + 16 * i + 4), s = rd32(ent + 16 * i + 8), e = rd32(ent + 16 * i + 12);
        if (!(attr >> 24)) continue;
        size_t at = (size_t)(names - d) + (size_t)(attr & 0xffffff) * 4;
        if (at >= len || strnlen((const char *)d + at, len - at) == len - at) continue;
        if (strcmp((const char *)d + at, want) || e < s || (size_t)data_off + e > len) continue;
        *flen = e - s;
        return d + data_off + s;
    }
    return NULL;
}

// An NX icon font: its single BC4 sheet (pointer into f) and its code -> glyph map. false: not that kind of font.
static bool parse_arms_font(const uint8_t *f, uint32_t len, ArmsFont *af, const uint8_t **sheet, uint32_t *sheet_len,
                            uint32_t *sw, uint32_t *sh) {
    if (len < 0x60 || memcmp(f, "FFNT", 4) || f[4] != 0xFF || f[5] != 0xFE) return false;
    const uint8_t *t = NULL;
    for (uint32_t p = 0x14; p + 0x20 <= len && !t; p += 4) if (!memcmp(f + p, "TGLP", 4)) t = f + p;
    if (!t) return false;
    uint32_t tsize = rd32(t + 4), nsheets = t[10], ssize = rd32(t + 12), fmt = rd16(t + 18), data_off = rd32(t + 28);
    af->cw = (uint16_t)(t[8] + 1), af->ch = (uint16_t)(t[9] + 1), af->cols = rd16(t + 20);
    *sw = rd16(t + 24), *sh = rd16(t + 26);
    if (nsheets != 1 || (fmt & 0x7fff) != 12 /* BC4 */ || !af->cols || !*sw || !*sh) return false;
    uint32_t bytes = (*sw / 4) * (*sh / 4) * 8;  // (the texture: the rest of the section is padding)
    if (bytes > ssize || data_off + bytes > len) return false;
    // The texture's bytes start at the sheet offset rounded up to its block-linear block (512 bytes per GOB times the
    // block height's 16 GOBs: 0x2000; ARMSIconFont: the field says 0x1000, the image starts at 0x2000 - checked
    // against the texture the game uploads, every block in place)
    uint32_t th = tegra_default_block_height_log2(*sh / 4), al = 512u << th, at = (data_off + al - 1) / al * al;
    if (at + bytes <= data_off + ssize && at + bytes <= len) data_off = at;
    *sheet = f + data_off, *sheet_len = bytes;
    af->n = 0;
    uint32_t p = (uint32_t)(t - f) + tsize;
    for (; p + 0x18 <= len; p += 4) {
        if (memcmp(f + p, "CMAP", 4)) continue;
        uint32_t cb = rd32(f + p + 8), ce = rd32(f + p + 12), m = rd16(f + p + 16);
        const uint8_t *body = f + p + 0x18;
        if (ce < cb || ce - cb > 0x10000) continue;
        if (m == 0 && body + 2 <= f + len) {  // direct
            uint16_t first = rd16(body);
            for (uint32_t c = cb; c <= ce && af->n < 256; c++) af->code[af->n] = (uint16_t)c, af->glyph[af->n++] = (uint16_t)(first + c - cb);
        } else if (m == 1 && body + 2 * (ce - cb + 1) <= f + len) {  // table
            for (uint32_t c = cb; c <= ce && af->n < 256; c++) {
                uint16_t g = rd16(body + 2 * (c - cb));
                if (g != 0xFFFF) af->code[af->n] = (uint16_t)c, af->glyph[af->n++] = g;
            }
        } else if (m == 2 && body + 4 <= f + len) {  // scan: pairs
            uint16_t n = rd16(body);
            for (uint32_t i = 0; i < n && body + 4 + 8 * (i + 1) <= f + len && af->n < 256; i++)
                af->code[af->n] = (uint16_t)rd32(body + 4 + 8 * i), af->glyph[af->n++] = rd16(body + 8 + 8 * i);
        }
    }
    return af->n > 0;
}

static void scan_arms_icons(void) {
    static const char *const regions[] = {"US", "EU", "JP", "KR", "CN", "TW"};
    for (int i = 0; i < (int)(sizeof g_arms_font_of_ref); i++) g_arms_font_of_ref[i] = -1;
    for (int ri = 0; ri < (int)(sizeof regions / sizeof *regions); ri++) {
        char rel[64];
        snprintf(rel, sizeof rel, "Font/BmpFont_%s.szs", regions[ri]);
        size_t zlen, len;
        uint8_t *z = game_file(rel, &zlen);
        if (!z) continue;
        uint8_t *d = yaz0_decode(z, zlen, &len);
        free(z);
        if (!d) continue;
        uint32_t flen, slen, sw, sh;
        const uint8_t *f = sarc_file(d, len, "ARMSIconFont.bffnt", &flen), *sheet;
        ArmsFont af;
        if (f && g_arms_nfonts < (int)(sizeof g_arms_font / sizeof *g_arms_font) &&
            parse_arms_font(f, flen, &af, &sheet, &slen, &sw, &sh)) {
            int before = g_nrefs;
            add_ref(R_ARMS_SHEET, K_BC4, 0, sw, sh, 1, sheet, slen, "ARMSIconFont", 4);
            if (g_nrefs > before) {  // (a new sheet: the regions share most)
                g_arms_font[g_arms_nfonts] = af;
                g_arms_font_of_ref[g_nrefs - 1] = (int8_t)g_arms_nfonts++;
            }
        }
        free(d);
    }
    if (g_arms_nfonts) LOG("[texpatch] ARMS icon font: %d sheet(s)\n", g_arms_nfonts);
}

// ------------------------------------------------------------------ ARMS Rearmed's logo & splash art (tools/branding.py)
// <extracted>/branding/manifest.txt: "<name> <w> <h> <bc4|bc7> <orig> <rgba>" per texture - the game's texture as it holds
// it in memory (.orig: how it's recognised on upload) and its replacement (.rgba, RGBA8 w x h), both made at setup from
// the player's own game files. While "Rearmed logo & splash art" is on (Video tab) the replacement is encoded into the
// texture's own format as it is uploaded; off, the game's own is uploaded (a change re-uploads: texpatch_generation).
typedef struct {
    char rgba[640];
    uint8_t *px;    // (the replacement, read at its first use)
    bool tried, splash;
} Brand;
static Brand g_brand[8];
static int g_nbrand;
static int8_t g_brand_of_ref[96];

static void branding_dir(char *out, size_t n) {
    const char *r = g_romfs_dir ? g_romfs_dir : "extracted/romfs";
    size_t l = strlen(r);
    if (l >= 6 && (!strcmp(r + l - 6, "/romfs") || !strcmp(r + l - 6, "\\romfs"))) snprintf(out, n, "%.*s/branding", (int)(l - 6), r);
    else snprintf(out, n, "extracted/branding");
}

static void scan_branding(void) {
    for (int i = 0; i < (int)sizeof g_brand_of_ref; i++) g_brand_of_ref[i] = -1;
    char dir[600], p[900], line[600];
    branding_dir(dir, sizeof dir);
    snprintf(p, sizeof p, "%s/manifest.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_nbrand < (int)(sizeof g_brand / sizeof *g_brand)) {
        char name[64], kind[8], orig[160], rgba[160];
        unsigned w, h;
        if (line[0] == '#' || sscanf(line, "%63s %u %u %7s %159s %159s", name, &w, &h, kind, orig, rgba) != 6) continue;
        int k = !strcmp(kind, "bc4") ? K_BC4 : !strcmp(kind, "bc7") ? K_BC7 : -1;
        size_t len;
        snprintf(p, sizeof p, "%s/%s", dir, orig);
        uint8_t *d = k >= 0 ? read_file(p, &len) : NULL;
        if (!d) continue;
        int before = g_nrefs;
        add_ref(R_BRAND, k, 0, w, h, 1, d, (uint32_t)len, name, 0);
        free(d);
        if (g_nrefs > before) {
            Brand *b = &g_brand[g_nbrand];
            snprintf(b->rgba, sizeof b->rgba, "%s/%s", dir, rgba);
            b->splash = !strcmp(name, "Boot_ControllerStrap");
            g_brand_of_ref[g_nrefs - 1] = (int8_t)g_nbrand++;
        }
    }
    fclose(f);
    if (g_nbrand) LOG("[texpatch] Rearmed logo & splash art: %d textures (%s)\n", g_nbrand, dir);
}

// "Rearmed logo & splash art" (LARECOMP_REARMED_ART=0 / 1: test override)
bool texpatch_brand_on(void) {
    static int env = -2;
    if (env == -2) { const char *e = getenv("LARECOMP_REARMED_ART"); env = e ? atoi(e) != 0 : -1; }
    return env >= 0 ? env != 0 : g_settings.rearmed_art;
}

// Known texture `id` is ARMS's button-icon font sheet (exec.c: its glyphs are text too)
bool texpatch_is_icon_sheet(int id) { return id >= 0 && id < g_nrefs && g_refs[id].role == R_ARMS_SHEET; }

// Known texture `id` is the Rearmed boot splash (runtime/nvn/vulkan/exec.c hides the game's own text over it)
bool texpatch_is_brand_splash(int id) {
    return id >= 0 && id < g_nrefs && g_refs[id].role == R_BRAND && g_brand_of_ref[id] >= 0 && g_brand[g_brand_of_ref[id]].splash;
}

static void load_refs(void) {
    scan_branding();
    scan_arms_icons();
    scan_sarc("region_common/ui/Game.arc");
    scan_sarc("region_common/ui/StartUp.arc");
    scan_sarc("regionUS/common/ui/GameRegionCommon.arc");
    scan_sarc("regionEU/common/ui/GameRegionCommon.arc");
    size_t len;
    uint8_t *f;
    static const char *const models[] = {"region_common/actor/ItemBottle.bfres", "region_common/actor/Playerlink.bfres",
                                         "region_common/actor/ItemFairy.bfres", "region_common/actor/ItemMagicPowder.bfres",
                                         "region_common/actor/ItemDungeonMap.bfres",
                                         "region_common/actor/ItemPowerPiece.bfres"};
    for (int m = 0; m < (int)(sizeof models / sizeof *models); m++)
        if ((f = read_all(models[m], &len))) {  // (their textures: one BNTX inside)
            g_scanning_link = m == 1;
            for (size_t i = 0; i + 8 < len; i += 4)
                if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, false); break; }
            g_scanning_link = false;
            free(f);
        }
    f = read_all("region_common/ui/pictogram.bffnt", &len);  // message icons: one RGBA8 sheet in a BNTX
    if (f) {
        for (size_t i = 0; i + 8 < len; i += 4)
            if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, true); break; }
        free(f);
    }
    LOG("[texpatch] %d known textures (button glyphs, pictogram sheet, Hero Mode icon, bottle icon / model, tunic, hearts)\n",
        g_nrefs);
}

int texpatch_bottle_kind(int id) {
    if (id < 0 || id >= g_nrefs) return 0;
    return g_refs[id].role == R_BOTTLE ? 1 : g_refs[id].role == R_BOTTLE_MODEL ? 2 : g_refs[id].role == R_FAIRY_MODEL ? 3
         : g_refs[id].role == R_POWDER_MODEL ? 4 : g_refs[id].role == R_MAP_MODEL ? 5 : g_refs[id].role == R_POWER_PIECE_MODEL ? 6 : 0;
}

int texpatch_heart_kind(int id) {
    if (id < 0 || id >= g_nrefs) return 0;
    return g_refs[id].role == R_HEART;
}

static int ref_of(int role) {
    for (int i = 0; i < g_nrefs; i++) if (g_refs[i].role == role) return i;
    return -1;
}

// A seed made with a tunic colour (older seeds: tools/rando/cosmetics.py) ships its own recoloured Playerlink.bfres:
// its tunic map is recognised too, so the Cosmetics tab's colour wins over it. Read again when the seed changes
// (seeds switch in place).
static uint8_t *g_seed_tunic;
static uint32_t g_seed_tunic_size;
static char g_seed_tunic_dir[300] = "\1";
static const uint8_t *seed_tunic(uint32_t *size) {
    const char *dir = rando_seed_dir();
    if (strcmp(dir ? dir : "", g_seed_tunic_dir)) {
        strlcpy(g_seed_tunic_dir, dir ? dir : "", sizeof g_seed_tunic_dir);
        free(g_seed_tunic);
        g_seed_tunic = NULL;
        char p[600];
        size_t len;
        uint8_t *f = NULL;
        if (dir) snprintf(p, sizeof p, "%s/romfs/region_common/actor/Playerlink.bfres", dir), f = read_file(p, &len);
        int n0 = g_nrefs;
        for (size_t i = 0; f && i + 8 < len; i += 4)  // (a copy only differing from the game's adds a ref: taken out)
            if (!memcmp(f + i, "BNTX\0\0\0\0", 8)) { scan_bntx(f + i, len - i, false); break; }
        free(f);
        for (int i = n0; i < g_nrefs; i++) {
            if (g_refs[i].role == R_TUNIC && !g_seed_tunic) { g_seed_tunic = g_refs[i].data; g_seed_tunic_size = g_refs[i].size; }
            else free(g_refs[i].data);
        }
        g_nrefs = n0;
        if (g_seed_tunic) LOG("[texpatch] seed tunic map (%s)\n", dir);
    }
    *size = g_seed_tunic_size;
    return g_seed_tunic;
}

static TexLayout g_tunic_layout;  // (the game's tunic texture: its levels' places in the map's data)
static bool g_tunic_layout_ok;
bool texpatch_is_item_cursor(int id) { return id >= 0 && id < g_nrefs && g_refs[id].role == R_ITEM_CURSOR; }

int texpatch_identify(const NvnTexture *t) {
    pthread_once(&g_once, load_refs);
    if (!t->addr || t->depth > 1) return -1;
    uint64_t total = tegra_total_size(&t->layout);
    for (int i = 0; i < g_nrefs; i++) {
        const Ref *r = &g_refs[i];
        // (only the bottle model's and the tunic's maps have mip levels: compared in full, all levels)
        bool tunic = r->role == R_TUNIC || r->role == R_TUNIC_RED || r->role == R_TUNIC_BLUE;
        if (r->w != t->width || r->h != t->height || (r->role != R_POWDER_MODEL && r->role != R_MAP_MODEL &&
            r->role != R_POWER_PIECE_MODEL &&
            (t->levels > 1) != (r->role == R_BOTTLE_MODEL || r->role == R_FAIRY_MODEL || tunic || r->role == R_SKIN))) continue;
        uint64_t n = r->size < total ? r->size : total;
        if (!memcmp((const void *)(uintptr_t)t->addr, r->data, n)) {
            static int nlog;
            if (nlog++ < 64) LOG("[texpatch] %s (%ux%u fmt %#x) uploaded\n", r->name, r->w, r->h, t->format);
            if (tunic) { g_tunic_layout = t->layout; g_tunic_layout_ok = true; }  // (all three: the same layout)
            return i;
        }
    }
    int tu = ref_of(R_TUNIC);
    if (tu >= 0 && t->levels > 1 && t->width == g_refs[tu].w && t->height == g_refs[tu].h) {
        uint32_t n;
        const uint8_t *s = seed_tunic(&n);
        if (s && n == g_refs[tu].size && n <= total && !memcmp((const void *)(uintptr_t)t->addr, s, n)) {
            LOG("[texpatch] seed's tunic map uploaded\n");
            g_tunic_layout = t->layout;
            g_tunic_layout_ok = true;
            return tu;
        }
    }
    int rc = recolour_identify(t, total);  // (Cosmetics recolours: identified only, runtime/nvn/recolour.c)
    return rc >= 0 ? TEXPATCH_RECOLOUR_BASE + rc : -1;
}

static int prompt_style(void) {
    // LARECOMP_PROMPTS=nintendo|xbox|playstation: test override, until frame LARECOMP_PROMPTS_UNTIL (then the setting)
    static int env = -2;
    static long until;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PROMPTS");
        env = !e ? -1 : !strcmp(e, "xbox") ? PROMPTS_XBOX : !strcmp(e, "playstation") ? PROMPTS_PLAYSTATION :
              !strcmp(e, "nintendo") ? PROMPTS_NINTENDO : !strcmp(e, "keyboard") ? PROMPTS_KEYBOARD : PROMPTS_AUTO;
        until = getenv("LARECOMP_PROMPTS_UNTIL") ? atol(getenv("LARECOMP_PROMPTS_UNTIL")) : -1;
    }
    extern uint64_t renderer_present_count(void);
    int p = env >= 0 && (until < 0 || (long)renderer_present_count() < until) ? env : g_settings.prompts;
    if (p == PROMPTS_XBOX) return STYLE_XBOX;
    if (p == PROMPTS_PLAYSTATION) return STYLE_PS;
    if (p == PROMPTS_NINTENDO) return STYLE_NINTENDO;
    if (p == PROMPTS_KEYBOARD) return STYLE_KB;
    // Auto: the keyboard and mouse while they were used last (runtime/hle/input.c), else the controller SDL reports.
    // Only ARMS's icon font has keyboard icons (no other known texture is touched in that style).
    extern int input_last_kbm(void);
    extern int input_pad_family(void);  // runtime/hle/input.c: the controller SDL reports
    if (g_arms_nfonts && input_last_kbm()) return STYLE_KB;
    int f = input_pad_family();
    return f == 1 ? STYLE_XBOX : f == 2 ? STYLE_PS : STYLE_NINTENDO;
}

static bool hide_hero(void) {
    static int env = -2;  // LARECOMP_HIDE_HERO=0/1: test override
    if (env == -2) { const char *e = getenv("LARECOMP_HIDE_HERO"); env = e ? atoi(e) != 0 : -1; }
    return env >= 0 ? env : g_settings.hide_hero_icon;
}

// The label a glyph of `role` gets: whichever button is bound to that Switch input now
static int prompt_label(int role) {
    static const int8_t slot[] = {[R_A] = BIND_A, [R_B] = BIND_B, [R_X] = BIND_X, [R_Y] = BIND_Y, [R_L] = BIND_L,
                                  [R_R] = BIND_R, [R_ZL] = BIND_ZL, [R_ZR] = BIND_ZR, [R_PLUS] = BIND_PLUS,
                                  [R_MINUS] = BIND_MINUS};
    if (role < R_A || role > R_MINUS) return role;
    switch (bind_prompt_host(slot[role])) {
    case 1 /* east */: return R_A;
    case 0 /* south */: return R_B;
    case 3 /* north */: return R_X;
    case 2 /* west */: return R_Y;
    case 9 /* left shoulder */: return R_L;
    case 10 /* right shoulder */: return R_R;
    case PADB_LTRIGGER: return R_ZL;
    case PADB_RTRIGGER: return R_ZR;
    case 6 /* start */: return R_PLUS;
    case 4 /* back */: return R_MINUS;
    case 7: return L_LS;
    case 8: return L_RS;
    case 11: return L_DUP;
    case 12: return L_DDOWN;
    case 13: return L_DLEFT;
    case 14: return L_DRIGHT;
    default: return L_OTHER;
    }
}

static uint32_t labels_hash(void) {
    uint32_t h = 0;
    for (int r = R_A; r <= R_MINUS; r++) h = h * 41 + (uint32_t)(prompt_label(r) - r + 64);
    return h;
}

// (a Nintendo-style glyph is only redrawn when its button was remapped)
static bool restyled(int role, int style) { return style != STYLE_NINTENDO || prompt_label(role) != role; }

static void cosmetics_test(void);
static uint32_t skin_rgb(void);
static uint32_t tunic_rgb(void);

static uint32_t kb_labels_hash(void) {  // (the keyboard bindings: ARMS's key cap icons)
    uint32_t h = 0;
    for (int i = 0; i < BIND_KEY_COUNT; i++) h = h * 31 + (uint32_t)(uint16_t)g_bind_key[i];
    return h;
}

uint32_t texpatch_generation(void) {  // (a rebind re-uploads the glyphs, like a style change; so does a tunic colour)
    cosmetics_test();
    int style = prompt_style();
    return (1 + (uint32_t)style + (hide_hero() ? 8 : 0) + (labels_hash() << 4)) ^ tunic_rgb() * 0x9E3779B1u ^
           skin_rgb() * 0x85EBCA6Bu ^ (style == STYLE_KB ? kb_labels_hash() * 0xC2B2AE35u : 0) ^
           (texpatch_brand_on() ? 0x5BD1E995u : 0);
}

// ------------------------------------------------------------------ block codecs (BC4/BC5, BC1/BC3, ASTC 4x4)
static void bc4_decode(const uint8_t *b, uint8_t out[16]) {
    int e0 = b[0], e1 = b[1], pal[8] = {e0, e1};
    if (e0 > e1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * e0 + i * e1 + 3) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * e0 + i * e1 + 2) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
}

static void bc4_encode(const uint8_t v[16], uint8_t *b) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    uint64_t bits = 0;
    if (hi == lo) { b[0] = b[1] = (uint8_t)hi; }
    else {
        int pal[8] = {hi, lo};
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
        b[0] = (uint8_t)hi; b[1] = (uint8_t)lo;
        for (int i = 0; i < 16; i++) {
            int best = 0, be = 1 << 30;
            for (int k = 0; k < 8; k++) { int e = abs(pal[k] - v[i]); if (e < be) { be = e; best = k; } }
            bits |= (uint64_t)best << (3 * i);
        }
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
}

static void rgb565(uint16_t c, int o[3]) {
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = b << 3 | b >> 2;
}

static void bc1_decode4(const uint8_t *b, uint8_t out[16][4]) {  // (BC2/BC3 colour: always the 4-colour mode)
    int c[4][3];
    rgb565(rd16(b), c[0]); rgb565(rd16(b + 2), c[1]);
    for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
    uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) out[i][k] = (uint8_t)c[(idx >> (2 * i)) & 3][k];
}

static uint16_t to565(const uint8_t *p) { return (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3); }

static void bc1_encode4(const uint8_t px[16][4], uint8_t *b) {
    int bi = 0, bj = 0, bd = -1;  // the two most distant colours of the block as endpoints
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++) {
            int d = 0;
            for (int k = 0; k < 3; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    uint16_t c0 = to565(px[bi]), c1 = to565(px[bj]);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    int c[4][3];
    rgb565(c0, c[0]); rgb565(c1, c[1]);
    for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int s = 0; s < 4; s++) {
            int e = 0;
            for (int k = 0; k < 3; k++) e += (c[s][k] - px[i][k]) * (c[s][k] - px[i][k]);
            if (e < be) { be = e; best = s; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    b[0] = (uint8_t)c0; b[1] = (uint8_t)(c0 >> 8); b[2] = (uint8_t)c1; b[3] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) b[4 + i] = (uint8_t)(idx >> (8 * i));
}

// ASTC 4x4: one partition, colour endpoint mode 12 (LDR RGBA direct) with 8-bit endpoints, a 4x4 grid of 2-bit
// weights (block mode 0x42: 11 + 2 + 4 + 64 endpoint + 32 weight bits). Only blocks we change are re-encoded.
static void put_bits(uint8_t *b, int at, int n, uint32_t v) {
    for (int i = 0; i < n; i++) if (v >> i & 1) b[(at + i) >> 3] |= (uint8_t)(1 << ((at + i) & 7));
}

static void astc_encode4(const uint8_t px[16][4], bool srgb, uint8_t *b) {
    int bi = 0, bj = 0, bd = -1;
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++) {
            int d = 0;
            for (int k = 0; k < 4; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > bd) { bd = d; bi = i; bj = j; }
        }
    const uint8_t *e0 = px[bi], *e1 = px[bj];
    if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2]) { const uint8_t *t = e0; e0 = e1; e1 = t; }  // (no blue contraction)
    memset(b, 0, 16);
    put_bits(b, 0, 11, 0x42);
    put_bits(b, 13, 4, 12);
    for (int k = 0; k < 4; k++) { put_bits(b, 17 + 16 * k, 8, e0[k]); put_bits(b, 25 + 16 * k, 8, e1[k]); }
    static const int wq[4] = {0, 21, 43, 64};
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int q = 0; q < 4; q++) {
            int e = 0;
            for (int k = 0; k < 4; k++) {
                int c0 = srgb && k < 3 ? (e0[k] << 8 | 0x80) : e0[k] * 257, c1 = srgb && k < 3 ? (e1[k] << 8 | 0x80) : e1[k] * 257;
                int v = ((c0 * (64 - wq[q]) + c1 * wq[q] + 32) >> 6) >> 8;
                e += (v - px[i][k]) * (v - px[i][k]);
            }
            if (e < be) { be = e; best = q; }
        }
        if (best & 1) b[(127 - 2 * i) >> 3] |= (uint8_t)(1 << ((127 - 2 * i) & 7));  // weights: bit-reversed from the top
        if (best & 2) b[(126 - 2 * i) >> 3] |= (uint8_t)(1 << ((126 - 2 * i) & 7));
    }
}

// Level data <-> RGBA8 image (BC5: red, green -> R, G; alpha = green, the glyph's shape)
static void decode_image(const Ref *r, const uint8_t *src, uint32_t row, uint8_t *img) {
    uint32_t W = r->w, H = r->h, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (r->kind == K_RGBA8) { for (uint32_t y = 0; y < H; y++) memcpy(img + y * W * 4, src + y * row, W * 4); return; }
    if (r->kind == K_ASTC4) {
        uint8_t *blocks = malloc((size_t)bw * bh * 16);
        for (uint32_t y = 0; y < bh; y++) memcpy(blocks + y * bw * 16, src + y * row, bw * 16);
        astc_decode_ldr(blocks, W, H, 4, 4, r->srgb, img);
        free(blocks);
        return;
    }
    if (r->kind == K_BC4) {  // (a mask: every channel the mask, so the shape is in alpha as for the others)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                uint8_t a[16];
                bc4_decode(src + by * row + bx * 8, a);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < W && y < H) memset(img + (y * W + x) * 4, a[i], 4);
                }
            }
        return;
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + by * row + bx * 16;
            uint8_t px[16][4] = {{0}}, a[16], c2[16];
            if (r->kind == K_BC5) {
                bc4_decode(b, a); bc4_decode(b + 8, c2);
                for (int i = 0; i < 16; i++) { px[i][0] = a[i]; px[i][1] = c2[i]; px[i][2] = 0; px[i][3] = c2[i]; }
            } else {
                bc4_decode(b, a); bc1_decode4(b + 8, px);
                for (int i = 0; i < 16; i++) px[i][3] = a[i];
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < W && y < H) memcpy(img + (y * W + x) * 4, px[i], 4);
            }
        }
}

static void encode_image(const Ref *r, const uint8_t *img, const uint8_t *dirty, uint8_t *dst, uint32_t row) {
    uint32_t W = r->w, H = r->h, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (r->kind == K_RGBA8) { for (uint32_t y = 0; y < H; y++) memcpy(dst + y * row, img + y * W * 4, W * 4); return; }
    if (r->kind == K_BC4) {
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                if (!dirty[by * bw + bx]) continue;
                uint8_t a[16];
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= W) x = W - 1;
                    if (y >= H) y = H - 1;
                    a[i] = img[(y * W + x) * 4 + 3];
                }
                bc4_encode(a, dst + by * row + bx * 8);
            }
        return;
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            if (!dirty[by * bw + bx]) continue;
            uint8_t px[16][4], *b = dst + by * row + bx * 16;
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= W) x = W - 1;
                if (y >= H) y = H - 1;
                memcpy(px[i], img + (y * W + x) * 4, 4);
            }
            if (r->kind == K_BC5) {
                uint8_t c0[16], c1[16];
                for (int i = 0; i < 16; i++) { c0[i] = px[i][0]; c1[i] = px[i][1]; }
                bc4_encode(c0, b); bc4_encode(c1, b + 8);
            } else if (r->kind == K_BC3) {
                uint8_t a[16];
                for (int i = 0; i < 16; i++) a[i] = px[i][3];
                bc4_encode(a, b); bc1_encode4(px, b + 8);
            } else {
                astc_encode4(px, r->srgb, b);
            }
        }
}

// ------------------------------------------------------------------ labels
static stbtt_fontinfo g_font;
static bool g_font_ok;
static uint8_t *g_font_data;

static void try_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_font_data = malloc((size_t)n);
    if (fread(g_font_data, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0)))
        g_font_ok = true;
    else { free(g_font_data); g_font_data = NULL; }
    fclose(f);
}

static void load_font(void) {  // a bold system font for the letters (none of the game's fonts are needed)
    static const char *const paths[] = {
        "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/Library/Fonts/Arial Bold.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
    };
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && !g_font_ok; i++) try_font(paths[i]);
    char own[1100];  // none of the system's: the bundled DejaVu Sans Bold (third_party/fonts/DejaVu, in every release)
    if (!g_font_ok && rt_resource("DejaVuSans-Bold.ttf", "third_party/fonts/DejaVu/DejaVuSans-Bold.ttf", own, sizeof own)) try_font(own);
    if (!g_font_ok) LOG("[texpatch] no system font for the button letters: Xbox letters stay Nintendo's\n");
}

#define CV 256  // label canvas (drawn large, then fitted into the glyph)

static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = sat((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f));
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

typedef struct { float ax, ay, bx, by; } Seg;

static void draw_segs(float *cv, const Seg *s, int n, float r) {  // round-capped strokes
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float d = 1e9f;
            for (int i = 0; i < n; i++) { float e = seg_dist(x + 0.5f, y + 0.5f, s[i].ax, s[i].ay, s[i].bx, s[i].by); if (e < d) d = e; }
            float a = sat(r - d + 0.5f);
            if (a > cv[y * CV + x]) cv[y * CV + x] = a;
        }
}

static void draw_ring(float *cv, float cx, float cy, float rad, float th) {
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
            float a = sat(th / 2 - fabsf(d - rad) + 0.5f);
            if (a > cv[y * CV + x]) cv[y * CV + x] = a;
        }
}

static void draw_rect(float *cv, float x0, float y0, float x1, float y1, float th) {
    Seg s[4] = {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}};
    draw_segs(cv, s, 4, th / 2);
}

static bool draw_text(float *cv, const char *text) {
    if (!g_font_ok) return false;
    float sc = stbtt_ScaleForPixelHeight(&g_font, 200);
    float x = 8;
    for (const char *p = text; *p; p++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, *p, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, *p, sc, sc, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0, ox = (int)x + x0, oy = 200 + y0;
        if (w > 0 && h > 0) {
            unsigned char *bm = malloc((size_t)w * h);
            stbtt_MakeCodepointBitmap(&g_font, bm, w, h, w, sc, sc, *p);
            for (int yy = 0; yy < h; yy++)
                for (int xx = 0; xx < w; xx++) {
                    int X = ox + xx, Y = oy + yy;
                    if (X >= 0 && X < CV && Y >= 0 && Y < CV) { float a = bm[yy * w + xx] / 255.0f; if (a > cv[Y * CV + X]) cv[Y * CV + X] = a; }
                }
            free(bm);
        }
        x += adv * sc;
        if (p[1]) x += stbtt_GetCodepointKernAdvance(&g_font, p[0], p[1]) * sc;
    }
    return true;
}

// The new label `role` (R_* / L_*, canvas coverage). symbol = true: a square pictogram (fitted by its size, not cap
// height).
static bool draw_label(int role, int style, float *cv, bool *symbol) {
    memset(cv, 0, sizeof(float) * CV * CV);
    *symbol = role == R_PLUS || role == R_MINUS || (style == STYLE_PS && role <= R_Y) || role >= L_DUP;
    if (role >= L_DUP && role <= L_DRIGHT) {  // d-pad: an arrow (drawn pointing up, then turned)
        static const float up[3][4] = {{128, 228, 128, 40}, {128, 34, 46, 116}, {128, 34, 210, 116}};
        Seg s[3];
        for (int i = 0; i < 3; i++) {
            float p[4];
            for (int k = 0; k < 4; k += 2) {
                float x = up[i][k] - 128, y = up[i][k + 1] - 128;
                float rx = role == L_DUP ? x : role == L_DDOWN ? -x : role == L_DLEFT ? y : -y;
                float ry = role == L_DUP ? y : role == L_DDOWN ? -y : role == L_DLEFT ? -x : x;
                p[k] = rx + 128, p[k + 1] = ry + 128;
            }
            s[i] = (Seg){p[0], p[1], p[2], p[3]};
        }
        draw_segs(cv, s, 3, 16);
        return true;
    }
    if (role == L_OTHER) { draw_ring(cv, 128, 128, 40, 80); return true; }  // a paddle / extra button: a dot
    if (role == L_LS || role == L_RS)
        return draw_text(cv, style == STYLE_PS ? (role == L_LS ? "L3" : "R3") : (role == L_LS ? "LS" : "RS"));
    if (style == STYLE_NINTENDO && role == R_PLUS) {
        Seg s[2] = {{128, 40, 128, 216}, {40, 128, 216, 128}};
        draw_segs(cv, s, 2, 18);
        return true;
    }
    if (style == STYLE_NINTENDO && role == R_MINUS) {
        Seg s[1] = {{40, 128, 216, 128}};
        draw_segs(cv, s, 1, 18);
        return true;
    }
    if (role == R_PLUS) {  // Menu / Options: three bars
        Seg s[3] = {{48, 64, 208, 64}, {48, 128, 208, 128}, {48, 192, 208, 192}};
        draw_segs(cv, s, 3, 15);
        return true;
    }
    if (role == R_MINUS) {
        if (style == STYLE_XBOX) {  // View: two overlapping windows
            draw_rect(cv, 92, 40, 220, 140, 22);
            for (int y = 104; y < 228; y++) for (int x = 24; x < 164; x++) cv[y * CV + x] = 0;  // (front window hides the back)
            draw_rect(cv, 36, 116, 152, 216, 22);
        } else {  // Create / Share: three rays
            Seg s[3] = {{128, 40, 128, 140}, {48, 92, 104, 164}, {208, 92, 152, 164}};
            draw_segs(cv, s, 3, 15);
            Seg d[1] = {{128, 206, 128, 206}};
            draw_segs(cv, d, 1, 20);
        }
        return true;
    }
    if (style == STYLE_PS && role <= R_Y) {
        if (role == R_A) draw_ring(cv, 128, 128, 96, 26);  // east: circle
        else if (role == R_B) { Seg s[2] = {{44, 44, 212, 212}, {212, 44, 44, 212}}; draw_segs(cv, s, 2, 14); }  // south: cross
        else if (role == R_X) { Seg s[3] = {{128, 30, 226, 200}, {226, 200, 30, 200}, {30, 200, 128, 30}}; draw_segs(cv, s, 3, 13); }  // north: triangle
        else draw_rect(cv, 44, 44, 212, 212, 26);  // west: square
        return true;
    }
    static const char *const xbox[] = {"", "B", "A", "Y", "X", "LB", "RB", "LT", "RT"};
    static const char *const ps[] = {"", "", "", "", "", "L1", "R1", "L2", "R2"};
    static const char *const nin[] = {"", "A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    if (role > R_ZR) return false;
    return draw_text(cv, style == STYLE_XBOX ? xbox[role] : style == STYLE_PS ? ps[role] : nin[role]);
}

// Relabel one glyph: img (W x H RGBA8, alpha channel `ach` = the shape), region x0,y0 w,h. flip: the region is
// stored upside down (font sheets).
static void relabel(uint8_t *img, int W, int x0, int y0, int w, int h, int ach, bool flip, int role, int style,
                    uint8_t *dirty, int bw) {
    int n = w * h;
    uint8_t *px = malloc((size_t)n * 4), *S = calloc((size_t)n, 1), *I = calloc((size_t)n, 1), *L = calloc((size_t)n, 1);
    int *stack = malloc(sizeof(int) * (size_t)n * 4);
    float *cv = malloc(sizeof(float) * CV * CV);
    for (int y = 0; y < h; y++) {
        int sy = flip ? h - 1 - y : y;
        memcpy(px + y * w * 4, img + ((y0 + sy) * W + x0) * 4, (size_t)w * 4);
    }
    // shape: alpha > 0.5 with its holes filled (the region outside it is what the border reaches)
    uint8_t *out = calloc((size_t)n, 1);
    int sp = 0;
    for (int i = 0; i < n; i++) {
        int x = i % w, y = i / w;
        if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && px[i * 4 + ach] <= 127) { out[i] = 1; stack[sp++] = i; }
    }
    while (sp) {
        int i = stack[--sp], x = i % w, y = i / w;
        const int nb[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
        for (int k = 0; k < 4; k++)
            if (nb[k] >= 0 && !out[nb[k]] && px[nb[k] * 4 + ach] <= 127) { out[nb[k]] = 1; stack[sp++] = nb[k]; }
    }
    int sx0 = w, sy0 = h, sx1 = -1, sy1 = -1, area = 0;
    for (int i = 0; i < n; i++)
        if (!out[i]) {
            S[i] = 1, area++;
            int x = i % w, y = i / w;
            if (x < sx0) sx0 = x;
            if (x > sx1) sx1 = x;
            if (y < sy0) sy0 = y;
            if (y > sy1) sy1 = y;
        }
    if (area < 30) goto done;
    for (int i = 0; i < n; i++) {  // interior: 2 pixels in from the shape's edge
        int x = i % w, y = i / w, ok = S[i];
        for (int dy = -2; dy <= 2 && ok; dy++)
            for (int dx = -2; dx <= 2 && ok; dx++) {
                int xx = x + dx, yy = y + dy;
                ok = xx >= 0 && yy >= 0 && xx < w && yy < h && S[yy * w + xx];
            }
        I[i] = (uint8_t)ok;
    }
    // body colour = the most common colour inside, label colour = the most common one that clearly differs
    {
        static uint16_t hist[1 << 16];
        int best = -1, bc = 0;
        memset(hist, 0, sizeof hist);
        for (int i = 0; i < n; i++)
            if (I[i]) {
                const uint8_t *p = px + i * 4;
                int q = (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4;
                if (++hist[q] > bc) { bc = hist[q]; best = q; }
            }
        if (best < 0) goto done;
        float body[4] = {0}, lab[4] = {0};
        int nb = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            if (I[i] && ((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4) == best) {
                for (int k = 0; k < 4; k++) body[k] += p[k];
                nb++;
            }
        }
        for (int k = 0; k < 4; k++) body[k] /= nb;
        memset(hist, 0, sizeof hist);
        best = -1, bc = 0;
        int lx0 = w, ly0 = h, lx1 = -1, ly1 = -1;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            float d = 0;
            for (int k = 0; k < 4; k++) d += fabsf(p[k] - body[k]);
            if (!I[i] || d < 64) continue;
            L[i] = 1;
            int x = i % w, y = i / w;
            if (x < lx0) lx0 = x;
            if (x > lx1) lx1 = x;
            if (y < ly0) ly0 = y;
            if (y > ly1) ly1 = y;
            int q = (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4;
            if (++hist[q] > bc) { bc = hist[q]; best = q; }
        }
        if (best < 0) goto done;
        int nl = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = px + i * 4;
            if (L[i] && ((p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4) == best) {
                for (int k = 0; k < 4; k++) lab[k] += p[k];
                nl++;
            }
        }
        for (int k = 0; k < 4; k++) lab[k] /= nl;
        bool symbol;
        if (!draw_label(prompt_label(role), style, cv, &symbol)) goto done;
        // paint the old label (and its anti-aliased fringe) over with the body colour
        for (int i = 0; i < n; i++) {
            int x = i % w, y = i / w, near = 0, in = S[i];
            for (int dy = -2; dy <= 2 && !near; dy++)
                for (int dx = -2; dx <= 2 && !near; dx++) {
                    int xx = x + dx, yy = y + dy;
                    near = xx >= 0 && yy >= 0 && xx < w && yy < h && L[yy * w + xx];
                }
            for (int dy = -1; dy <= 1 && in; dy++)
                for (int dx = -1; dx <= 1 && in; dx++) {
                    int xx = x + dx, yy = y + dy;
                    in = xx >= 0 && yy >= 0 && xx < w && yy < h && S[yy * w + xx];
                }
            if (near && in) for (int k = 0; k < 4; k++) px[i * 4 + k] = (uint8_t)lroundf(body[k]);
        }
        // fit the new label: text at the old label's height (narrower than the shape), symbols by the old label's size
        int cx0 = CV, cy0 = CV, cx1 = -1, cy1 = -1;
        for (int i = 0; i < CV * CV; i++)
            if (cv[i] > 0.02f) {
                int x = i % CV, y = i / CV;
                if (x < cx0) cx0 = x;
                if (x > cx1) cx1 = x;
                if (y < cy0) cy0 = y;
                if (y > cy1) cy1 = y;
            }
        if (cx1 < 0) goto done;
        float lw = (float)(lx1 - lx0 + 1), lh = (float)(ly1 - ly0 + 1), iw = (float)(sx1 - sx0 + 1), ih = (float)(sy1 - sy0 + 1);
        float cw = (float)(cx1 - cx0 + 1), ch = (float)(cy1 - cy0 + 1);
        float tcx = (lx0 + lx1 + 1) / 2.0f, tcy = (ly0 + ly1 + 1) / 2.0f, th;
        if (symbol) {
            float s = fmaxf(lw, lh);
            if (role == R_PLUS || role == R_MINUS) s = lw;  // ("-" has almost no height)
            s = fminf(s, fminf(iw, ih) * 0.62f);
            th = s * ch / fmaxf(cw, ch);
            tcx = (sx0 + sx1 + 1) / 2.0f, tcy = (sy0 + sy1 + 1) / 2.0f;
        } else {
            th = role == R_PLUS || role == R_MINUS ? fminf(lw, ih * 0.6f) : lh;  // (remapped Plus / Minus: by "+"'s width)
            if (th * cw / ch > iw * 0.78f) th = iw * 0.78f * ch / cw;
        }
        float tw = th * cw / ch, k = ch / th;  // canvas pixels per glyph pixel
        float bx0 = tcx - tw / 2, by0 = tcy - th / 2;
        for (int y = (int)floorf(by0); y <= (int)ceilf(by0 + th); y++)
            for (int x = (int)floorf(bx0); x <= (int)ceilf(bx0 + tw); x++) {
                if (x < 0 || y < 0 || x >= w || y >= h || !S[y * w + x]) continue;
                float a = 0;  // 4x4 samples of the canvas over this pixel
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float u = cx0 + (x + (sx + 0.5f) / 4 - bx0) * k, v = cy0 + (y + (sy + 0.5f) / 4 - by0) * k;
                        int iu = (int)u, iv = (int)v;
                        if (iu >= cx0 && iv >= cy0 && iu <= cx1 && iv <= cy1) a += cv[iv * CV + iu];
                    }
                a /= 16;
                if (a <= 0) continue;
                uint8_t *p = px + (y * w + x) * 4;
                for (int c = 0; c < 4; c++) p[c] = (uint8_t)lroundf(p[c] + (lab[c] - p[c]) * a);
            }
        // write back, marking changed blocks
        for (int y = 0; y < h; y++) {
            int sy = flip ? h - 1 - y : y;
            uint8_t *d = img + ((y0 + sy) * W + x0) * 4;
            for (int x = 0; x < w; x++)
                if (memcmp(d + x * 4, px + (y * w + x) * 4, 4)) {
                    memcpy(d + x * 4, px + (y * w + x) * 4, 4);
                    dirty[((y0 + sy) / 4) * bw + (x0 + x) / 4] = 1;
                }
        }
    }
done:
    free(px); free(S); free(I); free(L); free(stack); free(cv); free(out);
}

// ------------------------------------------------------------------ ARMS: key caps, the mouse, stick presses
// Glyphs redrawn whole (not relabelled): a key cap (rounded square) or a mouse in the glyph's place, or a stick's ring
// with "L3" / "R3". The icon font has two looks: filled (the shape solid, the label cut out of it: the game's tint
// fills the shape, the background shows through the label) and outline (the shape's edge and the label drawn).
enum { ICON_KEY, ICON_MOUSE, ICON_RING, ICON_KEYS4 };  // (KEYS4: a stick's four keys as an arrow-key cluster)
enum { SYM_NONE, SYM_SHIFT, SYM_SPACE, SYM_ENTER, SYM_BACKSPACE, SYM_UP, SYM_DOWN, SYM_LEFT, SYM_RIGHT };
typedef struct {
    int icon;          // ICON_*
    int mouse;         // ICON_MOUSE: 1 left, 2 middle, 3 right
    int sym;           // SYM_*: drawn instead of text
    char text[8];
    int plabel, pstyle;  // else a controller label (draw_label's R_* / L_* in that style): Xbox / PlayStation rings
} CellIcon;

// Text of any length on the canvas: drawn at the height that keeps it inside the canvas's width
static bool draw_text_fit(float *cv, const char *text) {
    if (!g_font_ok || !*text) return false;
    float sc = stbtt_ScaleForPixelHeight(&g_font, 200), wsum = 0;
    for (const char *p = text; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, *p, &adv, &lsb);
        wsum += adv * sc + (p[1] ? stbtt_GetCodepointKernAdvance(&g_font, p[0], p[1]) * sc : 0);
    }
    float px = wsum > CV - 24 ? 200 * (CV - 24) / wsum : 200;
    sc = stbtt_ScaleForPixelHeight(&g_font, px);
    float x = 8;
    for (const char *p = text; *p; p++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, *p, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, *p, sc, sc, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0, ox = (int)x + x0, oy = (int)(128 + px / 2) + y0;
        if (w > 0 && h > 0) {
            unsigned char *bm = malloc((size_t)w * h);
            stbtt_MakeCodepointBitmap(&g_font, bm, w, h, w, sc, sc, *p);
            for (int yy = 0; yy < h; yy++)
                for (int xx = 0; xx < w; xx++) {
                    int X = ox + xx, Y = oy + yy;
                    if (X >= 0 && X < CV && Y >= 0 && Y < CV) { float a = bm[yy * w + xx] / 255.0f; if (a > cv[Y * CV + X]) cv[Y * CV + X] = a; }
                }
            free(bm);
        }
        x += adv * sc;
        if (p[1]) x += stbtt_GetCodepointKernAdvance(&g_font, p[0], p[1]) * sc;
    }
    return true;
}

static void fill_rrect(float *cv, float x0, float y0, float x1, float y1, float rad, bool cut) {  // (cut: clears)
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = fmaxf(fmaxf(x0 + rad - px, px - (x1 - rad)), 0), qy = fmaxf(fmaxf(y0 + rad - py, py - (y1 - rad)), 0);
            float a = sat(rad - hypotf(qx, qy) + 0.5f);
            if (px < x0 - 1 || px > x1 + 1 || py < y0 - 1 || py > y1 + 1) a = 0;
            float *c = &cv[y * CV + x];
            if (cut) *c = fminf(*c, 1 - a);
            else if (a > *c) *c = a;
        }
}

static void stroke_rrect(float *cv, float x0, float y0, float x1, float y1, float rad, float th) {
    static float tmp[CV * CV];
    memset(tmp, 0, sizeof tmp);
    fill_rrect(tmp, x0, y0, x1, y1, rad, false);
    fill_rrect(tmp, x0 + th, y0 + th, x1 - th, y1 - th, fmaxf(rad - th, 1), true);
    for (int i = 0; i < CV * CV; i++) if (tmp[i] > cv[i]) cv[i] = tmp[i];
}

// A key's label: a symbol (drawn with strokes) or text, into `cv` (cleared first). false: nothing drawn.
static bool draw_key_label(const CellIcon *ic, float *cv) {
    memset(cv, 0, sizeof(float) * CV * CV);
    if (ic->plabel) {
        bool symbol;
        return draw_label(ic->plabel, ic->pstyle, cv, &symbol);
    }
    switch (ic->sym) {
    case SYM_SHIFT: {  // a hollow arrow up
        Seg s[7] = {{128, 30, 226, 132}, {226, 132, 172, 132}, {172, 132, 172, 226}, {172, 226, 84, 226},
                    {84, 226, 84, 132}, {84, 132, 30, 132}, {30, 132, 128, 30}};
        draw_segs(cv, s, 7, 13);
        return true;
    }
    case SYM_SPACE: { Seg s[3] = {{30, 110, 30, 190}, {30, 190, 226, 190}, {226, 190, 226, 110}}; draw_segs(cv, s, 3, 16); return true; }
    case SYM_ENTER: {
        Seg s[4] = {{206, 40, 206, 170}, {206, 170, 60, 170}, {60, 170, 112, 118}, {60, 170, 112, 222}};
        draw_segs(cv, s, 4, 15);
        return true;
    }
    case SYM_BACKSPACE: {
        Seg s[3] = {{216, 128, 46, 128}, {46, 128, 104, 70}, {46, 128, 104, 186}};
        draw_segs(cv, s, 3, 16);
        return true;
    }
    case SYM_UP: case SYM_DOWN: case SYM_LEFT: case SYM_RIGHT: {
        bool symbol;
        static const int roles[] = {L_DUP, L_DDOWN, L_DLEFT, L_DRIGHT};
        return draw_label(roles[ic->sym - SYM_UP], STYLE_NINTENDO, cv, &symbol);
    }
    default: return ic->text[0] && draw_text_fit(cv, ic->text);
    }
}

// Coverage bounds of a canvas (false: empty)
static bool cv_bounds(const float *cv, int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = CV, *x1 = *y1 = -1;
    for (int i = 0; i < CV * CV; i++)
        if (cv[i] > 0.02f) {
            int x = i % CV, y = i / CV;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    return *x1 >= 0;
}

// `src`'s drawing fitted (aspect kept, centred) into the box bx0..bx1 / by0..by1 of `dst` (max with what's there)
static void cv_fit(float *dst, const float *src, float bx0, float by0, float bx1, float by1) {
    int sx0, sy0, sx1, sy1;
    if (!cv_bounds(src, &sx0, &sy0, &sx1, &sy1)) return;
    float sw = (float)(sx1 - sx0 + 1), sh = (float)(sy1 - sy0 + 1), k = fminf((bx1 - bx0) / sw, (by1 - by0) / sh);
    float ox = (bx0 + bx1) / 2 - sw * k / 2, oy = (by0 + by1) / 2 - sh * k / 2;
    for (int y = 0; y < CV; y++)
        for (int x = 0; x < CV; x++) {
            float u = sx0 + (x + 0.5f - ox) / k, v = sy0 + (y + 0.5f - oy) / k;
            int iu = (int)floorf(u), iv = (int)floorf(v);
            if (iu < sx0 || iv < sy0 || iu > sx1 || iv > sy1) continue;
            float a = src[iv * CV + iu];
            if (a > dst[y * CV + x]) dst[y * CV + x] = a;
        }
}

// The whole icon on the canvas (coverage 0..1), the filled or the outline look
static void draw_cell_icon(const CellIcon *ic, bool filled, float *cv) {
    static float shape[CV * CV], label[CV * CV], fitted[CV * CV];
    memset(shape, 0, sizeof shape);
    memset(fitted, 0, sizeof fitted);
    float lx0 = 62, ly0 = 62, lx1 = 194, ly1 = 194;  // (the label's box)
    if (ic->icon == ICON_MOUSE) {
        const float x0 = 52, y0 = 8, x1 = 204, y1 = 248, rad = 76, split = 112;
        if (filled) fill_rrect(shape, x0, y0, x1, y1, rad, false);
        else stroke_rrect(shape, x0, y0, x1, y1, rad, 20);
        static float btn[CV * CV];  // the button: the body's part above `split`, left / right of the middle
        memset(btn, 0, sizeof btn);
        fill_rrect(btn, x0, y0, x1, y1, rad, false);
        for (int y = 0; y < CV; y++)
            for (int x = 0; x < CV; x++) {
                bool in = y < split - 6 && (ic->mouse == 1 ? x < 122 : ic->mouse == 3 ? x > 134 : x > 108 && x < 148 && y > 36 && y < 96);
                if (!in) btn[y * CV + x] = 0;
            }
        Seg s[2] = {{x0 + 4, split, x1 - 4, split}, {128, y0 + 4, 128, split}};
        static float lines[CV * CV];
        memset(lines, 0, sizeof lines);
        if (ic->mouse != 2) draw_segs(lines, s, 2, 6);
        else draw_segs(lines, s, 1, 6);
        for (int i = 0; i < CV * CV; i++)
            cv[i] = filled ? fminf(shape[i], 1 - fmaxf(btn[i], lines[i])) : fmaxf(shape[i], fmaxf(btn[i], lines[i]));
        return;
    }
    if (ic->icon == ICON_KEYS4) {  // four small caps: one over three (text: up, left, down, right)
        const float k = 76, gap = 10, ax = (CV - (3 * k + 2 * gap)) / 2, ay = (CV - (2 * k + gap)) / 2, rad = 18;
        const float cx[4] = {ax + k + gap, ax, ax + k + gap, ax + 2 * (k + gap)}, cy[4] = {ay, ay + k + gap, ay + k + gap, ay + k + gap};
        for (int i = 0; i < 4; i++) {
            if (filled) fill_rrect(shape, cx[i], cy[i], cx[i] + k, cy[i] + k, rad, false);
            else stroke_rrect(shape, cx[i], cy[i], cx[i] + k, cy[i] + k, rad, 10);
            char ch[2] = {ic->text[i], 0};
            memset(label, 0, sizeof label);
            if (ch[0] && draw_text_fit(label, ch)) cv_fit(fitted, label, cx[i] + 18, cy[i] + 15, cx[i] + k - 18, cy[i] + k - 15);
        }
        for (int i = 0; i < CV * CV; i++) cv[i] = filled ? fminf(shape[i], 1 - fitted[i]) : fmaxf(shape[i], fitted[i]);
        return;
    }
    if (ic->icon == ICON_RING) {  // a stick: its ring
        if (filled) { for (int y = 0; y < CV; y++) for (int x = 0; x < CV; x++) shape[y * CV + x] = sat(118 - hypotf(x + 0.5f - 128, y + 0.5f - 128) + 0.5f); }
        else draw_ring(shape, 128, 128, 108, 20);
        lx0 = 64, ly0 = 76, lx1 = 192, ly1 = 180;
        if (ic->plabel) lx0 = ly0 = 70, lx1 = ly1 = 186;  // (a button's symbol: square)
    } else {  // a key cap
        if (filled) fill_rrect(shape, 10, 10, 246, 246, 46, false);
        else stroke_rrect(shape, 10, 10, 246, 246, 46, 20);
        size_t n = strlen(ic->text);
        if (!ic->sym && n >= 3) lx0 = 34, lx1 = 222;  // (longer words: the cap's width)
        if (!ic->sym && n == 1) ly0 = 52, ly1 = 204;
    }
    if (draw_key_label(ic, label)) cv_fit(fitted, label, lx0, ly0, lx1, ly1);
    for (int i = 0; i < CV * CV; i++) cv[i] = filled ? fminf(shape[i], 1 - fitted[i]) : fmaxf(shape[i], fitted[i]);
}

// Replace one glyph cell (x0,y0 w x h in the RGBA image, stored upside down when `flip`) by `ic`, sized and placed as
// the glyph's own shape was (its bounds: a square around them)
static void redraw_cell(uint8_t *img, int W, int x0, int y0, int w, int h, bool flip, const CellIcon *ic, bool filled,
                        uint8_t *dirty, int bw) {
    int gx0 = w, gy0 = h, gx1 = -1, gy1 = -1;  // the glyph's bounds, upright
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sy = flip ? h - 1 - y : y;
            if (img[((y0 + sy) * W + x0 + x) * 4 + 3] > 127) {
                if (x < gx0) gx0 = x;
                if (x > gx1) gx1 = x;
                if (y < gy0) gy0 = y;
                if (y > gy1) gy1 = y;
            }
        }
    if (gx1 < 0) return;
    static float cv[CV * CV];
    draw_cell_icon(ic, filled, cv);
    float side = (float)((gx1 - gx0 > gy1 - gy0 ? gx1 - gx0 : gy1 - gy0) + 1);
    float cx = (gx0 + gx1 + 1) / 2.0f, cy = (gy0 + gy1 + 1) / 2.0f, bx = cx - side / 2, by = cy - side / 2;
    float k = CV / side;  // canvas pixels per glyph pixel
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0;
            if (x >= bx - 1 && x <= bx + side && y >= by - 1 && y <= by + side)
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        int u = (int)((x + (sx + 0.5f) / 4 - bx) * k), v = (int)((y + (sy + 0.5f) / 4 - by) * k);
                        if (u >= 0 && v >= 0 && u < CV && v < CV) a += cv[v * CV + u];
                    }
            uint8_t val = (uint8_t)lroundf(sat(a / 16) * 255);
            int sy = flip ? h - 1 - y : y;
            uint8_t *p = img + ((y0 + sy) * W + x0 + x) * 4;
            if (p[3] != val || p[0] != val) {
                memset(p, val, 4);
                dirty[((y0 + sy) / 4) * bw + (x0 + x) / 4] = 1;
            }
        }
}

// The key cap / mouse for a keyboard binding (BIND_* code: a scancode or BIND_MOUSE_BASE + button). false: unbound.
static bool kb_icon_of(int code, CellIcon *ic) {
    memset(ic, 0, sizeof *ic);
    if (code <= 0) return false;
    if (bind_is_mouse(code)) {
        int b = code - BIND_MOUSE_BASE;  // SDL_BUTTON_LEFT 1, MIDDLE 2, RIGHT 3, X1 4, X2 5
        if (b <= 3) { ic->icon = ICON_MOUSE; ic->mouse = b; return true; }
        snprintf(ic->text, sizeof ic->text, "M%d", b);
        return true;
    }
    switch (code) {
    case SDL_SCANCODE_SPACE: ic->sym = SYM_SPACE; return true;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: ic->sym = SYM_SHIFT; return true;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: ic->sym = SYM_ENTER; return true;
    case SDL_SCANCODE_BACKSPACE: ic->sym = SYM_BACKSPACE; return true;
    case SDL_SCANCODE_UP: ic->sym = SYM_UP; return true;
    case SDL_SCANCODE_DOWN: ic->sym = SYM_DOWN; return true;
    case SDL_SCANCODE_LEFT: ic->sym = SYM_LEFT; return true;
    case SDL_SCANCODE_RIGHT: ic->sym = SYM_RIGHT; return true;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: strcpy(ic->text, "Ctrl"); return true;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: strcpy(ic->text, "Alt"); return true;
    case SDL_SCANCODE_TAB: strcpy(ic->text, "Tab"); return true;
    case SDL_SCANCODE_ESCAPE: strcpy(ic->text, "Esc"); return true;
    case SDL_SCANCODE_CAPSLOCK: strcpy(ic->text, "Caps"); return true;
    default: break;
    }
    const char *name = SDL_GetScancodeName((SDL_Scancode)code);
    if (!name || !*name) return false;
    if (!strncmp(name, "Keypad ", 7)) name += 7;  // ("Keypad 8": the digit)
    snprintf(ic->text, sizeof ic->text, "%.4s", name);
    return true;
}

// A stick's movement keys, when they're four single characters, as the four keys laid out like arrow keys (up over
// left, down, right): W over A S D, 8 over 4 5 6 (the numpad - a cap reading "8456" said nothing); text = up, left,
// down, right
static bool kb_stick_icon(int up, CellIcon *ic) {
    memset(ic, 0, sizeof *ic);
    static const int order[4] = {0, 2, 1, 3};  // BIND_*_UP, _DOWN, _LEFT, _RIGHT -> up, left, down, right
    char s[8] = "";
    for (int i = 0; i < 4; i++) {
        int code = g_bind_key[up + order[i]];
        const char *n = code > 0 && !bind_is_mouse(code) ? SDL_GetScancodeName((SDL_Scancode)code) : NULL;
        if (n && !strncmp(n, "Keypad ", 7)) n += 7;
        if (!n || strlen(n) != 1) return kb_icon_of(g_bind_key[up], ic);  // (else: the up key's own cap)
        s[i] = n[0], s[i + 1] = 0;
    }
    strcpy(ic->text, s);
    ic->icon = ICON_KEYS4;
    return true;
}

static const struct { uint16_t code; uint8_t role, filled; } k_arms_codes[] = {
    {0xE0A0, R_A, 0}, {0xE0A1, R_B, 0}, {0xE0A2, R_X, 0}, {0xE0A3, R_Y, 0}, {0xE0A4, R_L, 0}, {0xE0A5, R_R, 0},
    {0xE0A6, R_ZL, 0}, {0xE0A7, R_ZR, 0}, {0xE0B3, R_PLUS, 0}, {0xE0B4, R_MINUS, 0},
    {0xE0C1, AR_LSTICK, 0}, {0xE0C2, AR_RSTICK, 0}, {0xE0C4, AR_LS_PRESS, 0}, {0xE0C5, AR_RS_PRESS, 0},
    {0xE0E0, R_A, 1}, {0xE0E1, R_B, 1}, {0xE0E2, R_X, 1}, {0xE0E3, R_Y, 1}, {0xE0E4, R_L, 1}, {0xE0E5, R_R, 1},
    {0xE0E6, R_ZL, 1}, {0xE0E7, R_ZR, 1}, {0xE0EF, R_PLUS, 1}, {0xE0F0, R_MINUS, 1},
    {0xE101, AR_LSTICK, 1}, {0xE102, AR_RSTICK, 1}, {0xE104, AR_LS_PRESS, 1}, {0xE105, AR_RS_PRESS, 1},
};

static void arms_sheet_apply(int id, uint8_t *img, int W, int H, int style, uint8_t *dirty, int bw) {
    int fi = id >= 0 && id < (int)sizeof g_arms_font_of_ref ? g_arms_font_of_ref[id] : -1;
    if (getenv("LARECOMP_LOG_ICONS")) LOG("[icons] icon sheet (ref %d, font %d): style %d\n", id, fi, style);
    if (fi < 0) return;
    const ArmsFont *af = &g_arms_font[fi];
    static const int8_t slot[] = {[R_A] = BIND_A, [R_B] = BIND_B, [R_X] = BIND_X, [R_Y] = BIND_Y, [R_L] = BIND_L,
                                  [R_R] = BIND_R, [R_ZL] = BIND_ZL, [R_ZR] = BIND_ZR, [R_PLUS] = BIND_PLUS,
                                  [R_MINUS] = BIND_MINUS};
    for (int i = 0; i < (int)(sizeof k_arms_codes / sizeof *k_arms_codes); i++) {
        int g = -1;
        for (int k = 0; k < af->n && g < 0; k++) if (af->code[k] == k_arms_codes[i].code) g = af->glyph[k];
        if (g < 0) continue;
        int x0 = (g % af->cols) * af->cw, y0 = H - (g / af->cols + 1) * af->ch, role = k_arms_codes[i].role;
        bool filled = k_arms_codes[i].filled;
        if (y0 < 0 || x0 + af->cw > W) continue;
        CellIcon ic;
        if (style == STYLE_KB) {
            bool ok = role == AR_LSTICK ? kb_stick_icon(BIND_LS_UP, &ic) : role == AR_RSTICK ? kb_stick_icon(BIND_RS_UP, &ic)
                    : role == AR_LS_PRESS ? kb_icon_of(g_bind_key[BIND_LSTICK], &ic)
                    : role == AR_RS_PRESS ? kb_icon_of(g_bind_key[BIND_RSTICK], &ic) : kb_icon_of(g_bind_key[slot[role]], &ic);
            if (getenv("LARECOMP_LOG_ICONS"))
                LOG("[icons] U+%04X glyph %d cell (%d,%d) %s: icon %d mouse %d sym %d text \"%s\"\n", k_arms_codes[i].code, g,
                    x0, y0, filled ? "filled" : "outline", ic.icon, ic.mouse, ic.sym, ic.text);
            if (ok) redraw_cell(img, W, x0, y0, af->cw, af->ch, true, &ic, filled, dirty, bw);
        } else if (role == AR_LS_PRESS || role == AR_RS_PRESS) {  // L3 / R3 (Xbox: LS / RS) on the stick's ring
            if (style == STYLE_NINTENDO) continue;
            memset(&ic, 0, sizeof ic);
            ic.icon = ICON_RING;
            strcpy(ic.text, style == STYLE_PS ? (role == AR_LS_PRESS ? "L3" : "R3") : (role == AR_LS_PRESS ? "LS" : "RS"));
            redraw_cell(img, W, x0, y0, af->cw, af->ch, true, &ic, filled, dirty, bw);
        } else if (role <= R_MINUS && restyled(role, style)) {
            bool round = role <= R_Y || role == R_PLUS || role == R_MINUS;
            if (round && !filled) {  // (outline rings: relabelling thinned the ring where it met the old letter)
                memset(&ic, 0, sizeof ic);
                ic.icon = ICON_RING, ic.plabel = prompt_label(role), ic.pstyle = style;
                redraw_cell(img, W, x0, y0, af->cw, af->ch, true, &ic, false, dirty, bw);
            } else {
                relabel(img, W, x0, y0, af->cw, af->ch, 3, true, role, style, dirty, bw);
            }
        }
    }
}

// ------------------------------------------------------------------ cosmetics (Cosmetics tab): tunic and heart colours
// The colour rows share one palette (runtime/cosmetic_colour.h): the chosen colour is a target for the thing's typical
// colour, reached by a hue / saturation / value change that keeps the shading.
// Tunic: tools/rando/cosmetics.py's recolour (a seed option once, now live): where the green and red tunic maps differ
// (cloth, cap, collar ring) the green pixels change (PIL HSV, 0-255) from the green tunic's typical colour (hue 145,
// saturation 0.75, value 0.55 - its map reads 0.89 / 0.38, lit it looks paler and brighter) to the chosen one. Every
// mip level is recoloured from the game's own green map, so a seed's own colour is replaced too; "Default" uploads
// whatever the files hold (green, or an older seed's colour).
// Hearts: the chosen colour is what a full heart gets; empty hearts and the frame keep their shade of it (darker).
static uint32_t tunic_rgb(void) {  // (+1: 0 = Default)
    uint32_t rgb;
    return col_target(&g_settings.tunic, COL_SLOT_TUNIC, &rgb) ? rgb + 1 : 0;
}

// LARECOMP_COSMETICS=<frame>:<tunic>:<hearts>,...  test hook: set the colours (palette names / #RRGGBB) at those frames
static void cosmetics_test(void) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_COSMETICS"); }
    long at;
    char tu[32], he[32];
    if (!next || !*next || sscanf(next, "%ld:%31[^:]:%31[^,]", &at, tu, he) != 3) return;
    extern uint64_t renderer_present_count(void);
    if ((long)renderer_present_count() < at) return;
    col_parse(&g_settings.tunic, tu);
    col_parse(&g_settings.hearts, he);
    LOG("[cosmetics] test: tunic %s, hearts %s\n", tu, he);
    next = strchr(next, ',');
    if (next) next++;
}

static float srgb_lin(int c) { float v = c / 255.0f; return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }

// Heart draws: the shader's luminance tint (translate.c `ltint`) maps the red's brightness onto the chosen colour,
// scaled so the full heart's red (255, 47, 47: linear luminance 0.319) becomes exactly that colour; the empty heart
// (92, 15, 15) and the frame's darker red keep their brightness relative to it.
void texpatch_heart_tint(int kind, float tint[8]) {
    cosmetics_test();
    uint32_t rgb;
    if (!kind || !col_target(&g_settings.hearts, COL_SLOT_HEARTS, &rgb)) return;
    for (int k = 0; k < 3; k++) tint[k] = srgb_lin((int)(rgb >> (16 - 8 * k) & 255)) / 0.319f;
    tint[3] = 1;
    tint[4] = tint[5] = tint[6] = 0;
    tint[7] = 1;
}

static void bc1_decode(const uint8_t *b, uint8_t out[16][4]) {  // (BC1: 3-colour mode when c0 <= c1)
    uint16_t e0 = rd16(b), e1 = rd16(b + 2);
    int c[4][3];
    rgb565(e0, c[0]); rgb565(e1, c[1]);
    for (int k = 0; k < 3; k++) {
        if (e0 > e1) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
        else { c[2][k] = (c[0][k] + c[1][k]) / 2; c[3][k] = 0; }
    }
    uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; i++) {
        int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 3; k++) out[i][k] = (uint8_t)c[s][k];
        out[i][3] = e0 <= e1 && s == 3 ? 0 : 255;
    }
}

static void bc1_image(const uint8_t *lin, uint32_t w, uint32_t h, uint8_t *rgb) {  // linear BC1 blocks -> RGB
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bc1_decode(lin + (by * bw + bx) * 8, px);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(rgb + (y * w + x) * 3, px[i], 3);
            }
        }
}

static uint8_t clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void hsv_shift(uint8_t *p, const float *m) {  // PIL's RGB <-> HSV (all 0-255), hue moved from green
    float r = p[0], g = p[1], b = p[2], mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), h = 0, s = 0, v = mx;
    if (mx > mn) {
        s = (mx - mn) * 255 / mx;
        float rc = (mx - r) / (mx - mn), gc = (mx - g) / (mx - mn), bc = (mx - b) / (mx - mn);
        h = r == mx ? bc - gc : g == mx ? 2 + rc - bc : 4 + gc - rc;
        h = fmodf(h / 6 + 1, 1) * 255;
    }
    if (m[0] >= 0) h = fmodf(floorf(h) + (m[0] - 100 /* the green tunic's hue */) + 256, 256);
    s = clamp8(s * m[1]); v = clamp8(v * m[2]);
    float hh = h / 255 * 6, f, q, t, pp, ss = s / 255;
    int i = (int)hh % 6;
    f = hh - floorf(hh); pp = v * (1 - ss); q = v * (1 - ss * f); t = v * (1 - ss * (1 - f));
    float o[6][3] = {{v, t, pp}, {q, v, pp}, {pp, v, t}, {pp, q, v}, {t, pp, v}, {v, pp, q}};
    for (int k = 0; k < 3; k++) p[k] = clamp8(o[i][k] + 0.5f);
}

// One mip level of the recoloured tunic (linear BC1 blocks), made on first use and kept for that colour
static uint8_t *g_tunic_lv[16];
static uint32_t g_tunic_lv_colour;
static const uint8_t *tunic_level(uint32_t colour, uint32_t lv) {  // (colour: tunic_rgb())
    if (colour != g_tunic_lv_colour) {
        for (int i = 0; i < 16; i++) { free(g_tunic_lv[i]); g_tunic_lv[i] = NULL; }
        g_tunic_lv_colour = colour;
    }
    int gi = ref_of(R_TUNIC), ri = ref_of(R_TUNIC_RED);
    if (lv >= 16 || lv >= g_tunic_layout.levels || gi < 0 || ri < 0 || g_refs[gi].size != g_refs[ri].size) return NULL;
    if (g_tunic_lv[lv]) return g_tunic_lv[lv];
    LevelLayout l = tegra_level(&g_tunic_layout, lv);
    if (l.offset + (uint64_t)l.wb * l.hb * 8 > g_refs[gi].size) return NULL;
    uint32_t W = l.w, H = l.h, n = W * H, nb = l.wb * l.hb;
    uint8_t *green = malloc((size_t)nb * 8), *red = malloc((size_t)nb * 8), *gimg = malloc((size_t)n * 3), *rimg = malloc((size_t)n * 3);
    float *mask = malloc(n * sizeof(float)), *tmp = malloc(n * sizeof(float));
    tegra_deswizzle(g_refs[gi].data + l.offset, green, l.wb, l.hb, 1, 8, l.block_height_log2, l.block_depth_log2);
    tegra_deswizzle(g_refs[ri].data + l.offset, red, l.wb, l.hb, 1, 8, l.block_height_log2, l.block_depth_log2);
    bc1_image(green, W, H, gimg);
    bc1_image(red, W, H, rimg);
    // mask: luma of the difference, (d - 12) * 6, Gaussian blur (sigma 1.2) - as cosmetics.py did
    for (uint32_t i = 0; i < n; i++) {
        int d[3];
        for (int k = 0; k < 3; k++) d[k] = abs(gimg[i * 3 + k] - rimg[i * 3 + k]);
        float lu = (d[0] * 299 + d[1] * 587 + d[2] * 114) / 1000.0f;
        mask[i] = fminf(fmaxf((lu - 12) * 6, 0), 255) / 255;
    }
    float kern[7], ks = 0;
    for (int i = -3; i <= 3; i++) ks += kern[i + 3] = expf(-(float)(i * i) / (2 * 1.2f * 1.2f));
    for (int pass = 0; pass < 2; pass++) {  // (separable: rows into tmp, then columns back)
        const float *src = pass ? tmp : mask;
        float *dst = pass ? mask : tmp;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                float acc = 0;
                for (int i = -3; i <= 3; i++) {
                    int xx = (int)x + (pass ? 0 : i), yy = (int)y + (pass ? i : 0);
                    xx = xx < 0 ? 0 : xx >= (int)W ? (int)W - 1 : xx;
                    yy = yy < 0 ? 0 : yy >= (int)H ? (int)H - 1 : yy;
                    acc += src[yy * W + xx] * kern[i + 3];
                }
                dst[y * W + x] = acc / ks;
            }
    }
    float sh, sf, vf;
    col_mapping(145 / 360.0f, 0.75f, 0.55f, colour - 1, &sh, &sf, &vf);
    float m3[3] = {sh == 0 ? -1 : 100 + (sh - floorf(sh)) * 255, sf, vf};  // (hsv_shift: PIL hue, moved from the green's 100)
    uint8_t *out = malloc((size_t)nb * 8);
    memcpy(out, green, (size_t)nb * 8);
    for (uint32_t by = 0; by < l.hb; by++)
        for (uint32_t bx = 0; bx < l.wb; bx++) {
            uint8_t px[16][4];
            bool any = false;
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                x = x < W ? x : W - 1;
                y = y < H ? y : H - 1;
                uint32_t at = y * W + x;
                float m = mask[at];
                uint8_t c[3] = {gimg[at * 3], gimg[at * 3 + 1], gimg[at * 3 + 2]};
                if (m > 0.002f) {
                    any = true;
                    hsv_shift(c, m3);
                    for (int k = 0; k < 3; k++) c[k] = clamp8(gimg[at * 3 + k] + (c[k] - gimg[at * 3 + k]) * m + 0.5f);
                }
                memcpy(px[i], c, 3);
                px[i][3] = 255;
            }
            if (any) bc1_encode4(px, out + (by * l.wb + bx) * 8);
        }
    free(green); free(red); free(gimg); free(rimg); free(mask); free(tmp);
    return g_tunic_lv[lv] = out;
}

static void tunic_apply(uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    uint32_t c = tunic_rgb();
    if (!c || !g_tunic_layout_ok) return;
    const uint8_t *src = tunic_level(c, lv);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (!src || row < bw * 8) return;
    for (uint32_t y = 0; y < bh; y++) memcpy(blocks + y * row, src + y * bw * 8, bw * 8);
}

// Link's skin colour (Cosmetics tab "Body color"): any colour over his skin, keeping the shading. The skin is told by
// its colour in the maps (BC1): a peach hue, not grey, bright - so the brows and eyes (dark), the mouth's dark lines
// and open mouths, the hair, the tunic and the leather stay (skin_face.h skin_weight). The colour row's target is
// reached from the skin's typical colour (hue 21, saturation 0.47, value 0.96).
static uint32_t skin_rgb(void) {  // (+1: 0 = Default)
    uint32_t rgb;
    return col_target(&g_settings.skin, COL_SLOT_SKIN, &rgb) ? rgb + 1 : 0;
}

// body: the tunic map (arms, hands), recoloured block by block, the skin told on `orig` (the map as uploaded: a
// tunic recoloured to a peach hue would read as skin; NULL: on the blocks themselves); else a face map, recoloured as
// a whole level (skin_face: with a very dark colour its details are told apart by shape). mouth: one of the mouth maps.
// The face maps' smaller mip levels are made from the recoloured full-size level (box filter), as the game's own are
// from its: recoloured on their own, the details' shapes are too few pixels to tell apart (an open mouth's outline
// would fill it). The levels of one upload come in order, on one thread: the full-size result is kept per thread.
static __thread struct { int id; uint32_t c, w, h; uint8_t *img; } t_face;

static void skin_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row, bool body, bool mouth,
                       const uint8_t *orig) {
    uint32_t c = skin_rgb();
    if (!c) return;
    float sh, ts, tv;
    SkinParams k;
    col_mapping(21 / 360.0f, 0.47f, 0.96f, c - 1, &sh, &k.sf, &k.vf);
    col_hsv(c - 1, &k.th, &ts, &tv);
    k.grey = sh == 0 && ts < 0.08f;
    // A very dark body colour (Black, or any Custom / Random one this dark: luminance under 0.22) would swallow the
    // face's dark details - pupils, brows, the mouth's and ears' lines, all drawn dark in the face maps on the skin.
    // Then they turn light, open mouths keeping their dark inside within a light outline (skin_face).
    float lum = (0.299f * (float)((c - 1) >> 16 & 255) + 0.587f * (float)((c - 1) >> 8 & 255) + 0.114f * (float)((c - 1) & 255)) / 255;
    k.light = !body && lum < 0.22f;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (row < bw * 8) return;
    if (body) {
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                uint8_t *b = blocks + by * row + bx * 8, px[16][4], ox[16][4];
                bc1_decode(b, px);
                bc1_decode(orig ? orig + by * row + bx * 8 : b, ox);
                bool any = false, holes = false;
                for (int i = 0; i < 16; i++) {
                    if (!px[i][3]) { holes = true; continue; }
                    float m = skin_weight(ox[i], true, false);
                    if (m < 0.004f) continue;
                    any = true;
                    skin_recolour(px[i], m, &k);
                }
                if (any && !holes) bc1_encode4(px, b);
            }
        return;
    }
    uint32_t iw = bw * 4, ih = bh * 4;  // (whole blocks: a level under 4 x 4 is padded)
    uint8_t *img = malloc((size_t)iw * ih * 4), *was = malloc((size_t)iw * ih * 4);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bc1_decode(blocks + by * row + bx * 8, px);
            for (int i = 0; i < 16; i++) memcpy(img + (((by * 4 + (i >> 2)) * iw) + bx * 4 + (i & 3)) * 4, px[i], 4);
        }
    memcpy(was, img, (size_t)iw * ih * 4);
    if (lv && t_face.img && t_face.id == id && t_face.c == c && (t_face.w >> lv ? t_face.w >> lv : 1) == w &&
        (t_face.h >> lv ? t_face.h >> lv : 1) == h) {
        uint32_t f = 1u << lv;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t acc = 0, cnt = 0;
                    for (uint32_t yy = y * f; yy < (y + 1) * f && yy < t_face.h; yy++)
                        for (uint32_t xx = x * f; xx < (x + 1) * f && xx < t_face.w; xx++, cnt++)
                            acc += t_face.img[(yy * t_face.w + xx) * 4 + ch];
                    if (cnt) img[(y * iw + x) * 4 + ch] = (uint8_t)((acc + cnt / 2) / cnt);
                }
    } else {
        skin_face(img, iw, ih, mouth, &k);
        if (!lv && iw == w && ih == h) {
            free(t_face.img);
            t_face.img = malloc((size_t)w * h * 4);
            memcpy(t_face.img, img, (size_t)w * h * 4);
            t_face.id = id, t_face.c = c, t_face.w = w, t_face.h = h;
        }
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            bool any = false, holes = false;
            for (int i = 0; i < 16; i++) {
                size_t at = (((by * 4 + (i >> 2)) * iw) + bx * 4 + (i & 3)) * 4;
                memcpy(px[i], img + at, 4);
                holes |= !was[at + 3];
                any |= memcmp(img + at, was + at, 3) != 0;
            }
            if (any && !holes) bc1_encode4(px, blocks + by * row + bx * 8);
        }
    free(img);
    free(was);
}

// pictogram.bffnt: glyph index -> role (its CMAP: '+' 0, '-' 1, 'A' 2, 'B' 3, 'L' 6, 'R' 8, 'X' 10, 'Y' 11, 'Z' 12
// = ZL, 'l' 13 / 'r' 15 = L / R (second style), 'z' 17 = ZR); 4 columns of 64x64 cells, rows from the bottom.
static const int8_t k_sheet_roles[18] = {R_PLUS, R_MINUS, R_A, R_B, 0, 0, R_L, 0, R_R, 0, R_X, R_Y, R_ZL, R_L, 0, R_R, 0, R_ZR};

// BC7 mode 6 (one subset, RGBA endpoints of 7 bits + a p-bit each, 4-bit indices): a block's two endpoints from its
// colours' range, each channel's ends ordered as it varies with the channel that varies most; flat art (the Rearmed
// splash, the logo circle) encodes cleanly.
static void bc7_encode6(const uint8_t px[16][4], uint8_t *out) {
    static const int wt[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int mn[4], mx[4], ref = 0;
    float mean[4] = {0};
    for (int c = 0; c < 4; c++) {
        mn[c] = 255, mx[c] = 0;
        for (int i = 0; i < 16; i++) {
            if (px[i][c] < mn[c]) mn[c] = px[i][c];
            if (px[i][c] > mx[c]) mx[c] = px[i][c];
            mean[c] += px[i][c] / 16.0f;
        }
        if (mx[c] - mn[c] > mx[ref] - mn[ref]) ref = c;
    }
    int e[2][4];
    for (int c = 0; c < 4; c++) {
        float cov = 0;
        for (int i = 0; i < 16; i++) cov += (px[i][c] - mean[c]) * (px[i][ref] - mean[ref]);
        e[0][c] = cov < 0 ? mx[c] : mn[c];
        e[1][c] = cov < 0 ? mn[c] : mx[c];
    }
    int q[2][4], p[2], ex[2][4];  // 7-bit endpoints, p-bits, their 8-bit values
    for (int k = 0; k < 2; k++) {
        int best = 1 << 30;
        for (int pb = 0; pb < 2; pb++) {
            int err = 0, qq[4];
            for (int c = 0; c < 4; c++) {
                int v = (e[k][c] - pb + 1) >> 1;
                qq[c] = v < 0 ? 0 : v > 127 ? 127 : v;
                err += abs(((qq[c] << 1) | pb) - e[k][c]);
            }
            if (err < best) { best = err; p[k] = pb; memcpy(q[k], qq, sizeof qq); }
        }
        for (int c = 0; c < 4; c++) ex[k][c] = (q[k][c] << 1) | p[k];
    }
    int pal[16][4], idx[16];
    for (int j = 0; j < 16; j++)
        for (int c = 0; c < 4; c++) pal[j][c] = ((64 - wt[j]) * ex[0][c] + wt[j] * ex[1][c] + 32) >> 6;
    for (int i = 0; i < 16; i++) {
        int best = 1 << 30;
        for (int j = 0; j < 16; j++) {
            int d = 0;
            for (int c = 0; c < 4; c++) d += (pal[j][c] - px[i][c]) * (pal[j][c] - px[i][c]);
            if (d < best) { best = d; idx[i] = j; }
        }
    }
    if (idx[0] & 8) {  // (the anchor index keeps only 3 bits: swap the endpoints so its top bit is 0)
        for (int c = 0; c < 4; c++) { int t = q[0][c]; q[0][c] = q[1][c]; q[1][c] = t; }
        int t = p[0]; p[0] = p[1]; p[1] = t;
        for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
    }
    memset(out, 0, 16);
    put_bits(out, 0, 7, 0x40);  // mode 6
    for (int c = 0; c < 4; c++) { put_bits(out, 7 + c * 14, 7, (uint32_t)q[0][c]); put_bits(out, 14 + c * 14, 7, (uint32_t)q[1][c]); }
    put_bits(out, 63, 1, (uint32_t)p[0]);
    put_bits(out, 64, 1, (uint32_t)p[1]);
    put_bits(out, 65, 3, (uint32_t)idx[0]);
    for (int i = 1; i < 16; i++) put_bits(out, 68 + (i - 1) * 4, 4, (uint32_t)idx[i]);
}

// The Rearmed replacement of known texture `id` (R_BRAND) into its level 0, in its own format
static void brand_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    const Ref *r = &g_refs[id];
    int bi = g_brand_of_ref[id];
    if (lv || bi < 0 || r->w != w || r->h != h || !texpatch_brand_on()) return;
    Brand *b = &g_brand[bi];
    if (!b->tried) {
        b->tried = true;
        size_t len;
        b->px = read_file(b->rgba, &len);
        if (b->px && len != (size_t)w * h * 4) { free(b->px); b->px = NULL; }
        if (!b->px) LOG("[texpatch] Rearmed art: %s missing or the wrong size (set the game up again)\n", b->rgba);
    }
    if (!b->px) return;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w) x = w - 1;
                if (y >= h) y = h - 1;
                memcpy(px[i], b->px + ((size_t)y * w + x) * 4, 4);
            }
            if (r->kind == K_BC4) {
                uint8_t v[16];
                for (int i = 0; i < 16; i++) v[i] = px[i][0];
                bc4_encode(v, blocks + by * row + bx * 8);
            } else {
                bc7_encode6(px, blocks + by * row + bx * 16);
            }
        }
    static int nlog;
    if (nlog++ < 8) LOG("[texpatch] Rearmed art: %s\n", r->name);
}

void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    if (id < 0 || id >= g_nrefs) return;
    const Ref *r = &g_refs[id];
    if (r->role == R_BRAND) { brand_apply(id, lv, blocks, w, h, row); return; }
    if (r->role == R_TUNIC || r->role == R_TUNIC_RED || r->role == R_TUNIC_BLUE) {
        size_t n = (size_t)((h + 3) / 4 - 1) * row + (w + 3) / 4 * 8;  // (the level's blocks)
        uint8_t *orig = tunic_rgb() && skin_rgb() && row >= (w + 3) / 4 * 8 ? malloc(n) : NULL;
        if (orig) memcpy(orig, blocks, n);
        tunic_apply(lv, blocks, w, h, row);
        skin_apply(id, lv, blocks, w, h, row, true, false, orig);  // (the arms and hands)
        free(orig);
        return;
    }
    if (r->role == R_SKIN) { skin_apply(id, lv, blocks, w, h, row, false, !strncmp(r->name, "MI_mouth_alb", 12), NULL); return; }
    if (lv || r->w != w || r->h != h) return;
    if (r->role == R_BOTTLE || r->role == R_BOTTLE_MODEL || r->role == R_FAIRY_MODEL || r->role == R_POWDER_MODEL ||
        r->role == R_ITEM_CURSOR || r->role == R_DGN_MAP || r->role == R_MAP_MODEL || r->role == R_POWER_PIECE_MODEL ||
        r->role == R_HEART) return;
    if (r->role == R_HERO) {  // fully transparent (BC5 blocks of zeros)
        if (hide_hero()) for (uint32_t y = 0; y < (h + 3) / 4; y++) memset(blocks + y * row, 0, (w + 3) / 4 * 16);
        return;
    }
    int style = prompt_style();
    if (style == STYLE_NINTENDO) {  // the game's own look, unless a button was remapped
        bool any = false;
        for (int rl = R_A; rl <= R_MINUS; rl++) any |= restyled(rl, style);
        if (!any) return;
    }
    static pthread_once_t font_once = PTHREAD_ONCE_INIT;
    pthread_once(&font_once, load_font);
    int bw = (int)(w + 3) / 4;
    uint8_t *img = malloc((size_t)w * h * 4), *dirty = calloc((size_t)bw * ((h + 3) / 4), 1);
    decode_image(r, blocks, row, img);
    int ach = r->kind == K_BC5 ? 1 : 3;
    if (r->role == R_ARMS_SHEET) {
        arms_sheet_apply(id, img, (int)w, (int)h, style, dirty, bw);
    } else if (r->role == R_SHEET) {
        for (int g = 0; g < (int)sizeof k_sheet_roles; g++) {
            int y0 = (int)h - (g / 4 + 1) * 64;
            if (k_sheet_roles[g] && y0 >= 0 && (g % 4 + 1) * 64 <= (int)w && restyled(k_sheet_roles[g], style))
                relabel(img, (int)w, (g % 4) * 64, y0, 64, 64, ach, true, k_sheet_roles[g], style, dirty, bw);
        }
    } else if (restyled(r->role, style)) {
        relabel(img, (int)w, 0, 0, (int)w, (int)h, ach, false, r->role, style, dirty, bw);
    }
    encode_image(r, img, dirty, blocks, row);
    free(img);
    free(dirty);
}

// The Hint Sheet's icon (runtime/hle/hint_sheet.c): the dungeon map's (120 x 120) centred on a bottle-sized canvas
// (144 x 144, as ItemBottle_00^w: the same pane draws it at the size the game draws its own item icons), RGBA8 sRGB,
// made once from the user's own game files. The renderers sample it in place of the bottle icon for the Hint Sheet's
// draws. NULL: the game files don't have it.
NvnTexture *texpatch_hint_icon(void) {
    static NvnTexture t;
    static int state;  // 0 not made yet, 1 made, -1 none
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!state) {
        state = -1;
        pthread_once(&g_once, load_refs);
        int i = ref_of(R_DGN_MAP);
        if (i >= 0 && g_refs[i].kind == K_BC3) {
            const Ref *r = &g_refs[i];
            uint32_t bw = (r->w + 3) / 4, bh = (r->h + 3) / 4, S = 144;
            uint8_t *lin = malloc((size_t)bw * bh * 16), *img = malloc((size_t)r->w * r->h * 4), *canvas = calloc((size_t)S * S, 4);
            tegra_deswizzle(r->data, lin, bw, bh, 1, 16, r->bhl, 0);
            decode_image(r, lin, bw * 16, img);
            uint32_t ox = (S - r->w) / 2, oy = (S - r->h) / 2;
            for (uint32_t y = 0; y < r->h; y++) memcpy(canvas + ((y + oy) * S + ox) * 4, img + y * r->w * 4, r->w * 4);
            t.target = 1;  // (2D)
            t.format = r->srgb ? 0x38 : 0x25;  // RGBA8 sRGB / unorm, as the map's own colour space
            t.width = t.height = S;
            t.depth = t.levels = 1;
            t.layout = (TexLayout){.width = S, .height = S, .depth = 1, .layers = 1, .levels = 1, .fmt = nvn_format_info(t.format)};
            tegra_layout_init(&t.layout);
            uint64_t total = tegra_total_size(&t.layout);
            uint8_t *mem = calloc(total, 1);
            LevelLayout l0 = tegra_level(&t.layout, 0);
            tegra_swizzle(canvas, mem, l0.wb, l0.hb, 1, 4, l0.block_height_log2, l0.block_depth_log2);
            t.addr = (uint64_t)(uintptr_t)mem;
            strlcpy(t.label, "HintSheetIcon", sizeof t.label);
            free(lin), free(img), free(canvas);
            state = 1;
            LOG("[texpatch] Hint Sheet icon made from %s\n", r->name);
        }
    }
    pthread_mutex_unlock(&lock);
    return state > 0 ? &t : NULL;
}

// The game's own UI textures, by name (e.g. "MapMark_05^w"), decoded to RGBA8 (w x h, the texture's own colour
// encoding) from the user's UI archives: out[i] NULL if it isn't there or its format isn't one of ours. Each archive is
// read once for the whole list. The caller frees them; returns how many were found.
// (Runtime-made icons for our overlays: runtime/ui/triforce_icons.mm, race_overlay.mm - nothing of the game ships.)
int texpatch_ui_rgba_list(const char *const *want, int nwant, uint8_t **out, int *w_out, int *h_out) {
    static const char *const arcs[] = {"region_common/ui/Game.arc", "regionUS/common/ui/GameRegionCommon.arc",
                                       "regionEU/common/ui/GameRegionCommon.arc", "region_common/ui/StartUp.arc"};
    int found = 0;
    for (int i = 0; i < nwant; i++) out[i] = NULL;
    for (int a = 0; a < 4 && found < nwant; a++) {
        size_t len;
        uint8_t *d = read_all(arcs[a], &len);
        if (!d) continue;
        if (len > 0x20 && !memcmp(d, "SARC", 4)) {
            uint32_t data_off = rd32(d + 0xC);
            uint16_t n = rd16(d + 0x1A);
            const uint8_t *ent = d + 0x20, *names = d + 0x20 + 16 * n + 8;
            for (uint16_t i = 0; i < n && found < nwant && ent + 16 * (i + 1) <= d + len; i++) {
                uint32_t attr = rd32(ent + 16 * i + 4), s = rd32(ent + 16 * i + 8), e = rd32(ent + 16 * i + 12);
                const char *nm = (attr >> 24) ? (const char *)names + (attr & 0xffffff) * 4 : "";
                if ((const uint8_t *)nm >= d + len || strcmp(nm, "timg/__Combined.bntx") || data_off + e > len) continue;
                const uint8_t *b = d + data_off + s;
                size_t blen = e - s;
                if (blen < 0x40 || memcmp(b, "BNTX", 4)) continue;
                uint32_t count = rd32(b + 0x24);
                uint64_t infos = rd64(b + 0x28);
                for (uint32_t k = 0; k < count && found < nwant && infos + 8 * k + 8 <= blen; k++) {
                    uint64_t brti = rd64(b + infos + 8 * k);
                    if (brti + 0x90 > blen || memcmp(b + brti, "BRTI", 4)) continue;
                    const uint8_t *t = b + brti + 16;
                    uint64_t name_at = rd64(t + 80), ptrs = rd64(t + 96);
                    if (name_at + 2 >= blen) continue;
                    uint16_t nl = rd16(b + name_at);
                    int wi = -1;
                    for (int q = 0; q < nwant && wi < 0; q++)
                        if (!out[q] && nl == strlen(want[q]) && !memcmp(b + name_at + 2, want[q], nl)) wi = q;
                    if (wi < 0 || ptrs + 8 > blen) continue;
                    uint32_t fmt = rd32(t + 12), w = rd32(t + 20), h = rd32(t + 24), size = rd32(t + 64);
                    uint64_t data = rd64(b + ptrs);
                    int kind = (fmt >> 8) == 0x1c ? K_BC3 : (fmt >> 8) == 0x2d ? K_ASTC4 : -1;
                    if (kind < 0 || data + size > blen) continue;
                    Ref r = {0, (uint8_t)kind, (uint8_t)((fmt & 0xff) == 6), 0, (uint16_t)w, (uint16_t)h, 1, size, NULL, ""};
                    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                    uint8_t *lin = calloc((size_t)bw * bh, 16);
                    tegra_deswizzle(b + data, lin, bw, bh, 1, 16, rd32(t + 36) & 7, 0);
                    out[wi] = malloc((size_t)w * h * 4);
                    decode_image(&r, lin, bw * 16, out[wi]);
                    free(lin);
                    w_out[wi] = (int)w, h_out[wi] = (int)h;
                    found++;
                }
            }
        }
        free(d);
    }
    return found;
}

// One of them (NULL: not there)
uint8_t *texpatch_ui_rgba(const char *want, int *w_out, int *h_out) {
    uint8_t *out;
    return texpatch_ui_rgba_list(&want, 1, &out, w_out, h_out) ? out : NULL;
}

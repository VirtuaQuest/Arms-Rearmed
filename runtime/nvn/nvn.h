// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include "../hle.h"
#include "tegra.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
    // NVN_MEMORY_POOL_FLAGS_VIRTUAL (0x200): no storage of its own; MapVirtual points ranges of it at physical pools'
    // memory (objects.c nvn_pool_addr). Ranges never mapped read as `backing`, zeroed host memory made on first use.
    struct NvnVMap { uint64_t voff, size, phys; } *maps;
    int nmaps, cap_maps;
    uint64_t backing;
} NvnPool;
#define NVN_POOL_VIRTUAL 0x200u
// Host objects behind guest NVN objects (objects.c). NVN objects are plain data on the Switch and games copy them
// by value (ARMS: textures and samplers) and finalize the copies too - sometimes only the copies. A host object is
// released by the first Finalize of any object holding it, once; later ones (other copies) do nothing, and a copy
// used after that names no live object (nvn_texture_of: left out, never a freed pointer).
void nvn_live_add(void *host, uint64_t owner);
bool nvn_live_release(void *host, uint64_t guest);  // true: `host` was live (now released: free it)
bool nvn_live_has(const void *host);
// The live host texture behind a guest NVNtexture* (NULL, logged, when it names none: never initialized, or overwritten)
struct NvnTexture *nvn_texture_of(uint64_t guest, const char *what);
// The host address of `off` in a pool (a virtual pool's offsets go through its mappings)
uint64_t nvn_pool_addr(NvnPool *p, uint64_t off);

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    uint32_t pool_refs;  // texture pool entries that name it (cleared when it is finalized: runtime/nvn/objects.c)
    bool format_views;   // named through a view in a format the backend can only give with a flag (Metal: PixelFormatView)
    char label[64];
    int present_crop[4]; // the window's crop (x, y, w, h texels) when it was last presented (ARMS: dynamic resolution)
    bool finalized;      // nvnTextureFinalize ran (through any copy); collected later (objects.c nvn_texture_gc)
    uint64_t fin_at, last_use;  // present counts: when finalized, when last named through a guest object
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // our layout of the guest NVNtextureView (0x28 bytes)

// A view naming a texture (pool entry, render target, clear, copy): one in another format is told to the backend before
// the texture is used through it.
void backend_texture_view_format(NvnTexture *tex, uint32_t format);  // (runtime/nvn/metal.m, vulkan/textures.c)
static inline void nvn_texture_note_view(NvnTexture *t, const NvnTextureView *v) {
    if (t && v && (v->setmask & 4) && v->format != t->format) backend_texture_view_format(t, v->format);
}

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
    uint32_t dead_format;  // the texture registered here was finalized (NVN format + 1; 0 = no): tex is NULL
} NvnTexEntry;

// A draw named a texture pool slot whose texture was finalized (runtime/nvn/objects.c): logged, a few times
void nvn_dead_slot_sampled(const NvnTexEntry *e, uint32_t slot);

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (stored in guest command memory)
typedef struct CmdChunk CmdChunk;
typedef struct {
    // Records live in host blocks, never in the game's command memory (cmdbuf.c: why, and when blocks are reused)
    uint64_t mem;    // the host block being written
    uint64_t size;   // its size
    uint64_t used;   // bytes used in it
    uint64_t chunk, chunk_size;  // the guest chunk last added and its size (never written)
    uint64_t vused;              // what the game is told is used in it: an estimate of NVN's own encoding
    bool oom_called;             // the game's out-of-memory callback ran for this chunk
    CmdChunk *hc;                // the chunk's host blocks
    uint32_t hc_gen;             // hc's generation when taken (it changes when the game reuses the chunk)
    int blk;                     // the block of hc in use
    uint64_t next_start, cc_epoch;  // the next chunk's start after ours (and the chunk epoch it was found in)
    uint64_t start;  // first record of the list being recorded
    uint64_t callback, callback_data;
    bool recording;
    uint64_t guest;  // the guest NVNcommandBuffer*
} NvnCmdBuf;

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH, CMD_SET_DEPTH_CLAMP,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);

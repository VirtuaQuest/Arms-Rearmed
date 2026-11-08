// NVN command buffers. Commands are recorded as CmdRec records in host memory, never in the game's command memory:
// our records are several times the size of NVN's own encoding, and ARMS hands out command memory from an arena by
// what GetCommandMemoryUsed says a list took, so lists written in the game's memory were overwritten by the next
// ones (CallCommands targets changed after recording; render targets read as garbage pointers).
//
// Each guest chunk a command buffer is given (AddCommandMemory) gets host blocks (CmdChunk); a list's handle is the
// host address of its first record. A chunk's blocks are recycled when the game adds command memory that starts
// inside it again: on the Switch that overwrites the lists recorded there, so the game is done with them (it waits
// for the GPU first, as our fences are signalled when the render thread has run a list). The game is told an
// estimate of NVN's own usage (vused), so its arena advances about as it does on the console.
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#define ONCE(what) do { static int n_; if (!n_++) LOG("[nvn] %s: first call, not implemented%c", what, 10); } while (0)

#define JUMP_RESERVE (sizeof(CmdRec) + 8)
#define BLOCK_MAX (64u << 10)

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    nvn_live_add(cb, A(0));
    RET(1);
}
static void cb_release_orphan(NvnCmdBuf *cb);
NVN(nvnCommandBufferFinalize) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (nvn_live_release(cb, A(0))) {
        cb_release_orphan(cb);
        free(cb);
    }
    SET_HOST(A(0), 0);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// ---- host chunks: live ones sorted by guest start address. A retired chunk is quarantined for QUARANTINE presented
// frames before its blocks are reused: our estimate of NVN's usage is only an estimate, so a list the game still
// calls (CallCommands) in the same frame may sit in a chunk that looked overwritten; the render thread also runs a
// frame or two behind.
#define QUARANTINE 6
struct CmdChunk {
    uint64_t start, size;
    uint32_t gen;         // bumped when retired: a command buffer still holding it moves on
    bool orphan;          // a command buffer's private chunk (its guest chunk was retired while it recorded)
    uint64_t retired_at;  // present count when retired
    int n, cap;           // blocks
    void **blk;
    uint32_t *sz;
};
static pthread_mutex_t g_cc_lock = PTHREAD_MUTEX_INITIALIZER;
static CmdChunk **g_cc, **g_q;  // live chunks; the quarantine (a FIFO ring, oldest first)
static size_t g_cc_n, g_cc_cap, g_q_head, g_q_n, g_q_cap;
static uint64_t g_cc_bytes;  // host bytes in blocks (statistics)
extern uint64_t renderer_present_count(void);

static size_t cc_lower(uint64_t a) {  // first live chunk starting at or after a
    size_t lo = 0, hi = g_cc_n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (g_cc[m]->start < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}
static void cc_free_push(CmdChunk *e) {  // (under g_cc_lock) into quarantine
    e->gen++;
    e->retired_at = renderer_present_count();
    if (g_q_n == g_q_cap) {
        size_t nc = g_q_cap ? g_q_cap * 2 : 1024;
        CmdChunk **nq = malloc(sizeof *nq * nc);
        for (size_t k = 0; k < g_q_n; k++) nq[k] = g_q[(g_q_head + k) % g_q_cap];
        free(g_q);
        g_q = nq, g_q_cap = nc, g_q_head = 0;
    }
    g_q[(g_q_head + g_q_n++) % g_q_cap] = e;
}
// (under g_cc_lock) the chunks that start inside [a, a + size) end: the game is writing over their lists
static uint64_t g_retire_by;  // (debug: the command buffer retiring, for LARECOMP_CHECK_LISTS)
static void cc_retire_range(uint64_t a, uint64_t size) {
    size_t i = cc_lower(a), j = i;
    extern int nvn_check_lists(void);
    while (j < g_cc_n && g_cc[j]->start < a + (size ? size : 1)) {
        static int nlog;
        if (nvn_check_lists() && nlog++ < 4000)
            LOG("[cmdchk] chunk %llx retired by cb %llx (range %llx+%llx)\n", (unsigned long long)g_cc[j]->start,
                (unsigned long long)g_retire_by, (unsigned long long)a, (unsigned long long)size);
        cc_free_push(g_cc[j++]);
    }
    if (j > i) {
        memmove(g_cc + i, g_cc + j, (g_cc_n - j) * sizeof *g_cc);
        g_cc_n -= j - i;
    }
}
static volatile uint64_t g_cc_epoch;  // (bumped by every chunk added: command buffers re-check what follows theirs)
// (under g_cc_lock) the start of the first live chunk after `a` (UINT64_MAX: none)
static uint64_t cc_next_start(uint64_t a) {
    size_t i = cc_lower(a + 1);
    return i < g_cc_n ? g_cc[i]->start : UINT64_MAX;
}
// (under g_cc_lock) a chunk for guest memory [start, start + size); keyed: findable by later retirements
static CmdChunk *cc_new(uint64_t start, uint64_t size, bool keyed) {
    if (keyed) g_cc_epoch++;
    CmdChunk *e;
    if (g_q_n && renderer_present_count() >= g_q[g_q_head]->retired_at + QUARANTINE) {
        e = g_q[g_q_head];
        g_q_head = (g_q_head + 1) % g_q_cap, g_q_n--;
    } else {
        e = calloc(1, sizeof *e);
    }
    e->start = start;
    e->size = size;
    e->orphan = !keyed;
    if (keyed) {
        if (g_cc_n == g_cc_cap) {
            g_cc_cap = g_cc_cap ? g_cc_cap * 2 : 1024;
            g_cc = realloc(g_cc, sizeof *g_cc * g_cc_cap);
        }
        size_t i = cc_lower(start);
        memmove(g_cc + i + 1, g_cc + i, (g_cc_n - i) * sizeof *g_cc);
        g_cc[i] = e;
        g_cc_n++;
    }
    return e;
}
// (under g_cc_lock) block i of a chunk, at least `want` bytes
static void *cc_block(CmdChunk *e, int i, uint32_t want) {
    while (e->n <= i) {
        if (e->n == e->cap) {
            e->cap = e->cap ? e->cap * 2 : 4;
            e->blk = realloc(e->blk, sizeof *e->blk * (size_t)e->cap);
            e->sz = realloc(e->sz, sizeof *e->sz * (size_t)e->cap);
        }
        e->blk[e->n] = NULL, e->sz[e->n] = 0;
        e->n++;
    }
    if (e->sz[i] < want) {
        g_cc_bytes += want - e->sz[i];
        free(e->blk[i]);
        e->blk[i] = malloc(want);
        e->sz[i] = want;
    }
    return e->blk[i];
}

static void cb_release_orphan(NvnCmdBuf *cb) {  // (a finalized command buffer's private chunk: into quarantine)
    pthread_mutex_lock(&g_cc_lock);
    if (cb->hc && cb->hc->orphan && cb->hc->gen == cb->hc_gen) cc_free_push(cb->hc);
    cb->hc = NULL;
    pthread_mutex_unlock(&g_cc_lock);
}

// Moves cb on to its chunk's next block (or a fresh chunk), with room for `need` bytes plus a jump. With `link`, the
// list being recorded goes on there through a jump from where it stopped (a retired chunk's blocks stay ours
// through the quarantine, so that jump is safe).
static void next_block(NvnCmdBuf *cb, uint32_t need, bool link) {
    pthread_mutex_lock(&g_cc_lock);
    if (!cb->hc || cb->hc->gen != cb->hc_gen) {  // (no memory given, or retired while still in use: a private one)
        cb->hc = cc_new(cb->chunk, cb->chunk_size, false);
        cb->hc_gen = cb->hc->gen;
        cb->blk = -1;
    }
    int i = ++cb->blk;
    // first block: about what our records of a chunk this size take (they run ~4x NVN's encoding); then doubling
    uint32_t base = cb->chunk_size ? (uint32_t)(cb->chunk_size * 6 > BLOCK_MAX ? BLOCK_MAX : cb->chunk_size * 6) : 4096;
    if (base < 1024) base = 1024;
    uint32_t want = i == 0 ? base : (base << (i < 6 ? i : 6)) > BLOCK_MAX ? BLOCK_MAX : base << i;
    if (want < need + JUMP_RESERVE) want = need + (uint32_t)JUMP_RESERVE;
    uint64_t blk = (uint64_t)(uintptr_t)cc_block(cb->hc, i, want);
    pthread_mutex_unlock(&g_cc_lock);
    if (link && cb->mem && cb->recording) raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec))->x[0] = blk;
    cb->mem = blk;
    cb->size = want;
    cb->used = 0;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = nvn_pool_addr(HOST(NvnPool, A(1)), A(2)), size = A(3);
    {
        static int log = -1, n;
        if (log < 0) log = getenv("LARECOMP_LOG_CMDMEM") ? atoi(getenv("LARECOMP_LOG_CMDMEM")) : 0;
        if (log && n < log) {
            n++;
            LOG("[cmdmem] cb %llx add %llx+%llx (pool %llx +%llx) recording %d, was %llx used ~%llu/%llu; %zu chunks, %llu KB lr %llx\n",
                (unsigned long long)cb->guest, (unsigned long long)mem, (unsigned long long)size,
                (unsigned long long)A(1), (unsigned long long)A(2), cb->recording, (unsigned long long)cb->chunk,
                (unsigned long long)cb->vused, (unsigned long long)cb->chunk_size, g_cc_n,
                (unsigned long long)(g_cc_bytes >> 10), (unsigned long long)c->x[30]);
        }
    }
    pthread_mutex_lock(&g_cc_lock);
    g_retire_by = cb->guest;
    cc_retire_range(mem, 1);  // (a chunk added again at the same place: its first command overwrites the old lists)
    if (cb->hc && cb->hc->orphan && cb->hc->gen == cb->hc_gen) cc_free_push(cb->hc);  // (moving on from a private one)
    bool link = cb->hc != NULL;  // (the block the list stopped in: ours, or in quarantine)
    CmdChunk *e = cc_new(mem, size, true);
    cb->next_start = cc_next_start(mem);
    cb->cc_epoch = g_cc_epoch;
    pthread_mutex_unlock(&g_cc_lock);
    cb->chunk = mem;
    cb->chunk_size = size;
    cb->vused = 0;
    cb->oom_called = false;
    cb->hc = e;
    cb->hc_gen = e->gen;
    cb->blk = -1;
    // (a list being recorded goes on in the new chunk; its blocks so far stay where they are)
    uint64_t mem0 = cb->mem, used0 = cb->used;
    next_block(cb, 0, false);
    if (link && mem0 && cb->recording) raw_rec(mem0 + used0, CMD_JUMP, sizeof(CmdRec))->x[0] = cb->mem;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; ONCE("AddControlMemory"); }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->chunk_size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->vused); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->chunk_size > cb->vused ? cb->chunk_size - cb->vused : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; ONCE("SetCommandMemoryCallbackEnabled"); }

// Reserve a record. When the game's chunk would be full by NVN's own measure, its out-of-memory callback runs once
// (as on the Switch: it may add memory); our records go on in host memory either way. Callers snapshot their
// arguments first (the callback is guest code).
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    uint64_t est = 16 + (uint64_t)((extra + 3) & ~3u);  // (NVN's encoding: a few method words, inline data)
    if (cb->callback && !cb->oom_called && cb->chunk_size && cb->vused + est > cb->chunk_size) {
        cb->oom_called = true;
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = est;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    if (!cb->mem || cb->used + size + JUMP_RESERVE > cb->size || (cb->hc && cb->hc->gen != cb->hc_gen)) next_block(cb, size, true);
    uint64_t v0 = cb->vused;
    cb->vused = cb->vused + est > cb->chunk_size ? cb->chunk_size : cb->vused + est;
    // what NVN would have written now reaches other chunks' starts: their lists are overwritten
    if (cb->chunk && cb->vused > v0 && (cb->chunk + cb->vused > cb->next_start || cb->cc_epoch != g_cc_epoch)) {
        pthread_mutex_lock(&g_cc_lock);
        uint64_t from = cb->chunk + (v0 ? v0 : 1);
        g_retire_by = cb->guest;
        cc_retire_range(from, cb->chunk + cb->vused - from);
        cb->next_start = cc_next_start(cb->chunk);
        cb->cc_epoch = g_cc_epoch;
        pthread_mutex_unlock(&g_cc_lock);
    }
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
// (debug) LARECOMP_CHECK_LISTS=1: each list's records are hashed at EndRecording and checked again when it is
// submitted or copied; a list changed in between (its memory reused too early) is logged with its buffer and chunk.
typedef struct { uint64_t handle, hash, cb, chunk; uint32_t n; } ListCheck;
static ListCheck g_lchk[1 << 14];
static pthread_mutex_t g_lchk_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t list_hash(uint64_t p, uint32_t *n) {
    uint64_t h = 1469598103934665603ull;
    *n = 0;
    for (int guard = 0; p && guard < 1000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || (r->size & 7)) return h ^ 0xBAD;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < r->size / 8; i++) h = (h ^ w[i]) * 0x100000001B3ull;
        ++*n;
        if (r->op == CMD_END) break;
        p += r->size;
    }
    return h;
}
int nvn_check_lists(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_CHECK_LISTS") != NULL;
    return on;
}
static void list_note(uint64_t handle, NvnCmdBuf *cb) {
    ListCheck e = {handle, 0, cb->guest, cb->chunk, 0};
    e.hash = list_hash(handle, &e.n);
    pthread_mutex_lock(&g_lchk_lock);
    g_lchk[(handle >> 3) & ((1 << 14) - 1)] = e;
    pthread_mutex_unlock(&g_lchk_lock);
}
void nvn_list_verify(uint64_t handle, const char *where) {
    pthread_mutex_lock(&g_lchk_lock);
    ListCheck e = g_lchk[(handle >> 3) & ((1 << 14) - 1)];
    pthread_mutex_unlock(&g_lchk_lock);
    if (e.handle != handle) return;
    uint32_t n;
    uint64_t h = list_hash(handle, &n);
    static int nlog;
    if (h != e.hash && nlog++ < 30)
        LOG("[cmdchk] %s: list %llx changed since recorded (cb %llx chunk %llx, %u -> %u records)\n", where,
            (unsigned long long)handle, (unsigned long long)e.cb, (unsigned long long)e.chunk, e.n, n);
}

NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    if (nvn_check_lists()) list_note(cb->start, cb);
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }


// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;  // host program (NULL: the stages unbound)
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; ONCE("BindSeparateSamplers"); }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; ONCE("BindSeparateSampler"); }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; ONCE("BindTransformFeedbackBuffers"); }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; ONCE("BindTransformFeedbackBuffer"); }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; ONCE("BindCoverageModulationTable"); }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = (uint64_t)(uintptr_t)(colors ? nvn_texture_of(colors[i], "SetRenderTargets") : NULL);
        if (views && views[i]) {
            rt.has_view[i] = 1;
            memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView));
            nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.tex[i], &rt.views[i]);
        }
    }
    rt.depth = (uint64_t)(uintptr_t)nvn_texture_of(s.x[4], "SetRenderTargets depth");
    if (s.x[5]) {
        rt.has_dview = 1;
        memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView));
        nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.depth, &rt.dview);
    }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; ONCE("SetRenderEnableConditional"); }
NVN(nvnCommandBufferSetDepthClamp) { (void)c; ONCE("SetDepthClamp"); }
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; ONCE("SetRasterizerDiscard"); }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; ONCE("SetConservativeRasterEnable"); }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; ONCE("SetConservativeRasterDilate"); }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; ONCE("SetSubpixelPrecisionBias"); }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; ONCE("SetViewportSwizzles"); }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; ONCE("SetStencilCullCriteria"); }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; ONCE("SetColorReductionEnable"); }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; ONCE("SetColorReductionThresholds"); }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; ONCE("SetTiledCacheAction"); }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; ONCE("SetTiledCacheTileSize"); }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; ONCE("SetShaderScratchMemory"); }
NVN(nvnCommandBufferSetPatchSize) { (void)c; ONCE("SetPatchSize"); }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; ONCE("SetInnerTessellationLevels"); }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; ONCE("SetOuterTessellationLevels"); }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; ONCE("SetProgramSubroutines"); }
NVN(nvnCommandBufferSaveZCullData) { (void)c; ONCE("SaveZCullData"); }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; ONCE("RestoreZCullData"); }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)nvn_texture_of(s.x[1], "ClearTexture");
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); nvn_texture_note_view((NvnTexture *)(uintptr_t)ct.tex, &ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; ONCE("DiscardDepthStencil"); }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { ONCE("MultiDrawArraysIndirectCount (not drawn)"); }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { ONCE("MultiDrawElementsIndirectCount (not drawn)"); }
NVN(nvnCommandBufferDrawTransformFeedback) { ONCE("DrawTransformFeedback"); }
NVN(nvnCommandBufferDrawTexture) { ONCE("DrawTexture"); }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
NVN(nvnCommandBufferDispatchComputeIndirect) { ONCE("DispatchComputeIndirect"); Snap s = snap(c); record(c, CMD_DISPATCH_INDIRECT, 0, &s); }
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; ONCE("BeginTransformFeedback"); }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; ONCE("EndTransformFeedback"); }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; ONCE("PauseTransformFeedback"); }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; ONCE("ResumeTransformFeedback"); }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = (uint64_t)(uintptr_t)nvn_texture_of(tex, "texture copy");
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); nvn_texture_note_view((NvnTexture *)(uintptr_t)t->tex, &t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; ONCE("TiledDownsample"); }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; ONCE("TiledDownsampleTextureView"); }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; ONCE("ResolveDepthBuffer"); }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; ONCE("ReportValue"); }
NVN(nvnCommandBufferResetCounter) { (void)c; ONCE("ResetCounter"); }
NVN(nvnCommandBufferCallCommands) {
    extern int nvn_check_lists(void);
    extern void nvn_list_verify(uint64_t handle, const char *where);
    if (nvn_check_lists() && A(2)) for (uint64_t i = 0; i < A(1); i++) nvn_list_verify(((uint64_t *)AP(2))[i], "CallCommands");
    record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8);
}
// CopyCommands(cmd, count, handles): NVN copies the lists' commands, so the game may reuse their memory right away
// (a CMD_CALL to them would then run whatever was recorded there next). Their records are copied in, jumps followed.
NVN(nvnCommandBufferCopyCommands) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t n = A(1), *src = (uint64_t *)(uintptr_t)A(2);
    if (!src || !n) return;
    uint64_t *h = malloc(n * sizeof *h);
    memcpy(h, src, n * sizeof *h);
    extern int nvn_check_lists(void);
    extern void nvn_list_verify(uint64_t handle, const char *where);
    if (nvn_check_lists()) for (uint64_t i = 0; i < n; i++) nvn_list_verify(h[i], "CopyCommands");
    for (uint64_t i = 0; i < n; i++) {
        uint64_t p = h[i];
        for (int guard = 0; p && guard < 1000000; guard++) {
            const CmdRec *r = (const CmdRec *)(uintptr_t)p;
            if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || (r->size & 7)) {
                LOG("[nvn] CopyCommands: bad record op %u size %u at %llx (list %llx)\n", r->op, r->size,
                    (unsigned long long)p, (unsigned long long)h[i]);
                break;
            }
            if (r->op == CMD_END) break;
            if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
            uint32_t size = r->size;
            CmdRec *d = rec(c, cb, r->op, size - (uint32_t)sizeof(CmdRec));
            memcpy((uint8_t *)d + 8, (const uint8_t *)r + 8, size - 8);
            p += size;
        }
    }
    free(h);
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }

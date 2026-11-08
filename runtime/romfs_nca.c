// The game's RomFS read in place from the player's NSPs (romfs_nca.h).
//
// The image is a range of a virtual storage. Without an update that's the base game's RomFS section; with one it's the
// update's Bktr patch: an "indirect" table maps each range of the virtual storage to the base section (0) or to the
// update's own section (1). Both sections are AES-128-CTR, the counter's low half the NCA offset / 16 and its high
// half the section's; the update's section changes the high half's low word ("generation") by range, from a second
// table. extracted/romfs.src holds both tables, each section's place in its NSP and its key (tools/nsp_extract.py).
#include <fcntl.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "romfs_nca.h"
#include "rt.h"

typedef struct {
    int fd;
    uint64_t file_off, nca_off, size;  // (in its NSP, in its NCA)
    uint8_t ctr_hi[8];
    __m128i rk[11];
} Section;

typedef struct { uint64_t virt, phys; int sec; } Ind;
typedef struct { uint64_t off; uint32_t gen; } Ctr;

static Section g_sec[2];
static int g_nsec;
static Ind *g_ind;
static int g_nind;
static Ctr *g_ctr;
static int g_nctr;
static uint64_t g_image_off, g_image_size, g_end, g_ctr_end;

// ---- AES-128 (AES-NI: every x86-64 CPU this runs on has it)
#define KEXP(i, rcon)                                                                     \
    do {                                                                                  \
        __m128i k = rk[i - 1], g = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k, rcon), 0xFF); \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                       \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                       \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                                       \
        rk[i] = _mm_xor_si128(k, g);                                                      \
    } while (0)

__attribute__((target("aes,sse4.1"))) static void aes_expand(__m128i rk[11], const uint8_t key[16]) {
    rk[0] = _mm_loadu_si128((const __m128i *)key);
    KEXP(1, 0x01); KEXP(2, 0x02); KEXP(3, 0x04); KEXP(4, 0x08); KEXP(5, 0x10);
    KEXP(6, 0x20); KEXP(7, 0x40); KEXP(8, 0x80); KEXP(9, 0x1B); KEXP(10, 0x36);
}

// data ^= AES-CTR keystream, blocks counted from `block` (counter = hi || big-endian block), 8 blocks at a time
__attribute__((target("aes,sse4.1"))) static void aes_ctr(const __m128i rk[11], const uint8_t hi[8], uint64_t block,
                                                          uint8_t *data, size_t nblocks) {
    uint8_t c[8][16];
    for (int j = 0; j < 8; j++) memcpy(c[j], hi, 8);
    while (nblocks) {
        size_t m = nblocks < 8 ? nblocks : 8;
        __m128i x[8];
        for (size_t j = 0; j < m; j++) {
            uint64_t be = __builtin_bswap64(block + j);
            memcpy(c[j] + 8, &be, 8);
            x[j] = _mm_xor_si128(_mm_loadu_si128((const __m128i *)c[j]), rk[0]);
        }
        for (int r = 1; r < 10; r++)
            for (size_t j = 0; j < m; j++) x[j] = _mm_aesenc_si128(x[j], rk[r]);
        for (size_t j = 0; j < m; j++) {
            __m128i *d = (__m128i *)(data + 16 * j);
            _mm_storeu_si128(d, _mm_xor_si128(_mm_loadu_si128(d), _mm_aesenclast_si128(x[j], rk[10])));
        }
        data += 16 * m, block += m, nblocks -= m;
    }
}

static int hexbytes(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

// n plaintext bytes of section s at off, with counter high half hi
#define READ_CHUNK 0x40000
static int section_read(const Section *s, const uint8_t hi[8], uint8_t *dst, uint64_t off, uint64_t n) {
    static __thread uint8_t *tmp;  // (per reading thread: guest threads' stacks are small)
    if (!tmp && !(tmp = malloc(READ_CHUNK + 32))) return -1;
    while (n) {
        uint64_t lo = off & ~0xFull, skip = off - lo;
        uint64_t k = n < READ_CHUNK - skip ? n : READ_CHUNK - skip;
        uint64_t len = (skip + k + 0xF) & ~0xFull;
        if (pread(s->fd, tmp, (size_t)len, (off_t)(s->file_off + lo)) != (ssize_t)len) return -1;
        aes_ctr(s->rk, hi, (s->nca_off + lo) >> 4, tmp, (size_t)(len >> 4));
        memcpy(dst, tmp + skip, (size_t)k);
        dst += k, off += k, n -= k;
    }
    return 0;
}

// the update's section: counter generation by range (ctr table, sorted by offset)
static int patch_read(uint8_t *dst, uint64_t off, uint64_t n) {
    const Section *s = &g_sec[1];
    while (n) {
        int lo = 0, hi = g_nctr - 1, i = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (g_ctr[mid].off <= off) i = mid, lo = mid + 1;
            else hi = mid - 1;
        }
        uint64_t end = i + 1 < g_nctr ? g_ctr[i + 1].off : g_ctr_end;
        uint64_t k = end > off && end - off < n ? end - off : n;
        uint8_t ctr[8];
        memcpy(ctr, s->ctr_hi, 4);
        uint32_t be = __builtin_bswap32(g_ctr[i].gen);
        memcpy(ctr + 4, &be, 4);
        if (section_read(s, ctr, dst, off, k)) return -1;
        dst += k, off += k, n -= k;
    }
    return 0;
}

ssize_t romfs_nca_pread(void *dst_, uint64_t n, uint64_t off) {
    uint8_t *dst = dst_;
    plat_os_write((uint64_t)(uintptr_t)dst, n);
    if (off >= g_image_size) return 0;
    if (n > g_image_size - off) n = g_image_size - off;
    uint64_t v = g_image_off + off, left = n;
    while (left) {
        int lo = 0, hi = g_nind - 1, i = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (g_ind[mid].virt <= v) i = mid, lo = mid + 1;
            else hi = mid - 1;
        }
        uint64_t end = i + 1 < g_nind ? g_ind[i + 1].virt : g_end;
        uint64_t k = end - v < left ? end - v : left, p = g_ind[i].phys + (v - g_ind[i].virt);
        int r = g_ind[i].sec == 0 ? section_read(&g_sec[0], g_sec[0].ctr_hi, dst, p, k) : patch_read(dst, p, k);
        if (r) {
            LOG("[romfs] can't read the game data at %llx in its NSP\n", (unsigned long long)p);
            return -1;
        }
        dst += k, v += k, left -= k;
    }
    return (ssize_t)n;
}

uint64_t romfs_nca_size(void) { return g_image_size; }

void romfs_nca_close(void) {
    for (int i = 0; i < g_nsec; i++)
        if (g_sec[i].fd >= 0 && (i == 0 || g_sec[i].fd != g_sec[0].fd)) close(g_sec[i].fd);
    g_nsec = 0;
    free(g_ind), g_ind = NULL, g_nind = 0;
    free(g_ctr), g_ctr = NULL, g_nctr = 0;
}

bool romfs_nca_open(const char *src, char *why, size_t whylen) {
    FILE *f = fopen(src, "r");
    if (!f) { snprintf(why, whylen, "(no %s) ", src); return false; }
    char line[1400];
    int cap_ind = 0, cap_ctr = 0, ok = 1;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        unsigned long long a, b, c, d;
        int sec, used;
        char hi[17], key[33];
        if (line[0] == '#' || !line[0]) continue;
        if (sscanf(line, "romfs %llx %llx", &a, &b) == 2) g_image_off = a, g_image_size = b;
        else if (sscanf(line, "section %d %llx %llx %llx %16s %32s %n", &sec, &a, &b, &c, hi, key, &used) == 6) {
            if (sec != g_nsec || sec > 1) { ok = 0; break; }
            Section *s = &g_sec[g_nsec++];
            uint8_t k[16];
            s->file_off = a, s->nca_off = b, s->size = c;
            if (!hexbytes(hi, s->ctr_hi, 8) || !hexbytes(key, k, 16)) { ok = 0; break; }
            aes_expand(s->rk, k);
            s->fd = open(line + used, O_RDONLY);
            if (s->fd < 0) { snprintf(why, whylen, "(can't open %s) ", line + used); fclose(f); romfs_nca_close(); return false; }
        } else if (sscanf(line, "end %llx", &a) == 1) g_end = a;
        else if (sscanf(line, "ctrend %llx", &a) == 1) g_ctr_end = a;
        else if (sscanf(line, "ind %llx %llx %llu", &a, &b, &d) == 3) {
            if (g_nind == cap_ind) g_ind = realloc(g_ind, sizeof *g_ind * (cap_ind = cap_ind ? cap_ind * 2 : 4096));
            g_ind[g_nind++] = (Ind){a, b, (int)d};
        } else if (sscanf(line, "ctr %llx %llu", &a, &d) == 2) {
            if (g_nctr == cap_ctr) g_ctr = realloc(g_ctr, sizeof *g_ctr * (cap_ctr = cap_ctr ? cap_ctr * 2 : 4096));
            g_ctr[g_nctr++] = (Ctr){a, (uint32_t)d};
        } else ok = 0;
    }
    fclose(f);
    for (int i = 0; ok && i < g_nind; i++) ok = g_ind[i].sec < g_nsec && (i == 0 || g_ind[i].virt > g_ind[i - 1].virt);
    if (ok && g_nsec == 2) ok = g_nctr > 0;
    if (!ok || !g_nsec || !g_nind || !g_image_size) {
        snprintf(why, whylen, "(%s is damaged: extract the game again) ", src);
        romfs_nca_close();
        return false;
    }
    // the image's header must be a RomFS header (0x50 bytes long): else the key or the tables are wrong
    uint64_t hdr = 0;
    if (romfs_nca_pread(&hdr, 8, 0) != 8 || hdr != 0x50) {
        snprintf(why, whylen, "(%s doesn't match your NSPs: extract the game again) ", src);
        romfs_nca_close();
        return false;
    }
    LOG("[romfs] the game data read in place from %s%s (%.2f GB, %d remapped ranges)\n", g_nsec == 2 ? "the update over " : "",
        "the base game's NSP", g_image_size / 1e9, g_nind);
    return true;
}

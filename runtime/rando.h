// Randomizer seeds: LAS Recomp's feature, not part of MUA3 Recomp. These keep the shared code's "no seed" path.
#pragma once
#include <stdbool.h>
#include <stddef.h>

static inline const char *rando_seed_dir(void) { return NULL; }
static inline bool rando_romfs_override(const char *path, char *host, size_t size) {
    (void)path, (void)host, (void)size;
    return false;
}
static inline void rando_boot(void) {}

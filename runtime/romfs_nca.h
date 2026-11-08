// The game's RomFS read in place from the player's NSPs (romfs_nca.c), as tools/nsp_extract.py describes it in
// extracted/romfs.src: no 18 GB copy of the game data on disk.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

bool romfs_nca_open(const char *src, char *why, size_t whylen);  // (false: why says what's wrong)
uint64_t romfs_nca_size(void);                                    // the RomFS image's size
ssize_t romfs_nca_pread(void *dst, uint64_t n, uint64_t off);     // n bytes of the image at off (thread-safe)
void romfs_nca_close(void);

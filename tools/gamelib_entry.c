// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

#define MODULE(n) extern const uint64_t n##_fn_addrs[], n##_fn_count; extern const GuestFn n##_fn_ptrs[];
MODULE(game)
MODULE(sdk)
MODULE(glslc)
#undef MODULE
#define MODULE(n) {#n, n##_fn_addrs, n##_fn_ptrs, &n##_fn_count}

GAMELIB_EXPORT const GameLibTables *mua3game_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, 3, {MODULE(game), MODULE(sdk), MODULE(glslc)}};
    return &t;
}

// The in-game menu's game-specific tabs and the overlays drawn over the game (runtime/ui/menu.mm calls these). LAS
// Recomp's were Link's Awakening's (randomizer, warp, cheats, save editor, tracker, ...); DOAX3 has none of its own yet.
#include <stddef.h>

#include "imgui.h"

extern "C" {
// ---- menu tabs
void save_editor_draw(float scale) { (void)scale; ImGui::TextDisabled("Not available yet."); }
void save_editor_on_open(void) {}
void rando_tab_draw(float scale) { (void)scale; ImGui::TextDisabled("Not available yet."); }
bool rando_tab_took_back(void) { return false; }
bool warp_tab_draw(float scale) { (void)scale; ImGui::TextDisabled("Not available yet."); return false; }
void cheats_draw(float scale) { (void)scale; ImGui::TextDisabled("Not available yet."); }
void difficulty_draw(float scale) { (void)scale; }
bool cosmetics_seed_on(void) { return false; }

// ---- overlays
bool chaos_overlay_wanted(void) { return false; }
void chaos_overlay_draw(ImVec2 display, ImFont *font) { (void)display, (void)font; }
bool inventory_overlay_wanted(void) { return false; }
void inventory_overlay_draw(ImVec2 display, ImFont *font) { (void)display, (void)font; }
bool pickup_flyin_wanted(void) { return false; }
void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale) { (void)display, (void)font, (void)scale; }
bool race_overlay_wanted(void) { return false; }
bool race_overlay_key(char *out, size_t cap) { (void)out, (void)cap; return false; }
void race_overlay_draw(ImVec2 display, ImFont *font, float scale) { (void)display, (void)font, (void)scale; }
bool tracker_overlay_wanted(void) { return false; }
void tracker_overlay_draw(ImVec2 display, ImFont *font) { (void)display, (void)font; }
bool tracker_dungeon_wanted(void) { return false; }
void tracker_dungeon_draw(ImVec2 display, ImFont *font) { (void)display, (void)font; }
}

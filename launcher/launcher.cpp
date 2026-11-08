// MUA3 Recomp launcher: the display and frame rate settings, then the game.
//
// It edits only its own keys in larecomp.ini (the game's settings file, in the data folder: the folder with extracted/,
// next to the launcher or one up from it) and leaves every other line as it is; Play saves them and starts
// mua3recomp.exe from the data folder, as tools/run.sh does. The game reads the same keys (runtime/settings.c):
//   fullscreen, exclusive_fullscreen    windowed / borderless fullscreen / exclusive fullscreen
//   window_width, window_height         windowed: the window's size; exclusive: the display mode (0: the desktop's)
//   resolution, upscaler, antialias, aspect, show_fps, frame_rate (30 / 60 / 120)
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ---- paths
static std::string g_exe_dir, g_root;  // (the launcher's folder; the data folder)

static bool is_dir(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}
static bool is_file(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

static void find_paths(void) {
    const char *base = SDL_GetBasePath();  // (ends with a separator)
    g_exe_dir = base ? base : "./";
    std::string up = g_exe_dir + "..\\";
    g_root = is_dir(g_exe_dir + "extracted") ? g_exe_dir : is_dir(up + "extracted") ? up : g_exe_dir;
#ifdef _WIN32
    char full[MAX_PATH];  // (build\..\ -> the folder itself)
    if (GetFullPathNameA(g_root.c_str(), sizeof full, full, NULL)) g_root = full;
    if (!g_root.empty() && g_root.back() != '\\') g_root += '\\';
#endif
}

// ---- larecomp.ini: its lines kept; get / set by key
static std::vector<std::string> g_ini;

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static void ini_load(void) {
    g_ini.clear();
    FILE *f = fopen((g_root + "larecomp.ini").c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        g_ini.push_back(l);
    }
    fclose(f);
}
static bool ini_key_of(const std::string &line, std::string *key, std::string *val) {
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) return false;
    *key = trim(line.substr(0, eq));
    *val = trim(line.substr(eq + 1));
    return true;
}
static std::string ini_get(const char *key, const char *def) {
    std::string k, v;
    for (auto &l : g_ini) if (ini_key_of(l, &k, &v) && k == key) return v;
    return def;
}
static void ini_set(const char *key, const std::string &val) {
    std::string k, v;
    for (auto &l : g_ini)
        if (ini_key_of(l, &k, &v) && k == key) { l = std::string(key) + " = " + val; return; }
    g_ini.push_back(std::string(key) + " = " + val);
}
static bool ini_save(void) {
    FILE *f = fopen((g_root + "larecomp.ini").c_str(), "w");
    if (!f) return false;
    if (g_ini.empty() || g_ini[0].rfind("#", 0) != 0) fprintf(f, "# MUA3 Recomp settings (the launcher and the in-game menu)\n");
    for (auto &l : g_ini) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    return true;
}

// ---- the choices
struct Choice { const char *label, *value; };
static const Choice k_render[] = {{"Auto (this GPU's default)", "auto"}, {"Match display", "match"}, {"1x (1080p)", "1x"},
                                  {"2x (4K)", "2x"}, {"3x", "3x"}, {"4x", "4x"}, {"Dynamic", "dynamic"},
                                  {"75% of the display", "75%"}, {"50% of the display", "50%"}, {"25% of the display", "25%"}};
static const Choice k_upscaler[] = {{"Auto", "auto"}, {"AMD FSR 1 (sharp)", "fsr1"}, {"Bilinear", "bilinear"}};
static const Choice k_aa[] = {{"Off", "off"}, {"SMAA", "smaa"}};
static const Choice k_aspect[] = {{"Unlocked (fill the window)", "unlocked"}, {"Original 16:9", "original"}};
static int choice_index(const Choice *c, int n, const std::string &v) {
    for (int i = 0; i < n; i++) if (v == c[i].value) return i;
    return 0;
}
static bool choice_combo(const char *label, int *i, const Choice *c, int n) {
    bool changed = false;
    if (ImGui::BeginCombo(label, c[*i].label)) {
        for (int k = 0; k < n; k++)
            if (ImGui::Selectable(c[k].label, k == *i)) *i = k, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

struct Mode { int w, h; float hz; };
static std::vector<Mode> g_modes;  // (the primary display's sizes, largest first, each at its highest refresh rate)
static Mode g_desktop = {1920, 1080, 60};

static void list_modes(void) {
    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(d)) g_desktop = {m->w, m->h, m->refresh_rate};
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
    for (int i = 0; modes && i < n; i++) {
        auto it = std::find_if(g_modes.begin(), g_modes.end(), [&](const Mode &x) { return x.w == modes[i]->w && x.h == modes[i]->h; });
        if (it == g_modes.end()) g_modes.push_back({modes[i]->w, modes[i]->h, modes[i]->refresh_rate});
        else it->hz = std::max(it->hz, modes[i]->refresh_rate);
    }
    SDL_free(modes);
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode &a, const Mode &b) { return a.w * a.h > b.w * b.h; });
    if (g_modes.empty()) g_modes.push_back(g_desktop);
}

// ---- the settings being edited
static int s_mode;            // 0 windowed, 1 borderless, 2 exclusive
static int s_size;            // index into g_modes, -1: default (windowed 1280x720 / the desktop's)
static int s_render, s_upscaler, s_aa, s_aspect, s_fps;
static bool s_show_fps;

static void settings_from_ini(void) {
    bool fs = atoi(ini_get("fullscreen", "0").c_str()) != 0, ex = atoi(ini_get("exclusive_fullscreen", "0").c_str()) != 0;
    s_mode = !fs ? 0 : ex ? 2 : 1;
    int w = atoi(ini_get("window_width", "0").c_str()), h = atoi(ini_get("window_height", "0").c_str());
    s_size = -1;
    for (size_t i = 0; i < g_modes.size(); i++) if (g_modes[i].w == w && g_modes[i].h == h) s_size = (int)i;
    s_render = choice_index(k_render, IM_ARRAYSIZE(k_render), ini_get("resolution", "auto"));
    s_upscaler = choice_index(k_upscaler, IM_ARRAYSIZE(k_upscaler), ini_get("upscaler", "auto"));
    s_aa = choice_index(k_aa, IM_ARRAYSIZE(k_aa), ini_get("antialias", "off"));
    s_aspect = choice_index(k_aspect, IM_ARRAYSIZE(k_aspect), ini_get("aspect", "unlocked"));
    int fps = atoi(ini_get("frame_rate", atoi(ini_get("fps60", "0").c_str()) ? "60" : "30").c_str());
    s_fps = fps >= 120 ? 2 : fps >= 60 ? 1 : 0;
    s_show_fps = atoi(ini_get("show_fps", "0").c_str()) != 0;
}

static bool settings_to_ini(void) {
    ini_set("fullscreen", s_mode ? "1" : "0");
    ini_set("exclusive_fullscreen", s_mode == 2 ? "1" : "0");
    ini_set("window_width", s_size >= 0 ? std::to_string(g_modes[s_size].w) : "0");
    ini_set("window_height", s_size >= 0 ? std::to_string(g_modes[s_size].h) : "0");
    ini_set("resolution", k_render[s_render].value);
    ini_set("upscaler", k_upscaler[s_upscaler].value);
    ini_set("antialias", k_aa[s_aa].value);
    ini_set("aspect", k_aspect[s_aspect].value);
    ini_set("frame_rate", s_fps == 2 ? "120" : s_fps == 1 ? "60" : "30");
    ini_set("show_fps", s_show_fps ? "1" : "0");
    return ini_save();
}

// ---- the game
static std::string launch_game(void) {
    std::string exe = g_exe_dir + "mua3recomp.exe";
    if (!is_file(exe)) exe = g_root + "build\\mua3recomp.exe";
    if (!is_file(exe)) return "mua3recomp.exe isn't next to the launcher.";
    if (!is_dir(g_root + "extracted")) return "No extracted\\ folder with the game's files next to the launcher (or one folder up).";
#ifdef _WIN32
    SetEnvironmentVariableA("LARECOMP_DATA_ROOT", g_root.c_str());  // (as tools/run.sh: the data folder)
    std::string cmd = "\"" + exe + "\" extracted";
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmd.data(), NULL, NULL, FALSE, 0, NULL, g_root.c_str(), &si, &pi))
        return "Couldn't start the game (Windows error " + std::to_string(GetLastError()) + ").";
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return "";
#else
    return "Starting the game is only implemented on Windows.";
#endif
}

// ---- UI
static void section(const char *title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.15f, 1));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}
static void hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.68f, 0.75f, 1));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void style(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0, s.FrameRounding = 5, s.GrabRounding = 5, s.PopupRounding = 5;
    s.FramePadding = ImVec2(10, 6), s.ItemSpacing = ImVec2(10, 9);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.22f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.23f, 0.32f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.19f, 0.27f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.28f, 0.40f, 1);
    c[ImGuiCol_Header] = ImVec4(0.55f, 0.12f, 0.12f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.16f, 0.16f, 0.9f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.78f, 0.15f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.15f, 1);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    find_paths();
    list_modes();
    ini_load();
    settings_from_ini();

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window *win = SDL_CreateWindow("Marvel Ultimate Alliance 3 Recomp - Launcher", (int)(640 * scale), (int)(740 * scale),
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) return 1;
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture *art = NULL;
    for (const std::string &p : {g_exe_dir + "icon.png", g_root + "build\\icon.png"}) {
        if (art || !is_file(p)) continue;
        if (SDL_Surface *s = SDL_LoadPNG(p.c_str())) {
            SDL_SetWindowIcon(win, s);
            art = SDL_CreateTextureFromSurface(ren, s);
            SDL_DestroySurface(s);
        }
    }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    for (const std::string &p : {g_exe_dir + "DejaVuSans.ttf", g_root + "third_party\\fonts\\DejaVu\\DejaVuSans.ttf",
                                 std::string("C:\\Windows\\Fonts\\segoeui.ttf")})
        if (is_file(p) && io.Fonts->AddFontFromFileTTF(p.c_str(), 19 * scale)) break;
    style();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    std::string status;
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        // header: the game's icon and title
        float icon = 96 * scale;
        if (art) { ImGui::Image((ImTextureID)(intptr_t)art, ImVec2(icon, icon)); ImGui::SameLine(); }
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, 14 * scale));
        ImGui::SetWindowFontScale(1.35f);
        ImGui::TextUnformatted("Marvel Ultimate Alliance 3");
        ImGui::SetWindowFontScale(1.0f);
        hint("PC recompilation - settings and launch");
        ImGui::EndGroup();

        float label_w = 190 * scale;
        ImGui::PushItemWidth(-1);
        section("DISPLAY");
        const char *modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Window mode"); ImGui::SameLine(label_w);
        ImGui::Combo("##mode", &s_mode, modes, 3);

        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Resolution"); ImGui::SameLine(label_w);
        if (s_mode == 1) {
            ImGui::BeginDisabled();
            char d[64];
            snprintf(d, sizeof d, "Desktop (%dx%d)", g_desktop.w, g_desktop.h);
            if (ImGui::BeginCombo("##size_b", d)) ImGui::EndCombo();
            ImGui::EndDisabled();
        } else {
            char cur[64];
            if (s_size >= 0) snprintf(cur, sizeof cur, s_mode == 2 ? "%dx%d (%.0f Hz)" : "%dx%d", g_modes[s_size].w, g_modes[s_size].h, g_modes[s_size].hz);
            else snprintf(cur, sizeof cur, s_mode == 2 ? "Desktop (%dx%d)" : "Default (1280x720)", g_desktop.w, g_desktop.h);
            if (ImGui::BeginCombo("##size", cur)) {
                char d[64];
                snprintf(d, sizeof d, s_mode == 2 ? "Desktop (%dx%d)" : "Default (1280x720)", g_desktop.w, g_desktop.h);
                if (ImGui::Selectable(d, s_size < 0)) s_size = -1;
                for (size_t i = 0; i < g_modes.size(); i++) {
                    char m[64];
                    snprintf(m, sizeof m, s_mode == 2 ? "%dx%d (%.0f Hz)" : "%dx%d", g_modes[i].w, g_modes[i].h, g_modes[i].hz);
                    if (ImGui::Selectable(m, s_size == (int)i)) s_size = (int)i;
                }
                ImGui::EndCombo();
            }
        }
        hint(s_mode == 0 ? "The window's size (it can be resized while playing; F11 or Alt+Enter switches to fullscreen)."
             : s_mode == 1 ? "Fills the screen at the desktop's resolution and refresh rate."
                           : "Switches the display to this resolution at its highest refresh rate.");

        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Render resolution"); ImGui::SameLine(label_w);
        choice_combo("##render", &s_render, k_render, IM_ARRAYSIZE(k_render));
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Upscaler"); ImGui::SameLine(label_w);
        choice_combo("##upscaler", &s_upscaler, k_upscaler, IM_ARRAYSIZE(k_upscaler));
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Anti-aliasing"); ImGui::SameLine(label_w);
        choice_combo("##aa", &s_aa, k_aa, IM_ARRAYSIZE(k_aa));
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Aspect ratio"); ImGui::SameLine(label_w);
        choice_combo("##aspect", &s_aspect, k_aspect, IM_ARRAYSIZE(k_aspect));

        section("FRAME RATE");
        const char *rates[] = {"30 fps", "60 fps", "120 fps"};
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(rates[i], s_fps == i)) s_fps = i;
        }
        hint(s_fps == 0 ? "The original frame rate."
             : s_fps == 1 ? "The game keeps time by the clock, so it plays at its normal speed at 60."
                          : "Normal game speed at 120 too. Needs a fast CPU and GPU and a 120 Hz (or faster) display; "
                            "where the PC can't keep up it runs between 60 and 120.");
        if (s_fps == 2 && g_desktop.hz > 0 && g_desktop.hz < 119 && s_mode != 2) {
            char w[128];
            snprintf(w, sizeof w, "Your display runs at %.0f Hz: frames above that aren't shown.", g_desktop.hz);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1));
            ImGui::TextUnformatted(w);
            ImGui::PopStyleColor();
        }
        ImGui::Checkbox("Show the frame rate in the corner", &s_show_fps);
        ImGui::PopItemWidth();

        // buttons, at the bottom
        float bh = 46 * scale, bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bh - ImGui::GetTextLineHeightWithSpacing() - 2 * ImGui::GetStyle().ItemSpacing.y -
                             ImGui::GetStyle().WindowPadding.y - 6 * scale);
        if (!status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1));
            ImGui::TextUnformatted(status.c_str());
            ImGui::PopStyleColor();
        } else {
            hint(("Settings: " + g_root + "larecomp.ini").c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.10f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.16f, 0.16f, 1));
        static int autoplay = getenv("MUA3_LAUNCHER_PLAY") ? 5 : -1;  // (test: Play after a few frames)
        if (ImGui::Button("PLAY", ImVec2(bw, bh)) || (autoplay > 0 && --autoplay == 0)) {
            status = settings_to_ini() ? launch_game() : "Couldn't write larecomp.ini.";
            if (status.empty()) quit = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(bw, bh))) status = settings_to_ini() ? "Saved." : "Couldn't write larecomp.ini.";
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(bw, bh))) quit = true;

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 18, 20, 28, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        {   // (test) MUA3_LAUNCHER_SHOT=<file.bmp>: the window's look saved after a few frames, then quit
            static int frames;
            const char *shot = getenv("MUA3_LAUNCHER_SHOT");
            if (shot && ++frames == 5) {
                if (SDL_Surface *s = SDL_RenderReadPixels(ren, NULL)) SDL_SaveBMP(s, shot), SDL_DestroySurface(s);
                quit = true;
            }
        }
        SDL_RenderPresent(ren);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (art) SDL_DestroyTexture(art);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

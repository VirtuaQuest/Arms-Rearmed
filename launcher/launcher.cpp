// ARMS Rearmed launcher: picks the game files (base game, update, keys), runs the extraction, recompilation and build,
// edits the display / audio settings, shows the controls, and starts the game.
//
// Look: an original bright, sporty style (yellow and black with neon accents, slanted banners, springs and halftone
// dots, all drawn here) - no game art, fonts or logos. Text is Baloo 2 (SIL Open Font License, third_party/fonts).
//
// Files (in the data folder: the repository root, the folder with the Makefile - next to the launcher or one up):
//   larecomp.ini   the game's settings; only the launcher's keys are edited, every other line is kept as it is
//                  (the game reads the same keys: runtime/settings.c)
//   launcher.ini   the launcher's own: the game file paths, the MSYS2 folder
// Steps run as child processes (their output in the log panel; a job object ends them all on Cancel):
//   Python environment  py -3.12 -m venv .venv; pip install the recompiler's modules (README)
//   Extract             .venv\Scripts\python -I tools\nsp_extract.py <base> <keys> extracted [--update <update>]
//   Recompile           make gen                (MSYS2 UCRT64: clang, make)
//   Build               make -jN build/armsrecomp.exe build/armsgame.dll
// In the release folder (make dist: armsgen, zig, gamelib-src next to the launcher) the same steps need nothing
// installed: armsgen extract / armsgen gen, then the game code compiled by the bundled zig (build_gamelib), all in
// the launcher's own folder. Command lines (RecompLauncher, recomplauncher.json):
//   --prepare --game=<xci|nsp> --update=<nsp> --keys=<prod.keys>   no window: INSTALL, "progress" lines on stdout
//              (--base= is the same as --game=; each also as two arguments: --keys <prod.keys>)
//   --play     the game, waited for; the window only opens when the game code must be built again first
// Tests: ARMS_LAUNCHER_SHOT=<file.bmp> saves the window after a few frames and quits; ARMS_LAUNCHER_TAB=<n> opens
// that page first; ARMS_LAUNCHER_PLAY=1 presses Play.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ================================================================== palette
static const ImU32 C_YELLOW = IM_COL32(255, 225, 10, 255), C_YELLOW2 = IM_COL32(255, 205, 0, 255);
static const ImU32 C_BLACK = IM_COL32(18, 18, 22, 255), C_INK = IM_COL32(28, 28, 36, 240);
static const ImU32 C_PINK = IM_COL32(255, 45, 120, 255), C_CYAN = IM_COL32(25, 211, 255, 255);
static const ImU32 C_LIME = IM_COL32(166, 255, 46, 255), C_ORANGE = IM_COL32(255, 138, 0, 255);
static const ImU32 C_WHITE = IM_COL32(255, 255, 255, 255), C_GREY = IM_COL32(150, 152, 165, 255);
static ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// ================================================================== paths
static std::string g_exe_dir, g_root;  // (the launcher's folder; the data folder, both ending in a separator)

static bool is_dir(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}
static bool is_file(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}
static std::string full_path(const std::string &p) {
#ifdef _WIN32
    char full[MAX_PATH];
    if (GetFullPathNameA(p.c_str(), sizeof full, full, NULL)) return full;
#endif
    return p;
}
static void find_paths(void) {
    const char *base = SDL_GetBasePath();
    g_exe_dir = base ? base : ".\\";
    std::string up = g_exe_dir + "..\\";
    if (is_file(g_exe_dir + "Makefile") || is_dir(g_exe_dir + "extracted")) g_root = g_exe_dir;
    else if (is_file(up + "Makefile") || is_dir(up + "extracted")) g_root = up;
    else g_root = g_exe_dir;
    g_root = full_path(g_root);
    if (!g_root.empty() && g_root.back() != '\\' && g_root.back() != '/') g_root += '\\';
}

// The release folder (make dist): the extractor / generator (armsgen), the compiler (zig) and what the game code is
// compiled against (gamelib-src) next to the launcher, so INSTALL needs no MSYS2 or Python. gamelib-src/gamelib.id:
// the id the game (armsrecomp.exe) wants its game code library built with (GAMELIB_ID), then the C flags.
static bool g_release;
static std::string g_gamelib_id, g_gen_flags;
static void find_release(void) {
    g_release = is_file(g_exe_dir + "armsgen\\armsgen.exe") && is_file(g_exe_dir + "zig\\zig.exe") &&
                is_file(g_exe_dir + "gamelib-src\\gamelib.id");
    if (!g_release) return;
    if (FILE *f = fopen((g_exe_dir + "gamelib-src\\gamelib.id").c_str(), "r")) {
        char a[256] = {0}, b[1024] = {0};
        if (fgets(a, sizeof a, f)) g_gamelib_id = a;
        if (fgets(b, sizeof b, f)) g_gen_flags = b;
        fclose(f);
        for (std::string *s : {&g_gamelib_id, &g_gen_flags})
            while (!s->empty() && (s->back() == '\n' || s->back() == '\r' || s->back() == ' ')) s->pop_back();
    }
    if (g_gamelib_id.empty()) g_release = false;
}
// The id a game code library built from these game files has (runtime/gamelib.h gamelib_full_id): GAMELIB_ID-<the
// first 8 bytes of exefs/main's build ID, in hex>. "" when exefs/main is unreadable.
static std::string gamelib_full_id(void) {
    FILE *f = fopen((g_root + "extracted\\exefs\\main").c_str(), "rb");
    if (!f) return "";
    unsigned char b[8];
    bool ok = fseek(f, 64, SEEK_SET) == 0 && fread(b, 1, 8, f) == 8;
    fclose(f);
    if (!ok) return "";
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02X", b[i]);
    return g_gamelib_id + "-" + hex;
}
static bool gamelib_current(void) {  // armsgame.dll is there and was built for these game files and this release
    std::string want = gamelib_full_id(), have;
    if (want.empty() || !is_file(g_root + "armsgame.dll")) return false;
    if (FILE *f = fopen((g_root + "armsgame.dll.id").c_str(), "r")) {
        char id[256] = {0};
        if (fgets(id, sizeof id, f)) have = id;
        fclose(f);
    }
    while (!have.empty() && (have.back() == '\n' || have.back() == '\r')) have.pop_back();
    return have == want;
}

// ================================================================== ini files (lines kept; get / set by key)
struct Ini {
    std::string path;
    std::vector<std::string> lines;
    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    static bool split(const std::string &line, std::string *k, std::string *v) {
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) return false;
        *k = trim(line.substr(0, eq)), *v = trim(line.substr(eq + 1));
        return true;
    }
    void load(const std::string &p) {
        path = p;
        lines.clear();
        FILE *f = fopen(p.c_str(), "r");
        if (!f) return;
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    std::string get(const char *key, const char *def) const {
        std::string k, v;
        for (auto &l : lines) if (split(l, &k, &v) && k == key) return v;
        return def;
    }
    void set(const char *key, const std::string &val) {
        std::string k, v;
        for (auto &l : lines) if (split(l, &k, &v) && k == key) { l = std::string(key) + " = " + val; return; }
        lines.push_back(std::string(key) + " = " + val);
    }
    int remove_prefix(const char *prefix) {
        std::string k, v;
        size_t n = lines.size();
        lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string &l) {
            return split(l, &k, &v) && k.rfind(prefix, 0) == 0;
        }), lines.end());
        return (int)(n - lines.size());
    }
    bool save(const char *header) const {
        FILE *f = fopen(path.c_str(), "w");
        if (!f) return false;
        if (lines.empty() || lines[0].rfind("#", 0) != 0) fprintf(f, "# %s\n", header);
        for (auto &l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
        return true;
    }
};
static Ini g_settings_ini, g_launcher_ini;

// ================================================================== the settings being edited (larecomp.ini)
struct Choice { const char *label, *value; };
static const Choice k_render[] = {{"Match display", "match"}, {"1x (1080p)", "1x"}, {"2x (4K)", "2x"}, {"3x", "3x"},
                                  {"4x", "4x"}, {"Dynamic (holds 60 fps)", "dynamic"}, {"75% of the display", "75%"},
                                  {"50% of the display", "50%"}, {"Auto (this GPU's default)", "auto"}};
static const Choice k_upscaler[] = {{"Auto", "auto"}, {"AMD FSR 1 (sharp)", "fsr1"}, {"Bilinear (soft)", "bilinear"}};
static const Choice k_aa[] = {{"Off", "off"}, {"SMAA", "smaa"}};
static const Choice k_aspect[] = {{"Original 16:9 (bars on wider screens)", "original"}, {"Stretch to the window", "unlocked"}};
static int choice_index(const Choice *c, int n, const std::string &v) {
    for (int i = 0; i < n; i++) if (v == c[i].value) return i;
    return 0;
}

struct Mode { int w, h; float hz; };
static std::vector<Mode> g_modes;
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

static int s_mode, s_size, s_render, s_upscaler, s_aa, s_aspect;
static bool s_show_fps, s_rumble, s_bg_input;
static float s_volume = 1.0f;

static void settings_from_ini(void) {
    const Ini &I = g_settings_ini;
    bool fs = atoi(I.get("fullscreen", "0").c_str()) != 0, ex = atoi(I.get("exclusive_fullscreen", "0").c_str()) != 0;
    s_mode = !fs ? 0 : ex ? 2 : 1;
    int w = atoi(I.get("window_width", "0").c_str()), h = atoi(I.get("window_height", "0").c_str());
    s_size = -1;
    for (size_t i = 0; i < g_modes.size(); i++) if (g_modes[i].w == w && g_modes[i].h == h) s_size = (int)i;
    s_render = choice_index(k_render, IM_ARRAYSIZE(k_render), I.get("resolution", "match"));
    s_upscaler = choice_index(k_upscaler, IM_ARRAYSIZE(k_upscaler), I.get("upscaler", "auto"));
    s_aa = choice_index(k_aa, IM_ARRAYSIZE(k_aa), I.get("antialias", "off"));
    s_aspect = choice_index(k_aspect, IM_ARRAYSIZE(k_aspect), I.get("aspect", "original"));
    s_show_fps = atoi(I.get("show_fps", "0").c_str()) != 0;
    s_rumble = atoi(I.get("rumble", "1").c_str()) != 0;
    s_bg_input = atoi(I.get("background_input", "0").c_str()) != 0;
    s_volume = (float)atof(I.get("volume", "1.0").c_str());
}
static bool settings_to_ini(void) {
    Ini &I = g_settings_ini;
    I.set("fullscreen", s_mode ? "1" : "0");
    I.set("exclusive_fullscreen", s_mode == 2 ? "1" : "0");
    I.set("window_width", s_size >= 0 ? std::to_string(g_modes[s_size].w) : "0");
    I.set("window_height", s_size >= 0 ? std::to_string(g_modes[s_size].h) : "0");
    I.set("resolution", k_render[s_render].value);
    I.set("upscaler", k_upscaler[s_upscaler].value);
    I.set("antialias", k_aa[s_aa].value);
    I.set("aspect", k_aspect[s_aspect].value);
    I.set("show_fps", s_show_fps ? "1" : "0");
    I.set("rumble", s_rumble ? "1" : "0");
    I.set("background_input", s_bg_input ? "1" : "0");
    char v[32];
    snprintf(v, sizeof v, "%.2f", s_volume);
    I.set("volume", v);
    return I.save("ARMS Rearmed settings (the launcher and the in-game menu)");
}

// ================================================================== game files and toolchain
static char p_base[1024], p_update[1024], p_keys[1024], p_msys[512] = "C:\\msys64";
static std::mutex g_dialog_mu;
static std::string g_dialog_result;
static int g_dialog_target = -1;  // (which field the open file dialog fills)

static void paths_from_ini(void) {
    snprintf(p_base, sizeof p_base, "%s", g_launcher_ini.get("base_game", "").c_str());
    snprintf(p_update, sizeof p_update, "%s", g_launcher_ini.get("update", "").c_str());
    snprintf(p_keys, sizeof p_keys, "%s", g_launcher_ini.get("keys", "").c_str());
    snprintf(p_msys, sizeof p_msys, "%s", g_launcher_ini.get("msys2", "C:\\msys64").c_str());
}
static void paths_to_ini(void) {
    g_launcher_ini.set("base_game", p_base);
    g_launcher_ini.set("update", p_update);
    g_launcher_ini.set("keys", p_keys);
    g_launcher_ini.set("msys2", p_msys);
    g_launcher_ini.save("ARMS Rearmed launcher: your game files (never shared) and tools");
}

static void SDLCALL dialog_done(void *userdata, const char *const *files, int filter) {
    (void)filter;
    std::lock_guard<std::mutex> lk(g_dialog_mu);
    if (files && files[0]) { g_dialog_result = files[0]; g_dialog_target = (int)(intptr_t)userdata; }
}
static void browse(int target, SDL_Window *win) {
    static const SDL_DialogFileFilter game[] = {{"Switch game (XCI, NSP)", "xci;nsp"}, {"All files", "*"}};
    static const SDL_DialogFileFilter upd[] = {{"Switch update (NSP)", "nsp"}, {"All files", "*"}};
    static const SDL_DialogFileFilter keys[] = {{"Switch keys (prod.keys)", "keys;txt"}, {"All files", "*"}};
    const SDL_DialogFileFilter *f = target == 0 ? game : target == 1 ? upd : keys;
    SDL_ShowOpenFileDialog(dialog_done, (void *)(intptr_t)target, win, f, 2, NULL, false);
}

struct Status { bool venv, msys, extracted, generated, built; std::string version; };
static Status g_status;
static std::string nacp_version(void) {  // (control.nacp: DisplayVersion at 0x3060, 16 bytes)
    FILE *f = fopen((g_root + "extracted\\control\\control.nacp").c_str(), "rb");
    if (!f) return "";
    char v[17] = {0};
    if (fseek(f, 0x3060, SEEK_SET) == 0) (void)!fread(v, 1, 16, f);
    fclose(f);
    return v;
}
static void refresh_status(void) {
    g_status.extracted = is_file(g_root + "extracted\\exefs\\main") && is_file(g_root + "extracted\\romfs.src");
    g_status.version = nacp_version();
    if (g_release) {  // (the tools are in the release; the generated C isn't kept once it's built)
        g_status.venv = g_status.msys = true;
        g_status.built = is_file(g_exe_dir + "armsrecomp.exe") && gamelib_current();
        g_status.generated = g_status.built;
        return;
    }
    g_status.venv = is_file(g_root + ".venv\\Scripts\\python.exe");
    g_status.msys = is_file(std::string(p_msys) + "\\usr\\bin\\make.exe") && is_file(std::string(p_msys) + "\\ucrt64\\bin\\clang.exe");
    g_status.generated = is_dir(g_root + "generated\\game");
    g_status.built = is_file(g_root + "build\\armsrecomp.exe") && is_file(g_root + "build\\armsgame.dll");
}

// ================================================================== the step runner (child processes, a log)
enum { ST_WAIT, ST_RUN, ST_OK, ST_FAIL, ST_SKIP };
enum { K_PROCESS, K_BUILD_GAMELIB };  // (a command line; the release's own compile + link of the game code)
struct Step {
    std::string title, cmd, cwd;
    bool msys;
    bool game = false;
    int kind = K_PROCESS;
    double weight = 1;  // (its share of the whole run, for the progress the prepare step prints)
    int state = ST_WAIT;
    double secs = 0;
    std::string why;  // (when it failed: its last line of output, which says why)
};
static std::vector<Step> g_steps;
static std::atomic<double> g_step_frac{0};
static size_t g_step_index;
static std::mutex g_log_mu;
static std::vector<std::string> g_log;
static std::atomic<bool> g_running{false}, g_cancel{false};
static std::atomic<int> g_result{0};  // 0 none, 1 done, 2 failed, 3 cancelled
static std::thread g_worker;
static bool g_cli;            // armslauncher --prepare: no window; progress lines on stdout (RecompLauncher's prepare)
static FILE *g_cli_log;       // (and the whole log in prepare.log)
#ifdef _WIN32
static HANDLE g_job;
#endif

static void cli_out(const std::string &s) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);  // (a GUI-subsystem program: the handle RecompLauncher gave it)
    DWORD n;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s.data(), (DWORD)s.size(), &n, NULL);
#else
    fputs(s.c_str(), stdout), fflush(stdout);
#endif
}
// The whole run's progress (0..1) from the steps' weights and the running step's own fraction, printed as
// "progress <0..1> <text>" in prepare mode (at most every half second, or when the text changes)
static void report_progress(void) {
    if (!g_cli || g_steps.empty()) return;
    static double last_t;
    static std::string last_text;
    double total = 0, done = 0;
    for (size_t i = 0; i < g_steps.size(); i++) {
        if (g_steps[i].state == ST_SKIP) continue;
        total += g_steps[i].weight;
        if (i < g_step_index) done += g_steps[i].weight;
        else if (i == g_step_index) done += g_steps[i].weight * std::clamp(g_step_frac.load(), 0.0, 1.0);
    }
    std::string text = g_step_index < g_steps.size() ? g_steps[g_step_index].title : "Done";
    double now = SDL_GetTicks() / 1000.0;
    if (text == last_text && now - last_t < 0.5) return;
    last_t = now, last_text = text;
    char line[512];
    snprintf(line, sizeof line, "progress %.3f %s\n", total > 0 ? done / total : 0, text.c_str());
    cli_out(line);
}

static void log_line(const std::string &s) {
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log.push_back(s);
        if (g_log.size() > 4000) g_log.erase(g_log.begin(), g_log.begin() + 1000);
        if (g_cli_log) fprintf(g_cli_log, "%s\n", s.c_str()), fflush(g_cli_log);
    }
    // the generator's "<module>: progress <done>/<total>" lines: how far the running step is
    size_t p = s.find(": progress ");
    if (p != std::string::npos) {
        long a = 0, b = 0;
        if (sscanf(s.c_str() + p + 11, "%ld/%ld", &a, &b) == 2 && b > 0) g_step_frac = (double)a / b;
    }
    report_progress();
}

#ifdef _WIN32
static int run_process(const Step &st) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cmd = st.cmd;
    std::string path_saved;
    if (st.msys) {  // (MSYS2 UCRT64's tools first, as in the README's Git Bash setup)
        char *old = getenv("PATH");
        path_saved = old ? old : "";
        std::string p = std::string(p_msys) + "\\ucrt64\\bin;" + p_msys + "\\usr\\bin;" + path_saved;
        SetEnvironmentVariableA("PATH", p.c_str());
    }
    if (st.game) {  // (the game itself, headless: it builds the shader caches and quits - runtime/nvn/objects.c)
        SetEnvironmentVariableA("LARECOMP_PREP_ONLY", "1");
        SetEnvironmentVariableA("LARECOMP_HIDDEN", "1");
        SetEnvironmentVariableA("LARECOMP_DATA_ROOT", g_root.c_str());
        char *old = getenv("PATH");
        path_saved = old ? old : "";
        std::string p = g_root + "build;" + std::string(p_msys) + "\\ucrt64\\bin;" + path_saved;
        SetEnvironmentVariableA("PATH", p.c_str());
    }
    BOOL ok = CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                             st.cwd.c_str(), &si, &pi);
    if (st.msys || st.game) SetEnvironmentVariableA("PATH", path_saved.c_str());
    if (st.game) {
        SetEnvironmentVariableA("LARECOMP_PREP_ONLY", NULL);
        SetEnvironmentVariableA("LARECOMP_HIDDEN", NULL);
    }
    CloseHandle(wr);
    if (!ok) {
        log_line("Could not start: " + cmd + " (Windows error " + std::to_string(GetLastError()) + ")");
        CloseHandle(rd);
        return -1;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string part;
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        part.append(buf, n);
        size_t p;
        while ((p = part.find_first_of("\r\n")) != std::string::npos) {
            if (p) log_line(part.substr(0, p));
            part.erase(0, p + 1);
        }
    }
    if (!part.empty()) log_line(part);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#else
static int run_process(const Step &st) { log_line("Running steps is only implemented on Windows: " + st.cmd); return -1; }
#endif

static std::string q(const std::string &s) { return "\"" + s + "\""; }

#ifdef _WIN32
// The release's build of the game code library: generated\{game,sdk}\*.c compiled by the bundled zig, as many at a
// time as the CPU and memory allow, then linked against the game's import library (armsrecomp.dll.a) with zig's
// runtime built ahead of time (zig\rt, tools/zig_slim.sh) - as the Makefile builds build/armsgame.dll with clang.
// Writes armsgame.dll and armsgame.dll.id (the id the game checks) next to the game, then deletes the generated C,
// the objects and zig's cache (about 900 MB).
static HANDLE spawn_logged(const std::string &cmd, const std::string &cwd, const std::string &log) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return NULL;
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    std::string c = cmd;
    BOOL ok = CreateProcessA(NULL, c.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd.c_str(),
                             &si, &pi);
    CloseHandle(out);
    if (!ok) return NULL;
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static std::string file_text(const std::string &p, size_t max) {
    std::string s;
    if (FILE *f = fopen(p.c_str(), "rb")) {
        s.resize(max);
        s.resize(fread(&s[0], 1, max, f));
        fclose(f);
    }
    return s;
}
static void remove_tree(const std::string &dir) {  // (a folder this launcher made: generated\, gamebuild\obj\)
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = d.cFileName, p = dir + "\\" + n;
            if (n == "." || n == "..") continue;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryA(p.c_str()) || DeleteFileA(p.c_str());
            else if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
static int build_gamelib(void) {
    const std::string work = g_root + "gamebuild\\", objdir = work + "obj\\";
    CreateDirectoryA(work.c_str(), NULL);
    CreateDirectoryA(objdir.c_str(), NULL);
    std::string full = gamelib_full_id();
    if (full.empty()) { log_line("extracted\\exefs\\main is missing: extract the game first."); return 1; }
    const std::string zig = q(g_exe_dir + "zig\\zig.exe");
    std::string flags;  // (gamelib.id's flags; zig names CPUs its own way: -march=x86-64-v3 is -mcpu=x86_64_v3)
    for (size_t a = 0; a < g_gen_flags.size();) {
        size_t b = g_gen_flags.find(' ', a);
        if (b == std::string::npos) b = g_gen_flags.size();
        std::string f = g_gen_flags.substr(a, b - a);
        a = b + 1;
        if (f.empty()) continue;
        if (f.rfind("-march=", 0) == 0) {
            f = "-mcpu=" + f.substr(7);
            std::replace(f.begin() + 6, f.end(), '-', '_');
        }
        flags += " " + f;
    }
    flags += " " + q("-I" + g_exe_dir + "gamelib-src");
    std::string zc = work + "zig-cache";
    SetEnvironmentVariableA("ZIG_GLOBAL_CACHE_DIR", zc.c_str());
    SetEnvironmentVariableA("ZIG_LOCAL_CACHE_DIR", zc.c_str());

    struct Unit { std::string src, obj, log; unsigned long long bytes; bool entry; };
    std::vector<Unit> units;
    for (const char *m : {"game", "sdk"}) {
        WIN32_FIND_DATAA d;
        std::string dir = g_root + "generated\\" + m + "\\";
        HANDLE h = FindFirstFileA((dir + "*.c").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = d.cFileName, stem = std::string(m) + "_" + n.substr(0, n.size() - 2);
            units.push_back({dir + n, objdir + stem + ".o", objdir + stem + ".log",
                             ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow, false});
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    if (units.empty()) { log_line("No generated C in generated\\: recompile first."); return 1; }
    std::sort(units.begin(), units.end(), [](const Unit &a, const Unit &b) { return a.bytes > b.bytes; });  // (biggest first)
    units.push_back({g_exe_dir + "gamelib-src\\gamelib_entry.c", objdir + "entry.o", objdir + "entry.log", 0, true});

    int jobs = std::max(1, SDL_GetNumLogicalCPUCores());
    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {  // (about 1.5 GB for each compiler on the biggest files)
        int by_mem = (int)((ms.ullTotalPhys / (1024.0 * 1024 * 1024) - 3) / 1.5);
        jobs = std::min(jobs, std::max(1, by_mem));
    }
    jobs = std::min(jobs, 32);
    log_line("Compiling " + std::to_string(units.size()) + " files with zig, " + std::to_string(jobs) + " at a time");
    std::vector<std::pair<HANDLE, size_t>> run;
    size_t next = 0, done = 0;
    bool failed = false;
    while (!g_cancel && (run.size() || (!failed && next < units.size()))) {
        while (!failed && (int)run.size() < jobs && next < units.size()) {
            const Unit &u = units[next];
            std::string cmd = zig + " cc" + flags;
            if (u.entry) cmd += " \"-DGAMELIB_ID=\\\"" + full + "\\\"\"";
            cmd += " -c " + q(u.src) + " -o " + q(u.obj);
            HANDLE h = spawn_logged(cmd, g_root, u.log);
            if (!h) {
                log_line("Couldn't start the compiler (Windows error " + std::to_string(GetLastError()) + "): " + cmd);
                failed = true;
                break;
            }
            run.push_back({h, next++});
        }
        if (run.empty()) break;
        std::vector<HANDLE> hs;
        for (auto &r : run) hs.push_back(r.first);
        DWORD w = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, 250);
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + hs.size()) {
            size_t k = w - WAIT_OBJECT_0, idx = run[k].second;
            DWORD code = 1;
            GetExitCodeProcess(run[k].first, &code);
            CloseHandle(run[k].first);
            run.erase(run.begin() + k);
            if (code != 0) {
                failed = true;
                log_line("Compiling " + units[idx].src + " failed (exit code " + std::to_string(code) + "):");
                std::string t = file_text(units[idx].log, 4000);
                for (size_t a = 0; a < t.size();) {
                    size_t b = t.find('\n', a);
                    if (b == std::string::npos) b = t.size();
                    if (b > a) log_line("  " + t.substr(a, b - a - (t[b - 1] == '\r')));
                    a = b + 1;
                }
            } else {
                DeleteFileA(units[idx].log.c_str());
                if (++done % 25 == 0) log_line("  compiled " + std::to_string(done) + " of " + std::to_string(units.size()));
            }
            g_step_frac = 0.92 * done / units.size();
            report_progress();
        }
    }
    for (auto &r : run) CloseHandle(r.first);  // (cancelled: the job object ends them)
    if (g_cancel || failed) return 1;

    log_line("Linking armsgame.dll");
    if (FILE *f = fopen((work + "objs.rsp").c_str(), "wb")) {  // (forward slashes: backslashes are escapes in it)
        for (auto &u : units) {
            std::string o = u.obj;
            std::replace(o.begin(), o.end(), '\\', '/');
            fprintf(f, "\"%s\"\n", o.c_str());
        }
        fclose(f);
    }
    const std::string dll = g_root + "armsgame.dll", tmp = dll + ".tmp";
    Step link = {"Link", zig + " lld-link " + q("@" + g_exe_dir + "zig\\rt\\link.rsp") + " " +
                         q("-LIBPATH:" + g_exe_dir + "zig\\rt") + " " + q("-OUT:" + tmp) + " " + q("@" + work + "objs.rsp") +
                         " " + q(g_exe_dir + "armsrecomp.dll.a"), g_root, false};
    int code = run_process(link);
    if (code != 0) { log_line("Linking failed (exit code " + std::to_string(code) + ")."); return code ? code : 1; }
    DeleteFileA((dll + ".id").c_str());
    if (!MoveFileExA(tmp.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log_line("Couldn't replace armsgame.dll (Windows error " + std::to_string(GetLastError()) +
                 "): close the game if it's running, then try again.");
        return 1;
    }
    if (FILE *f = fopen((dll + ".id").c_str(), "w")) fprintf(f, "%s\n", full.c_str()), fclose(f);
    log_line("Built armsgame.dll (" + full + ")");
    remove_tree(g_root + "gamebuild");  // (the objects and zig's cache: ~900 MB, of no use once it's linked)
    remove_tree(g_root + "generated");
    g_step_frac = 1;
    return 0;
}
#else
static int build_gamelib(void) { log_line("Building the game code is only implemented on Windows."); return 1; }
#endif

static void worker(void) {
    int result = 1;
    for (size_t i = 0; i < g_steps.size(); i++) {
        Step &st = g_steps[i];
        if (st.state == ST_SKIP) continue;
        if (g_cancel) { st.state = ST_SKIP; continue; }
        g_step_index = i;
        g_step_frac = 0;
        st.state = ST_RUN;
        log_line("");
        log_line("==== " + st.title);
        if (!st.cmd.empty()) log_line("> " + st.cmd);
        uint64_t t0 = SDL_GetTicks();
        int code = st.kind == K_BUILD_GAMELIB ? build_gamelib() : run_process(st);
        st.secs = (SDL_GetTicks() - t0) / 1000.0;
        if (g_cancel) { st.state = ST_FAIL; result = 3; log_line("Cancelled."); break; }
        if (code != 0) {
            st.state = ST_FAIL;
            result = 2;
            {
                std::lock_guard<std::mutex> lk(g_log_mu);
                for (size_t j = g_log.size(); j-- > 0 && g_log[j].rfind("> ", 0) != 0 && g_log[j].rfind("==== ", 0) != 0;)
                    if (!g_log[j].empty()) { st.why = g_log[j]; break; }
            }
            log_line("Failed (exit code " + std::to_string(code) + ").");
            break;
        }
        st.state = ST_OK;
        g_step_frac = 1;
        log_line("Done in " + std::to_string((int)st.secs) + " s.");
    }
    g_step_index = g_steps.size();
    g_result = result;
    g_running = false;
}

// The release's steps: everything from the folder the launcher is in (make dist)
static void release_steps(bool extract, bool recompile, bool build) {
    std::string gen = q(g_exe_dir + "armsgen\\armsgen.exe");
    if (extract) {
        std::string c = gen + " extract " + q(p_base) + " " + q(p_keys) + " extracted";
        if (p_update[0]) c += " --update " + q(p_update);
        c += " --title 01009B500007C000 --build 501924b7c22129e5 --want \"ARMS 5.4.0\"";  // (else: why, in one line)
        Step s = {"Extract the game's code", c, g_root, false};
        s.weight = 1;
        g_steps.push_back(s);
    }
    if (recompile) {
        Step s = {"Recompile the game (ARM64 to C)", gen + " gen game extracted\\exefs\\main 0x7000000000 generated\\game --simd c",
                  g_root, false};
        s.weight = 2.5;
        g_steps.push_back(s);
        s = {"Recompile the system code", gen + " gen sdk extracted\\exefs\\sdk 0x7006000000 generated\\sdk --hle " +
                                              q(g_exe_dir + "gamelib-src\\hle_symbols.txt") + " --simd c", g_root, false};
        s.weight = 1;
        g_steps.push_back(s);
    }
    if (build) {
        Step s = {"Build the game code (C to x86-64)", "", g_root, false};
        s.kind = K_BUILD_GAMELIB;
        s.weight = 12;
        g_steps.push_back(s);
        Step prep = {"Prepare the shaders", q(g_exe_dir + "armsrecomp.exe") + " extracted", g_root, false};
        prep.game = true;
        prep.weight = 2;
        g_steps.push_back(prep);
    }
}

static void start_steps(bool extract, bool recompile, bool build) {
    if (g_running) return;
    if (g_worker.joinable()) g_worker.join();
    g_steps.clear();
    g_cancel = false;
    g_result = 0;
    if (g_release) {
        release_steps(extract, recompile, build);
        {
            std::lock_guard<std::mutex> lk(g_log_mu);
            g_log.clear();
        }
        g_running = true;
        g_worker = std::thread(worker);
        return;
    }
    std::string py = g_root + ".venv\\Scripts\\python.exe";
    if (!g_status.venv) {
        g_steps.push_back({"Python environment", "py -3.12 -m venv .venv", g_root, false});
        g_steps.push_back({"Python modules", q(py) + " -m pip install --disable-pip-version-check unicorn==2.1.4 capstone==5.0.9 "
                                             "lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0", g_root, false});
    }
    if (extract) {
        std::string c = q(py) + " -I tools\\nsp_extract.py " + q(p_base) + " " + q(p_keys) + " extracted";
        if (p_update[0]) c += " --update " + q(p_update);
        g_steps.push_back({"Extract the game's code", c, g_root, false});
    }
    std::string make = q(std::string(p_msys) + "\\usr\\bin\\make.exe");
    if (recompile) g_steps.push_back({"Recompile (ARM64 to C)", make + " gen PY=.venv/Scripts/python.exe", g_root, true});
    if (build) {
        int n = SDL_GetNumLogicalCPUCores();
        g_steps.push_back({"Build the PC version", make + " -j" + std::to_string(n > 0 ? n : 8) +
                                                    " PY=.venv/Scripts/python.exe build/armsrecomp.exe build/armsgame.dll build/.dlls", g_root, true});
        // the game's shaders built into the caches now, so the first start doesn't build them in play
        Step prep = {"Prepare the shaders", q(g_root + "build\\armsrecomp.exe") + " extracted", g_root, false};
        prep.game = true;
        g_steps.push_back(prep);
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log.clear();
    }
    g_running = true;
    g_worker = std::thread(worker);
}
static void cancel_steps(void) {
    g_cancel = true;
#ifdef _WIN32
    if (g_job) TerminateJobObject(g_job, 1);
#endif
}

// ================================================================== the game
// wait_code: wait for the game to close and give its exit code (armslauncher --play, as RecompLauncher starts it)
static std::string launch_game(int *wait_code = nullptr) {
    std::string exe = g_release ? g_exe_dir + "armsrecomp.exe" : g_root + "build\\armsrecomp.exe";
    if (!is_file(exe)) exe = g_exe_dir + "armsrecomp.exe";
    if (!is_file(exe)) return "The PC version isn't built yet: set up your game files first (GAME FILES).";
    if (!is_dir(g_root + "extracted")) return "No extracted game files: set them up first (GAME FILES).";
#ifdef _WIN32
    SetEnvironmentVariableA("LARECOMP_DATA_ROOT", g_root.c_str());
    {   // the build copies the runtime DLLs next to the game (make dlls); MSYS2's own folder is the fallback
        static bool once;
        if (!once) {
            once = true;
            char *old = getenv("PATH");
            std::string p = g_root + "build;" + std::string(p_msys) + "\\ucrt64\\bin;" + (old ? old : "");
            SetEnvironmentVariableA("PATH", p.c_str());
        }
    }
    std::string cmd = q(exe) + " extracted";
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmd.data(), NULL, NULL, FALSE, 0, NULL, g_root.c_str(), &si, &pi))
        return "Couldn't start the game (Windows error " + std::to_string(GetLastError()) + ").";
    CloseHandle(pi.hThread);
    if (wait_code) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        *wait_code = (int)code;
    }
    CloseHandle(pi.hProcess);
    return "";
#else
    return "Starting the game is only implemented on Windows.";
#endif
}

// ================================================================== drawing helpers
static ImFont *f_body, *f_head, *f_title, *f_small;
static float g_scale = 1;

// Text drawn sheared to the right (an italic look for the bold display font): its vertices are moved after AddText.
static void slant_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float shear = 0.22f) {
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, col, text);
    float base = pos.y + size;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * shear;
}
static void slant_text_outlined(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, ImU32 outline, const char *t,
                                float w = 3) {
    w *= g_scale;
    for (int k = 0; k < 8; k++) {
        float a = k * 3.14159265f / 4;
        slant_text(dl, font, size, ImVec2(pos.x + cosf(a) * w, pos.y + sinf(a) * w), outline, t);
    }
    slant_text(dl, font, size, ImVec2(pos.x + w * 1.6f, pos.y + w * 1.6f), outline, t);  // (drop shadow)
    slant_text(dl, font, size, pos, col, t);
}
// A parallelogram: the sporty slanted panel
static void slab(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float skew) {
    dl->AddQuadFilled(ImVec2(a.x + skew, a.y), ImVec2(b.x + skew, a.y), ImVec2(b.x - skew, b.y), ImVec2(a.x - skew, b.y), col);
}
// A coil spring along a line from p to q: loops as ellipses, compressed by `squash` (0..1)
static void spring(ImDrawList *dl, ImVec2 p, ImVec2 q, float radius, int loops, ImU32 col, ImU32 shade, float thick) {
    ImVec2 d(q.x - p.x, q.y - p.y);
    float len = sqrtf(d.x * d.x + d.y * d.y);
    ImVec2 u(d.x / len, d.y / len), n(-u.y, u.x);
    const int seg = loops * 24;
    ImVec2 prev;
    for (int i = 0; i <= seg; i++) {
        float t = (float)i / seg, a = t * loops * 6.2831853f;
        float along = t * len + sinf(a) * radius * 0.55f, side = cosf(a) * radius;
        ImVec2 pt(p.x + u.x * along + n.x * side, p.y + u.y * along + n.y * side);
        if (i) dl->AddLine(prev, pt, sinf(a) > 0 ? col : shade, thick);
        prev = pt;
    }
}
static void halftone(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float step) {
    for (float y = a.y; y < b.y; y += step)
        for (float x = a.x + (fmodf(y, step * 2) < step ? 0 : step / 2); x < b.x; x += step) {
            float k = (x - a.x) / (b.x - a.x);  // (dots grow left to right)
            dl->AddCircleFilled(ImVec2(x, y), step * 0.08f + step * 0.32f * k, col, 8);
        }
}
static void background(ImDrawList *dl, ImVec2 size, float t) {
    dl->AddRectFilled(ImVec2(0, 0), size, C_YELLOW);
    float w = 90 * g_scale, off = fmodf(t * 18 * g_scale, w * 2);  // diagonal stripes, drifting
    for (float x = -size.y - w * 2 + off; x < size.x + w; x += w * 2)
        dl->AddQuadFilled(ImVec2(x, 0), ImVec2(x + w, 0), ImVec2(x + w + size.y, size.y), ImVec2(x + size.y, size.y), C_YELLOW2);
    halftone(dl, ImVec2(size.x * 0.55f, size.y * 0.62f), size, IM_COL32(0, 0, 0, 40), 14 * g_scale);
    // springs: one pink from the left, one cyan from the right, breathing
    float s1 = 0.5f + 0.5f * sinf(t * 1.7f), s2 = 0.5f + 0.5f * sinf(t * 1.7f + 2.1f);
    spring(dl, ImVec2(-40 * g_scale, size.y * 0.80f), ImVec2(size.x * (0.20f + 0.05f * s1), size.y * (0.66f - 0.03f * s1)),
           26 * g_scale, 9, C_PINK, IM_COL32(170, 20, 80, 255), 7 * g_scale);
    spring(dl, ImVec2(size.x + 40 * g_scale, size.y * 0.30f), ImVec2(size.x * (0.82f - 0.05f * s2), size.y * (0.20f + 0.03f * s2)),
           22 * g_scale, 8, C_CYAN, IM_COL32(10, 120, 160, 255), 6 * g_scale);
    // the black band at the bottom
    dl->AddQuadFilled(ImVec2(0, size.y - 46 * g_scale), ImVec2(size.x, size.y - 70 * g_scale), ImVec2(size.x, size.y),
                      ImVec2(0, size.y), C_BLACK);
}

static bool big_button(const char *id, const char *label, ImVec2 size, ImU32 fill, ImU32 text, bool enabled = true) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size) && enabled;
    bool hover = ImGui::IsItemHovered() && enabled, held = ImGui::IsItemActive() && enabled;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float sk = size.y * 0.22f, sh = (held ? 2.0f : 6.0f) * g_scale;
    ImVec2 a(p.x + sk, p.y), b(p.x + size.x - sk, p.y + size.y);
    slab(dl, ImVec2(a.x + sh, a.y + sh), ImVec2(b.x + sh, b.y + sh), C_BLACK, sk);
    ImU32 f = !enabled ? IM_COL32(120, 120, 130, 255) : hover ? IM_COL32(255, 255, 255, 255) : fill;
    if (held) a.x += 3 * g_scale, a.y += 3 * g_scale, b.x += 3 * g_scale, b.y += 3 * g_scale;
    slab(dl, a, b, f, sk);
    ImFont *font = size.y > 70 * g_scale ? f_title : f_head;
    float fs = font->FontSize * (size.y > 70 * g_scale ? 0.9f : 1.0f);
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
    ImVec2 tp((a.x + b.x) / 2 - ts.x / 2 - fs * 0.1f, (a.y + b.y) / 2 - ts.y / 2);
    slant_text(dl, font, fs, tp, hover && enabled ? C_BLACK : text, label);
    return pressed;
}
static void heading(const char *text, ImU32 col = C_YELLOW) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    slant_text(dl, f_head, f_head->FontSize, p, col, text);
    ImGui::Dummy(ImVec2(0, f_head->FontSize + 4 * g_scale));
}
static void hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(C_GREY));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
static void check_mark(bool ok, bool busy = false, bool pending = false) {  // a dot with a tick / a cross (no glyphs)
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 10 * g_scale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() / 2 + 1);
    if (pending) {
        dl->AddCircle(c, r - 1, C_GREY, 20, 2 * g_scale);
    } else if (busy) {
        float t = (float)ImGui::GetTime() * 6;
        dl->PathArcTo(c, r - 1, t, t + 4.2f, 16);
        dl->PathStroke(C_CYAN, 0, 3 * g_scale);
    } else {
        dl->AddCircleFilled(c, r, ok ? C_LIME : C_PINK, 20);
        if (ok) {
            dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), C_BLACK, 2.6f * g_scale);
            dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y - r * 0.4f), C_BLACK, 2.6f * g_scale);
        } else {
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), C_WHITE, 2.6f * g_scale);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), C_WHITE, 2.6f * g_scale);
        }
    }
    ImGui::Dummy(ImVec2(r * 2 + 6 * g_scale, 0));
    ImGui::SameLine();
}
static bool combo(const char *label, int *i, const Choice *c, int n) {
    bool changed = false;
    if (ImGui::BeginCombo(label, c[*i].label)) {
        for (int k = 0; k < n; k++)
            if (ImGui::Selectable(c[k].label, k == *i)) *i = k, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}
static void row_label(const char *text, float w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(w);
}

// ================================================================== pages
static std::string g_status_msg;
static double g_status_until;
static void flash(const std::string &s) { g_status_msg = s; g_status_until = ImGui::GetTime() + 6; }

static void page_play(void) {
    heading("READY TO FIGHT?");
    float lw = 230 * g_scale;
    struct { const char *name; bool ok; const char *fix; } rows[] = {
        {"Your game files are extracted", g_status.extracted, "GAME FILES: choose your game, update and keys, then INSTALL."},
        {"The game's code is recompiled", g_status.generated, "GAME FILES: INSTALL recompiles it."},
        {"The PC version is built", g_status.built, "GAME FILES: INSTALL builds it."},
    };
    for (auto &r : rows) {
        if (g_release && &r == &rows[1]) continue;  // (one step with the build there)
        check_mark(r.ok);
        ImGui::TextUnformatted(r.name);
        if (!r.ok) { ImGui::SameLine(); ImGui::TextColored(V(C_GREY), "  %s", r.fix); }
    }
    if (!g_status.version.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(V(C_CYAN), "Game version %s", g_status.version.c_str());
    }
    ImGui::Spacing();
    ImGui::Spacing();
    bool ready = g_status.extracted && g_status.built && !g_running;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail.x * 0.12f);
    static int autoplay = getenv("ARMS_LAUNCHER_PLAY") ? 5 : -1;
    if (big_button("##play", "FIGHT!", ImVec2(avail.x * 0.76f, 92 * g_scale), C_PINK, C_WHITE, ready) ||
        (ready && autoplay > 0 && --autoplay == 0)) {
        settings_to_ini();
        std::string e = launch_game();
        if (e.empty()) flash("The game is starting - have fun!");
        else flash(e);
    }
    ImGui::Spacing();
    row_label("Window", lw);
    const char *modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    ImGui::SetNextItemWidth(320 * g_scale);
    ImGui::Combo("##mode_play", &s_mode, modes, 3);
    row_label("Frame rate counter", lw);
    ImGui::Checkbox("##fps_play", &s_show_fps);
    ImGui::Spacing();
    hint("Keyboard and mouse: WASD move, left / right mouse punch, middle mouse (or both) grab, Space jump, Shift dash, "
         "Ctrl guard, Q / E rush. Controllers work too. Esc or F1 in game opens the menu (controls, video, audio).");
}

static void path_row(const char *label, char *buf, size_t n, int target, SDL_Window *win, bool optional, float lw) {
    bool have = buf[0] && is_file(buf);
    check_mark(have || (optional && !buf[0]));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130 * g_scale);
    char id[32];
    snprintf(id, sizeof id, "##path%d", target);
    if (ImGui::InputText(id, buf, n)) paths_to_ini();
    ImGui::SameLine();
    snprintf(id, sizeof id, "Browse##%d", target);
    if (ImGui::Button(id, ImVec2(118 * g_scale, 0))) browse(target, win);
}

static void page_setup(SDL_Window *win) {
    heading("YOUR GAME FILES");
    hint("Use your own copy of ARMS, dumped from your Switch with your own keys. Nothing is downloaded or shared: the "
         "game data stays in your files, read in place.");
    ImGui::Spacing();
    float lw = 270 * g_scale;
    path_row("Base game (XCI or NSP)", p_base, sizeof p_base, 0, win, false, lw);
    path_row("5.4.0 update (NSP)", p_update, sizeof p_update, 1, win, false, lw);
    path_row("Keys (prod.keys)", p_keys, sizeof p_keys, 2, win, false, lw);
    ImGui::Spacing();
    if (g_release) {
        heading("TOOLS", C_CYAN);
        check_mark(true);
        ImGui::TextUnformatted("Included: the extractor, the recompiler and the compiler (nothing else to install)");
        ImGui::Spacing();
    } else {
    heading("TOOLS", C_CYAN);
    check_mark(g_status.msys);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("MSYS2 (clang, make)");
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::InputText("##msys", p_msys, sizeof p_msys)) { paths_to_ini(); refresh_status(); }
    if (!g_status.msys) {
        ImGui::SameLine();
        ImGui::TextColored(V(C_PINK), "not found: see the README");
    }
    check_mark(g_status.venv);
    ImGui::TextUnformatted(g_status.venv ? "Python environment (.venv)" : "Python environment: made by INSTALL (needs Python 3.12)");
    ImGui::Spacing();
    }

    bool inputs = p_base[0] && is_file(p_base) && p_keys[0] && is_file(p_keys) && p_update[0] && is_file(p_update);
    bool can = !g_running && g_status.msys;
    float bw = (ImGui::GetContentRegionAvail().x - 24 * g_scale) / 3;
    if (big_button("##install", "INSTALL", ImVec2(bw, 58 * g_scale), C_LIME, C_BLACK, can && inputs)) {
        paths_to_ini();
        start_steps(true, true, true);
    }
    ImGui::SameLine();
    if (big_button("##rebuild", "REBUILD", ImVec2(bw, 58 * g_scale), C_CYAN, C_BLACK, can && g_status.extracted))
        start_steps(!is_dir(g_root + "extracted\\shaders") && inputs, true, true);
    ImGui::SameLine();
    if (big_button("##cancel", "CANCEL", ImVec2(bw, 58 * g_scale), C_PINK, C_WHITE, g_running)) cancel_steps();
    hint("INSTALL: extract, recompile and build (about 10 minutes the first time). REBUILD: recompile and build again "
         "from the extracted files.");
    ImGui::Spacing();

    for (auto &st : g_steps) {
        check_mark(st.state == ST_OK, st.state == ST_RUN, st.state == ST_WAIT || st.state == ST_SKIP);
        if (st.state == ST_RUN) ImGui::TextColored(V(C_CYAN), "%s...", st.title.c_str());
        else if (st.state == ST_OK) ImGui::Text("%s  (%d s)", st.title.c_str(), (int)st.secs);
        else if (st.state == ST_FAIL) ImGui::TextColored(V(C_PINK), "%s: failed", st.title.c_str());
        else ImGui::TextColored(V(C_GREY), "%s", st.title.c_str());
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 160));
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(f_small);
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        ImGuiListClipper clip;
        clip.Begin((int)g_log.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const std::string &l = g_log[i];
                bool err = l.find("error") != std::string::npos || l.find("Failed") != std::string::npos;
                if (l.rfind("====", 0) == 0) ImGui::TextColored(V(C_YELLOW), "%s", l.c_str());
                else if (err) ImGui::TextColored(V(C_PINK), "%s", l.c_str());
                else ImGui::TextUnformatted(l.c_str());
            }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void page_graphics(void) {
    heading("DISPLAY");
    float lw = 250 * g_scale, cw = 420 * g_scale;
    row_label("Window", lw);
    const char *modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    ImGui::SetNextItemWidth(cw);
    ImGui::Combo("##mode", &s_mode, modes, 3);
    row_label(s_mode == 2 ? "Display mode" : "Window size", lw);
    ImGui::SetNextItemWidth(cw);
    if (s_mode == 1) {
        char d[64];
        snprintf(d, sizeof d, "The desktop's (%dx%d)", g_desktop.w, g_desktop.h);
        ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##size_b", d)) ImGui::EndCombo();
        ImGui::EndDisabled();
    } else {
        char cur[64];
        auto label = [&](int i, char *out, size_t n) {
            if (i < 0) snprintf(out, n, s_mode == 2 ? "The desktop's (%dx%d)" : "Default (1280x720)", g_desktop.w, g_desktop.h);
            else snprintf(out, n, s_mode == 2 ? "%dx%d (%.0f Hz)" : "%dx%d", g_modes[i].w, g_modes[i].h, g_modes[i].hz);
        };
        label(s_size, cur, sizeof cur);
        if (ImGui::BeginCombo("##size", cur)) {
            for (int i = -1; i < (int)g_modes.size(); i++) {
                char m[64];
                label(i, m, sizeof m);
                if (ImGui::Selectable(m, s_size == i)) s_size = i;
            }
            ImGui::EndCombo();
        }
    }
    row_label("Aspect ratio", lw);
    ImGui::SetNextItemWidth(cw);
    combo("##aspect", &s_aspect, k_aspect, IM_ARRAYSIZE(k_aspect));
    ImGui::Spacing();
    heading("QUALITY", C_CYAN);
    row_label("Internal resolution", lw);
    ImGui::SetNextItemWidth(cw);
    combo("##render", &s_render, k_render, IM_ARRAYSIZE(k_render));
    row_label("Upscaler", lw);
    ImGui::SetNextItemWidth(cw);
    combo("##upscaler", &s_upscaler, k_upscaler, IM_ARRAYSIZE(k_upscaler));
    row_label("Anti-aliasing", lw);
    ImGui::SetNextItemWidth(cw);
    combo("##aa", &s_aa, k_aa, IM_ARRAYSIZE(k_aa));
    row_label("Frame rate", lw);
    ImGui::TextColored(V(C_LIME), "60 fps");
    ImGui::SameLine();
    ImGui::TextColored(V(C_GREY), "(ARMS's own: its fights step once a frame)");
    row_label("Frame rate counter", lw);
    ImGui::Checkbox("##fps", &s_show_fps);
    hint("Internal resolution renders the game above (2x, 3x, 4x) or below its own 1080p. Dynamic lowers it on the fly "
         "to hold 60 fps. F11 or Alt+Enter switches fullscreen in game.");
    ImGui::Spacing();
    heading("SOUND & CONTROLLER", C_PINK);
    row_label("Volume", lw);
    ImGui::SetNextItemWidth(cw);
    int pct = (int)lroundf(s_volume * 100);
    if (ImGui::SliderInt("##vol", &pct, 0, 200, "%d%%")) s_volume = pct / 100.0f;
    row_label("Rumble", lw);
    ImGui::Checkbox("##rumble", &s_rumble);
    row_label("Controllers in the background", lw);
    ImGui::Checkbox("##bg", &s_bg_input);
}

static void controls_table(const char *id, const char *const (*rows)[2], int n) {
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        for (int i = 0; i < n; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(V(C_YELLOW), "%s", rows[i][0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rows[i][1]);
        }
        ImGui::EndTable();
    }
}
static void page_controls(void) {
    static const char *const kbm[][2] = {
        {"Move / curve a punch", "W A S D"}, {"Left punch", "Left mouse (or Backspace)"}, {"Right punch", "Right mouse (or Enter)"},
        {"Grab", "Middle mouse, or both buttons"}, {"Jump", "Space"}, {"Dash", "Left Shift"}, {"Guard", "Left Ctrl"},
        {"Rush (ARMS Rush)", "Q or E"}, {"Menus", "Arrows, Enter, Backspace"}, {"Pause (+) / (-)", "Tab / M"},
        {"In-game menu", "Esc or F1"}, {"Fullscreen", "F11 or Alt+Enter"},
    };
    static const char *const pad[][2] = {
        {"Move / curve a punch", "Left stick"}, {"Left punch", "B / bottom button"}, {"Right punch", "A / right button"},
        {"Grab", "A + B together"}, {"Jump", "X / top button"}, {"Dash", "Y / left button"}, {"Guard", "Left stick press"},
        {"Rush (ARMS Rush)", "L or R (bumpers)"}, {"Pause", "Start / Menu"}, {"In-game menu", "Home / Guide"},
    };
    float half = (ImGui::GetContentRegionAvail().x - 24 * g_scale) / 2;
    float tall = std::max(200 * g_scale, ImGui::GetContentRegionAvail().y - f_head->FontSize - 120 * g_scale);
    ImGui::BeginGroup();
    heading("KEYBOARD & MOUSE");
    ImGui::BeginChild("##kbm", ImVec2(half, tall));
    controls_table("##kbmt", kbm, IM_ARRAYSIZE(kbm));
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * g_scale);
    ImGui::BeginGroup();
    heading("CONTROLLER", C_CYAN);
    ImGui::BeginChild("##padc", ImVec2(half, tall));
    controls_table("##padt", pad, IM_ARRAYSIZE(pad));
    ImGui::Spacing();
    hint("Xbox, PlayStation, Switch Pro and other controllers work through SDL. Buttons go by position, as on the "
         "Switch: the right face button punches right, the bottom one punches left.");
    ImGui::EndChild();
    ImGui::EndGroup();
    hint("Rebind anything in game: Esc or F1, then Controls (click a row, then press the key, mouse button or "
         "controller button).");
    if (ImGui::Button("Reset keyboard & mouse")) {
        int n = g_settings_ini.remove_prefix("bind_key_");
        g_settings_ini.save("ARMS Rearmed settings (the launcher and the in-game menu)");
        flash(n ? "Keyboard and mouse back to the defaults above." : "Keyboard and mouse were already on the defaults.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset controller")) {
        int n = g_settings_ini.remove_prefix("bind_pad_");
        g_settings_ini.save("ARMS Rearmed settings (the launcher and the in-game menu)");
        flash(n ? "Controller back to the defaults above." : "The controller was already on the defaults.");
    }
}

static void page_about(void) {
    heading("ABOUT");
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("ARMS Rearmed runs your copy of ARMS natively on Windows: the game's ARM64 code is recompiled to "
                           "C and built for x86-64, and a Switch runtime (graphics through Vulkan, audio, input, files) "
                           "takes the console's place.");
    ImGui::Spacing();
    ImGui::TextColored(V(C_CYAN), "Credits");
    ImGui::BulletText("ARMS Rearmed by Peiikai and contributors (MIT): the recompiler and the Switch runtime");
    ImGui::BulletText("SDL 3, Dear ImGui, FFmpeg, the Vulkan SDK, AMD FidelityFX FSR, SMAA");
    ImGui::BulletText("Baloo 2 font (Ek Type, SIL Open Font License)");
    ImGui::BulletText("Switchbrew's documentation");
    ImGui::Spacing();
    ImGui::TextColored(V(C_CYAN), "Legal");
    ImGui::TextColored(V(C_GREY), "ARMS is a trademark of Nintendo. This project is not affiliated with or endorsed by "
                       "Nintendo. It contains no game code, data, art or fonts: you need your own legally obtained copy "
                       "of the game, its update and your console's keys. This launcher's look is original.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextColored(V(C_GREY), "Data folder: %s", g_root.c_str());
}

// ================================================================== style
static void style(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0, s.ChildRounding = 10, s.FrameRounding = 8, s.GrabRounding = 8, s.PopupRounding = 8;
    s.FramePadding = ImVec2(12, 7), s.ItemSpacing = ImVec2(10, 9), s.WindowPadding = ImVec2(0, 0);
    s.FrameBorderSize = 0, s.ChildBorderSize = 1, s.ScrollbarSize = 14, s.ScrollbarRounding = 8;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = V(C_WHITE);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_PopupBg] = V(IM_COL32(30, 30, 38, 250));
    c[ImGuiCol_FrameBg] = V(IM_COL32(52, 52, 64, 255));
    c[ImGuiCol_FrameBgHovered] = V(IM_COL32(70, 70, 88, 255));
    c[ImGuiCol_FrameBgActive] = V(IM_COL32(84, 84, 104, 255));
    c[ImGuiCol_Button] = V(IM_COL32(52, 52, 64, 255));
    c[ImGuiCol_ButtonHovered] = V(C_CYAN);
    c[ImGuiCol_ButtonActive] = V(C_PINK);
    c[ImGuiCol_Header] = V(IM_COL32(255, 45, 120, 200));
    c[ImGuiCol_HeaderHovered] = V(C_PINK);
    c[ImGuiCol_HeaderActive] = V(C_PINK);
    c[ImGuiCol_CheckMark] = V(C_LIME);
    c[ImGuiCol_SliderGrab] = V(C_YELLOW);
    c[ImGuiCol_SliderGrabActive] = V(C_LIME);
    c[ImGuiCol_TableRowBg] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = V(IM_COL32(255, 225, 10, 160));
    c[ImGuiCol_TextSelectedBg] = V(IM_COL32(25, 211, 255, 120));
}

static ImFont *load_font(float size) {
    ImGuiIO &io = ImGui::GetIO();
    for (const std::string &p : {g_exe_dir + "Baloo2-SemiBold.ttf", g_root + "third_party\\fonts\\Baloo2\\Baloo2-SemiBold.ttf",
                                 std::string("C:\\Windows\\Fonts\\segoeuib.ttf"), std::string("C:\\Windows\\Fonts\\arialbd.ttf")})
        if (is_file(p))
            if (ImFont *f = io.Fonts->AddFontFromFileTTF(p.c_str(), size)) return f;
    return io.Fonts->AddFontDefault();
}

// ================================================================== main
// armslauncher --prepare --game=<game.xci|nsp> --update=<update.nsp> --keys=<prod.keys>: RecompLauncher's
// prepare step (recomplauncher.json). No window: the paths are saved (launcher.ini), then INSTALL's steps run, with
// "progress <0..1> <text>" lines on stdout and the whole log in prepare.log. Exit code 0 when the game is ready.
static int prepare_cli(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        auto val = [&](const char *key, char *dst, size_t n) {  // (--key=<path>, or --key <path>)
            size_t k = strlen(key);
            if (!strncmp(a, key, k) && a[k] == '=') { snprintf(dst, n, "%s", a + k + 1); return true; }
            if (!strcmp(a, key) && i + 1 < argc) { snprintf(dst, n, "%s", argv[++i]); return true; }
            return false;
        };
        if (!val("--base", p_base, sizeof p_base) && !val("--game", p_base, sizeof p_base) &&
            !val("--update", p_update, sizeof p_update) && !val("--keys", p_keys, sizeof p_keys) && strcmp(a, "--prepare"))
            cli_out(std::string("unknown argument: ") + a + "\n");
    }
    g_cli_log = fopen((g_root + "prepare.log").c_str(), "w");
    auto fail = [](const std::string &why) {
        cli_out("error " + why + "\n");
        log_line(why);
        return 1;
    };
    if (!g_release) return fail("This isn't a release folder (armsgen, zig and gamelib-src next to the launcher).");
    if (!p_base[0] || !is_file(p_base)) return fail("The game (an XCI or NSP of ARMS) wasn't found: " + std::string(p_base));
    if (!p_keys[0] || !is_file(p_keys)) return fail("The keys file (prod.keys) wasn't found: " + std::string(p_keys));
    if (!p_update[0] || !is_file(p_update)) return fail("The 5.4.0 update (NSP) wasn't found: " + std::string(p_update));
    paths_to_ini();
    start_steps(true, true, true);
    g_worker.join();
    if (g_result != 1) {
        std::string why = "A step failed: see prepare.log in the game's folder.";
        for (auto &st : g_steps)
            if (st.state == ST_FAIL) {
                why = st.title + " failed: " + (st.why.empty() ? "" : st.why + " ") + "(prepare.log in the game's folder has more)";
                break;
            }
        cli_out("error " + why + "\n");
        return 2;
    }
    cli_out("progress 1 Ready to play\n");
    return 0;
}

int main(int argc, char **argv) {
    bool prepare = false, play = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) prepare = true;
        if (!strcmp(argv[i], "--play")) play = true;
    }
    find_paths();
    find_release();
    g_launcher_ini.load(g_root + "launcher.ini");
    paths_from_ini();
#ifdef _WIN32
    g_job = CreateJobObjectA(NULL, NULL);  // (Cancel / closing the launcher ends a step's whole process tree)
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
#endif
    if (prepare) {
        g_cli = true;
        int r = prepare_cli(argc, argv);
        if (g_cli_log) fclose(g_cli_log);
        return r;
    }
    refresh_status();
    // armslauncher --play (RecompLauncher's Play): the game straight away, waited for (its exit code is ours). Only when
    // the game code needs building again - a release update changed the recompiler - the window opens and rebuilds it
    // first, from the game files set up before.
    if (play && g_status.extracted && g_status.built) {
        int code = 0;
        std::string e = launch_game(&code);
        if (!e.empty()) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ARMS Rearmed", e.c_str(), NULL); return 1; }
        return code;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    list_modes();
    g_settings_ini.load(g_root + "larecomp.ini");
    settings_from_ini();
    bool play_after = play && g_status.extracted && p_base[0] && p_keys[0];  // (rebuild, then play)

    g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_scale <= 0) g_scale = 1;
    int ww = (int)(1240 * g_scale), wh = (int)(840 * g_scale);  // (within the desktop, high-DPI screens included)
    if (ww > g_desktop.w * 9 / 10) ww = g_desktop.w * 9 / 10;
    if (wh > g_desktop.h * 9 / 10) wh = g_desktop.h * 9 / 10;
    SDL_Window *win = SDL_CreateWindow("ARMS Rearmed - Launcher", ww, wh,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    SDL_SetWindowMinimumSize(win, std::min(ww, (int)(1000 * g_scale)), std::min(wh, (int)(700 * g_scale)));
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) return 1;
    SDL_SetRenderVSync(ren, 1);
    for (const std::string &p : {g_exe_dir + "icon.png", g_root + "build\\icon.png"}) {
        if (!is_file(p)) continue;
        if (SDL_Surface *s = SDL_LoadPNG(p.c_str())) { SDL_SetWindowIcon(win, s); SDL_DestroySurface(s); break; }
    }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    f_body = load_font(21 * g_scale);
    f_small = load_font(16 * g_scale);
    f_head = load_font(34 * g_scale);
    f_title = load_font(76 * g_scale);
    style();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    static const char *const tabs[] = {"PLAY", "GAME FILES", "GRAPHICS", "CONTROLS", "ABOUT"};
    int tab = g_status.built && g_status.extracted ? 0 : 1;
    if (getenv("ARMS_LAUNCHER_TAB")) tab = std::clamp(atoi(getenv("ARMS_LAUNCHER_TAB")), 0, 4);
    bool quit = false, was_running = false;
    uint64_t last_status = 0;
    int exit_code = 0;
    if (play_after) {
        tab = 1;
        start_steps(!is_dir(g_root + "extracted\\shaders") && is_file(p_base) && is_file(p_keys), true, true);
        flash("The game code needs building again for this version: it plays as soon as it's done.");
    } else if (play) {
        flash("Set up your game files first: choose them here, then INSTALL.");
    }
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        {   // a file dialog's answer (SDL may call back from another thread)
            std::lock_guard<std::mutex> lk(g_dialog_mu);
            if (g_dialog_target >= 0) {
                char *dst = g_dialog_target == 0 ? p_base : g_dialog_target == 1 ? p_update : p_keys;
                snprintf(dst, g_dialog_target == 0 ? sizeof p_base : g_dialog_target == 1 ? sizeof p_update : sizeof p_keys,
                         "%s", g_dialog_result.c_str());
                g_dialog_target = -1;
                paths_to_ini();
            }
        }
        {   // (test) ARMS_LAUNCHER_INSTALL=1: INSTALL with launcher.ini's paths; =2: REBUILD. The log goes to
            // ARMS_LAUNCHER_LOG when it ends, and the launcher quits.
            static int auto_run = getenv("ARMS_LAUNCHER_INSTALL") ? atoi(getenv("ARMS_LAUNCHER_INSTALL")) : 0, frames;
            if (auto_run && ++frames == 10) {
                log_line(std::string("(test) ") + (auto_run == 1 ? "INSTALL" : "REBUILD") + " from " + g_root);
                if (auto_run == 1) start_steps(true, true, true);
                else start_steps(false, true, true);
            }
            bool end = auto_run && frames > 10 && !g_running;
            if (auto_run && frames > 10 && (end || frames % 60 == 0))
                if (const char *lp = getenv("ARMS_LAUNCHER_LOG"))
                    if (FILE *f = fopen(lp, "w")) {
                        std::lock_guard<std::mutex> lk(g_log_mu);
                        for (auto &l : g_log) fprintf(f, "%s\n", l.c_str());
                        if (end) fprintf(f, "RESULT %d\n", g_result.load());
                        fclose(f);
                    }
            if (end) quit = true;
        }
        if (was_running && !g_running) {  // (a run just ended)
            refresh_status();
            flash(g_result == 1 ? "All done: the PC version is built. Press FIGHT! on the PLAY page."
                  : g_result == 3 ? "Cancelled." : "A step failed: see the log for why.");
            if (g_result == 1) tab = 0;
            if (play_after && g_result == 1 && g_status.built) {  // (--play: rebuilt, so the game now, waited for)
                SDL_HideWindow(win);
                std::string e = launch_game(&exit_code);
                if (!e.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ARMS Rearmed", e.c_str(), NULL), exit_code = 1;
                quit = true;
            }
            play_after = false;
        }
        was_running = g_running;
        if (SDL_GetTicks() - last_status > 2000 && !g_running) { refresh_status(); last_status = SDL_GetTicks(); }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImVec2 size = io.DisplaySize;
        float t = (float)ImGui::GetTime();
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        background(bg, size, t);

        // header: a black slanted banner with the title
        float hh = 118 * g_scale;
        slab(bg, ImVec2(-60 * g_scale, 22 * g_scale), ImVec2(size.x * 0.62f, 22 * g_scale + hh), C_BLACK, 28 * g_scale);
        slant_text_outlined(bg, f_title, f_title->FontSize, ImVec2(42 * g_scale, 26 * g_scale), C_YELLOW, C_BLACK, "ARMS REARMED", 2);
        slant_text(bg, f_body, f_body->FontSize, ImVec2(52 * g_scale, 22 * g_scale + hh - 32 * g_scale), C_CYAN,
                   "PC LAUNCHER  -  RECOMPILED, NATIVE, 60 FPS");

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("##root", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(f_body);
        // tabs: slanted buttons under the header
        float tx = 40 * g_scale, ty = 22 * g_scale + hh + 18 * g_scale, th = 44 * g_scale;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 5; i++) {
            ImVec2 ts = f_body->CalcTextSizeA(f_body->FontSize, FLT_MAX, 0, tabs[i]);
            float tw = ts.x + 54 * g_scale;
            ImGui::SetCursorScreenPos(ImVec2(tx, ty));
            char id[16];
            snprintf(id, sizeof id, "##tab%d", i);
            if (ImGui::InvisibleButton(id, ImVec2(tw, th))) tab = i;
            bool hover = ImGui::IsItemHovered(), on = tab == i;
            slab(dl, ImVec2(tx + 4 * g_scale, ty + 4 * g_scale), ImVec2(tx + tw + 4 * g_scale, ty + th + 4 * g_scale), C_BLACK, 10 * g_scale);
            slab(dl, ImVec2(tx, ty), ImVec2(tx + tw, ty + th), on ? C_PINK : hover ? C_CYAN : C_BLACK, 10 * g_scale);
            slant_text(dl, f_body, f_body->FontSize, ImVec2(tx + 27 * g_scale - 6 * g_scale, ty + (th - ts.y) / 2),
                       on ? C_WHITE : hover ? C_BLACK : C_YELLOW, tabs[i], 0.18f);
            tx += tw + 14 * g_scale;
        }
        // the page: a dark card
        ImVec2 pa(32 * g_scale, ty + th + 22 * g_scale), pb(size.x - 32 * g_scale, size.y - 64 * g_scale);
        dl->AddRectFilled(ImVec2(pa.x + 8 * g_scale, pa.y + 8 * g_scale), ImVec2(pb.x + 8 * g_scale, pb.y + 8 * g_scale), C_BLACK, 16 * g_scale);
        dl->AddRectFilled(pa, pb, C_INK, 16 * g_scale);
        dl->AddRectFilled(pa, ImVec2(pb.x, pa.y + 6 * g_scale), tab == 0 ? C_PINK : tab == 1 ? C_LIME : tab == 2 ? C_CYAN
                          : tab == 3 ? C_ORANGE : C_YELLOW, 16 * g_scale, ImDrawFlags_RoundCornersTop);
        ImGui::SetCursorScreenPos(ImVec2(pa.x + 28 * g_scale, pa.y + 22 * g_scale));
        ImGui::BeginChild("##page", ImVec2(pb.x - pa.x - 56 * g_scale, pb.y - pa.y - 40 * g_scale), ImGuiChildFlags_None,
                          tab == 1 ? ImGuiWindowFlags_None : ImGuiWindowFlags_None);
        switch (tab) {
        case 0: page_play(); break;
        case 1: page_setup(win); break;
        case 2: page_graphics(); break;
        case 3: page_controls(); break;
        default: page_about(); break;
        }
        ImGui::EndChild();
        // the bottom bar: status line, Save, Quit
        float by = size.y - 50 * g_scale;
        ImGui::SetCursorScreenPos(ImVec2(40 * g_scale, by + 10 * g_scale));
        if (g_running) {
            int done = 0, n = 0;
            for (auto &st : g_steps) if (st.state != ST_SKIP) { n++; if (st.state == ST_OK) done++; }
            char pr[96];
            snprintf(pr, sizeof pr, "Working: step %d of %d", std::min(done + 1, n), n);
            ImGui::TextColored(V(C_YELLOW), "%s", pr);
        } else if (!g_status_msg.empty() && ImGui::GetTime() < g_status_until) {
            ImGui::TextColored(V(C_YELLOW), "%s", g_status_msg.c_str());
        } else {
            ImGui::TextColored(V(C_GREY), "Settings: %slarecomp.ini", g_root.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(size.x - 300 * g_scale, by + 4 * g_scale));
        if (ImGui::Button("Save settings", ImVec2(150 * g_scale, 0)))
            flash(settings_to_ini() ? "Settings saved." : "Couldn't write larecomp.ini.");
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(110 * g_scale, 0))) quit = true;
        ImGui::PopFont();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 255, 225, 10, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        {   // (test) ARMS_LAUNCHER_SHOT=<file.bmp>: the window after a few frames, then quit
            static int frames;
            const char *shot = getenv("ARMS_LAUNCHER_SHOT");
            if (shot && ++frames == 8) {
                if (SDL_Surface *s = SDL_RenderReadPixels(ren, NULL)) SDL_SaveBMP(s, shot), SDL_DestroySurface(s);
                quit = true;
            }
        }
        SDL_RenderPresent(ren);
    }
    if (g_running) cancel_steps();
    if (g_worker.joinable()) g_worker.join();
    settings_to_ini();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return exit_code;
}

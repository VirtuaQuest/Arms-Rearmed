# ARMS Rearmed build (Windows: MSYS2 UCRT64 clang + Vulkan, as LAS Recomp's Windows build).
#   make gen   recompile extracted/exefs (your own game files) into generated/
#   make -j    build/armsrecomp.exe and the game code library build/armsgame.dll
# From an MSYS2 UCRT64 shell, or Git Bash with: export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
comma := ,
B        ?= build
GEN      ?= generated
CC       := clang
CXX      := clang++
IMGUI    := third_party/imgui
PY       := .venv/Scripts/python.exe

# The release's version (its zip's name, the log's [info] line). RELEASE=1: the build `make dist` ships (no console
# window - the log goes to armsrecomp.log next to the exe - no debug info, the original icon, DEPS' LGPL FFmpeg).
VERSION  := 0.1.0
RELEASE  ?=
DEPS     ?=

OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
            $(if $(DEPS),-I$(DEPS)/include)
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h)
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime \
            -D_FILE_OFFSET_BITS=64 $(GENARCH)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(OSFLAGS)
LDFLAGS  := -fuse-ld=lld $(if $(DEPS),-L$(DEPS)/lib) -Wl,--stack,16777216 -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows -Wl$(comma)--strip-debug) \
            -lvulkan-1 -lws2_32 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            -Wl,--error-limit=0 -Wl,--export-all-symbols -Wl,--out-implib,$(B)/armsrecomp.dll.a

EXE      := $(B)/armsrecomp.exe
GAMELIB  := $(B)/armsgame.dll
MODULES  := game sdk

GEN_SRC  := $(strip $(foreach m,$(MODULES),$(wildcard $(GEN)/$(m)/*.c)))
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(B)/obj/%.o,$(GEN_SRC)) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/windows.c
RT_HDRS  := $(wildcard runtime/*.h runtime/platform/*.h runtime/nvn/*.h runtime/nvn/maxwell/*.h \
                       runtime/nvn/vulkan/*.h runtime/ui/*.h)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
UI_SRC   := $(wildcard runtime/ui/*.mm)
RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o $(B)/imgui/imgui_impl_vulkan.o

# what the game code library is built from: a library with another id is never loaded (runtime/gamelib.h)
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/simd_c.py \
                tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h runtime/simd_fp.h \
                runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)
GAMELIB_BUILD := $(shell od -An -tx1 -j64 -N8 extracted/exefs/main 2>/dev/null | tr -d ' \n' | tr a-f A-F)
GAMELIB_FULL_ID := $(GAMELIB_ID)-$(or $(GAMELIB_BUILD),unknown)

# the icon: a source build uses the game's own from your dump (extracted/control); the release, the original one in
# docs/icon (tools/make_original_icon.py) - nothing of the game is shipped
ICON_SRC := $(if $(RELEASE),,$(firstword $(wildcard extracted/control/icon_AmericanEnglish.dat extracted/control/icon_*.dat)))
ICON_ICO := $(if $(ICON_SRC),$(B)/icon.ico,$(if $(RELEASE),docs/icon/icon.ico))
ICON_RES := $(if $(ICON_ICO),$(B)/icon_res.o)

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

# ---- the game code: generated C from the game's own NSOs (never committed)
gen:
	cd tools && ../$(PY) gen.py game ../extracted/exefs/main 0x7000000000 ../$(GEN)/game --simd c
	cd tools && ../$(PY) gen.py sdk ../extracted/exefs/sdk 0x7006000000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt --simd c

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_FULL_ID)"' -c $< -o $@

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) -shared -fuse-ld=lld $(B)/armsrecomp.dll.a -o $@.tmp $^ $(B)/armsrecomp.dll.a
	mv -f $@.tmp $@
	echo $(GAMELIB_FULL_ID) > $@.id

# ---- the runtime
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/hle/game_hooks.o: RTFLAGS += -DARMS_VERSION='"$(VERSION)$(if $(RELEASE),,-dev)"'
$(B)/rt/hle/game_hooks.o: Makefile

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/inc/shader_variants.inc: runtime/nvn/shader_variants.txt
	@mkdir -p $(dir $@)
	(LC_ALL=C tr -d '\r' < $< | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/'; echo '""') > $@
$(B)/rt/nvn/shader_prep.o: $(B)/inc/shader_variants.inc
TRANSLATOR_ID := $(shell cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | sha256sum | cut -c1-12)
$(B)/rt/nvn/shader_prep.o: RTFLAGS += -I$(B)/inc -DSHADER_TRANSLATOR_ID='"$(TRANSLATOR_ID)"'

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ) $(ICON_RES)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# ---- the icon (the window's and the executables'): from the game's own icon in extracted/control (your dump), when
# it's there - nothing of the game is committed
$(B)/icon.png $(B)/icon.ico &: $(ICON_SRC) tools/make_icon.py
	@mkdir -p $(B)
	$(PY) -I tools/make_icon.py extracted/control $(B)
$(B)/icon_res.o: $(ICON_ICO)
	@mkdir -p $(B)
	printf '1 ICON "%s"\n' "$(ICON_ICO)" > $(B)/icon.rc
	windres -i $(B)/icon.rc -o $@

# ---- the launcher (launcher/launcher.cpp): the display and frame rate settings, then the game
LAUNCHER := $(B)/armslauncher.exe
LAUNCHER_IMGUI := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
                  $(B)/imgui/imgui_impl_sdl3.o $(B)/imgui/imgui_impl_sdlrenderer3.o
$(B)/launcher/launcher.o: launcher/launcher.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(LAUNCHER): $(B)/launcher/launcher.o $(LAUNCHER_IMGUI) $(ICON_RES) $(if $(ICON_SRC),$(B)/icon.png)
	$(CXX) -o $@ $(filter %.o,$^) -fuse-ld=lld -mwindows -lSDL3 $(if $(RELEASE),-Wl$(comma)--strip-debug)
launcher: $(LAUNCHER)
all: $(LAUNCHER)

# ---- the MSYS2 runtime DLLs the game and the launcher need, copied next to them, so they start outside an MSYS2 shell
# (from Explorer or the launcher): whatever ldd finds under /ucrt64 (SDL3, FFmpeg, shaderc, libstdc++...)
DLL_STAMP := $(B)/.dlls
$(DLL_STAMP): $(wildcard $(EXE) $(LAUNCHER))
	@for f in $$(ldd $^ | grep -io '/ucrt64/bin/[^ ]*\.dll' | sort -u); do cp -u "$$f" $(B)/; done
	@echo "runtime DLLs copied into $(B)/"
	@touch $@
dlls: $(EXE) $(LAUNCHER)
	@rm -f $(DLL_STAMP)
	@$(MAKE) --no-print-directory $(DLL_STAMP)
all: $(DLL_STAMP)

# ---- the release: build/dist/ARMS-Rearmed-v<VERSION>-Windows-x64.zip, top folder "ARMS Rearmed/", for players and
# RecompLauncher (recomplauncher.json). It holds no game code and no game files: the launcher's prepare step
# (armslauncher --prepare) extracts the player's own game, recompiles it with armsgen and builds the game code
# library with zig on their machine. Needs, once:
#   make deps-windows       an LGPL-only FFmpeg (tools/build_deps_windows.sh; MSYS2's is a GPL build)
#   make gen-helper         armsgen: tools/nsp_extract.py + tools/gen.py frozen by PyInstaller (no Python needed)
#   tools/fetch_zig.sh      the pinned zig (the C compiler for the player's machine), slimmed by tools/zig_slim.sh
DIST     := build/dist/ARMS Rearmed
DIST_ZIP := ARMS-Rearmed-v$(VERSION)-Windows-x64.zip
WIN_B    := build/win-release
GEN_PY   ?= py -V:3.12
GEN_VPY  := build/gen-venv/Scripts/python.exe

deps-windows:
	tools/build_deps_windows.sh

gen-helper: build/gen-venv/.ok
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/armsgen.spec
	$(GEN_VPY) tools/pe_no_cfg.py build/helper/armsgen/armsgen.exe
	@du -sh build/helper/armsgen
build/gen-venv/.ok:
	$(GEN_PY) -m venv build/gen-venv
	$(GEN_VPY) -m pip install -q unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pyinstaller==6.22.3
	touch $@

dist:
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -f build/helper/armsgen/armsgen.exe ] || { echo "make gen-helper first (the extractor and generator)"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 GEN=build/no-generated-code \
	    $(WIN_B)/armsrecomp.exe $(WIN_B)/armslauncher.exe
	! nm $(WIN_B)/armsrecomp.exe | grep -E ' f_70[0-9a-f]{8}' >/dev/null  # (no game code inside)
	tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/armsrecomp.exe $(WIN_B)/armsrecomp.dll.a $(WIN_B)/armslauncher.exe "$(DIST)/"
	cp -R build/helper/armsgen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	printf '%s\n%s\n' '$(GAMELIB_ID)' '$(strip $(filter-out -Iruntime,$(GENFLAGS)))' > "$(DIST)/gamelib-src/gamelib.id"
	cp docs/icon/icon.png "$(DIST)/icon.png"
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$(DIST)/"
	cp third_party/fonts/Baloo2/OFL.txt "$(DIST)/Baloo2-OFL.txt"
	cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$(DIST)/"
	cp third_party/fonts/DejaVu/LICENSE "$(DIST)/DejaVu-LICENSE.txt"
	cp third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf "$(DIST)/"
	cp third_party/fonts/BIZUDPGothic/OFL.txt "$(DIST)/BIZUDPGothic-OFL.txt"
	cp README.md LICENSE.LASRecomp recomplauncher.json "$(DIST)/"
	cp docs/THIRD_PARTY.md "$(DIST)/THIRD_PARTY.txt"
	PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/armsrecomp.exe $(WIN_B)/armslauncher.exe | \
	    awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}' | sort -u | xargs -I{} cp -u {} "$(DIST)/"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc|libssl|gnutls'  # (no GPL codec libraries in the release)
	# (no game files, generated code or game code library; no path of this machine inside)
	! find "$(DIST)" \( -name extracted -o -name 'generated*' -o -name exefs -o -name romfs -o -name 'armsgame*' \
	    -o -name '*.nso' -o -name '*.nca' -o -name '*.keys' -o -name 'launcher.ini' -o -name 'larecomp.ini' \) | grep .
	h="$${USERPROFILE:-C:\\Users\\$${USERNAME:-$$USER}}"; \
	    ! grep -rlaiF -e "$(CURDIR)" -e "$$(cygpath -m '$(CURDIR)')" -e "$$(cygpath -w '$(CURDIR)')" \
	    -e "$$(cygpath -w "$$h")" -e "$$(cygpath -m "$$h")" "$(DIST)"
	cd build/dist && rm -f "$(DIST_ZIP)" && zip -qr9 "$(DIST_ZIP)" "ARMS Rearmed"
	@du -sh "build/dist/$(DIST_ZIP)"

runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

clean:
	rm -rf $(B)

.PHONY: all gen runtime-objs clean launcher dlls deps-windows gen-helper dist

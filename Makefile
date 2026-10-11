# Thin developer wrapper over CMake, Ninja and CTest. All build logic lives in CMakeLists.txt.
# Run `make help` for targets and variables.

GAME       ?= commandw
ROM_DIR    ?= $(CURDIR)/roms/$(GAME)
BUILD_TYPE ?= Release
BUILD_DIR  ?= build-$(GAME)
GENERATOR  ?= Ninja
CMAKE_ARGS ?=
FRAMES     ?= 600
JOBS       ?=

.DEFAULT_GOAL := help
.PHONY: help list-games rom-check configure build test run smoke headless clean distclean

# Single-quote a value for the shell so paths with spaces or quotes survive.
shq = '$(subst ','\'',$(1))'

# GNU make predefines CC and CXX (cc, g++); forward them only when set explicitly.
CC_ARG   = $(if $(filter-out default undefined,$(origin CC)),-DCMAKE_C_COMPILER=$(call shq,$(CC)))
CXX_ARG  = $(if $(filter-out default undefined,$(origin CXX)),-DCMAKE_CXX_COMPILER=$(call shq,$(CXX)))
JOBS_ARG = $(if $(JOBS),-j $(call shq,$(JOBS)))
# Source of truth: F3_GAME_EXECUTABLE in CMakeLists.txt, published to the cache.
# Recursive, so it is read when a recipe runs, after configure has refreshed the cache.
EXE_NAME = $(shell sed -n 's/^F3_GAME_EXECUTABLE:INTERNAL=//p' $(call shq,$(BUILD_DIR)/CMakeCache.txt) 2>/dev/null)
EXE      = $(BUILD_DIR)/$(EXE_NAME)
need_exe = test -x $(call shq,$(EXE)) || { printf 'error: %s was not built; check that ROM_DIR=%s holds the %s ROM set\n' $(call shq,$(EXE)) $(call shq,$(ROM_DIR)) $(call shq,$(GAME)) >&2; exit 1; }

# Set per target by `headless` (target-specific variables reach its prerequisites).
CONFIG_EXTRA =

help:
	@echo "Usage: make <target> [VAR=value ...]   (example: make build GAME=bubblemj)"
	@echo ""
	@echo "Targets:"
	@echo "  configure   cmake configure into BUILD_DIR (fails early if ROM_DIR is missing)"
	@echo "  build       configure, then cmake --build"
	@echo "  test        build, then ctest --output-on-failure"
	@echo "  run         build, then run the game executable windowed"
	@echo "  smoke       build, then headless run for FRAMES frames"
	@echo "  headless    configure, build, test with F3RT_SDL=OFF F3RT_GPU=OFF in build-GAME-headless"
	@echo "  clean       cmake --build --target clean"
	@echo "  distclean   rm -rf BUILD_DIR (only if it holds a CMakeCache.txt)"
	@echo "  list-games  ROM sets under roms/ that have games/SET/config.toml"
	@echo "  help        this table"
	@echo ""
	@echo "Variables (override on the command line):"
	@echo "  GAME        ROM set / game id                     (commandw)"
	@echo "  ROM_DIR     directory holding the set's ROM files (roms/GAME)"
	@echo "  BUILD_TYPE  CMAKE_BUILD_TYPE                      (Release)"
	@echo "  BUILD_DIR   build directory                       (build-GAME)"
	@echo "  GENERATOR   CMake generator                       (Ninja)"
	@echo "  CMAKE_ARGS  extra raw arguments for configure     (empty)"
	@echo "  CC, CXX     compilers, forwarded to CMake when set"
	@echo "  JOBS        parallel jobs for build and test      (CMake default)"
	@echo "  FRAMES      frames for smoke                      (600)"

list-games:
	@for d in roms/*/; do s=$${d#roms/}; s=$${s%/}; if [ -f games/$$s/config.toml ]; then echo "$$s"; fi; done

rom-check:
	@test -d $(call shq,$(ROM_DIR)) || { printf 'error: ROM directory %s does not exist (GAME=%s). Set ROM_DIR=/path/to/roms/%s, or pick a set from make list-games.\n' $(call shq,$(ROM_DIR)) $(call shq,$(GAME)) $(call shq,$(GAME)) >&2; exit 1; }

configure: rom-check
	cmake -S . -B $(call shq,$(BUILD_DIR)) -G $(call shq,$(GENERATOR)) \
		-DCMAKE_BUILD_TYPE=$(call shq,$(BUILD_TYPE)) \
		-DF3_GAME=$(call shq,$(GAME)) \
		-DF3_ROM_DIR=$(call shq,$(ROM_DIR)) \
		$(CC_ARG) $(CXX_ARG) $(CONFIG_EXTRA) $(CMAKE_ARGS)

build: configure
	cmake --build $(call shq,$(BUILD_DIR)) $(JOBS_ARG)

test: build
	ctest --test-dir $(call shq,$(BUILD_DIR)) --output-on-failure $(JOBS_ARG)

run: build
	@$(need_exe)
	$(call shq,$(EXE))

smoke: build
	@$(need_exe)
	$(call shq,$(EXE)) --frames $(call shq,$(FRAMES)) --headless

headless: BUILD_DIR = build-$(GAME)-headless
headless: CONFIG_EXTRA = -DF3RT_SDL=OFF -DF3RT_GPU=OFF
headless: build test

clean:
	cmake --build $(call shq,$(BUILD_DIR)) --target clean

distclean:
	@test -f $(call shq,$(BUILD_DIR))/CMakeCache.txt || { printf 'error: %s has no CMakeCache.txt; refusing to remove it\n' $(call shq,$(BUILD_DIR)) >&2; exit 1; }
	rm -rf -- $(call shq,$(BUILD_DIR))

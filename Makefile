# jami-bridge Makefile — convenient targets for common operations.
#
# For container builds (Linux):
#   make dev           — incremental build in dev container
#   make dev-run       — run dev binary in runtime container
#   make dev-dist      — bundle into dist tarball
#   make dist          — production dist image
#   make test-dist     — test dist in minimal container
#
# For native builds (macOS / host with libjami):
#   make native        — cmake configure + build
#   make native-dist   — bundle into portable dist tarball
#   make run           — run the native binary
#
# Cleanup:
#   make clean         — remove build artifacts
#   make clean-all     — remove everything (containers, images, build dir)

.PHONY: native native-dist native-configure native-clean run
.PHONY: dev dev-run dev-dist dev-shell dev-kill dev-clean
.PHONY: base bridge dist test-dist check
.PHONY: clean clean-all help

BUILD_DIR := build
DIST_DIR   := jami-bridge-dist-output

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ═══════════════════════════════════════════════════════════════════════
# Native builds — no containers (macOS or host with libjami)
# ═══════════════════════════════════════════════════════════════════════

native-configure:
	@mkdir -p $(BUILD_DIR)
	@if [ -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		echo "=== CMake cache exists, reconfiguring ==="; \
		rm -f $(BUILD_DIR)/CMakeCache.txt; \
	fi
	cd $(BUILD_DIR) && cmake ..

native: native-configure
	cd $(BUILD_DIR) && make -j$(NPROC)
	@echo "=== Build successful: $(BUILD_DIR)/jami-bridge ==="

native-dist: native
	./build.sh native-dist

native-clean:
	rm -rf $(BUILD_DIR)

run: native
	./$(BUILD_DIR)/jami-bridge $(ARGS)

# ═══════════════════════════════════════════════════════════════════════
# Container builds — Podman (Linux)
# ═══════════════════════════════════════════════════════════════════════

base:
	./build.sh base

check:
	./build.sh check

bridge:
	./build.sh bridge

dev:
	./build.sh dev

dev-run:
	./build.sh dev-run $(ARGS)

dev-dist:
	./build.sh dev-dist

dev-shell:
	./build.sh dev-shell

dev-kill:
	./build.sh dev-kill

dev-clean:
	./build.sh dev-clean

dist: bridge
	./build.sh dist

test-dist: bridge
	./build.sh test-dist

# ═══════════════════════════════════════════════════════════════════════
# Cleanup
# ═══════════════════════════════════════════════════════════════════════

clean: native-clean
	./build.sh clean

clean-all: native-clean
	./build.sh clean-all

# ═══════════════════════════════════════════════════════════════════════
# Help
# ═══════════════════════════════════════════════════════════════════════

help:
	@echo "jami-bridge Makefile"
	@echo ""
	@echo "Native builds (macOS / host with libjami):"
	@echo "  make native         Build with cmake + make (no containers)"
	@echo "  make native-dist    Bundle into portable dist tarball"
	@echo "  make run            Build and run (pass ARGS='--stdio --debug' etc.)"
	@echo "  make native-clean   Remove build directory"
	@echo ""
	@echo "Container builds (Linux, Podman):"
	@echo "  make base           Build base image (daemon, ~10-20min)"
	@echo "  make dev            Incremental build in dev container"
	@echo "  make dev-run        Run dev binary in runtime container"
	@echo "  make dev-dist       Bundle dev binary into dist tarball"
	@echo "  make dev-shell      Shell into dev container"
	@echo "  make dev-kill       Stop dev container"
	@echo "  make dev-clean      Remove build dir + dev container"
	@echo "  make dist           Production dist image"
	@echo "  make test-dist      Test dist in minimal container"
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean          Remove build artifacts + images"
	@echo "  make clean-all      Remove EVERYTHING (expensive!)"
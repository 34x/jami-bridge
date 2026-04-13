#!/bin/bash
# Build jami-bridge in containers. All compilation happens inside containers.
#
# Production builds (full image rebuild):
#   ./build.sh base        — Build base image (daemon from source, ~4GB, slow)
#   ./build.sh bridge         — Build bridge production image
#   ./build.sh dist        — Build self-contained dist tarball
#   ./build.sh test-dist   — Test dist in fresh container
#   ./build.sh all         — base → bridge
#   ./build.sh clean       — Remove images
#
# Development builds (fast, incremental, source mounted):
#   ./build.sh dev         — Build or rebuild bridge in dev container (fast!)
#   ./build.sh dev-run     — Run the dev binary in a fresh runtime container
#   ./build.sh dev-shell   — Open a shell in the dev container
#   ./build.sh dev-clean   — Remove dev build directory
#   ./build.sh dev-kill    — Stop and remove the dev container
#
# The dev workflow keeps a persistent container running. Source is mounted
# from the host, so only the bridge binary is rebuilt — not the 4GB daemon.
# Use 'dev-run' to test the binary in a clean runtime container — no
# image rebuild or dist packaging needed.

set -e

# Detect OS for platform-specific operations
OS_NAME="$(uname -s)"
case "$OS_NAME" in
    Linux)  PLATFORM="linux" ;;
    Darwin) PLATFORM="macos" ;;
    *)      PLATFORM="unknown" ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Build context for Containerfile.base: the bridge repo root (contains daemon/ submodule)
# When jami-bridge is its own repo, daemon/ is a git submodule here.
BUILD_CONTEXT="$SCRIPT_DIR"
IMAGE_BASE="jami-bridge-base"
IMAGE_BRIDGE="jami-bridge"
IMAGE_DEV="jami-bridge-dev"
IMAGE_RUNTIME="jami-bridge-runtime"
DEV_CONTAINER="jami-bridge-dev"
BRIDGE_SOURCE="$SCRIPT_DIR"  # jami-bridge/ directory on host
BUILD_DIR="$BRIDGE_SOURCE/build"  # build/ on host, mounted into container

# ── Native build (no containers) ─────────────────────────────────────────
#
# For macOS or any host that has libjami installed. Builds directly on the
# host — no Podman/Docker needed.
#
# Prerequisites:
#   - cmake, make, C++17 compiler
#   - libjami (built with -Dinterfaces=library) installed
#   - Set JAMI_INCLUDE_DIR and JAMI_LIBRARY if not in standard paths
#
# Usage:
#   ./build.sh native
#   ./build.sh native-build              # cmake configure + make
#   ./build.sh native-dist               # bundle libs into dist/

native_build() {
    local build_dir="$SCRIPT_DIR/build"
    mkdir -p "$build_dir"

    echo "=== Native build (PLATFORM=$PLATFORM) ==="

    # Configure with cmake
    echo "    Configuring..."
    cd "$build_dir"
    cmake .. "$@"

    # Build
    echo "    Compiling..."
    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    cd "$SCRIPT_DIR"

    # Verify
    if [ -f "$build_dir/jami-bridge" ]; then
        local size
        size=$(du -h "$build_dir/jami-bridge" | cut -f1)
        echo "=== Build successful: $build_dir/jami-bridge ($size) ==="
    else
        echo "ERROR: Binary not found at $build_dir/jami-bridge"
        exit 1
    fi
}

native_dist() {
    local build_dir="$SCRIPT_DIR/build"
    local binary="$build_dir/jami-bridge"

    if [ ! -f "$binary" ]; then
        echo "ERROR: No binary found. Run './build.sh native' first."
        exit 1
    fi

    local dist_dir="$SCRIPT_DIR/jami-bridge-dist-output/jami-bridge-dist"
    mkdir -p "$dist_dir/lib"

    echo "=== Creating native dist (PLATFORM=$PLATFORM) ==="

    # Copy binary
    cp "$binary" "$dist_dir/jami-bridge"
    chmod +x "$dist_dir/jami-bridge"

    if [ "$PLATFORM" = "macos" ]; then
        native_dist_macos "$dist_dir"
    else
        native_dist_linux "$dist_dir"
    fi

    echo "=== Dist contents ==="
    du -sh "$dist_dir"
    ls -lh "$dist_dir/"
    echo "=== Bundled libraries ==="
    ls -lh "$dist_dir/lib/"

    # Create tarball
    cd "$SCRIPT_DIR/jami-bridge-dist-output"
    tar czf jami-bridge-dist.tar.gz jami-bridge-dist/
    echo "=== Tarball: $(ls -lh jami-bridge-dist.tar.gz | cut -f5) ==="
    echo "=== Distribution created at: $SCRIPT_DIR/jami-bridge-dist-output/ ==="
}

native_dist_macos() {
    local dist_dir="$1"
    echo "=== Bundling libraries (macOS — otool/install_name_tool) ==="

    # Find all linked libraries using otool
    # macOS: otool -L shows linked libraries (equivalent of ldd)
    # Bundle all except system libraries (/usr/lib, /System, @rpath system)
    changed=1
    while [ "$changed" -ne 0 ]; do
        changed=0
        for binary_path in $(
            DYLD_LIBRARY_PATH="$dist_dir/lib" otool -L "$dist_dir/jami-bridge" "$dist_dir"/lib/*.dylib 2>/dev/null \
            | grep -E '^\t/' \
            | awk '{print $1}' \
            | sort -u
        ); do
            lib_name=$(basename "$binary_path")
            # Skip system libraries
            case "$binary_path" in
                /usr/lib/*|/System/*|@rpath/*|@loader_path/*|@executable_path/*)
                    continue ;;
            esac
            # Skip if already bundled
            [ -f "$dist_dir/lib/$lib_name" ] && continue
            # Skip if not a .dylib or .so
            case "$lib_name" in
                *.dylib|*.so*) ;;
                *) continue ;;
            esac
            # Resolve symlink and copy
            cp -L "$binary_path" "$dist_dir/lib/$lib_name" && echo "  bundled: $lib_name" && changed=1 || echo "  FAILED: $lib_name"
        done
    done

    # Fix install names: change @rpath/libfoo.dylib → @loader_path/libfoo.dylib
    # This makes each lib find its neighbors in the same lib/ directory.
    echo "=== Patching install names ==="
    for so in "$dist_dir/lib"/*.dylib "$dist_dir/lib"/*.so*; do
        [ -e "$so" ] || continue
        [ -L "$so" ] && continue
        local lib_name
        lib_name=$(basename "$so")
        # Set the library's own ID to @loader_path/libfoo.dylib
        install_name_tool -id "@loader_path/$lib_name" "$so" 2>/dev/null || true
        # Change @rpath references to @loader_path
        for ref in $(otool -L "$so" | awk '{print $1}' | grep '@rpath/'); do
            local ref_name
            ref_name=$(basename "$ref")
            # Only rewrite refs to libs we've bundled
            if [ -f "$dist_dir/lib/$ref_name" ]; then
                install_name_tool -change "$ref" "@loader_path/$ref_name" "$so" 2>/dev/null || true
            fi
        done
    done

    # Also fix the main binary
    for ref in $(otool -L "$dist_dir/jami-bridge" | awk '{print $1}' | grep '@rpath/'); do
        local ref_name
        ref_name=$(basename "$ref")
        if [ -f "$dist_dir/lib/$ref_name" ]; then
            install_name_tool -change "$ref" "@loader_path/$ref_name" "$dist_dir/jami-bridge" 2>/dev/null || true
        fi
    done

    # Create symlinks for versioned libs (libfoo.1.dylib → libfoo.dylib)
    cd "$dist_dir/lib"
    for dylib in *.dylib.*; do
        [ -e "$dylib" ] || continue
        [ -L "$dylib" ] && continue
        # e.g. libfoo.1.2.3.dylib → libfoo.1.dylib
        local base
        base=$(echo "$dylib" | sed -E 's/\.[0-9]+\.dylib$/.dylib/')
        [ "$base" != "$dylib" ] && [ ! -e "$base" ] && ln -sf "$dylib" "$base" 2>/dev/null || true
    done

    # Verify
    echo "=== Verifying library resolution ==="
    DYLD_LIBRARY_PATH="$dist_dir/lib" otool -L "$dist_dir/jami-bridge" | grep -v '/usr/lib\|/System\|@loader_path' | head -5 || true
    echo "Done."
}

native_dist_linux() {
    local dist_dir="$1"
    echo "=== Bundling libraries (Linux — ldd/patchelf) ==="

    # Copy libjami from wherever it was found
    # (first check if it's already in lib/ or in the build dir)
    local jami_lib
    jami_lib=$(ldd "$dist_dir/jami-bridge" 2>/dev/null | grep libjami | awk '{print $3}' | head -1)
    if [ -n "$jami_lib" ] && [ -f "$jami_lib" ]; then
        cp -L "$jami_lib" "$dist_dir/lib/" && echo "  copied: $(basename $jami_lib)"
    fi

    # Bundle ALL non-glibc shared libraries (iterative)
    changed=1
    while [ "$changed" -ne 0 ]; do
        changed=0
        for lib_path in $(
            LD_LIBRARY_PATH="$dist_dir/lib" ldd "$dist_dir/jami-bridge" "$dist_dir"/lib/*.so.* 2>/dev/null \
            | grep -oP '/[^\s:]+' \
            | sort -u
        ); do
            lib_name=$(basename "$lib_path")
            case "$lib_name" in
                ld-linux-x86-64.so.*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|linux-vdso.so.*)
                    continue ;;
            esac
            [ -f "$dist_dir/lib/$lib_name" ] && continue
            case "$lib_name" in *.so*) ;; *) continue ;; esac
            cp -L "$lib_path" "$dist_dir/lib/$lib_name" && echo "  bundled: $lib_name" && changed=1 || echo "  FAILED: $lib_name"
        done
    done

    # Patch RUNPATH on all bundled libs
    echo "=== Patching RUNPATH ==="
    for so in "$dist_dir/lib"/*.so*; do
        [ -L "$so" ] && continue
        patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
    done

    # Create symlinks for versioned libs
    cd "$dist_dir/lib"
    for so in *.so.*.*; do
        [ -L "$so" ] && continue
        major="${so%.so.*}.so.$(echo "$so" | sed 's/.*\.so\.//; s/\..*//')"
        [ "$major" != "$so" ] && ln -sf "$so" "$major" 2>/dev/null
    done
    for so in *.so.*; do
        [ -L "$so" ] && continue
        base="${so%.so.*}.so"
        [ ! -L "$base" ] && [ ! -e "$base" ] && ln -sf "$so" "$base" 2>/dev/null || true
    done

    # Verify
    echo "=== Verifying library resolution ==="
    LD_LIBRARY_PATH="$dist_dir/lib" ldd "$dist_dir/jami-bridge" | grep "not found" && echo "ERROR: Missing libs!" && exit 1 || echo "All libs resolved."
}

build_base() {
    echo "=== Building base image (daemon from source in library mode) ==="
    echo "    This may take 10-20 minutes on first build..."
    podman build \
        -t "$IMAGE_BASE" \
        -f "$SCRIPT_DIR/Containerfile.base" \
        "$BUILD_CONTEXT"
    echo "=== Base image built: $IMAGE_BASE ==="
}

check_symbols() {
    echo "=== Checking libjami:: symbol visibility ==="
    podman run --rm localhost/$IMAGE_BASE bash -c '
        echo "libjami files:"
        find /usr/local -name "libjami*.so*" -exec ls -lh {} \;
        echo "---"
        echo "libjami:: symbols in libjami.so (library mode):"
        nm -D /usr/local/lib64/libjami.so 2>/dev/null | c++filt | grep "libjami::" | wc -l
        echo "---"
        echo "Key API symbols:"
        nm -D /usr/local/lib64/libjami.so 2>/dev/null | c++filt | grep "libjami::\(init\|start\|fini\|sendMessage\|startConversation\|getAccountList\)" | head -10
        echo "---"
        echo "Headers installed:"
        ls /usr/local/include/jami/
    '
}

# ── Production image (COPY source in, full rebuild) ─────────────────────

build_bridge() {
    echo "=== Building bridge production image ==="
    podman build \
        -t "$IMAGE_BRIDGE" \
        -f "$SCRIPT_DIR/Containerfile" \
        "$BUILD_CONTEXT"
    echo "=== bridge image built: $IMAGE_BRIDGE ==="
}

build_dist() {
    echo "=== Building self-contained distribution ==="
    podman build \
        -t jami-bridge-dist \
        -f "$SCRIPT_DIR/Containerfile.dist" \
        "$BUILD_CONTEXT"
    echo "=== Distribution image built: jami-bridge-dist ==="
    echo ""
    echo "To extract the tarball:"
    echo "  ./build.sh dist-extract"
}

test_dist() {
    echo "=== Testing dist in fresh container ==="
    podman build \
        -t jami-bridge-test \
        -f "$SCRIPT_DIR/Containerfile.test" \
        "$BUILD_CONTEXT"
    echo "=== Dist test complete ==="
}

# ── Development builds (persistent container, source mounted) ───────────
#
# The dev workflow uses a persistent container that stays running.
# Source code is bind-mounted from the host, so:
#   - Only the bridge binary is rebuilt (not the 4GB daemon)
#   - cmake is incremental — only changed files are recompiled
#   - The output binary appears at jami-bridge/build/jami-bridge on the host
#   - You can iterate on source code and rebuild in seconds

dev_build_image() {
    echo "=== Building dev image (thin layer on base) ==="
    podman build \
        -t "$IMAGE_DEV" \
        -f "$SCRIPT_DIR/Containerfile.dev" \
        "$BUILD_CONTEXT"
    echo "=== Dev image built: $IMAGE_DEV ==="
}

dev_ensure_container() {
    # Check if dev container exists and is running
    if podman container exists "$DEV_CONTAINER" 2>/dev/null; then
        local state
        state=$(podman inspect --format '{{.State.Status}}' "$DEV_CONTAINER" 2>/dev/null || echo "")
        if [ "$state" = "running" ]; then
            return 0  # Already running
        elif [ "$state" = "exited" ] || [ "$state" = "stopped" ]; then
            podman start "$DEV_CONTAINER"
            return 0
        fi
        # Unknown state — recreate
        podman rm "$DEV_CONTAINER" 2>/dev/null || true
    fi

    # Ensure dev image exists
    if ! podman image exists localhost/$IMAGE_DEV 2>/dev/null; then
        dev_build_image
    fi

    # Ensure build directory exists on host
    mkdir -p "$BUILD_DIR"

    # Create and start the dev container with source mounted
    echo "=== Creating dev container with mounted source ==="
    podman run -d \
        --name "$DEV_CONTAINER" \
        -v "$BRIDGE_SOURCE:/build/jami-bridge:Z" \
        -v "$BUILD_DIR:/build/jami-bridge/build:Z" \
        localhost/$IMAGE_DEV \
        sleep infinity
    echo "=== Dev container started: $DEV_CONTAINER ==="
}

dev_build() {
    dev_ensure_container

    echo "=== Building bridge in dev container (incremental) ==="

    # Check if cmake has been configured
    if ! podman exec "$DEV_CONTAINER" test -f /build/jami-bridge/build/Makefile 2>/dev/null; then
        echo "    First run — configuring with cmake..."
        podman exec "$DEV_CONTAINER" bash -c \
            "cd /build/jami-bridge/build && \
             cmake .. \
                 -DJAMI_INCLUDE_DIR=/usr/local/include/jami \
                 -DJAMI_LIBRARY=/usr/local/lib64/libjami.so"
    fi

    echo "    Compiling..."
    podman exec "$DEV_CONTAINER" make -C /build/jami-bridge/build -j"$(nproc)"

    # Verify binary exists on host
    if [ -f "$BUILD_DIR/jami-bridge" ]; then
        local size
        size=$(du -h "$BUILD_DIR/jami-bridge" | cut -f1)
        echo "=== Build successful: $BUILD_DIR/jami-bridge ($size) ==="
    else
        echo "ERROR: Binary not found at $BUILD_DIR/jami-bridge"
        exit 1
    fi
}

dev_shell() {
    dev_ensure_container
    echo "=== Opening shell in dev container ==="
    echo "    Source mounted: /build/jami-bridge"
    echo "    Build dir:      /build/jami-bridge/build"
    echo "    To rebuild:     cd build && make -j\$(nproc)"
    echo ""
    podman exec -it "$DEV_CONTAINER" bash
}

# ── Runtime image (minimal Fedora + system libs, for dev-run) ───────────
#
# This is a small image (~200MB) with just the runtime dependencies.
# The bridge binary and bundled libs are mounted at runtime, so this
# image never needs rebuilding — it's just the OS + system .so files.

dev_ensure_runtime() {
    if ! podman image exists localhost/$IMAGE_RUNTIME 2>/dev/null; then
        echo "=== Building runtime image (one-time, ~200MB) ==="
        podman build -t "$IMAGE_RUNTIME" -f "$SCRIPT_DIR/Containerfile.runtime" "$SCRIPT_DIR"
    fi
}

dev_run() {
    dev_ensure_container
    dev_ensure_runtime

    # Verify the binary exists
    if [ ! -f "$BUILD_DIR/jami-bridge" ]; then
        echo "ERROR: No binary found. Run './build.sh dev' first."
        exit 1
    fi

    # Create a dist-like directory from the dev binary + base image libs
    local dev_dist="$SCRIPT_DIR/dev-dist"
    mkdir -p "$dev_dist/lib"

    # Copy the fresh binary (only ~3.5MB, fast)
    cp "$BUILD_DIR/jami-bridge" "$dev_dist/jami-bridge"
    chmod +x "$dev_dist/jami-bridge"

    # Copy bundled libs from the dev container (only needed once).
    # Bundles ALL non-glibc shared libraries for cross-distro portability.
    # Must iterate because ldd only resolves direct deps — transitive deps
    # of newly bundled libs are discovered in subsequent passes.
    if [ ! -f "$dev_dist/lib/libjami.so.16.0.0" ] || [ $(ls "$dev_dist/lib/" 2>/dev/null | wc -l) -lt 10 ]; then
        echo "=== Bundling all non-glibc libs from dev container (one-time) ==="
        podman exec "$DEV_CONTAINER" bash -c '
            set -e
            mkdir -p /tmp/dev-dist/lib

            # Copy libjami
            cp /usr/local/lib64/libjami.so.16.0.0 /tmp/dev-dist/lib/

            # Iteratively resolve ALL transitive deps.
            # Each pass discovers deps of newly bundled libs.
            changed=1
            while [ "$changed" -ne 0 ]; do
                changed=0
                for lib_path in $( \
                    LD_LIBRARY_PATH=/tmp/dev-dist/lib ldd /tmp/dev-dist/lib/*.so.* \
                    | grep -oP "/[^\s:]+" \
                    | sort -u \
                ); do \
                    lib_name=$(basename "$lib_path"); \
                    case "$lib_name" in \
                        ld-linux-x86-64.so.*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|linux-vdso.so.*) \
                            continue ;; \
                    esac; \
                    [ -f "/tmp/dev-dist/lib/$lib_name" ] && continue;
                    # Skip non-library entries (e.g. binary paths from ldd args)
                    case "$lib_name" in *.so*) ;; *) continue ;; esac; \
                    cp -L "$lib_path" "/tmp/dev-dist/lib/$lib_name" && echo "  bundled: $lib_name" && changed=1 || echo "  FAILED: $lib_name"; \
                done; \
            done
            echo "=== Bundle complete: $(ls /tmp/dev-dist/lib/*.so.* 2>/dev/null | wc -l) libraries ==="

            # Patch RUNPATH on all bundled libs
            cd /tmp/dev-dist/lib
            for so in *.so*; do
                [ -L "$so" ] && continue
                patchelf --set-rpath \$ORIGIN "$so" 2>/dev/null || true
            done

            # Create symlinks for versioned libs
            for so in *.so.*.*; do
                [ -L "$so" ] && continue
                major="${so%.so.*}.so.$(echo "$so" | sed "s/.*\.so\.//; s/\..*//")"
                [ "$major" != "$so" ] && ln -sf "$so" "$major" 2>/dev/null
            done
            for so in *.so.*; do
                [ -L "$so" ] && continue
                base="${so%.so.*}.so"
                [ ! -L "$base" ] && [ ! -e "$base" ] && ln -sf "$so" "$base" 2>/dev/null || true
            done
        '
        podman cp "$DEV_CONTAINER":/tmp/dev-dist/lib/. "$dev_dist/lib/"
        echo "=== All libs bundled ==="
    fi

    # Run the dev binary in a fresh runtime container with the dist mounted
    # Pass any extra args to jami-bridge
    # Parse --port from args for port mapping (default 8090)
    local run_port=8090
    for arg in "$@"; do
        if [[ "$prev" == "--port" ]] || [[ "$arg" == --port=* ]]; then
            run_port="${arg#--port=}"
        fi
        prev="$arg"
    done

    echo "=== Running dev binary in runtime container (port $run_port) ==="
    podman run --rm \
        -p "$run_port:$run_port" \
        -v "$dev_dist:/opt/jami-bridge:Z" \
        -e LD_LIBRARY_PATH=/opt/jami-bridge/lib \
        localhost/$IMAGE_RUNTIME \
        /opt/jami-bridge/jami-bridge "${@:- --host 0.0.0.0 --port 8090}"
}

dev_kill() {
    if podman container exists "$DEV_CONTAINER" 2>/dev/null; then
        podman rm -f "$DEV_CONTAINER" 2>/dev/null || true
        echo "=== Dev container removed ==="
    else
        echo "=== No dev container to remove ==="
    fi
}

dev_clean() {
    dev_kill
    if [ -d "$BUILD_DIR" ]; then
        echo "=== Removing build directory ==="
        rm -r "$BUILD_DIR"
        echo "=== Build directory removed ==="
    else
        echo "=== No build directory to remove ==="
    fi
}

# ── Other commands ─────────────────────────────────────────────────────

dist_from_dev() {
    echo "=== Creating dist tarball from dev binary ==="
    dev_ensure_container

    # Verify the binary exists
    if ! podman exec "$DEV_CONTAINER" test -f /build/jami-bridge/build/jami-bridge; then
        echo "ERROR: No binary found. Run './build.sh dev' first."
        exit 1
    fi

    # Create a fully self-contained dist inside the container.
    # ALL non-glibc shared libraries are bundled so the dist works
    # across Linux distros (any system with glibc 2.35+).
    podman exec "$DEV_CONTAINER" bash -c '
        set -e
        mkdir -p /tmp/dist/jami-bridge-dist/lib

        # Copy binary
        cp /build/jami-bridge/build/jami-bridge /tmp/dist/jami-bridge-dist/jami-bridge
        chmod +x /tmp/dist/jami-bridge-dist/jami-bridge

        # Copy libjami.so
        cp /usr/local/lib64/libjami.so.16.0.0 /tmp/dist/jami-bridge-dist/lib/libjami.so.16.0.0

        # Bundle ALL non-glibc shared libraries for cross-distro portability.
        # glibc core (libc, libm, libdl, libpthread, ld-linux, linux-vdso)
        # is guaranteed on every Linux host — skip those.
        # Must iterate because ldd only resolves direct deps — transitive
        # deps of newly bundled libs are discovered in subsequent passes.
        cd /tmp/dist/jami-bridge-dist
        echo "=== Bundling all non-glibc shared libraries (iterative) ==="
        changed=1
        while [ "$changed" -ne 0 ]; do
            changed=0
            for lib_path in $( \
                LD_LIBRARY_PATH=/tmp/dist/jami-bridge-dist/lib ldd ./jami-bridge ./lib/*.so.* \
                | grep -oP "/[^\s:]+" \
                | sort -u \
            ); do \
                lib_name=$(basename "$lib_path"); \
                case "$lib_name" in \
                    ld-linux-x86-64.so.*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|linux-vdso.so.*) \
                        continue ;; \
                esac; \
                [ -f "lib/$lib_name" ] && continue;
                # Skip non-library entries (e.g. binary paths from ldd args)
                case "$lib_name" in *.so*) ;; *) continue ;; esac; \
                cp -L "$lib_path" "lib/$lib_name" && echo "  bundled: $lib_name" && changed=1 || echo "  FAILED: $lib_name"; \
            done; \
        done
        echo "=== Bundle complete: $(ls lib/*.so.* 2>/dev/null | wc -l) libraries ===" 

        # Patch RUNPATH on ALL bundled libs so they find each other
        echo "=== Patching RUNPATH on all bundled libs ==="
        for so in lib/*.so*; do
            [ -L "$so" ] && continue
            patchelf --set-rpath \$ORIGIN "$so" 2>/dev/null || true
        done

        # Create symlinks for versioned libs
        echo "=== Creating symlinks ==="
        cd lib
        for so in *.so.*.*; do
            [ -L "$so" ] && continue
            major="${so%.so.*}.so.$(echo "$so" | sed "s/.*\.so\.//; s/\..*//")"
            [ "$major" != "$so" ] && ln -sf "$so" "$major" 2>/dev/null
        done
        for so in *.so.*; do
            [ -L "$so" ] && continue
            base="${so%.so.*}.so"
            [ ! -L "$base" ] && [ ! -e "$base" ] && ln -sf "$so" "$base" 2>/dev/null || true
        done

        # Verify all libraries resolve
        echo "=== Verifying library resolution ==="
        cd /tmp/dist/jami-bridge-dist
        LD_LIBRARY_PATH=/tmp/dist/jami-bridge-dist/lib ldd ./jami-bridge | grep "not found" && echo "ERROR: Missing libs!" && exit 1 || echo "All libs resolved."

        echo "=== Dist contents ==="
        du -sh /tmp/dist/jami-bridge-dist/
        ls -lh /tmp/dist/jami-bridge-dist/
        echo "=== Bundled libraries ==="
        ls -lh /tmp/dist/jami-bridge-dist/lib/

        # Create tarball
        cd /tmp/dist && tar czf /tmp/jami-bridge-dist.tar.gz jami-bridge-dist/
        ls -lh /tmp/jami-bridge-dist.tar.gz
    '

    # Extract tarball to host
    local host_dist="$SCRIPT_DIR/jami-bridge-dist-output"
    mkdir -p "$host_dist"
    podman cp "$DEV_CONTAINER":/tmp/jami-bridge-dist.tar.gz "$host_dist/jami-bridge-dist.tar.gz"

    # Also extract to host for testing
    cd "$host_dist"
    tar xzf jami-bridge-dist.tar.gz

    echo "=== Distribution created at: $host_dist/ ==="
    echo "    Binary: $host_dist/jami-bridge-dist/jami-bridge"
    echo "    Tarball: $host_dist/jami-bridge-dist.tar.gz"
    ls -lh "$host_dist/jami-bridge-dist.tar.gz"
}

clean() {
    echo "=== Removing containers and images ==="
    dev_kill 2>/dev/null || true
    podman rmi "$IMAGE_BRIDGE" 2>/dev/null || true
    podman rmi jami-bridge-dist 2>/dev/null || true
    podman rmi jami-bridge-test 2>/dev/null || true
    # Don't remove base or dev by default — too expensive to rebuild
    podman image prune -f 2>/dev/null || true
    echo "=== Done ==="
}

clean_all() {
    echo "=== Removing ALL jami images (including base) ==="
    dev_kill 2>/dev/null || true
    podman rmi "$IMAGE_BRIDGE" 2>/dev/null || true
    podman rmi "$IMAGE_DEV" 2>/dev/null || true
    podman rmi jami-bridge-dist 2>/dev/null || true
    podman rmi jami-bridge-test 2>/dev/null || true
    podman rmi "$IMAGE_BASE" 2>/dev/null || true
    podman image prune -f 2>/dev/null || true
    echo "=== Done ==="
}

case "${1:-help}" in
    # Production builds
    base)
        build_base
        ;;
    check)
        check_symbols
        ;;
    bridge)
        build_bridge
        ;;
    dist)
        build_bridge
        build_dist
        ;;
    test-dist)
        build_bridge
        test_dist
        ;;
    all)
        build_base
        check_symbols
        build_bridge
        ;;

    # Development builds (fast incremental)
    dev)
        dev_build
        ;;
    dev-shell)
        dev_shell
        ;;
    dev-run)
        shift
        dev_run "$@"
        ;;
    dev-kill)
        dev_kill
        ;;
    dev-clean)
        dev_clean
        ;;
    dev-dist)
        dist_from_dev
        ;;

    # Cleanup
    clean)
        clean
        ;;
    clean-all)
        clean_all
        ;;

    # Native build (no containers — for macOS or host with libjami)
    native)
        native_build
        ;;
    native-dist)
        native_dist
        ;;

    # Help
    help|*)
        echo "jami-bridge build script"
        echo ""
        echo "Native builds (no containers — for macOS or host with libjami):"
        echo "  native        Build bridge on host (cmake + make)"
        echo "  native-dist   Bundle bridge + libs into dist tarball"
        echo ""
        echo "Production builds (full image rebuild, slow):"
        echo "  base          Build base image (daemon from source, ~10-20min)"
        echo "  check         Check libjami:: symbols in base image"
        echo "  bridge        Build bridge production image"
        echo "  dist          Build bridge + create self-contained dist tarball"
        echo "  test-dist     Test dist in a fresh Fedora container"
        echo "  all           base → bridge"
        echo ""
        echo "Development builds (incremental, source mounted, fast):"
        echo "  dev           Build/rebuild bridge in dev container (seconds!)"
        echo "  dev-run       Run dev binary in a fresh runtime container"
        echo "  dev-shell     Open a shell in the dev container"
        echo "  dev-kill      Stop and remove the dev container"
        echo "  dev-clean     Remove build directory and dev container"
        echo "  dev-dist      Create dist tarball from dev binary (no image rebuild)"
        echo ""
        echo "Cleanup:"
        echo "  clean         Remove bridge/dist images (keeps base/dev)"
        echo "  clean-all     Remove ALL images including base (expensive!)"
        ;;
esac
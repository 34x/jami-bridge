#!/bin/bash
# Build jami-sdk in containers. All compilation happens inside containers.
#
# Production builds (full image rebuild):
#   ./build.sh base        — Build base image (daemon from source, ~4GB, slow)
#   ./build.sh sdk         — Build SDK production image
#   ./build.sh dist        — Build self-contained dist tarball
#   ./build.sh test-dist   — Test dist in fresh container
#   ./build.sh all         — base → sdk
#   ./build.sh clean       — Remove images
#
# Development builds (fast, incremental, source mounted):
#   ./build.sh dev         — Build or rebuild SDK in dev container (fast!)
#   ./build.sh dev-run     — Run the dev binary in a fresh runtime container
#   ./build.sh dev-shell   — Open a shell in the dev container
#   ./build.sh dev-clean   — Remove dev build directory
#   ./build.sh dev-kill    — Stop and remove the dev container
#
# The dev workflow keeps a persistent container running. Source is mounted
# from the host, so only the SDK binary is rebuilt — not the 4GB daemon.
# Use 'dev-run' to test the binary in a clean runtime container — no
# image rebuild or dist packaging needed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Build context for Containerfile.base: the SDK repo root (contains daemon/ submodule)
# When jami-sdk is its own repo, daemon/ is a git submodule here.
BUILD_CONTEXT="$SCRIPT_DIR"
IMAGE_BASE="jami-sdk-base"
IMAGE_SDK="jami-sdk"
IMAGE_DEV="jami-sdk-dev"
IMAGE_RUNTIME="jami-sdk-runtime"
DEV_CONTAINER="jami-sdk-dev"
SDK_SOURCE="$SCRIPT_DIR"  # jami-sdk/ directory on host
BUILD_DIR="$SDK_SOURCE/build"  # build/ on host, mounted into container

# ── Base image (daemon from source) ────────────────────────────────────

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

build_sdk() {
    echo "=== Building SDK production image ==="
    podman build \
        -t "$IMAGE_SDK" \
        -f "$SCRIPT_DIR/Containerfile" \
        "$BUILD_CONTEXT"
    echo "=== SDK image built: $IMAGE_SDK ==="
}

build_dist() {
    echo "=== Building self-contained distribution ==="
    podman build \
        -t jami-sdk-dist \
        -f "$SCRIPT_DIR/Containerfile.dist" \
        "$BUILD_CONTEXT"
    echo "=== Distribution image built: jami-sdk-dist ==="
    echo ""
    echo "To extract the tarball:"
    echo "  podman create --name dist-extract jami-sdk-dist"
    echo "  podman cp dist-extract:/dist/jami-sdk-dist.tar.gz ."
    echo "  podman rm dist-extract"
    echo "  tar xzf jami-sdk-dist.tar.gz"
    echo "  ./jami-sdk-dist/jami-sdk --help"
}

test_dist() {
    echo "=== Testing dist in fresh container ==="
    podman build \
        -t jami-sdk-test \
        -f "$SCRIPT_DIR/Containerfile.test" \
        "$BUILD_CONTEXT"
    echo "=== Dist test complete ==="
}

# ── Development builds (persistent container, source mounted) ───────────
#
# The dev workflow uses a persistent container that stays running.
# Source code is bind-mounted from the host, so:
#   - Only the SDK binary is rebuilt (not the 4GB daemon)
#   - cmake is incremental — only changed files are recompiled
#   - The output binary appears at jami-sdk/build/jami-sdk on the host
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
        -v "$SDK_SOURCE:/build/jami-sdk:Z" \
        -v "$BUILD_DIR:/build/jami-sdk/build:Z" \
        localhost/$IMAGE_DEV \
        sleep infinity
    echo "=== Dev container started: $DEV_CONTAINER ==="
}

dev_build() {
    dev_ensure_container

    echo "=== Building SDK in dev container (incremental) ==="

    # Check if cmake has been configured
    if ! podman exec "$DEV_CONTAINER" test -f /build/jami-sdk/build/Makefile 2>/dev/null; then
        echo "    First run — configuring with cmake..."
        podman exec "$DEV_CONTAINER" bash -c \
            "cd /build/jami-sdk/build && \
             cmake .. \
                 -DJAMI_INCLUDE_DIR=/usr/local/include/jami \
                 -DJAMI_LIBRARY=/usr/local/lib64/libjami.so"
    fi

    echo "    Compiling..."
    podman exec "$DEV_CONTAINER" make -C /build/jami-sdk/build -j"$(nproc)"

    # Verify binary exists on host
    if [ -f "$BUILD_DIR/jami-sdk" ]; then
        local size
        size=$(du -h "$BUILD_DIR/jami-sdk" | cut -f1)
        echo "=== Build successful: $BUILD_DIR/jami-sdk ($size) ==="
    else
        echo "ERROR: Binary not found at $BUILD_DIR/jami-sdk"
        exit 1
    fi
}

dev_shell() {
    dev_ensure_container
    echo "=== Opening shell in dev container ==="
    echo "    Source mounted: /build/jami-sdk"
    echo "    Build dir:      /build/jami-sdk/build"
    echo "    To rebuild:     cd build && make -j\$(nproc)"
    echo ""
    podman exec -it "$DEV_CONTAINER" bash
}

# ── Runtime image (minimal Fedora + system libs, for dev-run) ───────────
#
# This is a small image (~200MB) with just the runtime dependencies.
# The SDK binary and bundled libs are mounted at runtime, so this
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
    if [ ! -f "$BUILD_DIR/jami-sdk" ]; then
        echo "ERROR: No binary found. Run './build.sh dev' first."
        exit 1
    fi

    # Create a dist-like directory from the dev binary + base image libs
    local dev_dist="$SCRIPT_DIR/dev-dist"
    mkdir -p "$dev_dist/lib"

    # Copy the fresh binary (only ~3.5MB, fast)
    cp "$BUILD_DIR/jami-sdk" "$dev_dist/jami-sdk"
    chmod +x "$dev_dist/jami-sdk"

    # Copy bundled libs from the dev container (only needed once)
    if [ ! -f "$dev_dist/lib/libjami.so.16.0.0" ]; then
        echo "=== Copying bundled libs from dev container (one-time) ==="
        podman exec "$DEV_CONTAINER" bash -c '
            mkdir -p /tmp/dev-dist/lib
            cp /usr/local/lib64/libjami.so.16.0.0 /tmp/dev-dist/lib/
            cp /usr/lib64/libgit2.so.1.9.2 /tmp/dev-dist/lib/
            cp /usr/lib64/libsecp256k1.so.5.0.0 /tmp/dev-dist/lib/
            cp /usr/lib64/libllhttp.so.9.3.1 /tmp/dev-dist/lib/
            cd /tmp/dev-dist/lib
            ln -sf libjami.so.16.0.0 libjami.so.16
            ln -sf libjami.so.16 libjami.so
            ln -sf libgit2.so.1.9.2 libgit2.so.1.9
            ln -sf libsecp256k1.so.5.0.0 libsecp256k1.so.5
            ln -sf libllhttp.so.9.3.1 libllhttp.so.9.3
            patchelf --set-rpath \$ORIGIN libjami.so.16.0.0 2>/dev/null || true
            patchelf --set-rpath \$ORIGIN libgit2.so.1.9.2 2>/dev/null || true
            patchelf --set-rpath \$ORIGIN libsecp256k1.so.5.0.0 2>/dev/null || true
            patchelf --set-rpath \$ORIGIN libllhttp.so.9.3.1 2>/dev/null || true
        '
        podman cp "$DEV_CONTAINER":/tmp/dev-dist/lib/. "$dev_dist/lib/"
        echo "=== Bundled libs copied ==="
    fi

    # Run the dev binary in a fresh runtime container with the dist mounted
    # Pass any extra args to jami-sdk
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
        -v "$dev_dist:/opt/jami-sdk:Z" \
        localhost/$IMAGE_RUNTIME \
        /opt/jami-sdk/jami-sdk "${@:- --host 0.0.0.0 --port 8090}"
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
    if ! podman exec "$DEV_CONTAINER" test -f /build/jami-sdk/build/jami-sdk; then
        echo "ERROR: No binary found. Run './build.sh dev' first."
        exit 1
    fi

    # Create a temporary dist directory inside the container
    podman exec "$DEV_CONTAINER" bash -c '
        set -e
        mkdir -p /tmp/dist/jami-sdk-dist/lib

        # Copy binary
        cp /build/jami-sdk/build/jami-sdk /tmp/dist/jami-sdk-dist/jami-sdk

        # Copy libjami.so
        cp /usr/local/lib64/libjami.so.16.0.0 /tmp/dist/jami-sdk-dist/lib/libjami.so.16.0.0

        # Copy system libraries that may not be on the host
        cp /usr/lib64/libgit2.so.1.9.2 /tmp/dist/jami-sdk-dist/lib/libgit2.so.1.9.2
        cp /usr/lib64/libsecp256k1.so.5.0.0 /tmp/dist/jami-sdk-dist/lib/libsecp256k1.so.5.0.0
        cp /usr/lib64/libllhttp.so.9.3.1 /tmp/dist/jami-sdk-dist/lib/libllhttp.so.9.3.1

        # Create symlinks
        cd /tmp/dist/jami-sdk-dist/lib
        ln -sf libjami.so.16.0.0 libjami.so.16
        ln -sf libjami.so.16 libjami.so
        ln -sf libgit2.so.1.9.2 libgit2.so.1.9
        ln -sf libsecp256k1.so.5.0.0 libsecp256k1.so.5
        ln -sf libllhttp.so.9.3.1 libllhttp.so.9.3

        # Patch RPATH on bundled libs
        patchelf --set-rpath \$ORIGIN /tmp/dist/jami-sdk-dist/lib/libjami.so.16.0.0
        patchelf --set-rpath \$ORIGIN /tmp/dist/jami-sdk-dist/lib/libgit2.so.1.9.2
        patchelf --set-rpath \$ORIGIN /tmp/dist/jami-sdk-dist/lib/libsecp256k1.so.5.0.0
        patchelf --set-rpath \$ORIGIN /tmp/dist/jami-sdk-dist/lib/libllhttp.so.9.3.1

        chmod +x /tmp/dist/jami-sdk-dist/jami-sdk

        # Verify
        echo "=== Dist contents ==="
        du -sh /tmp/dist/jami-sdk-dist/
        ls -lh /tmp/dist/jami-sdk-dist/
        ls -lh /tmp/dist/jami-sdk-dist/lib/

        # Create tarball
        cd /tmp/dist && tar czf /tmp/jami-sdk-dist.tar.gz jami-sdk-dist/
        ls -lh /tmp/jami-sdk-dist.tar.gz
    '

    # Extract tarball to host
    local host_dist="$SCRIPT_DIR/jami-sdk-dist-output"
    mkdir -p "$host_dist"
    podman cp "$DEV_CONTAINER":/tmp/jami-sdk-dist.tar.gz "$host_dist/jami-sdk-dist.tar.gz"

    # Also extract to host for testing
    cd "$host_dist"
    tar xzf jami-sdk-dist.tar.gz

    echo "=== Distribution created at: $host_dist/ ==="
    echo "    Binary: $host_dist/jami-sdk-dist/jami-sdk"
    echo "    Tarball: $host_dist/jami-sdk-dist.tar.gz"
    ls -lh "$host_dist/jami-sdk-dist.tar.gz"
}

clean() {
    echo "=== Removing containers and images ==="
    dev_kill 2>/dev/null || true
    podman rmi "$IMAGE_SDK" 2>/dev/null || true
    podman rmi jami-sdk-dist 2>/dev/null || true
    podman rmi jami-sdk-test 2>/dev/null || true
    # Don't remove base or dev by default — too expensive to rebuild
    podman image prune -f 2>/dev/null || true
    echo "=== Done ==="
}

clean_all() {
    echo "=== Removing ALL jami images (including base) ==="
    dev_kill 2>/dev/null || true
    podman rmi "$IMAGE_SDK" 2>/dev/null || true
    podman rmi "$IMAGE_DEV" 2>/dev/null || true
    podman rmi jami-sdk-dist 2>/dev/null || true
    podman rmi jami-sdk-test 2>/dev/null || true
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
    sdk)
        build_sdk
        ;;
    dist)
        build_sdk
        build_dist
        ;;
    test-dist)
        build_sdk
        test_dist
        ;;
    all)
        build_base
        check_symbols
        build_sdk
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

    # Help
    help|*)
        echo "jami-sdk build script"
        echo ""
        echo "Production builds (full image rebuild, slow):"
        echo "  base          Build base image (daemon from source, ~10-20min)"
        echo "  check         Check libjami:: symbols in base image"
        echo "  sdk           Build SDK production image"
        echo "  dist          Build SDK + create self-contained dist tarball"
        echo "  test-dist     Test dist in a fresh Fedora container"
        echo "  all           base → sdk"
        echo ""
        echo "Development builds (incremental, source mounted, fast):"
        echo "  dev           Build/rebuild SDK in dev container (seconds!)"
        echo "  dev-run       Run dev binary in a fresh runtime container"
        echo "  dev-shell     Open a shell in the dev container"
        echo "  dev-kill      Stop and remove the dev container"
        echo "  dev-clean     Remove build directory and dev container"
        echo "  dev-dist      Create dist tarball from dev binary (no image rebuild)"
        echo ""
        echo "Cleanup:"
        echo "  clean         Remove SDK/dist images (keeps base/dev)"
        echo "  clean-all     Remove ALL images including base (expensive!)"
        ;;
esac
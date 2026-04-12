# Multi-stage build: compiles jami-sdk against the pre-built daemon,
# then creates a minimal runtime image with a self-contained distribution.
#
# Requires: jami-sdk-base image (built with Containerfile.base)
#
# Build: podman build -t jami-sdk -f Containerfile <project-root>
#   where <project-root> contains both daemon/ and jami-sdk/

# ── Stage 1: Build ──────────────────────────────────────────────────────
FROM jami-sdk-base AS builder

# Install build deps needed for the SDK (but not for the daemon)
RUN dnf install -y nlohmann-json-devel && dnf clean all

# Copy jami-sdk source
COPY jami-sdk/src/ /build/jami-sdk/src/
COPY jami-sdk/vendor/ /build/jami-sdk/vendor/
COPY jami-sdk/CMakeLists.txt /build/jami-sdk/

# Build jami-sdk against the pre-built libjami.so from the base image
RUN cd /build/jami-sdk && \
    mkdir -p build && cd build && \
    cmake .. \
        -DJAMI_INCLUDE_DIR=/usr/local/include/jami \
        -DJAMI_LIBRARY=/usr/local/lib64/libjami.so && \
    make -j$(nproc)

# ── Stage 2: Runtime ───────────────────────────────────────────────────
FROM fedora:43

# Install runtime dependencies (minimal set for libjami.so + system libs)
RUN dnf install -y \
    gnutls nettle opus speexdsp \
    yaml-cpp jsoncpp fmt \
    libuuid libargon2 libarchive \
    openssl-libs \
    alsa-lib \
    libgit2 libsecp256k1 \
    libva libvdpau mesa-libGL \
    libX11 libXext libXfixes \
    pipewire-libs pulseaudio-libs \
    dbus-libs msgpack sqlite-libs \
    pcre2 expat \
    && dnf clean all

# Copy jami library (only libjami.so — all other deps are system libs)
COPY --from=builder /usr/local/lib64/libjami.so.16.0.0 /usr/local/lib64/libjami.so.16.0.0
RUN cd /usr/local/lib64 && ln -sf libjami.so.16.0.0 libjami.so.16 && ln -sf libjami.so.16 libjami.so

# Copy our binary (with RPATH $ORIGIN/lib embedded)
COPY --from=builder /build/jami-sdk/build/jami-sdk /usr/local/bin/jami-sdk

# Update library cache
RUN ldconfig

# Create non-root user
RUN useradd -m jami && \
    mkdir -p /home/jami/.local/share/jami /home/jami/.config/jami && \
    chown -R jami:jami /home/jami

USER jami
ENV XDG_DATA_HOME=/home/jami/.local/share
ENV XDG_CONFIG_HOME=/home/jami/.config

EXPOSE 8090

ENTRYPOINT ["jami-sdk"]
CMD ["--host", "0.0.0.0", "--port", "8090"]
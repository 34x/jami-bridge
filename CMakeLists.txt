cmake_minimum_required(VERSION 3.16)
project(jami-bridge VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────

# Jami daemon (libjami-core) — must be built with -Dinterfaces=library
# Set JAMI_INCLUDE_DIR and JAMI_LIBRARY via cmake or environment.
#
# On Linux with jami-daemon installed from package:
#   JAMI_INCLUDE_DIR=/usr/include/jami
#   JAMI_LIBRARY=/usr/lib64/libjami-core.so
#
# When building from source (recommended):
#   Build jami-daemon with: meson setup build -Dinterfaces=library
#   This produces libjami.so with libjami:: symbols exported.
#
# On Windows:
#   Build jami-daemon with Visual Studio + vcpkg dependencies
#   Produces jami.dll / jami.lib

find_path(JAMI_INCLUDE_DIR
    NAMES jami.h
    HINTS
        ${CMAKE_SOURCE_DIR}/../daemon/src
        $ENV{JAMI_INCLUDE_DIR}
        /usr/include/jami
        /usr/local/include/jami
)

find_library(JAMI_LIBRARY
    NAMES jami jami-core
    HINTS
        ${CMAKE_SOURCE_DIR}/../daemon/build/src
        $ENV{JAMI_LIBRARY_DIR}
        /usr/lib64
        /usr/lib
        /usr/local/lib
)

if(NOT JAMI_INCLUDE_DIR)
    message(FATAL_ERROR "Jami headers not found. Set JAMI_INCLUDE_DIR or build daemon from source.")
endif()

if(NOT JAMI_LIBRARY)
    message(FATAL_ERROR "Jami library not found. Set JAMI_LIBRARY or build daemon with -Dinterfaces=library.")
endif()

message(STATUS "Jami include: ${JAMI_INCLUDE_DIR}")
message(STATUS "Jami library: ${JAMI_LIBRARY}")

# ── Source Files ────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/client.cpp
    src/server.cpp
    src/stdio_server.cpp
    src/api_docs.cpp
    src/hook.cpp
    src/config.cpp
)

# ── Executable ──────────────────────────────────────────────────────────

add_executable(jami-bridge ${SOURCES})

target_include_directories(jami-bridge PRIVATE
    ${JAMI_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
)

target_link_libraries(jami-bridge PRIVATE
    ${JAMI_LIBRARY}
    pthread
)

# Platform-specific libraries and settings
if(WIN32)
    # On Windows, hook.cpp is not compiled (uses POSIX fork/exec)
    list(REMOVE_ITEM SOURCES src/hook.cpp)
    target_link_libraries(jami-bridge PRIVATE ws2_32 crypt32)
    # Windows doesn't use RPATH
    set_target_properties(jami-bridge PROPERTIES
        BUILD_RPATH ""
        INSTALL_RPATH ""
        BUILD_WITH_INSTALL_RPATH FALSE
    )
else()
    target_link_libraries(jami-bridge PRIVATE pthread)
    # Embed RPATH so the binary finds libs in $ORIGIN/lib/ next to itself.
    # This eliminates the need for LD_LIBRARY_PATH or wrapper scripts.
    set_target_properties(jami-bridge PROPERTIES
        BUILD_RPATH "$ORIGIN/lib"
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# ── Install ────────────────────────────────────────────────────────────

install(TARGETS jami-bridge DESTINATION bin)
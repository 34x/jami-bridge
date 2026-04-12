# Repo Split Plan — jami-sdk + jami-bot

## Target Structure

```
/path/to/jami-sdk/        ← NEW standalone repo
├── .git/                             ← fresh git repo
├── daemon/                           ← git submodule (jami-daemon)
├── src/                              ← C++ source (client, server, etc.)
├── vendor/                           ← httplib.h, json.hpp
├── examples/echo-bot/               ← simple hook example (stays in SDK)
├── Containerfile.base               ← COPY daemon/ from submodule
├── Containerfile.dev                ← dev layer on base
├── Containerfile                    ← production build
├── build.sh                         ← all build commands
├── AGENTS.md                        ← project context for AI agents
├── CMakeLists.txt
└── README.md

/path/to/jami-bot/        ← NEW standalone repo
├── .git/                             ← fresh git repo
├── bot.py                            ← main bot script
├── AGENTS.md                         ← project context for AI agents
└── README.md
```

## What Moves Where

### jami-sdk repo (from current jami-project/jami-sdk/)

Everything in `jami-sdk/` EXCEPT `examples/pi-bot/` moves to the new repo:
- All `src/` files
- `vendor/` (httplib.h, json.hpp)
- `Containerfile.*`
- `build.sh`
- `CMakeLists.txt`
- `README.md`
- `AGENTS.md`
- `examples/echo-bot/` (stays — it's an SDK feature example)

**New addition**: `daemon/` git submodule pointing to `https://review.jami.net/jami-daemon`

### jami-bot repo (from current jami-sdk/examples/pi-bot/)

- `bot.py` — the main bot script
- `README.md` — bot-specific docs
- `AGENTS.md` — bot context for AI agents

### Stays in jami-project (upstream)

- `daemon/` (original submodule)
- `jami-server/` (Python/DBus version, separate project)
- All upstream Jami submodules (client-qt, etc.)

## Dependency Chain

```
jami-daemon (upstream, git submodule)
     ↑
jami-sdk (links to libjami.so, built from daemon submodule)
     ↑ (runtime-only, via STDIO JSON-RPC)
jami-bot (subprocess of jami-sdk, no code imports)
```

- Daemon → SDK: compiled dependency (libjami.so)
- SDK → Bot: runtime binary dependency (jami-sdk process)
- Bot has **no code imports** from SDK — pure STDIO JSON-RPC contract

## Setup Steps (for new session)

1. Create jami-sdk directory — copy files, init git, add daemon submodule
2. Create jami-bot directory — copy bot.py, init git
3. Verify builds still work (`./build.sh base`, `./build.sh dev`)
4. Verify bot works with `JAMI_SDK_PATH` pointing to SDK dist
5. Update .gitignore in jami-project to remove jami-sdk/ tracking

## Key Build Changes

- `Containerfile.base` now uses `COPY daemon/` instead of `git clone`
- `build.sh` uses `BUILD_CONTEXT=$SCRIPT_DIR` (SDK repo root)
- Base image is built locally only (once, ~4GB, ~20min)
- No registry push needed — cached locally by podman

## Bot Runtime Changes

- `JAMI_SDK_PATH` env var or `--jami PATH` flag for binary location
- Bot is Python stdlib-only — no pip packages from SDK
- Can run standalone once jami-sdk binary is available
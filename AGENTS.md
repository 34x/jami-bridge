# AGENTS.md — jami-bridge Project Context

> This file provides project context for AI coding agents working on jami-bridge.

## Project Overview

**jami-bridge** is a self-contained C++ service that links directly to `libjami.so`
(library mode, no DBus), exposing an HTTP REST API, STDIO JSON-RPC interface,
and CLI commands for programmatic access to the Jami messaging platform.

It runs as a single process (like Android/iOS/Windows Jami apps), requires no
DBus daemon, and focuses on messaging (no calls/file transfers initially).

## Architecture

Three modes in one binary (`jami-bridge`), all sharing the same `Client` class:

| Mode | Flag | Interface | Use Case |
|------|------|-----------|----------|
| HTTP | *(default)* | REST API on port 8090 | Scripting, webhooks, general use |
| STDIO | `--stdio` | JSON-RPC 2.0 over stdin/stdout | Bot integration, event-driven |
| CLI | `--list-accounts` etc. | One-shot commands | Shell scripts, quick queries |

### Hook System (POSIX-only)

`--hook COMMAND` spawns an external process per event, pipes JSON to stdin,
reads stdout for `{"reply": "text"}` auto-reply. Uses fork/exec/pipes with timeout.
On Windows, `--hook` returns an error — use STDIO or HTTP mode instead.

### File Transfers (STDIO-only)

File transfer APIs (`sendFile`, `downloadFile`, `cancelTransfer`, `transferInfo`)
and the `onDataTransferEvent` signal are **only available via STDIO JSON-RPC**.
They are intentionally **not exposed as HTTP REST endpoints** because they accept
arbitrary filesystem paths, which would be a security risk on a network-facing API.

When HTTP file transfer support is added, it will include path restrictions and
authentication.

### Invite Policy

By default, the bridge is a **passive bridge** — it emits events but takes no action.

| Flag | Behavior | Use Case |
|------|----------|----------|
| *(none)* | Passive — emit events, no accept/decline | Bot handles invites via STDIO/HTTP |
| `--auto-accept` | Accept ALL invites | Open bot, testing |
| `--auto-accept-from URI` | Accept from owner, decline others | Production bot |
| `--reject-unknown` | Reject ALL invites | Lockdown after setup |

Policy only installed when a flag is explicitly set. Applies to both conversation
requests (group invites) and trust requests (contact requests).

Config file: `autoAccept` (bool), `autoAcceptFrom` (string), `rejectUnknown` (bool).
Priority: `--auto-accept` > `--auto-accept-from` > `--reject-unknown`.

## Key Source Files

| File | Purpose |
|------|---------|
| `src/client.h` | Client class, Events struct, Stats struct |
| `src/client.cpp` | libjami:: API wrapper, signal handlers, sync message loading |
| `src/server.h/cpp` | HTTP REST server (cpp-httplib) |
| `src/stdio_server.h/cpp` | JSON-RPC 2.0 STDIO server |
| `src/hook.h/cpp` | POSIX-only fork/exec/pipes hook manager |
| `src/config.h/cpp` | CLI args + JSON config file parsing |
| `src/api_docs.h/cpp` | Interactive HTML docs + OpenAPI 3.0.3 spec |
| `src/main.cpp` | Entry point, CLI parsing, resolve_account, invite policy |
| `vendor/httplib.h` | cpp-httplib v0.18.3 (header-only HTTP) |
| `vendor/json.hpp` | nlohmann/json v3.11.3 (header-only JSON) |

## Build System

All builds happen **inside podman containers** — never on the host.

### Build commands (`build.sh`)

| Command | Description | Time |
|---------|-------------|------|
| `./build.sh base` | Build jami-bridge-base image (daemon from source) | ~20min, once |
| `./build.sh dev` | Incremental build in dev container | <8s |
| `./build.sh dev-run [args]` | Run dev binary in runtime container | — |
| `./build.sh dev-dist` | Bundle binary + libs into `jami-bridge-dist-output/` | ~30s |
| `./build.sh dev-shell` | Shell into running dev container | — |
| `./build.sh dev-kill` | Stop dev container | — |
| `./build.sh dev-clean` | Remove dev container and build dir | — |
| `./build.sh dist` | Production multi-stage build + dist image | ~5min |
| `./build.sh test-dist` | Test production dist in fresh container | — |
| `./build.sh sdk` | Production multi-stage build (no dist) | ~5min |

### Containerfiles

| File | Purpose | Image Size |
|------|---------|-----------|
| `Containerfile.base` | Builds jami-daemon from source in library mode | 4 GB |
| `Containerfile.dev` | Thin layer on base for incremental dev | ~4 GB (shared layers) |
| `Containerfile` | Production multi-stage build | 1.38 GB |

### Dev Workflow

1. `./build.sh base` — once, builds the 4GB base image
2. `./build.sh dev` — starts persistent container `jami-bridge-dev` with source bind-mounted
3. Edit code on host → `./build.sh dev` rebuilds incrementally (<8s)
4. `./build.sh dev-dist` — bundles binary + libs into `jami-bridge-dist-output/` (no image rebuild)

**Typical cycle:** `./build.sh dev && ./build.sh dev-dist`

Output: `jami-bridge-dist-output/jami-bridge-dist/jami-bridge` — self-contained binary
with RPATH `$ORIGIN/lib`, works on host directly (Fedora 43+).

**For running in a container:** `./build.sh dev-run [--port 8091]`

### Container Images

| Image | Purpose |
|-------|---------|
| `localhost/jami-bridge-base:latest` | Daemon compiled from source (library mode) |
| `localhost/jami-bridge-dev:latest` | Thin dev layer on base |
| `localhost/jami-bridge:latest` | Production image |

## Dependencies

### Jami Daemon

- `libjami.so` (236 MB) — built from `../daemon/` with `-Dinterfaces=library`
- API namespace: **`libjami::`** (NOT `DRing::`, which was renamed in recent versions)
- Exported via `LIBJAMI_PUBLIC` visibility macro (needs `jami_EXPORTS` + `LIBJAMI_BUILD` defines)
- 1049 `libjami::` symbols exported

### Key Jami API Functions

- `libjami::init()`, `libjami::start()`, `libjami::fini()`
- `libjami::sendMessage()`, `libjami::startConversation()`
- `libjami::getAccountList()`, `libjami::getAccountDetails()`
- `libjami::registerSignalHandlers()` — takes `std::map<std::string, std::shared_ptr<CallbackWrapperBase>>`
- `libjami::acceptConversationRequest()`, `libjami::declineConversationRequest()`
- `libjami::sendTrustRequest()`, `libjami::acceptTrustRequest()`, `libjami::discardTrustRequest()`
- `libjami::getConversations()`, `libjami::getConversationRequests()`
- `libjami::loadConversationMessages()` — async, needs condition_variable wait for sync
- `libjami::sendFile()`, `libjami::downloadFile()`, `libjami::cancelDataTransfer()`, `libjami::fileTransferInfo()`

### Key Signal Names

| Signal | Old Name | Purpose |
|--------|----------|---------|
| `SwarmMessageReceived` | `MessageReceived` | New message in conversation |
| `SwarmLoaded` | `ConversationLoaded` | Message history loaded |
| `ConversationRequestReceived` | — | Group/conversation invite |
| `IncomingTrustRequest` | — | Contact request |
| `registrationStateChanged` | — | Account registration status |
| `DataTransferEvent` | — | File transfer progress/status |

### SwarmMessage Body Key

- **Loaded history**: `body["body"]` contains the text
- **Live received messages**: `body["text/plain"]` contains the text
- This is a Jami daemon quirk — the key differs between loaded and live messages.

## Self-Contained Distribution

The `dev-dist` command bundles the dev binary + required shared libs:

```
jami-bridge-dist/
├── jami-bridge          # Binary (3.6 MB), RPATH: $ORIGIN/lib
└── lib/
    ├── libjami.so.16.0.0         237 MB  Jami daemon
    ├── libgit2.so.1.9.2           1.3 MB  Git support
    ├── libsecp256k1.so.5.0.0      1.3 MB  Crypto
    └── libllhttp.so.9.3.1          80 KB  HTTP parser
```

- Binary has `$ORIGIN/lib` RPATH — works without `LD_LIBRARY_PATH`
- All bundled `.so` files also get `$ORIGIN` RPATH via `patchelf` (transitive deps)
- Total: 243 MB extracted, ~92 MB compressed tarball
- Output at: `jami-bridge-dist-output/jami-bridge-dist/`

**To rebuild the dist:** `./build.sh dev && ./build.sh dev-dist`

## Configuration

### CLI → Config File → Defaults Priority

Every setting can be set via CLI arg, JSON config file (`--config FILE.json`), or defaults.
CLI always wins → config file fills remaining defaults → built-in defaults last.

### Config File Format

```json
{
  "host": "0.0.0.0",
  "port": 8090,
  "debug": false,
  "dataDir": "/path/to/data",
  "configDir": "/path/to/config",
  "cacheDir": "/path/to/cache",
  "account": "<account-id>",
  "accountAlias": "bot",
  "accountPassword": "secret",
  "autoAccept": true,
  "autoAcceptFrom": "owner-jami-uri",
  "rejectUnknown": false,
  "hook": {
    "command": "./bot.sh",
    "events": "onMessageReceived,onConversationReady",
    "timeout": 30
  }
}
```

### Account Management

`--account` flag accepts:
- Empty: auto-detect first account, or create new if none exist
- Account ID (hex string): use specific account
- `archive:///path/to/file.gz`: import from archive (file must exist)
- `/path/to/file.gz`: **create-or-reuse** — import if file exists, create new + export if not
- `new`: create a new account

The create-or-reuse pattern enables persistent bot accounts:
```bash
# First run: creates account + exports to /tmp/jami-bot.gz
# Subsequent runs: imports the saved archive
jami-bridge --account /tmp/jami-bot.gz
```

## Key API Details

### HTTP Endpoints (port 8090)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/ping` | GET | Health check |
| `/api/version` | GET | Version + capabilities |
| `/api/stats` | GET | Runtime statistics |
| `/api/shutdown` | POST | Graceful shutdown |
| `/api/accounts` | GET | List accounts |
| `/api/accounts/:id` | GET | Account details |
| `/api/accounts/:id/contacts` | GET | Contact list |
| `/api/accounts/:id/conversations` | GET | Conversation list |
| `/api/accounts/:id/conversations/:conv` | GET | Conversation detail |
| `/api/accounts/:id/conversations/:conv/messages` | GET | Message history |
| `/api/accounts/:id/conversations/:conv/send` | POST | Send message |
| `/api/accounts/:id/conversations/:conv/invite` | POST | Invite member |
| `/api/conversations` | POST | Create conversation |
| `/api/requests` | GET | Pending conversation requests |
| `/api/requests/:id/accept` | POST | Accept request |
| `/api/requests/:id/decline` | POST | Decline request |
| `/api/openapi.json` | GET | OpenAPI 3.0.3 spec |
| `/` | GET | Interactive HTML docs |

### Shutdown

`POST /api/shutdown` → spawns thread → `svr_->stop()` → `_exit(0)`.
SIGTERM works. SIGINT is unreliable (cpp-httplib doesn't handle it well).

### Stats Counters

- `messages_received`, `messages_sent`
- `invites_accepted`, `invites_declined`
- `hook_invocations`, `hook_replies`, `hook_timeouts`, `hook_errors`
- `uptime_seconds`

## Cross-Platform

| Feature | Linux | Windows |
|---------|-------|---------|
| HTTP mode | ✅ | ✅ |
| STDIO mode | ✅ | ✅ |
| CLI mode | ✅ | ✅ |
| Hook mode | ✅ | ❌ (POSIX-only) |
| Cross-compile | — | `Containerfile.mingw` (experimental) |

## Constraints

- **Never** use `rm -rf` with `-f` flag
- **Never** build/compile on host — only in podman containers
- **Never** use absolute paths — only relative
- Port 8090 (avoids common port conflicts)
- `curl` on host needs `-4` flag to force IPv4
- The existing `jami-bot` container (Python/DBus version) must not be disturbed

## Test Account

- Account ID: `<account-id>` (displayed on startup)
- Jami URI: `<your-jami-uri>` (displayed on startup)
- Alias: `bot`

## Upcoming Features

1. SSE/WebSocket for real-time event push (HTTP mode)
2. Authentication for HTTP server
3. HTTP file transfer endpoints (with path restrictions and authentication)
4. Windows cross-compilation testing
5. Reducing `libjami.so` size (236 MB)
6. More hook event types
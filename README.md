# jami-bridge

**Unofficial** self-contained C++ bridge for the [Jami](https://jami.net/) messaging platform.

> **Not affiliated with** [Savoir-faire Linux](https://www.savoirfairelinux.com/) or the
> Jami project. Jami is a trademark of Savoir-faire Linux Inc.
>
> Built with [pi.dev](https://pi.dev) and **GLM-5.1**.

Runs the Jami daemon in-process (`libjami.so`, library mode — same approach as
Android/iOS/Windows clients, no DBus required) and exposes it through three interfaces:

| Mode | Use case |
|------|----------|
| **HTTP REST** | Web services, scripting, remote access |
| **STDIO (JSON-RPC)** | Bots, automation — single process, real-time events |
| **CLI (one-shot)** | Shell scripts, quick operations |
| **Hook** | Event-driven scripting — run any command on events |

```
jami-bridge/              ← the binary (3.2 MB)
lib/                   ← 4 bundled libraries (240 MB)
  ├── libjami.so.16.0.0      Jami daemon
  ├── libgit2.so.1.9.2       Git repository support
  ├── libsecp256k1.so.5.0.0  Crypto (elliptic curves)
  └── libllhttp.so.9.3.1     HTTP parser
```

No host packages needed — all non-glibc dependencies are bundled.
Works out of the box on any Linux with glibc 2.35+ (Fedora 38+, Ubuntu 22.04+, Debian 12+, RHEL 9+).
Just extract and run.

---

## Quick Start

### Download

Grab the latest distribution tarball from releases, or build from source (see [Building](#building)).

```bash
tar xzf jami-bridge-dist.tar.gz
cd jami-bridge-dist
```

### Run

```bash
# HTTP server (default, port 8090)
./jami-bridge

# The bridge prints your bot's Jami identity on startup:
#   [jami-bridge] Bot identity: <your-jami-uri> (account: <account-id>, alias: bot)
#   [jami-bridge] Add this bot to a group: invite <your-jami-uri>

# Custom host/port
./jami-bridge --host 127.0.0.1 --port 3000

# Health check
curl -4 http://127.0.0.1:8090/api/ping

# Interactive API docs
open http://127.0.0.1:8090/
```

No `LD_LIBRARY_PATH`, no wrapper script — the binary has `$ORIGIN/lib` RPATH
baked in, so it automatically finds `./lib/` next to itself.

### Shutdown

```bash
curl -4 -X POST http://127.0.0.1:8090/api/shutdown
```

Or send SIGTERM to the process. (SIGINT/Ctrl+C is unreliable — the PJSIP
library inside the daemon intercepts it.)

---

## Running Modes

### HTTP REST API (default)

```bash
./jami-bridge [--host HOST] [--port PORT] [--debug]
```

Starts an HTTP server. All operations are REST endpoints.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/ping` | GET | Health check |
| `/api/shutdown` | POST | Shut down server |
| `/api/accounts` | GET | List accounts |
| `/api/accounts` | POST | Create account (`{alias, password}`) |
| `/api/accounts/:id` | GET | Account details |
| `/api/accounts/:id/status` | GET | Registration status |
| `/api/accounts/:id/conversations` | GET | List conversations |
| `/api/accounts/:id/conversations` | POST | Create conversation (`{title?}`) |
| `/api/accounts/:id/conversations/:conv` | GET | Conversation info + members |
| `/api/accounts/:id/conversations/:conv/invite` | POST | Invite member (`{uri}`) |
| `/api/accounts/:id/conversations/:conv/messages` | POST | Send message (`{body}`) |
| `/api/accounts/:id/conversations/:conv/messages` | GET | Load messages (`?count=N&from=ID`) |

Interactive HTML docs at `GET /`, OpenAPI 3.0 spec at `GET /api/openapi.json`.

### STDIO (JSON-RPC over stdin/stdout)

```bash
./jami-bridge --stdio
```

Reads JSON-RPC 2.0 requests from stdin (one per line), writes responses to stdout.
**Events are pushed as notifications** — no polling needed.

```bash
# Example session
echo '{"jsonrpc":"2.0","method":"listAccounts","id":1}' | ./jami-bridge --stdio
# → {"jsonrpc":"2.0","id":1,"result":{"accounts":["<account-id>"]}}

# Send a message
echo '{"jsonrpc":"2.0","method":"sendMessage","params":{"accountId":"...","conversationId":"...","body":"hello"},"id":2}' | ./jami-bridge --stdio

# Real-time event notification (pushed by bridge without request):
# {"jsonrpc":"2.0","method":"onMessageReceived","params":{"accountId":"...","conversationId":"...","from":"...","body":"hi!"}}
```

**JSON-RPC methods** mirror the REST API:

| Method | Key params | Returns |
|--------|-----------|---------|
| `ping` | — | `{status, version}` |
| `shutdown` | — | `{status}` |
| `listAccounts` | — | `{accounts: [id]}` |
| `createAccount` | `alias?, password?` | `{accountId}` |
| `getAccountDetails` | `accountId` | `{details}` |
| `getAccountStatus` | `accountId` | `{status}` |
| `removeAccount` | `accountId` | `{removed}` |
| `listConversations` | `accountId` | `{conversations: [{id, mode, title, members}]}` |
| `createConversation` | `accountId, title?` | `{conversationId, title}` |
| `getConversation` | `accountId, conversationId` | `{mode, title, memberCount, members}` |
| `removeConversation` | `accountId, conversationId` | `{removed}` |
| `inviteMember` | `accountId, conversationId, uri` | `{invited}` |
| `removeMember` | `accountId, conversationId, uri` | `{removed}` |
| `updateConversation` | `accountId, conversationId, info` | `{updated}` |
| `sendMessage` | `accountId, conversationId, body, parentId?` | `{sent, conversationId}` |
| `loadMessages` | `accountId, conversationId, count?, from?` | `{messages: [...]}` |
| `listRequests` | `accountId` | `{requests}` |
| `acceptRequest` | `accountId, conversationId` | `{accepted}` |
| `declineRequest` | `accountId, conversationId` | `{declined}` |
| `addContact` | `accountId, uri` | `{added}` |
| `removeContact` | `accountId, uri` | `{removed}` |
| `sendTrustRequest` | `accountId, uri` | `{sent}` |
| `listTrustRequests` | `accountId` | `{requests}` |

**Event notifications** (pushed by bridge):

| Method | When |
|--------|------|
| `onMessageReceived` | New message in a conversation |
| `onRegistrationChanged` | Account registration status changed |
| `onConversationRequestReceived` | New conversation invite received |

Shutdown: send `EOF` on stdin, or call the `shutdown` method.

### CLI (one-shot commands)

```bash
# List accounts
./jami-bridge --list-accounts

# List conversations
./jami-bridge --list-conversations --account ACCOUNT_ID

# Send a message
./jami-bridge --send-message --account ACCOUNT_ID --conversation CONV_ID --body "Hello!"

# Load recent messages
./jami-bridge --load-messages --account ACCOUNT_ID --conversation CONV_ID --count 20
```

All CLI commands print JSON to stdout and exit. The daemon starts, runs the
command, and shuts down.

### Hook (event-driven scripting)

The simplest way to react to Jami events — run any command when a message arrives.
No HTTP server, no STDIO protocol, no programming language required.

```bash
# Run a shell script on every message
./jami-bridge --hook ./on-message.sh

# Run a Python bot
./jami-bridge --hook "python3 bot.py"

# Combine with HTTP for bidirectional use
./jami-bridge --port 8090 --hook "python3 bot.py"

# Handle all event types
./jami-bridge --hook ./handler.sh --hook-events onMessageReceived,onConversationRequestReceived

# Custom timeout
./jami-bridge --hook "python3 slow_bot.py" --hook-timeout 60
```

**How it works:** When an event arrives, the bridge spawns the hook command. The event
data is available as:

| Input | Description |
|-------|-------------|
| `$JAMI_EVENT` | Full event JSON (recommended — no escaping issues) |
| stdin | Same JSON piped to the command's stdin |
| `$JAMI_EVENT_TYPE` | Event type: `onMessageReceived`, `onConversationRequestReceived`, etc. |
| `$JAMI_ACCOUNT_ID` | Account ID |
| `$JAMI_CONVERSATION_ID` | Conversation ID |

**Replies:** If the hook prints JSON to stdout, the bridge can send messages back:

```json
{"reply": "Got it!"}
```

```json
{"replies": ["Part 1...", "Part 2...", "Part 3..."]}
```

If stdout is empty or not JSON, no message is sent. Hook stderr goes to the
bridge stderr (visible in logs).

**Example — Shell script:**

```bash
#!/bin/bash
# on-message.sh — echo bot
BODY=$(echo "$JAMI_EVENT" | python3 -c "import sys,json; print(json.load(sys.stdin)['body'])")
printf '{"reply": "Echo: %s"}\n' "$BODY"
```

**Example — Python bot:**

```python
#!/usr/bin/env python3
import json, os, sys

# Read event from env var (no stdin parsing needed)
event = json.loads(os.environ['JAMI_EVENT'])

if event['type'] == 'onMessageReceived':
    body = event.get('body', '')
    # Reply back to the same conversation
    print(json.dumps({"reply": f"You said: {body}"}))
```

**Event types** (`--hook-events`):

| Type | Description |
|------|-------------|
| `onMessageReceived` | New message in a conversation |
| `onConversationRequestReceived` | Incoming conversation invite |
| `onTrustRequestReceived` | Incoming contact/trust request |
| `onConversationReady` | Conversation finished loading |
| `onMessageStatusChanged` | Message delivery status changed |
| `onRegistrationChanged` | Account registration status changed |
| `all` | All events |

Default: `onMessageReceived` only.

**Event JSON example:**

```json
{
  "type": "onMessageReceived",
  "accountId": "<account-id>",
  "conversationId": "<conversation-id>",
  "from": "<sender-uri>",
  "body": "Hello!",
  "id": "abc123",
  "timestamp": ""
}
```

Each event spawns a fresh process — no state leaks, no threading concerns.
The hook command runs via `/bin/sh -c`, so pipes and redirects work.

### Invite Policy

By default, the bridge acts as a **passive bridge** — it emits conversation/trust
request events but takes no action. This lets consumers (bots, scripts) decide
what to do.

For convenience (especially with `--hook` mode where the hook can't call back
into the bridge), three invite policy flags are available:

| Flag | Behavior |
|------|----------|
| *(none)* | **Passive** — emit events, no accept/decline |
| `--auto-accept` | Accept ALL incoming invites (anyone can add this bot) |
| `--auto-accept-from URI` | Accept invites only from this Jami URI (owner), decline others |
| `--reject-unknown` | Reject ALL incoming requests (lockdown mode) |

**Typical workflow:**

```bash
# Setup phase: accept from you only, so you can add the bot to groups
./jami-bridge --hook bot.py --auto-accept-from <your-jami-uri>

# Production: reject everyone, only respond to messages in existing rooms
./jami-bridge --hook bot.py --reject-unknown
```

**Config file:**

```json
{
  "autoAccept": true,
  "autoAcceptFrom": "<your-jami-uri>",
  "rejectUnknown": false
}
```

Priority: `--auto-accept` > `--auto-accept-from` > `--reject-unknown`.

The policy applies to both *conversation requests* (group invites) and
*trust requests* (contact requests). Stats are tracked:
`invites_accepted` and `invites_declined` in `GET /api/stats`.

---

## Data & Profile

The daemon stores account data, conversation history, and cache using the
[XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html):

| Data | Default path | Override |
|------|-------------|----------|
| Account data | `~/.local/share/jami/` | `--data-dir` or `XDG_DATA_HOME` |
| Config | `~/.config/jami/` | `--config-dir` or `XDG_CONFIG_HOME` |
| Cache | `~/.cache/jami/` | `--cache-dir` or `XDG_CACHE_HOME` |

### Using an existing Jami account

If the Jami desktop app (Flatpak) is already configured with an account, the
bridge will automatically find and use it — they share the same data directory.
This means:

- **Same account, same conversations** — messages sent via bridge appear in the
  desktop app and vice versa
- **Don't run both simultaneously** — the daemon opens network ports and
  manages state; running two instances on the same account can cause conflicts

### Using a separate profile

To use a separate Jami identity (e.g., for a bot that shouldn't share the
desktop app's account):

```bash
# Create isolated directories
mkdir -p /tmp/jami-bot/{data,config,cache}

# Run with custom XDG paths
XDG_DATA_HOME=/tmp/jami-bot/data \
XDG_CONFIG_HOME=/tmp/jami-bot/config \
XDG_CACHE_HOME=/tmp/jami-bot/cache \
  ./jami-bridge --port 8091

# Or via STDIO
XDG_DATA_HOME=/tmp/jami-bot/data \
XDG_CONFIG_HOME=/tmp/jami-bot/config \
XDG_CACHE_HOME=/tmp/jami-bot/cache \
  ./jami-bridge --stdio
```

Then create a new account via the API:

```bash
# HTTP
curl -4 -X POST http://127.0.0.1:8091/api/accounts -d '{"alias":"my-bot"}'

# STDIO
echo '{"jsonrpc":"2.0","method":"createAccount","params":{"alias":"my-bot"},"id":1}' | ...
```

The same can be done with CLI flags (no env vars needed):

```bash
# Use separate data dirs
./jami-bridge --data-dir /tmp/jami-bot/data \
            --config-dir /tmp/jami-bot/config \
            --cache-dir /tmp/jami-bot/cache \
            --port 8091

# Create a new account
./jami-bridge --data-dir /tmp/jami-bot/data \
            --account new --account-alias my-bot
```

### Account Management

The `--account` flag controls which Jami account the bridge uses:

| `--account` value | Behavior |
|---|---|
| *(not specified)* | Use first existing account, or create new one |
| `<account-id>` | Use specific account by ID |
| `new` | Always create a new account |
| `archive:///path/to/file.gz` | Import account from archive |
| `/path/to/file.gz` | Import if file exists; create new + export if not |

```bash
# Auto-detect (use existing or create new)
./jami-bridge

# Use specific account
./jami-bridge --account <account-id>

# Import from archive (backup file from Jami desktop app)
./jami-bridge --account archive:///path/to/jami-backup.gz

# Always create new
./jami-bridge --account new --account-alias my-bot

# Create-or-reuse: first run creates + exports, subsequent runs import
./jami-bridge --account /tmp/jami-bot.gz

# Import with password
./jami-bridge --account /path/to/export.gz --account-password secret123
```

### Config File

All settings can be specified in a JSON config file:

```json
{
  "host": "127.0.0.1",
  "port": 8090,
  "dataDir": "/opt/jami/data",
  "configDir": "/opt/jami/config",
  "cacheDir": "/opt/jami/cache",
  "account": "<account-id>",
  "debug": false,
  "hook": {
    "command": "python3 /opt/jami/bot.py",
    "events": "onMessageReceived,onConversationReady",
    "timeout": 30
  }
}
```

```bash
./jami-bridge --config /etc/jami/bridge.json
```

CLI arguments override config file values.

### Docker / Podman

```bash
# Run in a container with persistent data
podman run -d \
  -p 8090:8090 \
  -v jami-data:/data \
  -e XDG_DATA_HOME=/data \
  -e XDG_CONFIG_HOME=/data/config \
  -e XDG_CACHE_HOME=/data/cache \
  jami-bridge
```

---

## Python Bot Example

The `examples/pi-bot/` directory contains a complete chat bot that bridges
Jami conversations to the [pi](https://github.com/nicoschmdt/pi) AI coding agent.

### How it works

```
Jami user ↔ jami-bridge (STDIO) ↔ bot.py ↔ pi
```

The bot launches `jami-bridge --stdio` as a subprocess and communicates via
JSON-RPC. **No HTTP server, no polling** — events are pushed in real-time.

### Quick start

```bash
cd examples/pi-bot

# Auto-detect account and conversation
python3 bot.py --jami /path/to/jami-bridge

# Specify conversation
python3 bot.py --jami /path/to/jami-bridge --conversation CONV_ID

# Dry-run (don't call pi, just log)
python3 bot.py --jami /path/to/jami-bridge --dry-run
```

### Bot options

| Flag | Default | Description |
|------|---------|-------------|
| `--jami PATH` | `jami-bridge` | Path to jami-bridge binary |
| `--account ID` | auto-detect | Jami account ID |
| `--conversation ID` | auto-detect | Conversation to monitor |
| `--history N` | `20` | Recent messages to include as context |
| `--session-dir DIR` | `/tmp/jami-bot-sessions` | pi session files directory |
| `--system-prompt TEXT` | (built-in) | System prompt for pi |
| `--no-session` | off | Disable pi sessions (stateless) |
| `--no-ack` | off | Don't send acknowledgment messages |
| `--pi-args ARGS` | (none) | Extra arguments passed to pi CLI |
| `--dry-run` | off | Log messages without calling pi |

### Bot features

- **Real-time events** via STDIO JSON-RPC (no polling, no HTTP)
- **Per-conversation pi sessions** — persistent context with autocompact
- **Acknowledgment messages** — "💭 received, thinking..." (filtered from pi context)
- **Silence mode** — pi can choose not to respond in group chats (`__SILENT__`)
- **Sender-aware formatting** — shows who said what
- **Conversation history** — injects recent messages on first session

See [examples/pi-bot/README.md](examples/pi-bot/README.md) for full details.

---

## Building

All compilation happens inside Podman containers — **nothing is built on the host**.

### Prerequisites

- Podman (or Docker with equivalent commands)
- ~5 GB disk space for the base image

### Development workflow (recommended)

The fastest way to iterate on the bridge. Source is mounted from the host,
so only changed files are recompiled. Uses the existing dev container
with a mounted output directory — no image rebuilds, no `podman create/cp/rm`
extraction dance.

**One-time setup:**

```bash
./build.sh base    # Build the base image (~10-20 min)
```

**Build + run cycle:**

```bash
# Rebuild the binary (incremental — only changed .cpp files recompile)
./build.sh dev

# Run the binary in a lightweight runtime container
./build.sh dev-run [--port 8091]
```

**Build + dist cycle (for testing the self-contained distribution):**

```bash
# Rebuild → bundle into dist output (same container, no image rebuild)
./build.sh dev && ./build.sh dev-dist

# Run the dist binary directly on the host
./jami-bridge-dist-output/jami-bridge-dist/jami-bridge --help
```

This is the `dev` equivalent of `make clean && make build`:
`dev` recompiles, `dev-dist` bundles the fresh binary + libs into
`jami-bridge-dist-output/`.

**All dev commands:**

| Command | Description |
|---------|-------------|
| `./build.sh dev` | Compile bridge in dev container (incremental, <8s on rebuild) |
| `./build.sh dev-run [args]` | Run dev binary in runtime container (port forwarding, signals) |
| `./build.sh dev-dist` | Bundle binary + libs into `jami-bridge-dist-output/` |
| `./build.sh dev-shell` | Shell into dev container |
| `./build.sh dev-kill` | Stop dev container |
| `./build.sh dev-clean` | Remove dev container and build dir |

### CI / release builds

Full image builds — for tagged releases or when you need the production
distribution tarball via multi-stage build.

```bash
# Production bridge + dist image (multi-stage, ~5 min)
./build.sh dist

# Test the dist in a fresh Fedora container
./build.sh test-dist
```

The `dist` target builds a `jami-bridge-dist` container image with the
tarball at `/dist/jami-bridge-dist.tar.gz`. Extract it:

```bash
podman create --name extract jami-bridge-dist
podman cp extract:/dist/jami-bridge-dist.tar.gz .
podman rm extract
tar xzf jami-bridge-dist.tar.gz
./jami-bridge-dist/jami-bridge --help
```

Produces a self-contained directory:

```
jami-bridge-dist/          (243 MB extracted, 92 MB compressed)
├── jami-bridge            3.2 MB binary (RPATH: $ORIGIN/lib)
└── lib/
    ├── libjami.so.16.0.0       237 MB  Jami daemon
    ├── libgit2.so.1.9.2         1.3 MB  Git support
    ├── libsecp256k1.so.5.0.0    1.3 MB  Crypto
    └── libllhttp.so.9.3.1        80 KB  HTTP parser
```

### How the RPATH works

The binary has `$ORIGIN/lib` embedded in its ELF `RUNPATH`. When the dynamic
linker loads the binary, `$ORIGIN` resolves to the directory containing the
binary, so `./lib/` is searched automatically.

Transitive dependencies also work: each `.so` file in `lib/` has `$ORIGIN`
set as its RUNPATH (via `patchelf`), so `libgit2.so` finds `libllhttp.so`
in the same directory. This is necessary because modern glibc's `RUNPATH`
does **not** cascade to transitive dependencies.

Result: **just extract the tarball and run** — no environment variables,
no wrapper scripts, no system package installs — all non-glibc libs are bundled.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    jami-bridge                          │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ HTTP     │  │ STDIO    │  │ CLI (one-shot)   │  │
│  │ REST API │  │ JSON-RPC │  │ list-accounts    │  │
│  │ (cpp-httplib)│ (stdin/stdout)│  send-message   │  │
│  └────┬─────┘  └────┬─────┘  └───────┬──────────┘  │
│       │              │                │              │
│  ┌────┴──────────────┴────────────────┘              │
│  │                                                │  │
│  │   ┌──────────┐                                │  │
│  │   │ Hook     │ Event ──► spawn command       │  │
│  │   │ ($JAMI_EVENT, stdin, reply)               │  │
│  │   └──────────┘                                │  │
│  │                                                │  │
│  └────────┬───────────────────────────────────────┘  │
│           │                                          │
│   ┌───────▼───────┐                                │
│   │  Client API   │                                │
│   │  (C++ wrapper)│                                │
│   └───────┬───────┘                                │
│           │                                          │
│   ┌───────▼───────┐                                │
│   │  libjami.so   │  (in-process daemon)           │
│   └───────────────┘                                │
└──────────────────────────────────────────────────────┘
```

- **Single process** — the Jami daemon runs in-process via `libjami.so` (same
  architecture as Android/iOS/Windows clients)
- **No DBus** — library mode, not the DBus interface used by the Linux desktop app
- **Cross-platform** — C++17, no platform-specific code except `httplib.h`
  which handles Windows/Unix differences

---

## Ports

Default: **8090** (avoids common port conflicts).

Change with `--port`:
```bash
./jami-bridge --port 3000
```

---

## Troubleshooting

### "libjami.so.16: cannot open shared object file"

The `lib/` directory must be next to the binary:
```
my-dir/
├── jami-bridge
└── lib/
    ├── libjami.so.16.0.0
    └── ...
```

Verify RPATH:
```bash
readelf -d jami-bridge | grep RPATH
# Should show: $ORIGIN/lib
```

### "error while loading shared libraries: libllhttp.so.9.3"

All four `.so` files must be in `lib/`, including symlinks. Verify:
```bash
ldd jami-bridge | grep "not found"
# Should show nothing
```

### "Account not registered" / empty account list

- The daemon needs a few seconds to register on the Jami network after startup
- If using a separate profile (`XDG_DATA_HOME`), you need to create an account first
- If sharing the desktop app's data, make sure the desktop app isn't running simultaneously

### Port already in use

```bash
./jami-bridge --port 3000    # Use a different port
```

---

## License

**GPL-3.0** — See [LICENSE](LICENSE).

jami-bridge links directly against `libjami.so` (GPL-3.0, Copyright © 2004-2026
Savoir-faire Linux Inc.) and bundles it in the distribution package, forming a
GPL combined work. The entire distribution (binary + libjami.so) is GPL-3.0.

Third-party components: cpp-httplib (MIT), nlohmann/json (MIT), libgit2
(GPL-2.0+exception), libsecp256k1 (MIT), libllhttp (MIT). All compatible with GPL-3.0.
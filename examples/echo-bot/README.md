# echo-bot: Simple Shell Hook Bot

The simplest possible Jami bot — a shell script that echoes messages back.

## Usage

```bash
# Make sure jami-sdk is in PATH or specify full path
cd jami-sdk-dist

# Run with echo bot
./jami-sdk --hook ../examples/echo-bot/echo-bot.sh

# Or with HTTP server also running
./jami-sdk --port 8090 --hook ../examples/echo-bot/echo-bot.sh
```

## How It Works

1. `jami-sdk --hook` runs the daemon and waits for events
2. When a message arrives, it spawns `echo-bot.sh`
3. The script reads `$JAMI_EVENT` (full event JSON)
4. It prints `{"reply": "..."}` to stdout
5. The SDK sends the reply to the same conversation

## Customization

Edit `echo-bot.sh` to change the reply logic. The script has access to:

| Variable | Description |
|----------|-------------|
| `$JAMI_EVENT` | Full event JSON (recommended) |
| `$JAMI_EVENT_TYPE` | `onMessageReceived`, `onConversationRequestReceived`, etc. |
| `$JAMI_ACCOUNT_ID` | Account ID |
| `$JAMI_CONVERSATION_ID` | Conversation ID |
| stdin | Same JSON as `$JAMI_EVENT` |

## Reply Format

Print JSON to stdout:

```json
{"reply": "Simple text reply"}
```

Or multiple messages:

```json
{"replies": ["Part 1", "Part 2", "Part 3"]}
```

No output or non-JSON output: no reply sent.

## See Also

- [pi-bot](../pi-bot/) — Full-featured Python bot using STDIO/JSON-RPC
- [README](../../README.md) — SDK documentation with hook reference
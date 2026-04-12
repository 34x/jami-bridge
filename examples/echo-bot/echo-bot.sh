#!/bin/bash
# echo-bot.sh — Simple Jami echo bot using --hook
#
# Usage:
#   ./jami-bridge --hook ./echo-bot.sh
#   ./jami-bridge --port 8090 --hook ./echo-bot.sh  # HTTP + hook
#
# The hook receives event data via:
#   $JAMI_EVENT       — Full event JSON (recommended)
#   $JAMI_EVENT_TYPE  — "onMessageReceived", etc.
#   $JAMI_ACCOUNT_ID  — Account ID
#   $JAMI_CONVERSATION_ID — Conversation ID
#   stdin              — Same JSON piped to stdin
#
# The hook can reply by printing JSON to stdout:
#   {"reply": "message text"}             — send one message
#   {"replies": ["msg1", "msg2"]}         — send multiple messages

set -euo pipefail

# Only handle messages (but --hook-events defaults to this)
if [ "$JAMI_EVENT_TYPE" != "onMessageReceived" ]; then
    exit 0
fi

# Parse the event JSON to extract the message body
# Using python3 for JSON parsing (available on most systems)
BODY=$(echo "$JAMI_EVENT" | python3 -c "
import sys, json
event = json.load(sys.stdin)
print(event.get('body', ''))
" 2>/dev/null || echo "")

FROM=$(echo "$JAMI_EVENT" | python3 -c "
import sys, json
event = json.load(sys.stdin)
print(event.get('from', 'unknown')[:8])
" 2>/dev/null || echo "unknown")

# Log to stderr (visible in jami-bridge logs)
echo "[echo-bot] Received from $FROM: $BODY" >&2

# Reply with an echo
if [ -n "$BODY" ]; then
    printf '{"reply": "Echo from %s: %s"}\n' "$FROM" "$BODY"
fi
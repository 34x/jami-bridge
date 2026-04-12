#pragma once
/// @file hook.h
/// @brief External hook execution — runs a command on events,
///        passes event JSON via env var + stdin, reads stdout for replies.
///
/// NOTE: Hook support is POSIX-only (uses fork/exec/pipes).
/// On Windows, use STDIO or HTTP mode for event processing instead.
///
/// Usage:
///   jami-sdk --hook "python3 bot.py"
///   jami-sdk --hook "./on-message.sh"
///   jami-sdk --port 8090 --hook "python3 bot.py"  # HTTP + hook
///   jami-sdk --hook-events onMessageReceived,onConversationReady "./bot.sh"
///
/// Hook protocol:
///   1. SDK spawns the hook command for each matching event
///   2. Event JSON is available as:
///      - Environment variable:  $JAMI_EVENT  (recommended)
///      - Stdin pipe:             piped as JSON + EOF
///   3. Hook's stdout is read for a JSON response (optional)
///   4. If response contains {"reply": "text"}, sends message to same conversation
///   5. If response contains {"replies": ["a", "b"]}, sends multiple messages
///
/// Environment variables set for the hook:
///   JAMI_EVENT            — Full event JSON (recommended — no escaping issues)
///   JAMI_EVENT_TYPE       — "onMessageReceived", "onConversationReady", etc.
///   JAMI_ACCOUNT_ID       — Account ID
///   JAMI_CONVERSATION_ID  — Conversation ID (if applicable)
///
/// The hook command runs in a shell (/bin/sh -c), so pipes and redirects work.
/// Hook stderr goes to the SDK's stderr (visible in logs).
/// If the hook times out (--hook-timeout), it's killed with SIGKILL.

#ifndef _WIN32

#include "client.h"  // For Events, Client, Message

#include <string>
#include <vector>

namespace jami {

/// Result of executing a hook command.
struct HookResult {
    std::string output;       ///< Hook's stdout (trimmed)
    int exit_code = -1;       ///< Process exit code (-1 if error or killed)
    bool timed_out = false;   ///< Whether the process was killed due to timeout
};

/// Execute a command, passing event JSON as env var + stdin.
///
/// @param command          Shell command to execute (via /bin/sh -c)
/// @param event_json       JSON string (set as JAMI_EVENT env var + piped to stdin)
/// @param timeout_seconds  Max seconds to wait (0 = no timeout)
/// @param env_vars         Additional environment variables (KEY=VALUE pairs)
/// @returns                HookResult with output and status
HookResult run_hook_command(const std::string& command,
                              const std::string& event_json,
                              int timeout_seconds = 30,
                              const std::vector<std::string>& env_vars = {});

/// Hook manager — registers event callbacks on a Client and dispatches to hooks.
///
/// When an event matches (based on --hook-events filter), the hook command
/// is spawned in a detached thread. The hook's stdout is parsed for response
/// actions (reply/replies) which are sent back to the originating conversation.
class HookManager {
public:
    /// Create a hook manager.
    /// @param client    Client instance (for sending replies)
    /// @param command   Shell command to execute for each event
    /// @param events    Comma-separated event types to handle
    /// @param timeout   Hook process timeout in seconds (0 = no timeout)
    HookManager(Client& client, const std::string& command,
                const std::string& events = "onMessageReceived",
                int timeout = 30);

    /// Install event callbacks on the Events struct.
    /// Wraps existing callbacks to also dispatch to the hook.
    /// After calling this, call client.update_callbacks(events) to apply.
    void install_callbacks(Events& events);

private:
    Client& client_;
    std::string command_;
    int timeout_;

    /// Which event types to dispatch. True = dispatch to hook.
    bool handle_message_ = false;
    bool handle_conversation_request_ = false;
    bool handle_trust_request_ = false;
    bool handle_registration_changed_ = false;
    bool handle_conversation_ready_ = false;
    bool handle_message_status_changed_ = false;

    /// Dispatch an event to the hook command.
    /// Runs in a detached thread — never blocks the daemon.
    void dispatch(const std::string& event_type, const std::string& event_json,
                  const std::string& account_id, const std::string& conv_id);

    /// Parse hook output for response actions and execute them.
    void handle_response(const std::string& output,
                         const std::string& account_id,
                         const std::string& conv_id);
};

#endif // !_WIN32

} // namespace jami
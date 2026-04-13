/*
 *  jami-bridge — Unofficial Jami messaging bridge
 *  Copyright (C) 2025-2026 Contributors to the jami-bridge project
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/// @file hook.cpp
/// @brief Hook execution and management.
///
/// Hook commands receive event data as:
///   1. Environment variable $JAMI_EVENT (full JSON — recommended for Python etc.)
///   2. Stdin pipe (same JSON — for scripts that read stdin)
///   3. Convenience env vars: $JAMI_EVENT_TYPE, $JAMI_ACCOUNT_ID, $JAMI_CONVERSATION_ID
///
/// Hook commands respond on stdout:
///   {"reply": "text"}             → send one message to same conversation
///   {"replies": ["a", "b", ...]}  → send multiple messages to same conversation
///   Empty/non-JSON output          → no action
///
/// Each event spawns a new process (no state leaks, no threading issues).


#include "log.h"
#include "hook.h"
#include "client.h"

#include <nlohmann/json.hpp>

// Hook execution uses POSIX fork/exec/pipes.
// Windows support would need CreateProcess + named pipes.
// This is planned for a future release.
#ifdef _WIN32
#error "Hook support on Windows is not yet implemented. Use STDIO or HTTP mode instead."
#endif

#include <iostream>
#include <chrono>
#include <thread>
#include <array>
#include <algorithm>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>

using json = nlohmann::json;

namespace jami {

// ── Process execution ────────────────────────────────────────────────────

HookResult run_hook_command(const std::string& command,
                             const std::string& event_json,
                             int timeout_seconds,
                             const std::vector<std::string>& env_vars)
{
    HookResult result;

    // Create pipes: stdin (parent→child) and stdout (child→parent)
    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) < 0) {
        jami::log_tag("hook", "Failed to create stdin pipe: ", std::strerror(errno));
        return result;
    }
    if (pipe(stdout_pipe) < 0) {
        jami::log_tag("hook", "Failed to create stdout pipe: ", std::strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return result;
    }

    pid_t pid = fork();

    if (pid < 0) {
        jami::log_tag("hook", "Failed to fork: ", std::strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // ── Child process ──────────────────────────────────────────────

        close(stdin_pipe[1]);   // Close write end of stdin pipe
        close(stdout_pipe[0]); // Close read end of stdout pipe

        // Redirect stdin/stdout
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        // stderr is inherited — hook's stderr goes to SDK's stderr

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // Set extra environment variables (JAMI_EVENT, etc.)
        for (const auto& env : env_vars) {
            auto eq_pos = env.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = env.substr(0, eq_pos);
                std::string val = env.substr(eq_pos + 1);
                setenv(key.c_str(), val.c_str(), 1);
            }
        }

        // Execute the hook command via shell
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    // ── Parent process ──────────────────────────────────────────────────

    close(stdin_pipe[0]);   // Close read end (parent writes)
    close(stdout_pipe[1]); // Close write end (parent reads)

    // Write event JSON to child's stdin, then close to signal EOF.
    // Event JSON is typically <1KB, well within pipe buffer (64KB on Linux).
    ssize_t total_written = 0;
    ssize_t to_write = static_cast<ssize_t>(event_json.size());
    const char* data = event_json.data();
    while (total_written < to_write) {
        ssize_t n = write(stdin_pipe[1], data + total_written, to_write - total_written);
        if (n < 0) {
            if (errno == EPIPE || errno == EINTR) break;
            jami::log_tag("hook", "Write error: ", std::strerror(errno));
            break;
        }
        total_written += n;
    }
    close(stdin_pipe[1]);  // Signal EOF to child

    // Read child's stdout with timeout using poll()
    std::string output;
    std::array<char, 4096> buf;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(
        timeout_seconds > 0 ? timeout_seconds : 3600
    );

    while (true) {
        int remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
        ).count();

        if (remaining_ms <= 0 && timeout_seconds > 0) {
            jami::log_tag("hook", "Timed out after ", timeout_seconds, "s, killing process ", pid);
            kill(pid, SIGKILL);
            result.timed_out = true;
            break;
        }

        struct pollfd pfd;
        pfd.fd = stdout_pipe[0];
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, std::min(remaining_ms > 0 ? remaining_ms : 1000, 1000));

        if (ret < 0) {
            if (errno == EINTR) continue;
            jami::log_tag("hook", "Poll error: ", std::strerror(errno));
            break;
        }

        if (ret == 0) {
            // No data — check if child is still alive
            int status;
            if (waitpid(pid, &status, WNOHANG) == pid) {
                result.exit_code = WEXITSTATUS(status);
                // Try to read remaining output
                while (true) {
                    ssize_t n = read(stdout_pipe[0], buf.data(), buf.size());
                    if (n <= 0) break;
                    output.append(buf.data(), n);
                }
                break;
            }
            continue;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = read(stdout_pipe[0], buf.data(), buf.size());
            if (n <= 0) break;
            output.append(buf.data(), n);
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            // Read remaining data before closing
            while (true) {
                ssize_t n = read(stdout_pipe[0], buf.data(), buf.size());
                if (n <= 0) break;
                output.append(buf.data(), n);
            }
            break;
        }
    }

    close(stdout_pipe[0]);

    // Wait for child to finish
    int status = 0;
    waitpid(pid, &status, 0);
    if (result.exit_code == -1) {
        result.exit_code = WEXITSTATUS(status);
    }

    // Trim trailing whitespace
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                                output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }

    result.output = output;
    return result;
}

// ── HookManager ──────────────────────────────────────────────────────────

HookManager::HookManager(Client& client, const std::string& command,
                         const std::string& events, int timeout,
                         int max_concurrent)
    : client_(client), command_(command), timeout_(timeout)
    , max_concurrent_(max_concurrent), active_hooks_(0)
{
    // Parse comma-separated event types
    std::string remaining = events;
    while (!remaining.empty()) {
        auto comma = remaining.find(',');
        std::string token = remaining.substr(0, comma);
        // Trim whitespace
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();

        if (token == "onMessageReceived" || token == "all") handle_message_ = true;
        else if (token == "onConversationRequestReceived" || token == "all") handle_conversation_request_ = true;
        else if (token == "onTrustRequestReceived" || token == "all") handle_trust_request_ = true;
        else if (token == "onRegistrationChanged" || token == "all") handle_registration_changed_ = true;
        else if (token == "onConversationReady" || token == "all") handle_conversation_ready_ = true;
        else if (token == "onConversationMemberEvent" || token == "all") handle_conversation_member_event_ = true;
        else if (token == "onMessageStatusChanged" || token == "all") handle_message_status_changed_ = true;
        else if (!token.empty()) {
            jami::log_tag("hook", "Unknown event type: ", token, " (supported: onMessageReceived, onConversationRequestReceived,", " onTrustRequestReceived, onRegistrationChanged, onConversationReady,", " onConversationMemberEvent, onMessageStatusChanged, all)");
        }

        if (comma == std::string::npos) break;
        remaining = remaining.substr(comma + 1);
    }

    // "all" enables everything
    // (already set above)
}

void HookManager::install_callbacks(Events& events) {
    if (handle_message_) {
        auto orig_cb = events.on_message_received;
        events.on_message_received = [this, orig_cb](const Message& msg) {
            if (orig_cb) orig_cb(msg);

            json event_json;
            event_json["eventType"] = "onMessageReceived";
            event_json["accountId"] = msg.account_id;
            event_json["conversationId"] = msg.conversation_id;
            event_json["from"] = msg.from;
            event_json["body"] = msg.body;
            event_json["id"] = msg.id;
            event_json["messageType"] = msg.type;
            event_json["timestamp"] = msg.timestamp;
            event_json["parentId"] = msg.parent_id;

            dispatch("onMessageReceived", event_json.dump(),
                     msg.account_id, msg.conversation_id);
        };
    }

    if (handle_conversation_request_) {
        auto orig_cb = events.on_conversation_request_received;
        events.on_conversation_request_received =
            [this, orig_cb](const std::string& account_id,
                           const std::string& conv_id,
                           const std::map<std::string, std::string>& metadatas) {
            if (orig_cb) orig_cb(account_id, conv_id, metadatas);

            json event_json;
            event_json["type"] = "onConversationRequestReceived";
            event_json["accountId"] = account_id;
            event_json["conversationId"] = conv_id;
            // Include metadata if available
            for (const auto& [k, v] : metadatas) {
                event_json["metadata"][k] = v;
            }

            dispatch("onConversationRequestReceived", event_json.dump(),
                     account_id, conv_id);
        };
    }

    if (handle_registration_changed_) {
        auto orig_cb = events.on_registration_changed;
        events.on_registration_changed =
            [this, orig_cb](const std::string& account_id,
                           const std::string& state,
                           int code,
                           const std::string& detail) {
            if (orig_cb) orig_cb(account_id, state, code, detail);

            json event_json;
            event_json["type"] = "onRegistrationChanged";
            event_json["accountId"] = account_id;
            event_json["state"] = state;
            event_json["code"] = code;
            event_json["detail"] = detail;

            dispatch("onRegistrationChanged", event_json.dump(),
                     account_id, "");
        };
    }

    if (handle_conversation_ready_) {
        auto orig_cb = events.on_conversation_ready;
        events.on_conversation_ready =
            [this, orig_cb](const std::string& account_id,
                           const std::string& conv_id) {
            if (orig_cb) orig_cb(account_id, conv_id);

            json event_json;
            event_json["type"] = "onConversationReady";
            event_json["accountId"] = account_id;
            event_json["conversationId"] = conv_id;

            dispatch("onConversationReady", event_json.dump(),
                     account_id, conv_id);
        };
    }

    if (handle_conversation_member_event_) {
        auto orig_cb = events.on_conversation_member_event;
        events.on_conversation_member_event =
            [this, orig_cb](const std::string& account_id,
                           const std::string& conv_id,
                           const std::string& member_uri,
                           int event) {
            if (orig_cb) orig_cb(account_id, conv_id, member_uri, event);

            json event_json;
            event_json["type"] = "onConversationMemberEvent";
            event_json["accountId"] = account_id;
            event_json["conversationId"] = conv_id;
            event_json["memberUri"] = member_uri;
            event_json["event"] = event;

            dispatch("onConversationMemberEvent", event_json.dump(),
                     account_id, conv_id);
        };
    }

    if (handle_message_status_changed_) {
        auto orig_cb = events.on_message_status_changed;
        events.on_message_status_changed =
            [this, orig_cb](const std::string& account_id,
                           const std::string& conversation_id,
                           const std::string& peer,
                           const std::string& message_id,
                           int state) {
            if (orig_cb) orig_cb(account_id, conversation_id, peer, message_id, state);

            json event_json;
            event_json["type"] = "onMessageStatusChanged";
            event_json["accountId"] = account_id;
            event_json["conversationId"] = conversation_id;
            event_json["peer"] = peer;
            event_json["messageId"] = message_id;
            event_json["state"] = state;

            dispatch("onMessageStatusChanged", event_json.dump(),
                     account_id, conversation_id);
        };
    }

    if (handle_trust_request_) {
        auto orig_cb = events.on_trust_request_received;
        events.on_trust_request_received =
            [this, orig_cb](const std::string& account_id,
                           const std::string& from_uri,
                           const std::string& conv_id) {
            if (orig_cb) orig_cb(account_id, from_uri, conv_id);

            json event_json;
            event_json["type"] = "onTrustRequestReceived";
            event_json["accountId"] = account_id;
            event_json["from"] = from_uri;
            event_json["conversationId"] = conv_id;

            dispatch("onTrustRequestReceived", event_json.dump(),
                     account_id, conv_id);
        };
    }
}

void HookManager::dispatch(const std::string& event_type,
                            const std::string& event_json,
                            const std::string& account_id,
                            const std::string& conv_id)
{
    // Wait for a concurrency slot (bounded semaphore pattern)
    // This prevents unbounded thread creation during message floods.
    {
        std::unique_lock<std::mutex> lock(concurrency_mtx_);
        concurrency_cv_.wait(lock, [this]{ return active_hooks_ < max_concurrent_; });
        active_hooks_++;
    }

    // Spawn a thread so we don't block the daemon's event loop.
    // Each hook invocation is a separate process — no state leaks.
    std::string cmd = command_;
    int timeout = timeout_;
    std::string acc_id = account_id;
    std::string cid = conv_id;

    std::thread([this, cmd, event_json, event_type, acc_id, cid, timeout]() {
        // Release the concurrency slot when done
        struct SlotGuard {
            HookManager* hm;
            ~SlotGuard() {
                {
                    std::lock_guard<std::mutex> lock(hm->concurrency_mtx_);
                    hm->active_hooks_--;
                }
                hm->concurrency_cv_.notify_one();
            }
        } slot_guard{this};

        try {
            jami::log_tag("hook", "Dispatching ", event_type, " to: ", cmd);

            client_.stats().hook_invocations++;

            // Build environment variables
            std::vector<std::string> env_vars = {
                "JAMI_EVENT_TYPE=" + event_type,
                "JAMI_ACCOUNT_ID=" + acc_id,
                "JAMI_CONVERSATION_ID=" + cid,
                "JAMI_EVENT=" + event_json,
            };

            HookResult result = run_hook_command(cmd, event_json, timeout, env_vars);

            if (result.timed_out) {
                jami::log_tag("hook", "Hook timed out");
                client_.stats().hook_timeouts++;
                return;
            }

            if (result.exit_code != 0 && result.exit_code != -1) {
                jami::log_tag("hook", "Hook exited with code ", result.exit_code);
                client_.stats().hook_errors++;
            }

            if (!result.output.empty()) {
                handle_response(result.output, acc_id, cid);
            }
        } catch (const std::exception& e) {
            jami::log_tag("hook", "Error: ", e.what());
        }
    }).detach();
}

void HookManager::handle_response(const std::string& output,
                                   const std::string& account_id,
                                   const std::string& conv_id)
{
    json response;
    try {
        response = json::parse(output);
    } catch (const json::parse_error& e) {
        // Not JSON — just log it. This is fine for scripts that don't respond.
        jami::log_tag("hook", "Output (not JSON): ", output.substr(0, 200));
        return;
    }

    bool sent_reply = false;

    // Handle "reply" — send a single message back
    if (response.contains("reply") && response["reply"].is_string()) {
        if (account_id.empty() || conv_id.empty()) {
            jami::log_tag("hook", "Cannot reply: missing accountId or conversationId");
            return;
        }
        std::string reply_text = response["reply"].get<std::string>();
        jami::log_tag("hook", "Reply: ", reply_text.substr(0, 100));
        client_.send_message(account_id, conv_id, reply_text);
        client_.stats().hook_replies++;
        sent_reply = true;
    }

    // Handle "replies" — send multiple messages back
    if (response.contains("replies") && response["replies"].is_array()) {
        if (account_id.empty() || conv_id.empty()) {
            jami::log_tag("hook", "Cannot reply: missing accountId or conversationId");
            return;
        }
        for (const auto& r : response["replies"]) {
            if (r.is_string()) {
                client_.send_message(account_id, conv_id, r.get<std::string>());
                client_.stats().hook_replies++;
                sent_reply = true;
            }
        }
    }

    if (!sent_reply && !response.contains("reply") && !response.contains("replies")) {
        // Valid JSON but no reply field — that's OK, just log
        jami::log_tag("hook", "JSON response (no reply): ", output.substr(0, 200));
    }
}

} // namespace jami
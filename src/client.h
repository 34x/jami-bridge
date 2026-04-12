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

#pragma once
/// @file client.h
/// @brief Minimal Jami client — wraps libjami calls for messaging and conversations.
///
/// Links against libjami (built with -Dinterfaces=library).
/// Runs the daemon in-process — no DBus needed.
///
/// The daemon's public API uses the `libjami` namespace (renamed from
/// `DRing` in recent versions). Signal callbacks use
/// `libjami::exportable_callback<libjami::SomeSignal::Type>`.

#include <jami/jami.h>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>
#include <jami/datatransfer_interface.h>

#include <functional>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace jami {

/// Information about a conversation member.
struct Member {
    std::string uri;
    std::string role;       ///< "admin" or "member"
    std::string last_displayed;
};

/// A conversation (swarm room).
struct Conversation {
    std::string id;
    std::map<std::string, std::string> info;  ///< title, mode, etc.
    std::vector<Member> members;
};

/// A received message.
struct Message {
    std::string account_id;
    std::string conversation_id;
    std::string from;
    std::string body;       ///< For swarm: body["text/plain"], for non-swarm: plain text
    std::string id;         ///< message ID
    std::string timestamp;
    std::string parent_id;  ///< reply-to (linearizedParent)
    std::string type;       ///< "text", "file", etc.
};

/// Information about a file transfer.
struct FileTransfer {
    std::string account_id;
    std::string conversation_id;
    std::string interaction_id;  ///< The message/interaction ID of the file message
    std::string file_id;         ///< The file transfer ID
    std::string display_name;    ///< Human-readable file name
    std::string path;            ///< Local file path (for outgoing: source, for incoming: destination)
    std::string mimetype;        ///< MIME type
    int64_t total_size = 0;      ///< Total file size in bytes
    int64_t bytes_progress = 0;  ///< Bytes transferred so far
    int event_code = 0;          ///< DataTransferEventCode
};

/// Callbacks for asynchronous events from the daemon.
struct Events {
    std::function<void(const Message&)> on_message_received;
    std::function<void(const std::string& account_id, const std::string& conv_id,
                       const std::string& member_uri, int event)> on_conversation_member_event;
    std::function<void(const std::string& account_id, const std::string& conv_id)> on_conversation_ready;
    std::function<void(const std::string& account_id, const std::string& conv_id,
                       const std::map<std::string, std::string>& metadatas)> on_conversation_request_received;
    std::function<void(const std::string& account_id,
                       const std::string& from_uri,
                       const std::string& conv_id)> on_trust_request_received;
    std::function<void(const std::string& account_id, const std::string& state,
                       int code, const std::string& detail)> on_registration_changed;
    std::function<void(const std::string& account_id,
                       const std::string& conversation_id,
                       const std::string& peer,
                       const std::string& message_id,
                       int state)> on_message_status_changed;
    /// Called when messages have been loaded (response to load_messages).
    std::function<void(uint32_t req_id,
                       const std::string& account_id,
                       const std::string& conv_id,
                       const std::vector<libjami::SwarmMessage>& messages)> on_messages_loaded;

    /// Called when a file transfer event occurs.
    std::function<void(const FileTransfer& transfer)> on_data_transfer_event;
};

/// Minimal Jami client — messaging and conversations only.
///
/// Initializes the daemon in-process (library mode) and exposes
/// a simple C++ API for:
///   - Account creation/management
///   - Conversation (room) creation, joining, inviting
///   - Sending and receiving text messages
///
/// No DBus needed. Cross-platform (Linux, Windows, macOS).
class Client {
public:
    /// Create and initialize the Jami client.
    /// @param events  Callbacks for async events (messages, etc.)
    /// @param debug   Enable debug logging
    explicit Client(const Events& events = {}, bool debug = false);

    /// Shut down the daemon and clean up.
    ~Client();

    // Non-copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────

    /// Is the daemon running?
    bool is_running() const { return running_; }

    // ── Account Management ──────────────────────────────────────────

    /// Create a new Jami account. Returns the account ID.
    std::string create_account(const std::string& alias = "",
                                const std::string& password = "");

    /// List all account IDs.
    std::vector<std::string> list_accounts() const;

    /// Get account details (type, username, etc.).
    std::map<std::string, std::string> account_details(const std::string& account_id) const;

    /// Set account details (e.g. alias, displayName).
    void set_account_details(const std::string& account_id,
                              const std::map<std::string, std::string>& details);

    /// Get volatile details (registration status, etc.).
    std::map<std::string, std::string> account_volatile_details(const std::string& account_id) const;

    /// Remove an account.
    void remove_account(const std::string& account_id);

    /// Update account profile (display name, avatar) — pushed to contacts.
    void update_profile(const std::string& account_id,
                        const std::string& display_name = "",
                        const std::string& avatar = "");

    /// Export account to a file (for backup/transfer).
    bool export_account(const std::string& account_id,
                         const std::string& path,
                         const std::string& password = "");

    /// Import account from a file.
    std::string import_account(const std::string& path, const std::string& password = "");

    /// Set account active/inactive (connect/disconnect).
    void set_account_active(const std::string& account_id, bool active);

    /// Re-register signal callbacks (e.g., after hook wraps existing callbacks).
    /// This is a no-op on the Client side — callbacks are registered in the
    /// constructor. When wrapping (e.g., for hooks), call this after modifying
    /// the Events struct that was already registered.
    void update_callbacks(const Events& events);

    // ── Conversations (Rooms) ───────────────────────────────────────

    /// Create a new conversation (room). Returns the conversation ID.
    std::string create_conversation(const std::string& account_id);

    /// List all conversations for an account.
    std::vector<std::string> list_conversations(const std::string& account_id) const;

    /// Get conversation info (title, mode, etc.).
    std::map<std::string, std::string> conversation_info(const std::string& account_id,
                                                           const std::string& conv_id) const;

    /// Get conversation members.
    std::vector<Member> conversation_members(const std::string& account_id,
                                              const std::string& conv_id) const;

    /// Invite someone to a conversation. Their Jami ID (hash) or registered username.
    void invite_member(const std::string& account_id,
                       const std::string& conv_id,
                       const std::string& uri);

    /// Remove someone from a conversation.
    void remove_member(const std::string& account_id,
                       const std::string& conv_id,
                       const std::string& uri);

    /// Remove (leave) a conversation.
    bool remove_conversation(const std::string& account_id,
                              const std::string& conv_id);

    /// Update conversation info (title, description, etc.).
    void update_conversation_info(const std::string& account_id,
                                   const std::string& conv_id,
                                   const std::map<std::string, std::string>& info);

    // ── Conversation Requests (Invites) ─────────────────────────────

    /// List pending conversation requests (invites).
    std::vector<std::map<std::string, std::string>> conversation_requests(
        const std::string& account_id) const;

    /// Accept a conversation request (join a room).
    void accept_request(const std::string& account_id,
                        const std::string& conv_id);

    /// Decline a conversation request.
    void decline_request(const std::string& account_id,
                         const std::string& conv_id);

    // ── Messaging ───────────────────────────────────────────────────

    /// Send a text message. Returns empty string (message ID arrives via signal).
    std::string send_message(const std::string& account_id,
                              const std::string& conv_id,
                              const std::string& body,
                              const std::string& parent_id = "");

    /// Edit a previously sent message. Returns the new message ID.
    std::string edit_message(const std::string& account_id,
                              const std::string& conv_id,
                              const std::string& new_body,
                              const std::string& edited_id);

    /// Load conversation messages (triggers on_messages_loaded callback).
    uint32_t load_messages(const std::string& account_id,
                            const std::string& conv_id,
                            const std::string& from_id = "",
                            size_t count = 64);

    /// Synchronous version: load messages and wait for the result.
    /// Returns the loaded messages, or empty vector on timeout.
    std::vector<libjami::SwarmMessage> load_messages_sync(
        const std::string& account_id,
        const std::string& conv_id,
        const std::string& from_id = "",
        size_t count = 64,
        int timeout_ms = 5000);

    // ── Contacts & Trust ────────────────────────────────────────────

    /// Send a contact/trust request to someone.
    void send_trust_request(const std::string& account_id,
                             const std::string& to_uri);

    /// Accept a trust request.
    bool accept_trust_request(const std::string& account_id,
                               const std::string& from_uri);

    /// Decline a trust request.
    bool decline_trust_request(const std::string& account_id,
                                const std::string& from_uri);

    /// List trust requests.
    std::vector<std::map<std::string, std::string>> trust_requests(
        const std::string& account_id) const;

    /// Add a contact.
    void add_contact(const std::string& account_id, const std::string& uri);

    /// Remove a contact.
    void remove_contact(const std::string& account_id, const std::string& uri, bool ban = false);

    /// List contacts for an account.
    std::vector<std::map<std::string, std::string>> list_contacts(const std::string& account_id) const;

    // ── Statistics ────────────────────────────────────────────────────

    /// Runtime statistics.
    struct Stats {
        std::chrono::steady_clock::time_point start_time;
        uint64_t messages_received = 0;
        uint64_t messages_sent = 0;
        uint64_t hook_invocations = 0;
        uint64_t hook_replies = 0;
        uint64_t hook_timeouts = 0;
        uint64_t hook_errors = 0;
        uint64_t invites_accepted = 0;
        uint64_t invites_declined = 0;
    };

    /// Get mutable reference to stats (for tracking).
    Stats& stats() { return stats_; }

    /// Get const reference to stats.
    const Stats& stats() const { return stats_; }

    // ── File Transfers ──────────────────────────────────────────────

    /// Send a file to a conversation.
    /// The file at `path` must exist on disk.
    /// `display_name` is the name shown to the recipient (can be empty to use path basename).
    /// `reply_to` is an optional message ID to reply to.
    void send_file(const std::string& account_id,
                   const std::string& conv_id,
                   const std::string& path,
                   const std::string& display_name = "",
                   const std::string& reply_to = "");

    /// Download a file from a conversation to a local path.
    /// `interaction_id` is the message ID of the file message.
    /// `file_id` is the file transfer identifier within that message.
    /// `download_path` is where to save the file on disk.
    bool download_file(const std::string& account_id,
                       const std::string& conv_id,
                       const std::string& interaction_id,
                       const std::string& file_id,
                       const std::string& download_path);

    /// Cancel an ongoing file transfer.
    libjami::DataTransferError cancel_data_transfer(const std::string& account_id,
                                                    const std::string& conv_id,
                                                    const std::string& file_id);

    /// Get info about an ongoing or completed file transfer.
    /// Returns true if the transfer was found and info was filled in.
    bool file_transfer_info(const std::string& account_id,
                            const std::string& conv_id,
                            const std::string& file_id,
                            FileTransfer& info);

    // ── Name Service ─────────────────────────────────────────────────

    /// Look up a registered name. Result comes via RegisteredNameFound signal.
    bool lookup_name(const std::string& account_id, const std::string& name);

    /// Register a name for the account. Result via NameRegistrationEnded signal.
    bool register_name(const std::string& account_id,
                       const std::string& password,
                       const std::string& name);

private:
    Events events_;
    std::atomic<bool> running_{false};
    std::mutex cb_mutex_;  ///< Protects callback registration
    Stats stats_;          ///< Runtime statistics
};

} // namespace jami
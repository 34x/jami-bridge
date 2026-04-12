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

/// @file main.cpp
/// @brief Entry point for jami-bridge — HTTP, STDIO, or CLI mode.
///
/// All configuration is explicit: CLI args, config file, or environment
/// variables. No guessing — every default can be overridden.
///
/// Account handling:
///   --account ID          Use existing account by hex ID
///   --account archive:///path/to/file.gz   Import account from archive
///   --account /path/to/file.gz             (same, shorthand)
///   --account new          Create a brand new account
///   (no --account)         Use first existing, or create new one automatically

#include "client.h"
#include "server.h"

#ifndef _WIN32
#include "hook.h"
#endif
#include "stdio_server.h"
#include "config.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

using json = nlohmann::json;

/// Global flag for signal handling
static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

/// Check if a file exists (regular file).
static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// Wait for a newly created account's Jami URI to become available.
/// The URI is assigned asynchronously after the DHT registers.
/// Returns the URI, or empty string on timeout.
static std::string wait_for_jami_uri(jami::Client& client,
                                      const std::string& account_id,
                                      int timeout_ms = 10000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto vdetails = client.account_volatile_details(account_id);
        std::string username = vdetails.count("Account.username") ? vdetails["Account.username"] : "";
        if (!username.empty()) return username;

        auto details = client.account_details(account_id);
        username = details.count("Account.username") ? details["Account.username"] : "";
        if (!username.empty()) return username;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return "";
}

/// Print the bot identity line with the Jami URI.
/// Waits briefly for the URI if it's not immediately available.
static void print_bot_identity(jami::Client& client, const std::string& account_id) {
    auto details = client.account_details(account_id);
    std::string alias = details.count("Account.alias") ? details["Account.alias"] : "";

    // Try volatile details first (populated after registration)
    auto vdetails = client.account_volatile_details(account_id);
    std::string username = vdetails.count("Account.username") ? vdetails["Account.username"] : "";

    // Fall back to static details
    if (username.empty()) {
        username = details.count("Account.username") ? details["Account.username"] : "";
    }

    // If still empty, wait for registration (up to 10s)
    if (username.empty()) {
        std::cerr << "[jami-bridge] Waiting for Jami URI..." << std::endl;
        username = wait_for_jami_uri(client, account_id);
    }

    std::cerr << "[jami-bridge] Bot identity: " << username
              << " (account: " << account_id
              << ", alias: " << alias << ")" << std::endl;
    std::cerr << "[jami-bridge] Add this bot to a group: invite " << username << std::endl;
}

/// Resolve which account to use based on config.
/// Returns the account ID to use.
/// May create or import an account if configured.
static std::string resolve_account(jami::Client& client, const jami::Config& cfg) {
    auto accounts = client.list_accounts();

    // Explicit account ID
    if (!cfg.account.empty() && cfg.account != "new"
        && cfg.account.find("archive://") != 0
        && cfg.account[0] != '/') {
        // It's an account ID — verify it exists
        for (const auto& id : accounts) {
            if (id == cfg.account) {
                std::cerr << "[jami-bridge] Using account: " << cfg.account << std::endl;
                return cfg.account;
            }
        }
        std::cerr << "[jami-bridge] Error: Account not found: " << cfg.account << std::endl;
        std::cerr << "[jami-bridge] Available accounts:" << std::endl;
        for (const auto& id : accounts) {
            auto details = client.account_details(id);
            std::cerr << "  " << id
                      << " (alias: " << (details.count("Account.alias") ? details["Account.alias"] : "") << ")"
                      << std::endl;
        }
        return "";
    }

    // Import account from archive (path exists)
    if (!cfg.account.empty() && (cfg.account.find("archive://") == 0 || cfg.account[0] == '/')) {
        std::string path = cfg.account.find("archive://") == 0
            ? cfg.account.substr(10)
            : cfg.account;

        if (file_exists(path)) {
            // Existing archive — import it
            std::cerr << "[jami-bridge] Importing account from: " << path << std::endl;
            std::string account_id = client.import_account(path, cfg.account_password);
            if (account_id.empty()) {
                std::cerr << "[jami-bridge] Error: Failed to import account" << std::endl;
                return "";
            }
            std::cerr << "[jami-bridge] Imported account: " << account_id << std::endl;
            return account_id;
        } else {
            // Path doesn't exist — create new account and export it
            std::cerr << "[jami-bridge] Creating new account (archive will be saved to: " << path << ")" << std::endl;
            std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
            std::cerr << "[jami-bridge] Created account: " << account_id << std::endl;

            // Wait for the account to be ready before exporting
            std::this_thread::sleep_for(std::chrono::seconds(3));

            bool exported = client.export_account(account_id, path, cfg.account_password);
            if (exported) {
                std::cerr << "[jami-bridge] Account exported to: " << path << std::endl;
            } else {
                std::cerr << "[jami-bridge] Warning: Failed to export account to " << path << std::endl;
            }
            return account_id;
        }
    }

    // Create new account explicitly requested
    if (cfg.account == "new") {
        std::cerr << "[jami-bridge] Creating new account (alias: " << cfg.account_alias << ")..." << std::endl;
        std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
        std::cerr << "[jami-bridge] Created account: " << account_id << std::endl;
        return account_id;
    }

    // Auto-detect: use first existing account, or create new one
    if (!accounts.empty()) {
        std::string id = accounts[0];
        auto details = client.account_details(id);
        std::string alias = details.count("Account.alias") ? details["Account.alias"] : "";
        std::string username = details.count("Account.username") ? details["Account.username"] : "";
        std::cerr << "[jami-bridge] Auto-detected account: " << id
                  << " (alias: " << alias << ", username: " << username << ")" << std::endl;
        return id;
    }

    // No accounts exist — create one
    std::cerr << "[jami-bridge] No accounts found. Creating new account (alias: "
              << cfg.account_alias << ")..." << std::endl;
    std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
    std::cerr << "[jami-bridge] Created account: " << account_id << std::endl;
    return account_id;
}

/// Run a one-shot CLI command, print result as JSON, and return exit code.
static int run_cli(jami::Client& client, const jami::Config& cfg) {
    // Resolve account
    std::string account_id;
    if (cfg.cli_command != "list-accounts" && cfg.cli_command != "stats") {
        account_id = resolve_account(client, cfg);
        if (account_id.empty()) {
            return 1;
        }
    }

    if (cfg.cli_command == "list-accounts") {
        auto accounts = client.list_accounts();
        json result = json::array();
        for (const auto& id : accounts) {
            auto details = client.account_details(id);
            auto vdetails = client.account_volatile_details(id);
            result.push_back({
                {"id", id},
                {"alias", details.count("Account.alias") ? details["Account.alias"] : ""},
                {"username", details.count("Account.username") ? details["Account.username"] : ""},
                {"registered", vdetails.count("Account.registrationStatus") &&
                              vdetails.at("Account.registrationStatus") == "REGISTERED"},
            });
        }
        std::cout << result.dump(2) << std::endl;
        return 0;
    }

    if (cfg.cli_command == "stats") {
        auto& stats = client.stats();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - stats.start_time
        ).count();
        json result = {
            {"uptimeSeconds", uptime},
            {"messagesReceived", stats.messages_received},
            {"messagesSent", stats.messages_sent},
            {"invitesAccepted", stats.invites_accepted},
            {"invitesDeclined", stats.invites_declined},
            {"hookInvocations", stats.hook_invocations},
            {"hookReplies", stats.hook_replies},
            {"hookTimeouts", stats.hook_timeouts},
            {"hookErrors", stats.hook_errors},
        };
        std::cout << result.dump(2) << std::endl;
        return 0;
    }

    if (cfg.cli_command == "list-conversations") {
        auto conv_ids = client.list_conversations(account_id);
        json result = json::array();
        for (const auto& cid : conv_ids) {
            auto info = client.conversation_info(account_id, cid);
            auto members = client.conversation_members(account_id, cid);
            json mode_val = info.count("mode") ? json(std::stoi(info["mode"])) : json();
            result.push_back({
                {"id", cid},
                {"mode", mode_val},
                {"title", info.count("title") ? info["title"] : ""},
                {"members", members.size()},
            });
        }
        std::cout << result.dump(2) << std::endl;
        return 0;
    }

    if (cfg.cli_command == "send-message") {
        client.send_message(account_id, cfg.conversation_id, cfg.body);
        json result = {{"sent", true}, {"conversationId", cfg.conversation_id}};
        std::cout << result.dump(2) << std::endl;
        return 0;
    }

    if (cfg.cli_command == "load-messages") {
        auto messages = client.load_messages_sync(account_id, cfg.conversation_id, "", cfg.count);
        json result = json::array();
        for (const auto& msg : messages) {
            result.push_back({
                {"id", msg.id},
                {"type", msg.type},
                {"from", msg.body.count("author") ? msg.body.at("author") : ""},
                {"body", msg.body.count("body") ? msg.body.at("body") : ""},
                {"parentId", msg.linearizedParent},
                {"timestamp", msg.body.count("timestamp") ? msg.body.at("timestamp") : ""},
            });
        }
        std::cout << result.dump(2) << std::endl;
        return 0;
    }

    std::cerr << "Error: unknown command '" << cfg.cli_command << "'" << std::endl;
    return 1;
}

int main(int argc, char* argv[]) {
    // ── Parse configuration ──────────────────────────────────────────────

    jami::Config cfg;
    int rc = cfg.parse_args(argc, argv);
    if (rc == -1) return 0;  // --help
    if (rc != 0) return rc;

    rc = cfg.validate();
    if (rc != 0) return rc;

    // ── Set daemon paths BEFORE init ──────────────────────────────────────

    cfg.apply_daemon_paths();

    // ── Suppress PJSIP's SIGINT handler ──────────────────────────────────

    signal(SIGTERM, signal_handler);

    // ── Set up event callbacks ────────────────────────────────────────────

    jami::Events events;

    if (cfg.mode == jami::Config::Mode::STDIO) {
        // In STDIO mode, push events as JSON-RPC notifications
        events.on_message_received = [](const jami::Message& msg) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onMessageReceived";
            notification["params"] = {
                {"accountId", msg.account_id},
                {"conversationId", msg.conversation_id},
                {"from", msg.from},
                {"body", msg.body},
                {"id", msg.id},
                {"type", msg.type},
                {"timestamp", msg.timestamp},
                {"parentId", msg.parent_id},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_registration_changed = [](const std::string& account_id,
                                            const std::string& state,
                                            int code,
                                            const std::string& detail) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onRegistrationChanged";
            notification["params"] = {
                {"accountId", account_id},
                {"state", state},
                {"code", code},
                {"detail", detail},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_conversation_request_received = [](const std::string& account_id,
                                                       const std::string& conv_id,
                                                       const std::map<std::string, std::string>& meta) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onConversationRequestReceived";
            std::string from;
            auto it = meta.find("from");
            if (it != meta.end()) from = it->second;
            notification["params"] = {
                {"accountId", account_id},
                {"conversationId", conv_id},
                {"from", from},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_conversation_ready = [](const std::string& account_id,
                                           const std::string& conv_id) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onConversationReady";
            notification["params"] = {
                {"accountId", account_id},
                {"conversationId", conv_id},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_conversation_member_event = [](const std::string& account_id,
                                                    const std::string& conv_id,
                                                    const std::string& member_uri,
                                                    int event) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onConversationMemberEvent";
            // event: 0 = add, 1 = joins, 2 = leave, 3 = banned
            notification["params"] = {
                {"accountId", account_id},
                {"conversationId", conv_id},
                {"memberUri", member_uri},
                {"event", event},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_message_status_changed = [](const std::string& account_id,
                                               const std::string& conversation_id,
                                               const std::string& peer,
                                               const std::string& message_id,
                                               int state) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onMessageStatusChanged";
            notification["params"] = {
                {"accountId", account_id},
                {"conversationId", conversation_id},
                {"peer", peer},
                {"messageId", message_id},
                {"state", state},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_trust_request_received = [](const std::string& account_id,
                                                const std::string& from_uri,
                                                const std::string& conv_id) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onTrustRequestReceived";
            notification["params"] = {
                {"accountId", account_id},
                {"from", from_uri},
                {"conversationId", conv_id},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_name_registration_ended = [](const std::string& account_id,
                                                 int state,
                                                 const std::string& name) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onNameRegistrationEnded";
            // state: 0=success, 1=invalid, 2=already taken, 3=error, 4=unsupported
            notification["params"] = {
                {"accountId", account_id},
                {"state", state},
                {"name", name},
            };
            std::cout << notification.dump() << std::endl;
        };
        events.on_data_transfer_event = [](const jami::FileTransfer& transfer) {
            json notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "onDataTransferEvent";
            notification["params"] = {
                {"accountId", transfer.account_id},
                {"conversationId", transfer.conversation_id},
                {"interactionId", transfer.interaction_id},
                {"fileId", transfer.file_id},
                {"eventCode", transfer.event_code},
            };
            if (!transfer.path.empty())
                notification["params"]["path"] = transfer.path;
            if (transfer.total_size > 0)
                notification["params"]["totalSize"] = transfer.total_size;
            if (transfer.bytes_progress > 0)
                notification["params"]["bytesProgress"] = transfer.bytes_progress;
            std::cout << notification.dump() << std::endl;
        };
    } else if (cfg.mode == jami::Config::Mode::CLI) {
        // CLI mode: suppress event output, just print command result
    } else {
        // HTTP mode: log events to stderr
        events.on_message_received = [](const jami::Message& msg) {
            std::cerr << "[jami-bridge] Message received: "
                      << "from=" << msg.from << " "
                      << "conv=" << msg.conversation_id << " "
                      << "body=\"" << msg.body << "\""
                      << std::endl;
        };
        events.on_registration_changed = [](const std::string& account_id,
                                            const std::string& state,
                                            int code,
                                            const std::string& detail) {
            std::cerr << "[jami-bridge] Registration: "
                      << "account=" << account_id << " "
                      << "state=" << state << " "
                      << "code=" << code << std::endl;
        };
        events.on_conversation_request_received = [](const std::string& account_id,
                                                       const std::string& conv_id,
                                                       const std::map<std::string, std::string>& meta) {
            std::string from;
            auto it = meta.find("from");
            if (it != meta.end()) from = it->second;
            std::cerr << "[jami-bridge] Conversation request: "
                      << "account=" << account_id << " "
                      << "conv=" << conv_id << " "
                      << "from=" << (from.empty() ? "(unknown)" : from) << std::endl;
        };
        events.on_conversation_ready = [](const std::string& account_id,
                                           const std::string& conv_id) {
            std::cerr << "[jami-bridge] Conversation ready: "
                      << "account=" << account_id << " "
                      << "conv=" << conv_id << std::endl;
        };
        events.on_conversation_member_event = [](const std::string& account_id,
                                                    const std::string& conv_id,
                                                    const std::string& member_uri,
                                                    int event) {
            const char* action = "unknown";
            switch (event) {
                case 0: action = "added"; break;
                case 1: action = "joined"; break;
                case 2: action = "left"; break;
                case 3: action = "banned"; break;
            }
            std::cerr << "[jami-bridge] Member event: "
                      << "conv=" << conv_id << " "
                      << "member=" << member_uri << " "
                      << "action=" << action << std::endl;
        };
        events.on_message_status_changed = [](const std::string& account_id,
                                               const std::string& conversation_id,
                                               const std::string& peer,
                                               const std::string& message_id,
                                               int state) {
            std::cerr << "[jami-bridge] Message status: "
                      << "msg=" << message_id << " "
                      << "state=" << state << std::endl;
        };
        events.on_trust_request_received = [](const std::string& account_id,
                                                const std::string& from_uri,
                                                const std::string& conv_id) {
            std::cerr << "[jami-bridge] Trust request: from=" << from_uri
                      << " conv=" << conv_id << std::endl;
        };
        events.on_data_transfer_event = [](const jami::FileTransfer& transfer) {
            std::cerr << "[jami-bridge] Data transfer: "
                      << "conv=" << transfer.conversation_id << " "
                      << "file=" << transfer.file_id << " "
                      << "event=" << transfer.event_code << " "
                      << "progress=" << transfer.bytes_progress << "/" << transfer.total_size
                      << std::endl;
        };
    }

    // ── Initialize daemon ──────────────────────────────────────────────

    std::cerr << "[jami-bridge] Starting..." << std::endl;

    jami::Client client(events, cfg.debug);

    // ── Display bot identity ──────────────────────────────────────────────
    // Show the bot's Jami ID early so the owner knows where to send invites.
    // We check accounts after a brief delay (daemon needs time to load).

    // ── Install invite policy ───────────────────────────────────────────
    // Wrap the conversation_request_received and IncomingTrustRequest handlers
    // based on invite policy flags.
    //
    // Policy:  -1 (default) = passive (emit events, no accept/decline)
    //           0 = accept all (--auto-accept)
    //           1 = accept from owner only, decline others (--auto-accept-from)
    //           2 = reject all (--reject-unknown)

    if (cfg.mode != jami::Config::Mode::CLI && cfg.auto_accept != -1) {
        // Only install policy handlers when a policy is explicitly set.
        // Default (-1) = passive bridge: events flow, no auto-action.

        auto old_conv_req_cb = events.on_conversation_request_received;
        int policy = cfg.auto_accept;
        std::string owner_uri = cfg.auto_accept_from;

        events.on_conversation_request_received =
            [policy, owner_uri, old_conv_req_cb, &client](
                const std::string& account_id,
                const std::string& conv_id,
                const std::map<std::string, std::string>& metadatas) {

            // Extract who sent the invite
            std::string from;
            auto it = metadatas.find("from");
            if (it != metadatas.end()) from = it->second;

            bool should_accept = false;
            bool should_decline = false;
            if (policy == 0) {
                // Accept all
                should_accept = true;
            } else if (policy == 1) {
                // Accept from owner, decline everyone else
                if (!owner_uri.empty() && from == owner_uri) {
                    should_accept = true;
                } else {
                    should_decline = true;
                }
            } else if (policy == 2) {
                // Reject all
                should_decline = true;
            }

            if (should_accept) {
                std::cerr << "[jami-bridge] Accepting invite: conv=" << conv_id
                          << " from=" << (from.empty() ? "(unknown)" : from) << std::endl;
                client.accept_request(account_id, conv_id);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                std::cerr << "[jami-bridge] Declining invite: conv=" << conv_id
                          << " from=" << (from.empty() ? "(unknown)" : from) << std::endl;
                client.decline_request(account_id, conv_id);
                client.stats().invites_declined++;
            }

            // Chain to previous callback (hook, logging, etc.)
            if (old_conv_req_cb) {
                old_conv_req_cb(account_id, conv_id, metadatas);
            }
        };

        // Same policy for IncomingTrustRequest (contact requests)
        auto old_trust_req_cb = events.on_trust_request_received;
        events.on_trust_request_received =
            [policy, owner_uri, old_trust_req_cb, &client](
                const std::string& account_id,
                const std::string& from_uri,
                const std::string& conv_id) {

            bool should_accept = false;
            bool should_decline = false;
            if (policy == 0) {
                should_accept = true;
            } else if (policy == 1) {
                if (!owner_uri.empty() && from_uri == owner_uri) {
                    should_accept = true;
                } else {
                    should_decline = true;
                }
            } else if (policy == 2) {
                should_decline = true;
            }

            if (should_accept) {
                std::cerr << "[jami-bridge] Accepting trust request from=" << from_uri << std::endl;
                client.accept_trust_request(account_id, from_uri);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                std::cerr << "[jami-bridge] Declining trust request from=" << from_uri << std::endl;
                client.decline_trust_request(account_id, from_uri);
                client.stats().invites_declined++;
            }

            // Chain to previous callback
            if (old_trust_req_cb) {
                old_trust_req_cb(account_id, from_uri, conv_id);
            }
        };

        // Apply the updated callbacks
        client.update_callbacks(events);

        // Log the invite policy
        if (cfg.auto_accept == 0) {
            std::cerr << "[jami-bridge] Invite policy: accept all" << std::endl;
        } else if (cfg.auto_accept == 1) {
            std::cerr << "[jami-bridge] Invite policy: accept from owner only ("
                      << cfg.auto_accept_from << ")" << std::endl;
        } else if (cfg.auto_accept == 2) {
            std::cerr << "[jami-bridge] Invite policy: reject all (lockdown)" << std::endl;
        }
    }

    // ── Install hook callbacks ──────────────────────────────────────────

#ifndef _WIN32
    std::unique_ptr<jami::HookManager> hook_manager;
    if (!cfg.hook_command.empty()) {
        hook_manager = std::make_unique<jami::HookManager>(
            client, cfg.hook_command, cfg.hook_events, cfg.hook_timeout);
        hook_manager->install_callbacks(events);
        // Re-register callbacks (install_callbacks modified Events struct)
        client.update_callbacks(events);
        std::cerr << "[jami-bridge] Hook: " << cfg.hook_command
                  << " (events: " << cfg.hook_events
                  << ", timeout: " << cfg.hook_timeout << "s)" << std::endl;
    }
#else
    if (!cfg.hook_command.empty()) {
        std::cerr << "[jami-bridge] Error: --hook is not supported on Windows yet. Use STDIO mode." << std::endl;
        return 1;
    }
#endif

    // ── Resolve account ────────────────────────────────────────────────

    // In non-CLI modes, we need an account for the API.
    // Store the resolved account ID so the server can use it.
    // The resolve happens lazily when an API call needs it,
    // but for HTTP/STDIO modes we resolve early for the log message.
    std::string resolved_account_id;
    if (cfg.mode != jami::Config::Mode::CLI) {
        // Give the daemon time to load accounts
        std::this_thread::sleep_for(std::chrono::seconds(2));
        resolved_account_id = resolve_account(client, cfg);
        if (resolved_account_id.empty() && !cfg.account.empty() && cfg.account != "new") {
            std::cerr << "[jami-bridge] Warning: Could not resolve configured account" << std::endl;
        }

        // Display the bot's Jami identity for easy invites
        if (!resolved_account_id.empty()) {
            print_bot_identity(client, resolved_account_id);
        }
    }

    // ── Run in requested mode ──────────────────────────────────────────

    if (cfg.mode == jami::Config::Mode::CLI) {
        // CLI mode: give daemon time to initialize
        std::this_thread::sleep_for(std::chrono::seconds(2));
        int rc = run_cli(client, cfg);
        _exit(rc);
    }

    if (cfg.mode == jami::Config::Mode::STDIO) {
        jami::StdioServer stdio_server(client);
        stdio_server.run();
        _exit(0);
    }

    // HTTP mode
    std::cerr << "[jami-bridge] HTTP server listening on " << cfg.host << ":" << cfg.port << std::endl;
    jami::Server server(client, cfg.host, cfg.port);
    server.run();

    std::cerr << "[jami-bridge] Shutting down..." << std::endl;
    _exit(0);
}
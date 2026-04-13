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

#include "log.h"

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
        jami::log("Waiting for Jami URI...");
        username = wait_for_jami_uri(client, account_id);
    }

    jami::log("Bot identity: ", username, " (account: ", account_id, ", alias: ", alias, ")");
    jami::log("Add this bot to a group: invite ", username);
}

/// Resolve which account to use based on config.
/// Returns the account ID to use.
/// May create or import an account if configured.
/// Resolve a single account when --account SPEC is given.
/// Returns the account ID, or empty string on error.
static std::string resolve_single_account(jami::Client& client, const jami::Config& cfg) {
    auto accounts = client.list_accounts();

    if (cfg.account.empty()) return "";  // shouldn't be called with empty spec

    // Try matching by account ID (hex string)
    for (const auto& id : accounts) {
        if (id == cfg.account) {
            jami::log("Using account: ", cfg.account);
            return cfg.account;
        }
    }
    // Try matching by Jami URI (e.g. "jami://abc123" or just the hash)
    // Strip "jami://" prefix if present
    std::string uri = cfg.account;
    if (uri.find("jami://") == 0) uri = uri.substr(7);
    for (const auto& id : accounts) {
        auto details = client.account_details(id);
        std::string username = details.count("Account.username") ? details["Account.username"] : "";
        // Match against the full URI or the hash portion
        if (username == cfg.account || username == uri
            || username.find(uri) != std::string::npos) {
            jami::log("Using account: ", id, " (matched by URI: ", cfg.account, ")");
            return id;
        }
    }
    jami::log("Error: Account not found: ", cfg.account);
    jami::log("Available accounts:");
    for (const auto& id : accounts) {
        auto details = client.account_details(id);
        jami::log("  ", id,
                  " (alias: ", (details.count("Account.alias") ? details["Account.alias"] : ""), ")");
    }
    return "";
}

/// Resolve account(s) for the bridge.
///
/// Returns a list of account IDs to serve.
/// - --account SPEC (hex/URI):  returns [that account]
/// - --account archive:///path:  returns [imported account]
/// - --account /path.gz:  returns [imported/created account]
/// - --account new:  returns [new account]
/// - (empty --account, single account):  returns [that account]
/// - (empty --account, multiple accounts):  returns ALL accounts
/// - (empty --account, no accounts):  creates one, returns [it]
static std::vector<std::string> resolve_accounts(jami::Client& client, const jami::Config& cfg) {
    auto accounts = client.list_accounts();

    // ── Explicit --account SPEC ────────────────────────────────────────
    if (!cfg.account.empty()) {
        // archive:///path or /path.gz
        if (cfg.account.find("archive://") == 0 || cfg.account[0] == '/') {
            std::string path = cfg.account.find("archive://") == 0
                ? cfg.account.substr(10)
                : cfg.account;

            if (file_exists(path)) {
                // ── Check if the account is already loaded ──────────────
                // Importing an archive that's already in the daemon creates
                // a duplicate. Match by exported URI to reuse existing.
                //
                // We peek into the archive to get the Jami URI, then
                // check loaded accounts for a match. The archive is a
                // gzip file; the daemon can tell us the details after a
                // trial import, but that has side effects. Instead, we
                // import and then check if we created a duplicate — if so,
                // remove the new one and return the existing.
                //
                // Simpler approach: import, then check if there's now
                // more than one account with the same URI. If the import
                // created a duplicate, remove it and return the original.
                jami::log("Importing account from: ", path);
                std::string new_id = client.import_account(path, cfg.account_password);
                if (new_id.empty()) {
                    jami::log("Error: Failed to import account");
                    return {};
                }

                // Check if an existing (non-new) account has the same URI
                auto new_details = client.account_details(new_id);
                std::string new_uri = new_details.count("Account.username")
                    ? new_details["Account.username"] : "";
                // URI may not be available immediately after import
                if (new_uri.empty()) {
                    auto vdetails = client.account_volatile_details(new_id);
                    new_uri = vdetails.count("Account.username")
                        ? vdetails["Account.username"] : "";
                }

                // Wait briefly for URI if not yet available
                if (new_uri.empty()) {
                    new_uri = wait_for_jami_uri(client, new_id, 5000);
                }

                // Look for duplicate: another loaded account with same URI
                std::string existing_id;
                for (const auto& id : accounts) {
                    if (id == new_id) continue;  // skip the one we just imported
                    auto details = client.account_details(id);
                    std::string uri = details.count("Account.username")
                        ? details["Account.username"] : "";
                    if (uri.empty()) {
                        auto vd = client.account_volatile_details(id);
                        uri = vd.count("Account.username") ? vd["Account.username"] : "";
                    }
                    if (!uri.empty() && uri == new_uri) {
                        existing_id = id;
                        break;
                    }
                }

                if (!existing_id.empty()) {
                    // The imported account is a duplicate — remove it, use the original
                    jami::log("Account already loaded as: ", existing_id,
                             " — removing duplicate: ", new_id);
                    client.remove_account(new_id);
                    return {existing_id};
                }

                jami::log("Imported account: ", new_id);
                return {new_id};
            } else {
                jami::log("Creating new account (archive will be saved to: ", path, ")");
                std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
                jami::log("Created account: ", account_id);

                // Wait for the account to be ready before exporting
                std::this_thread::sleep_for(std::chrono::seconds(3));

                bool exported = client.export_account(account_id, path, cfg.account_password);
                if (exported) {
                    jami::log("Account exported to: ", path);
                } else {
                    jami::log("Warning: Failed to export account to ", path);
                }
                return {account_id};
            }
        }

        // "new" — create a fresh account
        if (cfg.account == "new") {
            jami::log("Creating new account (alias: ", cfg.account_alias, ")...");
            std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
            jami::log("Created account: ", account_id);
            return {account_id};
        }

        // Hex ID or Jami URI — resolve to a single account
        std::string id = resolve_single_account(client, cfg);
        if (id.empty()) return {};
        return {id};
    }

    // ── No --account specified — auto-detect or create ─────────────────
    if (accounts.empty()) {
        jami::log("No accounts found. Creating new account (alias: ", cfg.account_alias, ")...");
        std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
        jami::log("Created account: ", account_id);
        return {account_id};
    }

    if (accounts.size() == 1) {
        std::string id = accounts[0];
        auto details = client.account_details(id);
        std::string alias = details.count("Account.alias") ? details["Account.alias"] : "";
        std::string username = details.count("Account.username") ? details["Account.username"] : "";
        jami::log("Auto-detected account: ", id, " (alias: ", alias, ", username: ", username, ")");
        return {id};
    }

    // Multiple accounts — serve all of them
    jami::log("Auto-detected ", accounts.size(), " accounts — serving all:");
    for (const auto& id : accounts) {
        auto details = client.account_details(id);
        std::string alias = details.count("Account.alias") ? details["Account.alias"] : "";
        std::string username = details.count("Account.username") ? details["Account.username"] : "";
        jami::log("  ", id, " (alias: ", alias, ", username: ", username, ")");
    }
    return accounts;
}

/// Run a one-shot CLI command, print result as JSON, and return exit code.
static int run_cli(jami::Client& client, const jami::Config& cfg) {
    // Resolve account
    std::string account_id;
    if (cfg.cli_command != "list-accounts" && cfg.cli_command != "stats") {
        auto resolved = resolve_accounts(client, cfg);
        if (resolved.empty()) {
            return 1;
        }
        account_id = resolved[0];  // CLI mode uses first account
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

    jami::log("Error: unknown command '", cfg.cli_command, "'");
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
            jami::log("Message received: ", "from=", msg.from, " conv=", msg.conversation_id);
        };
        events.on_registration_changed = [](const std::string& account_id,
                                            const std::string& state,
                                            int code,
                                            const std::string& detail) {
            jami::log("Registration: ", "account=", account_id, " ", "state=", state, " ", "code=", code);
        };
        events.on_conversation_request_received = [](const std::string& account_id,
                                                       const std::string& conv_id,
                                                       const std::map<std::string, std::string>& meta) {
            std::string from;
            auto it = meta.find("from");
            if (it != meta.end()) from = it->second;
            jami::log("Conversation request: ", "account=", account_id, " ", "conv=", conv_id, " ", "from=", (from.empty() ? "(unknown)" : from));
        };
        events.on_conversation_ready = [](const std::string& account_id,
                                           const std::string& conv_id) {
            jami::log("Conversation ready: ", "account=", account_id, " ", "conv=", conv_id);
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
            jami::log("Member event: ", "conv=", conv_id, " ", "member=", member_uri, " ", "action=", action);
        };
        events.on_message_status_changed = [](const std::string& account_id,
                                               const std::string& conversation_id,
                                               const std::string& peer,
                                               const std::string& message_id,
                                               int state) {
            jami::log("Message status: ", "msg=", message_id, " ", "state=", state);
        };
        events.on_trust_request_received = [](const std::string& account_id,
                                                const std::string& from_uri,
                                                const std::string& conv_id) {
            jami::log("Trust request: from=", from_uri, " conv=", conv_id);
        };
        events.on_data_transfer_event = [](const jami::FileTransfer& transfer) {
            jami::log("Data transfer: ", "conv=", transfer.conversation_id, " ", "file=", transfer.file_id, " ", "event=", transfer.event_code, " ", "progress=", transfer.bytes_progress, "/", transfer.total_size);
        };
    }

    // ── Initialize daemon ──────────────────────────────────────────────

    jami::log("Starting...");

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
                jami::log("Accepting invite: conv=", conv_id, " from=", (from.empty() ? "(unknown)" : from));
                client.accept_request(account_id, conv_id);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                jami::log("Declining invite: conv=", conv_id, " from=", (from.empty() ? "(unknown)" : from));
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
                jami::log("Accepting trust request from=", from_uri);
                client.accept_trust_request(account_id, from_uri);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                jami::log("Declining trust request from=", from_uri);
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
            jami::log("Invite policy: accept all");
        } else if (cfg.auto_accept == 1) {
            jami::log("Invite policy: accept from owner only (", cfg.auto_accept_from, ")");
        } else if (cfg.auto_accept == 2) {
            jami::log("Invite policy: reject all (lockdown)");
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
        jami::log("Hook: ", cfg.hook_command, " (events: ", cfg.hook_events, ", timeout: ", cfg.hook_timeout, "s)");
    }
#else
    if (!cfg.hook_command.empty()) {
        jami::log("Error: --hook is not supported on Windows yet. Use STDIO mode.");
        return 1;
    }
#endif

    // ── Resolve account ────────────────────────────────────────────────

    // In non-CLI modes, we need an account for the API.
    // Store the resolved account ID so the server can use it.
    // The resolve happens lazily when an API call needs it,
    // but for HTTP/STDIO modes we resolve early for the log message.
    std::vector<std::string> resolved_accounts;
    if (cfg.mode != jami::Config::Mode::CLI) {
        // Give the daemon time to load accounts
        std::this_thread::sleep_for(std::chrono::seconds(2));
        resolved_accounts = resolve_accounts(client, cfg);
        if (resolved_accounts.empty() && !cfg.account.empty() && cfg.account != "new") {
            jami::log("Warning: Could not resolve configured account");
        }

        // Display the bot's Jami identity for easy invites
        for (const auto& aid : resolved_accounts) {
            print_bot_identity(client, aid);
        }
    }

    // ── Install account filter ─────────────────────────────────────────
    // When --account resolves to a single account, only emit events for
    // that account. This prevents cross-account event leakage when the
    // daemon has multiple accounts loaded (e.g. data directory has accounts
    // A and B, but the client only cares about A).
    //
    // When serving multiple accounts (no --account specified, multiple
    // accounts exist), all events are emitted — no filter needed.

    if (resolved_accounts.size() == 1) {
        std::string filter_account = resolved_accounts[0];

        // Wrap on_message_received
        auto orig_msg_cb = std::move(events.on_message_received);
        events.on_message_received = [filter_account, orig_msg_cb](const jami::Message& msg) {
            if (msg.account_id == filter_account) {
                if (orig_msg_cb) orig_msg_cb(msg);
            }
        };

        // Wrap on_conversation_ready
        auto orig_conv_ready_cb = std::move(events.on_conversation_ready);
        events.on_conversation_ready = [filter_account, orig_conv_ready_cb](
                const std::string& account_id, const std::string& conv_id) {
            if (account_id == filter_account) {
                if (orig_conv_ready_cb) orig_conv_ready_cb(account_id, conv_id);
            }
        };

        // Wrap on_conversation_member_event
        auto orig_member_cb = std::move(events.on_conversation_member_event);
        events.on_conversation_member_event = [filter_account, orig_member_cb](
                const std::string& account_id, const std::string& conv_id,
                const std::string& member_uri, int event) {
            if (account_id == filter_account) {
                if (orig_member_cb) orig_member_cb(account_id, conv_id, member_uri, event);
            }
        };

        // Wrap on_conversation_request_received
        auto orig_conv_req_cb = std::move(events.on_conversation_request_received);
        events.on_conversation_request_received = [filter_account, orig_conv_req_cb](
                const std::string& account_id, const std::string& conv_id,
                const std::map<std::string, std::string>& metadatas) {
            if (account_id == filter_account) {
                if (orig_conv_req_cb) orig_conv_req_cb(account_id, conv_id, metadatas);
            }
        };

        // Wrap on_trust_request_received
        auto orig_trust_cb = std::move(events.on_trust_request_received);
        events.on_trust_request_received = [filter_account, orig_trust_cb](
                const std::string& account_id, const std::string& from_uri,
                const std::string& conv_id) {
            if (account_id == filter_account) {
                if (orig_trust_cb) orig_trust_cb(account_id, from_uri, conv_id);
            }
        };

        // Wrap on_registration_changed
        auto orig_reg_cb = std::move(events.on_registration_changed);
        events.on_registration_changed = [filter_account, orig_reg_cb](
                const std::string& account_id, const std::string& state,
                int code, const std::string& detail) {
            if (account_id == filter_account) {
                if (orig_reg_cb) orig_reg_cb(account_id, state, code, detail);
            }
        };

        // Wrap on_message_status_changed
        auto orig_status_cb = std::move(events.on_message_status_changed);
        events.on_message_status_changed = [filter_account, orig_status_cb](
                const std::string& account_id, const std::string& conversation_id,
                const std::string& peer, const std::string& message_id, int state) {
            if (account_id == filter_account) {
                if (orig_status_cb) orig_status_cb(account_id, conversation_id, peer, message_id, state);
            }
        };

        // Wrap on_name_registration_ended
        auto orig_name_reg_cb = std::move(events.on_name_registration_ended);
        events.on_name_registration_ended = [filter_account, orig_name_reg_cb](
                const std::string& account_id, int state, const std::string& name) {
            if (account_id == filter_account) {
                if (orig_name_reg_cb) orig_name_reg_cb(account_id, state, name);
            }
        };

        // Wrap on_messages_loaded (sync message loading — pass-through,
        // but filter for consistency)
        auto orig_loaded_cb = std::move(events.on_messages_loaded);
        events.on_messages_loaded = [filter_account, orig_loaded_cb](
                uint32_t req_id, const std::string& account_id,
                const std::string& conv_id,
                const std::vector<libjami::SwarmMessage>& messages) {
            if (account_id == filter_account) {
                if (orig_loaded_cb) orig_loaded_cb(req_id, account_id, conv_id, messages);
            }
        };

        // Wrap on_data_transfer_event
        auto orig_transfer_cb = std::move(events.on_data_transfer_event);
        events.on_data_transfer_event = [filter_account, orig_transfer_cb](
                const jami::FileTransfer& transfer) {
            if (transfer.account_id == filter_account) {
                if (orig_transfer_cb) orig_transfer_cb(transfer);
            }
        };

        // Re-register callbacks with the filtered versions
        client.update_callbacks(events);
        jami::log("Account filter: only emitting events for ", filter_account.substr(0, 8), "...");
    } else if (resolved_accounts.empty()) {
        jami::log("No account filter: no accounts resolved");
    } else {
        jami::log("No account filter: emitting events for ALL ", resolved_accounts.size(), " accounts");
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
    jami::log("HTTP server listening on ", cfg.host, ":", cfg.port);
    jami::Server server(client, cfg.host, cfg.port);
    server.run();

    jami::log("Shutting down...");
    _exit(0);
}
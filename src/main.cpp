/// @file main.cpp
/// @brief Entry point for jami-sdk — HTTP, STDIO, or CLI mode.
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
        std::cerr << "[jami-sdk] Waiting for Jami URI..." << std::endl;
        username = wait_for_jami_uri(client, account_id);
    }

    std::cerr << "[jami-sdk] Bot identity: " << username
              << " (account: " << account_id
              << ", alias: " << alias << ")" << std::endl;
    std::cerr << "[jami-sdk] Add this bot to a group: invite " << username << std::endl;
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
                std::cerr << "[jami-sdk] Using account: " << cfg.account << std::endl;
                return cfg.account;
            }
        }
        std::cerr << "[jami-sdk] Error: Account not found: " << cfg.account << std::endl;
        std::cerr << "[jami-sdk] Available accounts:" << std::endl;
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
            std::cerr << "[jami-sdk] Importing account from: " << path << std::endl;
            std::string account_id = client.import_account(path, cfg.account_password);
            if (account_id.empty()) {
                std::cerr << "[jami-sdk] Error: Failed to import account" << std::endl;
                return "";
            }
            std::cerr << "[jami-sdk] Imported account: " << account_id << std::endl;
            return account_id;
        } else {
            // Path doesn't exist — create new account and export it
            std::cerr << "[jami-sdk] Creating new account (archive will be saved to: " << path << ")" << std::endl;
            std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
            std::cerr << "[jami-sdk] Created account: " << account_id << std::endl;

            // Wait for the account to be ready before exporting
            std::this_thread::sleep_for(std::chrono::seconds(3));

            bool exported = client.export_account(account_id, path, cfg.account_password);
            if (exported) {
                std::cerr << "[jami-sdk] Account exported to: " << path << std::endl;
            } else {
                std::cerr << "[jami-sdk] Warning: Failed to export account to " << path << std::endl;
            }
            return account_id;
        }
    }

    // Create new account explicitly requested
    if (cfg.account == "new") {
        std::cerr << "[jami-sdk] Creating new account (alias: " << cfg.account_alias << ")..." << std::endl;
        std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
        std::cerr << "[jami-sdk] Created account: " << account_id << std::endl;
        return account_id;
    }

    // Auto-detect: use first existing account, or create new one
    if (!accounts.empty()) {
        std::string id = accounts[0];
        auto details = client.account_details(id);
        std::string alias = details.count("Account.alias") ? details["Account.alias"] : "";
        std::string username = details.count("Account.username") ? details["Account.username"] : "";
        std::cerr << "[jami-sdk] Auto-detected account: " << id
                  << " (alias: " << alias << ", username: " << username << ")" << std::endl;
        return id;
    }

    // No accounts exist — create one
    std::cerr << "[jami-sdk] No accounts found. Creating new account (alias: "
              << cfg.account_alias << ")..." << std::endl;
    std::string account_id = client.create_account(cfg.account_alias, cfg.account_password);
    std::cerr << "[jami-sdk] Created account: " << account_id << std::endl;
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
    } else if (cfg.mode == jami::Config::Mode::CLI) {
        // CLI mode: suppress event output, just print command result
    } else {
        // HTTP mode: log events to stderr
        events.on_message_received = [](const jami::Message& msg) {
            std::cerr << "[jami-sdk] Message received: "
                      << "from=" << msg.from << " "
                      << "conv=" << msg.conversation_id << " "
                      << "body=\"" << msg.body << "\""
                      << std::endl;
        };
        events.on_registration_changed = [](const std::string& account_id,
                                            const std::string& state,
                                            int code,
                                            const std::string& detail) {
            std::cerr << "[jami-sdk] Registration: "
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
            std::cerr << "[jami-sdk] Conversation request: "
                      << "account=" << account_id << " "
                      << "conv=" << conv_id << " "
                      << "from=" << (from.empty() ? "(unknown)" : from) << std::endl;
        };
        events.on_conversation_ready = [](const std::string& account_id,
                                           const std::string& conv_id) {
            std::cerr << "[jami-sdk] Conversation ready: "
                      << "account=" << account_id << " "
                      << "conv=" << conv_id << std::endl;
        };
        events.on_message_status_changed = [](const std::string& account_id,
                                               const std::string& conversation_id,
                                               const std::string& peer,
                                               const std::string& message_id,
                                               int state) {
            std::cerr << "[jami-sdk] Message status: "
                      << "msg=" << message_id << " "
                      << "state=" << state << std::endl;
        };
        events.on_trust_request_received = [](const std::string& account_id,
                                                const std::string& from_uri,
                                                const std::string& conv_id) {
            std::cerr << "[jami-sdk] Trust request: from=" << from_uri
                      << " conv=" << conv_id << std::endl;
        };
    }

    // ── Initialize daemon ──────────────────────────────────────────────

    std::cerr << "[jami-sdk] Starting..." << std::endl;

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
                std::cerr << "[jami-sdk] Accepting invite: conv=" << conv_id
                          << " from=" << (from.empty() ? "(unknown)" : from) << std::endl;
                client.accept_request(account_id, conv_id);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                std::cerr << "[jami-sdk] Declining invite: conv=" << conv_id
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
                std::cerr << "[jami-sdk] Accepting trust request from=" << from_uri << std::endl;
                client.accept_trust_request(account_id, from_uri);
                client.stats().invites_accepted++;
            } else if (should_decline) {
                std::cerr << "[jami-sdk] Declining trust request from=" << from_uri << std::endl;
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
            std::cerr << "[jami-sdk] Invite policy: accept all" << std::endl;
        } else if (cfg.auto_accept == 1) {
            std::cerr << "[jami-sdk] Invite policy: accept from owner only ("
                      << cfg.auto_accept_from << ")" << std::endl;
        } else if (cfg.auto_accept == 2) {
            std::cerr << "[jami-sdk] Invite policy: reject all (lockdown)" << std::endl;
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
        std::cerr << "[jami-sdk] Hook: " << cfg.hook_command
                  << " (events: " << cfg.hook_events
                  << ", timeout: " << cfg.hook_timeout << "s)" << std::endl;
    }
#else
    if (!cfg.hook_command.empty()) {
        std::cerr << "[jami-sdk] Error: --hook is not supported on Windows yet. Use STDIO mode." << std::endl;
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
            std::cerr << "[jami-sdk] Warning: Could not resolve configured account" << std::endl;
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
    std::cerr << "[jami-sdk] HTTP server listening on " << cfg.host << ":" << cfg.port << std::endl;
    jami::Server server(client, cfg.host, cfg.port);
    server.run();

    std::cerr << "[jami-sdk] Shutting down..." << std::endl;
    _exit(0);
}
/// @file config.cpp
/// @brief Configuration loading and validation for jami-sdk.

#include "config.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

using json = nlohmann::json;

namespace jami {

void Config::print_usage(const char* prog) const {
    std::cout << "Usage: " << prog << " [mode] [options]\n"
              << "\n"
              << "Modes:\n"
              << "  (default)            HTTP REST API server\n"
              << "  --stdio              JSON-RPC over stdin/stdout\n"
              << "  --send-message       Send a message and exit\n"
              << "  --list-accounts      List accounts and exit\n"
              << "  --list-conversations List conversations and exit\n"
              << "  --load-messages      Load messages and exit\n"
              << "  --stats              Show runtime statistics and exit\n"
              << "\n"
              << "Server options:\n"
              << "  --host HOST          Listen address (default: 0.0.0.0)\n"
              << "  --port PORT          Listen port (default: 8090)\n"
              << "\n"
              << "Account options:\n"
              << "  --account SPEC       Account to use (default: auto-detect)\n"
              << "                         ID: use existing account (hex string)\n"
              << "                         archive:///path: import from archive file\n"
              << "                         /path/to/file.gz: import if exists,\n"
              << "                                           create+export if not\n"
              << "                         new: create a new account\n"
              << "                         (empty): use first existing, or create new\n"
              << "  --account-alias NAME  Alias for new/created accounts (default: bot)\n"
              << "  --account-password PW  Password for archive import or new account\n"
              << "\n"
              << "Data paths (override daemon's XDG directories):\n"
              << "  --data-dir DIR       Data directory (default: $XDG_DATA_HOME/jami\n"
              << "                                               or ~/.local/share/jami)\n"
              << "  --config-dir DIR     Config directory (default: $XDG_CONFIG_HOME/jami\n"
              << "                                                or ~/.config/jami)\n"
              << "  --cache-dir DIR      Cache directory (default: $XDG_CACHE_HOME/jami\n"
              << "                                               or ~/.cache/jami)\n"
              << "\n"
              << "STDIO options:\n"
              << "  --stdio              Run in JSON-RPC mode (read stdin, write stdout)\n"
              << "\n"
              << "Hook options (run command on events):\n"
              << "  --hook CMD           Command to execute on events (via /bin/sh -c)\n"
              << "  --hook-events E      Comma-separated event types (default: onMessageReceived)\n"
              << "                       Supported: onMessageReceived, onConversationReady,\n"
              << "                                  onConversationRequestReceived,\n"
              << "                                  onTrustRequestReceived,\n"
              << "                                  onRegistrationChanged, onMessageStatusChanged,\n"
              << "                                  all\n"
              << "  --hook-timeout N     Timeout per hook invocation in seconds (default: 30)\n"
              << "\n"
              << "  The hook receives event data via:\n"
              << "    $JAMI_EVENT       Full event JSON (recommended)\n"
              << "    stdin             Same JSON piped to stdin\n"
              << "    $JAMI_EVENT_TYPE Event type (e.g. onMessageReceived)\n"
              << "    $JAMI_ACCOUNT_ID  Account ID\n"
              << "    $JAMI_CONVERSATION_ID  Conversation ID\n"
              << "\n"
              << "  Hook stdout can trigger replies:\n"
              << "    {\"reply\": \"text\"}               Send one message back\n"
              << "    {\"replies\": [\"msg1\", \"msg2\"]}    Send multiple messages back\n"
              << "\n"
              << "Invite policy (how to handle incoming conversation/trust requests):\n"
              << "  (default)            Passive — emit events only, no accept/decline\n"
              << "  --auto-accept        Accept ALL incoming invites\n"
              << "  --auto-accept-from U  Accept invites only from this Jami URI (owner),\n"
              << "                       decline all others\n"
              << "  --reject-unknown    Reject ALL incoming requests (lockdown mode)\n"
              << "\n"
              << "CLI (one-shot) options:\n"
              << "  --conversation ID    Conversation ID\n"
              << "  --body TEXT          Message body (for --send-message)\n"
              << "  --count N            Number of messages (for --load-messages, default: 10)\n"
              << "\n"
              << "Config file:\n"
              << "  --config FILE        Load settings from JSON config file\n"
              << "                       CLI arguments override config file values\n"
              << "\n"
              << "Common options:\n"
              << "  --debug              Enable debug logging\n"
              << "  --help               Show this help\n";
}

int Config::parse_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--stdio") {
            mode = Mode::STDIO;
        } else if (arg == "--send-message") {
            mode = Mode::CLI;
            cli_command = "send-message";
        } else if (arg == "--list-accounts") {
            mode = Mode::CLI;
            cli_command = "list-accounts";
        } else if (arg == "--list-conversations") {
            mode = Mode::CLI;
            cli_command = "list-conversations";
        } else if (arg == "--load-messages") {
            mode = Mode::CLI;
            cli_command = "load-messages";
        } else if (arg == "--stats") {
            mode = Mode::CLI;
            cli_command = "stats";
        } else if (arg == "--account" && i + 1 < argc) {
            account = argv[++i];
        } else if (arg == "--account-alias" && i + 1 < argc) {
            account_alias = argv[++i];
        } else if (arg == "--account-password" && i + 1 < argc) {
            account_password = argv[++i];
        } else if (arg == "--conversation" && i + 1 < argc) {
            conversation_id = argv[++i];
        } else if (arg == "--body" && i + 1 < argc) {
            body = argv[++i];
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::atoi(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config-dir" && i + 1 < argc) {
            config_dir = argv[++i];
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (arg == "--hook" && i + 1 < argc) {
            hook_command = argv[++i];
        } else if (arg == "--hook-events" && i + 1 < argc) {
            hook_events = argv[++i];
        } else if (arg == "--hook-timeout" && i + 1 < argc) {
            hook_timeout = std::atoi(argv[++i]);
        } else if (arg == "--auto-accept") {
            auto_accept = 0;  // accept all
            auto_accept_all_explicit = true;
        } else if (arg == "--auto-accept-from" && i + 1 < argc) {
            auto_accept = 1;  // accept from owner only
            auto_accept_from = argv[++i];
        } else if (arg == "--reject-unknown") {
            auto_accept = 2;  // reject all
        } else if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return -1;  // caller should exit(0)
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Load config file first, then let CLI args override
    if (!config_file.empty()) {
        int rc = load_config_file();
        if (rc != 0) return rc;
    }

    // Re-apply CLI args over config file values.
    // We need to re-parse because config file values were filled in,
    // but CLI should take precedence. Since we set from CLI first,
    // then loaded config, we need to re-apply CLI.
    // Simpler: track which args were explicitly set from CLI.
    // Actually, we already set CLI values first, then load_config_file()
    // only fills in values that are still at defaults. So CLI always wins.

    // Set default account alias if creating a new account
    if (account_alias.empty()) {
        account_alias = "bot";
    }

    // Resolve invite policy conflicts:
    // --auto-accept takes precedence over --auto-accept-from / --reject-unknown
    if (auto_accept_all_explicit && auto_accept != 0) {
        auto_accept = 0;  // accept all wins
    }
    // --auto-accept-from takes precedence over --reject-unknown
    if (!auto_accept_from.empty() && auto_accept != 0) {
        auto_accept = 1;  // owner-only wins
    }

    return 0;
}

int Config::load_config_file() {
    if (config_file.empty()) return 0;

    std::ifstream f(config_file);
    if (!f.is_open()) {
        std::cerr << "Error: Cannot open config file: " << config_file << std::endl;
        return 1;
    }

    json cfg;
    try {
        cfg = json::parse(f);
    } catch (const json::parse_error& e) {
        std::cerr << "Error: Invalid JSON in config file: " << e.what() << std::endl;
        return 1;
    }

    // Only override values that haven't been set from CLI.
    // We check for "still at default" by comparing against defaults.
    // This is imperfect but works for our use case.

    if (cfg.count("host") && host == "0.0.0.0") {
        host = cfg["host"].get<std::string>();
    }
    if (cfg.count("port") && port == 8090) {
        port = cfg["port"].get<int>();
    }
    if (cfg.count("debug") && !debug) {
        debug = cfg["debug"].get<bool>();
    }
    if (cfg.count("dataDir") && data_dir.empty()) {
        data_dir = cfg["dataDir"].get<std::string>();
    }
    if (cfg.count("configDir") && config_dir.empty()) {
        config_dir = cfg["configDir"].get<std::string>();
    }
    if (cfg.count("cacheDir") && cache_dir.empty()) {
        cache_dir = cfg["cacheDir"].get<std::string>();
    }
    if (cfg.count("account") && account.empty()) {
        account = cfg["account"].get<std::string>();
    }
    if (cfg.count("accountAlias") && account_alias == "bot") {
        account_alias = cfg["accountAlias"].get<std::string>();
    }
    if (cfg.count("accountPassword") && account_password.empty()) {
        account_password = cfg["accountPassword"].get<std::string>();
    }
    if (cfg.count("hook") && cfg["hook"].is_object()) {
        auto& hook = cfg["hook"];
        if (hook.count("command") && hook_command.empty()) {
            hook_command = hook["command"].get<std::string>();
        }
        if (hook.count("events") && hook_events == "onMessageReceived") {
            hook_events = hook["events"].get<std::string>();
        }
        if (hook.count("timeout") && hook_timeout == 30) {
            hook_timeout = hook["timeout"].get<int>();
        }
    }

    // Invite policy
    if (cfg.count("autoAccept") && auto_accept == -1) {
        if (cfg["autoAccept"].is_boolean()) {
            auto_accept = cfg["autoAccept"].get<bool>() ? 0 : -1;
            if (auto_accept == 0) auto_accept_all_explicit = true;
        }
    }
    if (cfg.count("autoAcceptFrom") && auto_accept_from.empty()) {
        auto_accept = 1;  // owner-only mode
        auto_accept_from = cfg["autoAcceptFrom"].get<std::string>();
    }
    if (cfg.count("rejectUnknown") && auto_accept == -1) {
        if (cfg["rejectUnknown"].is_boolean() && cfg["rejectUnknown"].get<bool>()) {
            auto_accept = 2;  // reject all
        }
    }
    // Config file conflict: both autoAccept and autoAcceptFrom
    if (cfg.count("autoAccept") && cfg.count("autoAcceptFrom")
        && cfg["autoAccept"].get<bool>()) {
        auto_accept = 0;  // accept all wins
        auto_accept_all_explicit = true;
    }

    return 0;
}

void Config::apply_daemon_paths() const {
    // The Jami daemon reads XDG env vars to determine data/config/cache paths.
    // We set them before libjami::init() so the daemon picks them up.
    // Note: these are the BASE directories (without /jami suffix),
    // because the daemon itself appends "jami" (the PACKAGE name).

    if (!data_dir.empty()) {
        setenv("XDG_DATA_HOME", data_dir.c_str(), 1);
        std::cerr << "[jami-sdk] Data dir: " << data_dir << std::endl;
    }
    if (!config_dir.empty()) {
        setenv("XDG_CONFIG_HOME", config_dir.c_str(), 1);
        std::cerr << "[jami-sdk] Config dir: " << config_dir << std::endl;
    }
    if (!cache_dir.empty()) {
        setenv("XDG_CACHE_HOME", cache_dir.c_str(), 1);
        std::cerr << "[jami-sdk] Cache dir: " << cache_dir << std::endl;
    }
}

int Config::validate() const {
    // STDIO + hook incompatible (both use stdout)
    if (mode == Mode::STDIO && !hook_command.empty()) {
        std::cerr << "Error: --stdio and --hook are incompatible (both use stdout)" << std::endl;
        return 1;
    }

    // CLI + hook incompatible
    if (mode == Mode::CLI && !hook_command.empty()) {
        std::cerr << "Error: --hook cannot be used with CLI commands (one-shot mode)" << std::endl;
        return 1;
    }

    // auto-accept-from requires an owner URI
    if (auto_accept == 1 && auto_accept_from.empty()) {
        std::cerr << "Error: --auto-accept-from requires a Jami URI" << std::endl;
        return 1;
    }

    // --auto-accept and --auto-accept-from conflict
    if (auto_accept_all_explicit && !auto_accept_from.empty()) {
        std::cerr << "Warning: --auto-accept and --auto-accept-from both set; "
                  << "--auto-accept takes precedence (accept all)" << std::endl;
    }
    // --reject-unknown with accept flags
    if (auto_accept == 2 && auto_accept_all_explicit) {
        std::cerr << "Warning: --reject-unknown and --auto-accept both set; "
                  << "--auto-accept takes precedence" << std::endl;
    }
    if (auto_accept == 2 && !auto_accept_from.empty()) {
        std::cerr << "Warning: --reject-unknown and --auto-accept-from both set; "
                  << "--auto-accept-from takes precedence" << std::endl;
    }

    // CLI commands that require --account
    if (mode == Mode::CLI) {
        if (cli_command == "send-message" && (account.empty() || conversation_id.empty() || body.empty())) {
            if (account.empty()) {
                // Will try auto-detect later, not an error here
            }
            if (conversation_id.empty()) {
                std::cerr << "Error: --conversation required for --send-message" << std::endl;
                return 1;
            }
            if (body.empty()) {
                std::cerr << "Error: --body required for --send-message" << std::endl;
                return 1;
            }
        }
        if (cli_command == "list-conversations" && account.empty()) {
            // Will try auto-detect, not an error here
        }
        if (cli_command == "load-messages" && (conversation_id.empty())) {
            std::cerr << "Error: --conversation required for --load-messages" << std::endl;
            return 1;
        }
    }

    // Account path checks
    if (!account.empty() && account != "new") {
        // Check if it's an archive path
        if (account.find("archive://") == 0) {
            std::string path = account.substr(10);
            std::ifstream test(path);
            if (!test.is_open()) {
                std::cerr << "Error: Account archive not found: " << path << std::endl;
                return 1;
            }
        }
        // Note: bare paths like /path/to/file.gz are allowed to not exist.
        // If the file exists, the account will be imported.
        // If it doesn't, a new account will be created and exported to that path.
        // This enables the create-or-reuse pattern:
        //   jami-sdk --account /tmp/bot.gz  (first run: create, subsequent: import)
    }

    return 0;
}

} // namespace jami
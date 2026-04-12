#pragma once
/// @file config.h
/// @brief Configuration for jami-sdk — loads from CLI args, config file, and defaults.
///
/// Every setting can be overridden by CLI argument, config file, or environment
/// variable. Priority: CLI > config file > environment > defaults.
///
/// Config file format (JSON):
///   {
///     "host": "0.0.0.0",
///     "port": 8090,
///     "debug": false,
///     "dataDir": "/path/to/data",
///     "configDir": "/path/to/config",
///     "cacheDir": "/path/to/cache",
///     "account": "archive:///path/to/account.gz",
///     "accountAlias": "bot",
///     "accountPassword": "secret",
///     "autoAccept": false,
///     "autoAcceptFrom": "owner-jami-uri",
///     "rejectUnknown": false,
///     "hook": {
///       "command": "./bot.sh",
///       "events": "onMessageReceived,onConversationReady",
///       "timeout": 30
///     }
///   }

#include <string>
#include <map>

namespace jami {

/// Configuration — all knobs that can be turned.
struct Config {
    // ── Mode ──────────────────────────────────────────────────────────
    /// Run mode: "http", "stdio", or CLI commands
    enum class Mode {
        HTTP,       ///< Default: HTTP REST API server
        STDIO,      ///< JSON-RPC over stdin/stdout
        CLI,        ///< One-shot command, then exit
    };

    Mode mode = Mode::HTTP;

    // ── HTTP server ────────────────────────────────────────────────────
    std::string host = "0.0.0.0";  ///< Listen address
    int port = 8090;                ///< Listen port

    // ── Logging ────────────────────────────────────────────────────────
    bool debug = false;             ///< Enable verbose daemon logging

    // ── Daemon paths ──────────────────────────────────────────────────
    /// Data directory for Jami account storage.
    /// Overrides XDG_DATA_HOME for the daemon.
    /// Empty string = use system default (~/.local/share/jami).
    std::string data_dir;

    /// Config directory for Jami configuration.
    /// Overrides XDG_CONFIG_HOME for the daemon.
    /// Empty string = use system default (~/.config/jami).
    std::string config_dir;

    /// Cache directory for Jami cache.
    /// Overrides XDG_CACHE_HOME for the daemon.
    /// Empty string = use system default (~/.cache/jami).
    std::string cache_dir;

    // ── Account ───────────────────────────────────────────────────────
    /// Account specification. Can be:
    ///   - Empty string: use first existing account, or create new one
    ///   - Account ID (hex string): use specific existing account
    ///   - "archive:///path/to/account.gz": import account from archive
    ///   - "/path/to/account.gz": import account from archive
    ///   - "new": always create a new account
    std::string account;

    /// Alias for newly created accounts (only used when creating).
    std::string account_alias;

    /// Password for account archive (import) or new account (create).
    std::string account_password;

    // ── CLI commands ──────────────────────────────────────────────────
    /// CLI command to execute (empty = not a CLI command).
    /// One of: "send-message", "list-accounts", "list-conversations",
    ///         "load-messages", "stats"
    std::string cli_command;

    /// CLI: conversation ID (for send-message, load-messages).
    std::string conversation_id;

    /// CLI: message body (for send-message).
    std::string body;

    /// CLI: number of messages to load (for load-messages).
    int count = 10;

    // ── Invite policy ────────────────────────────────────────────────
    /// Invite policy for conversation/trust requests:
    ///   -1 (default): passive — emit events, don't accept or decline
    ///    0: accept all requests from anyone (--auto-accept)
    ///    1: accept from owner only, decline others (--auto-accept-from)
    ///    2: reject all requests (--reject-unknown)
    int auto_accept = -1;

    /// Jami URI of the owner — only their invites are accepted
    /// when auto_accept == 1. Checked against metadatas["from"] or
    /// the trust request "from" field.
    std::string auto_accept_from;

    /// Track if --auto-accept was explicitly set (for conflict detection)
    bool auto_accept_all_explicit = false;

    // ── Hook ───────────────────────────────────────────────────────────
    std::string hook_command;                     ///< Command to execute
    std::string hook_events = "onMessageReceived"; ///< Comma-separated event types
    int hook_timeout = 30;                        ///< Seconds per hook invocation

    // ── Config file ────────────────────────────────────────────────────
    /// Path to JSON config file. If set, values are loaded from here
    /// first, then overridden by CLI arguments.
    std::string config_file;

    // ── Methods ────────────────────────────────────────────────────────

    /// Parse command-line arguments into this Config.
    /// Returns 0 on success, -1 on error (message printed to stderr).
    int parse_args(int argc, char* argv[]);

    /// Load config from a JSON file (if config_file is set).
    /// File values have lower priority than CLI args — only fills
    /// in values that haven't been explicitly set by CLI.
    /// Returns 0 on success, -1 on error.
    int load_config_file();

    /// Override daemon XDG directories from config.
    /// Must be called BEFORE libjami::init().
    void apply_daemon_paths() const;

    /// Print help/usage to stdout.
    void print_usage(const char* prog) const;

    /// Validate the config (check for conflicts, missing values).
    /// Returns 0 if valid, -1 if not (message printed to stderr).
    int validate() const;
};

} // namespace jami
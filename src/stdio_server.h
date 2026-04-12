/// @file stdio_server.h
/// @brief JSON-RPC server over stdin/stdout for Jami messaging.
///
/// Reads JSON-RPC requests from stdin, writes JSON-RPC responses to stdout.
/// Events (message received, etc.) are pushed as notifications.
///
/// This is simpler for programmatic access than HTTP — no port conflicts,
/// no CORS, no HTTP library needed on the client side.
///
/// Protocol: JSON-RPC 2.0 over newline-delimited JSON lines.
///
/// Request example:
///   {"jsonrpc":"2.0","method":"sendMessage","params":{"accountId":"...","conversationId":"...","body":"hello"},"id":1}
///
/// Response:
///   {"jsonrpc":"2.0","result":{"sent":true,"conversationId":"..."},"id":1}
///
/// Event notification (pushed by server):
///   {"jsonrpc":"2.0","method":"onMessageReceived","params":{"accountId":"...","conversationId":"...","from":"...","body":"..."}}

#pragma once

#include "client.h"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

namespace jami {

class StdioServer {
public:
    StdioServer(Client& client);
    ~StdioServer();

    /// Run the stdio server loop. Reads requests from stdin,
    /// processes them, writes responses to stdout.
    /// Blocks until EOF on stdin or a "shutdown" request.
    void run();

private:
    Client& client_;
    std::atomic<bool> running_{false};

    /// Process a single JSON-RPC request and return the response.
    std::string handle_request(const std::string& json_line);

    /// Send a JSON-RPC notification (event) to stdout.
    void send_notification(const std::string& method, const std::string& params_json);
};

} // namespace jami
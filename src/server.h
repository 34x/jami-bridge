#pragma once
/// @file server.h
/// @brief HTTP REST server exposing the minimal Jami client API.

#include "client.h"
#include <string>
#include <atomic>
#include <httplib.h>

namespace jami {

class Server {
public:
    Server(Client& client, const std::string& host = "0.0.0.0", int port = 8090);
    void run();
    void stop();

private:
    Client& client_;
    std::string host_;
    int port_;
    httplib::Server* svr_ = nullptr;
    std::atomic<bool> running_{false};
    void setup_routes(httplib::Server* srv);
};

} // namespace jami
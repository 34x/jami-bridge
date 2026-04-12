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
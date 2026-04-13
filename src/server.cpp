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

/// @file server.cpp
/// @brief HTTP REST server for Jami messaging and conversations.
///
/// JSON endpoints:
///   GET  /api/ping                     — health check
///   POST /api/shutdown                  — shut down the server
///   GET  /api/accounts                 — list accounts
///   POST /api/accounts                 — create account {alias, password}
///   GET  /api/accounts/:id             — account details
///   GET  /api/accounts/:id/status      — volatile status
///   DELETE /api/accounts/:id            — remove account
///   POST /api/accounts/:id/export      — export account {path, password}
///   POST /api/accounts/import           — import account {path, password}
///
///   GET  /api/accounts/:id/conversations          — list conversations
///   POST /api/accounts/:id/conversations           — create conversation
///   GET  /api/accounts/:id/conversations/:conv    — conversation info + members
///   DELETE /api/accounts/:id/conversations/:conv   — leave conversation
///   POST /api/accounts/:id/conversations/:conv/invite   — invite member {uri}
///   POST /api/accounts/:id/conversations/:conv/remove    — remove member {uri}
///   POST /api/accounts/:id/conversations/:conv/update   — update info {title, ...}
///
///   GET  /api/accounts/:id/requests       — list conversation requests
///   POST /api/accounts/:id/requests/:conv/accept   — accept request
///   POST /api/accounts/:id/requests/:conv/decline  — decline request
///
///   POST /api/accounts/:id/conversations/:conv/messages — send message {body, parent_id}
///   GET  /api/accounts/:id/conversations/:conv/messages  — load messages {from, count}
///
///   POST /api/accounts/:id/contacts       — add contact {uri}
///   DELETE /api/accounts/:id/contacts/:uri — remove contact {ban}
///   POST /api/accounts/:id/trust-request  — send trust request {uri}
///   GET  /api/accounts/:id/trust-requests — list trust requests

#include "server.h"
#include "client.h"
#include "api_docs.h"
#include "util.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

using json = nlohmann::json;

namespace jami {

// Helper: success JSON response
static void json_ok(httplib::Response& res, const json& body) {
    res.set_content(body.dump(2), "application/json");
    res.status = 200;
}

// Helper: error JSON response
static void json_error(httplib::Response& res, int status, const std::string& msg) {
    res.set_content(json{{"error", msg}}.dump(2), "application/json");
    res.status = status;
}

Server::Server(Client& client, const std::string& host, int port)
    : client_(client), host_(host), port_(port)
{
}

void Server::setup_routes(httplib::Server* svr) {
    // svr is already the httplib::Server pointer — no cast needed
    // All routes use 'svr->' instead of 'svr.'

    // ── API Documentation ──────────────────────────────────────────
    svr->Get("/", [svr](const httplib::Request&, httplib::Response& res) {
        res.set_content(docs::index_html(), "text/html");
    });
    svr->Get("/api/openapi.json", [svr](const httplib::Request&, httplib::Response& res) {
        res.set_content(docs::openapi_json(), "application/json");
    });

    // ── Timeouts ──────────────────────────────────────────────────
    svr->set_payload_max_length(1024 * 1024); // 1 MB max
    svr->set_read_timeout(10);  // seconds — body read timeout (long for uploads)
    svr->set_write_timeout(5);   // seconds — response write timeout
    svr->set_idle_interval(0, 500000); // 500ms idle timeout (microseconds)

    // ── CORS ────────────────────────────────────────────────────────
    // WARNING: Access-Control-Allow-Origin: * means ANY website on the
    // internet can call this API. This is intentional for local dev/bot
    // usage, but on a network-facing deployment, consider restricting
    // to specific origins or adding authentication.
    svr->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
    });
    svr->Options(R"(.*)", [](const httplib::Request&, httplib::Response&) {});

    // ── Health ──────────────────────────────────────────────────────
    svr->Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        json_ok(res, json{{"status", "ok"}, {"version", jami::VERSION}});
    });

    // ── Version ────────────────────────────────────────────────────
    svr->Get("/api/version", [](const httplib::Request&, httplib::Response& res) {
        json_ok(res, json{
            {"version", jami::VERSION},
            {"daemon", "jami"},
            {"mode", "library"},
            {"api", "REST+STDIO+CLI+hook"},
            {"endpoints", json::array({
                "ping", "version", "stats", "shutdown",
                "accounts", "conversations", "messages",
                "requests", "contacts", "lookup"
            })}
        });
    });

    // ── Stats ──────────────────────────────────────────────────────
    svr->Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
        auto& stats = client_.stats();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - stats.start_time
        ).count();
        json_ok(res, json{
            {"uptime_seconds", uptime},
            {"messages_received", stats.messages_received},
            {"messages_sent", stats.messages_sent},
            {"invites_accepted", stats.invites_accepted},
            {"invites_declined", stats.invites_declined},
            {"hook_invocations", stats.hook_invocations},
            {"hook_replies", stats.hook_replies},
            {"hook_timeouts", stats.hook_timeouts},
            {"hook_errors", stats.hook_errors},
        });
    });

    // ── Shutdown ─────────────────────────────────────────────────────
    svr->Post("/api/shutdown", [this](const httplib::Request&, httplib::Response& res) {
        json_ok(res, json{{"status", "shutting_down"}});
        // Stop the server from a separate thread so the response is sent first
        std::thread([this]{
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            svr_->stop();
        }).detach();
    });

    // ── Account Management ──────────────────────────────────────────

    // List accounts
    svr->Get("/api/accounts", [this](const httplib::Request&, httplib::Response& res) {
        auto accounts = client_.list_accounts();
        json_ok(res, json{{"accounts", accounts}});
    });

    // Create account
    svr->Post("/api/accounts", [this](const httplib::Request& req, httplib::Response& res) {
        std::string alias, password;
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                alias = body.value("alias", "");
                password = body.value("password", "");
            } catch (const json::parse_error& e) {
                json_error(res, 400, std::string("Invalid JSON: ") + e.what());
                return;
            }
        }
        auto id = client_.create_account(alias, password);
        json_ok(res, json{{"account_id", id}});
    });

    // Account details
    svr->Get(R"(/api/accounts/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto details = client_.account_details(account_id);
        json_ok(res, json{{"account_id", account_id}, {"details", map_to_json(details)}});
    });

    // Account volatile status
    svr->Get(R"(/api/accounts/([^/]+)/status)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto details = client_.account_volatile_details(account_id);
        json_ok(res, json{{"account_id", account_id}, {"status", map_to_json(details)}});
    });

    // Remove account
    svr->Delete(R"(/api/accounts/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        client_.remove_account(account_id);
        json_ok(res, json{{"removed", true}});
    });

    // Export account
    svr->Post(R"(/api/accounts/([^/]+)/export)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string path, password;
        try {
            auto body = json::parse(req.body);
            path = body.at("path").get<std::string>();
            password = body.value("password", "");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
            return;
        }
        bool ok = client_.export_account(account_id, path, password);
        if (ok) {
            json_ok(res, json{{"exported", true}, {"path", path}});
        } else {
            json_error(res, 500, "Export failed");
        }
    });

    // Import account
    svr->Post("/api/accounts/import", [this](const httplib::Request& req, httplib::Response& res) {
        std::string path, password;
        try {
            auto body = json::parse(req.body);
            path = body.at("path").get<std::string>();
            password = body.value("password", "");
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
            return;
        }
        auto id = client_.import_account(path, password);
        json_ok(res, json{{"account_id", id}});
    });

    // ── Conversations ───────────────────────────────────────────────

    // List conversations (with info)
    svr->Get(R"(/api/accounts/([^/]+)/conversations)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto conv_ids = client_.list_conversations(account_id);
        json conv_list = json::array();
        for (const auto& cid : conv_ids) {
            auto info = client_.conversation_info(account_id, cid);
            auto members = client_.conversation_members(account_id, cid);
            int mode_int = 0;
            try { if (info.count("mode")) mode_int = std::stoi(info.at("mode")); } catch (...) {}
            json mode_val = info.count("mode") ? json(mode_int) : json();
            json entry = {
                {"id", cid},
                {"mode", mode_val},
                {"title", info.count("title") ? info["title"] : ""},
                {"members", members.size()},
            };
            // Mode: null=unknown, 0=one-to-one, 1=admin-only, 2=invites-only, 3=public
            conv_list.push_back(entry);
        }
        json_ok(res, json{{"account_id", account_id}, {"conversations", conv_list}});
    });

    // Create conversation (optional: {"title": "My Room"})
    svr->Post(R"(/api/accounts/([^/]+)/conversations)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string title;
        if (!req.body.empty()) {
            try {
                auto body = json::parse(req.body);
                title = body.value("title", "");
            } catch (...) {}
        }
        auto conv_id = client_.create_conversation(account_id);
        if (!title.empty()) {
            client_.update_conversation_info(account_id, conv_id, {{"title", title}});
        }
        json_ok(res, json{{"account_id", account_id}, {"conversation_id", conv_id}, {"title", title}});
    });

    // Get conversation info + members
    svr->Get(R"(/api/accounts/([^/]+)/conversations/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];

        auto info = client_.conversation_info(account_id, conv_id);
        auto members = client_.conversation_members(account_id, conv_id);

        json members_json = json::array();
        for (const auto& m : members) {
            members_json.push_back(json{{"uri", m.uri}, {"role", m.role}});
        }

        int mode_int = 0;
        try { if (info.count("mode")) mode_int = std::stoi(info.at("mode")); } catch (...) {}
        json mode_val = info.count("mode") ? json(mode_int) : json();
        json_ok(res, json{
            {"account_id", account_id},
            {"conversation_id", conv_id},
            {"mode", mode_val},
            {"title", info.count("title") ? info["title"] : ""},
            {"member_count", members.size()},
            {"members", members_json}
        });
    });

    // Remove (leave) conversation
    svr->Delete(R"(/api/accounts/([^/]+)/conversations/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        bool ok = client_.remove_conversation(account_id, conv_id);
        json_ok(res, json{{"removed", ok}});
    });

    // Invite member
    svr->Post(R"(/api/accounts/([^/]+)/conversations/([^/]+)/invite)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        try {
            auto body = json::parse(req.body);
            auto uri = body.at("uri").get<std::string>();
            client_.invite_member(account_id, conv_id, uri);
            json_ok(res, json{{"invited", true}, {"uri", uri}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // Remove member
    svr->Post(R"(/api/accounts/([^/]+)/conversations/([^/]+)/remove)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        try {
            auto body = json::parse(req.body);
            auto uri = body.at("uri").get<std::string>();
            client_.remove_member(account_id, conv_id, uri);
            json_ok(res, json{{"removed", true}, {"uri", uri}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // Update conversation info
    svr->Post(R"(/api/accounts/([^/]+)/conversations/([^/]+)/update)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        try {
            auto body = json::parse(req.body);
            std::map<std::string, std::string> info;
            for (auto& [k, v] : body.items()) {
                info[k] = v.get<std::string>();
            }
            client_.update_conversation_info(account_id, conv_id, info);
            json_ok(res, json{{"updated", true}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // ── Conversation Requests (Invites) ─────────────────────────────

    // List requests
    svr->Get(R"(/api/accounts/([^/]+)/requests)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto requests = client_.conversation_requests(account_id);
        json arr = json::array();
        for (const auto& r : requests) {
            arr.push_back(map_to_json(r));
        }
        json_ok(res, json{{"account_id", account_id}, {"requests", arr}});
    });

    // Accept request
    svr->Post(R"(/api/accounts/([^/]+)/requests/([^/]+)/accept)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        client_.accept_request(account_id, conv_id);
        json_ok(res, json{{"accepted", true}, {"conversation_id", conv_id}});
    });

    // Decline request
    svr->Post(R"(/api/accounts/([^/]+)/requests/([^/]+)/decline)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        client_.decline_request(account_id, conv_id);
        json_ok(res, json{{"declined", true}, {"conversation_id", conv_id}});
    });

    // ── Messaging ───────────────────────────────────────────────────

    // Send message
    svr->Post(R"(/api/accounts/([^/]+)/conversations/([^/]+)/messages)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        try {
            auto body = json::parse(req.body);
            auto message = body.at("body").get<std::string>();
            auto parent_id = body.value("parent_id", "");
            client_.send_message(account_id, conv_id, message, parent_id);
            json_ok(res, json{{"sent", true}, {"conversation_id", conv_id}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // Load messages
    svr->Get(R"(/api/accounts/([^/]+)/conversations/([^/]+)/messages)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string conv_id = req.matches[2];
        std::string from_id = req.has_param("from") ? req.get_param_value("from") : "";
        size_t count = req.has_param("count") ? std::stoul(req.get_param_value("count")) : 64;

        auto messages = client_.load_messages_sync(account_id, conv_id, from_id, count);

        json msgs_json = json::array();
        for (const auto& msg : messages) {
            json m;
            m["id"] = msg.id;
            m["type"] = msg.type;
            m["from"] = msg.body.count("author") ? msg.body.at("author") : "";
            // In SwarmMessage, "body" is the entire commit map.
            // The actual text content is in msg.body["body"].
            m["body"] = msg.body.count("body") ? msg.body.at("body") : "";
            m["parent_id"] = msg.linearizedParent;
            m["timestamp"] = msg.body.count("timestamp") ? msg.body.at("timestamp") : "";
            msgs_json.push_back(m);
        }

        json_ok(res, json{
            {"account_id", account_id},
            {"conversation_id", conv_id},
            {"count", msgs_json.size()},
            {"messages", msgs_json}
        });
    });

    // ── Contacts & Trust ────────────────────────────────────────────

    // Add contact
    svr->Post(R"(/api/accounts/([^/]+)/contacts)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            auto uri = body.at("uri").get<std::string>();
            client_.add_contact(account_id, uri);
            json_ok(res, json{{"added", true}, {"uri", uri}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // Remove contact
    svr->Delete(R"(/api/accounts/([^/]+)/contacts/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string uri = req.matches[2];
        bool ban = req.has_param("ban") && req.get_param_value("ban") == "true";
        client_.remove_contact(account_id, uri, ban);
        json_ok(res, json{{"removed", true}, {"uri", uri}});
    });

    // Send trust request
    svr->Post(R"(/api/accounts/([^/]+)/trust-request)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            auto uri = body.at("uri").get<std::string>();
            client_.send_trust_request(account_id, uri);
            json_ok(res, json{{"sent", true}, {"uri", uri}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // List trust requests
    svr->Get(R"(/api/accounts/([^/]+)/trust-requests)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto requests = client_.trust_requests(account_id);
        json arr = json::array();
        for (const auto& r : requests) {
            arr.push_back(map_to_json(r));
        }
        json_ok(res, json{{"account_id", account_id}, {"requests", arr}});
    });

    // ── Name Service ────────────────────────────────────────────────

    // Lookup name
    svr->Get(R"(/api/accounts/([^/]+)/lookup/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        std::string name = req.matches[2];
        bool ok = client_.lookup_name(account_id, name);
        json_ok(res, json{{"account_id", account_id}, {"name", name}, {"requested", ok}});
    });

    // ── Contacts ──────────────────────────────────────────────────────

    // List contacts for an account
    svr->Get(R"(/api/accounts/([^/]+)/contacts)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        auto contacts = client_.list_contacts(account_id);
        json arr = json::array();
        for (const auto& c : contacts) {
            arr.push_back(map_to_json(c));
        }
        json_ok(res, json{{"account_id", account_id}, {"contacts", arr}});
    });

    // Set account active/inactive
    svr->Post(R"(/api/accounts/([^/]+)/active)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string account_id = req.matches[1];
        try {
            auto body = json::parse(req.body);
            bool active = body.at("active").get<bool>();
            client_.set_account_active(account_id, active);
            json_ok(res, json{{"account_id", account_id}, {"active", active}});
        } catch (const std::exception& e) {
            json_error(res, 400, std::string("Invalid request: ") + e.what());
        }
    });

    // ── Catch-all: fast 404 for unknown routes ───────────────────
    // httplib processes routes in registration order, so these wildcard
    // handlers will only match if no earlier route matched.
    svr->Get(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        json_error(res, 404, "Not found: " + req.method + " " + req.path);
    });
    svr->Post(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        json_error(res, 404, "Not found: " + req.method + " " + req.path);
    });
    svr->Put(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        json_error(res, 404, "Not found: " + req.method + " " + req.path);
    });
    svr->Delete(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        json_error(res, 404, "Not found: " + req.method + " " + req.path);
    });
}

void Server::run() {
    httplib::Server svr;
    setup_routes(&svr);
    svr_ = &svr;
    running_ = true;

    std::cout << "[jami-bridge] HTTP server listening on " << host_ << ":" << port_ << std::endl;

    if (!svr.listen(host_, port_)) {
        std::cerr << "[jami-bridge] Failed to bind " << host_ << ":" << port_ << std::endl;
    }
    running_ = false;
    svr_ = nullptr;
}

void Server::stop() {
    if (svr_) {
        svr_->stop();
    }
}

} // namespace jami
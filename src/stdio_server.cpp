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

/// @file stdio_server.cpp
/// @brief JSON-RPC 2.0 server over stdin/stdout.
///
/// Newline-delimited JSON on stdin/stdout. The same API as the HTTP server
/// but accessed via JSON-RPC method calls instead of HTTP routes.
///
/// Methods mirror the REST API:
///   ping                             — health check
///   shutdown                         — shut down
///   listAccounts                     — list account IDs
///   createAccount    {alias,password} — create account
///   importAccount    {path,password?} — import account from archive file
///   exportAccount    {accountId,path,password?} — export account to archive file
///   getAccountDetails {accountId}    — account details
///   setAccountDetails {accountId,details} — update account details (alias, etc.)
///   updateProfile     {accountId,displayName?,avatar?} — push display name/avatar to contacts
///   getAccountStatus  {accountId}    — volatile status
///   removeAccount    {accountId}     — remove account
///   registerName     {accountId,name,password?} — register a public name
///   listConversations {accountId}    — list conversations
///   createConversation {accountId,title?} — create conversation
///   getConversation   {accountId,conversationId} — info + members
///   removeConversation {accountId,conversationId} — leave
///   inviteMember     {accountId,conversationId,uri} — invite
///   sendMessage      {accountId,conversationId,body} — send message
///   editMessage      {accountId,conversationId,body,messageId} — edit message
///   loadMessages     {accountId,conversationId,count?,from?} — load messages
///   acceptRequest    {accountId,conversationId} — accept invite
///   declineRequest   {accountId,conversationId} — decline invite
///   listRequests     {accountId}     — list pending requests
///   addContact       {accountId,uri} — add contact
///   removeContact    {accountId,uri} — remove contact
///
///   ── File Transfers ──────────────────────────────────────────────
///   sendFile         {accountId,conversationId,path,displayName?,replyTo?} — send file
///   downloadFile     {accountId,conversationId,interactionId,fileId,path} — download file
///   cancelTransfer   {accountId,conversationId,fileId} — cancel transfer
///   transferInfo     {accountId,conversationId,fileId} — get transfer info
///
/// Events (pushed as notifications):
///   onMessageReceived   {accountId,conversationId,from,body,id,type,timestamp,parentId}
///   onRegistrationChanged {accountId,state,code,detail}
///   onConversationRequestReceived {accountId,conversationId}
///   onDataTransferEvent {accountId,conversationId,interactionId,fileId,eventCode,path?,totalSize?,bytesProgress?}


#include "log.h"
#include "stdio_server.h"
#include "util.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <chrono>

using json = nlohmann::json;

namespace jami {

// Static mutex definition
std::mutex StdioServer::stdout_mtx_;

// Thread-safe write to stdout
void StdioServer::write_stdout_locked(const std::string& line) {
    std::lock_guard<std::mutex> lock(stdout_mtx_);
    std::cout << line << std::endl;
}

// Write a line to stdout (legacy — not thread-safe)
static void write_stdout(const std::string& line) {
    StdioServer::write_stdout_locked(line);
}

StdioServer::StdioServer(Client& client)
    : client_(client)
{
}

StdioServer::~StdioServer() {
    running_ = false;
}

void StdioServer::send_notification(const std::string& method, const std::string& params_json) {
    json notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = method;
    // params_json is already a JSON string
    notification["params"] = json::parse(params_json);
    write_stdout(notification.dump());
}

void StdioServer::run() {
    running_ = true;

    // Notify the client that we're ready to process requests
    send_notification("onReady", "{}");

    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::string response = handle_request(line);
        if (!response.empty()) {
            write_stdout(response);
        }
    }

    running_ = false;
    jami::log_tag("stdio", "Stdin closed, exiting.");
}

std::string StdioServer::handle_request(const std::string& json_line) {
    json req;
    try {
        req = json::parse(json_line);
    } catch (const json::parse_error& e) {
        json err;
        err["jsonrpc"] = "2.0";
        err["error"] = {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}};
        err["id"] = nullptr;
        return err.dump();
    }

    std::string method = req.value("method", "");
    auto params = req.value("params", json::object());
    auto id = req.value("id", json(nullptr));

    // Only JSON-RPC 2.0
    if (req.value("jsonrpc", "") != "2.0") {
        json err;
        err["jsonrpc"] = "2.0";
        err["error"] = {{"code", -32600}, {"message", "Invalid request: jsonrpc must be \"2.0\""}};
        err["id"] = id;
        return err.dump();
    }

    if (method.empty()) {
        json err;
        err["jsonrpc"] = "2.0";
        err["error"] = {{"code", -32600}, {"message", "Invalid request: missing method"}};
        err["id"] = id;
        return err.dump();
    }

    // Helper to build success response
    auto make_result = [&](const json& result) -> std::string {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["result"] = result;
        resp["id"] = id;
        return resp.dump();
    };

    // Helper to build error response
    auto make_error = [&](int code, const std::string& msg) -> std::string {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["error"] = {{"code", code}, {"message", msg}};
        resp["id"] = id;
        return resp.dump();
    };

    // ── Dispatch ──────────────────────────────────────────────────────

    try {

    if (method == "ping") {
        return make_result({{"status", "ok"}, {"version", jami::VERSION}});
    }

    if (method == "version") {
        return make_result({
            {"version", jami::VERSION},
            {"daemon", "jami"},
            {"mode", "library"},
            {"api", "REST+STDIO+CLI+hook"},
        });
    }

    if (method == "stats") {
        auto& stats = client_.stats();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - stats.start_time
        ).count();
        return make_result({
            {"uptimeSeconds", uptime},
            {"messagesReceived", stats.messages_received},
            {"messagesSent", stats.messages_sent},
            {"hookInvocations", stats.hook_invocations},
            {"hookReplies", stats.hook_replies},
            {"hookTimeouts", stats.hook_timeouts},
            {"hookErrors", stats.hook_errors},
        });
    }

    if (method == "shutdown") {
        running_ = false;
        return make_result({{"status", "shutting_down"}});
    }

    // ── Account Management ───────────────────────────────────────────

    if (method == "listAccounts") {
        auto accounts = client_.list_accounts();
        return make_result({{"accounts", accounts}});
    }

    if (method == "createAccount") {
        std::string alias = params.value("alias", "");
        std::string password = params.value("password", "");
        auto id = client_.create_account(alias, password);
        return make_result({{"accountId", id}});
    }

    if (method == "importAccount") {
        std::string path = params.at("path");
        std::string password = params.value("password", "");
        auto id = client_.import_account(path, password);
        if (id.empty()) {
            return make_error(-32603, "Failed to import account from: " + path);
        }
        return make_result({{"accountId", id}});
    }

    if (method == "exportAccount") {
        std::string account_id = params.at("accountId");
        std::string path = params.at("path");
        std::string password = params.value("password", "");
        bool ok = client_.export_account(account_id, path, password);
        if (!ok) {
            return make_error(-32603, "Failed to export account to: " + path);
        }
        return make_result({{"exported", true}, {"path", path}});
    }

    if (method == "getAccountDetails") {
        std::string account_id = params.at("accountId");
        auto details = client_.account_details(account_id);
        return make_result({{"accountId", account_id}, {"details", map_to_json(details)}});
    }

    if (method == "getAccountStatus") {
        std::string account_id = params.at("accountId");
        auto details = client_.account_volatile_details(account_id);
        return make_result({{"accountId", account_id}, {"status", map_to_json(details)}});
    }

    if (method == "setAccountDetails") {
        std::string account_id = params.at("accountId");
        std::map<std::string, std::string> details;
        if (params.contains("details") && params["details"].is_object()) {
            for (auto& [k, v] : params["details"].items()) {
                details[k] = v.get<std::string>();
            }
        }
        client_.set_account_details(account_id, details);
        return make_result({{"updated", true}, {"accountId", account_id}});
    }

    if (method == "updateProfile") {
        std::string account_id = params.at("accountId");
        std::string display_name = params.value("displayName", "");
        std::string avatar = params.value("avatar", "");
        client_.update_profile(account_id, display_name, avatar);
        return make_result({{"updated", true}, {"accountId", account_id}});
    }

    if (method == "removeAccount") {
        std::string account_id = params.at("accountId");
        client_.remove_account(account_id);
        return make_result({{"removed", true}});
    }

    if (method == "registerName") {
        std::string account_id = params.at("accountId");
        std::string name = params.at("name");
        std::string password = params.value("password", "");
        bool ok = client_.register_name(account_id, password, name);
        return make_result({{"registered", ok}, {"accountId", account_id}, {"name", name}});
    }

    // ── Conversations ────────────────────────────────────────────────

    if (method == "listConversations") {
        std::string account_id = params.at("accountId");
        auto conv_ids = client_.list_conversations(account_id);
        json conv_list = json::array();
        for (const auto& cid : conv_ids) {
            auto info = client_.conversation_info(account_id, cid);
            auto members = client_.conversation_members(account_id, cid);
            int mode_int = 0;
            try { if (info.count("mode")) mode_int = std::stoi(info.at("mode")); } catch (...) {}
            json mode_val = info.count("mode") ? json(mode_int) : json();
            conv_list.push_back({
                {"id", cid},
                {"mode", mode_val},
                {"title", info.count("title") ? info["title"] : ""},
                {"members", members.size()},
            });
        }
        return make_result({{"accountId", account_id}, {"conversations", conv_list}});
    }

    if (method == "createConversation") {
        std::string account_id = params.at("accountId");
        std::string title = params.value("title", "");
        auto conv_id = client_.create_conversation(account_id);
        if (!title.empty()) {
            client_.update_conversation_info(account_id, conv_id, {{"title", title}});
        }
        return make_result({{"accountId", account_id}, {"conversationId", conv_id}, {"title", title}});
    }

    if (method == "getConversation") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        auto info = client_.conversation_info(account_id, conv_id);
        auto members = client_.conversation_members(account_id, conv_id);

        json members_json = json::array();
        for (const auto& m : members) {
            members_json.push_back({{"uri", m.uri}, {"role", m.role}});
        }

        int mode_int = 0;
        try { if (info.count("mode")) mode_int = std::stoi(info.at("mode")); } catch (...) {}
        json mode_val = info.count("mode") ? json(mode_int) : json();
        return make_result({
            {"accountId", account_id},
            {"conversationId", conv_id},
            {"mode", mode_val},
            {"title", info.count("title") ? info["title"] : ""},
            {"memberCount", members.size()},
            {"members", members_json},
        });
    }

    if (method == "removeConversation") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        bool ok = client_.remove_conversation(account_id, conv_id);
        return make_result({{"removed", ok}});
    }

    if (method == "inviteMember") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string uri = params.at("uri");
        client_.invite_member(account_id, conv_id, uri);
        return make_result({{"invited", true}, {"uri", uri}});
    }

    if (method == "removeMember") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string uri = params.at("uri");
        client_.remove_member(account_id, conv_id, uri);
        return make_result({{"removed", true}, {"uri", uri}});
    }

    if (method == "updateConversation") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::map<std::string, std::string> info;
        if (params.contains("info") && params["info"].is_object()) {
            for (auto& [k, v] : params["info"].items()) {
                info[k] = v.get<std::string>();
            }
        }
        client_.update_conversation_info(account_id, conv_id, info);
        return make_result({{"updated", true}});
    }

    // ── Conversation Requests ────────────────────────────────────────

    if (method == "listRequests") {
        std::string account_id = params.at("accountId");
        auto requests = client_.conversation_requests(account_id);
        json arr = json::array();
        for (const auto& r : requests) {
            arr.push_back(map_to_json(r));
        }
        return make_result({{"accountId", account_id}, {"requests", arr}});
    }

    if (method == "acceptRequest") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        client_.accept_request(account_id, conv_id);
        return make_result({{"accepted", true}, {"conversationId", conv_id}});
    }

    if (method == "declineRequest") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        client_.decline_request(account_id, conv_id);
        return make_result({{"declined", true}, {"conversationId", conv_id}});
    }

    // ── Messaging ─────────────────────────────────────────────────────

    if (method == "sendMessage") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string body_str = params.at("body");
        std::string parent_id = params.value("parentId", "");
        client_.send_message(account_id, conv_id, body_str, parent_id);
        return make_result({{"sent", true}, {"conversationId", conv_id}});
    }

    if (method == "editMessage") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string new_body = params.at("body");
        std::string edited_id = params.at("messageId");
        client_.edit_message(account_id, conv_id, new_body, edited_id);
        return make_result({{"edited", true}, {"conversationId", conv_id}});
    }

    if (method == "loadMessages") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string from_id = params.value("from", "");
        size_t count = params.value("count", 64);

        auto messages = client_.load_messages_sync(account_id, conv_id, from_id, count);

        json msgs_json = json::array();
        for (const auto& msg : messages) {
            msgs_json.push_back({
                {"id", msg.id},
                {"type", msg.type},
                {"from", msg.body.count("author") ? msg.body.at("author") : ""},
                {"body", msg.body.count("body") ? msg.body.at("body") : ""},
                {"parentId", msg.linearizedParent},
                {"timestamp", msg.body.count("timestamp") ? msg.body.at("timestamp") : ""},
            });
        }

        return make_result({
            {"accountId", account_id},
            {"conversationId", conv_id},
            {"count", msgs_json.size()},
            {"messages", msgs_json},
        });
    }

    // ── Contacts & Trust ─────────────────────────────────────────────

    if (method == "addContact") {
        std::string account_id = params.at("accountId");
        std::string uri = params.at("uri");
        client_.add_contact(account_id, uri);
        return make_result({{"added", true}, {"uri", uri}});
    }

    if (method == "removeContact") {
        std::string account_id = params.at("accountId");
        std::string uri = params.at("uri");
        client_.remove_contact(account_id, uri);
        return make_result({{"removed", true}, {"uri", uri}});
    }

    if (method == "listContacts") {
        std::string account_id = params.at("accountId");
        auto contacts = client_.list_contacts(account_id);
        json arr = json::array();
        for (const auto& c : contacts) {
            arr.push_back(map_to_json(c));
        }
        return make_result({{"accountId", account_id}, {"contacts", arr}});
    }

    if (method == "sendTrustRequest") {
        std::string account_id = params.at("accountId");
        std::string uri = params.at("uri");
        client_.send_trust_request(account_id, uri);
        return make_result({{"sent", true}, {"uri", uri}});
    }

    if (method == "listTrustRequests") {
        std::string account_id = params.at("accountId");
        auto requests = client_.trust_requests(account_id);
        json arr = json::array();
        for (const auto& r : requests) {
            arr.push_back(map_to_json(r));
        }
        return make_result({{"accountId", account_id}, {"requests", arr}});
    }

    if (method == "acceptTrustRequest") {
        std::string account_id = params.at("accountId");
        std::string from_uri = params.at("from");
        bool ok = client_.accept_trust_request(account_id, from_uri);
        return make_result({{"accepted", ok}, {"from", from_uri}});
    }

    if (method == "declineTrustRequest") {
        std::string account_id = params.at("accountId");
        std::string from_uri = params.at("from");
        bool ok = client_.decline_trust_request(account_id, from_uri);
        return make_result({{"declined", ok}, {"from", from_uri}});
    }

    // ── File Transfers ──────────────────────────────────────────────

    if (method == "sendFile") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string path = params.at("path");
        std::string display_name = params.value("displayName", "");
        std::string reply_to = params.value("replyTo", "");
        client_.send_file(account_id, conv_id, path, display_name, reply_to);
        return make_result({{"sent", true}, {"conversationId", conv_id}, {"path", path}});
    }

    if (method == "downloadFile") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string interaction_id = params.at("interactionId");
        std::string file_id = params.at("fileId");
        std::string download_path = params.at("path");
        bool ok = client_.download_file(account_id, conv_id, interaction_id, file_id, download_path);
        return make_result({{"downloading", ok}, {"conversationId", conv_id}, {"path", download_path}});
    }

    if (method == "cancelTransfer") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string file_id = params.at("fileId");
        auto err = client_.cancel_data_transfer(account_id, conv_id, file_id);
        return make_result({{"cancelled", err == libjami::DataTransferError::success},
                           {"errorCode", static_cast<int>(err)}});
    }

    if (method == "transferInfo") {
        std::string account_id = params.at("accountId");
        std::string conv_id = params.at("conversationId");
        std::string file_id = params.at("fileId");
        jami::FileTransfer info;
        bool found = client_.file_transfer_info(account_id, conv_id, file_id, info);
        if (!found) {
            return make_error(-32603, "Transfer not found: " + file_id);
        }
        return make_result({
            {"accountId", account_id},
            {"conversationId", conv_id},
            {"fileId", file_id},
            {"path", info.path},
            {"totalSize", info.total_size},
            {"bytesProgress", info.bytes_progress},
            {"eventCode", info.event_code}
        });
    }

    // ── Unknown method ───────────────────────────────────────────────

    return make_error(-32601, "Method not found: " + method);

    } catch (const json::out_of_range& e) {
        return make_error(-32602, std::string("Invalid params: missing required field — ") + e.what());
    } catch (const json::type_error& e) {
        return make_error(-32602, std::string("Invalid params: type error — ") + e.what());
    } catch (const std::exception& e) {
        return make_error(-32603, std::string("Internal error: ") + e.what());
    }
}

} // namespace jami
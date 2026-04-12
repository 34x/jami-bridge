/// @file client.cpp
/// @brief Minimal Jami client implementation — messaging and conversations.
///
/// Initializes the daemon in library mode and exposes a simple API.
/// No DBus — the daemon runs inside our process.
///
/// The daemon's public API lives in the `libjami` namespace.
/// Signal handlers are registered via libjami::registerSignalHandlers()
/// with libjami::exportable_callback<libjami::SignalType>.

#include "client.h"

#include <iostream>
#include <chrono>
#include <thread>

namespace jami {

// ── Construction / Destruction ────────────────────────────────────────────

Client::Client(const Events& events, bool debug)
    : events_(events)
{
    stats_.start_time = std::chrono::steady_clock::now();
    // Register signal handlers BEFORE init/start.
    // The new API uses libjami::exportable_callback<libjami::SignalType>
    // which wraps std::function in a CallbackWrapper for ABI stability.
    std::lock_guard<std::mutex> lock(cb_mutex_);

    libjami::registerSignalHandlers({
        // Swarm message received (the modern conversation protocol)
        libjami::exportable_callback<libjami::ConversationSignal::SwarmMessageReceived>(
            [this](const std::string& account_id,
                   const std::string& conv_id,
                   const libjami::SwarmMessage& swarm_msg) {
                Message msg;
                msg.account_id = account_id;
                msg.conversation_id = conv_id;
                msg.id = swarm_msg.id;
                msg.type = swarm_msg.type;
                msg.parent_id = swarm_msg.linearizedParent;
                // SwarmMessageReceived: body map has "text/plain" key
                // SwarmLoaded (history): body map has "body" key (commit format)
                auto it = swarm_msg.body.find("text/plain");
                if (it != swarm_msg.body.end()) {
                    msg.body = it->second;
                } else {
                    it = swarm_msg.body.find("body");
                    if (it != swarm_msg.body.end()) msg.body = it->second;
                }
                auto from_it = swarm_msg.body.find("author");
                if (from_it != swarm_msg.body.end()) {
                    msg.from = from_it->second;
                }

                if (events_.on_message_received) {
                    stats_.messages_received++;
                    events_.on_message_received(msg);
                }
            }),

        // Swarm message updated (edit, reaction, etc.)
        libjami::exportable_callback<libjami::ConversationSignal::SwarmMessageUpdated>(
            [this](const std::string& account_id,
                   const std::string& conv_id,
                   const libjami::SwarmMessage& swarm_msg) {
                // Treat updated messages the same as received for now
                Message msg;
                msg.account_id = account_id;
                msg.conversation_id = conv_id;
                msg.id = swarm_msg.id;
                msg.type = swarm_msg.type;
                msg.parent_id = swarm_msg.linearizedParent;
                auto it = swarm_msg.body.find("text/plain");
                if (it != swarm_msg.body.end()) {
                    msg.body = it->second;
                } else {
                    it = swarm_msg.body.find("body");
                    if (it != swarm_msg.body.end()) msg.body = it->second;
                }
                auto from_it = swarm_msg.body.find("author");
                if (from_it != swarm_msg.body.end()) msg.from = from_it->second;
                if (events_.on_message_received) {
                    stats_.messages_received++;
                    events_.on_message_received(msg);
                }
            }),

        // Conversation member event (add/join/leave/ban)
        libjami::exportable_callback<libjami::ConversationSignal::ConversationMemberEvent>(
            [this](const std::string& account_id,
                   const std::string& conv_id,
                   const std::string& member_uri,
                   int event) {
                if (events_.on_conversation_member_event) {
                    events_.on_conversation_member_event(account_id, conv_id, member_uri, event);
                }
            }),

        // Conversation ready
        libjami::exportable_callback<libjami::ConversationSignal::ConversationReady>(
            [this](const std::string& account_id,
                   const std::string& conv_id) {
                if (events_.on_conversation_ready) {
                    events_.on_conversation_ready(account_id, conv_id);
                }
            }),

        // Conversation request received (invite to join a room)
        libjami::exportable_callback<libjami::ConversationSignal::ConversationRequestReceived>(
            [this](const std::string& account_id,
                   const std::string& conv_id,
                   std::map<std::string, std::string> metadatas) {
                if (events_.on_conversation_request_received) {
                    events_.on_conversation_request_received(account_id, conv_id, metadatas);
                }
            }),

        // Trust/contact request received (someone wants to add us as contact)
        libjami::exportable_callback<libjami::ConfigurationSignal::IncomingTrustRequest>(
            [this](const std::string& account_id,
                   const std::string& from,
                   const std::string& conv_id,
                   const std::vector<uint8_t>& /*payload*/,
                   time_t /*received*/) {
                if (events_.on_trust_request_received) {
                    events_.on_trust_request_received(account_id, from, conv_id);
                }
            }),

        // Registration state changed
        libjami::exportable_callback<libjami::ConfigurationSignal::RegistrationStateChanged>(
            [this](const std::string& account_id,
                   const std::string& state,
                   int code,
                   const std::string& detail) {
                if (events_.on_registration_changed) {
                    events_.on_registration_changed(account_id, state, code, detail);
                }
            }),

        // Message status changed (sent, delivered, read)
        libjami::exportable_callback<libjami::ConfigurationSignal::AccountMessageStatusChanged>(
            [this](const std::string& account_id,
                   const std::string& conversation_id,
                   const std::string& peer,
                   const std::string& message_id,
                   int state) {
                if (events_.on_message_status_changed) {
                    events_.on_message_status_changed(account_id, conversation_id, peer, message_id, state);
                }
            }),

        // Messages loaded (response to loadConversation)
        libjami::exportable_callback<libjami::ConversationSignal::SwarmLoaded>(
            [this](uint32_t id,
                   const std::string& account_id,
                   const std::string& conv_id,
                   std::vector<libjami::SwarmMessage> messages) {
                if (events_.on_messages_loaded) {
                    events_.on_messages_loaded(id, account_id, conv_id, messages);
                }
            }),
    });

    // Initialize the daemon
    auto flags = debug
        ? static_cast<libjami::InitFlag>(libjami::LIBJAMI_FLAG_DEBUG | libjami::LIBJAMI_FLAG_CONSOLE_LOG)
        : libjami::LIBJAMI_FLAG_CONSOLE_LOG;
    if (!libjami::init(flags)) {
        throw std::runtime_error("Failed to initialize Jami daemon");
    }

    if (!libjami::start()) {
        libjami::fini();
        throw std::runtime_error("Failed to start Jami daemon");
    }

    running_ = true;
    std::cerr << "[jami-sdk] Daemon initialized and running" << std::endl;
}

Client::~Client() {
    if (running_) {
        libjami::fini();
        running_ = false;
        std::cerr << "[jami-sdk] Daemon shut down" << std::endl;
    }
}

// ── Account Management ──────────────────────────────────────────────────

std::string Client::create_account(const std::string& alias, const std::string& password) {
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";
    if (!alias.empty()) {
        details["Account.alias"] = alias;
    }
    if (!password.empty()) {
        details["Account.archivePassword"] = password;
    }
    auto account_id = libjami::addAccount(details);
    return account_id;
}

std::vector<std::string> Client::list_accounts() const {
    return libjami::getAccountList();
}

std::map<std::string, std::string> Client::account_details(const std::string& account_id) const {
    return libjami::getAccountDetails(account_id);
}

std::map<std::string, std::string> Client::account_volatile_details(const std::string& account_id) const {
    return libjami::getVolatileAccountDetails(account_id);
}

void Client::remove_account(const std::string& account_id) {
    libjami::removeAccount(account_id);
}

bool Client::export_account(const std::string& account_id,
                              const std::string& path,
                              const std::string& password) {
    return libjami::exportToFile(account_id, path, password);
}

std::string Client::import_account(const std::string& path, const std::string& password) {
    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";
    details["Account.archivePath"] = path;
    if (!password.empty()) {
        details["Account.archivePassword"] = password;
    }
    return libjami::addAccount(details);
}

void Client::update_callbacks(const Events& events) {
    // Update the event callbacks. This is used by the hook system to
    // wrap existing callbacks with hook dispatch logic.
    // Safe to call after construction — signal handlers access this->events_.
    std::lock_guard<std::mutex> lock(cb_mutex_);
    events_ = events;
}

void Client::set_account_active(const std::string& account_id, bool active) {
    libjami::setAccountActive(account_id, active);
}

// ── Conversations (Rooms) ────────────────────────────────────────────────

std::string Client::create_conversation(const std::string& account_id) {
    return libjami::startConversation(account_id);
}

std::vector<std::string> Client::list_conversations(const std::string& account_id) const {
    return libjami::getConversations(account_id);
}

std::map<std::string, std::string> Client::conversation_info(
    const std::string& account_id, const std::string& conv_id) const {
    return libjami::conversationInfos(account_id, conv_id);
}

std::vector<Member> Client::conversation_members(
    const std::string& account_id, const std::string& conv_id) const {
    auto raw_members = libjami::getConversationMembers(account_id, conv_id);
    std::vector<Member> members;
    members.reserve(raw_members.size());
    for (const auto& m : raw_members) {
        Member member;
        member.uri = m.at("uri");
        member.role = m.at("role");
        member.last_displayed = m.count("lastDisplayed") ? m.at("lastDisplayed") : "";
        members.push_back(member);
    }
    return members;
}

void Client::invite_member(const std::string& account_id,
                            const std::string& conv_id,
                            const std::string& uri) {
    libjami::addConversationMember(account_id, conv_id, uri);
}

void Client::remove_member(const std::string& account_id,
                            const std::string& conv_id,
                            const std::string& uri) {
    libjami::removeConversationMember(account_id, conv_id, uri);
}

bool Client::remove_conversation(const std::string& account_id,
                                  const std::string& conv_id) {
    return libjami::removeConversation(account_id, conv_id);
}

void Client::update_conversation_info(const std::string& account_id,
                                       const std::string& conv_id,
                                       const std::map<std::string, std::string>& info) {
    libjami::updateConversationInfos(account_id, conv_id, info);
}

// ── Conversation Requests (Invites) ──────────────────────────────────────

std::vector<std::map<std::string, std::string>> Client::conversation_requests(
    const std::string& account_id) const {
    return libjami::getConversationRequests(account_id);
}

void Client::accept_request(const std::string& account_id,
                             const std::string& conv_id) {
    libjami::acceptConversationRequest(account_id, conv_id);
}

void Client::decline_request(const std::string& account_id,
                              const std::string& conv_id) {
    libjami::declineConversationRequest(account_id, conv_id);
}

// ── Messaging ────────────────────────────────────────────────────────────

std::string Client::send_message(const std::string& account_id,
                                  const std::string& conv_id,
                                  const std::string& body,
                                  const std::string& parent_id) {
    // libjami::sendMessage(accountId, conversationId, message, replyTo, flag)
    stats_.messages_sent++;
    libjami::sendMessage(account_id, conv_id, body, parent_id);
    // sendMessage is void — message ID arrives via AccountMessageStatusChanged signal
    return "";
}

std::string Client::edit_message(const std::string& account_id,
                                  const std::string& conv_id,
                                  const std::string& new_body,
                                  const std::string& edited_id) {
    stats_.messages_sent++;
    // libjami::sendMessage with flag=1 means edit
    // The "message" is the new body, "commitId" is the ID of the message being edited
    libjami::sendMessage(account_id, conv_id, new_body, edited_id, 1);
    return "";
}

uint32_t Client::load_messages(const std::string& account_id,
                                const std::string& conv_id,
                                const std::string& from_id,
                                size_t count) {
    // libjami::loadConversation(accountId, conversationId, fromMessage, n)
    return libjami::loadConversation(account_id, conv_id, from_id, count);
}

std::vector<libjami::SwarmMessage> Client::load_messages_sync(
        const std::string& account_id,
        const std::string& conv_id,
        const std::string& from_id,
        size_t count,
        int timeout_ms) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    std::vector<libjami::SwarmMessage> result;

    // Save old callback and install our temporary one
    auto old_cb = events_.on_messages_loaded;
    events_.on_messages_loaded = [&](uint32_t id,
                                          const std::string& acc_id,
                                          const std::string& c_id,
                                          const std::vector<libjami::SwarmMessage>& msgs) {
        if (acc_id == account_id && c_id == conv_id) {
            result = msgs;
            done = true;
            cv.notify_one();
        }
    };

    // Request messages
    load_messages(account_id, conv_id, from_id, count);

    // Wait for response
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]{ return done; });

    // Restore old callback
    events_.on_messages_loaded = old_cb;

    return result;
}

// ── Contacts & Trust ─────────────────────────────────────────────────────

void Client::send_trust_request(const std::string& account_id,
                                 const std::string& to_uri) {
    libjami::sendTrustRequest(account_id, to_uri);
}

bool Client::accept_trust_request(const std::string& account_id,
                                    const std::string& from_uri) {
    return libjami::acceptTrustRequest(account_id, from_uri);
}

bool Client::decline_trust_request(const std::string& account_id,
                                    const std::string& from_uri) {
    return libjami::discardTrustRequest(account_id, from_uri);
}

std::vector<std::map<std::string, std::string>> Client::trust_requests(
    const std::string& account_id) const {
    return libjami::getTrustRequests(account_id);
}

void Client::add_contact(const std::string& account_id, const std::string& uri) {
    libjami::addContact(account_id, uri);
}

void Client::remove_contact(const std::string& account_id, const std::string& uri, bool ban) {
    libjami::removeContact(account_id, uri, ban);
}

std::vector<std::map<std::string, std::string>> Client::list_contacts(const std::string& account_id) const {
    return libjami::getContacts(account_id);
}

// ── Name Service ─────────────────────────────────────────────────────────

bool Client::lookup_name(const std::string& account_id, const std::string& name) {
    return libjami::lookupName(account_id, "", name);
}

bool Client::register_name(const std::string& account_id,
                            const std::string& password,
                            const std::string& name) {
    return libjami::registerName(account_id, name, password);
}

} // namespace jami
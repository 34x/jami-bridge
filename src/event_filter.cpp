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

/// @file event_filter.cpp
/// @brief Account-based event filtering.

#include "event_filter.h"

namespace jami {

void filter_events_by_account(Events& events, const std::string& filter_account) {
    // Wrap each callback to only fire when the event's account matches.
    // Uses std::move to transfer the old callback into the new lambda,
    // avoiding dangling references.

    auto orig_msg_cb = std::move(events.on_message_received);
    events.on_message_received = [filter_account, orig_msg_cb](const Message& msg) {
        if (msg.account_id == filter_account && orig_msg_cb) orig_msg_cb(msg);
    };

    auto orig_conv_ready_cb = std::move(events.on_conversation_ready);
    events.on_conversation_ready = [filter_account, orig_conv_ready_cb](
            const std::string& account_id, const std::string& conv_id) {
        if (account_id == filter_account && orig_conv_ready_cb) orig_conv_ready_cb(account_id, conv_id);
    };

    auto orig_member_cb = std::move(events.on_conversation_member_event);
    events.on_conversation_member_event = [filter_account, orig_member_cb](
            const std::string& account_id, const std::string& conv_id,
            const std::string& member_uri, int event) {
        if (account_id == filter_account && orig_member_cb) orig_member_cb(account_id, conv_id, member_uri, event);
    };

    auto orig_conv_req_cb = std::move(events.on_conversation_request_received);
    events.on_conversation_request_received = [filter_account, orig_conv_req_cb](
            const std::string& account_id, const std::string& conv_id,
            const std::map<std::string, std::string>& metadatas) {
        if (account_id == filter_account && orig_conv_req_cb) orig_conv_req_cb(account_id, conv_id, metadatas);
    };

    auto orig_trust_cb = std::move(events.on_trust_request_received);
    events.on_trust_request_received = [filter_account, orig_trust_cb](
            const std::string& account_id, const std::string& from_uri,
            const std::string& conv_id) {
        if (account_id == filter_account && orig_trust_cb) orig_trust_cb(account_id, from_uri, conv_id);
    };

    auto orig_reg_cb = std::move(events.on_registration_changed);
    events.on_registration_changed = [filter_account, orig_reg_cb](
            const std::string& account_id, const std::string& state,
            int code, const std::string& detail) {
        if (account_id == filter_account && orig_reg_cb) orig_reg_cb(account_id, state, code, detail);
    };

    auto orig_status_cb = std::move(events.on_message_status_changed);
    events.on_message_status_changed = [filter_account, orig_status_cb](
            const std::string& account_id, const std::string& conversation_id,
            const std::string& peer, const std::string& message_id, int state) {
        if (account_id == filter_account && orig_status_cb) orig_status_cb(account_id, conversation_id, peer, message_id, state);
    };

    auto orig_name_reg_cb = std::move(events.on_name_registration_ended);
    events.on_name_registration_ended = [filter_account, orig_name_reg_cb](
            const std::string& account_id, int state, const std::string& name) {
        if (account_id == filter_account && orig_name_reg_cb) orig_name_reg_cb(account_id, state, name);
    };

    auto orig_loaded_cb = std::move(events.on_messages_loaded);
    events.on_messages_loaded = [filter_account, orig_loaded_cb](
            uint32_t req_id, const std::string& account_id,
            const std::string& conv_id,
            const std::vector<libjami::SwarmMessage>& messages) {
        if (account_id == filter_account && orig_loaded_cb) orig_loaded_cb(req_id, account_id, conv_id, messages);
    };

    auto orig_transfer_cb = std::move(events.on_data_transfer_event);
    events.on_data_transfer_event = [filter_account, orig_transfer_cb](
            const FileTransfer& transfer) {
        if (transfer.account_id == filter_account && orig_transfer_cb) orig_transfer_cb(transfer);
    };
}

} // namespace jami
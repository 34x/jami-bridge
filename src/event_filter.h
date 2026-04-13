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
/// @file event_filter.h
/// @brief Event filtering utilities — wraps Events callbacks to filter by account.

#include "client.h"

namespace jami {

/// Apply a per-account filter to all event callbacks in `events`.
/// When the bridge serves a single account (resolved from --account),
/// we only emit events for that account. This prevents cross-account
/// event leakage when the daemon has multiple accounts loaded.
///
/// Each callback is wrapped: if the event's account_id matches
/// `filter_account`, the original callback is called; otherwise the
/// event is silently dropped.
///
/// After calling this, call client.update_callbacks(events) to apply.
void filter_events_by_account(Events& events, const std::string& filter_account);

} // namespace jami
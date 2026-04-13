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
/// @file util.h
/// @brief Shared utilities for jami-bridge.

#include <nlohmann/json.hpp>
#include <map>
#include <string>

namespace jami {

/// Convert a map<string,string> to a JSON object.
inline nlohmann::json map_to_json(const std::map<std::string, std::string>& m) {
    nlohmann::json j;
    for (const auto& [k, v] : m) {
        j[k] = v;
    }
    return j;
}

/// Project version — single source of truth.
inline constexpr const char* VERSION = "0.2.0";

} // namespace jami
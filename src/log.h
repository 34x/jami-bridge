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

/// @file log.h
/// @brief Simple timestamped logging for jami-bridge.

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace jami {

/// Returns current time as "HH:MM:SS.mmm" (milliseconds).
inline std::string ts() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

/// Log a message to stderr with timestamp and [jami-bridge] prefix.
/// Usage: jami::log("something happened: ", value);
template <typename... Args>
void log(Args&&... args) {
    std::cerr << ts() << " [jami-bridge] ";
    (std::cerr << ... << args) << std::endl;
}

/// Log with a custom tag (e.g. "hook", "stdio").
/// Usage: jami::log_tag("hook", "timed out after ", 30, "s");
template <typename... Args>
void log_tag(const std::string& tag, Args&&... args) {
    std::cerr << ts() << " [jami-bridge:" << tag << "] ";
    (std::cerr << ... << args) << std::endl;
}

} // namespace jami
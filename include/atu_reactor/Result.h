/*
 * Copyright (C) 2026 Alfredo Tupone
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// System headers
#include <system_error>
#include <variant>
#include <stdexcept>

namespace atu_reactor {

/**
 * @brief A lightweight wrapper to hold either a value or an error code.
 * Replaces exceptions for expected failures in the hot path.
 */
template <typename T>
    class Result {
        public:
            // Success constructor
            Result(T value) : m_data(std::move(value)) {}

            // Error constructor
            Result(std::error_code ec) : m_data(ec) {}

            bool has_value() const { return std::holds_alternative<T>(m_data); }
            explicit operator bool() const { return has_value(); }

            const T& value() const {
                if (!has_value())
                    throw std::get<std::error_code>(m_data).default_error_condition().message();
                return std::get<T>(m_data);
            }

            std::error_code error() const {
                return std::get<std::error_code>(m_data);
            }

        private:
            std::variant<T, std::error_code> m_data;
    };

} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4

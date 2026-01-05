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
 * @brief Exception thrown when accessing a Result that contains an error.
 */
class BadResultAccess : public std::runtime_error {
    public:
        explicit BadResultAccess(std::error_code ec)
            : std::runtime_error("Bad Result access: " + ec.message()), m_ec(ec) {}

        std::error_code error() const noexcept { return m_ec; }
    private:
        std::error_code m_ec;
};

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

        bool has_value() const noexcept { return std::holds_alternative<T>(m_data); }
        explicit operator bool() const noexcept { return has_value(); }

        const T& value() const {
            if (!has_value()) {
                throw BadResultAccess(std::get<std::error_code>(m_data));
            }
            return std::get<T>(m_data);
        }

        T& value() {
            if (!has_value()) {
                throw BadResultAccess(std::get<std::error_code>(m_data));
            }
            return std::get<T>(m_data);
        }

        // Error Access
        std::error_code error() const noexcept {
            if (has_value()) return {};
            return std::get<std::error_code>(m_data);
        }

        // Functional style: value_or
        T value_or(T&& default_value) {
            return has_value() ? std::get<T>(m_data) : std::forward<T>(default_value);
        }

    private:
        std::variant<T, std::error_code> m_data;
};

/**
 * @brief Specialization for void results (Success/Failure only).
 */
template <>
class Result<void> {
    public:
        // Success constructor (default)
        Result() : m_ec({}) {}

        // Error constructor
        Result(std::error_code ec) : m_ec(ec) {}

        // Static helpers for clarity
        static Result success() { return Result(); }
        static Result fail(std::error_code ec) { return Result(ec); }

        bool has_value() const noexcept { return !m_ec; }
        explicit operator bool() const noexcept { return has_value(); }

        // value() for void just checks for errors and throws if present
        void value() const {
            if (m_ec) throw BadResultAccess(m_ec);
        }

        std::error_code error() const noexcept { return m_ec; }

    private:
        std::error_code m_ec;
};

} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4

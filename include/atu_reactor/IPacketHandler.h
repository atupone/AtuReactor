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
#include <cstddef>
#include <cstdint>

namespace atu_reactor {

/**
 * @class IPacketHandler
 * @brief An interface for classes that handle incoming data packets.
 *
 * This abstract base class defines a single pure virtual method,
 * `handlePacket`, which must be implemented by any concrete class that
 * processes data packets. It ensures a consistent handling mechanism.
 */
class IPacketHandler {
public:
    /**
     * @brief Virtual destructor to ensure proper cleanup of derived
     * classes.
     */
    virtual ~IPacketHandler() = default;

    /**
     * @brief Handles a received data packet.
     *
     * Processes the raw data buffer of a packet. Derived classes are
     * responsible for implementing the specific logic for their
     * respective packet types.
     *
     * @param data A pointer to the raw packet data.
     * @param size The size of the packet data in bytes.
     */
    virtual void handlePacket(const uint8_t data[], size_t size) = 0;
};

} // namespace atu_reactor

// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4

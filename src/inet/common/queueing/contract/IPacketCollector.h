//
// Copyright (C) OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see http://www.gnu.org/licenses/.
//

#ifndef __INET_IPACKETCOLLECTOR_H
#define __INET_IPACKETCOLLECTOR_H

#include "inet/common/queueing/contract/IPacketProvider.h"

namespace inet {
namespace queueing {

/**
 * This class defines the interface for packet collectors.
 */
class INET_API IPacketCollector
{
  public:
    virtual ~IPacketCollector() {}

    /**
     * Returns the provider from where packets are collected. The gate must not be nullptr.
     */
    virtual IPacketProvider *getProvider(cGate *gate) = 0;

    /**
     * Notifies about a state change that allows to pop some packet from the
     * provider at the given gate. The gate is never nulltr.
     */
    virtual void handleCanPopPacket(cGate *gate) = 0;
};

} // namespace queueing
} // namespace inet

#endif // ifndef __INET_IPACKETCOLLECTOR_H


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

#include "inet/common/queueing/PacketFilter.h"

namespace inet {
namespace queueing {

Define_Module(PacketFilter);

void PacketFilter::initialize(int stage)
{
    PacketFilterBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        const char *filterClass = par("filterClass");
        packetFilterFunction = check_and_cast<IPacketFilterFunction *>(createOne(filterClass));
    }
}

bool PacketFilter::matchesPacket(Packet *packet)
{
    return packetFilterFunction->matchesPacket(packet);
}

} // namespace queueing
} // namespace inet


//
// Copyright (C) 2011 CoCo Communications
// Copyright (C) 2012 Opensim Ltd
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
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "IGMP.h"
#include "RoutingTableAccess.h"
#include "InterfaceTableAccess.h"
#include "IPv4ControlInfo.h"
#include "IPv4InterfaceData.h"
#include "NotificationBoard.h"

#include <algorithm>

Define_Module(IGMP);

// RFC 2236, Section 6: Host State Diagram
//                           ________________
//                          |                |
//                          |                |
//                          |                |
//                          |                |
//                --------->|   Non-Member   |<---------
//               |          |                |          |
//               |          |                |          |
//               |          |                |          |
//               |          |________________|          |
//               |                   |                  |
//               | leave group       | join group       | leave group
//               | (stop timer,      |(send report,     | (send leave
//               |  send leave if    | set flag,        |  if flag set)
//               |  flag set)        | start timer)     |
//       ________|________           |          ________|________
//      |                 |<---------          |                 |
//      |                 |                    |                 |
//      |                 |<-------------------|                 |
//      |                 |   query received   |                 |
//      | Delaying Member |    (start timer)   |   Idle Member   |
// ---->|                 |------------------->|                 |
//|     |                 |   report received  |                 |
//|     |                 |    (stop timer,    |                 |
//|     |                 |     clear flag)    |                 |
//|     |_________________|------------------->|_________________|
//| query received    |        timer expired
//| (reset timer if   |        (send report,
//|  Max Resp Time    |         set flag)
//|  < current timer) |
// -------------------
//
// RFC 2236, Section 7: Router State Diagram
//                                     --------------------------------
//                             _______|________  gen. query timer      |
// ---------                  |                |        expired        |
//| Initial |---------------->|                | (send general query,  |
// ---------  (send gen. q.,  |                |  set gen. q. timer)   |
//       set initial gen. q.  |                |<----------------------
//             timer)         |    Querier     |
//                            |                |
//                       -----|                |<---
//                      |     |                |    |
//                      |     |________________|    |
//query received from a |                           | other querier
//router with a lower   |                           | present timer
//IP address            |                           | expired
//(set other querier    |      ________________     | (send general
// present timer)       |     |                |    |  query,set gen.
//                      |     |                |    |  q. timer)
//                      |     |                |    |
//                       ---->|      Non       |----
//                            |    Querier     |
//                            |                |
//                            |                |
//                       ---->|                |----
//                      |     |________________|    |
//                      | query received from a     |
//                      | router with a lower IP    |
//                      | address                   |
//                      | (set other querier        |
//                      |  present timer)           |
//                       ---------------------------
//
//                              ________________
// ----------------------------|                |<-----------------------
//|                            |                |timer expired           |
//|               timer expired|                |(notify routing -,      |
//|          (notify routing -)|   No Members   |clear rxmt tmr)         |
//|                    ------->|    Present     |<-------                |
//|                   |        |                |       |                |
//|v1 report rec'd    |        |                |       |  ------------  |
//|(notify routing +, |        |________________|       | | rexmt timer| |
//| start timer,      |                    |            | |  expired   | |
//| start v1 host     |  v2 report received|            | | (send g-s  | |
//|  timer)           |  (notify routing +,|            | |  query,    | |
//|                   |        start timer)|            | |  st rxmt   | |
//|         __________|______              |       _____|_|______  tmr)| |
//|        |                 |<------------       |              |     | |
//|        |                 |                    |              |<----- |
//|        |                 | v2 report received |              |       |
//|        |                 | (start timer)      |              |       |
//|        | Members Present |<-------------------|    Checking  |       |
//|  ----->|                 | leave received     |   Membership |       |
//| |      |                 | (start timer*,     |              |       |
//| |      |                 |  start rexmt timer,|              |       |
//| |      |                 |  send g-s query)   |              |       |
//| |  --->|                 |------------------->|              |       |
//| | |    |_________________|                    |______________|       |
//| | |v2 report rec'd |  |                          |                   |
//| | |(start timer)   |  |v1 report rec'd           |v1 report rec'd    |
//| |  ----------------   |(start timer,             |(start timer,      |
//| |v1 host              | start v1 host timer)     | start v1 host     |
//| |tmr    ______________V__                        | timer)            |
//| |exp'd |                 |<----------------------                    |
//|  ------|                 |                                           |
//|        |    Version 1    |timer expired                              |
//|        | Members Present |(notify routing -)                         |
// ------->|                 |-------------------------------------------
//         |                 |<--------------------
// ------->|_________________| v1 report rec'd     |
//| v2 report rec'd |   |   (start timer,          |
//| (start timer)   |   |    start v1 host timer)  |
// -----------------     --------------------------

IGMP::HostGroupData::HostGroupData(IGMP *owner, const IPv4Address &group)
    : owner(owner), groupAddr(group)
{
    ASSERT(owner);
    ASSERT(groupAddr.isMulticast());

    state = IGMP_HGS_NON_MEMBER;
    flag = false;
    timer = NULL;
}

IGMP::HostGroupData::~HostGroupData()
{
    if (timer)
    {
        delete (IGMPHostTimerContext*)timer->getContextPointer();
        owner->cancelAndDelete(timer);
    }
}

IGMP::RouterGroupData::RouterGroupData(IGMP *owner, const IPv4Address &group)
    : owner(owner), groupAddr(group)
{
    ASSERT(owner);
    ASSERT(groupAddr.isMulticast());

    state = IGMP_RGS_NO_MEMBERS_PRESENT;
    timer = NULL;
    rexmtTimer = NULL;
    // v1HostTimer = NULL;
}

IGMP::RouterGroupData::~RouterGroupData()
{
    if (timer)
    {
        delete (IGMPRouterTimerContext*)timer->getContextPointer();
        owner->cancelAndDelete(timer);
    }
    if (rexmtTimer)
    {
        delete (IGMPRouterTimerContext*)rexmtTimer->getContextPointer();
        owner->cancelAndDelete(rexmtTimer);
    }
//    if (v1HostTimer)
//    {
//        delete (IGMPRouterTimerContext*)v1HostTimer->getContextPointer();
//        owner->cancelAndDelete(v1HostTimer);
//    }
}

IGMP::HostInterfaceData::HostInterfaceData(IGMP *owner)
    : owner(owner)
{
    ASSERT(owner);
}

IGMP::HostInterfaceData::~HostInterfaceData()
{
    for (GroupToHostDataMap::iterator it = groups.begin(); it != groups.end(); ++it)
        delete it->second;
}

IGMP::RouterInterfaceData::RouterInterfaceData(IGMP *owner)
    : owner(owner)
{
    ASSERT(owner);

    igmpRouterState = IGMP_RS_INITIAL;
    igmpQueryTimer = NULL;
}

IGMP::RouterInterfaceData::~RouterInterfaceData()
{
    owner->cancelAndDelete(igmpQueryTimer);

    for (GroupToRouterDataMap::iterator it = groups.begin(); it != groups.end(); ++it)
        delete it->second;
}

IGMP::HostGroupData *IGMP::createHostGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    HostInterfaceData *interfaceData = getHostInterfaceData(ie);
    ASSERT(interfaceData->groups.find(group) == interfaceData->groups.end());
    HostGroupData *data = new HostGroupData(this, group);
    interfaceData->groups[group] = data;
    return data;
}

IGMP::RouterGroupData *IGMP::createRouterGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);
    ASSERT(interfaceData->groups.find(group) == interfaceData->groups.end());
    RouterGroupData *data = new RouterGroupData(this, group);
    interfaceData->groups[group] = data;
    return data;
}

IGMP::HostInterfaceData *IGMP::getHostInterfaceData(InterfaceEntry *ie)
{
    int interfaceId = ie->getInterfaceId();
    InterfaceToHostDataMap::iterator it = hostData.find(interfaceId);
    if (it != hostData.end())
        return it->second;

    // create one
    HostInterfaceData *data = new HostInterfaceData(this);
    hostData[interfaceId] = data;
    return data;
}

IGMP::RouterInterfaceData *IGMP::getRouterInterfaceData(InterfaceEntry *ie)
{
    int interfaceId = ie->getInterfaceId();
    InterfaceToRouterDataMap::iterator it = routerData.find(interfaceId);
    if (it != routerData.end())
        return it->second;

    // create one
    RouterInterfaceData *data = new RouterInterfaceData(this);
    routerData[interfaceId] = data;
    return data;
}

IGMP::HostGroupData *IGMP::getHostGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    HostInterfaceData *interfaceData = getHostInterfaceData(ie);
    GroupToHostDataMap::iterator it = interfaceData->groups.find(group);
    return it != interfaceData->groups.end() ? it->second : NULL;
}

IGMP::RouterGroupData *IGMP::getRouterGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);
    GroupToRouterDataMap::iterator it = interfaceData->groups.find(group);
    return it != interfaceData->groups.end() ? it->second : NULL;
}

void IGMP::deleteHostInterfaceData(int interfaceId)
{
    InterfaceToHostDataMap::iterator interfaceIt = hostData.find(interfaceId);
    if (interfaceIt != hostData.end())
    {
        HostInterfaceData *interface = interfaceIt->second;
        hostData.erase(interfaceIt);
        delete interface;
    }
}

void IGMP::deleteRouterInterfaceData(int interfaceId)
{
    InterfaceToRouterDataMap::iterator interfaceIt = routerData.find(interfaceId);
    if (interfaceIt != routerData.end())
    {
        RouterInterfaceData *interface = interfaceIt->second;
        routerData.erase(interfaceIt);
        delete interface;
    }
}

void IGMP::deleteHostGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    HostInterfaceData *interfaceData = getHostInterfaceData(ie);
    GroupToHostDataMap::iterator it = interfaceData->groups.find(group);
    if (it != interfaceData->groups.end())
    {
        HostGroupData *data = it->second;
        interfaceData->groups.erase(it);
        delete data;
    }
}

void IGMP::deleteRouterGroupData(InterfaceEntry *ie, const IPv4Address &group)
{
    RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);
    GroupToRouterDataMap::iterator it = interfaceData->groups.find(group);
    if (it != interfaceData->groups.end())
    {
        RouterGroupData *data = it->second;
        interfaceData->groups.erase(it);
        delete data;
    }
}

void IGMP::initialize(int stage)
{
    if (stage == 0)
    {
        ift = InterfaceTableAccess().get();
        rt = RoutingTableAccess().get();
        nb = NotificationBoardAccess().get();

        nb->subscribe(this, NF_INTERFACE_DELETED);
        nb->subscribe(this, NF_IPv4_MCAST_JOIN);
        nb->subscribe(this, NF_IPv4_MCAST_LEAVE);

        enabled = par("enabled");
        externalRouter = gate("routerIn")->isPathOK() && gate("routerOut")->isPathOK();
        robustness = par("robustnessVariable");
        queryInterval = par("queryInterval");
        queryResponseInterval = par("queryResponseInterval");
        groupMembershipInterval = par("groupMembershipInterval");
        otherQuerierPresentInterval = par("otherQuerierPresentInterval");
        startupQueryInterval = par("startupQueryInterval");
        startupQueryCount = par("startupQueryCount");
        lastMemberQueryInterval = par("lastMemberQueryInterval");
        lastMemberQueryCount = par("lastMemberQueryCount");
        unsolicitedReportInterval = par("unsolicitedReportInterval");
        //version1RouterPresentInterval = par("version1RouterPresentInterval");

        numGroups = 0;
        numHostGroups = 0;
        numRouterGroups = 0;

        numQueriesSent = 0;
        numQueriesRecv = 0;
        numGeneralQueriesSent = 0;
        numGeneralQueriesRecv = 0;
        numGroupSpecificQueriesSent = 0;
        numGroupSpecificQueriesRecv = 0;
        numReportsSent = 0;
        numReportsRecv = 0;
        numLeavesSent = 0;
        numLeavesRecv = 0;

        WATCH(numGroups);
        WATCH(numHostGroups);
        WATCH(numRouterGroups);

        WATCH(numQueriesSent);
        WATCH(numQueriesRecv);
        WATCH(numGeneralQueriesSent);
        WATCH(numGeneralQueriesRecv);
        WATCH(numGroupSpecificQueriesSent);
        WATCH(numGroupSpecificQueriesRecv);
        WATCH(numReportsSent);
        WATCH(numReportsRecv);
        WATCH(numLeavesSent);
        WATCH(numLeavesRecv);
    }
    else if (stage == 1)
    {
        for (int i = 0; i < (int)ift->getNumInterfaces(); ++i)
            configureInterface(ift->getInterface(i));
        nb->subscribe(this, NF_INTERFACE_CREATED);
    }
}

IGMP::~IGMP()
{
    while (!hostData.empty())
        deleteHostInterfaceData(hostData.begin()->first);
    while (!routerData.empty())
        deleteRouterInterfaceData(routerData.begin()->first);
}

void IGMP::receiveChangeNotification(int category, const cPolymorphic *details)
{
    Enter_Method_Silent();

    InterfaceEntry *ie;
    int interfaceId;
    IPv4MulticastGroupInfo *info;
    switch (category)
    {
        case NF_INTERFACE_CREATED:
            ie = check_and_cast<InterfaceEntry*>(details);
            configureInterface(ie);
            break;
        case NF_INTERFACE_DELETED:
            ie = check_and_cast<InterfaceEntry*>(details);
            interfaceId = ie->getInterfaceId();
            deleteHostInterfaceData(interfaceId);
            deleteRouterInterfaceData(interfaceId);
            break;
        case NF_IPv4_MCAST_JOIN:
            info = check_and_cast<IPv4MulticastGroupInfo*>(details);
            multicastGroupJoined(info->ie, info->groupAddress);
            break;
        case NF_IPv4_MCAST_LEAVE:
            info = check_and_cast<IPv4MulticastGroupInfo*>(details);
            multicastGroupLeft(info->ie, info->groupAddress);
            break;
    }
}

void IGMP::configureInterface(InterfaceEntry *ie)
{
	/// joining to 224.0.0.1 and 224.0.0.2 is done in RoutingTable
	if (!ie->isLoopback()) {
	    if (rt->isMulticastForwardingEnabled()) {
            if (enabled) {
                // start querier on this interface
                cMessage *timer = new cMessage("IGMP query timer", IGMP_QUERY_TIMER);
                timer->setContextPointer(ie);
                RouterInterfaceData *routerData = getRouterInterfaceData(ie);
                routerData->igmpQueryTimer = timer;
                routerData->igmpRouterState = IGMP_RS_QUERIER;
                sendQuery(ie, IPv4Address(), queryResponseInterval); // general query
                startTimer(timer, startupQueryInterval);
            }
		}
	}
}

void IGMP::handleMessage(cMessage *msg)
{
	if (!enabled) {
		EV << "IGMP disabled, dropping packet.\n";
		delete msg;
		return;
	}

	if (msg->isSelfMessage()) {
	    switch (msg->getKind())
	    {
	        case IGMP_QUERY_TIMER:
	            processQueryTimer(msg);
	            break;
	        case IGMP_HOSTGROUP_TIMER:
	            processHostGroupTimer(msg);
	            break;
	        case IGMP_LEAVE_TIMER:
	            processLeaveTimer(msg);
	            break;
	        case IGMP_REXMT_TIMER:
	            processRexmtTimer(msg);
	            break;
	        default:
	            ASSERT(false);
	            break;
	    }
	}
	else if (!strcmp(msg->getArrivalGate()->getName(), "routerIn")) {
		send(msg, "ipOut");
	}
	else if (dynamic_cast<IGMPMessage *>(msg)) {
		processIgmpMessage((IGMPMessage *)msg);
	}
	else {
		ASSERT(false);
	}
}

void IGMP::multicastGroupJoined(InterfaceEntry *ie, const IPv4Address& groupAddr)
{
    ASSERT(ie);

	HostGroupData *groupData = createHostGroupData(ie, groupAddr);

    numGroups++;
	numHostGroups++;

	if (enabled && 
		groupAddr != IPv4Address::ALL_ROUTERS_MCAST && 
		groupAddr != IPv4Address::ALL_HOSTS_MCAST)
	{
		sendReport(ie, groupData);
		groupData->flag = true;
		startHostTimer(ie, groupData, unsolicitedReportInterval);
		groupData->state = IGMP_HGS_DELAYING_MEMBER;
	}
}

void IGMP::multicastGroupLeft(InterfaceEntry *ie, const IPv4Address& groupAddr)
{
    ASSERT(ie);

	HostGroupData *groupData = getHostGroupData(ie, groupAddr);
	if (groupData) {
        numHostGroups--;
        if (enabled)
        {
            if (groupData->state == IGMP_HGS_DELAYING_MEMBER)
                cancelEvent(groupData->timer);

            if (groupData->flag)
                sendLeave(ie, groupData);
        }

        deleteHostGroupData(ie, groupAddr);
        numGroups--;
	}
}

void IGMP::startTimer(cMessage *timer, double interval)
{
	ASSERT(timer);
	cancelEvent(timer);
	scheduleAt(simTime() + interval, timer);
}

void IGMP::startHostTimer(InterfaceEntry *ie, HostGroupData* group, double maxRespTime)
{
	if (!group->timer) {
		group->timer = new cMessage("IGMP group timer", IGMP_HOSTGROUP_TIMER);
		group->timer->setContextPointer(new IGMPHostTimerContext(ie, group));
	}

	double delay = uniform(0.0, maxRespTime);
	EV << "setting host timer for " << ie->getName() << " and group " << group->groupAddr.str() << " to " << delay << "\n";
	startTimer(group->timer, delay);
}

void IGMP::sendQuery(InterfaceEntry *ie, const IPv4Address& groupAddr, double maxRespTime)
{
	ASSERT(groupAddr != IPv4Address::ALL_HOSTS_MCAST);
	ASSERT(groupAddr != IPv4Address::ALL_ROUTERS_MCAST);

	RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);

	if (interfaceData->igmpRouterState == IGMP_RS_QUERIER) {
		IGMPMessage *msg = new IGMPMessage("IGMP query");
		msg->setType(IGMP_MEMBERSHIP_QUERY);
		msg->setGroupAddress(groupAddr);
		msg->setMaxRespTime((int)(maxRespTime * 10.0));
		msg->setByteLength(8);
		sendToIP(msg, ie, groupAddr.isUnspecified() ? IPv4Address::ALL_HOSTS_MCAST : groupAddr);

		numQueriesSent++;
		if (groupAddr.isUnspecified())
			numGeneralQueriesSent++;
		else
			numGroupSpecificQueriesSent++;
	}
}

void IGMP::sendReport(InterfaceEntry *ie, HostGroupData* group)
{
	ASSERT(group->groupAddr != IPv4Address::ALL_HOSTS_MCAST);
	ASSERT(group->groupAddr != IPv4Address::ALL_ROUTERS_MCAST);

	if (!ie->isLoopback())
	{
		IGMPMessage *msg = new IGMPMessage("IGMP report");
		msg->setType(IGMPV2_MEMBERSHIP_REPORT);
		msg->setGroupAddress(group->groupAddr);
		msg->setByteLength(8);
		sendToIP(msg, ie, group->groupAddr);
		numReportsSent++;
	}
}

void IGMP::sendLeave(InterfaceEntry *ie, HostGroupData* group)
{
	ASSERT(group->groupAddr != IPv4Address::ALL_HOSTS_MCAST);
	ASSERT(group->groupAddr != IPv4Address::ALL_ROUTERS_MCAST);

	if (!ie->isLoopback())
	{
		IGMPMessage *msg = new IGMPMessage("IGMP leave");
		msg->setType(IGMPV2_LEAVE_GROUP);
		msg->setGroupAddress(group->groupAddr);
		msg->setByteLength(8);
		sendToIP(msg, ie, IPv4Address::ALL_ROUTERS_MCAST);
		numLeavesSent++;
	}
}

void IGMP::sendToIP(IGMPMessage *msg, InterfaceEntry *ie, const IPv4Address& dest)
{
	ASSERT(!ie->isLoopback());

	IPv4ControlInfo *controlInfo = new IPv4ControlInfo();
	controlInfo->setProtocol(IP_PROT_IGMP);
	controlInfo->setInterfaceId(ie->getInterfaceId());
	controlInfo->setTimeToLive(1);
	controlInfo->setDestAddr(dest);
	msg->setControlInfo(controlInfo);

	send(msg, "ipOut");
}

void IGMP::processIgmpMessage(IGMPMessage *msg)
{
	IPv4ControlInfo *controlInfo = (IPv4ControlInfo *)msg->getControlInfo();
	InterfaceEntry *ie = ift->getInterfaceById(controlInfo->getInterfaceId());

	switch (msg->getType()) {
	case IGMP_MEMBERSHIP_QUERY:
		processQuery(ie, controlInfo->getSrcAddr(), msg);
		break;
	//case IGMPV1_MEMBERSHIP_REPORT:
	//	processV1Report(ie, msg);
	//	delete msg;
	//	break;
	case IGMPV2_MEMBERSHIP_REPORT:
		processV2Report(ie, msg);
		break;
	case IGMPV2_LEAVE_GROUP:
		processLeave(ie, msg);
		break;
	default:
		if (externalRouter)
			send(msg, "routerOut");
		else
		{
			delete msg;
			throw cRuntimeError("IGMP: Unhandled message type (%dq)", msg->getType());
		}
		break;
	}
}

void IGMP::processQueryTimer(cMessage *msg)
{
    InterfaceEntry *ie = (InterfaceEntry*)msg->getContextPointer();
    ASSERT(ie);
    RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);
    IGMPRouterState state = interfaceData->igmpRouterState;
	if (state == IGMP_RS_QUERIER || state == IGMP_RS_NON_QUERIER) {
	    interfaceData->igmpRouterState = IGMP_RS_QUERIER;
		sendQuery(ie, IPv4Address(), queryResponseInterval); // general query
		startTimer(msg, queryInterval);
	}
}

void IGMP::processHostGroupTimer(cMessage *msg)
{
    IGMPHostTimerContext *ctx = (IGMPHostTimerContext*)msg->getContextPointer();
    sendReport(ctx->ie, ctx->hostGroup);
    ctx->hostGroup->flag = true;
    ctx->hostGroup->state = IGMP_HGS_IDLE_MEMBER;
}

void IGMP::processLeaveTimer(cMessage *msg)
{
    IGMPRouterTimerContext *ctx = (IGMPRouterTimerContext*)msg->getContextPointer();

    // notify IPv4InterfaceData to update its listener list
    ctx->ie->ipv4Data()->removeMulticastListener(ctx->routerGroup->groupAddr);

    IPv4MulticastGroupInfo info(ctx->ie, ctx->routerGroup->groupAddr);
    nb->fireChangeNotification(NF_IPv4_MCAST_UNREGISTERED, &info);
    numRouterGroups--;

    if (ctx->routerGroup->state ==  IGMP_RGS_CHECKING_MEMBERSHIP) {
        cancelEvent(ctx->routerGroup->rexmtTimer);
    }
    ctx->routerGroup->state = IGMP_RGS_NO_MEMBERS_PRESENT;
    deleteRouterGroupData(ctx->ie, ctx->routerGroup->groupAddr);
    numGroups--;
}

void IGMP::processRexmtTimer(cMessage *msg)
{
    IGMPRouterTimerContext *ctx = (IGMPRouterTimerContext*)msg->getContextPointer();
    sendQuery(ctx->ie, ctx->routerGroup->groupAddr, lastMemberQueryInterval);
    startTimer(ctx->routerGroup->rexmtTimer, lastMemberQueryInterval);
    ctx->routerGroup->state = IGMP_RGS_CHECKING_MEMBERSHIP;
}

void IGMP::processQuery(InterfaceEntry *ie, const IPv4Address& sender, IGMPMessage *msg)
{
    HostInterfaceData *interfaceData = getHostInterfaceData(ie);

	numQueriesRecv++;

	IPv4Address &groupAddr = msg->getGroupAddress();
	if (groupAddr.isUnspecified()) {
		// general query
		numGeneralQueriesRecv++;
		for (GroupToHostDataMap::iterator it = interfaceData->groups.begin(); it != interfaceData->groups.end(); ++it) {
			processGroupQuery(ie, it->second, msg->getMaxRespTime());
		}
	}
	else {
		// group-specific query
		numGroupSpecificQueriesRecv++;
		GroupToHostDataMap::iterator it = interfaceData->groups.find(groupAddr);
		if (it != interfaceData->groups.end()) {
			processGroupQuery(ie, it->second, msg->getMaxRespTime());
		}
	}

	if (rt->isMulticastForwardingEnabled()) {
		if (sender < ie->ipv4Data()->getIPAddress()) {
			RouterInterfaceData *routerInterfaceData = getRouterInterfaceData(ie);
			startTimer(routerInterfaceData->igmpQueryTimer, otherQuerierPresentInterval);
			routerInterfaceData->igmpRouterState = IGMP_RS_NON_QUERIER;
		}
	}

	delete msg;
}

void IGMP::processGroupQuery(InterfaceEntry *ie, HostGroupData* group, int maxRespTime)
{
	double maxRespTimeSecs = (double)maxRespTime / 10.0;

	if (group->state == IGMP_HGS_DELAYING_MEMBER) {
		cMessage *timer = group->timer;
		simtime_t maxAbsoluteRespTime = simTime() + maxRespTimeSecs;
		if (timer->isScheduled() && maxAbsoluteRespTime < timer->getArrivalTime()) {
			startHostTimer(ie, group, maxRespTimeSecs);
		}
	}
	else if (group->state == IGMP_HGS_IDLE_MEMBER) {
		startHostTimer(ie, group, maxRespTimeSecs);
		group->state = IGMP_HGS_DELAYING_MEMBER;
	}
	else {
		// ignored
	}
}

void IGMP::processV2Report(InterfaceEntry *ie, IGMPMessage *msg)
{
	IPv4Address &groupAddr = msg->getGroupAddress();

	numReportsRecv++;

    HostGroupData *hostGroupData = getHostGroupData(ie, groupAddr);
	if (hostGroupData) {
		if (hostGroupData && hostGroupData->state == IGMP_HGS_IDLE_MEMBER) {
			cancelEvent(hostGroupData->timer);
			hostGroupData->flag = false;
			hostGroupData->state = IGMP_HGS_DELAYING_MEMBER;
		}
	}

	if (rt->isMulticastForwardingEnabled()) {
		RouterGroupData* routerGroupData = getRouterGroupData(ie, groupAddr);
		if (!routerGroupData) {
		    routerGroupData = createRouterGroupData(ie, groupAddr);
			numGroups++;
		}

		if (!routerGroupData->timer)
		{
		    routerGroupData->timer = new cMessage("IGMP leave timer", IGMP_LEAVE_TIMER);
		    routerGroupData->timer->setContextPointer(new IGMPRouterTimerContext(ie, routerGroupData));
		}
		if (!routerGroupData->rexmtTimer)
		{
		    routerGroupData->rexmtTimer = new cMessage("IGMP rexmt timer", IGMP_REXMT_TIMER);
            routerGroupData->rexmtTimer->setContextPointer(new IGMPRouterTimerContext(ie, routerGroupData));
		}

		if (routerGroupData->state == IGMP_RGS_NO_MEMBERS_PRESENT) {
		    // notify IPv4InterfaceData to update its listener list
		    ie->ipv4Data()->addMulticastListener(groupAddr);

			IPv4MulticastGroupInfo info(ie, routerGroupData->groupAddr);
			nb->fireChangeNotification(NF_IPv4_MCAST_REGISTERED, &info);
			numRouterGroups++;
		}

		startTimer(routerGroupData->timer, groupMembershipInterval);
		routerGroupData->state = IGMP_RGS_MEMBERS_PRESENT;
	}

	delete msg;
}

void IGMP::processLeave(InterfaceEntry *ie, IGMPMessage *msg)
{
	numLeavesRecv++;

	if (rt->isMulticastForwardingEnabled()) {
		IPv4Address &groupAddr = msg->getGroupAddress();
		RouterInterfaceData *interfaceData = getRouterInterfaceData(ie);
		RouterGroupData *groupData = getRouterGroupData(ie, groupAddr);
		if (groupData) {
			if (groupData->state == IGMP_RGS_MEMBERS_PRESENT) {
				startTimer(groupData->rexmtTimer, lastMemberQueryInterval);
				if (interfaceData->igmpRouterState == IGMP_RS_QUERIER) {
					startTimer(groupData->timer, lastMemberQueryInterval * lastMemberQueryCount);
				}
				else {
					double maxRespTimeSecs = (double)msg->getMaxRespTime() / 10.0;
					sendQuery(ie, groupAddr, maxRespTimeSecs * lastMemberQueryCount);
				}
				startTimer(groupData->rexmtTimer, lastMemberQueryInterval);
				groupData->state = IGMP_RGS_CHECKING_MEMBERSHIP;
			}
		}
	}

	delete msg;
}

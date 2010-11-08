/* Copyright (c) 2009-2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "CoordinatorServer.h"
#include "MasterClient.h"
#include "ProtoBuf.h"
#include "Recovery.h"

namespace RAMCloud {

CoordinatorServer::CoordinatorServer()
    : nextServerId(1)
    , backupList()
    , masterList()
    , tabletMap()
    , tables()
    , nextTableId(0)
    , mockRecovery(NULL)
{
}

CoordinatorServer::~CoordinatorServer()
{
    // delete wills
    foreach (const ProtoBuf::ServerList::Entry& master, masterList.server())
        delete reinterpret_cast<ProtoBuf::Tablets*>(master.user_data());
}

void
CoordinatorServer::run()
{
    while (true)
        handleRpc<CoordinatorServer>();
}

void
CoordinatorServer::dispatch(RpcType type,
                            Transport::ServerRpc& rpc,
                            Responder& responder)
{
    switch (type) {
        case CreateTableRpc::type:
            callHandler<CreateTableRpc, CoordinatorServer,
                        &CoordinatorServer::createTable>(rpc);
            break;
        case DropTableRpc::type:
            callHandler<DropTableRpc, CoordinatorServer,
                        &CoordinatorServer::dropTable>(rpc);
            break;
        case OpenTableRpc::type:
            callHandler<OpenTableRpc, CoordinatorServer,
                        &CoordinatorServer::openTable>(rpc);
            break;
        case EnlistServerRpc::type:
            callHandler<EnlistServerRpc, CoordinatorServer,
                        &CoordinatorServer::enlistServer>(rpc);
            break;
        case GetBackupListRpc::type:
            callHandler<GetBackupListRpc, CoordinatorServer,
                        &CoordinatorServer::getBackupList>(rpc);
            break;
        case GetTabletMapRpc::type:
            callHandler<GetTabletMapRpc, CoordinatorServer,
                        &CoordinatorServer::getTabletMap>(rpc);
            break;
        case HintServerDownRpc::type:
            callHandler<HintServerDownRpc, CoordinatorServer,
                        &CoordinatorServer::hintServerDown>(rpc, responder);
            break;
        case TabletsRecoveredRpc::type:
            callHandler<TabletsRecoveredRpc, CoordinatorServer,
                        &CoordinatorServer::tabletsRecovered>(rpc);
            break;
        case PingRpc::type:
            callHandler<PingRpc, Server, &Server::ping>(rpc);
            break;
        default:
            throw UnimplementedRequestError(HERE);
    }
}

/**
 * Top-level server method to handle the CREATE_TABLE request.
 * \copydetails Server::ping
 */
void
CoordinatorServer::createTable(const CreateTableRpc::Request& reqHdr,
                               CreateTableRpc::Response& respHdr,
                               Transport::ServerRpc& rpc)
{
    if (masterList.server_size() == 0)
        throw RetryException(HERE);

    const char* name = getString(rpc.recvPayload, sizeof(reqHdr),
                                 reqHdr.nameLength);
    if (tables.find(name) != tables.end())
        return;
    uint32_t tableId = nextTableId++;
    tables[name] = tableId;

    ProtoBuf::ServerList_Entry& master(*masterList.mutable_server(0));

    // Create tablet map entry.
    ProtoBuf::Tablets_Tablet& tablet(*tabletMap.add_tablet());
    tablet.set_table_id(tableId);
    tablet.set_start_object_id(0);
    tablet.set_end_object_id(~0UL);
    tablet.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);
    tablet.set_server_id(master.server_id());
    tablet.set_service_locator(master.service_locator());

    // Create will entry. The tablet is empty, so it doesn't matter where it
    // goes or in how many partitions, initially. It just has to go somewhere.
    ProtoBuf::Tablets& will(
        *reinterpret_cast<ProtoBuf::Tablets*>(master.user_data()));
    ProtoBuf::Tablets_Tablet& willEntry(*will.add_tablet());
    willEntry.set_table_id(tableId);
    willEntry.set_start_object_id(0);
    willEntry.set_end_object_id(~0UL);
    willEntry.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);
    uint64_t maxPartitionId;
    if (will.tablet_size() > 1)
        maxPartitionId = will.tablet(will.tablet_size() - 2).user_data();
    else
        maxPartitionId = 0;
    willEntry.set_user_data(maxPartitionId);

    // Inform the master.
    MasterClient masterClient(
        transportManager.getSession(master.service_locator().c_str()));
    // TODO(ongaro): filter tabletMap for those tablets belonging to master
    // before sending
    masterClient.setTablets(tabletMap);

    LOG(NOTICE, "Created table '%s' with id %u", name, tableId);
    LOG(DEBUG, "There are now %d tablets in the map", tabletMap.tablet_size());
}

/**
 * Top-level server method to handle the DROP_TABLE request.
 * \copydetails Server::ping
 */
void
CoordinatorServer::dropTable(const DropTableRpc::Request& reqHdr,
                             DropTableRpc::Response& respHdr,
                             Transport::ServerRpc& rpc)
{
    const char* name = getString(rpc.recvPayload, sizeof(reqHdr),
                                 reqHdr.nameLength);
    Tables::iterator it = tables.find(name);
    if (it == tables.end())
        return;
    uint32_t tableId = it->second;
    tables.erase(it);
    int32_t i = 0;
    while (i < tabletMap.tablet_size()) {
        if (tabletMap.tablet(i).table_id() == tableId) {
            tabletMap.mutable_tablet()->SwapElements(
                                            tabletMap.tablet_size() - 1, i);
            tabletMap.mutable_tablet()->RemoveLast();
        } else {
            ++i;
        }
    }
    // TODO(ongaro): update only affected masters, filter tabletMap for those
    // tablets belonging to each master
    const string& locator(masterList.server(0).service_locator());
    MasterClient master(transportManager.getSession(locator.c_str()));
    master.setTablets(tabletMap);

    LOG(NOTICE, "Dropped table '%s' with id %u", name, tableId);
    LOG(DEBUG, "There are now %d tablets in the map", tabletMap.tablet_size());
}

/**
 * Top-level server method to handle the OPEN_TABLE request.
 * \copydetails Server::ping
 */
void
CoordinatorServer::openTable(const OpenTableRpc::Request& reqHdr,
                             OpenTableRpc::Response& respHdr,
                             Transport::ServerRpc& rpc)
{
    const char* name = getString(rpc.recvPayload, sizeof(reqHdr),
                                 reqHdr.nameLength);
    Tables::iterator it(tables.find(name));
    if (it == tables.end())
        throw TableDoesntExistException(HERE);
    respHdr.tableId = it->second;
}

/**
 * Handle the ENLIST_SERVER RPC.
 * \copydetails Server::ping
 */
void
CoordinatorServer::enlistServer(const EnlistServerRpc::Request& reqHdr,
                                EnlistServerRpc::Response& respHdr,
                                Transport::ServerRpc& rpc)
{
    uint64_t serverId = nextServerId++;
    ProtoBuf::ServerType serverType =
        static_cast<ProtoBuf::ServerType>(reqHdr.serverType);
    ProtoBuf::ServerList& serverList(serverType == ProtoBuf::MASTER
                                     ? masterList
                                     : backupList);
    ProtoBuf::ServerList_Entry& server(*serverList.add_server());
    server.set_server_type(serverType);
    server.set_server_id(serverId);
    server.set_service_locator(getString(rpc.recvPayload, sizeof(reqHdr),
                                         reqHdr.serviceLocatorLength));
    if (server.server_type() == ProtoBuf::MASTER) {
        // create empty will
        server.set_user_data(
            reinterpret_cast<uint64_t>(new ProtoBuf::Tablets));
        LOG(DEBUG, "Master enlisted with id %lu", serverId);
    } else {
        LOG(DEBUG, "Backup enlisted with id %lu", serverId);
    }
    respHdr.serverId = serverId;
}

/**
 * Handle the GET_BACKUP_LIST RPC.
 * \copydetails Server::ping
 */
void
CoordinatorServer::getBackupList(const GetBackupListRpc::Request& reqHdr,
                                 GetBackupListRpc::Response& respHdr,
                                 Transport::ServerRpc& rpc)
{
    respHdr.serverListLength = serializeToResponse(rpc.replyPayload,
                                                   backupList);
}

/**
 * Handle the GET_TABLET_MAP RPC.
 * \copydetails Server::ping
 */
void
CoordinatorServer::getTabletMap(const GetTabletMapRpc::Request& reqHdr,
                                GetTabletMapRpc::Response& respHdr,
                                Transport::ServerRpc& rpc)
{
    respHdr.tabletMapLength = serializeToResponse(rpc.replyPayload,
                                                  tabletMap);
}

/**
 * Handle the ENLIST_SERVER RPC.
 * \copydetails Server::ping
 * \param responder
 *      Functor to respond to the RPC before returning from this method. Used
 *      to avoid deadlock between first master and coordinator.
 */
void
CoordinatorServer::hintServerDown(const HintServerDownRpc::Request& reqHdr,
                                  HintServerDownRpc::Response& respHdr,
                                  Transport::ServerRpc& rpc,
                                  Responder& responder)
{
    string serviceLocator(getString(rpc.recvPayload, sizeof(reqHdr),
                                    reqHdr.serviceLocatorLength));
    responder();

    // reqHdr, respHdr, and rpc are off-limits now

    LOG(DEBUG, "Hint server down: %s", serviceLocator.c_str());

    // is it a master?
    for (int32_t i = 0; i < masterList.server_size(); i++) {
        const ProtoBuf::ServerList::Entry& master(masterList.server(i));
        if (master.service_locator() == serviceLocator) {
            uint64_t serverId = master.server_id();
            std::auto_ptr<ProtoBuf::Tablets> will(
                reinterpret_cast<ProtoBuf::Tablets*>(master.user_data()));

            masterList.mutable_server()->SwapElements(
                                            masterList.server_size() - 1, i);
            masterList.mutable_server()->RemoveLast();

            // master is off-limits now

            foreach (ProtoBuf::Tablets::Tablet& tablet,
                     *tabletMap.mutable_tablet()) {
                if (tablet.server_id() == serverId)
                    tablet.set_state(ProtoBuf::Tablets_Tablet::RECOVERING);
            }

            if (mockRecovery != NULL) {
                (*mockRecovery)(serverId, *will, masterList, backupList);
            } else {
                LOG(DEBUG, "Trying partition recovery on %lu with %u masters "
                    "and %u backups", serverId, masterList.server_size(),
                    backupList.server_size());
                Recovery(serverId, *will, masterList, backupList).start();
                LOG(DEBUG, "OK! Recovered!");
            }
            return;
        }
    }

    // is it a backup?
    for (int32_t i = 0; i < backupList.server_size(); i++) {
        const ProtoBuf::ServerList::Entry& backup(backupList.server(i));
        if (backup.service_locator() == serviceLocator) {
            backupList.mutable_server()->SwapElements(
                                            backupList.server_size() - 1, i);
            backupList.mutable_server()->RemoveLast();

            // backup is off-limits now

            // TODO(ongaro): inform masters they need to replicate more
            return;
        }
    }
}

/**
 * Handle the TABLETS_RECOVERED RPC.
 * \copydetails Server::ping
 */
void
CoordinatorServer::tabletsRecovered(const TabletsRecoveredRpc::Request& reqHdr,
                                    TabletsRecoveredRpc::Response& respHdr,
                                    Transport::ServerRpc& rpc)
{
    ProtoBuf::Tablets tablets;
    ProtoBuf::parseFromResponse(rpc.recvPayload, sizeof(reqHdr),
                                reqHdr.tabletsLength, tablets);
    TEST_LOG("called with %u tablets", tablets.tablet_size());
}

} // namespace RAMCloud
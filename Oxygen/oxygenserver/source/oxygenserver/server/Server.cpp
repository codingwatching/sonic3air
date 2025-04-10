/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2024 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygenserver/pch.h"
#include "oxygenserver/server/Server.h"
#include "oxygenserver/Configuration.h"

#include "oxygen_netcore/network/ConnectionManager.h"
#include "oxygen_netcore/network/LagStopwatch.h"
#include "oxygen_netcore/serverclient/ProtocolVersion.h"

#include "PrivatePackets.h"
#include "Shared.h"

#include <thread>


namespace
{
	void processIP(std::string& ip)
	{
		// Localhost
		if (ip == "::")
		{
			ip = "::1";
		}

		// Return an IPv6-encoded IPv4 address as actual IPv4
		else if (rmx::startsWith(ip, "::ffff:"))
		{
			ip.erase(0, 7);
		}
	}
}


void Server::runServer()
{
	Configuration& config = Configuration::instance();
	const uint16 udpPort = (config.mUDPPort == 0) ? UDP_SERVER_PORT : config.mUDPPort;
	const uint16 tcpPort = (config.mTCPPort == 0) ? TCP_SERVER_PORT : config.mTCPPort;

	// Setup sockets
	UDPSocket udpSocket;
	if (!udpSocket.bindToPort(udpPort, SERVER_PROTOCOL_FAMILY))
		RMX_ERROR("UDP socket bind to port " << udpPort << " failed", return);
	RMX_LOG_INFO("UDP socket bound to port " << udpPort);

	TCPSocket tcpListenSocket;
	if (!tcpListenSocket.setupServer(tcpPort, SERVER_PROTOCOL_FAMILY))
		RMX_ERROR("TCP socket bind to port " << tcpPort << " failed", return);
	RMX_LOG_INFO("TCP socket bound to port " << tcpPort);

	// Setup connection manager
	ConnectionManager connectionManager(&udpSocket, &tcpListenSocket, *this, network::HIGHLEVEL_PROTOCOL_VERSION_RANGE);
#ifdef DEBUG
	setupDebugSettings(connectionManager.mDebugSettings);
#endif
	RMX_LOG_INFO("Ready for connections");

	// Prepare cached data
	{
		// Fill in available features
		mCachedServerFeaturesRequest.mResponse.mFeatures.emplace_back(network::GetServerFeaturesRequest::Response::Feature("app-update-check", 1, 1));
		mCachedServerFeaturesRequest.mResponse.mFeatures.emplace_back(network::GetServerFeaturesRequest::Response::Feature("channel-broadcasting", 1, 1));
		mCachedServerFeaturesRequest.mResponse.mFeatures.emplace_back(network::GetServerFeaturesRequest::Response::Feature("query-external-address", 1, 1));
	}

	// Setup sub-systems
	mVirtualDirectory.startup();

	// Prepare timing
	uint64 lastTimestamp = ConnectionManager::getCurrentTimestamp();
	mLastCleanupTimestamp = lastTimestamp;

	// Run the main loop
	mReceivedCloseEvent = false;
	while (!mReceivedCloseEvent)
	{
		const uint64 currentTimestamp = ConnectionManager::getCurrentTimestamp();
		const uint64 millisecondsElapsed = currentTimestamp - lastTimestamp;
		lastTimestamp = currentTimestamp;

		// Check for new packets
		if (!connectionManager.updateConnectionManager())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		// Perform cleanup regularly
		if (currentTimestamp - mLastCleanupTimestamp > 5000)	// Every 5 seconds
		{
			LAG_STOPWATCH("performCleanup", 2000);
			performCleanup();
			mLastCleanupTimestamp = currentTimestamp;
		}
	}

	connectionManager.terminateAllConnections();
	RMX_LOG_INFO("Server shutdown");
}

NetConnection* Server::createNetConnection(ConnectionManager& connectionManager, const SocketAddress& senderAddress)
{
	while (true)
	{
		const uint32 playerID = (uint32)(rand() & 0xff) + ((uint32)(rand() % 0xff) << 8) + ((uint32)(rand() % 0xff) << 16) + ((uint32)(rand() % 0xff) << 24);
		if (mNetConnectionsByPlayerID.count(playerID) == 0)
		{
			ServerNetConnection& connection = mNetConnectionPool.createObject(playerID);
			mNetConnectionsByPlayerID[playerID] = &connection;
			RMX_LOG_INFO("Created new connection with player ID " << connection.getHexPlayerID() << " (now " << mNetConnectionsByPlayerID.size() << " total connections)");
			return &connection;
		}
	}
	return nullptr;
}

void Server::destroyNetConnection(NetConnection& connection)
{
	ServerNetConnection& serverNetConnection = static_cast<ServerNetConnection&>(connection);
	RMX_LOG_INFO("Removing connection with player ID " << serverNetConnection.getHexPlayerID() << " (now " << (mNetConnectionsByPlayerID.size() - 1) << " total connections)");

	mChannels.removePlayerFromAllChannels(serverNetConnection);
	mNetplaySetup.onDestroyConnection(serverNetConnection);

	mNetConnectionsByPlayerID.erase(serverNetConnection.getPlayerID());
	mNetConnectionPool.destroyObject(serverNetConnection);
}

bool Server::onReceivedConnectionlessPacket(ConnectionlessPacketEvaluation& evaluation)
{
	switch (evaluation.mLowLevelSignature)
	{
		case network::GetExternalAddressConnectionless::SIGNATURE:
		{
			network::GetExternalAddressConnectionless packet;
			if (!packet.serializePacket(evaluation.mSerializer, 1))
				return false;

			if (packet.mPacketVersion == 1)
			{
				network::ReplyExternalAddressConnectionless reply;
				reply.mQueryID = packet.mQueryID;
				reply.mPacketVersion = packet.mPacketVersion;
				reply.mIP = evaluation.mSenderAddress.getIP();
				reply.mPort = evaluation.mSenderAddress.getPort();
				processIP(reply.mIP);
				evaluation.mConnectionManager.sendConnectionlessLowLevelPacket(reply, evaluation.mSenderAddress, 0, 0);
			}
			else
			{
				// Send back an error packet
				lowlevel::ErrorPacket reply;
				reply.mErrorCode = lowlevel::ErrorPacket::ErrorCode::UNSUPPORTED_VERSION;
				evaluation.mConnectionManager.sendConnectionlessLowLevelPacket(reply, evaluation.mSenderAddress, 0, 0);
			}
			return true;
		}
	}

	return false;
}

bool Server::onReceivedPacket(ReceivedPacketEvaluation& evaluation)
{
	// Go through sub-systems
	if (mChannels.onReceivedPacket(evaluation))
		return true;
	if (mNetplaySetup.onReceivedPacket(evaluation))
		return true;

	// Failed
	return false;
}

bool Server::onReceivedRequestQuery(ReceivedQueryEvaluation& evaluation)
{
	LAG_STOPWATCH("## Server::onReceivedRequestQuery", 1000);

	switch (evaluation.mPacketType)
	{
		case network::GetServerFeaturesRequest::Query::PACKET_TYPE:
		{
			// Re-use the already prepared request instance
			network::GetServerFeaturesRequest& request = mCachedServerFeaturesRequest;
			if (!evaluation.readQuery(request))
				return false;

			// Nothing more to change, the response is already filled in
			return evaluation.respond(request);
		}

		case network::GetExternalAddressRequest::Query::PACKET_TYPE:
		{
			using Request = network::GetExternalAddressRequest;
			Request request;
			if (!evaluation.readQuery(request))
				return false;

			// Send back the sender's IP and port as seen from the server
			request.mResponse.mIP = evaluation.mConnection.getRemoteAddress().getIP();
			request.mResponse.mPort = evaluation.mConnection.getRemoteAddress().getPort();
			return evaluation.respond(request);
		}
	}

	// Go through sub-systems
	if (mChannels.onReceivedRequestQuery(evaluation))
		return true;
	if (mNetplaySetup.onReceivedRequestQuery(evaluation))
		return true;
	if (mUpdateCheck.onReceivedRequestQuery(evaluation))
		return true;

	// Failed
	return false;
}

void Server::performCleanup()
{
	// Check for disconnected and empty connection instances
	std::vector<NetConnection*> connectionsToRemove;
	for (auto& pair : mNetConnectionsByPlayerID)
	{
		if (pair.second->getState() == NetConnection::State::DISCONNECTED || pair.second->getState() == NetConnection::State::EMPTY)
		{
			connectionsToRemove.push_back(pair.second);
		}
	}
	for (NetConnection* connection : connectionsToRemove)
	{
		destroyNetConnection(*connection);
	}
}

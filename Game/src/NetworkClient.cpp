#include "NetworkClient.h"

#include <cstdio>

namespace
{
	std::vector<std::uint8_t> ToBytes(const std::string& frame)
	{
		return std::vector<std::uint8_t>(frame.begin(), frame.end());
	}

	std::string ToFrame(const std::vector<std::uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}
}

NetworkClient::NetworkClient(const std::string& serverEndpoint)
	: m_Socket(Engine::SocketRole::Request), m_Connected(false), m_LocalPlayerId(0)
{
	m_Socket.Connect(serverEndpoint);

	m_Socket.Send(ToFrame(Engine::EncodeJoinRequest()));
	std::vector<std::uint8_t> replyBytes = ToBytes(m_Socket.Receive());

	if (Engine::PeekMessageType(replyBytes) != Engine::MessageType::Snapshot)
	{
		return; // server replied with an error, or something unrecognized: stay disconnected
	}

	std::optional<Engine::Snapshot> snapshot = Engine::DecodeSnapshot(replyBytes);
	if (!snapshot.has_value())
	{
		return;
	}

	m_LocalPlayerId = snapshot->RecipientId;
	m_LatestRoster = snapshot->Roster;
	m_Connected = true;
	std::printf("NetworkClient: connected as player %u\n", m_LocalPlayerId);
	std::fflush(stdout);
}

NetworkClient::~NetworkClient()
{
	Disconnect();
}

bool NetworkClient::IsConnected() const
{
	return m_Connected;
}

Engine::PlayerId NetworkClient::GetLocalPlayerId() const
{
	return m_LocalPlayerId;
}

bool NetworkClient::SendState(const Engine::PlayerState& localState)
{
	if (!m_Connected)
	{
		return false;
	}

	Engine::PlayerState stamped = localState;
	stamped.Id = m_LocalPlayerId;

	m_Socket.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ stamped, false })));
	std::vector<std::uint8_t> replyBytes = ToBytes(m_Socket.Receive());

	std::optional<Engine::Snapshot> snapshot = Engine::DecodeSnapshot(replyBytes);
	if (!snapshot.has_value())
	{
		return false;
	}

	m_LatestRoster = snapshot->Roster;
	return true;
}

const std::vector<Engine::PlayerState>& NetworkClient::GetLatestRoster() const
{
	return m_LatestRoster;
}

void NetworkClient::Disconnect()
{
	if (!m_Connected)
	{
		return;
	}

	Engine::PlayerState state{ m_LocalPlayerId, 0.0f, 0.0f, 0.0f, 0.0f };
	m_Socket.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ state, true })));

	// REQ/REP still requires a matching reply before the socket could be reused for
	// anything else; receive and discard it since this client is shutting down.
	m_Socket.Receive();

	std::printf("NetworkClient: disconnected player %u\n", m_LocalPlayerId);
	std::fflush(stdout);
	m_Connected = false;
}

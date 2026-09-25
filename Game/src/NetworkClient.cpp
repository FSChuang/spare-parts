#include "NetworkClient.h"

#include "Engine/Network/Socket.h"

#include <optional>

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

	// Smallest possible "<scheme>://<host>:<port>" rewrite — not a general URI parser.
	// Keeps everything up to and including the final ':' delimiter and replaces only the
	// port itself, so "tcp://127.0.0.1:5556" + 5558 -> "tcp://127.0.0.1:5558" and
	// "tcp://localhost:5556" + 5560 -> "tcp://localhost:5560" (the LAST ':' is always the
	// one separating host from port; an earlier one, e.g. in "tcp://", is never it).
	// Returns std::nullopt for input with no ':' at all, or a ':' with nothing after it —
	// not a shape this can safely rewrite.
	std::optional<std::string> BuildDedicatedEndpoint(const std::string& bootstrapEndpoint, std::uint16_t port)
	{
		std::size_t lastColon = bootstrapEndpoint.find_last_of(':');
		if (lastColon == std::string::npos || lastColon + 1 >= bootstrapEndpoint.size())
		{
			return std::nullopt;
		}

		return bootstrapEndpoint.substr(0, lastColon + 1) + std::to_string(port);
	}
}

NetworkClient::NetworkClient(const std::string& serverEndpoint)
	: m_Worker(&NetworkClient::WorkerMain, this, serverEndpoint)
{
	// Returns immediately: WorkerMain performs the entire bootstrap+dedicated-session
	// handshake on its own thread, not here, so construction never blocks the caller on
	// network I/O.
}

NetworkClient::~NetworkClient()
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_StopRequested = true;
	}
	m_OutgoingCondition.notify_one();

	// The main thread never touches a Socket directly — it only signals the worker and
	// waits for it to finish on its own terms (see WorkerMain's shutdown handling and the
	// KNOWN LIMITATION documented in NetworkClient.h).
	if (m_Worker.joinable())
	{
		m_Worker.join();
	}
}

bool NetworkClient::IsConnected() const
{
	std::lock_guard<std::mutex> lock(m_IncomingMutex);
	return m_Connected;
}

Engine::PlayerId NetworkClient::GetLocalPlayerId() const
{
	std::lock_guard<std::mutex> lock(m_IncomingMutex);
	return m_LocalPlayerId;
}

void NetworkClient::PublishState(const Engine::PlayerState& state)
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_PendingOutgoingState = state;
	}
	m_OutgoingCondition.notify_one();
}

std::optional<Engine::Snapshot> NetworkClient::GetLatestSnapshot() const
{
	std::lock_guard<std::mutex> lock(m_IncomingMutex);
	return m_LatestSnapshot;
}

void NetworkClient::WorkerMain(std::string endpoint)
{
	Engine::PlayerId assignedId = 0;
	std::optional<std::string> dedicatedEndpoint;

	{
		// Bootstrap phase: this Socket lives only inside this nested scope, so it is
		// guaranteed destroyed — connection closed — before any dedicated-session
		// gameplay traffic begins (Milestone 2 Section 4: bootstrap accepts JOIN only,
		// dedicated sessions accept StateUpdate only, and the two must never share a
		// socket).
		Engine::Socket bootstrapSocket(Engine::SocketRole::Request);
		bootstrapSocket.Connect(endpoint);

		bootstrapSocket.Send(ToFrame(Engine::EncodeJoinRequest()));
		std::vector<std::uint8_t> joinReplyBytes = ToBytes(bootstrapSocket.Receive());

		std::optional<Engine::JoinAccepted> accepted;
		if (Engine::PeekMessageType(joinReplyBytes) == Engine::MessageType::JoinAccepted)
		{
			accepted = Engine::DecodeJoinAccepted(joinReplyBytes);
		}

		if (!accepted.has_value())
		{
			// Error, or something malformed/unrecognized: publish disconnected and stop.
			// There is nothing to retry (no reconnect logic here). JOIN no longer
			// replies with a Snapshot now that dedicated sessions exist, so that is not
			// accepted here either.
			std::lock_guard<std::mutex> lock(m_IncomingMutex);
			m_Connected = false;
			return;
		}

		dedicatedEndpoint = BuildDedicatedEndpoint(endpoint, accepted->AssignedPort);
		if (!dedicatedEndpoint.has_value())
		{
			std::lock_guard<std::mutex> lock(m_IncomingMutex);
			m_Connected = false;
			return;
		}

		assignedId = accepted->AssignedId;
	}
	// `bootstrapSocket` was destroyed at the close of the scope above — gone before the
	// dedicated gameplay socket below is ever created.

	Engine::Socket socket(Engine::SocketRole::Request);
	socket.Connect(*dedicatedEndpoint);

	// This thread's own copy of the assigned ID, used only for this thread's own
	// subsequent logic (stamping outgoing states, building the final Leaving message).
	// Never read cross-thread; GetLocalPlayerId() reads the separate, mutex-published
	// m_LocalPlayerId member instead, so there is no ambiguity about which access needs
	// the lock.
	//
	// Connected is only ever set true here, once both the bootstrap handoff and the
	// dedicated connection have fully succeeded. There is deliberately no roster
	// Snapshot yet at this point — JOIN now only returns JoinAccepted; the first
	// regular StateUpdate below receives the first real Snapshot.
	{
		std::lock_guard<std::mutex> lock(m_IncomingMutex);
		m_LocalPlayerId = assignedId;
		m_Connected = true;
	}

	// The most recent state this thread has actually sent, kept purely locally so a
	// clean shutdown can report Leaving=true without ever asking the main thread for
	// m_Player (Game/Entity stay entirely main-thread-only, per design).
	std::optional<Engine::PlayerState> lastSentState;

	bool stopRequested = false;
	while (!stopRequested)
	{
		std::optional<Engine::PlayerState> stateToSend;
		{
			std::unique_lock<std::mutex> lock(m_OutgoingMutex);
			m_OutgoingCondition.wait(
			    lock, [this]() { return m_PendingOutgoingState.has_value() || m_StopRequested; });

			stateToSend = std::move(m_PendingOutgoingState);
			m_PendingOutgoingState.reset();
			stopRequested = m_StopRequested;
		}
		// Mutex released before any network I/O below — never hold a lock across Send/Receive.

		if (stateToSend.has_value())
		{
			stateToSend->Id = assignedId;
			lastSentState = stateToSend;

			socket.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ *stateToSend, false })));
			std::vector<std::uint8_t> replyBytes = ToBytes(socket.Receive());

			std::optional<Engine::Snapshot> snapshot = Engine::DecodeSnapshot(replyBytes);
			if (snapshot.has_value())
			{
				std::lock_guard<std::mutex> lock(m_IncomingMutex);
				m_LatestSnapshot = snapshot;
			}
			// Error/malformed reply: leave the previous Snapshot in place; do not crash.
		}
	}

	// Final, best-effort clean-leave exchange on the dedicated socket (the bootstrap
	// socket is long gone by now). If no normal state was ever published before
	// shutdown, fall back to a neutral state under the assigned ID.
	Engine::PlayerState leavingState =
	    lastSentState.value_or(Engine::PlayerState{ assignedId, 0.0f, 0.0f, 0.0f, 0.0f });
	leavingState.Id = assignedId;

	socket.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ leavingState, true })));
	socket.Receive(); // best-effort; the REQ/REP contract still requires consuming the reply

	{
		std::lock_guard<std::mutex> lock(m_IncomingMutex);
		m_Connected = false;
	}
	// `socket` goes out of scope here and is destroyed on this same thread.
}

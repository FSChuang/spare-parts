#include "NetworkClient.h"

#include "Engine/Network/Socket.h"

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
	: m_Worker(&NetworkClient::WorkerMain, this, serverEndpoint)
{
	// Returns immediately: WorkerMain performs Connect()/JOIN on its own thread, not
	// here, so construction never blocks the caller on network I/O.
}

NetworkClient::~NetworkClient()
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_StopRequested = true;
	}
	m_OutgoingCondition.notify_one();

	// The main thread never touches the Socket directly — it only signals the worker
	// and waits for it to finish on its own terms (see WorkerMain's shutdown handling
	// and the KNOWN LIMITATION documented in NetworkClient.h).
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
	// Created, used, and (on return) destroyed entirely on this thread — the Socket
	// is never named outside this function, so the main thread has no way to touch it.
	Engine::Socket socket(Engine::SocketRole::Request);
	socket.Connect(endpoint);

	socket.Send(ToFrame(Engine::EncodeJoinRequest()));
	std::vector<std::uint8_t> joinReplyBytes = ToBytes(socket.Receive());

	std::optional<Engine::Snapshot> joinSnapshot;
	if (Engine::PeekMessageType(joinReplyBytes) == Engine::MessageType::Snapshot)
	{
		joinSnapshot = Engine::DecodeSnapshot(joinReplyBytes);
	}

	if (!joinSnapshot.has_value())
	{
		// Server replied with an error, or something malformed/unrecognized: publish
		// disconnected and stop. There is nothing to retry (no reconnect logic here).
		std::lock_guard<std::mutex> lock(m_IncomingMutex);
		m_Connected = false;
		return;
	}

	// This thread's own copy of the assigned ID, used only for this thread's own
	// subsequent logic (stamping outgoing states, building the final Leaving message).
	// Never read cross-thread; GetLocalPlayerId() reads the separate, mutex-published
	// m_LocalPlayerId member instead, so there is no ambiguity about which access needs
	// the lock.
	Engine::PlayerId assignedId = joinSnapshot->RecipientId;

	// The most recent state this thread has actually sent, kept purely locally so a
	// clean shutdown can report Leaving=true without ever asking the main thread for
	// m_Player (Game/Entity stay entirely main-thread-only, per design).
	std::optional<Engine::PlayerState> lastSentState;

	{
		std::lock_guard<std::mutex> lock(m_IncomingMutex);
		m_LocalPlayerId = assignedId;
		m_LatestSnapshot = joinSnapshot;
		m_Connected = true;
	}

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

	// Final, best-effort clean-leave exchange. If no normal state was ever published
	// before shutdown, fall back to a neutral state under the assigned ID.
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

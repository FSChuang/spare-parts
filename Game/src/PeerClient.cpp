#include "PeerClient.h"

#include "Engine/Network/Socket.h"

#include <chrono>

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

	// How often the worker wakes on its own (in the absence of an explicit
	// PublishState/UpdatePeers/stop notification) to drain incoming peer traffic.
	// Small enough that peer reception never lags noticeably behind the 15-60 Hz
	// Timeline-scaled publish rates this project uses, large enough to never busy-spin.
	// An explicit notify_one() from PublishState()/UpdatePeers()/the destructor wakes
	// the worker immediately regardless of this interval — this is only the fallback
	// cadence for "nothing new was signaled, but check for incoming messages anyway".
	constexpr std::chrono::milliseconds WorkerPollInterval{ 10 };
}

PeerClient::PeerClient(Engine::PlayerId localPlayerId, std::uint16_t localP2pPort, std::string host)
	: m_Worker(&PeerClient::WorkerMain, this, localPlayerId, localP2pPort, std::move(host))
{
	// Returns immediately: WorkerMain performs the PUB bind and the entire steady-state
	// loop on its own thread, not here. Binding a PUB socket never blocks waiting for a
	// peer (unlike NetworkClient's bootstrap handshake, there is nothing to wait for),
	// so no readiness synchronization is needed — any PublishState()/UpdatePeers() call
	// that happens after construction simply queues in the mailbox until the worker's
	// first loop iteration, which cannot run before the bind above it in WorkerMain.
}

PeerClient::~PeerClient()
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_StopRequested = true;
	}
	m_WorkerWakeCondition.notify_one();

	// The main thread never touches a Socket directly. Unlike NetworkClient, the
	// worker's only blocking-ish wait is a short, bounded condition_variable wait —
	// there is no blocking Receive() anywhere in this class — so this join() completes
	// promptly regardless of whether any peer is alive or reachable.
	if (m_Worker.joinable())
	{
		m_Worker.join();
	}
}

void PeerClient::PublishState(const Engine::PlayerState& state)
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_PendingOutgoingState = state;
	}
	m_WorkerWakeCondition.notify_one();
}

void PeerClient::UpdatePeers(const std::vector<Engine::PeerInfo>& peers)
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_PendingPeers = peers;
	}
	m_WorkerWakeCondition.notify_one();
}

void PeerClient::PublishEvent(const Engine::NetworkEvent& event)
{
	{
		std::lock_guard<std::mutex> lock(m_OutgoingMutex);
		m_PendingOutgoingEvents.push_back(event);
	}
	m_WorkerWakeCondition.notify_one();
}

std::vector<Engine::NetworkEvent> PeerClient::PollEvents()
{
	std::lock_guard<std::mutex> lock(m_IncomingMutex);
	std::vector<Engine::NetworkEvent> events(m_IncomingEvents.begin(), m_IncomingEvents.end());
	m_IncomingEvents.clear();
	return events;
}

std::unordered_map<Engine::PlayerId, Engine::PlayerState> PeerClient::GetLatestPeerStates() const
{
	std::lock_guard<std::mutex> lock(m_IncomingMutex);
	return m_LatestPeerStates;
}

std::uint64_t PeerClient::GetSentStateUpdateCount() const
{
	return m_SentStateUpdateCount.load(std::memory_order_relaxed);
}

void PeerClient::WorkerMain(Engine::PlayerId localPlayerId, std::uint16_t localP2pPort, std::string host)
{
	// Created, used, and (on return) destroyed entirely on this thread — neither
	// Socket is ever named outside this function, so the main thread has no way to
	// touch either.
	Engine::Socket pub(Engine::SocketRole::Publish);
	pub.Bind("tcp://*:" + std::to_string(localP2pPort));

	Engine::Socket sub(Engine::SocketRole::Subscribe);
	sub.Subscribe(""); // empty topic: subscribe to everything a connected peer sends

	// Worker-local only (never touched from the main thread): the peers this worker is
	// ACTUALLY connected to right now, keyed by PlayerId so a directory change can
	// precisely Disconnect() only the peers that genuinely left, and so a repeated,
	// unchanged UpdatePeers() call reconnects nothing.
	std::unordered_map<Engine::PlayerId, std::string> connectedPeers;

	// The most recent state this thread has actually sent, kept purely locally so the
	// final advisory Leaving broadcast (see below) can reuse it — mirrors
	// NetworkClient::WorkerMain's identical `lastSentState` pattern.
	std::optional<Engine::PlayerState> lastSentState;

	bool stopRequested = false;
	while (!stopRequested)
	{
		std::optional<Engine::PlayerState> stateToSend;
		std::optional<std::vector<Engine::PeerInfo>> desiredPeers;
		std::deque<Engine::NetworkEvent> eventsToSend;
		{
			std::unique_lock<std::mutex> lock(m_OutgoingMutex);
			// Bounded wait, never a blocking Receive(): wakes immediately on a new
			// PublishState()/UpdatePeers()/destructor call via notify_one(), or at
			// worst after WorkerPollInterval, so incoming peer traffic is never left
			// undrained for long even if nothing new was ever published locally.
			m_WorkerWakeCondition.wait_for(lock, WorkerPollInterval,
			                               [this]() {
				                               return m_PendingOutgoingState.has_value() ||
				                                      m_PendingPeers.has_value() ||
				                                      !m_PendingOutgoingEvents.empty() || m_StopRequested;
			                               });

			stateToSend = std::move(m_PendingOutgoingState);
			m_PendingOutgoingState.reset();
			desiredPeers = std::move(m_PendingPeers);
			m_PendingPeers.reset();
			eventsToSend.swap(m_PendingOutgoingEvents);
			stopRequested = m_StopRequested;
		}
		// Mutex released before any network I/O below — never hold a lock across
		// Send/TryReceive/Connect/Disconnect.

		if (desiredPeers.has_value())
		{
			// Build the desired PlayerId -> endpoint map, excluding self: this worker
			// never subscribes to its own PUB socket.
			std::unordered_map<Engine::PlayerId, std::string> desired;
			for (const Engine::PeerInfo& peer : *desiredPeers)
			{
				if (peer.Id == localPlayerId)
				{
					continue;
				}
				desired.emplace(peer.Id, "tcp://" + host + ":" + std::to_string(peer.P2pPort));
			}

			// Disconnect peers no longer desired (or reassigned to a different
			// endpoint) before connecting new ones, so a changed port for the same
			// PlayerId never leaves two live connections for one identity.
			for (auto it = connectedPeers.begin(); it != connectedPeers.end();)
			{
				auto found = desired.find(it->first);
				if (found == desired.end() || found->second != it->second)
				{
					sub.Disconnect(it->second);
					it = connectedPeers.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (const auto& [id, endpoint] : desired)
			{
				if (connectedPeers.find(id) == connectedPeers.end())
				{
					sub.Connect(endpoint);
					connectedPeers.emplace(id, endpoint);
				}
			}

			// A peer dropped from the directory must not leave its last-known state
			// behind indefinitely — remove it from the latest-state map too, under its
			// own short critical section (never nested with anything above).
			{
				std::lock_guard<std::mutex> lock(m_IncomingMutex);
				for (auto it = m_LatestPeerStates.begin(); it != m_LatestPeerStates.end();)
				{
					it = (desired.find(it->first) == desired.end()) ? m_LatestPeerStates.erase(it) : std::next(it);
				}

				// Same for notifications not yet polled: a removed peer's queued events are
				// discarded rather than handed to the game after it has gone.
				for (auto it = m_IncomingEvents.begin(); it != m_IncomingEvents.end();)
				{
					it = (desired.find(it->SourcePlayerId) == desired.end()) ? m_IncomingEvents.erase(it)
					                                                         : std::next(it);
				}
			}
		}

		if (stateToSend.has_value())
		{
			stateToSend->Id = localPlayerId;
			lastSentState = stateToSend;
			pub.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ *stateToSend, false })));
			m_SentStateUpdateCount.fetch_add(1, std::memory_order_relaxed);
		}

		// Event notifications go out in the order they were raised, after this tick's state
		// (the two are independent streams on the one PUB socket). The source id is stamped
		// here, on the worker, from the id this client was constructed with.
		for (Engine::NetworkEvent event : eventsToSend)
		{
			event.SourcePlayerId = localPlayerId;
			pub.Send(ToFrame(Engine::EncodeNetworkEvent(event)));
		}

		// Drain every currently-available peer message — TryReceive() never blocks, so
		// this loop always terminates once nothing more is immediately available.
		for (;;)
		{
			std::optional<std::string> frame = sub.TryReceive();
			if (!frame.has_value())
			{
				break;
			}

			std::vector<std::uint8_t> bytes = ToBytes(*frame);

			// Multiplexing: the leading byte alone decides which decoder sees the frame, so an
			// event frame is never handed to the state decoder (or the reverse).
			if (Engine::IsNetworkEventFrame(bytes))
			{
				std::optional<Engine::NetworkEvent> event = Engine::DecodeNetworkEvent(bytes);
				if (!event.has_value() || event->SourcePlayerId == localPlayerId ||
				    connectedPeers.find(event->SourcePlayerId) == connectedPeers.end())
				{
					continue; // malformed, our own broadcast, or not a currently-desired peer
				}

				std::lock_guard<std::mutex> lock(m_IncomingMutex);
				m_IncomingEvents.push_back(*event);
				if (m_IncomingEvents.size() > MaxQueuedIncomingEvents)
				{
					m_IncomingEvents.pop_front();
				}
				continue;
			}

			std::optional<Engine::StateUpdate> update = Engine::DecodeStateUpdate(bytes);
			if (!update.has_value())
			{
				continue; // malformed: ignore, never crash
			}
			if (update->Leaving)
			{
				continue; // advisory only; directory removal (via UpdatePeers) is authoritative
			}
			if (update->State.Id == localPlayerId)
			{
				continue; // never store our own broadcast
			}
			if (connectedPeers.find(update->State.Id) == connectedPeers.end())
			{
				continue; // not a currently-desired peer (e.g. a stray message from one just removed)
			}

			std::lock_guard<std::mutex> lock(m_IncomingMutex);
			m_LatestPeerStates[update->State.Id] = update->State;
		}
	}

	// One final, best-effort advisory broadcast. Fire-and-forget: no acknowledgment is
	// waited for, and correctness never depends on any peer actually receiving it — the
	// server's peer directory remains the authoritative removal signal (see
	// server_main.cpp). If no real state was ever published, fall back to a neutral
	// state under the local PlayerId, mirroring NetworkClient::WorkerMain.
	Engine::PlayerState leavingState =
	    lastSentState.value_or(Engine::PlayerState{ localPlayerId, 0.0f, 0.0f, 0.0f, 0.0f });
	leavingState.Id = localPlayerId;
	pub.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ leavingState, true })));

	// `pub`/`sub` go out of scope here and are destroyed on this same thread.
}

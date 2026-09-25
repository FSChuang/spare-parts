#pragma once

#include "Engine/Network/Protocol.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Game-facing peer-to-peer networking glue for Milestone 2 Section 5 (Spare
// Parts-specific, not a generic Engine abstraction). Owns a background worker thread
// that exclusively creates, uses, and destroys both its ZeroMQ sockets — one Publish,
// one Subscribe — for its entire lifetime. The main/game thread never touches ZeroMQ
// directly and never blocks on network I/O. (Engine::Socket is only ever named inside
// PeerClient.cpp's worker function, not in this header, for the same reason
// NetworkClient keeps it out of its own header.)
//
// NOT yet wired into Game as of this checkpoint: this class is proven independently,
// over a real ZeroMQ PUB/SUB transport, before any Game/m_RemotePlayers integration.
//
// HOST ASSUMPTION: every peer is assumed reachable at 127.0.0.1 (default constructor
// argument) — this assignment's demo runs the server and every client on one machine,
// and Engine::PeerInfo deliberately carries no host field. This is a fixed, documented
// limitation, not general endpoint configuration; the `host` parameter exists only so
// tests can be explicit about it, never to support real multi-machine deployment.
//
// Main-thread API — every method here is non-blocking:
//   PublishState()        replaces the latest not-yet-sent local state; no network I/O.
//   UpdatePeers()          replaces the latest desired peer set; no network I/O. The
//                          worker reconciles its actual SUB connections against this on
//                          its own schedule.
//   GetLatestPeerStates()  a copy of the latest state received from each remote peer,
//                          keyed by PlayerId; no network I/O.
class PeerClient
{
public:
	explicit PeerClient(Engine::PlayerId localPlayerId, std::uint16_t localP2pPort, std::string host = "127.0.0.1");
	~PeerClient();

	PeerClient(const PeerClient&) = delete;
	PeerClient& operator=(const PeerClient&) = delete;

	// Replaces the latest not-yet-sent local state (a single slot, not a queue) and
	// wakes the worker thread. Safe to call every frame; intermediate unsent states are
	// simply overwritten and never sent — only the newest matters.
	void PublishState(const Engine::PlayerState& state);

	// Replaces the desired peer set (a single slot, not a queue) and wakes the worker
	// thread. PlayerId is the only identity used — never vector index. The worker
	// connects newly-desired peers and disconnects no-longer-desired ones; unchanged
	// peers are left alone (never reconnected).
	void UpdatePeers(const std::vector<Engine::PeerInfo>& peers);

	// A copy of the latest state received from each remote peer, keyed by PlayerId.
	// A peer removed via UpdatePeers() (and, before that, the local player's own id)
	// never appears here. MaxPlayers <= 8, so copying the whole map is cheap.
	std::unordered_map<Engine::PlayerId, Engine::PlayerState> GetLatestPeerStates() const;

private:
	// Runs entirely on the worker thread: binds a Publish socket to `localP2pPort`,
	// creates a Subscribe socket (subscribed to everything), then loops reconciling the
	// desired peer set, publishing whatever local state is pending, and draining
	// whatever peer messages are currently available — see PeerClient.cpp for the exact
	// loop structure and wait strategy.
	void WorkerMain(Engine::PlayerId localPlayerId, std::uint16_t localP2pPort, std::string host);

	// --- outgoing: written by the main thread, consumed by the worker thread ---
	mutable std::mutex m_OutgoingMutex;
	std::condition_variable m_WorkerWakeCondition;
	std::optional<Engine::PlayerState> m_PendingOutgoingState;
	std::optional<std::vector<Engine::PeerInfo>> m_PendingPeers;
	bool m_StopRequested = false;

	// --- incoming: written by the worker thread, read by the main thread ---
	mutable std::mutex m_IncomingMutex;
	std::unordered_map<Engine::PlayerId, Engine::PlayerState> m_LatestPeerStates;

	// Constructed last so it starts only once every member above it is fully
	// initialized (WorkerMain reads/writes them from the moment it starts running).
	std::thread m_Worker;
};

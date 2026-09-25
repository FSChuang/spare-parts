#pragma once

#include "Engine/Network/Protocol.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Game-facing networking glue for Milestone 2 Sections 2-4 (Spare Parts-specific, not a
// generic Engine abstraction). Owns a background worker thread that exclusively
// creates, uses, and destroys every ZeroMQ Socket for its entire lifetime — the
// main/game thread never touches ZeroMQ directly and never blocks on network I/O.
// (Engine::Socket is only ever named inside NetworkClient.cpp's worker function, not
// in this header, precisely because the main thread has no business holding one.)
//
// As of Section 4, the worker performs a two-phase handshake against the production
// bootstrap + dedicated-session server topology: a temporary bootstrap socket sends
// JOIN and receives JoinAccepted{id, port}, is destroyed, and a second, separate
// dedicated socket connects to that assigned port for the rest of this client's
// lifetime (every StateUpdate/Snapshot exchange, and the final Leaving=true). The
// bootstrap socket and the dedicated socket are never the same object and never
// coexist.
//
// Main-thread API — every method here is non-blocking:
//   PublishState()       replaces the latest not-yet-sent local state; no network I/O.
//   GetLatestSnapshot()  returns the latest known roster, if any; no network I/O.
//   IsConnected() / GetLocalPlayerId()  race-free reads of worker-published state.
//
// GetLatestSnapshot() may briefly return std::nullopt even once IsConnected() is true:
// JOIN itself no longer carries a roster (JoinAccepted has none), so the first real
// Snapshot only arrives after this client's first PublishState()-driven StateUpdate.
//
// KNOWN LIMITATION (accepted through Milestone 2 Section 4): if the worker thread is
// already blocked inside a Socket::Receive() call (bootstrap JOIN or a dedicated-session
// exchange) when the server stops responding, the destructor's std::thread::join() will
// wait for however long that Receive() takes to return — or forever, if the server never
// replies again. No receive timeout, reconnect, heartbeat, or socket cancellation is
// implemented to bound this; that remains explicitly out of scope. Shutdown completes
// promptly whenever the server remains healthy and responsive, which is the case this
// step is required to get right.
class NetworkClient
{
public:
	explicit NetworkClient(const std::string& serverEndpoint);
	~NetworkClient();

	NetworkClient(const NetworkClient&) = delete;
	NetworkClient& operator=(const NetworkClient&) = delete;

	// True once the worker thread's JOIN has succeeded and an ID was assigned. False
	// if JOIN failed (error/malformed reply) or hasn't completed yet. Race-free: both
	// this and GetLocalPlayerId() read state the worker publishes under m_IncomingMutex.
	bool IsConnected() const;
	Engine::PlayerId GetLocalPlayerId() const;

	// Replaces the latest not-yet-sent local state (a single slot, not a queue) and
	// wakes the worker thread. Safe to call every frame from the main thread; never
	// performs network I/O. If the game produces frames faster than the network thread
	// can exchange them, older unsent states are simply overwritten and never sent —
	// intentional, since only the most current state matters here.
	void PublishState(const Engine::PlayerState& state);

	// The latest roster the worker thread has received, if any exchange has completed
	// yet. Never blocks on network I/O. Deliberately not cleared after reading —
	// re-applying an unchanged Snapshot on repeated frames is harmless, since Game's
	// remote-player update (add/update/remove by PlayerId) is idempotent.
	std::optional<Engine::Snapshot> GetLatestSnapshot() const;

private:
	// Runs entirely on the worker thread: connects a temporary bootstrap Socket,
	// performs JOIN, decodes JoinAccepted, and destroys that socket; then connects a
	// separate dedicated Socket to the assigned port and loops sending the latest
	// published state and publishing whatever Snapshot comes back, until told to stop —
	// at which point it reports Leaving=true on the dedicated socket before returning
	// (which destroys it, on this same thread).
	void WorkerMain(std::string endpoint);

	// --- outgoing: written by the main thread, consumed by the worker thread ---
	mutable std::mutex m_OutgoingMutex;
	std::condition_variable m_OutgoingCondition;
	std::optional<Engine::PlayerState> m_PendingOutgoingState;
	bool m_StopRequested = false;

	// --- incoming: written by the worker thread, read by the main thread ---
	mutable std::mutex m_IncomingMutex;
	std::optional<Engine::Snapshot> m_LatestSnapshot;
	Engine::PlayerId m_LocalPlayerId = 0;
	bool m_Connected = false;

	// Constructed last so it starts only once every member above it is fully
	// initialized (WorkerMain reads/writes them from the moment it starts running).
	std::thread m_Worker;
};

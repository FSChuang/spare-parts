#pragma once

#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <string>
#include <vector>

// Game-facing networking glue for Milestone 2 Section 2 (Spare Parts-specific, not a
// generic Engine abstraction). Owns one persistent ZeroMQ REQ connection to the
// headless server, joins once at construction, and lets Game exchange its local
// player's state for the server's latest roster snapshot once per frame.
//
// IMPORTANT: every exchange here (the initial JOIN and every later SendState()) is a
// blocking, synchronous REQ/REP round trip on the calling (main/render) thread. If no
// server is reachable, construction will block indefinitely on the JOIN reply. This is
// an accepted, documented limitation for Section 2 only — Section 3 introduces the
// threading needed to stop networking from blocking the game loop. Do not add a
// receive timeout, retry logic, or a worker thread here; that is out of scope for
// this step.
class NetworkClient
{
public:
	explicit NetworkClient(const std::string& serverEndpoint);
	~NetworkClient();

	NetworkClient(const NetworkClient&) = delete;
	NetworkClient& operator=(const NetworkClient&) = delete;

	// True once JOIN has succeeded and an ID was assigned. False if the server
	// replied with an error (e.g. registry full) or a malformed/unrecognized reply.
	bool IsConnected() const;

	Engine::PlayerId GetLocalPlayerId() const;

	// Sends the local player's current state (its Id field is overwritten with the
	// assigned local player ID, so the caller cannot report under the wrong ID) and
	// blocks for the server's reply. Returns false if not connected or the exchange
	// failed (error/malformed reply); the previously stored roster is left unchanged
	// in that case so the game keeps rendering the last known state rather than
	// clearing everyone.
	bool SendState(const Engine::PlayerState& localState);

	// The latest roster received from the server, including the local player's own
	// entry. Game is responsible for excluding its own ID when building remote
	// players (see Game::UpdateNetworking).
	const std::vector<Engine::PlayerState>& GetLatestRoster() const;

	// Reports Leaving=true so the server removes this player immediately, rather
	// than waiting on any future disconnect/timeout detection (Section 2 has none).
	// Idempotent: a no-op if not connected or already disconnected. Called
	// automatically by the destructor, so RAII covers every normal exit path;
	// exposed publicly only in case Game needs an earlier, explicit shutdown point.
	void Disconnect();

private:
	Engine::Socket m_Socket;
	bool m_Connected;
	Engine::PlayerId m_LocalPlayerId;
	std::vector<Engine::PlayerState> m_LatestRoster;
};

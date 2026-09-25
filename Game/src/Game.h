#pragma once

#include "Engine/Entity/Entity.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "NetworkClient.h"
#include "PeerClient.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace Engine
{
	class InputManager;
	class Renderer;
}

// Minimal playable vertical slice for Spare Parts (拼裝求生): a moving platform, a
// controllable player affected by gravity, and a patrolling enemy. All game-specific state and
// rules live here, never in Engine/ (ENGINEERING_SPEC.md §0, §5).
//
// Milestone 2 Section 2: the local player is still fully client-simulated (input,
// physics, collisions), unchanged. Remote players are visual replicas only — no local
// input, no local physics, no local collision response, keyed explicitly by the
// PlayerId the server assigned them (never by roster array index, which is
// intentionally unordered).
//
// Milestone 2 Section 4 final checkpoint: m_Platform is now a visual/collision replica
// of the server-authoritative moving platform, too — its position is overwritten every
// frame from snapshot.Platform, never advanced locally with deltaTime, so it appears in
// the same world position for every connected client regardless of that client's own
// Timeline scale. The enemy remains fully local and intentionally unnetworked — this is
// outside the P2P requirement entirely, not an oversight; its position is expected to
// differ between clients, since each client simulates it with its own Timeline-scaled
// deltaTime. Before the first Snapshot ever arrives (JOIN itself no longer carries
// one), m_Platform simply keeps its static construction position — see
// UpdateNetworking().
//
// Milestone 2 Section 5 final integration: remote player position/velocity now come
// EXCLUSIVELY from PeerClient (direct peer-to-peer), never from the server Snapshot.
// Snapshot.Roster is membership data only — it decides WHICH remote PlayerIds are
// allowed to exist, never WHERE they are. There is no dual source of truth: search this
// file for any line reading snapshot.Roster[...].PositionX/PositionY/VelocityX/VelocityY
// and there is none. m_PeerClient is created lazily (once NetworkClient has learned a
// nonzero local PlayerId and P2P port), not in the constructor, because that
// information only becomes available asynchronously after the server's bootstrap
// handshake completes. Local player state now reaches the network on two independent
// schedules: a Timeline-scaled cadence for the P2P feed (0.5x/1x/2x, this project's
// existing scale-controls-rate requirement), and a fixed real-time cadence for the
// server session (membership/platform/peer-directory freshness, independent of Timeline
// scale or pause) — see UpdateNetworking().
//
// Falling off the (now-moving) platform and out the bottom of the logical world, and
// colliding with the enemy, both respawn the player via the same RespawnPlayer() helper
// — see ResolveOutOfBounds()/ResolveEnemyCollision().
class Game
{
public:
	explicit Game(const std::string& serverEndpoint);

	void Update(Engine::InputManager& input, float deltaTime);
	void Render(Engine::Renderer& renderer);

private:
	void UpdatePlayerMovement(Engine::InputManager& input);
	void UpdateEnemyPatrol(float deltaTime);
	void ResolvePlatformCollision();
	void ResolveOutOfBounds();
	void ResolveEnemyCollision();
	void RespawnPlayer();
	void UpdateNetworking(float deltaTime);

	Engine::Entity m_Platform;
	Engine::Entity m_Player;
	Engine::Entity m_Enemy;
	Engine::PhysicsSystem m_Physics;
	float m_EnemyPatrolDirection;

	// Destruction order matters here (members are destroyed in reverse declaration
	// order): m_PeerClient is declared AFTER m_Network, so it is destroyed BEFORE it —
	// peer-to-peer traffic stops first (PeerClient's own advisory, non-authoritative
	// Leaving broadcast), then NetworkClient sends this client's real, authoritative
	// Leaving=true to the server. Correctness never depends on this order (the two are
	// otherwise fully independent), but it mirrors the natural real-world sequence of
	// leaving peers before formally leaving the server.
	NetworkClient m_Network;
	std::unique_ptr<PeerClient> m_PeerClient;
	std::unordered_map<Engine::PlayerId, Engine::Entity> m_RemotePlayers;

	// Milestone 2 Section 4 final checkpoint (redirected to PeerClient in Section 5's
	// final integration): accumulates Timeline-scaled deltaTime so the local player's
	// state is actually published at a rate that scales with the Timeline (deltaTime is
	// already Timeline-scaled by the time it reaches Update()), rather than once per
	// render frame. See UpdateNetworking() for the exact scheme.
	float m_NetworkUpdateAccumulator = 0.0f;

	// Milestone 2 Section 5 final integration: the last time this client sent a
	// server-facing session refresh (membership/platform/peer-directory freshness),
	// measured in real time via steady_clock — deliberately NOT Timeline-scaled, so this
	// keeps happening at a fixed cadence even while Timeline is paused or scaled. See
	// UpdateNetworking() for the exact scheme.
	std::chrono::steady_clock::time_point m_LastServerRefreshTime;
};

#pragma once

#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "NetworkClient.h"
#include "PeerClient.h"

#include <chrono>
#include <memory>
#include <string>

namespace Engine
{
	class InputManager;
	class Renderer;
}

// Playable vertical slice for Spare Parts (拼裝求生): a side-scrolling two-screen world
// with a controllable player affected by gravity, a patrolling enemy, three static
// platforms, two independently-patterned server-authoritative moving platforms, multiple
// hidden spawn points, a hidden death zone, and a hidden scrolling boundary.
//
// Milestone 3: every object this game owns (both moving platforms, all three static
// platforms, the local player, the enemy, every remote player, every spawn point, the
// death zone, the boundary) is a plain Engine::GameObject held in one
// Engine::GameObjectRegistry (m_Objects) — a compositional model, not an inheritance
// hierarchy. Static platforms have no velocity property; spawn points/death
// zone/boundary have no render property and are therefore never drawn
// (Engine::Renderer::DrawObject silently skips them) despite still fully participating
// in collision (Engine::IsColliding only ever reads Transform data). A remote player is
// found by its stable server-assigned PlayerId via m_Objects.FindByNetworkId(), never by
// iteration order or array index.
//
// Side-scrolling (Milestone 3): the world is two 1920-wide screens placed side by side
// (world X 0-3840). m_CameraOffsetX is a purely PRESENTATIONAL client-local value — it is
// never written to any GameObject's actual position, never sent over the network, and
// never read by physics/collision. Crossing the hidden Boundary GameObject (built once,
// at the world-space screen seam, world X ~1920) snaps m_CameraOffsetX between 0 and
// 1920 depending on the player's direction of travel. Render() is the only place
// m_CameraOffsetX is consulted: it draws a COPY of each object translated left by the
// offset, leaving the registry's own world-coordinate GameObjects untouched — exactly
// the `renderX = worldX - cameraOffsetX` split that keeps physics/collision/networking
// entirely in shared world coordinates regardless of which screen is currently in view.
//
// Grounded/jump (Milestone 3): m_IsGrounded reflects whether the player was resolved as
// resting on a platform as of the END of the PREVIOUS frame's ResolvePlatformLanding()
// call; UpdatePlayerMovement() reads it at the START of the current frame to decide
// whether a jump is allowed. This one-frame lag is the standard, imperceptible way to
// answer "is the player currently grounded" without needing a lookahead.
//
// Milestone 2 Section 2: the local player is still fully client-simulated (input,
// physics, collisions), unchanged. Remote players are visual replicas only — no local
// input, no local physics, no local collision response, keyed explicitly by the
// PlayerId the server assigned them (never by roster array index, which is
// intentionally unordered).
//
// Milestone 2 Section 4 / Milestone 3: both moving-platform objects are visual/collision
// replicas of the two server-authoritative trajectories — their positions are
// overwritten every frame from snapshot.Platform/snapshot.Platform2, never advanced
// locally with deltaTime, so they appear in the same world position for every connected
// client regardless of that client's own Timeline scale. The enemy remains fully local
// and intentionally unnetworked — this is outside the P2P requirement entirely, not an
// oversight; its position is expected to differ between clients, since each client
// simulates it with its own Timeline-scaled deltaTime. Before the first Snapshot ever
// arrives (JOIN itself no longer carries one), each moving platform simply keeps its
// static construction position — see UpdateNetworking().
//
// Milestone 2 Section 5 final integration: remote player position/velocity now come
// EXCLUSIVELY from PeerClient (direct peer-to-peer), never from the server Snapshot.
// Snapshot.Roster is membership data only — it decides WHICH remote PlayerIds are
// allowed to exist, never WHERE they are. m_PeerClient is created lazily (once
// NetworkClient has learned a nonzero local PlayerId and P2P port), not in the
// constructor, because that information only becomes available asynchronously after the
// server's bootstrap handshake completes. Local player state now reaches the network on
// two independent schedules: a Timeline-scaled cadence for the P2P feed (0.5x/1x/2x,
// this project's existing scale-controls-rate requirement), and a fixed real-time
// cadence for the server session (membership/platform/peer-directory freshness,
// independent of Timeline scale or pause) — see UpdateNetworking().
//
// Falling into the death zone, and colliding with the enemy, both respawn the player via
// the same centralized RespawnPlayer() helper — see ResolveDeathZone()/
// ResolveEnemyCollision(). RespawnPlayer() always clears velocity and never touches the
// player GameObject's network identity, so remote peers simply observe the next
// published position normally.
class Game
{
public:
	explicit Game(const std::string& serverEndpoint);

	void Update(Engine::InputManager& input, float deltaTime);
	void Render(Engine::Renderer& renderer);

private:
	// Scene construction, called once from the constructor, in dependency order (spawn
	// points must exist before the player is first placed via SelectSpawnPosition()).
	void CreateMovingPlatforms();
	void CreateStaticPlatforms();
	void CreateSpawnPoints();
	void CreateDeathZones();
	void CreateBoundaries();

	void UpdatePlayerMovement(Engine::InputManager& input);
	void UpdateEnemyPatrol(float deltaTime);
	void ResolvePlatformLanding();
	void ResolveDeathZone();
	void ResolveEnemyCollision();
	void RespawnPlayer();
	void UpdateScrolling();
	void UpdateNetworking(float deltaTime);

	// Picks this local player's designated spawn position: a deterministic function of
	// its own server-assigned PlayerId (before one is known, resolves to the first spawn
	// point), so different players spawn at different points without any server
	// involvement — spawn points are deterministic, identical, client-local scene setup.
	Engine::Vector2 SelectSpawnPosition() const;

	// Every GameObject this client knows about: both moving platforms, all three static
	// platforms, the local player, the enemy, every spawn point, the death zone, the
	// boundary, and every currently-visible remote player. One registry, not several ad
	// hoc containers — Render()/collision/physics all operate on it directly.
	Engine::GameObjectRegistry m_Objects;
	Engine::ObjectId m_PlatformObjectId;
	Engine::ObjectId m_VerticalPlatformObjectId;
	Engine::ObjectId m_LocalPlayerObjectId;
	Engine::ObjectId m_EnemyObjectId;
	Engine::ObjectId m_DeathZoneObjectId;
	Engine::ObjectId m_BoundaryObjectId;

	Engine::PhysicsSystem m_Physics;
	float m_EnemyPatrolDirection;

	// True if ResolvePlatformLanding() resolved the player as resting on a platform as
	// of the end of the most recently completed Update() call. Read by
	// UpdatePlayerMovement() at the start of the NEXT frame to gate jumping — see the
	// class comment above for why this one-frame lag is fine.
	bool m_IsGrounded = false;

	// Milestone 3 side-scrolling: purely presentational, client-local camera translation
	// in world-space pixels. Never written to any GameObject, never networked, never
	// read by physics/collision — see the class comment above.
	float m_CameraOffsetX = 0.0f;

	// Destruction order matters here (members are destroyed in reverse declaration
	// order): m_PeerClient is declared AFTER m_Network, so it is destroyed BEFORE it —
	// peer-to-peer traffic stops first (PeerClient's own advisory, non-authoritative
	// Leaving broadcast), then NetworkClient sends this client's real, authoritative
	// Leaving=true to the server. Correctness never depends on this order (the two are
	// otherwise fully independent), but it mirrors the natural real-world sequence of
	// leaving peers before formally leaving the server.
	NetworkClient m_Network;
	std::unique_ptr<PeerClient> m_PeerClient;

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

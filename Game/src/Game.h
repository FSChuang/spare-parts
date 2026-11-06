#pragma once

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "LocalPlayerController.h"
#include "NetworkClient.h"
#include "PeerClient.h"
#include "PeerEventBridge.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
// (world X 0-3840). The camera offset (m_LocalPlayer->CameraOffsetX()) is a purely
// PRESENTATIONAL client-local value — it is never written to any GameObject's actual position, never sent over the network, and
// never read by physics/collision. Crossing the hidden Boundary GameObject (built once,
// at the world-space screen seam, world X ~1920) snaps the camera offset between 0 and
// 1920 depending on the player's direction of travel. Render() is the only place
// the camera offset is consulted: it draws a COPY of each object translated left by the
// offset, leaving the registry's own world-coordinate GameObjects untouched — exactly
// the `renderX = worldX - cameraOffsetX` split that keeps physics/collision/networking
// entirely in shared world coordinates regardless of which screen is currently in view.
//
// Grounded/jump (Milestone 3, event-driven in Milestone 4): the player's grounded state is
// granted by the Collision event raised on a NEW landing and cleared by a jump, by leaving
// the platform, or by a spawn; a Jump input event reads it to decide whether the jump is
// allowed. See LocalPlayerController.h.
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
// Milestone 4 (Event Management System): the local player's gameplay flows through one
// Engine::EventManager owned by Game (m_Events). Input (MoveLeft/MoveRight/Jump), Collision
// (landing, death zone, enemy), Death and Spawn are real events whose handlers apply the
// gameplay consequences — see LocalPlayerController.h for the event chain. Falling into the
// death zone, or touching the enemy, raises Collision -> Death -> Spawn; the Spawn handler
// is the single place a respawn takes effect (position, velocity, grounded, camera) and
// never touches the player GameObject's network identity, so remote peers simply observe the
// next published position normally. Update() runs two deliberate dispatch phases — see its
// definition for the exact frame order.
//
class Game
{
public:
	/// `timeSource` supplies the logical time that stamps every event — main.cpp wires it to
	/// the game Timeline's GetTime(), so a paused Timeline freezes future-scheduled events.
	Game(const std::string& serverEndpoint, std::function<double()> timeSource);

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

	// Reads the keyboard and hands the local player controller this frame's input.
	void PollInputEvents(Engine::InputManager& input);
	void UpdateEnemyPatrol(float deltaTime);
	void UpdateScrolling();
	// Registers a concise stdout line for each of the four event types (manual-run
	// visibility only; no gameplay depends on it).
	void SubscribeEventLogging();
	void UpdateNetworking(float deltaTime);

	// Picks this local player's designated spawn position: a deterministic function of
	// its own server-assigned PlayerId (before one is known, resolves to the first spawn
	// point), so different players spawn at different points without any server
	// involvement — spawn points are deterministic, identical, client-local scene setup.
	Engine::Vector2 SelectSpawnPosition() const;

	// Declared before everything that registers handlers on it, so it is destroyed AFTER
	// them (members are destroyed in reverse declaration order): ScopedSubscriptions must
	// never outlive their EventManager.
	Engine::EventManager m_Events;
	std::vector<Engine::ScopedSubscription> m_EventLogSubscriptions;

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

	// The local player's event-driven gameplay (input intent, landing, hazards, death,
	// respawn, camera offset). Constructed in the constructor body once the player, enemy
	// and death-zone objects exist; declared LAST so it is destroyed FIRST, unsubscribing its
	// handlers while m_Events is still alive. Also owns the presentational camera offset
	// (client-local, never networked, never read by physics/collision — see above).
	std::unique_ptr<LocalPlayerController> m_LocalPlayer;

	// Milestone 4 Checkpoint 3: gameplay event notifications between peers. Announces this
	// client's own deaths through m_PeerClient and turns peers' received death notifications
	// into local events plus a temporary visible marker on that remote player (see
	// PeerEventBridge.h). Declared last for the same reason as m_LocalPlayer: its Death
	// subscription must be released while m_Events is still alive.
	std::unique_ptr<PeerEventBridge> m_PeerEvents;
};

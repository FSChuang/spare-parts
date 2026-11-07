#pragma once

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "BulletField.h"
#include "GameAssets.h"
#include "NetworkClient.h"
#include "PeerClient.h"
#include "PeerEventBridge.h"
#include "ReplayController.h"
#include "ShipController.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace Engine
{
	class InputManager;
	class Renderer;
}

// Astro Arena: a small Astro Party-inspired multiplayer arena. One fixed 1920x1080 screen (world
// coordinates are screen coordinates, no camera): a walled map with a central reactor and four
// barriers, ships that fly forward constantly, rotate left/right and shoot bullets, die when they
// are hit or touch a wall or obstacle, and respawn after a delay with a short invulnerability
// window. There is no wrap-around: the walls are solid and deadly, and bullets are stopped by them.
//
// Controls: A/Left rotate left, D/Right rotate right, Space fire; F5/F6/F7 record/stop/play a
// replay; P pauses and 1/2/3 scale the Timeline (engine keys).
//
// Every object is a plain Engine::GameObject in one registry (m_Objects): the local ship, every
// remote ship, every bullet, and the map's walls and obstacles (static Boundary objects). Ship-specific state (heading, alive, cooldown, invulnerability) is
// Game-side — see ShipController.h; bullets — see BulletField.h. Ships and bullets are square
// GameObjects whose circle (radius = half the size) is the collision shape.
//
// The local ship is fully client-simulated and gameplay flows through one Engine::EventManager
// (m_Events): Input -> (Collision -> Death ->) Spawn, see ShipController.h.
//
// NETWORKING: server membership via NetworkClient, remote ship position/velocity exclusively from
// PeerClient (peer-to-peer), created lazily once the handshake completes. The wire carries no
// heading: a remote ship's heading is derived from its velocity direction (it flies at constant
// speed). COMBAT is peer-authoritative: each client alone decides whether ITS ship was hit. A
// fired bullet is announced as a Shoot (origin + heading); every other client rebuilds it from
// that and its own shared constants, and a rebuilt bullet that reaches a client's own ship raises
// that client's own Collision -> Death, announced as Death and later Spawn. Nobody ever declares
// another player dead; a bullet of mine touching a remote ship's picture does nothing. A remote
// ship is shown alive until its Death arrives and hidden (after a brief marker) until its Spawn.
// See PeerEventBridge.h.
//
// REPLAY: F5 starts recording, F6 stops, F7 plays it back (each raises a replay control event; on
// a Mac laptop hold fn). A local, in-memory review of the local ship and its bullets — see
// ReplayController.h. While it plays the live ship and live bullets are frozen out of the
// simulation and peers keep seeing the ship where it stood.
class Game
{
public:
	/// `timeSource` supplies the logical time that stamps every event and drives the ship's
	/// cooldown/respawn/invulnerability — main.cpp wires it to the game Timeline's GetTime().
	Game(const std::string& serverEndpoint, std::function<double()> timeSource, const GameAssets& assets);

	void Update(Engine::InputManager& input, float deltaTime);
	void Render(Engine::Renderer& renderer);

private:
	// Reads the keyboard and hands the ship controller this frame's input.
	void PollInputEvents(Engine::InputManager& input);
	// Registers a concise stdout line for each of the four event types (manual-run
	// visibility only; no gameplay depends on it).
	void SubscribeEventLogging();
	// Remembers where each ship dies so an explosion can play there (see Render()).
	void SubscribeExplosions();
	void UpdateNetworking(float deltaTime);

	// Turns the map's walls and obstacles into static Boundary objects in the registry.
	void CreateArenaSolids();

	// Drawing helpers, called from Render() in painter's order.
	void DrawBackground(Engine::Renderer& renderer) const;
	void DrawMap(Engine::Renderer& renderer) const;
	void DrawShip(Engine::Renderer& renderer, Engine::TextureId texture, Engine::Vector2 centre, float heading) const;
	void DrawExplosions(Engine::Renderer& renderer, double now) const;

	// This local player's designated spawn centre: a deterministic function of its own
	// server-assigned PlayerId (before one is known, the first spawn point), so different
	// players spawn at different points without any server involvement.
	Engine::Vector2 SelectSpawnCentre() const;

	// Declared before everything that registers handlers on it, so it is destroyed AFTER
	// them (members are destroyed in reverse declaration order): ScopedSubscriptions must
	// never outlive their EventManager.
	Engine::EventManager m_Events;
	std::vector<Engine::ScopedSubscription> m_EventLogSubscriptions;

	// A copy of the logical clock (the EventManager keeps its own).
	std::function<double()> m_TimeSource;

	// The textures the game draws with (handles; the Renderer owns the textures).
	GameAssets m_Assets;

	Engine::GameObjectRegistry m_Objects;
	Engine::ObjectId m_LocalShipObjectId;

	// Zero gravity: PhysicsSystem is used purely as the velocity integrator for ships/bullets.
	Engine::PhysicsSystem m_Physics;

	// Destruction order matters here (members are destroyed in reverse declaration
	// order): m_PeerClient is declared AFTER m_Network, so it is destroyed BEFORE it —
	// peer-to-peer traffic stops first, then NetworkClient sends this client's real,
	// authoritative Leaving=true to the server. Correctness never depends on this order.
	NetworkClient m_Network;
	std::unique_ptr<PeerClient> m_PeerClient;

	// Accumulates Timeline-scaled deltaTime so the local ship's state is published at a rate
	// that scales with the Timeline (see UpdateNetworking()).
	float m_NetworkUpdateAccumulator = 0.0f;

	// The last time this client sent a server-facing session refresh, in real time —
	// deliberately NOT Timeline-scaled (see UpdateNetworking()).
	std::chrono::steady_clock::time_point m_LastServerRefreshTime;

	// Bullets in flight (they live in m_Objects; this holds who fired them and their age).
	BulletField m_Bullets;

	// Remote ship object -> its last known heading, derived from velocity (presentation only).
	std::unordered_map<Engine::ObjectId, float> m_RemoteHeadings;

	// Ships that died recently: where, and when (logical time). Each plays one explosion.
	struct Explosion
	{
		Engine::Vector2 Centre;
		double StartTime;
	};
	std::vector<Explosion> m_Explosions;

	// The local ship's event-driven gameplay. Constructed in the constructor body once the ship
	// object exists; declared LAST so it is destroyed FIRST, unsubscribing its handlers while
	// m_Events is still alive.
	std::unique_ptr<ShipController> m_Ship;

	// Gameplay event notifications between peers (death/spawn/shoot announcements and remote life state).
	// Declared last for the same reason as m_Ship.
	std::unique_ptr<PeerEventBridge> m_PeerEvents;

	// Record / play back the local ship's gameplay. Declared last: it references m_Ship and
	// m_PeerEvents and must release its subscriptions before them and before m_Events.
	std::unique_ptr<ReplayController> m_Replay;
};

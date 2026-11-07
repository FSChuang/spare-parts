#pragma once

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "BulletField.h"
#include "NetworkClient.h"
#include "PeerClient.h"
#include "PeerEventBridge.h"
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

// Astro rebuild Checkpoint B: a small Astro Party-inspired arena. One fixed 1920x1080 screen
// (world coordinates are screen coordinates, no camera), ships that fly forward constantly,
// rotate left/right and shoot bullets, wrap around the arena edges, die when hit, and respawn
// after a delay with a short invulnerability window. Rendering is primitives only for now.
//
// Controls: A/Left rotate left, D/Right rotate right, Space fire, K (temporary dev aid) drop
// a hostile bullet on yourself to see the death/respawn chain while nobody else can shoot you.
//
// Every object is a plain Engine::GameObject in one registry (m_Objects): the local ship, every
// remote ship, every bullet. Ship-specific state (heading, alive, cooldown, invulnerability) is
// Game-side — see ShipController.h; bullets — see BulletField.h. Ships and bullets are square
// GameObjects whose circle (radius = half the size) is the collision shape.
//
// The local ship is fully client-simulated and gameplay flows through one Engine::EventManager
// (m_Events): Input -> (Collision -> Death ->) Spawn, see ShipController.h.
//
// NETWORKING is the Milestone 2/4 design, unchanged in this checkpoint: server membership via
// NetworkClient, remote ship position/velocity exclusively from PeerClient (peer-to-peer),
// created lazily once the handshake completes. The wire carries no heading: a remote ship's
// heading is derived from its velocity direction (it flies at constant speed). Bullets are NOT
// networked yet, so a ship cannot yet be shot by a remote player — that is the next checkpoint
// — and a remote death is shown by PeerEventBridge's temporary marker.
//
// REPLAY (F5/F6/F7) is switched off in this checkpoint: the old replay samples described a
// platformer (camera, platforms, enemy). The Engine's ReplayRecorder and the replay control
// events remain; the Astro replay controller returns in a later checkpoint.
class Game
{
public:
	/// `timeSource` supplies the logical time that stamps every event and drives the ship's
	/// cooldown/respawn/invulnerability — main.cpp wires it to the game Timeline's GetTime().
	Game(const std::string& serverEndpoint, std::function<double()> timeSource);

	void Update(Engine::InputManager& input, float deltaTime);
	void Render(Engine::Renderer& renderer);

private:
	// Reads the keyboard and hands the ship controller this frame's input.
	void PollInputEvents(Engine::InputManager& input);
	// Registers a concise stdout line for each of the four event types (manual-run
	// visibility only; no gameplay depends on it).
	void SubscribeEventLogging();
	void UpdateNetworking(float deltaTime);

	void DrawShip(Engine::Renderer& renderer, Engine::Vector2 centre, float heading, Engine::Color bodyColor) const;

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

	// The local ship's event-driven gameplay. Constructed in the constructor body once the ship
	// object exists; declared LAST so it is destroyed FIRST, unsubscribing its handlers while
	// m_Events is still alive.
	std::unique_ptr<ShipController> m_Ship;

	// Gameplay event notifications between peers (death announce + remote death marker).
	// Declared last for the same reason as m_Ship.
	std::unique_ptr<PeerEventBridge> m_PeerEvents;
};

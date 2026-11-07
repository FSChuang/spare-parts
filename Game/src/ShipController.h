#pragma once

#include "Engine/Core/Core.h"
#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"

#include "ArenaRules.h"
#include "BulletField.h"

#include <functional>
#include <limits>

// Astro rebuild Checkpoint B: the LOCAL ship's gameplay, driven by the Engine event system.
// Spare Parts-specific and deliberately SDL-free and network-free, so the whole event flow is
// unit tested without a Game. It replaces the platformer's LocalPlayerController and keeps
// that class's useful structure: ScopedSubscriptions, a per-frame death guard, and a real
// Input -> Collision -> Death -> Spawn chain.
//
// The ship's generic physical state (position, size, velocity, network id, colour) lives in an
// Engine::GameObject. Everything ship-specific (heading, alive, fire cooldown, invulnerability)
// lives in ShipState here — never in the Engine.
//
// Game remains responsible for the scene, the bullet field, world update order, networking and
// rendering. This class never owns any of those; it only acts on the ship object and fires
// into / consumes bullets from the BulletField it is handed.
//
// THE EVENT CHAIN
//   Input       MoveLeft / MoveRight (held) set the rotation intent; ActionPrimary fires.
//   Collision   Raised by DetectBulletHits() when a bullet overlaps the ship. Detection never
//               kills anything. The handler consumes the bullet and, unless the ship is
//               invulnerable, raises Death.
//   Death       The handler marks the ship dead, stops it, and SCHEDULES a Spawn
//               `RespawnDelay` later. It does not revive anything.
//   Spawn       The ONE place a ship becomes alive: position, heading (toward the arena
//               centre), velocity, alive, and the start of the invulnerability window. The
//               initial spawn at start-up goes through here too (a ship starts dead).
// Spawn.Position is the ship's CENTRE in arena coordinates.
//
// TIME. Cooldown, respawn and invulnerability use the logical clock handed in (the same
// Timeline-backed source as the EventManager), so pausing or scaling the Timeline stretches
// them exactly as it stretches movement. Movement uses the Timeline-scaled deltaTime.

/// Ship-specific gameplay state (see the file comment).
struct ShipState
{
	float HeadingRadians = 0.0f;
	bool Alive = false;
	/// Logical time of the last shot; -infinity until the first, so the first shot is never
	/// blocked by the cooldown.
	double LastFireTime = -std::numeric_limits<double>::infinity();
	/// Logical time until which the ship cannot be killed.
	double InvulnerableUntil = -std::numeric_limits<double>::infinity();
};

struct ShipConfig
{
	float Speed;               // px/s, forward
	float RotationSpeed;       // rad/s
	float Radius;              // collision circle
	float MuzzleDistance;      // bullet spawn distance ahead of the centre
	double FireCooldown;       // s
	double RespawnDelay;       // s
	double InvulnerabilityDuration; // s
};

// Dispatch priorities for events due at the same logical time (a tie-breaker only: the
// EventManager orders by timestamp first). Input applies first; a Spawn takes effect before
// anything else queued beside it.
constexpr Engine::EventPriority InputEventPriority = Engine::EventPriority::High;
constexpr Engine::EventPriority CollisionEventPriority = Engine::EventPriority::Normal;
constexpr Engine::EventPriority DeathEventPriority = Engine::EventPriority::High;
constexpr Engine::EventPriority SpawnEventPriority = Engine::EventPriority::Critical;

class ShipController
{
public:
	/// `events`, `objects` and `bullets` must outlive this controller (Game declares them
	/// first). `selectSpawnCentre` is asked for the ship's spawn point each time it dies.
	/// `timeSource` is the logical clock. Subscribes the handlers; removed on destruction.
	ShipController(Engine::EventManager& events, Engine::GameObjectRegistry& objects, BulletField& bullets,
	               Engine::ObjectId shipId, ShipConfig config, std::function<Engine::Vector2()> selectSpawnCentre,
	               std::function<double()> timeSource);

	// Handlers capture `this`, so the controller must never be copied or moved.
	ShipController(const ShipController&) = delete;
	ShipController& operator=(const ShipController&) = delete;

	/// Start of a frame: allows a new death to be raised (see RaiseDeath).
	void BeginFrame();

	/// Turns this frame's held rotate keys and fresh fire press into Input events. A
	/// MoveLeft/MoveRight event is raised only when that held key CHANGED since the last
	/// call; an ActionPrimary event is raised once per fresh press (`firePressed` must be an
	/// edge, not a held state).
	void RaiseInputEvents(bool leftHeld, bool rightHeld, bool firePressed);

	/// Turns the ship by the current rotation intent and sets its forward velocity; a dead
	/// ship gets zero velocity instead. Every frame; a zero `deltaTime` leaves the heading
	/// unchanged. The caller integrates the velocity with the physics system afterwards.
	void ApplySteering(float deltaTime);

	/// Wraps a ship that has fully left the arena to the opposite side (WrapShipCentre).
	void WrapAroundArena();

	/// Raises a Collision{ship, bullet} event for every bullet, not fired by this ship,
	/// currently overlapping it. Does nothing while dead. Changes no state itself.
	void DetectBulletHits();

	/// Raises a Spawn event placing the ship's centre at `centre` (the initial spawn).
	void RequestSpawn(Engine::Vector2 centre);

	const ShipState& State() const;
	bool IsAlive() const;
	bool IsInvulnerable() const;
	float Heading() const;

private:
	void OnInput(const Engine::InputEventData& input);
	void OnCollision(const Engine::CollisionEventData& collision);
	void OnDeath(const Engine::DeathEventData& death);
	void OnSpawn(const Engine::SpawnEventData& spawn);

	void TryFire();
	void RaiseDeath(Engine::ObjectId cause);
	Engine::GameObject& Ship();

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	BulletField& m_Bullets;
	Engine::ObjectId m_ShipId;
	ShipConfig m_Config;
	std::function<Engine::Vector2()> m_SelectSpawnCentre;
	std::function<double()> m_TimeSource;

	// What the keys looked like at the last RaiseInputEvents() call (observation), kept
	// separate from the intent the Input handler maintains (state).
	bool m_LeftHeldLastFrame = false;
	bool m_RightHeldLastFrame = false;

	bool m_RotateLeft = false;
	bool m_RotateRight = false;
	bool m_DeathRaisedThisFrame = false;
	ShipState m_State;

	// Declared last: destroyed first, unsubscribing while the EventManager is still alive.
	Engine::ScopedSubscription m_InputSubscription;
	Engine::ScopedSubscription m_CollisionSubscription;
	Engine::ScopedSubscription m_DeathSubscription;
	Engine::ScopedSubscription m_SpawnSubscription;
};

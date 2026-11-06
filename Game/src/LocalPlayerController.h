#pragma once

#include "Engine/Core/Core.h"
#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"

#include <functional>

// Milestone 4 Checkpoint 2: the local player's gameplay, driven by the Engine event system.
// Spare Parts-specific (not a generic Engine abstraction), and deliberately SDL-free and
// network-free so the whole event flow can be unit tested without constructing a Game
// (which owns a live NetworkClient worker thread). Game owns one of these and delegates the
// local player's input, landing, hazard and respawn logic to it; everything else (scene
// construction, enemy patrol, camera scrolling, networking, rendering) stays in Game.
//
// The four required event types and what they do here:
//
//   Input     MoveLeft / MoveRight / Jump actions. Held movement is a STATE (the *Active
//             flags below) that a MoveLeft/MoveRight event flips; a Jump event is a one-shot
//             effect (a jump, only if grounded). Game derives the events from held keys
//             (RaiseInputEvents()) and only raises one when a held action CHANGES.
//   Collision {player, other}: raised only on meaningful transitions — a new landing on a
//             platform, or overlap with the death zone or the client-local enemy — never for
//             a resting platform contact every frame. Its handler decides the consequence:
//             landing grants the grounded state; a hazard contact raises a Death event.
//   Death     {player, cause}: its handler picks the designated spawn position and requests
//             a Spawn. It does not itself move the player.
//   Spawn     {player, position}: its handler is the single place a (re)spawn takes effect:
//             position, zero velocity, not grounded, camera snapped to the spawn's screen.
//
// So a hazard touch is a real chain:  Collision -> Death -> Spawn  (all due at the same
// logical time, so one DispatchDue() call runs the whole chain).
//
// Death dedupe: at most ONE Death event is raised per frame (m_DeathRaisedThisFrame, reset
// by BeginFrame()), regardless of how many hazards touch the player or in what order the
// queued Collision events run. This is an explicit guard, not an ordering accident.
//
// Positional landing correction (snapping onto the platform, zeroing fall speed) is still
// direct, immediate physics resolution — only the grounded STATE change is event-handled.

/// Gameplay tunables the controller needs (owned by Game's top-level constants).
struct LocalPlayerConfig
{
	float MoveSpeed;  // px/s horizontal speed while a move action is active
	float JumpSpeed;  // px/s upward impulse applied by a grounded Jump
	float ViewWidth;  // width of one world screen, for the respawn camera snap
};

/// The runtime objects the controller acts on, all living in the caller's registry.
struct LocalPlayerObjects
{
	Engine::ObjectId Player;
	Engine::ObjectId Enemy;
	Engine::ObjectId DeathZone;
};

// Dispatch priorities, from most to least urgent for events due at the same logical time.
// Deliberately only a tie-breaker (Engine::EventManager orders by timestamp first): input
// applies first, a spawn takes effect before any other stale event queued alongside it.
constexpr Engine::EventPriority InputEventPriority = Engine::EventPriority::High;
constexpr Engine::EventPriority CollisionEventPriority = Engine::EventPriority::Normal;
constexpr Engine::EventPriority DeathEventPriority = Engine::EventPriority::High;
constexpr Engine::EventPriority SpawnEventPriority = Engine::EventPriority::Critical;

class LocalPlayerController
{
public:
	/// `events` and `objects` must outlive this controller (Game declares them first).
	/// `selectSpawnPosition` is asked for the player's designated spawn each time it dies.
	/// Subscribes this controller's handlers; they are removed when it is destroyed.
	LocalPlayerController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
	                      LocalPlayerObjects ids, LocalPlayerConfig config,
	                      std::function<Engine::Vector2()> selectSpawnPosition);

	// Handlers capture `this`, so the controller must never be copied or moved.
	LocalPlayerController(const LocalPlayerController&) = delete;
	LocalPlayerController& operator=(const LocalPlayerController&) = delete;

	/// Start of a frame: allows a new death to be raised (see class comment).
	void BeginFrame();

	/// Turns this frame's held movement keys and fresh jump press into Input events.
	/// A MoveLeft/MoveRight event is raised only when that held action CHANGED since the
	/// previous call; a Jump event is raised once per fresh press (`jumpPressed` must be an
	/// edge, not a held state).
	void RaiseInputEvents(bool leftHeld, bool rightHeld, bool jumpPressed);

	/// Applies the current movement intent to the player's horizontal velocity. Called every
	/// frame; velocity never depends on an event arriving that frame.
	void ApplyHorizontalMovement();

	/// Direct landing resolution: if the player is falling onto a platform, snaps it on top
	/// and zeroes the fall speed. Raises a Collision event only when this is a NEW landing
	/// (the player was not already grounded); marks the player un-grounded when it is not
	/// resting on anything.
	void ResolvePlatformLanding();

	/// Raises a Collision event for each hazard (death zone, client-local enemy) the player
	/// currently overlaps.
	void DetectHazardCollisions();

	/// Raises a Spawn event placing the player at `position` (used for the initial spawn;
	/// respawns after death request theirs through the Death handler).
	void RequestSpawn(Engine::Vector2 position);

	/// Replay playback gate (Milestone 4 Checkpoint 4). While on, this controller does not
	/// drive the live player: its Input/Collision/Death/Spawn handlers ignore the events they
	/// receive (replayed events are dispatched through the same EventManager but only OBSERVED,
	/// the recorded state samples own the player), and the per-frame input, movement, landing
	/// and hazard methods do nothing. Turning it off resumes normal gameplay unchanged.
	void SetPlaybackMode(bool playback);
	bool IsPlaybackMode() const;

	/// Overwrites the grounded state; used only to restore a saved state after a replay.
	void SetGrounded(bool grounded);

	/// True while the player is resting on a platform (landing grants it, a jump, leaving
	/// the platform, or a spawn clears it).
	bool IsGrounded() const;

	/// Presentation-only camera translation (world-space pixels); see Game.h.
	float CameraOffsetX() const;
	void SetCameraOffsetX(float offsetX);

private:
	void OnInput(const Engine::InputEventData& input);
	void OnCollision(const Engine::CollisionEventData& collision);
	void OnDeath(const Engine::DeathEventData& death);
	void OnSpawn(const Engine::SpawnEventData& spawn);

	void RaiseDeath(Engine::ObjectId cause);
	Engine::GameObject& Player();

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	LocalPlayerObjects m_Ids;
	LocalPlayerConfig m_Config;
	std::function<Engine::Vector2()> m_SelectSpawnPosition;

	// What the keys looked like at the last RaiseInputEvents() call (observation), kept
	// separate from the intent the Input handler maintains (state).
	bool m_LeftHeldLastFrame = false;
	bool m_RightHeldLastFrame = false;

	bool m_MoveLeftActive = false;
	bool m_MoveRightActive = false;
	bool m_IsGrounded = false;
	float m_CameraOffsetX = 0.0f;
	bool m_DeathRaisedThisFrame = false;
	bool m_PlaybackMode = false;

	// Declared last: destroyed first, unsubscribing while the EventManager is still alive.
	Engine::ScopedSubscription m_InputSubscription;
	Engine::ScopedSubscription m_CollisionSubscription;
	Engine::ScopedSubscription m_DeathSubscription;
	Engine::ScopedSubscription m_SpawnSubscription;
};

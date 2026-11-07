#include "LocalPlayerController.h"

#include "Engine/Collision/Collision.h"

#include "GameRules.h"

#include <utility>

LocalPlayerController::LocalPlayerController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                                             LocalPlayerObjects ids, LocalPlayerConfig config,
                                             std::function<Engine::Vector2()> selectSpawnPosition)
	: m_Events(events),
	  m_Objects(objects),
	  m_Ids(ids),
	  m_Config(config),
	  m_SelectSpawnPosition(std::move(selectSpawnPosition))
{
	m_InputSubscription = Engine::ScopedSubscription(
	    m_Events, Engine::EventType::Input,
	    [this](const Engine::Event& event) { OnInput(std::get<Engine::InputEventData>(event.Payload)); });
	m_CollisionSubscription = Engine::ScopedSubscription(
	    m_Events, Engine::EventType::Collision,
	    [this](const Engine::Event& event) { OnCollision(std::get<Engine::CollisionEventData>(event.Payload)); });
	m_DeathSubscription = Engine::ScopedSubscription(
	    m_Events, Engine::EventType::Death,
	    [this](const Engine::Event& event) { OnDeath(std::get<Engine::DeathEventData>(event.Payload)); });
	m_SpawnSubscription = Engine::ScopedSubscription(
	    m_Events, Engine::EventType::Spawn,
	    [this](const Engine::Event& event) { OnSpawn(std::get<Engine::SpawnEventData>(event.Payload)); });
}

Engine::GameObject& LocalPlayerController::Player()
{
	return *m_Objects.Find(m_Ids.Player);
}

void LocalPlayerController::BeginFrame()
{
	m_DeathRaisedThisFrame = false;
}

void LocalPlayerController::RaiseInputEvents(bool leftHeld, bool rightHeld, bool jumpPressed)
{
	if (m_PlaybackMode)
	{
		return; // live input must not reach a replayed player (and the held-key memory stays put)
	}

	if (leftHeld != m_LeftHeldLastFrame)
	{
		m_LeftHeldLastFrame = leftHeld;
		m_Events.Raise(Engine::InputEventData{ Engine::InputAction::MoveLeft, leftHeld }, InputEventPriority);
	}

	if (rightHeld != m_RightHeldLastFrame)
	{
		m_RightHeldLastFrame = rightHeld;
		m_Events.Raise(Engine::InputEventData{ Engine::InputAction::MoveRight, rightHeld }, InputEventPriority);
	}

	if (jumpPressed)
	{
		m_Events.Raise(Engine::InputEventData{ Engine::InputAction::ActionPrimary, true }, InputEventPriority);
	}
}

void LocalPlayerController::ApplyHorizontalMovement()
{
	if (m_PlaybackMode)
	{
		return;
	}

	Engine::GameObject& player = Player();
	Engine::Vector2 velocity = player.GetVelocity();

	if (m_MoveLeftActive && !m_MoveRightActive)
	{
		velocity.X = -m_Config.MoveSpeed;
	}
	else if (m_MoveRightActive && !m_MoveLeftActive)
	{
		velocity.X = m_Config.MoveSpeed;
	}
	else
	{
		velocity.X = 0.0f;
	}

	player.SetVelocity(velocity);
}

void LocalPlayerController::ResolvePlatformLanding()
{
	if (m_PlaybackMode)
	{
		return;
	}

	Engine::GameObject& player = Player();
	Engine::Vector2 velocity = player.GetVelocity();

	// Landing only ever happens while falling; moving upward (e.g. the frame right after a
	// jump) can never resolve as a landing, so the player is simply not grounded.
	if (velocity.Y > 0.0f)
	{
		for (auto& [id, object] : m_Objects)
		{
			if (object.GetType() != Engine::ObjectType::Platform || !Engine::IsColliding(player, object))
			{
				continue;
			}

			Engine::Vector2 position = player.GetPosition();
			position.Y = object.GetPosition().Y - player.GetSize().Y;
			player.SetPosition(position);

			velocity.Y = 0.0f;
			player.SetVelocity(velocity);

			// Resting on a platform re-lands every frame (gravity pushes the player into it
			// again); only a NEW landing is a gameplay collision worth an event. The
			// grounded state itself is granted by that event's handler.
			if (!m_IsGrounded)
			{
				m_Events.Raise(Engine::CollisionEventData{ m_Ids.Player, id }, CollisionEventPriority);
			}

			// Lands on the first colliding platform found; with no overlapping platforms by
			// design, this is deterministic. No moving-platform carrying (never existed).
			return;
		}
	}
	else if (velocity.Y == 0.0f && m_IsGrounded)
	{
		// Resting with no fall speed is NOT leaving the ground. The Timeline reads whole
		// milliseconds, so a fast frame can have deltaTime == 0: gravity adds nothing and the
		// player's fall speed is exactly 0. Clearing grounded here would make the next real
		// frame "land" again and re-raise the landing Collision every few frames (and make
		// jump gating flicker). Only upward speed (a jump) or a fall that finds no platform
		// under the player ends the grounded state.
		return;
	}

	m_IsGrounded = false;
}

void LocalPlayerController::DetectHazardCollisions()
{
	if (m_PlaybackMode)
	{
		return;
	}

	Engine::GameObject& player = Player();

	for (Engine::ObjectId hazardId : { m_Ids.DeathZone, m_Ids.Enemy })
	{
		const Engine::GameObject* hazard = m_Objects.Find(hazardId);
		if (hazard != nullptr && Engine::IsColliding(player, *hazard))
		{
			m_Events.Raise(Engine::CollisionEventData{ m_Ids.Player, hazardId }, CollisionEventPriority);
		}
	}
}

void LocalPlayerController::RequestSpawn(Engine::Vector2 position)
{
	m_Events.Raise(Engine::SpawnEventData{ m_Ids.Player, position }, SpawnEventPriority);
}

void LocalPlayerController::SetPlaybackMode(bool playback)
{
	m_PlaybackMode = playback;
}

bool LocalPlayerController::IsPlaybackMode() const
{
	return m_PlaybackMode;
}

void LocalPlayerController::SetGrounded(bool grounded)
{
	m_IsGrounded = grounded;
}

bool LocalPlayerController::IsGrounded() const
{
	return m_IsGrounded;
}

float LocalPlayerController::CameraOffsetX() const
{
	return m_CameraOffsetX;
}

void LocalPlayerController::SetCameraOffsetX(float offsetX)
{
	m_CameraOffsetX = offsetX;
}

void LocalPlayerController::OnInput(const Engine::InputEventData& input)
{
	if (m_PlaybackMode)
	{
		return; // replayed events are observed, never acted on live
	}

	switch (input.Action)
	{
	case Engine::InputAction::MoveLeft:
		m_MoveLeftActive = input.Pressed;
		break;

	case Engine::InputAction::MoveRight:
		m_MoveRightActive = input.Pressed;
		break;

	case Engine::InputAction::ActionPrimary:
		// Grounded-only, and consuming the grounded state: one jump per ground contact, so
		// two Jump events in one frame (e.g. Space and W together) still jump once.
		if (input.Pressed && m_IsGrounded)
		{
			Engine::GameObject& player = Player();
			Engine::Vector2 velocity = player.GetVelocity();
			velocity.Y = -m_Config.JumpSpeed;
			player.SetVelocity(velocity);
			m_IsGrounded = false;
		}
		break;
	}
}

void LocalPlayerController::OnCollision(const Engine::CollisionEventData& collision)
{
	if (m_PlaybackMode)
	{
		return; // replayed events are observed, never acted on live
	}

	Engine::ObjectId otherId = collision.A == m_Ids.Player ? collision.B : collision.A;
	const Engine::GameObject* other = m_Objects.Find(otherId);
	if (other == nullptr || (collision.A != m_Ids.Player && collision.B != m_Ids.Player))
	{
		return;
	}

	if (other->GetType() == Engine::ObjectType::Platform)
	{
		m_IsGrounded = true;
	}
	else if (other->GetType() == Engine::ObjectType::DeathZone || otherId == m_Ids.Enemy)
	{
		RaiseDeath(otherId);
	}
}

void LocalPlayerController::RaiseDeath(Engine::ObjectId cause)
{
	// The one explicit dedupe point: several hazards can touch the player in the same frame
	// (death zone AND enemy), but the player only dies once.
	if (m_DeathRaisedThisFrame)
	{
		return;
	}

	m_DeathRaisedThisFrame = true;
	m_Events.Raise(Engine::DeathEventData{ m_Ids.Player, cause }, DeathEventPriority);
}

void LocalPlayerController::OnDeath(const Engine::DeathEventData& death)
{
	if (m_PlaybackMode)
	{
		return; // replayed events are observed, never acted on live
	}

	if (death.Subject != m_Ids.Player)
	{
		return;
	}

	// Death only decides WHERE to respawn and asks for it; the Spawn handler applies it.
	RequestSpawn(m_SelectSpawnPosition());
}

void LocalPlayerController::OnSpawn(const Engine::SpawnEventData& spawn)
{
	if (m_PlaybackMode)
	{
		return; // replayed events are observed, never acted on live
	}

	if (spawn.Subject != m_Ids.Player)
	{
		return;
	}

	Engine::GameObject& player = Player();
	player.SetPosition(spawn.Position);
	player.SetVelocity({ 0.0f, 0.0f });
	m_IsGrounded = false;

	// UpdateScrolling() only moves the camera while the player touches the boundary, which
	// a freshly spawned player does not — so snap the camera to the spawn's own screen here
	// (the M3 respawn-camera fix, now living in the Spawn handler).
	m_CameraOffsetX = CameraOffsetForWorldX(spawn.Position.X, m_Config.ViewWidth);
}

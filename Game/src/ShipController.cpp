#include "ShipController.h"

#include <algorithm>
#include <limits>
#include <utility>

ShipController::ShipController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                               BulletField& bullets, Engine::ObjectId shipId, ShipConfig config,
                               std::function<Engine::Vector2()> selectSpawnCentre,
                               std::function<double()> timeSource)
	: m_Events(events),
	  m_Objects(objects),
	  m_Bullets(bullets),
	  m_ShipId(shipId),
	  m_Config(config),
	  m_SelectSpawnCentre(std::move(selectSpawnCentre)),
	  m_TimeSource(std::move(timeSource))
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

Engine::GameObject& ShipController::Ship()
{
	return *m_Objects.Find(m_ShipId);
}

void ShipController::BeginFrame()
{
	m_DeathRaisedThisFrame = false;
}

void ShipController::RaiseInputEvents(bool leftHeld, bool rightHeld, bool firePressed)
{
	if (m_PlaybackMode)
	{
		return; // live input must not reach a replayed ship (and the held-key memory stays put)
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

	if (firePressed)
	{
		m_Events.Raise(Engine::InputEventData{ Engine::InputAction::ActionPrimary, true }, InputEventPriority);
	}
}

void ShipController::ApplySteering(float deltaTime)
{
	if (m_PlaybackMode)
	{
		return;
	}

	Engine::GameObject& ship = Ship();

	if (!m_State.Alive)
	{
		ship.SetVelocity({ 0.0f, 0.0f });
		return;
	}

	// Heading grows clockwise on screen (see ArenaRules.h): right adds, left subtracts, and
	// holding both cancels out.
	float turn = (m_RotateRight ? 1.0f : 0.0f) - (m_RotateLeft ? 1.0f : 0.0f);
	m_State.HeadingRadians = NormalizeHeading(m_State.HeadingRadians + turn * m_Config.RotationSpeed * deltaTime);

	Engine::Vector2 forward = ForwardOf(m_State.HeadingRadians);
	ship.SetVelocity({ forward.X * m_Config.Speed, forward.Y * m_Config.Speed });
}

void ShipController::DetectSolidHits()
{
	// Nothing to do while dead, in playback, or protected: a wall is not consumed by touching it, so
	// an invulnerable ship simply passes through (spawn points are clear of every solid anyway).
	if (m_PlaybackMode || !m_State.Alive || IsInvulnerable())
	{
		return;
	}

	const Engine::Vector2 centre = CenterOf(Ship());
	for (const auto& [id, object] : m_Objects)
	{
		if (object.GetType() == Engine::ObjectType::Boundary &&
		    CircleVsRect(centre, m_Config.Radius, object.GetPosition(), object.GetSize()))
		{
			m_Events.Raise(Engine::CollisionEventData{ m_ShipId, id }, CollisionEventPriority);
		}
	}
}

void ShipController::DetectBulletHits()
{
	if (m_PlaybackMode || !m_State.Alive)
	{
		return;
	}

	Engine::Vector2 centre = CenterOf(Ship());
	for (Engine::ObjectId bulletId : m_Bullets.HitsOn(centre, m_Config.Radius, m_ShipId))
	{
		m_Events.Raise(Engine::CollisionEventData{ m_ShipId, bulletId }, CollisionEventPriority);
	}
}

void ShipController::SetShotListener(ShotListener listener)
{
	m_ShotListener = std::move(listener);
}

void ShipController::RequestSpawn(Engine::Vector2 centre)
{
	m_Events.Raise(Engine::SpawnEventData{ m_ShipId, centre }, SpawnEventPriority);
}

void ShipController::SetPlaybackMode(bool playback)
{
	m_PlaybackMode = playback;
}

bool ShipController::IsPlaybackMode() const
{
	return m_PlaybackMode;
}

ShipSnapshot ShipController::Capture() const
{
	const Engine::GameObject& ship = *m_Objects.Find(m_ShipId);
	const double now = m_TimeSource();

	// Remaining time, clamped at 0: a timer that already ran out is "complete", not negative.
	double cooldownRemaining = std::max(0.0, m_State.LastFireTime + m_Config.FireCooldown - now);
	double invulnerabilityRemaining = std::max(0.0, m_State.InvulnerableUntil - now);
	return ShipSnapshot{ ship.GetPosition(), ship.GetVelocity(), m_State.HeadingRadians, m_State.Alive,
		                 cooldownRemaining, invulnerabilityRemaining };
}

void ShipController::Restore(const ShipSnapshot& snapshot)
{
	Engine::GameObject& ship = Ship();
	ship.SetPosition(snapshot.Position);
	ship.SetVelocity(snapshot.Velocity);
	m_State.HeadingRadians = snapshot.HeadingRadians;
	m_State.Alive = snapshot.Alive;

	// Rebase the timers onto now. Zero remaining means "already complete" and is restored as
	// complete (-infinity), exactly like a ship that has never fired or never been protected.
	const double now = m_TimeSource();
	constexpr double Never = -std::numeric_limits<double>::infinity();
	m_State.LastFireTime = snapshot.CooldownRemaining > 0.0 ? now + snapshot.CooldownRemaining - m_Config.FireCooldown : Never;
	m_State.InvulnerableUntil = snapshot.InvulnerabilityRemaining > 0.0 ? now + snapshot.InvulnerabilityRemaining : Never;
}

void ShipController::ApplyReplayPose(Engine::Vector2 centre, Engine::Vector2 velocity, float headingRadians, bool alive)
{
	Engine::GameObject& ship = Ship();
	SetCenter(ship, centre);
	ship.SetVelocity(velocity);
	m_State.HeadingRadians = headingRadians;
	m_State.Alive = alive;
}

const ShipState& ShipController::State() const
{
	return m_State;
}

bool ShipController::IsAlive() const
{
	return m_State.Alive;
}

bool ShipController::IsInvulnerable() const
{
	return m_State.Alive && m_TimeSource() < m_State.InvulnerableUntil;
}

float ShipController::Heading() const
{
	return m_State.HeadingRadians;
}

void ShipController::OnInput(const Engine::InputEventData& input)
{
	if (m_PlaybackMode)
	{
		return; // replayed events are observed, never acted on live
	}

	switch (input.Action)
	{
	case Engine::InputAction::MoveLeft:
		m_RotateLeft = input.Pressed;
		break;

	case Engine::InputAction::MoveRight:
		m_RotateRight = input.Pressed;
		break;

	case Engine::InputAction::ActionPrimary:
		if (input.Pressed)
		{
			TryFire();
		}
		break;
	}
}

void ShipController::TryFire()
{
	if (!m_State.Alive)
	{
		return;
	}

	double now = m_TimeSource();
	if (now - m_State.LastFireTime < m_Config.FireCooldown)
	{
		return;
	}
	m_State.LastFireTime = now;

	Engine::Vector2 forward = ForwardOf(m_State.HeadingRadians);
	Engine::Vector2 centre = CenterOf(Ship());
	Engine::Vector2 muzzle{ centre.X + forward.X * m_Config.MuzzleDistance,
		                    centre.Y + forward.Y * m_Config.MuzzleDistance };
	m_Bullets.Fire(muzzle, m_State.HeadingRadians, m_ShipId);

	if (m_ShotListener)
	{
		m_ShotListener(muzzle, m_State.HeadingRadians);
	}
}

void ShipController::OnCollision(const Engine::CollisionEventData& collision)
{
	if (m_PlaybackMode)
	{
		return;
	}

	if (collision.A != m_ShipId && collision.B != m_ShipId)
	{
		return; // not about this ship
	}

	Engine::ObjectId otherId = collision.A == m_ShipId ? collision.B : collision.A;
	if (!m_State.Alive)
	{
		return; // a dead ship cannot be hit
	}

	// A wall or obstacle is lethal on contact — unless the ship is protected.
	const Engine::GameObject* other = m_Objects.Find(otherId);
	if (other != nullptr && other->GetType() == Engine::ObjectType::Boundary)
	{
		if (!IsInvulnerable())
		{
			RaiseDeath(otherId);
		}
		return;
	}

	if (!m_Bullets.Contains(otherId))
	{
		return; // not a bullet, or it is already gone
	}

	// The bullet is spent on contact, whether or not it hurts.
	m_Bullets.Remove(otherId);

	if (IsInvulnerable())
	{
		return;
	}

	RaiseDeath(otherId);
}

void ShipController::RaiseDeath(Engine::ObjectId cause)
{
	// The one explicit per-frame dedupe point: however many bullets hit in one frame, the
	// ship dies once.
	if (m_DeathRaisedThisFrame)
	{
		return;
	}

	m_DeathRaisedThisFrame = true;
	m_Events.Raise(Engine::DeathEventData{ m_ShipId, cause }, DeathEventPriority);
}

void ShipController::OnDeath(const Engine::DeathEventData& death)
{
	if (m_PlaybackMode)
	{
		return;
	}

	if (death.Subject != m_ShipId || !m_State.Alive)
	{
		return; // someone else's death, or this ship is already dead (never schedule two Spawns)
	}

	m_State.Alive = false;
	Ship().SetVelocity({ 0.0f, 0.0f });

	// Death only decides WHERE to come back and WHEN; the Spawn handler revives.
	m_Events.Schedule(Engine::SpawnEventData{ m_ShipId, m_SelectSpawnCentre() }, m_Config.RespawnDelay,
	                  SpawnEventPriority);
}

void ShipController::OnSpawn(const Engine::SpawnEventData& spawn)
{
	if (m_PlaybackMode)
	{
		return;
	}

	if (spawn.Subject != m_ShipId)
	{
		return;
	}

	Engine::GameObject& ship = Ship();
	SetCenter(ship, spawn.Position);

	m_State.HeadingRadians = NormalizeHeading(SpawnHeadingFor(spawn.Position));
	Engine::Vector2 forward = ForwardOf(m_State.HeadingRadians);
	ship.SetVelocity({ forward.X * m_Config.Speed, forward.Y * m_Config.Speed });

	m_State.Alive = true;
	m_State.InvulnerableUntil = m_TimeSource() + m_Config.InvulnerabilityDuration;
}

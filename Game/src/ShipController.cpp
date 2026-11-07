#include "ShipController.h"

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

void ShipController::WrapAroundArena()
{
	if (!m_State.Alive)
	{
		return;
	}

	Engine::GameObject& ship = Ship();
	SetCenter(ship, WrapShipCentre(CenterOf(ship), m_Config.Radius));
}

void ShipController::DetectBulletHits()
{
	if (!m_State.Alive)
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
	if (collision.A != m_ShipId && collision.B != m_ShipId)
	{
		return; // not about this ship
	}

	Engine::ObjectId otherId = collision.A == m_ShipId ? collision.B : collision.A;
	if (!m_State.Alive || !m_Bullets.Contains(otherId))
	{
		return; // a dead ship cannot be hit, and the bullet may already be gone
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
	if (spawn.Subject != m_ShipId)
	{
		return;
	}

	Engine::GameObject& ship = Ship();
	SetCenter(ship, spawn.Position);

	m_State.HeadingRadians = NormalizeHeading(HeadingTowardArenaCentre(spawn.Position));
	Engine::Vector2 forward = ForwardOf(m_State.HeadingRadians);
	ship.SetVelocity({ forward.X * m_Config.Speed, forward.Y * m_Config.Speed });

	m_State.Alive = true;
	m_State.InvulnerableUntil = m_TimeSource() + m_Config.InvulnerabilityDuration;
}

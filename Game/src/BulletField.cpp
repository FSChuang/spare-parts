#include "BulletField.h"

#include "ArenaRules.h"

#include <algorithm>
#include <cmath>

namespace
{
	// A bullet never moves further than this between two wall checks, so a slow frame cannot carry
	// it through a wall (the thinnest solid is 32 px; a 0.1 s frame at 720 px/s is 72 px).
	constexpr float MaxStepDistance = 16.0f;
}

BulletField::BulletField(Engine::GameObjectRegistry& objects, const Engine::PhysicsSystem& physics,
                         BulletConfig config)
	: m_Objects(objects),
	  m_Physics(physics),
	  m_Config(config)
{
}

Engine::ObjectId BulletField::Fire(Engine::Vector2 muzzleCentre, float headingRadians, Engine::ObjectId owner)
{
	const float size = 2.0f * m_Config.Radius;
	Engine::GameObject& object = m_Objects.Create(Engine::ObjectType::Generic, { 0.0f, 0.0f }, { size, size });
	SetCenter(object, muzzleCentre);

	Engine::Vector2 forward = ForwardOf(headingRadians);
	object.SetVelocity({ forward.X * m_Config.Speed, forward.Y * m_Config.Speed });

	m_Bullets.push_back({ object.GetId(), owner, 0.0f });
	return object.GetId();
}

bool BulletField::TouchesSolid(const Engine::GameObject& bullet) const
{
	const Engine::Vector2 centre = CenterOf(bullet);
	for (const auto& [id, other] : m_Objects)
	{
		if (other.GetType() == Engine::ObjectType::Boundary &&
		    CircleVsRect(centre, m_Config.Radius, other.GetPosition(), other.GetSize()))
		{
			return true;
		}
	}
	return false;
}

void BulletField::Update(float deltaTime)
{
	std::vector<bool> hitSolid(m_Bullets.size(), false);

	for (std::size_t i = 0; i < m_Bullets.size(); ++i)
	{
		Bullet& bullet = m_Bullets[i];
		Engine::GameObject* object = m_Objects.Find(bullet.Id);
		if (object != nullptr)
		{
			// Move in short steps, checking for a wall after each; a bullet that touches one is
			// spent right there.
			Engine::Vector2 velocity = object->GetVelocity();
			float distance = std::sqrt(velocity.X * velocity.X + velocity.Y * velocity.Y) * deltaTime;
			int steps = std::max(1, static_cast<int>(std::ceil(distance / MaxStepDistance)));
			for (int step = 0; step < steps && !hitSolid[i]; ++step)
			{
				m_Physics.Update(*object, deltaTime / static_cast<float>(steps));
				hitSolid[i] = TouchesSolid(*object);
			}
		}
		bullet.Age += deltaTime;
	}

	std::size_t index = 0;
	auto isFinished = [this, &hitSolid, &index](const Bullet& bullet)
	{
		const bool touchedSolid = hitSolid[index++];
		const Engine::GameObject* object = m_Objects.Find(bullet.Id);
		bool finished = object == nullptr || touchedSolid || bullet.Age >= m_Config.Lifetime ||
		                IsFullyOutsideArena(CenterOf(*object), m_Config.Radius);
		if (finished && object != nullptr)
		{
			m_Objects.Remove(bullet.Id);
		}
		return finished;
	};
	m_Bullets.erase(std::remove_if(m_Bullets.begin(), m_Bullets.end(), isFinished), m_Bullets.end());
}

std::vector<Engine::ObjectId> BulletField::HitsOn(Engine::Vector2 centre, float radius,
                                                  Engine::ObjectId excludedOwner) const
{
	std::vector<Engine::ObjectId> hits;
	for (const Bullet& bullet : m_Bullets)
	{
		const Engine::GameObject* object = m_Objects.Find(bullet.Id);
		if (object == nullptr || bullet.Owner == excludedOwner)
		{
			continue;
		}
		if (CirclesOverlap(centre, radius, CenterOf(*object), m_Config.Radius))
		{
			hits.push_back(bullet.Id);
		}
	}
	return hits;
}

bool BulletField::Contains(Engine::ObjectId bulletId) const
{
	return std::any_of(m_Bullets.begin(), m_Bullets.end(),
	                   [bulletId](const Bullet& bullet) { return bullet.Id == bulletId; });
}

bool BulletField::Remove(Engine::ObjectId bulletId)
{
	auto it = std::find_if(m_Bullets.begin(), m_Bullets.end(),
	                       [bulletId](const Bullet& bullet) { return bullet.Id == bulletId; });
	if (it == m_Bullets.end())
	{
		return false;
	}

	m_Objects.Remove(bulletId);
	m_Bullets.erase(it);
	return true;
}

const std::vector<Bullet>& BulletField::All() const
{
	return m_Bullets;
}

float BulletField::Radius() const
{
	return m_Config.Radius;
}

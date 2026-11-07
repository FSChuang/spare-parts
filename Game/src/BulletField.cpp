#include "BulletField.h"

#include "ArenaRules.h"

#include <algorithm>

namespace
{
	constexpr Engine::Color BulletColor{ 255, 240, 120, 255 };
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
	object.SetRender(BulletColor);

	m_Bullets.push_back({ object.GetId(), owner, 0.0f });
	return object.GetId();
}

void BulletField::Update(float deltaTime)
{
	for (Bullet& bullet : m_Bullets)
	{
		Engine::GameObject* object = m_Objects.Find(bullet.Id);
		if (object != nullptr)
		{
			m_Physics.Update(*object, deltaTime);
		}
		bullet.Age += deltaTime;
	}

	auto isFinished = [this](const Bullet& bullet)
	{
		const Engine::GameObject* object = m_Objects.Find(bullet.Id);
		bool finished = object == nullptr || bullet.Age >= m_Config.Lifetime ||
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

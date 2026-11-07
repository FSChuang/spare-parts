#pragma once

#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Physics/PhysicsSystem.h"

#include <vector>

// Astro rebuild Checkpoint B: the bullets in flight. Spare Parts-specific, SDL-free.
//
// A bullet is an ordinary Engine::GameObject (generic type, velocity, a render colour) living
// in the caller's registry, plus a tiny record kept here: who fired it and how old it is. No
// Engine::ObjectType::Bullet exists, and the Engine knows nothing about bullets.
//
// Bullets do not wrap: one expires when its lifetime runs out OR once it is entirely outside
// the arena. Lifetime is measured by summing the (Timeline-scaled) `deltaTime` of each
// Update(), so pausing or slowing the Timeline pauses or slows a bullet's life exactly as it
// slows its flight, and a zero-length frame changes nothing.
//
// The OWNER is the ObjectId of the firing ship, never a network id: a network id changes once
// the server assigns one, which would make a bullet fired just before that moment suddenly
// "belong to someone else". NoObjectId means "nobody's" (a bullet that can hit any ship).

struct BulletConfig
{
	float Speed;
	float Lifetime;
	float Radius;
};

struct Bullet
{
	Engine::ObjectId Id;
	Engine::ObjectId Owner;
	float Age;
};

class BulletField
{
public:
	/// `objects` and `physics` must outlive this. `physics` is the zero-gravity integrator
	/// the game uses for every moving thing.
	BulletField(Engine::GameObjectRegistry& objects, const Engine::PhysicsSystem& physics, BulletConfig config);

	/// Creates a bullet whose CENTRE is `muzzleCentre`, flying along `headingRadians` at the
	/// configured speed. Returns its object id.
	Engine::ObjectId Fire(Engine::Vector2 muzzleCentre, float headingRadians, Engine::ObjectId owner);

	/// Moves every bullet, ages it, and removes the ones that expired or left the arena.
	void Update(float deltaTime);

	/// Ids of the bullets overlapping the circle (centre, radius) that were NOT fired by
	/// `excludedOwner` — a ship is never hit by its own bullets.
	std::vector<Engine::ObjectId> HitsOn(Engine::Vector2 centre, float radius, Engine::ObjectId excludedOwner) const;

	bool Contains(Engine::ObjectId bulletId) const;
	/// Removes one bullet and its object (e.g. it hit something). False if it is not a bullet.
	bool Remove(Engine::ObjectId bulletId);

	const std::vector<Bullet>& All() const;
	float Radius() const;

private:
	Engine::GameObjectRegistry& m_Objects;
	const Engine::PhysicsSystem& m_Physics;
	BulletConfig m_Config;
	std::vector<Bullet> m_Bullets;
};

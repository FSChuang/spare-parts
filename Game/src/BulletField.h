#pragma once

#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Physics/PhysicsSystem.h"

#include <vector>

// The bullets in flight. Spare Parts-specific, SDL-free.
//
// A bullet is an ordinary Engine::GameObject (generic type, velocity) living
// in the caller's registry, plus a tiny record kept here: who fired it and how old it is. No
// Engine::ObjectType::Bullet exists, and the Engine knows nothing about bullets.
//
// A bullet is spent (removed, silently — it raises no event) when its lifetime runs out, when it
// touches a wall or obstacle (any registry object of type Boundary: the map is the same on every
// client, so a bullet rebuilt from a peer's Shoot is stopped by exactly the same geometry), or if
// it is somehow entirely outside the screen. It does not wrap or bounce. Lifetime is measured by summing the (Timeline-scaled)
// `deltaTime` of each Update(), so pausing or slowing the Timeline pauses or slows a bullet's life
// exactly as it slows its flight, and a zero-length frame changes nothing.
//
// OWNERSHIP (decided in the networked-combat checkpoint). A bullet's owner is the ObjectId of the
// ship object that fired it IN THIS CLIENT's registry, not a PlayerId:
//  - A client only ever asks one question: "is this bullet MINE?" (never hit my own ship), and
//    every client's own ship object id is fixed for the whole run. A PlayerId is not: the local
//    one is 0 until the server assigns it, so a bullet fired just before that would change
//    from "mine" to "somebody else's" and could shoot its own ship. An ObjectId cannot change.
//  - A bullet rebuilt from a peer's Shoot is owned by that peer's ship object here, or by
//    NoObjectId if the peer's state has not arrived yet. Either way it is not the local ship's,
//    so it can hit the local ship — which is the point.
//  - The PlayerId lives only at the wire boundary (the Shoot announcement and its receipt).
// NoObjectId means "nobody's" (a bullet that can hit any ship).

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
	bool TouchesSolid(const Engine::GameObject& bullet) const;

	Engine::GameObjectRegistry& m_Objects;
	const Engine::PhysicsSystem& m_Physics;
	BulletConfig m_Config;
	std::vector<Bullet> m_Bullets;
};

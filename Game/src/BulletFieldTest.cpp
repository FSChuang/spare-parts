// Tests for BulletField: creation, velocity from heading, movement, age/lifetime expiry,
// leaving the arena (no wrap), hit queries and owner exclusion. Real GameObjects and the real
// PhysicsSystem (zero gravity); no SDL, no sockets, no events. Expected values are written out.

#include "ArenaRules.h"
#include "BulletField.h"

#include "Engine/Event/Event.h"

#include <cmath>
#include <cstdio>

namespace
{
	int g_Failures = 0;

	void Check(bool condition, const char* name)
	{
		if (condition)
		{
			std::printf("[PASS] %s\n", name);
		}
		else
		{
			std::printf("[FAIL] %s\n", name);
			++g_Failures;
		}
	}

	bool Near(float a, float b, float tolerance = 1e-3f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	constexpr float Pi = 3.14159265358979323846f;

	// Slow bullets with a short life, so expiry by AGE can be tested without leaving the arena.
	constexpr BulletConfig SlowBullets{ 10.0f, 1.0f, 5.0f };

	struct World
	{
		explicit World(BulletConfig config = BulletConfig{ 200.0f, 1.4f, 5.0f })
			: Physics(0.0f),
			  Bullets(Objects, Physics, config)
		{
		}

		Engine::GameObjectRegistry Objects;
		Engine::PhysicsSystem Physics;
		BulletField Bullets;
	};
}

int main()
{
	// --- Fire ---
	{
		World world;
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 42);
		const Engine::GameObject* bullet = world.Objects.Find(id);
		Check(bullet != nullptr && world.Bullets.Contains(id) && world.Bullets.All().size() == 1, "Fire_CreatesOneBulletObject");
		Check(CenterOf(*bullet).X == 500.0f && CenterOf(*bullet).Y == 300.0f, "Fire_BulletCentreIsTheMuzzlePoint");
		Check(bullet->GetSize().X == 10.0f && bullet->GetSize().Y == 10.0f, "Fire_BulletSizeIsTwiceTheRadius");
		Check(bullet->GetVelocity().X == 200.0f && bullet->GetVelocity().Y == 0.0f, "Fire_HeadingZero_FliesRightAtBulletSpeed");
		Check(world.Bullets.All()[0].Owner == 42 && world.Bullets.All()[0].Age == 0.0f, "Fire_RecordsOwnerAndStartsAtAgeZero");
		Check(!bullet->HasNetworkId() && bullet->HasVelocity() && bullet->GetType() == Engine::ObjectType::Generic,
		      "Fire_BulletIsAnOrdinaryGenericObjectWithAVelocity");
	}
	{
		World world;
		Engine::ObjectId down = world.Bullets.Fire({ 500.0f, 300.0f }, Pi / 2.0f, 1);
		Engine::ObjectId diagonal = world.Bullets.Fire({ 500.0f, 300.0f }, Pi / 4.0f, 1);
		Check(Near(world.Objects.Find(down)->GetVelocity().X, 0.0f) && Near(world.Objects.Find(down)->GetVelocity().Y, 200.0f),
		      "Fire_QuarterTurn_FliesDown");
		Check(Near(world.Objects.Find(diagonal)->GetVelocity().X, 141.421f) &&
		          Near(world.Objects.Find(diagonal)->GetVelocity().Y, 141.421f),
		      "Fire_EighthTurn_FliesDiagonally_SpeedStaysConstant");
	}

	// --- Update: movement, zero dt ---
	{
		World world;
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.5f);
		Check(Near(CenterOf(*world.Objects.Find(id)).X, 600.0f) && Near(CenterOf(*world.Objects.Find(id)).Y, 300.0f),
		      "Update_MovesBulletByVelocityTimesDeltaTime"); // (moved in several short steps: equal up to rounding)
		Check(world.Bullets.All()[0].Age == 0.5f, "Update_AgesBulletByDeltaTime");

		world.Bullets.Update(0.0f);
		Check(Near(CenterOf(*world.Objects.Find(id)).X, 600.0f) && world.Bullets.All()[0].Age == 0.5f && world.Bullets.All().size() == 1,
		      "Update_ZeroDeltaTime_ChangesNothing");
	}

	// --- lifetime ---
	{
		World world(SlowBullets);
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.9f);
		Check(world.Bullets.Contains(id), "Lifetime_BeforeExpiry_BulletLives");
		world.Bullets.Update(0.2f);
		Check(!world.Bullets.Contains(id) && world.Objects.Find(id) == nullptr && world.Bullets.All().empty(),
		      "Lifetime_PastExpiry_BulletAndObjectAreRemoved");
	}
	{
		World world(SlowBullets);
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.5f);
		world.Bullets.Update(0.5f); // total exactly the lifetime
		Check(!world.Bullets.Contains(id), "Lifetime_ExactlyAtLifetime_Expires");
	}
	{
		// Pausing the Timeline gives deltaTime 0: a bullet's remaining life does not run down.
		World world(SlowBullets);
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		for (int i = 0; i < 1000; ++i)
		{
			world.Bullets.Update(0.0f);
		}
		Check(world.Bullets.Contains(id), "Lifetime_FrozenTime_NeverExpires");
	}

	// --- leaving the arena: bullets do not wrap ---
	{
		World world;
		Engine::ObjectId id = world.Bullets.Fire({ 1910.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.05f); // moves 10: centre 1920, still touching the arena
		Check(world.Bullets.Contains(id), "Arena_BulletAtTheEdge_StillExists");
		world.Bullets.Update(0.1f); // centre 1940: fully outside
		Check(!world.Bullets.Contains(id) && world.Objects.Find(id) == nullptr, "Arena_BulletFullyOutside_IsRemoved_NotWrapped");
	}

	// --- walls and obstacles: a bullet touching a Boundary object is spent, silently ---
	{
		World world;
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f }); // a wall x 600..632
		Engine::ObjectId toward = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);   // flies right into it
		Engine::ObjectId past = world.Bullets.Fire({ 500.0f, 600.0f }, 0.0f, 1);     // flies right below it
		world.Bullets.Update(0.3f); // 60 px: centre 560, edge 565: not touching yet
		Check(world.Bullets.Contains(toward) && world.Bullets.Contains(past), "Wall_BulletShortOfTheWall_Remains");
		world.Bullets.Update(0.1f); // centre 580.. 580+20=580: still short? 560+20 = 580, edge 585
		world.Bullets.Update(0.1f); // centre 600: touching the wall
		Check(!world.Bullets.Contains(toward) && world.Objects.Find(toward) == nullptr, "Wall_BulletHittingAWall_IsRemoved");
		Check(world.Bullets.Contains(past), "Wall_BulletThatMissesTheWall_Remains");
	}
	{
		World world;
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		world.Objects.Create(Engine::ObjectType::Generic, { 700.0f, 200.0f }, { 32.0f, 300.0f }); // NOT solid
		Engine::ObjectId bullet = world.Bullets.Fire({ 680.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.2f); // passes through the ordinary object
		Check(world.Bullets.Contains(bullet), "Wall_OnlyBoundaryObjectsStopBullets");
	}
	{
		World world; // an obstacle in the middle of the arena stops a bullet just the same
		world.Objects.Create(Engine::ObjectType::Boundary, { 900.0f, 500.0f }, { 180.0f, 180.0f });
		Engine::ObjectId bullet = world.Bullets.Fire({ 700.0f, 590.0f }, 0.0f, 1);
		for (int i = 0; i < 10 && world.Bullets.Contains(bullet); ++i)
		{
			world.Bullets.Update(0.1f);
		}
		Check(!world.Bullets.Contains(bullet), "Wall_BulletHittingAnObstacle_IsRemoved");
	}
	{
		World world; // bullets do not bounce or wrap: the one that hit is simply gone, the field is empty
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		world.Bullets.Fire({ 590.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.1f);
		Check(world.Bullets.All().empty() && world.Objects.Count() == 1, "Wall_NoBounceNoLeftoverObject");
	}
	{
		World world; // a slow frame must not carry a fast bullet THROUGH a thin wall
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		Engine::ObjectId bullet = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(1.0f); // 200 px in one frame: far past the 32 px wall
		Check(!world.Bullets.Contains(bullet), "Wall_OneHugeFrame_BulletDoesNotTunnelThroughTheWall");
	}
	{
		World world; // a bullet created inside a wall (e.g. firing with the nose against it) dies at once, even on a zero frame
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		Engine::ObjectId bullet = world.Bullets.Fire({ 610.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.0f);
		Check(!world.Bullets.Contains(bullet), "Wall_BulletCreatedInsideAWall_DiesEvenOnAZeroFrame");
	}
	{
		World world; // a bullet rebuilt from a peer's Shoot is owned by someone else but meets the same geometry
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		Engine::ObjectId remoteBullet = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, Engine::NoObjectId);
		world.Bullets.Update(0.6f);
		Check(!world.Bullets.Contains(remoteBullet), "Wall_AReconstructedBullet_StopsAtTheSameWall");
	}
	{
		World world; // a bullet far from every wall and obstacle is untouched by them (zero dt too)
		world.Objects.Create(Engine::ObjectType::Boundary, { 600.0f, 200.0f }, { 32.0f, 300.0f });
		Engine::ObjectId bullet = world.Bullets.Fire({ 300.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Update(0.0f);
		Check(world.Bullets.Contains(bullet), "Wall_NoContact_BulletRemains");
	}

	// --- several bullets are independent ---
	{
		World world(SlowBullets);
		Engine::ObjectId first = world.Bullets.Fire({ 100.0f, 100.0f }, 0.0f, 1);
		world.Bullets.Update(0.6f);
		Engine::ObjectId second = world.Bullets.Fire({ 100.0f, 200.0f }, 0.0f, 2);
		Engine::ObjectId third = world.Bullets.Fire({ 100.0f, 300.0f }, 0.0f, 3);
		Check(world.Bullets.All().size() == 3, "Multiple_ThreeBulletsInFlight");
		world.Bullets.Update(0.5f); // first is now 1.1 old: gone; the others are 0.5 old
		Check(!world.Bullets.Contains(first) && world.Bullets.Contains(second) && world.Bullets.Contains(third),
		      "Multiple_OnlyTheOldBulletExpires");
		Check(world.Bullets.All()[0].Owner == 2 && world.Bullets.All()[1].Owner == 3, "Multiple_OwnersKeptWithTheirBullets");
		Check(world.Objects.Count() == 2, "Multiple_NoLeakedObjectsInTheRegistry");
	}

	// --- hits ---
	{
		World world;
		Engine::ObjectId near = world.Bullets.Fire({ 520.0f, 300.0f }, 0.0f, 9);
		world.Bullets.Fire({ 700.0f, 300.0f }, 0.0f, 9);
		auto hits = world.Bullets.HitsOn({ 500.0f, 300.0f }, 24.0f, 1);
		Check(hits.size() == 1 && hits[0] == near, "Hits_OnlyTheOverlappingBulletIsReported");
		Check(world.Bullets.HitsOn({ 500.0f, 300.0f }, 24.0f, 9).empty(), "Hits_OwnersOwnBulletsNeverHitIt");

		// Tangent: ship radius 24 + bullet radius 5 = 29.
		world.Bullets.Fire({ 500.0f, 329.0f }, 0.0f, 9);
		world.Bullets.Fire({ 500.0f, 328.0f }, 0.0f, 8);
		auto edgeHits = world.Bullets.HitsOn({ 500.0f, 300.0f }, 24.0f, 1);
		Check(edgeHits.size() == 2, "Hits_TangentBulletMisses_BulletJustInsideHits"); // the 520 bullet + the 328 one
	}
	{
		World world;
		world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, Engine::NoObjectId);
		auto hits = world.Bullets.HitsOn({ 500.0f, 300.0f }, 24.0f, 1);
		Check(hits.size() == 1, "Hits_OwnerExclusion_StillHitByAnyoneElsesOrNobodysBullet");
	}

	// --- Remove ---
	{
		World world;
		Engine::ObjectId id = world.Bullets.Fire({ 500.0f, 300.0f }, 0.0f, 1);
		Check(world.Bullets.Remove(id) && !world.Bullets.Contains(id) && world.Objects.Find(id) == nullptr, "Remove_RemovesBulletAndObject");
		Check(!world.Bullets.Remove(id), "Remove_AlreadyGone_ReturnsFalse");
		Check(!world.Bullets.Remove(9999), "Remove_UnknownId_ReturnsFalse");
		Check(!world.Bullets.Contains(9999), "Contains_UnknownId_False");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

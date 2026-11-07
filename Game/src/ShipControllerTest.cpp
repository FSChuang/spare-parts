// Tests for ShipController: the local ship's event-driven gameplay (rotation, forward
// movement, firing and cooldown, the Collision -> Death -> delayed Spawn chain, spawn
// invulnerability, death dedupe). Uses the REAL controller, REAL Engine::EventManager on a fake
// clock, REAL GameObjects, the real BulletField and the real zero-gravity PhysicsSystem — so what
// is proven is the flow Game runs, not a re-implementation of it. No SDL, no sockets.
//
// The ship is configured with round, test-local numbers (speed 100, rotation 2 rad/s, cooldown
// 0.5 s, respawn 2 s, invulnerability 1 s) so every expectation is written out by hand.

#include "ArenaRules.h"
#include "BulletField.h"
#include "ShipController.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

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
	constexpr Engine::Vector2 ShipSize{ 48.0f, 48.0f };
	constexpr Engine::Vector2 FirstSpawn{ 480.0f, 540.0f };   // left of centre: spawn heading is exactly 0 (right)
	constexpr Engine::Vector2 SecondSpawn{ 1440.0f, 540.0f }; // right of centre: spawn heading points left

	constexpr ShipConfig TestConfig{ 100.0f, 2.0f, 24.0f, 36.0f, 0.5, 2.0, 1.0 };

	struct Scene
	{
		Scene()
			: Events([this] { return Now; }),
			  Physics(0.0f),
			  Bullets(Objects, Physics, BulletConfig{ 200.0f, 1.4f, 5.0f })
		{
			Engine::GameObject& ship = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(ship, { 100.0f, 100.0f });
			ship.SetVelocity({ 0.0f, 0.0f });
			ShipId = ship.GetId();
			OtherShipId = Objects.Create(Engine::ObjectType::Player, { 900.0f, 900.0f }, ShipSize).GetId();

			Controller = std::make_unique<ShipController>(
			    Events, Objects, Bullets, ShipId, TestConfig, [this] { return NextSpawn; }, [this] { return Now; });

			for (Engine::EventType type : { Engine::EventType::Input, Engine::EventType::Collision,
			                                 Engine::EventType::Death, Engine::EventType::Spawn })
			{
				Subscriptions.emplace_back(Events, type, [this](const Engine::Event& event) { Seen.push_back(event); });
			}
		}

		Engine::GameObject& Ship() { return *Objects.Find(ShipId); }
		Engine::Vector2 Centre() { return CenterOf(Ship()); }

		int Count(Engine::EventType type) const
		{
			int n = 0;
			for (const Engine::Event& event : Seen)
			{
				n += event.Type == type ? 1 : 0;
			}
			return n;
		}

		// Spawns the ship at FirstSpawn (heading 0), as Game does at start-up.
		void SpawnShip()
		{
			Controller->RequestSpawn(FirstSpawn);
			Events.DispatchDue();
		}

		// Spawns it and lets the spawn invulnerability run out: now an ordinary, killable ship.
		void SpawnVulnerableShip()
		{
			SpawnShip();
			Now += 1.0;
		}

		// One input phase: raise the events, then deliver them (Game's DispatchDue #1).
		void Input(bool left, bool right, bool fire)
		{
			Controller->BeginFrame();
			Controller->RaiseInputEvents(left, right, fire);
			Events.DispatchDue();
		}

		// A bullet nobody owns sitting on the ship; the hit itself is found by DetectHits().
		Engine::ObjectId DropHostileBulletOnShip()
		{
			return Bullets.Fire(Centre(), 0.0f, Engine::NoObjectId);
		}

		// Game's step 8: look for hits (raises Collision events only).
		void DetectHits()
		{
			Controller->BeginFrame();
			Controller->DetectBulletHits();
		}

		double Now = 0.0;
		Engine::Vector2 NextSpawn = SecondSpawn;
		Engine::EventManager Events;
		Engine::GameObjectRegistry Objects;
		Engine::PhysicsSystem Physics;
		BulletField Bullets;
		Engine::ObjectId ShipId = 0;
		Engine::ObjectId OtherShipId = 0;
		std::vector<Engine::Event> Seen;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<ShipController> Controller;
	};

	const Engine::InputEventData& InputOf(const Engine::Event& event)
	{
		return std::get<Engine::InputEventData>(event.Payload);
	}
}

int main()
{
	// ======== Spawn / alive state ========
	{
		Scene scene;
		Check(!scene.Controller->IsAlive(), "Spawn_ShipStartsDead");
		scene.Controller->RequestSpawn(FirstSpawn);
		Check(!scene.Controller->IsAlive() && scene.Events.PendingCount() == 1, "Spawn_RequestOnlyQueuesAnEvent");
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive(), "Spawn_Event_MakesTheShipAlive");
		Check(Near(scene.Centre().X, 480.0f) && Near(scene.Centre().Y, 540.0f), "Spawn_PlacesTheShipCentreAtThePosition");
		Check(Near(scene.Controller->Heading(), 0.0f), "Spawn_HeadingPointsTowardTheArenaCentre");
		Check(Near(scene.Ship().GetVelocity().X, 100.0f) && Near(scene.Ship().GetVelocity().Y, 0.0f),
		      "Spawn_SetsForwardVelocity");
		Check(scene.Controller->IsInvulnerable(), "Spawn_StartsTheInvulnerabilityWindow");

		scene.Controller->RequestSpawn(SecondSpawn);
		scene.Events.DispatchDue();
		Check(std::cos(scene.Controller->Heading()) < -0.99f, "Spawn_OnTheRight_HeadingPointsLeft");
	}
	{
		Scene scene; // a Spawn about someone else changes nothing here
		scene.Events.Raise(Engine::SpawnEventData{ scene.OtherShipId, FirstSpawn });
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive() && scene.Ship().GetVelocity().X == 0.0f, "Spawn_ForAnotherObject_Ignored");
	}

	// ======== Rotation and forward movement ========
	{
		Scene scene;
		scene.SpawnShip();
		scene.Input(false, false, false);
		scene.Controller->ApplySteering(0.5f);
		Check(Near(scene.Controller->Heading(), 0.0f), "Rotate_NoInput_HeadingUnchanged");
		Check(Near(scene.Ship().GetVelocity().X, 100.0f) && Near(scene.Ship().GetVelocity().Y, 0.0f),
		      "Rotate_NoInput_StillFliesForwardAtConstantSpeed");
	}
	{
		Scene scene;
		scene.SpawnShip();
		scene.Input(false, true, false);
		scene.Controller->ApplySteering(0.25f);
		Check(Near(scene.Controller->Heading(), 0.5f), "Rotate_Right_TurnsClockwise_PositiveHeading");
		Check(Near(scene.Ship().GetVelocity().X, 87.758f) && Near(scene.Ship().GetVelocity().Y, 47.943f),
		      "Rotate_Right_VelocityFollowsTheNewHeading");
	}
	{
		Scene scene;
		scene.SpawnShip();
		scene.Input(true, false, false);
		scene.Controller->ApplySteering(0.25f);
		Check(Near(scene.Controller->Heading(), -0.5f), "Rotate_Left_TurnsCounterClockwise_NegativeHeading");
		Check(scene.Ship().GetVelocity().Y < 0.0f, "Rotate_Left_ShipStartsHeadingUpTheScreen");
	}
	{
		Scene scene;
		scene.SpawnShip();
		scene.Input(true, true, false);
		scene.Controller->ApplySteering(0.25f);
		Check(Near(scene.Controller->Heading(), 0.0f), "Rotate_BothKeys_Cancel");
	}
	{
		Scene scene; // held rotation persists across frames until released
		scene.SpawnShip();
		scene.Input(false, true, false);
		scene.Controller->ApplySteering(0.1f);
		scene.Input(false, true, false); // still held: no new event
		scene.Controller->ApplySteering(0.1f);
		Check(Near(scene.Controller->Heading(), 0.4f), "Rotate_HeldKey_KeepsTurningEveryFrame");
		scene.Input(false, false, false); // released
		scene.Controller->ApplySteering(0.1f);
		Check(Near(scene.Controller->Heading(), 0.4f), "Rotate_ReleasedKey_StopsTurning");
	}
	{
		Scene scene; // input only takes effect through the EventManager
		scene.SpawnShip();
		scene.Controller->BeginFrame();
		scene.Controller->RaiseInputEvents(false, true, false);
		scene.Controller->ApplySteering(0.25f); // before DispatchDue
		Check(Near(scene.Controller->Heading(), 0.0f), "Input_IsAnEventNotADirectCall_NoEffectBeforeDispatch");
		scene.Events.DispatchDue();
		scene.Controller->ApplySteering(0.25f);
		Check(Near(scene.Controller->Heading(), 0.5f), "Input_TakesEffectAfterDispatch");
	}
	{
		Scene scene; // integrating the velocity moves the ship along its heading
		scene.SpawnShip();
		scene.Controller->ApplySteering(0.0f);
		scene.Physics.Update(scene.Ship(), 0.5f);
		Check(Near(scene.Centre().X, 530.0f) && Near(scene.Centre().Y, 540.0f), "Move_ForwardAtShipSpeed");
	}
	{
		Scene scene; // zero-length frame
		scene.SpawnShip();
		scene.Input(false, true, false);
		scene.Controller->ApplySteering(0.0f);
		scene.Physics.Update(scene.Ship(), 0.0f);
		Check(Near(scene.Controller->Heading(), 0.0f) && Near(scene.Centre().X, 480.0f), "ZeroDt_NoRotationAndNoMovement");
		Check(Near(scene.Ship().GetVelocity().X, 100.0f), "ZeroDt_VelocityStillValid");
	}
	{
		Scene scene; // the input event stream only changes on key CHANGES / fresh presses
		scene.SpawnShip();
		scene.Seen.clear();
		scene.Input(false, true, false);
		scene.Input(false, true, false);
		scene.Input(false, true, false);
		Check(scene.Count(Engine::EventType::Input) == 1 && InputOf(scene.Seen[0]).Action == Engine::InputAction::MoveRight &&
		          InputOf(scene.Seen[0]).Pressed,
		      "Input_HeldKey_RaisesOneEventNotOnePerFrame");
		scene.Input(false, false, false);
		Check(scene.Count(Engine::EventType::Input) == 2 && !InputOf(scene.Seen[1]).Pressed, "Input_Release_RaisesOneReleaseEvent");
		scene.Input(true, false, true);
		Check(scene.Count(Engine::EventType::Input) == 4, "Input_LeftPlusFire_RaiseTwoEvents");
	}

	// ======== Wrap-around through the controller ========
	{
		Scene scene;
		scene.SpawnShip();
		SetCenter(scene.Ship(), { -30.0f, 540.0f });
		scene.Controller->WrapAroundArena();
		Check(Near(scene.Centre().X, 1938.0f) && Near(scene.Centre().Y, 540.0f), "Wrap_ShipThatLeftTheLeftEdge_AppearsOnTheRight");
		SetCenter(scene.Ship(), { 960.0f, 540.0f });
		scene.Controller->WrapAroundArena();
		Check(Near(scene.Centre().X, 960.0f), "Wrap_ShipInsideTheArena_NotMoved");
	}

	// ======== Firing ========
	{
		Scene scene;
		scene.SpawnShip();
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 1, "Fire_ActionPrimary_CreatesABullet");
		const Bullet& bullet = scene.Bullets.All()[0];
		const Engine::GameObject* object = scene.Objects.Find(bullet.Id);
		Check(Near(CenterOf(*object).X, 516.0f) && Near(CenterOf(*object).Y, 540.0f),
		      "Fire_BulletAppearsAheadOfTheShipAtTheMuzzleDistance");
		Check(Near(object->GetVelocity().X, 200.0f) && Near(object->GetVelocity().Y, 0.0f), "Fire_BulletFliesAlongTheHeading");
		Check(bullet.Owner == scene.ShipId, "Fire_BulletIsOwnedByTheShip");
	}
	{
		Scene scene; // fire follows the CURRENT heading
		scene.SpawnShip();
		scene.Input(false, true, false);
		scene.Controller->ApplySteering(Pi / 4.0f); // 2 rad/s * pi/4 s = pi/2 rad: pointing down
		scene.Input(false, true, true);
		const Engine::GameObject* object = scene.Objects.Find(scene.Bullets.All()[0].Id);
		Check(Near(object->GetVelocity().X, 0.0f, 0.01f) && Near(object->GetVelocity().Y, 200.0f, 0.01f),
		      "Fire_AfterTurning_BulletFliesTheNewWay");
		Check(Near(CenterOf(*object).X, 480.0f, 0.01f) && Near(CenterOf(*object).Y, 576.0f, 0.01f),
		      "Fire_AfterTurning_MuzzleIsInFrontOfTheNewNose");
	}
	{
		Scene scene; // cooldown
		scene.SpawnShip();
		scene.Input(false, false, true);
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 1, "Cooldown_SecondShotInTheSameInstantBlocked");
		scene.Now += 0.25;
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 1, "Cooldown_HalfwayThrough_StillBlocked");
		scene.Now += 0.25; // exactly the cooldown
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 2, "Cooldown_ExactlyElapsed_ShotAllowed");
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 2, "Cooldown_RestartsAfterEachShot");
	}
	{
		Scene scene; // frozen time (a paused Timeline) never ends the cooldown
		scene.SpawnShip();
		scene.Input(false, false, true);
		for (int i = 0; i < 100; ++i)
		{
			scene.Input(false, false, true);
		}
		Check(scene.Bullets.All().size() == 1, "Cooldown_FrozenTime_NeverEnds");
	}
	{
		Scene scene; // a ship that has not spawned yet cannot fire
		scene.Input(false, false, true);
		Check(scene.Bullets.All().empty(), "Fire_BeforeTheFirstSpawn_Impossible");
	}

	// ======== Collision -> Death -> delayed Spawn ========
	{
		Scene scene;
		scene.SpawnVulnerableShip();
		scene.Seen.clear();
		Engine::ObjectId bullet = scene.DropHostileBulletOnShip();
		scene.DetectHits();
		Check(scene.Controller->IsAlive() && scene.Bullets.Contains(bullet) && scene.Events.PendingCount() == 1,
		      "Collision_Detection_ChangesNothingBeforeDispatch");
		Check(scene.Count(Engine::EventType::Death) == 0, "Collision_Detection_RaisesNoDeathDirectly");

		scene.Events.DispatchDue();
		Check(scene.Count(Engine::EventType::Collision) == 1 && scene.Count(Engine::EventType::Death) == 1,
		      "Chain_CollisionThenDeath_BothDispatched");
		Check(scene.Seen[0].Type == Engine::EventType::Collision && scene.Seen[1].Type == Engine::EventType::Death,
		      "Chain_CollisionIsHandledBeforeDeath");
		const auto& death = std::get<Engine::DeathEventData>(scene.Seen[1].Payload);
		Check(death.Subject == scene.ShipId && death.Cause == bullet, "Chain_DeathNamesTheShipAndTheBullet");
		Check(!scene.Controller->IsAlive(), "Death_MarksTheShipDead");
		Check(!scene.Bullets.Contains(bullet), "Collision_ConsumesTheBullet");
		Check(scene.Ship().GetVelocity().X == 0.0f && scene.Ship().GetVelocity().Y == 0.0f, "Death_StopsTheShip");
	}
	{
		Scene scene;
		scene.SpawnVulnerableShip();
		scene.DropHostileBulletOnShip();
		scene.DetectHits();
		scene.Events.DispatchDue();
		scene.Seen.clear();

		Check(scene.Count(Engine::EventType::Spawn) == 0 && scene.Events.PendingCount() == 1,
		      "Death_DoesNotRespawnImmediately_SpawnIsScheduled");
		scene.Now += 1.9;
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive() && scene.Count(Engine::EventType::Spawn) == 0, "Respawn_BeforeTheDelay_StillDead");
		scene.Now += 0.1; // total 2.0 s
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.Count(Engine::EventType::Spawn) == 1, "Respawn_AfterTheDelay_AliveAgain");
		Check(Near(scene.Centre().X, 1440.0f) && Near(scene.Centre().Y, 540.0f), "Respawn_AtTheSelectedSpawnPoint");
		Check(std::cos(scene.Controller->Heading()) < -0.99f, "Respawn_HeadingPointsIntoTheArena");
		Check(scene.Controller->IsInvulnerable(), "Respawn_StartsInvulnerable");
	}
	{
		Scene scene; // paused Timeline: the respawn never comes while time does not advance
		scene.SpawnVulnerableShip();
		scene.DropHostileBulletOnShip();
		scene.DetectHits();
		scene.Events.DispatchDue();
		for (int i = 0; i < 50; ++i)
		{
			scene.Events.DispatchDue();
		}
		Check(!scene.Controller->IsAlive(), "Respawn_FrozenTime_NeverArrives");
	}
	{
		Scene scene; // Spawn is the only revival path
		scene.SpawnVulnerableShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive(), "Revive_DeathAlone_KillsTheShip");
		scene.Controller->ApplySteering(0.1f);
		scene.Input(true, true, true);
		scene.Controller->WrapAroundArena();
		scene.Controller->DetectBulletHits();
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive(), "Revive_NothingButSpawnBringsAShipBack");
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive(), "Revive_TheScheduledSpawnDoes");
	}
	{
		Scene scene; // a second Death while already dead must not schedule a second Spawn
		scene.SpawnVulnerableShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		Check(scene.Events.PendingCount() == 1, "Death_Duplicate_SchedulesExactlyOneSpawn");
	}

	// ======== Dead ship ========
	{
		Scene scene;
		scene.SpawnVulnerableShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		Engine::Vector2 deathPlace = scene.Centre();

		scene.Input(false, false, true);
		Check(scene.Bullets.All().empty(), "Dead_CannotFire");

		scene.Input(false, true, false);
		scene.Controller->ApplySteering(0.5f);
		scene.Physics.Update(scene.Ship(), 0.5f);
		Check(scene.Centre().X == deathPlace.X && scene.Centre().Y == deathPlace.Y, "Dead_DoesNotMove");
		Check(Near(scene.Controller->Heading(), 0.0f), "Dead_DoesNotRotate");
		scene.Ship().SetVelocity({ 50.0f, 50.0f }); // stray velocity (e.g. left over from a network write)
		scene.Controller->ApplySteering(0.5f);
		Check(scene.Ship().GetVelocity().X == 0.0f && scene.Ship().GetVelocity().Y == 0.0f,
		      "Dead_SteeringForcesZeroVelocityEveryFrame");

		scene.DropHostileBulletOnShip();
		scene.DetectHits();
		Check(scene.Events.PendingCount() == 1, "Dead_CannotBeHitAgain_OnlyTheScheduledSpawnIsPending");
	}

	// ======== Invulnerability ========
	{
		Scene scene;
		scene.SpawnShip();
		Engine::ObjectId bullet = scene.DropHostileBulletOnShip();
		scene.DetectHits();
		scene.Seen.clear();
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.Count(Engine::EventType::Death) == 0, "Invulnerable_BlocksDeath");
		Check(!scene.Bullets.Contains(bullet), "Invulnerable_StillConsumesTheBullet");
		Check(scene.Count(Engine::EventType::Collision) == 1, "Invulnerable_TheCollisionStillHappens");
	}
	{
		Scene scene;
		scene.SpawnShip();
		scene.Now += 0.999;
		Check(scene.Controller->IsInvulnerable(), "Invulnerable_JustBeforeTheEnd_StillProtected");
		scene.Now += 0.001;
		Check(!scene.Controller->IsInvulnerable(), "Invulnerable_ExactlyAtTheEnd_NoLongerProtected");
		scene.DropHostileBulletOnShip();
		scene.DetectHits();
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive(), "Invulnerable_AfterTheWindow_DeathWorksAgain");
	}
	{
		Scene scene; // frozen time keeps the protection (a paused Timeline)
		scene.SpawnShip();
		for (int i = 0; i < 100; ++i)
		{
			scene.Events.DispatchDue();
		}
		Check(scene.Controller->IsInvulnerable(), "Invulnerable_FrozenTime_StaysProtected");
	}

	// ======== Death dedupe, ownership, other objects ========
	{
		Scene scene; // several bullets in one frame: one death, one respawn
		scene.SpawnVulnerableShip();
		scene.DropHostileBulletOnShip();
		scene.DropHostileBulletOnShip();
		scene.DropHostileBulletOnShip();
		scene.Seen.clear();
		scene.DetectHits();
		Check(scene.Events.PendingCount() == 3, "Dedupe_ThreeBulletsRaiseThreeCollisions");
		scene.Events.DispatchDue();
		Check(scene.Count(Engine::EventType::Death) == 1, "Dedupe_ThreeBulletsInOneFrame_ExactlyOneDeath");
		Check(scene.Events.PendingCount() == 1, "Dedupe_ThreeBulletsInOneFrame_ExactlyOneScheduledSpawn");
	}
	{
		Scene scene; // a ship is never hit by its own bullets
		scene.SpawnVulnerableShip();
		scene.Bullets.Fire(scene.Centre(), 0.0f, scene.ShipId);
		scene.DetectHits();
		Check(scene.Events.PendingCount() == 0, "Owner_OwnBulletOnTheShip_RaisesNoCollision");
	}
	{
		Scene scene; // events about other objects do not affect this ship
		scene.SpawnVulnerableShip();
		Engine::ObjectId bullet = scene.Bullets.Fire({ 900.0f, 900.0f }, 0.0f, Engine::NoObjectId);
		scene.Events.Raise(Engine::DeathEventData{ scene.OtherShipId, bullet });
		scene.Events.Raise(Engine::CollisionEventData{ scene.OtherShipId, bullet });
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.Events.PendingCount() == 0, "Foreign_DeathAndCollision_Ignored");
		Check(scene.Bullets.Contains(bullet), "Foreign_Collision_DoesNotConsumeSomeoneElsesBullet");
	}
	{
		Scene scene; // a collision with something that is not a bullet is harmless
		scene.SpawnVulnerableShip();
		scene.Events.Raise(Engine::CollisionEventData{ scene.ShipId, scene.OtherShipId });
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive(), "Collision_WithANonBullet_DoesNothing");
	}
	{
		Scene scene; // a ship can fight again after coming back
		scene.SpawnVulnerableShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		scene.Now += 1.0;
		scene.Input(false, false, true);
		Check(scene.Controller->IsAlive() && scene.Bullets.All().size() == 1, "Respawn_ShipCanFireAgain");
		scene.DropHostileBulletOnShip();
		scene.DetectHits();
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive(), "Respawn_ShipCanDieAgain");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

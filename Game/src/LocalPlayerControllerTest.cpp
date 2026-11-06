// Focused tests for LocalPlayerController (Milestone 4 Checkpoint 2): the local player's
// event-driven gameplay. Uses the REAL controller, a REAL Engine::EventManager driven by a
// fake clock, and REAL GameObjects — so what is proven here is the actual Input/Collision/
// Death/Spawn flow Game runs, not a re-implementation of it. Pure logic: no SDL, no sockets,
// no running server. Same manually invoked PASS/FAIL style as the other Game/src/*Test.cpp.

#include "LocalPlayerController.h"

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

	constexpr float MoveSpeed = 300.0f;
	constexpr float JumpSpeed = 650.0f;
	constexpr float ViewWidth = 1920.0f;
	constexpr Engine::Vector2 SpawnA{ 80.0f, 900.0f };
	constexpr Engine::Vector2 SpawnB{ 2150.0f, 750.0f };
	constexpr Engine::Vector2 FarAway{ 5000.0f, 0.0f };

	// One small world: a floor, a client-local enemy parked far away, a bottomless death zone
	// and the player, plus counters recording every event of each type the controller sees.
	struct Scene
	{
		Scene()
			: Events([this] { return Now; })
		{
			Engine::GameObject& player = Objects.Create(Engine::ObjectType::Player, { 100.0f, 100.0f }, { 50.0f, 50.0f });
			player.SetVelocity({ 0.0f, 0.0f });
			PlayerId = player.GetId();
			FloorId = Objects.Create(Engine::ObjectType::Platform, { 0.0f, 1000.0f }, { 850.0f, 40.0f }).GetId();
			EnemyId = Objects.Create(Engine::ObjectType::Generic, FarAway, { 50.0f, 50.0f }).GetId();
			DeathZoneId = Objects.Create(Engine::ObjectType::DeathZone, { 0.0f, 1080.0f }, { 3840.0f, 1000000.0f }).GetId();

			Controller = std::make_unique<LocalPlayerController>(
			    Events, Objects, LocalPlayerObjects{ PlayerId, EnemyId, DeathZoneId },
			    LocalPlayerConfig{ MoveSpeed, JumpSpeed, ViewWidth }, [this] { return NextSpawn; });

			Record(Engine::EventType::Input, SeenInput);
			Record(Engine::EventType::Collision, SeenCollision);
			Record(Engine::EventType::Death, SeenDeath);
			Record(Engine::EventType::Spawn, SeenSpawn);
		}

		void Record(Engine::EventType type, std::vector<Engine::Event>& into)
		{
			Subscriptions.emplace_back(Events, type, [&into](const Engine::Event& event) { into.push_back(event); });
		}

		Engine::GameObject& Player() { return *Objects.Find(PlayerId); }
		void PlacePlayer(float x, float y) { Player().SetPosition({ x, y }); }
		void PlaceEnemy(float x, float y) { Objects.Find(EnemyId)->SetPosition({ x, y }); }

		// One simulated frame's two dispatch phases, in Game::Update's order, minus physics
		// (tests set velocities/positions directly).
		void Frame(bool left, bool right, bool jump)
		{
			Controller->BeginFrame();
			Controller->RaiseInputEvents(left, right, jump);
			Events.DispatchDue();
			Controller->ApplyHorizontalMovement();
			Controller->ResolvePlatformLanding();
			Controller->DetectHazardCollisions();
			Events.DispatchDue();
		}

		// Drops the player just onto the floor and runs the landing through to a grounded state.
		void GroundThePlayer()
		{
			PlacePlayer(100.0f, 960.0f); // bottom edge 1010 overlaps the floor top at 1000
			Player().SetVelocity({ 0.0f, 100.0f });
			Controller->BeginFrame();
			Controller->ResolvePlatformLanding();
			Events.DispatchDue();
		}

		double Now = 0.0;
		Engine::EventManager Events;
		Engine::GameObjectRegistry Objects;
		Engine::ObjectId PlayerId = 0;
		Engine::ObjectId FloorId = 0;
		Engine::ObjectId EnemyId = 0;
		Engine::ObjectId DeathZoneId = 0;
		Engine::Vector2 NextSpawn = SpawnA;

		std::vector<Engine::Event> SeenInput;
		std::vector<Engine::Event> SeenCollision;
		std::vector<Engine::Event> SeenDeath;
		std::vector<Engine::Event> SeenSpawn;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<LocalPlayerController> Controller;
	};

	const Engine::InputEventData& InputOf(const Engine::Event& event)
	{
		return std::get<Engine::InputEventData>(event.Payload);
	}
}

int main()
{
	// 1 & 2. Held movement: one event per CHANGE of the held action, never one per frame.
	{
		Scene scene;
		scene.Frame(true, false, false);
		Check(scene.SeenInput.size() == 1 && InputOf(scene.SeenInput[0]).Action == Engine::InputAction::MoveLeft &&
		          InputOf(scene.SeenInput[0]).Pressed,
		      "Input_HoldLeft_RaisesOneMoveLeftPressed");
		Check(scene.Player().GetVelocity().X == -MoveSpeed, "Input_MoveLeftIntent_MovesPlayerLeft");

		for (int frame = 0; frame < 20; ++frame)
		{
			scene.Frame(true, false, false);
		}
		Check(scene.SeenInput.size() == 1 && scene.Player().GetVelocity().X == -MoveSpeed,
		      "Input_HoldLeftManyFrames_RaisesNoFurtherEvents_MovementContinues");

		scene.Frame(false, false, false);
		Check(scene.SeenInput.size() == 2 && !InputOf(scene.SeenInput[1]).Pressed &&
		          scene.Player().GetVelocity().X == 0.0f,
		      "Input_ReleaseLeft_RaisesOneReleasedEvent_StopsPlayer");
	}

	// Right mirrors left, and holding both cancels out (same as the pre-event behavior).
	{
		Scene scene;
		scene.Frame(false, true, false);
		Check(scene.SeenInput.size() == 1 && InputOf(scene.SeenInput[0]).Action == Engine::InputAction::MoveRight &&
		          scene.Player().GetVelocity().X == MoveSpeed,
		      "Input_HoldRight_RaisesMoveRightAndMovesPlayerRight");
		scene.Frame(true, true, false);
		Check(scene.SeenInput.size() == 2 && scene.Player().GetVelocity().X == 0.0f,
		      "Input_BothDirectionsHeld_CancelOut");
		scene.Frame(false, true, false);
		Check(scene.Player().GetVelocity().X == MoveSpeed, "Input_ReleaseLeftWhileRightHeld_ResumesRight");
	}

	// 3. Jump: one event per fresh press, grounded-only, no auto-jump while held.
	{
		Scene scene;
		scene.Player().SetVelocity({ 0.0f, 0.0f });
		scene.Frame(false, false, true);
		Check(scene.SeenInput.size() == 1 && InputOf(scene.SeenInput[0]).Action == Engine::InputAction::Jump,
		      "Jump_Press_RaisesOneJumpEvent");
		Check(scene.Player().GetVelocity().Y == 0.0f, "Jump_WhileNotGrounded_HasNoEffect");

		scene.GroundThePlayer();
		Check(scene.Controller->IsGrounded(), "Jump_SetupPlayerIsGrounded");
		scene.Frame(false, false, true);
		Check(scene.Player().GetVelocity().Y == -JumpSpeed && !scene.Controller->IsGrounded(),
		      "Jump_WhileGrounded_AppliesImpulseAndClearsGrounded");

		size_t jumpEventsAfterPress = scene.SeenInput.size();
		for (int frame = 0; frame < 10; ++frame)
		{
			scene.Frame(false, false, false); // key still held, but no fresh edge
		}
		Check(scene.SeenInput.size() == jumpEventsAfterPress, "Jump_HeldKey_RaisesNoFurtherEvents");

		scene.Player().SetVelocity({ 0.0f, -200.0f });
		scene.Frame(false, false, true);
		Check(scene.Player().GetVelocity().Y == -200.0f, "Jump_PressedMidAir_DoesNotJumpAgain");
	}

	// 4. Collision: a NEW landing raises exactly one Collision event whose handler grants
	// grounded; resting contact afterwards raises nothing; leaving the platform un-grounds.
	{
		Scene scene;
		scene.PlacePlayer(100.0f, 960.0f);
		scene.Player().SetVelocity({ 0.0f, 100.0f });
		scene.Controller->BeginFrame();
		scene.Controller->ResolvePlatformLanding();

		Check(scene.Player().GetPosition().Y == 950.0f && scene.Player().GetVelocity().Y == 0.0f,
		      "Landing_SnapsPlayerOntoPlatformDirectly");
		Check(scene.SeenCollision.empty() && !scene.Controller->IsGrounded() && scene.Events.PendingCount() == 1,
		      "Landing_RaisesCollisionButNothingHandledUntilDispatch");

		scene.Events.DispatchDue();
		const auto& landing = std::get<Engine::CollisionEventData>(scene.SeenCollision[0].Payload);
		Check(scene.SeenCollision.size() == 1 && landing.A == scene.PlayerId && landing.B == scene.FloorId,
		      "Landing_CollisionEventNamesPlayerAndPlatform");
		Check(scene.Controller->IsGrounded(), "Landing_CollisionHandlerGrantsGroundedState");

		for (int frame = 0; frame < 30; ++frame)
		{
			scene.Player().SetVelocity({ 0.0f, 16.0f }); // gravity pushes into the floor again
			scene.PlacePlayer(100.0f, 960.0f);
			scene.Controller->ResolvePlatformLanding();
			scene.Events.DispatchDue();
		}
		Check(scene.SeenCollision.size() == 1 && scene.Controller->IsGrounded(),
		      "Landing_RestingContact_DoesNotRaiseCollisionEveryFrame");

		scene.PlacePlayer(2000.0f, 500.0f);
		scene.Player().SetVelocity({ 0.0f, 10.0f });
		scene.Controller->ResolvePlatformLanding();
		Check(!scene.Controller->IsGrounded(), "Landing_LeavingPlatform_ClearsGrounded");
	}

	// 5. Death zone: one Collision, one Death (cause = the zone), one Spawn.
	{
		Scene scene;
		scene.PlacePlayer(100.0f, 1100.0f);
		scene.Player().SetVelocity({ 50.0f, 900.0f });
		scene.Frame(false, false, false);
		const auto& death = std::get<Engine::DeathEventData>(scene.SeenDeath[0].Payload);
		Check(scene.SeenDeath.size() == 1 && death.Subject == scene.PlayerId && death.Cause == scene.DeathZoneId,
		      "DeathZone_ProducesExactlyOneDeathEventWithZoneAsCause");
		Check(scene.SeenSpawn.size() == 1, "DeathZone_DeathCausesExactlyOneSpawn");
	}

	// 6. Enemy: one Death (cause = the enemy), one Spawn, no death-zone involvement.
	{
		Scene scene;
		scene.PlacePlayer(500.0f, 800.0f);
		scene.PlaceEnemy(520.0f, 810.0f);
		scene.Frame(false, false, false);
		const auto& death = std::get<Engine::DeathEventData>(scene.SeenDeath[0].Payload);
		Check(scene.SeenDeath.size() == 1 && death.Cause == scene.EnemyId, "Enemy_ProducesExactlyOneDeathEventWithEnemyAsCause");
		Check(scene.SeenSpawn.size() == 1, "Enemy_DeathCausesExactlyOneSpawn");
	}

	// 7. Death zone AND enemy in the same frame: two Collision events, still ONE effective death.
	{
		Scene scene;
		scene.PlacePlayer(500.0f, 1100.0f);
		scene.PlaceEnemy(520.0f, 1110.0f);
		scene.Frame(false, false, false);
		Check(scene.SeenCollision.size() == 2, "DoubleHazard_BothContactsAreDetectedAsCollisions");
		Check(scene.SeenDeath.size() == 1, "DoubleHazard_RaisesOnlyOneDeathEvent");
		Check(scene.SeenSpawn.size() == 1, "DoubleHazard_ProducesOnlyOneSpawn");

		// A new frame is a new chance to die: the guard is per frame, not permanent.
		scene.PlacePlayer(500.0f, 800.0f);
		scene.PlaceEnemy(520.0f, 810.0f);
		scene.Frame(false, false, false);
		Check(scene.SeenDeath.size() == 2 && scene.SeenSpawn.size() == 2, "DoubleHazard_NextFrameCanDieAgain");
	}

	// 8, 9 & 10. The chain: nothing happens until dispatch; ONE DispatchDue then completes
	// Collision -> Death -> Spawn with no event left pending, and the Spawn handler resets
	// position, velocity, grounded and camera.
	{
		Scene scene;
		scene.GroundThePlayer();
		scene.Controller->SetCameraOffsetX(ViewWidth);
		scene.NextSpawn = SpawnA;
		scene.PlacePlayer(100.0f, 1100.0f);
		scene.Player().SetVelocity({ 200.0f, 900.0f });

		scene.Controller->BeginFrame();
		scene.Controller->DetectHazardCollisions();
		Check(scene.SeenDeath.empty() && scene.Player().GetPosition().Y == 1100.0f,
		      "Chain_NothingHappensBeforeDispatch");

		std::size_t dispatched = scene.Events.DispatchDue();
		Check(scene.Events.PendingCount() == 0 && dispatched == 3,
		      "Chain_OneDispatchCompletesCollisionDeathSpawn");
		Check(scene.Player().GetPosition().X == SpawnA.X && scene.Player().GetPosition().Y == SpawnA.Y,
		      "Spawn_ResetsPosition");
		Check(scene.Player().GetVelocity().X == 0.0f && scene.Player().GetVelocity().Y == 0.0f, "Spawn_ZeroesVelocity");
		Check(!scene.Controller->IsGrounded(), "Spawn_ClearsGrounded");
		Check(scene.Controller->CameraOffsetX() == 0.0f, "Spawn_SnapsCameraToSpawnScreen_Screen0");
	}

	// Spawn on the second screen snaps the camera there; a Spawn for another object is ignored.
	{
		Scene scene;
		scene.NextSpawn = SpawnB;
		scene.PlacePlayer(100.0f, 1100.0f);
		scene.Frame(false, false, false);
		Check(scene.Controller->CameraOffsetX() == ViewWidth && scene.Player().GetPosition().X == SpawnB.X,
		      "Spawn_SnapsCameraToSpawnScreen_Screen1");

		scene.PlacePlayer(300.0f, 300.0f);
		scene.Events.Raise(Engine::SpawnEventData{ scene.EnemyId + 1000, SpawnA }, SpawnEventPriority);
		scene.Events.DispatchDue();
		Check(scene.Player().GetPosition().X == 300.0f, "Spawn_ForAnotherObject_IsIgnored");
	}

	// Initial spawn path: RequestSpawn is the same event, handled the same way.
	{
		Scene scene;
		scene.PlacePlayer(100.0f, 100.0f);
		scene.Controller->RequestSpawn(SpawnB);
		scene.Events.DispatchDue();
		Check(scene.SeenSpawn.size() == 1 && scene.Player().GetPosition().X == SpawnB.X &&
		          scene.Controller->CameraOffsetX() == ViewWidth,
		      "InitialSpawn_RequestSpawn_AppliesThroughSpawnEvent");
	}

	// A held move intent survives a respawn (the key is still down), exactly as before.
	{
		Scene scene;
		scene.Frame(true, false, false);
		scene.PlacePlayer(100.0f, 1100.0f);
		scene.Frame(true, false, false); // dies and respawns in this frame's second dispatch
		scene.Frame(true, false, false); // next frame: the unchanged intent applies again
		Check(scene.SeenDeath.size() == 1 && scene.SeenInput.size() == 1 &&
		          scene.Player().GetVelocity().X == -MoveSpeed,
		      "Respawn_HeldMoveIntent_PersistsWithoutNewInputEvent");
	}

	if (g_Failures == 0)
	{
		std::printf("All tests passed.\n");
		return 0;
	}

	std::printf("%d test(s) failed.\n", g_Failures);
	return 1;
}

// Tests for PeerEventBridge: the seam between network event notifications (Death, Spawn, Shoot)
// and the local game. Uses the REAL bridge, the REAL ShipController and BulletField, a REAL
// Engine::EventManager on a fake clock and REAL GameObjects, so what is proven is the actual
// authority model Game runs: each client decides only its OWN ship's death, a shooter only
// announces that it fired, and a remote player's life state follows its Death/Spawn events. No SDL,
// no sockets (the PeerClient transport itself is proven in PeerClientIntegrationTest).

#include "ArenaRules.h"
#include "BulletField.h"
#include "PeerEventBridge.h"
#include "ShipController.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

	// A precondition the rest of a test indexes into: on failure report it and stop, instead of
	// reading past the end of an empty vector.
	void Require(bool condition, const char* name)
	{
		Check(condition, name);
		if (!condition)
		{
			std::printf("Cannot continue: a precondition failed.\n");
			std::exit(1);
		}
	}

	bool Near(float a, float b, float tolerance = 1e-3f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	constexpr Engine::PlayerId LocalNetworkId = 1;
	constexpr Engine::PlayerId RemoteNetworkIdA = 7;
	constexpr Engine::PlayerId RemoteNetworkIdB = 9;
	constexpr Engine::Vector2 SpawnCentre{ 480.0f, 540.0f }; // spawn heading is exactly 0 (right)
	constexpr Engine::Vector2 ShipSize{ 48.0f, 48.0f };
	constexpr float BulletSpeed = 200.0f;
	constexpr float BulletLifetime = 1.4f;
	constexpr float BulletRadius = 5.0f;
	constexpr ShipConfig TestShip{ 100.0f, 2.0f, 24.0f, 36.0f, 0.5, 2.0, 1.0 };

	// A local ship (network id 1) and two remote players (7 and 9) in one registry, with the real
	// controller and bridge subscribed to the same EventManager exactly as Game wires them.
	struct Scene
	{
		Scene()
			: Events([this] { return Now; }),
			  Physics(0.0f),
			  Bullets(Objects, Physics, BulletConfig{ BulletSpeed, BulletLifetime, BulletRadius })
		{
			Engine::GameObject& ship = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(ship, SpawnCentre);
			ship.SetVelocity({ 11.0f, 22.0f });
			ship.SetNetworkId(LocalNetworkId);
			PlayerId = ship.GetId();

			RemoteAId = MakeRemote(RemoteNetworkIdA, { 1200.0f, 800.0f });
			RemoteBId = MakeRemote(RemoteNetworkIdB, { 1500.0f, 200.0f });
			// A wall (x 1100..1132, y 300..800): solid map geometry every client has.
			WallId = Objects.Create(Engine::ObjectType::Boundary, { 1100.0f, 300.0f }, { 32.0f, 500.0f }).GetId();

			Controller = std::make_unique<ShipController>(Events, Objects, Bullets, PlayerId, TestShip,
			                                              [] { return SpawnCentre; }, [this] { return Now; });
			Bridge = std::make_unique<PeerEventBridge>(Events, Objects, Bullets, PlayerId,
			                                          [this](const Engine::NetworkEvent& event) { Sent.push_back(event); });
			Controller->SetShotListener([this](Engine::Vector2 origin, float heading) { Bridge->AnnounceShot(origin, heading); });

			Subscriptions.emplace_back(Events, Engine::EventType::Death,
			                           [this](const Engine::Event& event) { SeenDeath.push_back(event); });
			Subscriptions.emplace_back(Events, Engine::EventType::Spawn,
			                           [this](const Engine::Event& event) { SeenSpawn.push_back(event); });
			Subscriptions.emplace_back(Events, Engine::EventType::Collision,
			                           [this](const Engine::Event& event) { SeenCollision.push_back(event); });
		}

		Engine::ObjectId MakeRemote(Engine::PlayerId networkId, Engine::Vector2 centre)
		{
			Engine::GameObject& remote = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(remote, centre);
			remote.SetNetworkId(networkId);
			return remote.GetId();
		}

		// Brings the local ship to life (announcing a Spawn), optionally lets its invulnerability
		// run out, then forgets everything announced or seen so each test sees only what it provokes.
		void StartLocalShip(bool vulnerable = true)
		{
			Controller->RequestSpawn(SpawnCentre);
			Events.DispatchDue();
			if (vulnerable)
			{
				Now += 1.0;
			}
			Sent.clear();
			SeenDeath.clear();
			SeenSpawn.clear();
			SeenCollision.clear();
		}

		void Input(bool left, bool right, bool fire)
		{
			Controller->BeginFrame();
			Controller->RaiseInputEvents(left, right, fire);
			Events.DispatchDue();
		}

		// What Game does after the bullets move: look for hits on the local ship, then dispatch.
		void HitDetectionFrame()
		{
			Controller->BeginFrame();
			Controller->DetectBulletHits();
			Events.DispatchDue();
		}

		Engine::GameObject& Ship() { return *Objects.Find(PlayerId); }

		static Engine::NetworkEvent DeathOf(Engine::PlayerId id) { return { Engine::NetworkEventKind::Death, id }; }
		static Engine::NetworkEvent SpawnOf(Engine::PlayerId id) { return { Engine::NetworkEventKind::Spawn, id }; }
		static Engine::NetworkEvent ShootBy(Engine::PlayerId id, float x, float y, float heading)
		{
			Engine::NetworkEvent shot{ Engine::NetworkEventKind::Shoot, id };
			shot.OriginX = x;
			shot.OriginY = y;
			shot.HeadingRadians = heading;
			return shot;
		}

		int SentOfKind(Engine::NetworkEventKind kind) const
		{
			int n = 0;
			for (const Engine::NetworkEvent& event : Sent)
			{
				n += event.Kind == kind ? 1 : 0;
			}
			return n;
		}

		double Now = 0.0;
		Engine::EventManager Events;
		Engine::GameObjectRegistry Objects;
		Engine::PhysicsSystem Physics;
		BulletField Bullets;
		Engine::ObjectId PlayerId = 0;
		Engine::ObjectId RemoteAId = 0;
		Engine::ObjectId RemoteBId = 0;
		Engine::ObjectId WallId = 0;

		std::vector<Engine::NetworkEvent> Sent;
		std::vector<Engine::Event> SeenDeath;
		std::vector<Engine::Event> SeenSpawn;
		std::vector<Engine::Event> SeenCollision;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<ShipController> Controller;
		std::unique_ptr<PeerEventBridge> Bridge;
	};
}

int main()
{
	// ======== Remote Death: resolution and local isolation ========
	{
		Scene scene;
		bool handled = scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		Require(handled && scene.SeenDeath.size() == 1, "RemoteDeath_RaisesExactlyOneDeathEvent");
		const auto& death = std::get<Engine::DeathEventData>(scene.SeenDeath[0].Payload);
		Check(death.Subject == scene.RemoteBId,
		      "RemoteDeath_NetworkPlayerId_ResolvesToMatchingRemoteObject");
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteBId) && scene.Bridge->IsRemoteAlive(scene.RemoteAId),
		      "RemoteDeath_OnlyTheNamedPlayerIsDead");
	}
	{
		Scene scene;
		scene.StartLocalShip();
		Engine::Vector2 positionBefore = scene.Ship().GetPosition();
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.Events.PendingCount() == 0 && scene.SeenSpawn.empty(),
		      "RemoteDeath_NeverKillsOrRespawnsTheLocalShip");
		Check(scene.Ship().GetPosition().X == positionBefore.X && scene.Ship().GetPosition().Y == positionBefore.Y,
		      "RemoteDeath_DoesNotMoveTheLocalShip");
		Check(scene.Sent.empty(), "RemoteDeath_IsNeverReBroadcast");
	}
	{
		Scene scene; // visible only once actually handled
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId) && scene.Events.PendingCount() == 1,
		      "RemoteDeath_QueuedNotHandledUntilDispatch");
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteAId), "RemoteDeath_AppliedOnDispatch");
	}

	// ======== Persistent remote death: only a Spawn ever brings a player back ========
	{
		Scene scene;
		scene.Now = 10.0;
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId), "Life_DiscoveredPeer_InitiallyAlive");
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteAId), "Life_Death_Dead");
		scene.Now = 11.5; // longer than any explosion
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteAId), "Life_LongAfterTheExplosion_StillDeadNotAliveAgain");
		scene.Now = 1000.0; // a very long time later: still no timer-driven revival
		scene.Bridge->PruneRemoved();
		for (int i = 0; i < 100; ++i)
		{
			scene.Events.DispatchDue();
		}
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteAId), "Life_NoTimerEverRevivesAPlayer");
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA)); // died again: still dead, still one entry
		scene.Events.DispatchDue();
		scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId), "Life_OneSpawnRevivesEvenAfterTwoDeaths");
	}
	{
		Scene scene; // the event is stamped with the receiver's logical time at raise, not dispatch time
		scene.Now = 42.0;
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Now = 50.0;
		scene.Events.DispatchDue();
		Check(scene.SeenDeath.size() == 1 && scene.SeenDeath[0].Timestamp == 42.0,
		      "Restamp_EventCarriesReceiversLogicalTimeAtRaise");
	}

	// ======== Remote Spawn ========
	{
		Scene scene;
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		bool handled = scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdA));
		Check(handled && !scene.Bridge->IsRemoteAlive(scene.RemoteAId), "RemoteSpawn_QueuedNotAppliedUntilDispatch");
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId), "RemoteSpawn_BringsThePlayerBack");
		Check(scene.SeenSpawn.size() == 1 && std::get<Engine::SpawnEventData>(scene.SeenSpawn[0].Payload).Subject == scene.RemoteAId,
		      "RemoteSpawn_IsASpawnEventAboutTheRemoteObject");
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteAId), "RemoteSpawn_AFurtherDeathKillsItAgain");
	}
	{
		Scene scene; // a Spawn for someone who is not dead is harmless
		scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId), "RemoteSpawn_ForALivePlayer_Harmless");
	}
	{
		Scene scene; // remote Spawn never respawns the local ship and is never re-broadcast
		scene.StartLocalShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.PlayerId, Engine::NoObjectId });
		scene.Events.DispatchDue(); // the local ship is dead, its own Spawn scheduled 2 s out
		scene.Sent.clear();
		scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive(), "RemoteSpawn_NeverRevivesTheLocalShip");
		Check(scene.Sent.empty(), "RemoteSpawn_IsNeverReBroadcast");
	}
	{
		Scene scene; // Death then Spawn, one after the other
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdB));
		scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteBId), "RemoteLife_DeathFollowedBySpawnInOneDispatch_EndsAlive");
	}

	// ======== Unknown, disconnected and looped-back notifications ========
	{
		Scene scene;
		Check(!scene.Bridge->HandleNetworkEvent(Scene::DeathOf(99)) && !scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(99)) &&
		          scene.Events.PendingCount() == 0,
		      "Ignored_DeathAndSpawnForAnUnknownPlayer_RaiseNothing");

		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		scene.Objects.Remove(scene.RemoteAId); // the peer disconnects
		scene.Bridge->PruneRemoved();
		Check(!scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdA)) &&
		          !scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(RemoteNetworkIdA)),
		      "Ignored_NotificationsFromADisconnectedPlayer");
		Engine::ObjectId rejoined = scene.MakeRemote(RemoteNetworkIdA, { 300.0f, 300.0f }); // same id comes back
		Check(scene.Bridge->IsRemoteAlive(rejoined), "Ignored_DisconnectedPlayersDeadFlagIsForgotten_RejoinIsAlive");

		// A queued notification whose player vanishes before dispatch is harmless too.
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdB));
		scene.Objects.Remove(scene.RemoteBId);
		scene.Events.DispatchDue();
		scene.Bridge->PruneRemoved();
		Engine::ObjectId rejoinedB = scene.MakeRemote(RemoteNetworkIdB, { 300.0f, 700.0f });
		Check(scene.Bridge->IsRemoteAlive(rejoinedB), "Ignored_PlayerRemovedBeforeDispatch_LeavesNoStaleDeadFlag");
	}
	{
		Scene scene; // our own id coming back must never be acted on
		Check(!scene.Bridge->HandleNetworkEvent(Scene::DeathOf(LocalNetworkId)) &&
		          !scene.Bridge->HandleNetworkEvent(Scene::SpawnOf(LocalNetworkId)) &&
		          !scene.Bridge->HandleNetworkEvent(Scene::ShootBy(LocalNetworkId, 600.0f, 300.0f, 0.0f)),
		      "SelfFilter_OwnDeathSpawnAndShootAreIgnored");
		Check(scene.Events.PendingCount() == 0 && scene.Bullets.All().empty(), "SelfFilter_NothingQueuedAndNoBulletMade");
	}

	// ======== Local announcements ========
	{
		Scene scene;
		scene.StartLocalShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.PlayerId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		Check(scene.Sent.size() == 1 && scene.Sent[0].Kind == Engine::NetworkEventKind::Death &&
		          scene.Sent[0].SourcePlayerId == LocalNetworkId,
		      "Announce_LocalDeath_SentExactlyOnceWithTheLocalId");
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		Check(scene.Sent.size() == 2 && scene.Sent[1].Kind == Engine::NetworkEventKind::Spawn &&
		          scene.Sent[1].SourcePlayerId == LocalNetworkId,
		      "Announce_LocalSpawn_SentExactlyOnceAfterTheDelay");
	}
	{
		Scene scene; // a successful shot: one bullet, one Shoot, with the real origin and heading
		scene.StartLocalShip();
		scene.Input(false, false, true);
		Check(scene.Bullets.All().size() == 1 && scene.SentOfKind(Engine::NetworkEventKind::Shoot) == 1 && scene.Sent.size() == 1,
		      "Announce_Fire_OneBulletAndExactlyOneShoot");
		Require(scene.Bullets.All().size() == 1 && scene.Sent.size() == 1, "Announce_FireProducedABulletAndAShoot");
		const Engine::GameObject* bullet = scene.Objects.Find(scene.Bullets.All()[0].Id);
		const Engine::NetworkEvent& shot = scene.Sent[0];
		Check(shot.SourcePlayerId == LocalNetworkId && Near(shot.OriginX, CenterOf(*bullet).X) && Near(shot.OriginY, CenterOf(*bullet).Y),
		      "Announce_Shoot_OriginIsTheActualBulletSpawnPoint");
		Check(Near(shot.OriginX, 516.0f) && Near(shot.OriginY, 540.0f) && Near(shot.HeadingRadians, scene.Controller->Heading()),
		      "Announce_Shoot_HeadingIsTheFiredHeading");
	}
	{
		Scene scene; // blocked shots announce nothing
		scene.StartLocalShip();
		scene.Input(false, false, true);
		scene.Input(false, false, true); // same instant: cooldown
		Check(scene.Bullets.All().size() == 1 && scene.Sent.size() == 1, "Announce_CooldownBlockedShot_NoSecondShoot");
		scene.Now += 0.2;
		scene.Input(false, false, true);
		Check(scene.Sent.size() == 1, "Announce_StillOnCooldown_NoShoot");
		scene.Now += 0.3;
		scene.Input(false, false, true);
		Check(scene.Sent.size() == 2 && scene.Bullets.All().size() == 2, "Announce_AfterTheCooldown_ShootAgain");
	}
	{
		Scene scene; // a dead ship, and one that has not spawned, announce no Shoot
		scene.Input(false, false, true);
		Check(scene.Sent.empty() && scene.Bullets.All().empty(), "Announce_BeforeTheFirstSpawn_NoShoot");
		scene.StartLocalShip();
		scene.Events.Raise(Engine::DeathEventData{ scene.PlayerId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		scene.Sent.clear();
		scene.Input(false, false, true);
		Check(scene.Sent.empty() && scene.Bullets.All().empty(), "Announce_DeadShip_NoShootAndNoBullet");
	}

	// ======== Replay gate ========
	{
		Scene scene;
		scene.StartLocalShip();
		scene.Bridge->SetAnnounceEnabled(false);
		scene.Input(false, false, true);
		scene.Events.Raise(Engine::DeathEventData{ scene.PlayerId, Engine::NoObjectId });
		scene.Events.DispatchDue();
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		Check(scene.Bullets.All().size() == 1 && scene.SeenDeath.size() == 1 && scene.SeenSpawn.size() == 1,
		      "ReplayGate_GameplayStillHappensLocally");
		Check(scene.Sent.empty(), "ReplayGate_Disabled_SuppressesShootDeathAndSpawn");

		scene.Bridge->SetAnnounceEnabled(true);
		scene.Now += 1.0;
		scene.Input(false, false, true);
		Check(scene.SentOfKind(Engine::NetworkEventKind::Shoot) == 1, "ReplayGate_Reenabled_AnnouncesAgain");
	}

	// ======== Remote Shoot: reconstruction ========
	{
		Scene scene;
		bool handled = scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 700.0f, 300.0f, 0.0f));
		Require(handled && scene.Bullets.All().size() == 1, "RemoteShoot_CreatesExactlyOneBullet");
		const Bullet& bullet = scene.Bullets.All()[0];
		const Engine::GameObject* object = scene.Objects.Find(bullet.Id);
		Check(Near(CenterOf(*object).X, 700.0f) && Near(CenterOf(*object).Y, 300.0f), "RemoteShoot_BulletStartsAtTheReceivedOrigin");
		Check(Near(object->GetVelocity().X, BulletSpeed) && Near(object->GetVelocity().Y, 0.0f),
		      "RemoteShoot_VelocityFollowsTheReceivedHeadingAtTheSharedSpeed");
		Check(bullet.Owner == scene.RemoteAId, "RemoteShoot_BulletIsOwnedByThatPlayersShip");
		Check(scene.Sent.empty() && scene.Events.PendingCount() == 0, "RemoteShoot_IsNotReBroadcastAndRaisesNoEvent");
	}
	{
		Scene scene; // heading is honoured, whatever it is
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 700.0f, 300.0f, 3.14159265f / 2.0f));
		Require(scene.Bullets.All().size() == 1, "RemoteShoot_HeadingTest_BulletExists");
		const Engine::GameObject* object = scene.Objects.Find(scene.Bullets.All()[0].Id);
		Check(Near(object->GetVelocity().X, 0.0f, 0.01f) && Near(object->GetVelocity().Y, BulletSpeed, 0.01f),
		      "RemoteShoot_HeadingQuarterTurn_FliesDown");
	}
	{
		Scene scene; // a shooter we have no object for yet still fires a real bullet, owned by nobody
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(55, 700.0f, 300.0f, 0.0f));
		Check(scene.Bullets.All().size() == 1 && scene.Bullets.All()[0].Owner == Engine::NoObjectId,
		      "RemoteShoot_FromAPlayerWhoseStateHasNotArrived_StillACounterBullet");
	}
	{
		Scene scene; // same speed and lifetime as a local bullet (they share one BulletField)
		scene.StartLocalShip();
		scene.Input(false, false, true);
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 1200.0f, 100.0f, 0.0f));
		Require(scene.Bullets.All().size() == 2, "RemoteShoot_SharedConstants_BothBulletsExist");
		const Engine::GameObject* local = scene.Objects.Find(scene.Bullets.All()[0].Id);
		const Engine::GameObject* remote = scene.Objects.Find(scene.Bullets.All()[1].Id);
		Check(Near(local->GetVelocity().X, remote->GetVelocity().X), "RemoteShoot_SameSpeedAsALocalBullet");
		scene.Bullets.Update(1.0f);
		Check(scene.Bullets.All().size() == 2, "RemoteShoot_AliveBeforeTheLifetime");
		scene.Bullets.Update(0.5f); // 1.5 s > 1.4 s lifetime
		Check(scene.Bullets.All().empty(), "RemoteShoot_ExpiresAfterTheSharedLifetime_LikeALocalOne");
	}
	{
		Scene scene; // zero-length frames neither move nor age a rebuilt bullet
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 700.0f, 300.0f, 0.0f));
		for (int i = 0; i < 1000; ++i)
		{
			scene.Bullets.Update(0.0f);
		}
		Require(scene.Bullets.All().size() == 1, "RemoteShoot_ZeroDt_BulletExists");
		const Bullet& bullet = scene.Bullets.All()[0];
		Check(bullet.Age == 0.0f && Near(CenterOf(*scene.Objects.Find(bullet.Id)).X, 700.0f),
		      "RemoteShoot_ZeroDt_BulletDoesNotMoveOrExpire");
	}
	{
		Scene scene; // a burst stays a burst, in order
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 100.0f, 300.0f, 0.0f));
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 200.0f, 300.0f, 0.0f));
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdB, 300.0f, 300.0f, 0.0f));
		bool inOrder = scene.Bullets.All().size() == 3;
		for (std::size_t i = 0; inOrder && i < 3; ++i)
		{
			inOrder = Near(CenterOf(*scene.Objects.Find(scene.Bullets.All()[i].Id)).X, 100.0f * static_cast<float>(i + 1));
		}
		Check(inOrder, "RemoteShoot_Burst_ThreeBulletsInArrivalOrder");
		Require(scene.Bullets.All().size() == 3, "RemoteShoot_Burst_AllThreeExist");
		Check(scene.Bullets.All()[2].Owner == scene.RemoteBId, "RemoteShoot_Burst_EachKeepsItsOwnShooter");
	}

	// ======== Authority: only the victim decides ========
	{
		Scene scene; // a local bullet over a remote ship's picture does nothing locally
		scene.StartLocalShip();
		Engine::ObjectId bullet = scene.Bullets.Fire(CenterOf(*scene.Objects.Find(scene.RemoteAId)), 0.0f, scene.PlayerId);
		scene.HitDetectionFrame();
		Check(scene.SeenCollision.empty() && scene.SeenDeath.empty(), "Authority_LocalBulletOnARemoteShip_RaisesNoCollisionOrDeath");
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteAId) && scene.Bullets.Contains(bullet),
		      "Authority_LocalBulletOnARemoteShip_DoesNotKillItAndPassesThrough");
		Check(scene.Sent.empty(), "Authority_LocalBulletOnARemoteShip_AnnouncesNothing");
	}
	{
		Scene scene; // handling a remote Shoot never declares anyone dead
		scene.StartLocalShip();
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 480.0f, 540.0f, 0.0f));
		scene.Events.DispatchDue();
		Check(scene.SeenDeath.empty() && scene.Bridge->IsRemoteAlive(scene.RemoteAId) && scene.Controller->IsAlive(),
		      "Authority_ReceivingAShoot_RaisesNoDeathForAnyone");
	}
	{
		Scene scene; // a rebuilt remote bullet reaching the local ship: Collision -> Death -> announced once -> Spawn announced
		scene.StartLocalShip();
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 480.0f, 540.0f, 0.0f));
		scene.Controller->BeginFrame();
		scene.Controller->DetectBulletHits();
		Check(scene.Controller->IsAlive() && scene.SeenDeath.empty() && scene.Events.PendingCount() == 1,
		      "Authority_RemoteBulletReachesTheShip_OnlyACollisionIsRaisedAtFirst");
		scene.Events.DispatchDue();
		Check(scene.SeenCollision.size() == 1 && scene.SeenDeath.size() == 1 && !scene.Controller->IsAlive(),
		      "Authority_RemoteBullet_CollisionThenLocalDeath");
		Require(scene.SeenDeath.size() == 1, "Authority_ADeathEventExists");
		const auto& death = std::get<Engine::DeathEventData>(scene.SeenDeath[0].Payload);
		Check(death.Subject == scene.PlayerId, "Authority_TheDeathIsTheLocalShipsOwn");
		Check(scene.Sent.size() == 1 && scene.Sent[0].Kind == Engine::NetworkEventKind::Death, "Authority_LocalDeathAnnouncedExactlyOnce");
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.SentOfKind(Engine::NetworkEventKind::Spawn) == 1 && scene.Sent.size() == 2,
		      "Authority_RespawnAnnouncedExactlyOnce_NothingElseSent");
	}
	{
		Scene scene; // a remote bullet from a shooter we don't know yet can still kill us
		scene.StartLocalShip();
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(55, 480.0f, 540.0f, 0.0f));
		scene.HitDetectionFrame();
		Check(!scene.Controller->IsAlive(), "Authority_BulletFromAnUnknownShooter_StillHitsTheLocalShip");
	}
	{
		Scene scene; // our own bullet never hits us, even right on top of us
		scene.StartLocalShip();
		scene.Input(false, false, true);
		Require(scene.Bullets.All().size() == 1, "Authority_OwnBullet_Exists");
		SetCenter(*scene.Objects.Find(scene.Bullets.All()[0].Id), CenterOf(scene.Ship()));
		scene.HitDetectionFrame();
		Check(scene.Controller->IsAlive() && scene.SeenCollision.empty(), "Authority_OwnBulletNeverHitsTheLocalShip");
	}
	{
		Scene scene; // spawn invulnerability: the bullet is spent, nobody dies, nothing is announced
		scene.StartLocalShip(false);
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 480.0f, 540.0f, 0.0f));
		scene.HitDetectionFrame();
		Check(scene.Controller->IsAlive() && scene.SeenDeath.empty() && scene.Bullets.All().empty() && scene.Sent.empty(),
		      "Authority_InvulnerableShip_BulletIsSpentWithoutADeathOrAnAnnouncement");
	}
	{
		Scene scene; // local and remote deaths in one dispatch each take their own path
		scene.StartLocalShip();
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 480.0f, 540.0f, 0.0f));
		scene.Controller->BeginFrame();
		scene.Controller->DetectBulletHits();
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive() && scene.SentOfKind(Engine::NetworkEventKind::Death) == 1 &&
		          !scene.Bridge->IsRemoteAlive(scene.RemoteBId) && scene.Bridge->IsRemoteAlive(scene.RemoteAId),
		      "Authority_LocalAndRemoteDeathInOneDispatch_EachHandledSeparately");
	}
	{
		Scene scene; // a Death about a non-networked object (a bullet) is neither tracked nor announced
		scene.StartLocalShip();
		Engine::ObjectId bullet = scene.Bullets.Fire({ 900.0f, 900.0f }, 0.0f, Engine::NoObjectId);
		scene.Events.Raise(Engine::DeathEventData{ bullet, Engine::NoObjectId });
		scene.Events.Raise(Engine::SpawnEventData{ bullet, { 900.0f, 900.0f } });
		scene.Events.DispatchDue();
		Check(scene.Sent.empty() && scene.Controller->IsAlive(), "NonNetworkedSubject_IsNeitherTrackedNorAnnounced");
	}

	// ======== Walls: the same geometry on every client ========
	{
		Scene scene; // a rebuilt remote bullet is stopped by the wall, raises no Death, and sends nothing
		scene.StartLocalShip();
		scene.Bridge->HandleNetworkEvent(Scene::ShootBy(RemoteNetworkIdA, 900.0f, 500.0f, 0.0f)); // flies right at the wall
		Require(scene.Bullets.All().size() == 1, "Map_RemoteBullet_Exists");
		scene.Bullets.Update(0.5f); // 100 px: x=1000, short of the wall
		Check(scene.Bullets.All().size() == 1, "Map_RemoteBullet_FliesUntilItMeetsTheWall");
		scene.Bullets.Update(0.5f); // x=1100: touching it
		Check(scene.Bullets.All().empty(), "Map_RemoteBullet_IsRemovedByTheWall");
		scene.HitDetectionFrame();
		Check(scene.SeenDeath.empty() && scene.SeenCollision.empty() && scene.Controller->IsAlive() && scene.Sent.empty(),
		      "Map_ABulletHittingAWall_RaisesNoDeathNoCollisionAndNoNetworkEvent");
	}
	{
		Scene scene; // a local ship flying into the wall dies through the normal chain and the death is announced once
		scene.StartLocalShip();
		SetCenter(scene.Ship(), { 1090.0f, 540.0f }); // overlapping the wall
		scene.Controller->BeginFrame();
		scene.Controller->DetectSolidHits();
		scene.Events.DispatchDue();
		Check(scene.SeenCollision.size() == 1 && scene.SeenDeath.size() == 1 && !scene.Controller->IsAlive(),
		      "Map_WallCollision_ThenLocalDeath");
		Check(scene.Sent.size() == 1 && scene.Sent[0].Kind == Engine::NetworkEventKind::Death && scene.Sent[0].SourcePlayerId == LocalNetworkId,
		      "Map_WallDeath_AnnouncedToPeersExactlyOnce");
		scene.Now += 2.0;
		scene.Events.DispatchDue();
		Check(scene.Controller->IsAlive() && scene.Sent.size() == 2 && scene.Sent[1].Kind == Engine::NetworkEventKind::Spawn,
		      "Map_WallDeath_ThenTheRespawnIsAnnounced");
	}
	{
		Scene scene; // a remote player's wall death (a Death notification) behaves like any other remote death
		scene.Bridge->HandleNetworkEvent(Scene::DeathOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteBId) && scene.Sent.empty(), "Map_RemoteWallDeath_ShowsDeadAndIsNotRebroadcast");
	}

	if (g_Failures == 0)
	{
		std::printf("All tests passed.\n");
		return 0;
	}

	std::printf("%d test(s) failed.\n", g_Failures);
	return 1;
}

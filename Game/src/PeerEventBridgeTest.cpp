// Focused tests for PeerEventBridge (Milestone 4 Checkpoint 3): the seam between received
// network event notifications and the local event system. Uses the REAL bridge, the REAL
// ShipController, a REAL Engine::EventManager on a fake clock and REAL GameObjects, so
// what is proven is the actual local-vs-remote Death separation Game runs. No SDL, no
// sockets (the PeerClient transport itself is proven in PeerClientIntegrationTest).

#include "ArenaRules.h"
#include "BulletField.h"
#include "ShipController.h"
#include "PeerEventBridge.h"

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

	constexpr Engine::PlayerId LocalNetworkId = 1;
	constexpr Engine::PlayerId RemoteNetworkIdA = 7;
	constexpr Engine::PlayerId RemoteNetworkIdB = 9;
	constexpr Engine::Vector2 SpawnCentre{ 400.0f, 300.0f };
	constexpr Engine::Vector2 ShipSize{ 48.0f, 48.0f };

	// A local ship (network id 1) and two remote players (7 and 9) in one registry, with the
	// real controller and bridge subscribed to the same EventManager exactly as Game wires them.
	struct Scene
	{
		Scene()
			: Events([this] { return Now; }),
			  Physics(0.0f),
			  Bullets(Objects, Physics, BulletConfig{ Astro::BulletSpeed, Astro::BulletLifetime, Astro::BulletRadius })
		{
			Engine::GameObject& ship = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(ship, SpawnCentre);
			ship.SetVelocity({ 11.0f, 22.0f });
			ship.SetNetworkId(LocalNetworkId);
			PlayerId = ship.GetId();

			RemoteAId = MakeRemote(RemoteNetworkIdA, 500.0f);
			RemoteBId = MakeRemote(RemoteNetworkIdB, 900.0f);

			Controller = std::make_unique<ShipController>(
			    Events, Objects, Bullets, PlayerId,
			    ShipConfig{ Astro::ShipSpeed, Astro::ShipRotationSpeed, Astro::ShipRadius, Astro::MuzzleDistance,
			                Astro::FireCooldown, Astro::RespawnDelay, Astro::InvulnerabilityDuration },
			    [] { return SpawnCentre; }, [this] { return Now; });
			Bridge = std::make_unique<PeerEventBridge>(Events, Objects, PlayerId, [this] { return Now; },
			                                          [this](Engine::NetworkEventKind kind) { SentToPeers.push_back(kind); });

			Subscriptions.emplace_back(Events, Engine::EventType::Death,
			                           [this](const Engine::Event& event) { SeenDeath.push_back(event); });
			Subscriptions.emplace_back(Events, Engine::EventType::Spawn,
			                           [this](const Engine::Event& event) { SeenSpawn.push_back(event); });
		}

		Engine::ObjectId MakeRemote(Engine::PlayerId networkId, float x)
		{
			Engine::GameObject& remote = Objects.Create(Engine::ObjectType::Player, { x, 800.0f }, ShipSize);
			remote.SetNetworkId(networkId);
			return remote.GetId();
		}

		// Brings the local ship to life and lets its spawn invulnerability run out, then forgets
		// the setup events so each test only sees what it provokes.
		void MakeShipVulnerable()
		{
			Controller->RequestSpawn(SpawnCentre);
			Events.DispatchDue();
			Now += Astro::InvulnerabilityDuration + 0.5;
			SeenSpawn.clear();
		}

		// A bullet nobody owns lands on the local ship through the real hit-detection path.
		void ShootLocalShip()
		{
			Bullets.Fire(CenterOf(Ship()), 0.0f, Engine::NoObjectId);
			Controller->BeginFrame();
			Controller->DetectBulletHits();
		}

		Engine::GameObject& Ship() { return *Objects.Find(PlayerId); }

		double Now = 0.0;
		Engine::EventManager Events;
		Engine::GameObjectRegistry Objects;
		Engine::PhysicsSystem Physics;
		BulletField Bullets;
		Engine::ObjectId PlayerId = 0;
		Engine::ObjectId RemoteAId = 0;
		Engine::ObjectId RemoteBId = 0;

		std::vector<Engine::NetworkEventKind> SentToPeers;
		std::vector<Engine::Event> SeenDeath;
		std::vector<Engine::Event> SeenSpawn;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<ShipController> Controller;
		std::unique_ptr<PeerEventBridge> Bridge;
	};

	Engine::NetworkEvent DeathOf(Engine::PlayerId id)
	{
		return Engine::NetworkEvent{ Engine::NetworkEventKind::Death, id };
	}
}

int main()
{
	// 1. The received network PlayerId resolves to the matching remote object — and only that one.
	{
		Scene scene;
		bool handled = scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		const auto& death = std::get<Engine::DeathEventData>(scene.SeenDeath[0].Payload);
		Check(handled && scene.SeenDeath.size() == 1 && death.Subject == scene.RemoteBId,
		      "Remote_NetworkPlayerId_ResolvesToMatchingRemoteObject");
		Check(scene.Bridge->IsMarkedDead(scene.RemoteBId) && !scene.Bridge->IsMarkedDead(scene.RemoteAId),
		      "Remote_OnlyTheNamedPlayerIsMarked");
	}

	// 2. A remote death never touches the local player, and is never echoed back out.
	{
		Scene scene;
		scene.MakeShipVulnerable();
		Engine::Vector2 positionBefore = scene.Ship().GetPosition();
		Engine::Vector2 velocityBefore = scene.Ship().GetVelocity();
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(scene.SeenSpawn.empty() && scene.Events.PendingCount() == 0, "Remote_Death_RaisesAndSchedulesNoSpawn");
		Check(scene.Ship().GetPosition().X == positionBefore.X && scene.Ship().GetPosition().Y == positionBefore.Y &&
		          scene.Ship().GetVelocity().X == velocityBefore.X && scene.Ship().GetVelocity().Y == velocityBefore.Y &&
		          scene.Controller->IsAlive(),
		      "Remote_Death_DoesNotKillOrMoveTheLocalShip");
		Check(scene.SentToPeers.empty(), "Remote_Death_IsNotReBroadcast");
	}

	// 3. The visible reaction only appears once the event is actually handled.
	{
		Scene scene;
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteAId) && scene.Events.PendingCount() == 1,
		      "Remote_Death_QueuedNotHandledUntilDispatch");
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsMarkedDead(scene.RemoteAId), "Remote_Death_ProducesVisibleMarkerOnDispatch");
	}

	// 4. The marker expires on logical time, from the receiver-local timestamp; it holds while
	// logical time is frozen (a paused Timeline) and PruneMarkers() only tidies it.
	{
		Scene scene;
		scene.Now = 10.0;
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		Check(scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_ActiveAtRaiseTime");
		scene.Now = 10.0 + PeerEventBridge::MarkDuration - 0.001;
		Check(scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_ActiveJustBeforeExpiry");
		scene.Now = 10.0 + PeerEventBridge::MarkDuration;
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_ExpiresAfterMarkDuration");
		scene.Bridge->PruneMarkers();
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_StaysExpiredAfterPrune");

		scene.Now = 20.0;
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		scene.Bridge->PruneMarkers();
		Check(scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_PruneKeepsAnActiveMarker");
		scene.Now = 20.5;
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		scene.Now = 21.2; // past the first marker's expiry, inside the refreshed one
		Check(scene.Bridge->IsMarkedDead(scene.RemoteAId), "Marker_AFurtherDeathRefreshesIt");
	}

	// 5. Unknown, disconnected, or self-referencing ids are ignored safely.
	{
		Scene scene;
		Check(!scene.Bridge->HandleNetworkEvent(DeathOf(99)) && scene.Events.PendingCount() == 0,
		      "Ignored_UnknownPlayerId_RaisesNothing");
		Check(!scene.Bridge->HandleNetworkEvent(DeathOf(LocalNetworkId)) && scene.Events.PendingCount() == 0,
		      "Ignored_EventClaimingToBeTheLocalPlayer_RaisesNothing");

		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Events.DispatchDue();
		scene.Objects.Remove(scene.RemoteAId); // the peer disconnects
		scene.Bridge->PruneMarkers();
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteAId), "Ignored_MarkerOfDisconnectedPlayer_IsCleanedUp");
		Check(!scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA)), "Ignored_DisconnectedPlayerId_RaisesNothing");

		// A queued notification whose player vanishes before dispatch is harmless too.
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdB));
		scene.Objects.Remove(scene.RemoteBId);
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteBId), "Ignored_PlayerRemovedBeforeDispatch_IsNotMarked");
	}

	// 6. A LOCAL death still runs Collision -> Death -> (delayed) Spawn, and is announced to peers once.
	{
		Scene scene;
		scene.MakeShipVulnerable();
		scene.ShootLocalShip();
		scene.Events.DispatchDue();
		Check(scene.SeenDeath.size() == 1 && !scene.Controller->IsAlive(),
		      "Local_Death_StillProducesExactlyOneDeath");
		Check(scene.SeenSpawn.empty() && scene.Events.PendingCount() == 1,
		      "Local_Death_SpawnIsScheduledNotImmediate");
		scene.Now += Astro::RespawnDelay;
		scene.Events.DispatchDue();
		Check(scene.SeenSpawn.size() == 1 && scene.Controller->IsAlive(), "Local_Death_StillRespawnsTheShipAfterTheDelay");
		Check(scene.SentToPeers.size() == 1 && scene.SentToPeers[0] == Engine::NetworkEventKind::Death,
		      "Local_Death_IsAnnouncedToPeersExactlyOnce");
		Check(!scene.Bridge->IsMarkedDead(scene.PlayerId) && !scene.Bridge->IsMarkedDead(scene.RemoteAId),
		      "Local_Death_MarksNobody");
	}

	// Local and remote deaths in the same dispatch each take their own path.
	{
		Scene scene;
		scene.MakeShipVulnerable();
		scene.ShootLocalShip();
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdB));
		scene.Events.DispatchDue();
		Check(!scene.Controller->IsAlive() && scene.SentToPeers.size() == 1 && scene.Bridge->IsMarkedDead(scene.RemoteBId),
		      "Mixed_LocalAndRemoteDeathInOneDispatch_EachHandledSeparately");
	}

	// A Death about a non-networked object (a bullet) is neither marked nor announced.
	{
		Scene scene;
		scene.MakeShipVulnerable();
		Engine::ObjectId bullet = scene.Bullets.Fire({ 900.0f, 900.0f }, 0.0f, Engine::NoObjectId);
		scene.Events.Raise(Engine::DeathEventData{ bullet, Engine::NoObjectId });
		scene.Events.DispatchDue();
		Check(!scene.Bridge->IsMarkedDead(bullet) && scene.SentToPeers.empty() && scene.Controller->IsAlive(),
		      "NonNetworkedDeath_IsNeitherMarkedNorAnnounced");
	}

	// 7. Re-stamping: the notification carries no timestamp at all, so the event is stamped with
	// the RECEIVER's logical time at the moment it is raised, whatever any sender's clock said.
	{
		Scene scene;
		scene.Now = 42.0;
		scene.Bridge->HandleNetworkEvent(DeathOf(RemoteNetworkIdA));
		scene.Now = 50.0; // later, when it is actually dispatched
		scene.Events.DispatchDue();
		Check(scene.SeenDeath.size() == 1 && scene.SeenDeath[0].Timestamp == 42.0,
		      "Restamp_EventCarriesReceiversLogicalTimeAtRaise");
		// The marker window is measured from that receiver-local timestamp, not from dispatch time.
		scene.Now = 42.0 + PeerEventBridge::MarkDuration + 0.5;
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteAId), "Restamp_MarkerMeasuredFromReceiverTimestamp");
	}

	if (g_Failures == 0)
	{
		std::printf("All tests passed.\n");
		return 0;
	}

	std::printf("%d test(s) failed.\n", g_Failures);
	return 1;
}

#include "PeerEventBridge.h"

#include "ArenaRules.h"

#include <utility>

PeerEventBridge::PeerEventBridge(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                                 BulletField& bullets, Engine::ObjectId localShipObjectId,
                                 std::function<double()> timeSource, NetworkSink sendToPeers)
	: m_Events(events),
	  m_Objects(objects),
	  m_Bullets(bullets),
	  m_LocalShipObjectId(localShipObjectId),
	  m_TimeSource(std::move(timeSource)),
	  m_SendToPeers(std::move(sendToPeers))
{
	m_DeathSubscription = Engine::ScopedSubscription(m_Events, Engine::EventType::Death,
	                                                  [this](const Engine::Event& event) { OnDeath(event); });
	m_SpawnSubscription = Engine::ScopedSubscription(m_Events, Engine::EventType::Spawn,
	                                                  [this](const Engine::Event& event) { OnSpawn(event); });
}

Engine::PlayerId PeerEventBridge::LocalNetworkId() const
{
	const Engine::GameObject* local = m_Objects.Find(m_LocalShipObjectId);
	return (local != nullptr && local->HasNetworkId()) ? local->GetNetworkId() : 0;
}

Engine::PlayerId PeerEventBridge::NetworkIdOf(Engine::ObjectId objectId) const
{
	const Engine::GameObject* object = m_Objects.Find(objectId);
	return (object != nullptr && object->HasNetworkId()) ? object->GetNetworkId() : 0;
}

bool PeerEventBridge::HandleNetworkEvent(const Engine::NetworkEvent& networkEvent)
{
	// Our own id coming back at us (it should never, the transport filters it) is not news.
	Engine::PlayerId localId = LocalNetworkId();
	if (localId != 0 && networkEvent.SourcePlayerId == localId)
	{
		return false;
	}

	if (networkEvent.Kind == Engine::NetworkEventKind::Shoot)
	{
		if (m_DiscardRemoteShoots)
		{
			return false; // drained by the caller, never rebuilt, never kept for later
		}

		// Rebuild the bullet; whom it hits is decided later, by the victim's own ship. Its owner
		// is that peer's ship object here when we know it, else nobody's — never the local ship.
		const Engine::GameObject* shooter = m_Objects.FindByNetworkId(networkEvent.SourcePlayerId);
		Engine::ObjectId owner = shooter != nullptr ? shooter->GetId() : Engine::NoObjectId;
		m_Bullets.Fire({ networkEvent.OriginX, networkEvent.OriginY }, networkEvent.HeadingRadians, owner);
		return true;
	}

	// Death / Spawn: the sender's PlayerId -> THIS client's own object for that player. Never the
	// sender's object id (it has none on the wire) and never the local ship's.
	const Engine::GameObject* remote = m_Objects.FindByNetworkId(networkEvent.SourcePlayerId);
	if (remote == nullptr || remote->GetId() == m_LocalShipObjectId)
	{
		return false;
	}

	// Raise() stamps these with the receiver's logical time.
	if (networkEvent.Kind == Engine::NetworkEventKind::Death)
	{
		m_Events.Raise(Engine::DeathEventData{ remote->GetId(), Engine::NoObjectId }, Engine::EventPriority::Normal);
		return true;
	}
	if (networkEvent.Kind == Engine::NetworkEventKind::Spawn)
	{
		m_Events.Raise(Engine::SpawnEventData{ remote->GetId(), CenterOf(*remote) }, Engine::EventPriority::Normal);
		return true;
	}
	return false;
}

void PeerEventBridge::AnnounceShot(Engine::Vector2 origin, float headingRadians)
{
	Engine::NetworkEvent shot{ Engine::NetworkEventKind::Shoot, LocalNetworkId() };
	shot.OriginX = origin.X;
	shot.OriginY = origin.Y;
	shot.HeadingRadians = headingRadians;
	Announce(shot);
}

void PeerEventBridge::Announce(const Engine::NetworkEvent& event)
{
	if (m_AnnounceEnabled)
	{
		m_SendToPeers(event);
	}
}

void PeerEventBridge::OnDeath(const Engine::Event& event)
{
	const auto& death = std::get<Engine::DeathEventData>(event.Payload);

	if (death.Subject == m_LocalShipObjectId)
	{
		// The local ship died (ShipController is scheduling the respawn): tell peers.
		Announce({ Engine::NetworkEventKind::Death, LocalNetworkId() });
		return;
	}

	// A Death about a remote player's object: it stays dead until its Spawn arrives. Anything that
	// is not a networked player object (a bullet, a stale id) is not ours to track.
	Engine::PlayerId playerId = NetworkIdOf(death.Subject);
	if (playerId != 0)
	{
		m_DeadSince[playerId] = event.Timestamp;
	}
}

void PeerEventBridge::OnSpawn(const Engine::Event& event)
{
	const auto& spawn = std::get<Engine::SpawnEventData>(event.Payload);

	if (spawn.Subject == m_LocalShipObjectId)
	{
		// The local ship came (back) to life: tell peers so they show it again.
		Announce({ Engine::NetworkEventKind::Spawn, LocalNetworkId() });
		return;
	}

	Engine::PlayerId playerId = NetworkIdOf(spawn.Subject);
	if (playerId != 0)
	{
		m_DeadSince.erase(playerId);
	}
}

void PeerEventBridge::SetAnnounceEnabled(bool enabled)
{
	m_AnnounceEnabled = enabled;
}

void PeerEventBridge::SetRemoteShootsDiscarded(bool discarded)
{
	m_DiscardRemoteShoots = discarded;
}

bool PeerEventBridge::IsRemoteAlive(Engine::ObjectId remoteObjectId) const
{
	return m_DeadSince.find(NetworkIdOf(remoteObjectId)) == m_DeadSince.end();
}

bool PeerEventBridge::IsDeathMarkerActive(Engine::ObjectId remoteObjectId) const
{
	auto it = m_DeadSince.find(NetworkIdOf(remoteObjectId));
	return it != m_DeadSince.end() && m_TimeSource() < it->second + MarkDuration;
}

void PeerEventBridge::PruneRemoved()
{
	for (auto it = m_DeadSince.begin(); it != m_DeadSince.end();)
	{
		it = m_Objects.FindByNetworkId(it->first) == nullptr ? m_DeadSince.erase(it) : std::next(it);
	}
}

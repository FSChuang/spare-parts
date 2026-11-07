#include "PeerEventBridge.h"

#include <utility>

PeerEventBridge::PeerEventBridge(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                                 Engine::ObjectId localPlayerObjectId, std::function<double()> timeSource,
                                 std::function<void(Engine::NetworkEventKind)> sendToPeers)
	: m_Events(events),
	  m_Objects(objects),
	  m_LocalPlayerObjectId(localPlayerObjectId),
	  m_TimeSource(std::move(timeSource)),
	  m_SendToPeers(std::move(sendToPeers))
{
	m_DeathSubscription = Engine::ScopedSubscription(m_Events, Engine::EventType::Death,
	                                                  [this](const Engine::Event& event) { OnDeath(event); });
}

bool PeerEventBridge::HandleNetworkEvent(const Engine::NetworkEvent& networkEvent)
{
	// Only one kind exists so far; a future kind adds its own case here.
	if (networkEvent.Kind != Engine::NetworkEventKind::Death)
	{
		return false;
	}

	// The sender's PlayerId -> THIS client's own object for that player. Never the sender's
	// object id (it has none on the wire) and never the local player's own object.
	const Engine::GameObject* remote = m_Objects.FindByNetworkId(networkEvent.SourcePlayerId);
	if (remote == nullptr || remote->GetId() == m_LocalPlayerObjectId)
	{
		return false;
	}

	// Raise() stamps this with the receiver's logical time.
	m_Events.Raise(Engine::DeathEventData{ remote->GetId(), Engine::NoObjectId },
	               Engine::EventPriority::Normal);
	return true;
}

void PeerEventBridge::OnDeath(const Engine::Event& event)
{
	const auto& death = std::get<Engine::DeathEventData>(event.Payload);

	if (death.Subject == m_LocalPlayerObjectId)
	{
		// The local player died (ShipController is handling the respawn): tell peers.
		if (m_AnnounceEnabled)
		{
			m_SendToPeers(Engine::NetworkEventKind::Death);
		}
		return;
	}

	// A Death about a remote player's object: show it. Anything that is not a networked
	// player object (a bullet, a stale id) is not ours to mark.
	const Engine::GameObject* subject = m_Objects.Find(death.Subject);
	if (subject != nullptr && subject->HasNetworkId())
	{
		m_MarkerExpiry[death.Subject] = event.Timestamp + MarkDuration;
	}
}

void PeerEventBridge::SetAnnounceEnabled(bool enabled)
{
	m_AnnounceEnabled = enabled;
}

bool PeerEventBridge::IsMarkedDead(Engine::ObjectId remoteObjectId) const
{
	auto it = m_MarkerExpiry.find(remoteObjectId);
	return it != m_MarkerExpiry.end() && m_TimeSource() < it->second;
}

void PeerEventBridge::PruneMarkers()
{
	const double now = m_TimeSource();
	for (auto it = m_MarkerExpiry.begin(); it != m_MarkerExpiry.end();)
	{
		bool expired = now >= it->second;
		bool objectGone = m_Objects.Find(it->first) == nullptr;
		it = (expired || objectGone) ? m_MarkerExpiry.erase(it) : std::next(it);
	}
}

#pragma once

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/EventProtocol.h"

#include <functional>
#include <unordered_map>

// Milestone 4 Checkpoint 3: the explicit seam between the network and the local event system
// for gameplay event notifications (Spare Parts-specific, SDL-free and socket-free, so it is
// unit tested without a Game or a real PeerClient). Neither side knows about the other:
// Engine::EventManager is not network-aware and PeerClient has no EventManager. This class,
// owned by Game and used only on the main thread, is the only place they meet.
//
// LOCAL -> NETWORK. When a Death event for the LOCAL player is dispatched (the same event
// ShipController turns into a scheduled respawn), this bridge also hands a Death notification
// to `sendToPeers` (Game wires it to PeerClient::PublishEvent). A Death event about any
// other object is never re-broadcast, so a received notification can never echo back out.
//
// NETWORK -> LOCAL. HandleNetworkEvent() takes a decoded notification, resolves its
// network PlayerId to THIS client's own GameObject for that remote player, and raises an
// ordinary Engine Death event about that object's local ObjectId. The EventManager stamps
// it with the RECEIVER's logical time — the sender's clock is never transmitted or used.
// A PlayerId with no matching remote object (unknown, or already disconnected) is ignored.
//
// WHY A REMOTE DEATH CAN NEVER RESPAWN THE LOCAL PLAYER. The raised event is the same
// EventType::Death, but its Subject is the remote player's object, and ShipController
// handles a Death only when the Subject is the local ship (it returns immediately
// otherwise). Local and remote deaths are therefore separated by WHO the event is about —
// no new engine event type, no flag — and each is handled by its own subscriber: the
// controller for the local ship, this bridge for remote players.
//
// REMOTE REACTION. This bridge's Death handler, for a remote subject, marks that player
// "recently died" for MarkDuration of logical time, measured from the event's own
// (receiver-local) timestamp. Game renders a marked remote player in an alternate color.
// Time comes from the same Timeline-backed source as the EventManager, so pausing or
// scaling the Timeline affects the marker exactly as it affects everything else.
//
// Delivery is not guaranteed (see PeerClient.h): a lost notification just means no marker.
class PeerEventBridge
{
public:
	/// How long, in logical seconds, a remote player stays marked after a Death event.
	static constexpr double MarkDuration = 1.0;

	/// `events` and `objects` must outlive the bridge. `localPlayerObjectId` is the local
	/// player's object, whose network id (set once connected) is what gets announced.
	/// `sendToPeers` is called, synchronously inside Death dispatch, once per local death.
	/// `timeSource` must be the same logical clock the EventManager uses.
	PeerEventBridge(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
	                Engine::ObjectId localPlayerObjectId, std::function<double()> timeSource,
	                std::function<void(Engine::NetworkEventKind)> sendToPeers);

	// The Death subscription captures `this`; never copy or move.
	PeerEventBridge(const PeerEventBridge&) = delete;
	PeerEventBridge& operator=(const PeerEventBridge&) = delete;

	/// Raises the local Death event for a received notification (see class comment).
	/// Returns false, raising nothing, if no remote object has that network PlayerId.
	bool HandleNetworkEvent(const Engine::NetworkEvent& networkEvent);

	/// While disabled, a local Death is NOT announced to peers. For replay playback (not
	/// wired in Game at present), so a replayed death is never broadcast as a new one. Remote
	/// notifications and markers are unaffected (peers are live during a local replay).
	void SetAnnounceEnabled(bool enabled);

	/// True while the remote object is within its marked-dead window.
	bool IsMarkedDead(Engine::ObjectId remoteObjectId) const;

	/// Drops expired markers and markers for objects that no longer exist (a remote player
	/// that disconnected). Call once per frame; IsMarkedDead() is correct without it.
	void PruneMarkers();

private:
	void OnDeath(const Engine::Event& event);

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	Engine::ObjectId m_LocalPlayerObjectId;
	std::function<double()> m_TimeSource;
	std::function<void(Engine::NetworkEventKind)> m_SendToPeers;

	// Remote object -> logical time its marker expires.
	std::unordered_map<Engine::ObjectId, double> m_MarkerExpiry;

	bool m_AnnounceEnabled = true;

	// Declared last: destroyed first, unsubscribing while m_Events is still alive.
	Engine::ScopedSubscription m_DeathSubscription;
};

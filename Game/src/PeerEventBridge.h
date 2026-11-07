#pragma once

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/EventProtocol.h"

#include "BulletField.h"

#include <functional>
#include <unordered_map>

// The explicit seam between the network and the local game for discrete gameplay notifications
// (Spare Parts-specific, SDL-free and socket-free, so it is unit tested without a Game or a real
// PeerClient). Neither side knows about the other: Engine::EventManager is not network-aware and
// PeerClient has no EventManager. This class, owned by Game and used only on the main thread, is
// the only place they meet. Three notifications cross it: Death, Spawn and Shoot.
//
// AUTHORITY MODEL. Every client is authoritative for its OWN ship only: its movement, whether a
// bullet hit it, its Death and its Spawn. Nobody ever declares ANOTHER player dead. A shooter
// announces that it fired; each other client rebuilds that bullet and, if the bullet reaches ITS
// own ship, that client raises its own Collision -> Death (see ShipController) and announces it.
//
// LOCAL -> NETWORK.
//  - A Death or Spawn event whose subject is the LOCAL ship is announced to `sendToPeers` (Game
//    wires it to PeerClient::PublishEvent). An event about any other object is never re-announced,
//    so a received notification can never echo back out.
//  - AnnounceShot() (Game wires ShipController's shot listener to it) announces a bullet the
//    local ship really fired, with its origin and heading.
//  SetAnnounceEnabled(false) silences all three at once (for replay playback, so a replayed
//  death, spawn or shot is never broadcast as a new one). SetRemoteShootsDiscarded(true) is its
//  receiving-side twin: during playback a remote Shoot is dropped, not rebuilt or queued.
//
// NETWORK -> LOCAL. HandleNetworkEvent() takes a decoded notification:
//  - Shoot: rebuilds the bullet in the receiver's BulletField from the received origin and
//    heading and the SHARED speed/lifetime constants. Nothing else: it does not decide who gets
//    hit. Accepted even if that player's state has not arrived yet (the bullet is real).
//  - Death / Spawn: resolves the sender's PlayerId to THIS client's own object for that remote
//    player and raises an ordinary Engine Death / Spawn event about THAT object. (A Death or
//    Spawn event about a remote object can never touch the local ship: ShipController handles
//    only events whose subject is the local ship.) An unknown or disconnected player is ignored.
//  - Anything claiming to come from the local player's own id is ignored (a loop-back).
//  The EventManager stamps events with the RECEIVER's logical time — the sender's clock is never
//  transmitted or used.
//
// REMOTE LIFE STATE, kept by PlayerId. A remote player is ALIVE until its Death arrives, and
// stays DEAD until its Spawn arrives — never because a timer ran out. (Newly discovered players
// have no entry, so they are alive.) A visible death marker shows for MarkDuration after the
// death, measured from the event's own receiver-local timestamp; after that the ship is simply
// hidden. Delivery is not guaranteed (see PeerClient.h): a lost Spawn leaves that player hidden
// until its next Death or Spawn arrives — an accepted limit of the peer-authoritative slice.
class PeerEventBridge
{
public:
	/// How long, in logical seconds, a remote player's death marker shows after its Death.
	static constexpr double MarkDuration = 1.0;

	using NetworkSink = std::function<void(const Engine::NetworkEvent&)>;

	/// `events`, `objects` and `bullets` must outlive the bridge. `localShipObjectId` is the local
	/// ship's object, whose network id (set once connected) identifies it on the wire.
	/// `sendToPeers` is called synchronously, once per announcement; the frame's source id is
	/// re-stamped by the transport. `timeSource` must be the same logical clock the EventManager uses.
	PeerEventBridge(Engine::EventManager& events, Engine::GameObjectRegistry& objects, BulletField& bullets,
	                Engine::ObjectId localShipObjectId, std::function<double()> timeSource, NetworkSink sendToPeers);

	// The subscriptions capture `this`; never copy or move.
	PeerEventBridge(const PeerEventBridge&) = delete;
	PeerEventBridge& operator=(const PeerEventBridge&) = delete;

	/// Applies one received notification (see the class comment). Returns false, doing
	/// nothing, if it was ignored (loop-back, or Death/Spawn for an unknown player).
	bool HandleNetworkEvent(const Engine::NetworkEvent& networkEvent);

	/// Announces a bullet the local ship just fired, at `origin` (its centre) along the heading.
	void AnnounceShot(Engine::Vector2 origin, float headingRadians);

	/// While disabled, nothing local is announced to peers (Death, Spawn or Shoot). Received
	/// notifications and remote life state are unaffected (peers stay live during a local replay).
	void SetAnnounceEnabled(bool enabled);

	/// While on, a received Shoot is drained and DISCARDED: no bullet is built and it is not kept
	/// for later. A remote shot belongs to the live timeline and must not interact with a
	/// replayed ship (nor surface, stale, once playback ends). Death, Spawn and remote state
	/// still apply normally. Game turns this on for the length of a replay playback.
	void SetRemoteShootsDiscarded(bool discarded);

	/// False from a remote player's Death until its Spawn; true for everyone else.
	bool IsRemoteAlive(Engine::ObjectId remoteObjectId) const;

	/// True while the remote player's death marker should be drawn (dead, within MarkDuration).
	bool IsDeathMarkerActive(Engine::ObjectId remoteObjectId) const;

	/// Forgets life state of players whose object no longer exists (disconnected). Call once per
	/// frame; the queries above are correct without it.
	void PruneRemoved();

private:
	void OnDeath(const Engine::Event& event);
	void OnSpawn(const Engine::Event& event);

	void Announce(const Engine::NetworkEvent& event);
	Engine::PlayerId LocalNetworkId() const;
	// The network id of a remote object, or 0 if it has none (not a networked player).
	Engine::PlayerId NetworkIdOf(Engine::ObjectId objectId) const;

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	BulletField& m_Bullets;
	Engine::ObjectId m_LocalShipObjectId;
	std::function<double()> m_TimeSource;
	NetworkSink m_SendToPeers;

	// PlayerId -> logical time of its (latest) Death. Present means dead.
	std::unordered_map<Engine::PlayerId, double> m_DeadSince;

	bool m_AnnounceEnabled = true;
	bool m_DiscardRemoteShoots = false;

	// Declared last: destroyed first, unsubscribing while m_Events is still alive.
	Engine::ScopedSubscription m_DeathSubscription;
	Engine::ScopedSubscription m_SpawnSubscription;
};

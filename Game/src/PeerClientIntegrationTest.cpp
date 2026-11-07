// Real, non-SDL integration test for PeerClient (Milestone 2 Section 5 PeerClient
// checkpoint). Unlike every other Game/src/*IntegrationTest.cpp file, this one needs NO
// running production server at all: PeerClient is pure peer-to-peer (no bootstrap, no
// dedicated session, no PlayerRegistry) — every scenario here is proven purely between
// real PeerClient instances, each with its own worker thread and real ZeroMQ PUB/SUB
// sockets communicating over real TCP loopback endpoints. No shared test shortcut ever
// passes state directly between instances.
//
// PUB/SUB's well-known "slow joiner" behavior (a freshly-connected SUB's subscription
// handshake takes a small, variable amount of time; any message published before it
// completes is silently dropped for that subscriber) is handled the same way
// game-engine's SocketPubSubTest.cpp handles it: bounded retry loops that keep
// publishing while polling, never a single Send() assumed to arrive.
//
// Not CTest-registered — same rationale as every other Game/src/*IntegrationTest.cpp
// file (a real multi-second/timing scenario, not a pure deterministic unit).

#include "PeerClient.h"
#include "ArenaRules.h"
#include "BulletField.h"
#include "PeerEventBridge.h"
#include "ShipController.h"

#include "Engine/Event/EventManager.h"
#include "Engine/GameObject/GameObjectRegistry.h"
#include "Engine/Network/EventProtocol.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr int PollIntervalMs = 5;
	constexpr int PollTimeoutMs = 2000;

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

	template <typename Predicate>
	bool WaitUntil(Predicate predicate, int timeoutMs, int intervalMs)
	{
		int elapsed = 0;
		while (!predicate())
		{
			if (elapsed >= timeoutMs)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
			elapsed += intervalMs;
		}
		return true;
	}

	Engine::PeerInfo MakePeer(Engine::PlayerId id, std::uint16_t port)
	{
		return Engine::PeerInfo{ id, port };
	}

	bool StateMatches(const std::unordered_map<Engine::PlayerId, Engine::PlayerState>& states, Engine::PlayerId id,
	                   const Engine::PlayerState& expected)
	{
		auto it = states.find(id);
		return it != states.end() && it->second.PositionX == expected.PositionX &&
		       it->second.PositionY == expected.PositionY && it->second.VelocityX == expected.VelocityX &&
		       it->second.VelocityY == expected.VelocityY;
	}

	std::vector<std::uint8_t> ToBytes(const std::string& frame)
	{
		return std::vector<std::uint8_t>(frame.begin(), frame.end());
	}

	std::string ToFrame(const std::vector<std::uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}

	// PUB/SUB drops anything published before the subscription finishes connecting, and an
	// event (unlike state) must never be re-sent to compensate — that would break the
	// "exactly one" checks. So: publish STATE until the peer provably receives it (which proves
	// the subscription is live), and only then publish events.
	bool WarmUp(PeerClient& from, Engine::PlayerId fromId, PeerClient& to)
	{
		Engine::PlayerState warm{ fromId, 1.0f, 2.0f, 0.0f, 0.0f };
		return WaitUntil(
		    [&]()
		    {
			    from.PublishState(warm);
			    return to.GetLatestPeerStates().find(fromId) != to.GetLatestPeerStates().end();
		    },
		    PollTimeoutMs, PollIntervalMs);
	}

	// Polls (on THIS thread, as Game does) until `expected` notifications have been collected
	// or the timeout passes; returns everything collected, in arrival order.
	std::vector<Engine::NetworkEvent> CollectEvents(PeerClient& client, std::size_t expected, int timeoutMs)
	{
		std::vector<Engine::NetworkEvent> collected;
		WaitUntil(
		    [&]()
		    {
			    for (const Engine::NetworkEvent& event : client.PollEvents())
			    {
				    collected.push_back(event);
			    }
			    return collected.size() >= expected;
		    },
		    timeoutMs, PollIntervalMs);
		return collected;
	}

	// Keeps polling for a quiet period and returns whatever extra arrived (to prove "exactly").
	std::vector<Engine::NetworkEvent> DrainFor(PeerClient& client, int durationMs)
	{
		std::vector<Engine::NetworkEvent> extra;
		for (int elapsed = 0; elapsed < durationMs; elapsed += PollIntervalMs)
		{
			for (const Engine::NetworkEvent& event : client.PollEvents())
			{
				extra.push_back(event);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		}
		return extra;
	}
}

int main()
{
	// --- Test 1: two peers, bidirectional exact delivery ---
	{
		PeerClient a(1, 6001);
		PeerClient b(2, 6002);
		a.UpdatePeers({ MakePeer(2, 6002) });
		b.UpdatePeers({ MakePeer(1, 6001) });

		Engine::PlayerState knownA{ 1, 100.0f, 200.0f, 10.0f, 0.0f };
		bool bGotA = WaitUntil(
		    [&]()
		    {
			    a.PublishState(knownA); // keep publishing: slow-joiner tolerant
			    return StateMatches(b.GetLatestPeerStates(), 1, knownA);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(bGotA, "TwoPeers_B_ReceivesExactKnownStateFromA");

		Engine::PlayerState knownB{ 2, 300.0f, 400.0f, -5.0f, 2.0f };
		bool aGotB = WaitUntil(
		    [&]()
		    {
			    b.PublishState(knownB);
			    return StateMatches(a.GetLatestPeerStates(), 2, knownB);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aGotB, "TwoPeers_A_ReceivesExactKnownStateFromB");
	}

	// --- Test 2: three peers, one PUB fans out to two SUB consumers ---
	{
		PeerClient a(1, 6011);
		PeerClient b(2, 6012);
		PeerClient c(3, 6013);
		std::vector<Engine::PeerInfo> allThree{ MakePeer(1, 6011), MakePeer(2, 6012), MakePeer(3, 6013) };
		a.UpdatePeers(allThree);
		b.UpdatePeers(allThree);
		c.UpdatePeers(allThree);

		Engine::PlayerState knownA{ 1, 111.0f, 222.0f, 3.0f, 4.0f };
		bool bGotA = false;
		bool cGotA = false;
		WaitUntil(
		    [&]()
		    {
			    a.PublishState(knownA);
			    bGotA = bGotA || StateMatches(b.GetLatestPeerStates(), 1, knownA);
			    cGotA = cGotA || StateMatches(c.GetLatestPeerStates(), 1, knownA);
			    return bGotA && cGotA;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(bGotA, "ThreePeers_B_ReceivesA");
		Check(cGotA, "ThreePeers_C_ReceivesA");

		// --- Test 4 continues here (directory removal), reusing this same trio ---

		// Remove A from B/C's desired directory.
		std::vector<Engine::PeerInfo> withoutA{ MakePeer(2, 6012), MakePeer(3, 6013) };
		b.UpdatePeers(withoutA);
		c.UpdatePeers(withoutA);

		bool aGoneFromB = WaitUntil([&]() { return b.GetLatestPeerStates().find(1) == b.GetLatestPeerStates().end(); },
		                            PollTimeoutMs, PollIntervalMs);
		bool aGoneFromC = WaitUntil([&]() { return c.GetLatestPeerStates().find(1) == c.GetLatestPeerStates().end(); },
		                            PollTimeoutMs, PollIntervalMs);
		Check(aGoneFromB, "DirectoryRemoval_A_RemovedFromB");
		Check(aGoneFromC, "DirectoryRemoval_A_RemovedFromC");

		// A keeps publishing a NEW, different state; it must not reappear anywhere.
		Engine::PlayerState aAfterRemoval{ 1, 999.0f, 999.0f, 999.0f, 999.0f };
		for (int i = 0; i < 20; ++i)
		{
			a.PublishState(aAfterRemoval);
			std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		}
		bool aStillAbsentFromB = b.GetLatestPeerStates().find(1) == b.GetLatestPeerStates().end();
		bool aStillAbsentFromC = c.GetLatestPeerStates().find(1) == c.GetLatestPeerStates().end();
		Check(aStillAbsentFromB, "DirectoryRemoval_SubsequentAPublishes_DoNotReinsertIntoB");
		Check(aStillAbsentFromC, "DirectoryRemoval_SubsequentAPublishes_DoNotReinsertIntoC");

		// B/C must still receive each other after A's removal.
		Engine::PlayerState knownB{ 2, 55.0f, 66.0f, 0.0f, 0.0f };
		bool cGotB = WaitUntil(
		    [&]()
		    {
			    b.PublishState(knownB);
			    return StateMatches(c.GetLatestPeerStates(), 2, knownB);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(cGotB, "DirectoryRemoval_B_And_C_StillReceiveEachOther");
	}

	// --- Test 3: late join ---
	{
		PeerClient a(1, 6021);
		PeerClient b(2, 6022);
		a.UpdatePeers({ MakePeer(2, 6022) });
		b.UpdatePeers({ MakePeer(1, 6021) });

		Engine::PlayerState earlyA{ 1, 1.0f, 2.0f, 3.0f, 4.0f };
		WaitUntil(
		    [&]()
		    {
			    a.PublishState(earlyA);
			    return StateMatches(b.GetLatestPeerStates(), 1, earlyA);
		    },
		    PollTimeoutMs, PollIntervalMs);

		// C constructed later; A/B never restarted.
		PeerClient c(3, 6023);
		std::vector<Engine::PeerInfo> allThree{ MakePeer(1, 6021), MakePeer(2, 6022), MakePeer(3, 6023) };
		a.UpdatePeers(allThree);
		b.UpdatePeers(allThree);
		c.UpdatePeers(allThree);

		Engine::PlayerState knownC{ 3, 7.0f, 8.0f, 9.0f, 10.0f };
		bool aGotC = WaitUntil(
		    [&]()
		    {
			    c.PublishState(knownC);
			    return StateMatches(a.GetLatestPeerStates(), 3, knownC);
		    },
		    PollTimeoutMs, PollIntervalMs);
		bool bGotC = WaitUntil(
		    [&]()
		    {
			    c.PublishState(knownC);
			    return StateMatches(b.GetLatestPeerStates(), 3, knownC);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aGotC, "LateJoin_A_EventuallyReceivesC");
		Check(bGotC, "LateJoin_B_EventuallyReceivesC");

		Engine::PlayerState knownA{ 1, 11.0f, 12.0f, 13.0f, 14.0f };
		bool cGotA = WaitUntil(
		    [&]()
		    {
			    a.PublishState(knownA);
			    return StateMatches(c.GetLatestPeerStates(), 1, knownA);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(cGotA, "LateJoin_C_EventuallyReceivesA");
	}

	// --- Test 5: malformed / self message ---
	{
		PeerClient a(1, 6041);

		// Self-message: even if a's own directory (accidentally) includes itself, it
		// must never connect to itself or record its own state as a "peer".
		a.UpdatePeers({ MakePeer(1, 6041), MakePeer(99, 6099) });
		Engine::PlayerState ownState{ 1, 1.0f, 1.0f, 1.0f, 1.0f };
		for (int i = 0; i < 10; ++i)
		{
			a.PublishState(ownState);
			std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		}
		bool neverStoredSelf = a.GetLatestPeerStates().find(1) == a.GetLatestPeerStates().end();
		Check(neverStoredSelf, "SelfMessage_NeverStoredAsPeerState");

		// Malformed message: a raw PUB socket (standing in for peer 99's declared
		// endpoint) sends garbage bytes; `a`'s SUB is connected to it via the directory
		// above. Must not crash and must not produce a stored state for id 99.
		Engine::Socket rawPub(Engine::SocketRole::Publish);
		rawPub.Bind("tcp://*:6099");

		// No ack exists for a PUB/SUB send, so there is nothing to WaitUntil() on here:
		// keep sending garbage across a window comfortably longer than the slow-joiner
		// connection-establishment delay, then check the result once.
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			rawPub.Send(ToFrame(std::vector<std::uint8_t>{ 0xDE, 0xAD, 0xBE, 0xEF }));
			std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		}

		bool malformedIgnored = a.GetLatestPeerStates().find(99) == a.GetLatestPeerStates().end();
		Check(malformedIgnored, "MalformedMessage_IgnoredWithoutCrash");
	}

	// --- Test 6: non-blocking API ---
	{
		PeerClient a(1, 6051);
		PeerClient b(2, 6052);

		auto burstStart = std::chrono::steady_clock::now();
		for (int i = 0; i < 1000; ++i)
		{
			a.PublishState(Engine::PlayerState{ 1, static_cast<float>(i), 0.0f, 0.0f, 0.0f });
		}
		a.UpdatePeers({ MakePeer(2, 6052) });
		auto burstDuration = std::chrono::steady_clock::now() - burstStart;
		Check(burstDuration < std::chrono::milliseconds(200), "NonBlocking_1000PublishStateCallsPlusUpdatePeers_ReturnQuickly");

		auto readStart = std::chrono::steady_clock::now();
		std::unordered_map<Engine::PlayerId, Engine::PlayerState> states = b.GetLatestPeerStates();
		auto readDuration = std::chrono::steady_clock::now() - readStart;
		(void)states;
		Check(readDuration < std::chrono::milliseconds(50), "NonBlocking_GetLatestPeerStates_ReturnsWithoutNetworkBlocking");
	}


	// ===== Milestone 4 Checkpoint 3: gameplay event notifications over the same channel =====

	// --- Test 7: one peer publishes a Death event; the other receives exactly one ---
	{
		PeerClient a(1, 6201);
		PeerClient b(2, 6202);
		a.UpdatePeers({ MakePeer(2, 6202) });
		b.UpdatePeers({ MakePeer(1, 6201) });
		Check(WarmUp(a, 1, b), "Event_Setup_SubscriptionEstablished");

		a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		std::vector<Engine::NetworkEvent> received = CollectEvents(b, 1, PollTimeoutMs);
		Check(received.size() == 1 && received[0].Kind == Engine::NetworkEventKind::Death &&
		          received[0].SourcePlayerId == 1,
		      "Event_Death_ReceivedByPeerWithSenderPlayerId");
		Check(DrainFor(b, 300).empty(), "Event_Death_ReceivedExactlyOnce");
		Check(a.PollEvents().empty(), "Event_SenderNeverReceivesItsOwnEvent");
	}

	// --- Test 8: several events are each delivered (a queue, not a latest-wins slot) ---
	{
		PeerClient a(1, 6211);
		PeerClient b(2, 6212);
		a.UpdatePeers({ MakePeer(2, 6212) });
		b.UpdatePeers({ MakePeer(1, 6211) });
		WarmUp(a, 1, b);

		const std::size_t eventCount = 25;
		for (std::size_t i = 0; i < eventCount; ++i)
		{
			a.PublishEvent({ Engine::NetworkEventKind::Death, 0 }); // back to back: a slot would keep only the last
		}
		std::vector<Engine::NetworkEvent> received = CollectEvents(b, eventCount, PollTimeoutMs);
		Check(received.size() == eventCount, "Event_BurstOfEvents_NoneOverwrittenOrCoalesced");
		Check(DrainFor(b, 200).empty(), "Event_BurstOfEvents_NoneDuplicated");
	}

	// --- Test 9: state and event frames interleaved on one channel stay independent ---
	{
		PeerClient a(1, 6221);
		PeerClient b(2, 6222);
		a.UpdatePeers({ MakePeer(2, 6222) });
		b.UpdatePeers({ MakePeer(1, 6221) });
		WarmUp(a, 1, b);

		const std::size_t eventCount = 10;
		Engine::PlayerState last{};
		for (std::size_t i = 0; i < eventCount; ++i)
		{
			last = Engine::PlayerState{ 1, 100.0f + static_cast<float>(i), 50.0f, 3.0f, 4.0f };
			a.PublishState(last);
			a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
		std::vector<Engine::NetworkEvent> received = CollectEvents(b, eventCount, PollTimeoutMs);
		Check(received.size() == eventCount, "Interleaved_EveryEventStillDelivered");
		bool stateIntact = WaitUntil([&]() { return StateMatches(b.GetLatestPeerStates(), 1, last); }, PollTimeoutMs,
		                              PollIntervalMs);
		Check(stateIntact, "Interleaved_LatestStateStillExact");
		Check(DrainFor(b, 200).empty(), "Interleaved_EventsNeverDuplicatedByStateFrames");
	}

	// --- Test 10: bad frames are ignored without crashing, and never fed to the wrong decoder ---
	{
		PeerClient a(1, 6231);
		Engine::Socket rawPub(Engine::SocketRole::Publish); // stands in for peer 99's declared endpoint
		rawPub.Bind("tcp://*:6299");
		a.UpdatePeers({ MakePeer(99, 6299) });

		std::string eventFrame = ToFrame(Engine::EncodeNetworkEvent({ Engine::NetworkEventKind::Death, 99 }));
		std::string truncated = eventFrame.substr(0, 3);
		std::string unknownKind = eventFrame;
		unknownKind[1] = 77;
		std::string stateFrame = ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ Engine::PlayerState{ 99, 5.0f, 6.0f, 0.0f, 0.0f }, false }));
		std::string strangerFrame = ToFrame(Engine::EncodeNetworkEvent({ Engine::NetworkEventKind::Death, 55 }));

		// Keep sending across the slow-joiner window; every frame here must be rejected.
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			rawPub.Send(truncated);
			rawPub.Send(unknownKind);
			rawPub.Send(strangerFrame); // well-formed, but from a player not in the directory
			rawPub.Send(stateFrame);    // valid state: must update state, must NOT become an event
			std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		}
		Check(a.PollEvents().empty(), "BadEventFrames_TruncatedUnknownKindAndStrangerSource_Ignored");
		Check(StateMatches(a.GetLatestPeerStates(), 99, Engine::PlayerState{ 99, 5.0f, 6.0f, 0.0f, 0.0f }),
		      "BadEventFrames_DoNotDisturbStateFramesOnTheSameChannel");

		// Once a valid event from the directory peer arrives it is accepted.
		bool accepted = WaitUntil(
		    [&]()
		    {
			    rawPub.Send(eventFrame);
			    return !a.PollEvents().empty();
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(accepted, "BadEventFrames_ValidEventFromDirectoryPeer_StillAccepted");
	}

	// --- Test 11: the worker never touches an EventManager; only the polling thread does ---
	{
		PeerClient a(1, 6241);
		PeerClient b(2, 6242);
		a.UpdatePeers({ MakePeer(2, 6242) });
		b.UpdatePeers({ MakePeer(1, 6241) });
		WarmUp(a, 1, b);

		double now = 0.0;
		Engine::EventManager manager([&]() { return now; });
		std::thread::id handlerThread;
		int handled = 0;
		Engine::ScopedSubscription subscription(manager, Engine::EventType::Death,
		                                        [&](const Engine::Event&)
		                                        {
			                                        handlerThread = std::this_thread::get_id();
			                                        ++handled;
		                                        });

		a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		for (const Engine::NetworkEvent& event : CollectEvents(b, 1, PollTimeoutMs))
		{
			manager.Raise(Engine::DeathEventData{ event.SourcePlayerId, Engine::NoObjectId });
		}
		Check(manager.PendingCount() == 1 && handled == 0,
		      "ThreadOwnership_ReceivedEventIsOnlyQueuedUntilMainThreadDispatches");
		manager.DispatchDue();
		Check(handled == 1 && handlerThread == std::this_thread::get_id(),
		      "ThreadOwnership_EventHandledOnTheMainThread");
	}

	// --- Test 12: peer-directory refresh / disconnect around pending events is safe ---
	{
		PeerClient a(1, 6251);
		PeerClient b(2, 6252);
		a.UpdatePeers({ MakePeer(2, 6252) });
		b.UpdatePeers({ MakePeer(1, 6251) });
		WarmUp(a, 1, b);

		a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		b.UpdatePeers({}); // b drops a right as the event is in flight
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		b.PollEvents(); // whatever beat the removal is fine; nothing may crash
		for (int i = 0; i < 3; ++i)
		{
			a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		}
		Check(DrainFor(b, 300).empty(), "PeerRefresh_RemovedPeersEventsAreNoLongerDelivered");

		b.UpdatePeers({ MakePeer(1, 6251) }); // and a rejoining peer works again
		WarmUp(a, 1, b);
		a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		Check(CollectEvents(b, 1, PollTimeoutMs).size() >= 1, "PeerRefresh_ReaddedPeerDeliversEventsAgain");
	}


	// ===== Networked arena combat: Shoot and Spawn over the same channel =====

	// --- Test 11: Shoot carries its payload exactly, and Shoot / Death / Spawn stay in order
	// with state frames interleaved between them ---
	{
		PeerClient a(1, 6271);
		PeerClient b(2, 6272);
		a.UpdatePeers({ MakePeer(2, 6272) });
		b.UpdatePeers({ MakePeer(1, 6271) });
		Check(WarmUp(a, 1, b), "Combat_Setup_SubscriptionEstablished");
		Check(WarmUp(b, 2, a), "Combat_Setup_ReverseSubscriptionEstablished");

		Engine::NetworkEvent firstShot{ Engine::NetworkEventKind::Shoot, 0 };
		firstShot.OriginX = 516.5f;
		firstShot.OriginY = -12.25f;
		firstShot.HeadingRadians = 1.5f;
		Engine::NetworkEvent secondShot{ Engine::NetworkEventKind::Shoot, 0 };
		secondShot.OriginX = 1943.0f;
		secondShot.OriginY = 540.0f;
		secondShot.HeadingRadians = -2.3561945f;

		Engine::PlayerState last{};
		a.PublishState(Engine::PlayerState{ 1, 10.0f, 20.0f, 3.0f, 4.0f });
		a.PublishEvent(firstShot);
		a.PublishState(Engine::PlayerState{ 1, 11.0f, 21.0f, 3.0f, 4.0f });
		a.PublishEvent(secondShot);
		a.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		last = Engine::PlayerState{ 1, 12.0f, 22.0f, 3.0f, 4.0f };
		a.PublishState(last);
		a.PublishEvent({ Engine::NetworkEventKind::Spawn, 0 });

		std::vector<Engine::NetworkEvent> received = CollectEvents(b, 4, PollTimeoutMs);
		Check(received.size() == 4, "Combat_FourEventsDelivered_NoneLostToStateFrames");
		bool orderAndKinds = received.size() == 4 && received[0].Kind == Engine::NetworkEventKind::Shoot &&
		                     received[1].Kind == Engine::NetworkEventKind::Shoot && received[2].Kind == Engine::NetworkEventKind::Death &&
		                     received[3].Kind == Engine::NetworkEventKind::Spawn;
		Check(orderAndKinds, "Combat_ShootShootDeathSpawn_ArriveInTheOrderTheyWereRaised");
		bool sourceIds = received.size() == 4;
		for (const Engine::NetworkEvent& event : received)
		{
			sourceIds = sourceIds && event.SourcePlayerId == 1;
		}
		Check(sourceIds, "Combat_EveryEventCarriesTheSendersPlayerId");
		Check(received.size() >= 2 && received[0].OriginX == 516.5f && received[0].OriginY == -12.25f &&
		          received[0].HeadingRadians == 1.5f,
		      "Combat_FirstShoot_PayloadExact");
		Check(received.size() >= 2 && received[1].OriginX == 1943.0f && received[1].OriginY == 540.0f &&
		          received[1].HeadingRadians == -2.3561945f,
		      "Combat_SecondShoot_PayloadExact_NotOverwrittenByTheFirst");
		Check(DrainFor(b, 250).empty(), "Combat_EventsDeliveredExactlyOnce");
		bool stateIntact = WaitUntil([&]() { return StateMatches(b.GetLatestPeerStates(), 1, last); }, PollTimeoutMs,
		                              PollIntervalMs);
		Check(stateIntact, "Combat_LatestStateStillExactAmongTheEvents");
		Check(a.PollEvents().empty(), "Combat_SenderNeverReceivesItsOwnEvents");

		// And the other direction: B's Death, then B's Spawn, reach A.
		b.PublishState(Engine::PlayerState{ 2, 5.0f, 6.0f, 0.0f, 0.0f });
		b.PublishEvent({ Engine::NetworkEventKind::Death, 0 });
		b.PublishState(Engine::PlayerState{ 2, 5.0f, 6.0f, 0.0f, 0.0f });
		b.PublishEvent({ Engine::NetworkEventKind::Spawn, 0 });
		std::vector<Engine::NetworkEvent> backwards = CollectEvents(a, 2, PollTimeoutMs);
		Check(backwards.size() == 2 && backwards[0].Kind == Engine::NetworkEventKind::Death &&
		          backwards[1].Kind == Engine::NetworkEventKind::Spawn && backwards[0].SourcePlayerId == 2 &&
		          backwards[1].SourcePlayerId == 2,
		      "Combat_BsDeathThenSpawn_ReachA_InOrder");
		Check(DrainFor(a, 200).empty(), "Combat_BsEvents_ReceivedExactlyOnce");
	}

	// --- Test 12: a burst of Shoots is a queue, not a latest-wins slot ---
	{
		PeerClient a(1, 6281);
		PeerClient b(2, 6282);
		a.UpdatePeers({ MakePeer(2, 6282) });
		b.UpdatePeers({ MakePeer(1, 6281) });
		WarmUp(a, 1, b);

		const std::size_t shotCount = 30;
		for (std::size_t i = 0; i < shotCount; ++i)
		{
			Engine::NetworkEvent shot{ Engine::NetworkEventKind::Shoot, 0 };
			shot.OriginX = static_cast<float>(i);
			shot.OriginY = 2.0f * static_cast<float>(i);
			shot.HeadingRadians = 0.01f * static_cast<float>(i);
			a.PublishEvent(shot);
		}
		std::vector<Engine::NetworkEvent> received = CollectEvents(b, shotCount, PollTimeoutMs);
		bool fifo = received.size() == shotCount;
		for (std::size_t i = 0; fifo && i < shotCount; ++i)
		{
			fifo = received[i].Kind == Engine::NetworkEventKind::Shoot && received[i].OriginX == static_cast<float>(i) &&
			       received[i].OriginY == 2.0f * static_cast<float>(i) &&
			       received[i].HeadingRadians == 0.01f * static_cast<float>(i);
		}
		Check(fifo, "Burst_ThirtyShoots_AllArrive_InOrder_PayloadsIntact");
		Check(DrainFor(b, 200).empty(), "Burst_NoShootDuplicated");
	}

	// --- Test 13: the whole path, two miniature "games", real PeerClient hop in the middle ---
	// A fires; B (a different logical clock) rebuilds the bullet; the bullet reaches B's own ship;
	// B alone decides it died and announces it; A shows B dead until B announces its Spawn.
	{
		struct World
		{
			World(Engine::PlayerId localId, Engine::PlayerId remoteId, double startTime, PeerClient& peer,
			      Engine::Vector2 shipCentre, Engine::Vector2 remoteCentre)
				: Now(startTime),
				  Events([this]() { return Now; }),
				  Physics(0.0f),
				  Bullets(Objects, Physics, BulletConfig{ 200.0f, 1.4f, 5.0f }),
				  Peer(peer)
			{
				Engine::GameObject& local = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, { 48.0f, 48.0f });
				SetCenter(local, shipCentre);
				local.SetVelocity({ 0.0f, 0.0f });
				local.SetNetworkId(localId);
				LocalObjectId = local.GetId();
				Engine::GameObject& remote = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, { 48.0f, 48.0f });
				SetCenter(remote, remoteCentre);
				remote.SetNetworkId(remoteId);
				RemoteObjectId = remote.GetId();

				Controller = std::make_unique<ShipController>(
				    Events, Objects, Bullets, LocalObjectId, ShipConfig{ 100.0f, 2.0f, 24.0f, 36.0f, 0.5, 2.0, 1.0 },
				    [shipCentre] { return shipCentre; }, [this]() { return Now; });
				Bridge = std::make_unique<PeerEventBridge>(Events, Objects, Bullets, LocalObjectId,
				                                          [&peer](const Engine::NetworkEvent& event) { peer.PublishEvent(event); });
				Controller->SetShotListener([this](Engine::Vector2 origin, float heading) { Bridge->AnnounceShot(origin, heading); });
			}

			// One game frame, in the order Game::Update runs it, then the network poll.
			void Step(float dt)
			{
				Now += dt;
				Controller->BeginFrame();
				Events.DispatchDue();
				Bullets.Update(dt);
				Controller->DetectSolidHits();
				Controller->DetectBulletHits();
				Events.DispatchDue();
				for (const Engine::NetworkEvent& event : Peer.PollEvents())
				{
					Bridge->HandleNetworkEvent(event);
				}
			}

			double Now;
			Engine::EventManager Events;
			Engine::GameObjectRegistry Objects;
			Engine::PhysicsSystem Physics;
			BulletField Bullets;
			PeerClient& Peer;
			Engine::ObjectId LocalObjectId = 0;
			Engine::ObjectId RemoteObjectId = 0;
			std::unique_ptr<ShipController> Controller;
			std::unique_ptr<PeerEventBridge> Bridge;
		};

		PeerClient peerA(1, 6261);
		PeerClient peerB(2, 6262);
		peerA.UpdatePeers({ MakePeer(2, 6262) });
		peerB.UpdatePeers({ MakePeer(1, 6261) });
		WarmUp(peerA, 1, peerB);
		WarmUp(peerB, 2, peerA);

		// A at x=480 facing right; B's ship sits 220 px ahead of it. Very different logical clocks.
		World worldA(1, 2, 1000.0, peerA, { 480.0f, 540.0f }, { 700.0f, 540.0f });
		World worldB(2, 1, 5.0, peerB, { 700.0f, 540.0f }, { 480.0f, 540.0f });

		// Both ships spawn; each spawn is announced and the invulnerability window passes.
		worldA.Controller->RequestSpawn({ 480.0f, 540.0f });
		worldB.Controller->RequestSpawn({ 700.0f, 540.0f });
		worldA.Step(0.0f);
		worldB.Step(0.0f);
		worldA.Now += 1.0;
		worldB.Now += 1.0;
		DrainFor(peerA, 150); // (the Spawn announcements are consumed below by the steps)
		worldA.Step(0.0f);
		worldB.Step(0.0f);
		Check(worldA.Controller->IsAlive() && worldB.Controller->IsAlive() && !worldA.Controller->IsInvulnerable(),
		      "EndToEnd_Setup_BothShipsAliveAndVulnerable");

		// A fires.
		worldA.Controller->RaiseInputEvents(false, false, true);
		worldA.Step(0.0f);
		Check(worldA.Bullets.All().size() == 1, "EndToEnd_AFiredOneBullet");

		// B receives exactly one Shoot and rebuilds one bullet.
		bool rebuilt = WaitUntil(
		    [&]()
		    {
			    worldB.Step(0.0f);
			    return worldB.Bullets.All().size() >= 1;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(rebuilt && worldB.Bullets.All().size() == 1, "EndToEnd_BRebuiltExactlyOneBullet");
		const Engine::GameObject* rebuiltBullet = worldB.Objects.Find(worldB.Bullets.All()[0].Id);
		Check(rebuiltBullet != nullptr && std::fabs(CenterOf(*rebuiltBullet).X - 516.0f) < 0.01f &&
		          std::fabs(CenterOf(*rebuiltBullet).Y - 540.0f) < 0.01f && rebuiltBullet->GetVelocity().X == 200.0f,
		      "EndToEnd_BsBulletMatchesAsOriginAndHeading");
		Check(worldB.Controller->IsAlive() && worldB.Bridge->IsRemoteAlive(worldB.RemoteObjectId),
		      "EndToEnd_NobodyIsDeadJustBecauseASoftShotWasReceived");

		// Both clients advance in step; B alone detects the hit on its own ship.
		for (int i = 0; i < 20 && worldB.Controller->IsAlive(); ++i)
		{
			worldA.Step(0.05f);
			worldB.Step(0.05f);
		}
		Check(!worldB.Controller->IsAlive(), "EndToEnd_BsOwnShipDiedWhenTheReconstructedBulletReachedIt");
		Check(worldA.Controller->IsAlive() && worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId),
		      "EndToEnd_AHasNotDeclaredBDeadItselfNorHasAnythingReachedItYet");

		// B announced its Death; A learns it and shows B dead (and persistently so).
		bool aSawDeath = WaitUntil(
		    [&]()
		    {
			    worldA.Step(0.0f);
			    return !worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aSawDeath, "EndToEnd_AShowsBDeadAfterBsOwnDeathAnnouncement");
		worldA.Now += 50.0;
		worldA.Step(0.0f);
		Check(!worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId), "EndToEnd_BStaysDeadForALongAfterTheExplosion");
		Check(worldA.Controller->IsAlive(), "EndToEnd_BsDeathNeverKilledA");

		// B's respawn delay elapses; its Spawn is announced and A shows it alive again.
		worldB.Now += 2.0;
		worldB.Step(0.0f);
		Check(worldB.Controller->IsAlive(), "EndToEnd_BRespawnedAfterTheDelay");
		bool aSawSpawn = WaitUntil(
		    [&]()
		    {
			    worldA.Step(0.0f);
			    return worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aSawSpawn, "EndToEnd_AShowsBAliveAgainOnlyAfterBsSpawnAnnouncement");

		// The same chain, but the killer is a WALL (map geometry, no Shoot involved): B flies into one.
		worldB.Now += 1.0; // B's spawn protection runs out
		worldB.Step(0.0f);
		Check(worldB.Controller->IsAlive() && !worldB.Controller->IsInvulnerable(), "EndToEnd_Wall_BIsAliveAndVulnerableAgain");
		worldB.Objects.Create(Engine::ObjectType::Boundary, { 690.0f, 500.0f }, { 40.0f, 80.0f }); // right on B's ship
		worldB.Step(0.0f);
		Check(!worldB.Controller->IsAlive(), "EndToEnd_Wall_BDiedOnTheWall");
		bool aSawWallDeath = WaitUntil(
		    [&]()
		    {
			    worldA.Step(0.0f);
			    return !worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aSawWallDeath, "EndToEnd_Wall_AShowsBDeadAfterAWallDeath");
		Check(worldA.Controller->IsAlive(), "EndToEnd_Wall_ANeverDiedFromBsWallDeath");
		// B's scheduled respawn comes due: the Spawn handler revives it (this miniature world's spawn
		// point is where the wall now stands, which is harmless: a fresh spawn is protected) and
		// announces the Spawn, so A shows it alive again.
		worldB.Now += 2.0;
		worldB.Step(0.0f);
		Check(worldB.Controller->IsAlive(), "EndToEnd_Wall_BRespawned");
		bool aSawWallSpawn = WaitUntil(
		    [&]()
		    {
			    worldA.Step(0.0f);
			    return worldA.Bridge->IsRemoteAlive(worldA.RemoteObjectId);
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(aSawWallSpawn, "EndToEnd_Wall_AShowsBAliveAgainAfterItsSpawn");

		// Nothing echoed: each side received only what the other announced.
		Check(worldB.Controller->IsAlive() && worldA.Controller->IsAlive() && worldA.Bullets.All().size() <= 1,
		      "EndToEnd_NoEchoOrFeedbackLoop");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

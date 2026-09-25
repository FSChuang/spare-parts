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

#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <chrono>
#include <cstdio>
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

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

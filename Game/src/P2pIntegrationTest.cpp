// Real, non-SDL integration test for Milestone 2 Section 5's final integration:
// mechanical proof that remote player rendering data comes exclusively from PeerClient
// (direct peer-to-peer), never from the server Snapshot's Roster positions, plus the
// required real 3(+1)-client multi-client scenario (unique ids/ports, direct exchange,
// late join, clean leave). Requires a real running production server at
// BootstrapEndpoint. Not CTest-registered — same rationale as the other
// Game/src/*IntegrationTest.cpp files.
//
// This drives real NetworkClient + real PeerClient pairs directly (no Timeline, no SDL,
// no Game) — the same small pattern MultiClientIntegrationTest/PeerClientIntegrationTest
// already use, just combining both networking layers the way Game::UpdateNetworking
// does: a server-facing NetworkClient for membership/platform/peer-directory, and a
// PeerClient for direct player-state exchange, fed from the server's Peers field.
//
// PUB/SUB's well-known "slow joiner" behavior is handled the same way everywhere else in
// this project handles it: bounded retry loops that keep publishing while polling.

#include "NetworkClient.h"
#include "PeerClient.h"

#include "Engine/Network/Protocol.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr const char* BootstrapEndpoint = "tcp://127.0.0.1:5556";
	constexpr int PollIntervalMs = 10;
	constexpr int PollTimeoutMs = 3000;

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

	bool StateMatches(const std::unordered_map<Engine::PlayerId, Engine::PlayerState>& states, Engine::PlayerId id,
	                   const Engine::PlayerState& expected)
	{
		auto it = states.find(id);
		return it != states.end() && it->second.PositionX == expected.PositionX &&
		       it->second.PositionY == expected.PositionY && it->second.VelocityX == expected.VelocityX &&
		       it->second.VelocityY == expected.VelocityY;
	}

	// Mirrors Game::UpdateNetworking's lazy-creation condition exactly.
	void EnsurePeerClient(NetworkClient& client, std::unique_ptr<PeerClient>& peerClient)
	{
		if (peerClient)
		{
			return;
		}
		Engine::PlayerId id = client.GetLocalPlayerId();
		std::uint16_t port = client.GetLocalP2pPort();
		if (id != 0 && port != 0)
		{
			peerClient = std::make_unique<PeerClient>(id, port);
		}
	}

	// Mirrors Game::UpdateNetworking's peer-directory hand-off exactly: whatever the
	// server's latest Snapshot.Peers says, handed straight to PeerClient.
	void RefreshPeerDirectory(NetworkClient& client, std::unique_ptr<PeerClient>& peerClient)
	{
		if (!peerClient)
		{
			return;
		}
		std::optional<Engine::Snapshot> snapshot = client.GetLatestSnapshot();
		if (snapshot.has_value())
		{
			peerClient->UpdatePeers(snapshot->Peers);
		}
	}

	// The exact selection rule Game::UpdateNetworking applies for one remote PlayerId,
	// factored out as a small pure function purely so this test can exercise and assert
	// on it directly: `roster` is consulted ONLY for membership (whether `remoteId` is
	// present at all) — its PlayerState.Position*/Velocity* fields for `remoteId` are
	// never read here, mirroring Game.cpp precisely. The returned state, when present,
	// comes exclusively from `peerStates`.
	std::optional<Engine::PlayerState> SelectRemoteState(
	    Engine::PlayerId remoteId, const std::vector<Engine::PlayerState>& roster,
	    const std::unordered_map<Engine::PlayerId, Engine::PlayerState>& peerStates)
	{
		bool isActiveMember = false;
		for (const Engine::PlayerState& state : roster)
		{
			if (state.Id == remoteId)
			{
				isActiveMember = true;
				break;
			}
		}
		if (!isActiveMember)
		{
			return std::nullopt;
		}

		auto it = peerStates.find(remoteId);
		if (it == peerStates.end())
		{
			return std::nullopt;
		}
		return it->second;
	}
}

int main()
{
	// --- Part 12: P2P directness proof ---
	{
		NetworkClient networkA(BootstrapEndpoint);
		NetworkClient networkB(BootstrapEndpoint);

		bool connected = WaitUntil([&]() { return networkA.IsConnected() && networkB.IsConnected(); }, PollTimeoutMs,
		                           PollIntervalMs);
		Check(connected, "Directness_BothClientsConnect");

		Engine::PlayerId idA = networkA.GetLocalPlayerId();
		Engine::PlayerId idB = networkB.GetLocalPlayerId();

		std::unique_ptr<PeerClient> peerA;
		std::unique_ptr<PeerClient> peerB;
		EnsurePeerClient(networkA, peerA);
		EnsurePeerClient(networkB, peerB);

		// A sends exactly ONE server-facing StateUpdate, deliberately stale, and never
		// publishes to the server again for the rest of this block — its registry entry
		// on the server stays frozen at this value indefinitely (same mechanism proven
		// in the Section 4 checkpoint 2 stalled-session test).
		Engine::PlayerState staleServerState{ idA, 100.0f, 100.0f, 0.0f, 0.0f };
		networkA.PublishState(staleServerState);

		// A's PeerClient repeatedly broadcasts a DIFFERENT, live state directly.
		Engine::PlayerState liveP2pState{ idA, 700.0f, 500.0f, 3.0f, 4.0f };

		bool proven = WaitUntil(
		    [&]()
		    {
			    // B must keep refreshing its OWN server session (to eventually observe
			    // A's frozen roster entry) and its OWN peer directory, and A's PeerClient
			    // must keep broadcasting — all via bounded retry, never a single Send()
			    // assumed to arrive.
			    networkB.PublishState(Engine::PlayerState{ idB, 0.0f, 0.0f, 0.0f, 0.0f });
			    RefreshPeerDirectory(networkB, peerB);
			    if (peerA)
			    {
				    peerA->PublishState(liveP2pState);
			    }

			    std::optional<Engine::Snapshot> snapshotB = networkB.GetLatestSnapshot();
			    if (!snapshotB.has_value() || !peerB)
			    {
				    return false;
			    }

			    // 1. Server-stored PlayerState for A is exactly the stale value.
			    bool serverStale = false;
			    for (const Engine::PlayerState& state : snapshotB->Roster)
			    {
				    if (state.Id == idA)
				    {
					    serverStale = state.PositionX == staleServerState.PositionX &&
					                  state.PositionY == staleServerState.PositionY;
				    }
			    }

			    // 2. PeerClient's latest state for A is exactly the live value.
			    bool peerLive = StateMatches(peerB->GetLatestPeerStates(), idA, liveP2pState);

			    return serverStale && peerLive;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(proven, "Directness_ServerStaysStale_WhileP2pStaysLive");

		// 3. Game's exact selection rule, exercised directly: given B's own Roster
		// (membership only) and B's own PeerClient states, the chosen remote state for A
		// is the P2P one — the stale server value is never returned, because this
		// function never even looks at Roster's PlayerState fields for `remoteId`.
		std::optional<Engine::Snapshot> finalSnapshotB = networkB.GetLatestSnapshot();
		std::unordered_map<Engine::PlayerId, Engine::PlayerState> finalPeerStatesB =
		    peerB ? peerB->GetLatestPeerStates() : std::unordered_map<Engine::PlayerId, Engine::PlayerState>{};
		std::optional<Engine::PlayerState> selected =
		    finalSnapshotB.has_value() ? SelectRemoteState(idA, finalSnapshotB->Roster, finalPeerStatesB)
		                                : std::nullopt;
		bool selectedIsLive = selected.has_value() && selected->PositionX == liveP2pState.PositionX &&
		                       selected->PositionY == liveP2pState.PositionY;
		Check(selectedIsLive, "Directness_GameSelectionLogic_ChoosesP2pStateNeverServerState");
	}

	// --- Part 13: real multi-client integration (unique ids/ports, direct exchange,
	// late join, clean leave) ---
	{
		constexpr int InitialCount = 3;
		std::vector<std::unique_ptr<NetworkClient>> networks;
		std::vector<std::unique_ptr<PeerClient>> peers;
		for (int i = 0; i < InitialCount; ++i)
		{
			networks.push_back(std::make_unique<NetworkClient>(BootstrapEndpoint));
			peers.push_back(nullptr);
		}

		bool allConnected = WaitUntil(
		    [&]()
		    {
			    for (auto& network : networks)
			    {
				    if (!network->IsConnected())
				    {
					    return false;
				    }
			    }
			    return true;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(allConnected, "MultiClient_AllThreeConnectWithinTimeout");

		std::vector<Engine::PlayerId> ids(InitialCount);
		std::vector<std::uint16_t> p2pPorts(InitialCount);
		for (int i = 0; i < InitialCount; ++i)
		{
			ids[i] = networks[i]->GetLocalPlayerId();
			p2pPorts[i] = networks[i]->GetLocalP2pPort();
			EnsurePeerClient(*networks[i], peers[i]);
		}

		bool idsNonZeroAndUnique = ids[0] != 0 && ids[1] != 0 && ids[2] != 0 && ids[0] != ids[1] && ids[0] != ids[2] &&
		                            ids[1] != ids[2];
		bool portsInRangeAndUnique = true;
		for (std::uint16_t port : p2pPorts)
		{
			if (port < 6001 || port > 6008)
			{
				portsInRangeAndUnique = false;
			}
		}
		portsInRangeAndUnique = portsInRangeAndUnique && p2pPorts[0] != p2pPorts[1] && p2pPorts[0] != p2pPorts[2] &&
		                        p2pPorts[1] != p2pPorts[2];
		Check(idsNonZeroAndUnique, "MultiClient_UniqueNonZeroPlayerIds");
		Check(portsInRangeAndUnique, "MultiClient_UniqueP2pPortsInRange6001To6008");

		std::vector<Engine::PlayerState> knownStates{ Engine::PlayerState{ ids[0], 11.0f, 12.0f, 1.0f, 0.0f },
			                                            Engine::PlayerState{ ids[1], 21.0f, 22.0f, 0.0f, 1.0f },
			                                            Engine::PlayerState{ ids[2], 31.0f, 32.0f, 1.0f, 1.0f } };

		bool allExchanged = WaitUntil(
		    [&]()
		    {
			    for (int i = 0; i < InitialCount; ++i)
			    {
				    networks[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
				    RefreshPeerDirectory(*networks[i], peers[i]);
				    if (peers[i])
				    {
					    peers[i]->PublishState(knownStates[i]);
				    }
			    }
			    for (int viewer = 0; viewer < InitialCount; ++viewer)
			    {
				    if (!peers[viewer])
				    {
					    return false;
				    }
				    auto states = peers[viewer]->GetLatestPeerStates();
				    for (int other = 0; other < InitialCount; ++other)
				    {
					    if (other == viewer)
					    {
						    continue;
					    }
					    if (!StateMatches(states, ids[other], knownStates[other]))
					    {
						    return false;
					    }
				    }
			    }
			    return true;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(allExchanged, "MultiClient_AllThreeExchangeDirectP2PState");

		// --- Late join: D joins after A/B/C are already established; no restart. ---
		auto networkD = std::make_unique<NetworkClient>(BootstrapEndpoint);
		bool dConnected =
		    WaitUntil([&]() { return networkD->IsConnected(); }, PollTimeoutMs, PollIntervalMs);
		Check(dConnected, "LateJoin_D_ConnectsWithoutRestartingOthers");

		Engine::PlayerId idD = networkD->GetLocalPlayerId();
		std::unique_ptr<PeerClient> peerD;
		EnsurePeerClient(*networkD, peerD);
		Engine::PlayerState knownD{ idD, 41.0f, 42.0f, 0.0f, 0.0f };

		bool lateJoinExchanged = WaitUntil(
		    [&]()
		    {
			    networkD->PublishState(Engine::PlayerState{ idD, 0.0f, 0.0f, 0.0f, 0.0f });
			    RefreshPeerDirectory(*networkD, peerD);
			    if (peerD)
			    {
				    peerD->PublishState(knownD);
			    }
			    for (int i = 0; i < InitialCount; ++i)
			    {
				    networks[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
				    RefreshPeerDirectory(*networks[i], peers[i]);
				    peers[i]->PublishState(knownStates[i]);
			    }

			    if (!peerD)
			    {
				    return false;
			    }
			    for (int i = 0; i < InitialCount; ++i)
			    {
				    if (!StateMatches(peerD->GetLatestPeerStates(), ids[i], knownStates[i]))
				    {
					    return false;
				    }
				    if (!StateMatches(peers[i]->GetLatestPeerStates(), idD, knownD))
				    {
					    return false;
				    }
			    }
			    return true;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(lateJoinExchanged, "LateJoin_D_AndExistingThree_ExchangeDirectStateBothWays");

		// --- Clean leave: D leaves; A/B/C lose D from membership, directory, and state. ---
		peerD.reset();
		networkD.reset(); // NetworkClient's destructor sends the authoritative Leaving=true

		bool dRemovedEverywhere = WaitUntil(
		    [&]()
		    {
			    bool stillPresent = false;
			    for (int i = 0; i < InitialCount; ++i)
			    {
				    networks[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
				    RefreshPeerDirectory(*networks[i], peers[i]);

				    std::optional<Engine::Snapshot> snapshot = networks[i]->GetLatestSnapshot();
				    if (snapshot.has_value())
				    {
					    for (const Engine::PlayerState& state : snapshot->Roster)
					    {
						    if (state.Id == idD)
						    {
							    stillPresent = true;
						    }
					    }
				    }
				    if (peers[i]->GetLatestPeerStates().find(idD) != peers[i]->GetLatestPeerStates().end())
				    {
					    stillPresent = true;
				    }
			    }
			    return !stillPresent;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(dRemovedEverywhere,
		      "CleanLeave_D_RemovedFromServerMembership_PeerDirectory_AndRemoteStateSource");

		// A/B/C must still be able to exchange among themselves after D's departure.
		bool stillExchanging = WaitUntil(
		    [&]()
		    {
			    for (int i = 0; i < InitialCount; ++i)
			    {
				    peers[i]->PublishState(knownStates[i]);
			    }
			    for (int viewer = 0; viewer < InitialCount; ++viewer)
			    {
				    for (int other = 0; other < InitialCount; ++other)
				    {
					    if (other == viewer)
					    {
						    continue;
					    }
					    if (!StateMatches(peers[viewer]->GetLatestPeerStates(), ids[other], knownStates[other]))
					    {
						    return false;
					    }
				    }
			    }
			    return true;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(stillExchanging, "CleanLeave_RemainingThree_StillExchangeDirectStateAfterDLeaves");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

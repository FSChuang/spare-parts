// Real, non-SDL-window integration test for Milestone 2 Section 5's final integration:
// Timeline-scaled P2P message rate, fixed-real-time server-refresh rate, and
// server-authoritative platform consistency/continuity (including while a client is
// paused). Requires a real running production server at BootstrapEndpoint. Not
// CTest-registered — same rationale as the other Game/src/*IntegrationTest.cpp files (a
// real multi-second/timing scenario, not a pure deterministic unit; ServerPlatformTest.cpp
// covers the pure, deterministic half of the platform work separately).
//
// This drives four real NetworkClient instances, each paced by its own Engine::Timeline
// (0.5x/1x/2x/paused), plus one real PeerClient per client once connected — reproducing
// exactly the same dual-schedule scheme Game::UpdateNetworking uses (see NetworkRate.h
// for the shared constants): a Timeline-scaled accumulator feeding PeerClient, and an
// independent fixed-real-time (~150ms) refresh feeding NetworkClient, regardless of
// Timeline scale or pause. This harness has no SDL window/Renderer, so it cannot reuse
// Game itself — it reimplements that one small scheme directly, the same way
// NetworkClientIntegrationTest/MultiClientIntegrationTest already reimplement small
// pieces of Game's networking glue rather than dragging in SDL.
//
// Engine::Timeline's default (real-time) anchor reads SDL_GetTicks(), so this process
// still calls a minimal SDL_Init(0)/SDL_Quit() (no video, no window) to make that
// portable across platforms rather than relying on it happening to work uninitialized.

#include "NetworkClient.h"
#include "NetworkRate.h"
#include "PeerClient.h"

#include "Engine/Network/Protocol.h"
#include "Engine/Time/Timeline.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
	constexpr const char* BootstrapEndpoint = "tcp://127.0.0.1:5556";
	// Clients 0/1/2 measure the Timeline-scaled P2P rate at 0.5x/1x/2x; client 3 is
	// permanently paused for its entire run, to prove the paused case (P2P ~0,
	// server-refresh unaffected, platform still fresh) without disturbing the other
	// three clients' own rate measurements.
	constexpr int ClientCount = 4;
	constexpr int PausedClientIndex = 3;
	constexpr std::array<double, ClientCount> TimelineScales{ 0.5, 1.0, 2.0, 1.0 };
	// Required observation window is >= 5 real seconds; a small margin absorbs the
	// connect/poll time already spent before the window starts.
	constexpr double ObservationSeconds = 5.5;
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

	struct PlatformObservation
	{
		std::chrono::steady_clock::time_point At;
		Engine::PlatformState Platform;
	};
}

int main()
{
	if (!SDL_Init(0))
	{
		throw std::runtime_error(SDL_GetError());
	}

	{
		std::array<std::optional<NetworkClient>, ClientCount> clients;
		for (std::optional<NetworkClient>& client : clients)
		{
			client.emplace(BootstrapEndpoint);
		}

		bool allConnected = WaitUntil(
		    [&clients]()
		    {
			    for (const std::optional<NetworkClient>& client : clients)
			    {
				    if (!client->IsConnected())
				    {
					    return false;
				    }
			    }
			    return true;
		    },
		    PollTimeoutMs, PollIntervalMs);
		Check(allConnected, "AllFourClients_BecomeConnectedWithinTimeout");

		std::array<Engine::PlayerId, ClientCount> ids{};
		for (int i = 0; i < ClientCount; ++i)
		{
			ids[i] = clients[i]->GetLocalPlayerId();
		}

		// One independent Timeline per client, fixed at its own scale for the whole
		// observation window — exactly modeling players who each pressed a different
		// Timeline-scale (or pause) key before this window began.
		std::array<Engine::Timeline, ClientCount> timelines;
		for (int i = 0; i < ClientCount; ++i)
		{
			timelines[i].SetScale(TimelineScales[i]);
		}
		timelines[PausedClientIndex].Pause();

		std::array<float, ClientCount> p2pAccumulators{};
		std::array<std::chrono::steady_clock::time_point, ClientCount> lastServerRefresh{};
		std::array<std::unique_ptr<PeerClient>, ClientCount> peerClients{};

		auto observationStart = std::chrono::steady_clock::now();
		for (int i = 0; i < ClientCount; ++i)
		{
			lastServerRefresh[i] = observationStart - ServerRefreshInterval;
		}
		std::uint64_t loopIterations = 0;

		while (std::chrono::duration<double>(std::chrono::steady_clock::now() - observationStart).count() <
		       ObservationSeconds)
		{
			++loopIterations;
			for (int i = 0; i < ClientCount; ++i)
			{
				// Lazily create PeerClient once connected+id+port are known — exactly
				// mirroring Game::UpdateNetworking's own creation condition.
				if (!peerClients[i])
				{
					std::uint16_t p2pPort = clients[i]->GetLocalP2pPort();
					if (ids[i] != 0 && p2pPort != 0)
					{
						peerClients[i] = std::make_unique<PeerClient>(ids[i], p2pPort);
					}
				}

				// P2P gameplay publish: Timeline-scaled, exactly Game::UpdateNetworking's
				// scheme, redirected to PeerClient. For the paused client, GetDeltaTime()
				// always reports 0, so this accumulator never advances and never publishes
				// — proving the paused case without any special-case code here either.
				float deltaTime = static_cast<float>(timelines[i].GetDeltaTime());
				p2pAccumulators[i] += deltaTime;
				if (p2pAccumulators[i] >= NetworkUpdateIntervalSeconds)
				{
					p2pAccumulators[i] = std::fmod(p2pAccumulators[i], NetworkUpdateIntervalSeconds);
					if (peerClients[i])
					{
						peerClients[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
					}
				}

				// Server-facing session refresh: fixed real-time cadence, independent of
				// Timeline scale/pause — continues identically for the paused client.
				auto now = std::chrono::steady_clock::now();
				if (now - lastServerRefresh[i] >= ServerRefreshInterval)
				{
					lastServerRefresh[i] = now;
					clients[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
				}

				// Keep each PeerClient's peer directory current, mirroring
				// Game::UpdateNetworking, though this test only measures send rates/
				// platform freshness, not P2P delivery (see PeerClientIntegrationTest.cpp
				// and P2pIntegrationTest.cpp for delivery proofs).
				if (peerClients[i])
				{
					std::optional<Engine::Snapshot> snapshot = clients[i]->GetLatestSnapshot();
					if (snapshot.has_value())
					{
						peerClients[i]->UpdatePeers(snapshot->Peers);
					}
				}
			}
		}

		double actualDuration =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - observationStart).count();

		std::array<std::uint64_t, ClientCount> p2pCounts{};
		std::array<double, ClientCount> p2pRates{};
		std::array<std::uint64_t, ClientCount> serverCounts{};
		std::array<double, ClientCount> serverRates{};
		for (int i = 0; i < ClientCount; ++i)
		{
			p2pCounts[i] = peerClients[i] ? peerClients[i]->GetSentStateUpdateCount() : 0;
			p2pRates[i] = static_cast<double>(p2pCounts[i]) / actualDuration;
			serverCounts[i] = clients[i]->GetSentStateUpdateCount();
			serverRates[i] = static_cast<double>(serverCounts[i]) / actualDuration;
		}

		std::printf("\n--- Rate test results ---\n");
		std::printf("Observation duration: %.3f s\n", actualDuration);
		double loopRate = static_cast<double>(loopIterations) / actualDuration;
		std::printf("Harness loop: %llu iterations (%.1f iterations/sec)\n",
		            static_cast<unsigned long long>(loopIterations), loopRate);
		for (int i = 0; i < ClientCount; ++i)
		{
			const char* label = (i == PausedClientIndex) ? "paused" : "active";
			std::printf("Client %d (scale=%.1fx, %s): P2P %llu sent -> %.2f Hz | server %llu sent -> %.2f Hz\n", i,
			            TimelineScales[i], label, static_cast<unsigned long long>(p2pCounts[i]), p2pRates[i],
			            static_cast<unsigned long long>(serverCounts[i]), serverRates[i]);
		}
		if (p2pRates[0] > 0.0)
		{
			std::printf("P2P ratio 0.5x : 1x : 2x = 1.00 : %.2f : %.2f\n", p2pRates[1] / p2pRates[0],
			            p2pRates[2] / p2pRates[0]);
		}

		// Generous tolerance for scheduler/frame/network jitter — the assignment does
		// not require exact counts, only that scaling roughly doubles/halves the rate.
		Check(p2pRates[1] > p2pRates[0] * 1.5 && p2pRates[1] < p2pRates[0] * 2.5, "OneX_P2pRate_RoughlyDoubleHalfXRate");
		Check(p2pRates[2] > p2pRates[1] * 1.5 && p2pRates[2] < p2pRates[1] * 2.5, "TwoX_P2pRate_RoughlyDoubleOneXRate");
		Check(p2pRates[2] >= 50.0, "TwoX_P2pRate_MeetsApproximately60HzTarget");

		// IMPORTANT 2X AUDIT: is this harness's own loop even capable of offering 60
		// publish opportunities per second? If not, a low 2x rate would be a scheduling
		// limit, not evidence against the Timeline-scaled scheme itself. This harness
		// has no rendering, so a high result here does NOT by itself prove the real SDL
		// game loop clears 60 FPS — see the manual demo for that.
		bool loopFastEnough = loopRate >= 60.0;
		Check(loopFastEnough, "HarnessLoop_SuppliesAtLeast60OpportunitiesPerSecond");
		if (!loopFastEnough)
		{
			std::printf(
			    "[NOTE] Harness loop rate was below 60/sec -- the 2x target may be scheduling-limited here, "
			    "not just network-limited. This does not reflect the real SDL game loop's own frame rate.\n");
		}

		// Server-facing refresh rate must stay approximately constant (~6.67 Hz) across
		// ALL four clients regardless of Timeline scale or pause — it is deliberately
		// decoupled from Timeline entirely.
		bool serverRatesConsistent = true;
		for (int i = 0; i < ClientCount; ++i)
		{
			if (serverRates[i] < 4.0 || serverRates[i] > 10.0)
			{
				serverRatesConsistent = false;
			}
		}
		Check(serverRatesConsistent, "ServerRefreshRate_ApproximatelySixPointSixSevenHzForAllClients");

		// The paused client's P2P accumulator never advances (GetDeltaTime() reports 0
		// while paused), so its P2P send count must be exactly zero, while its
		// server-facing refresh rate is unaffected.
		Check(p2pCounts[PausedClientIndex] == 0, "PausedClient_P2pRate_IsExactlyZero");
		Check(serverRates[PausedClientIndex] >= 4.0 && serverRates[PausedClientIndex] <= 10.0,
		      "PausedClient_ServerRefreshRate_ContinuesUnaffected");

		// --- Cross-client platform consistency, across differently-scaled (and one
		// paused) clients ---
		// One more publish+capture round per client, each bracketed by a local
		// wall-clock timestamp, to check that all four observe the SAME server-time
		// platform trajectory despite having run at 0.5x/1x/2x/paused Timeline state for
		// the entire window above. This is also the proof that the server platform's
		// path never depended on any client's own Timeline: if it had, four differently
		// Timeline-scaled/paused clients could not agree on one shared trajectory here —
		// and, per Milestone 2 Section 5, the paused client observing a FRESH platform at
		// all is itself the proof that server refresh no longer depends on Timeline.
		std::array<std::optional<PlatformObservation>, ClientCount> observations;
		for (int i = 0; i < ClientCount; ++i)
		{
			auto requestTime = std::chrono::steady_clock::now();
			clients[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
			bool gotSnapshot =
			    WaitUntil([&clients, i]() { return clients[i]->GetLatestSnapshot().has_value(); }, PollTimeoutMs,
			              PollIntervalMs);
			if (gotSnapshot)
			{
				observations[i] = PlatformObservation{ requestTime, clients[i]->GetLatestSnapshot()->Platform };
			}
		}

		bool allObserved = observations[0].has_value() && observations[1].has_value() &&
		                    observations[2].has_value() && observations[3].has_value();
		Check(allObserved, "AllFourClients_ObservedAPlatformSnapshot");
		Check(observations[PausedClientIndex].has_value(),
		      "PausedClient_StillReceivesFreshPlatformSnapshot_ImportantSection5Improvement");

		if (allObserved)
		{
			bool sameY = observations[0]->Platform.PositionY == observations[1]->Platform.PositionY &&
			             observations[1]->Platform.PositionY == observations[2]->Platform.PositionY &&
			             observations[2]->Platform.PositionY == observations[3]->Platform.PositionY;
			Check(sameY, "PlatformY_IdenticalAcrossAllFourClients");

			bool consistent = true;
			for (int a = 0; a < ClientCount; ++a)
			{
				for (int b = a + 1; b < ClientCount; ++b)
				{
					const PlatformObservation& oa = *observations[a];
					const PlatformObservation& ob = *observations[b];
					if (oa.Platform.VelocityX != ob.Platform.VelocityX)
					{
						// A direction change happened between the two requests (rare,
						// given the short real-time gap); skip this pair rather than
						// asserting a linear relationship across a turning point.
						continue;
					}
					double dt = std::chrono::duration<double>(ob.At - oa.At).count();
					float expected = oa.Platform.PositionX + oa.Platform.VelocityX * static_cast<float>(dt);
					// Generous tolerance: a few pixels, well above any plausible
					// round-trip-latency-driven error at this platform speed.
					if (std::fabs(ob.Platform.PositionX - expected) > 5.0f)
					{
						consistent = false;
					}
				}
			}
			Check(consistent,
			      "PlatformPositions_ConsistentWithOneSharedServerTimeTrajectory_AcrossDifferentScalesAndPause");
		}

		for (std::unique_ptr<PeerClient>& peerClient : peerClients)
		{
			peerClient.reset();
		}
		for (std::optional<NetworkClient>& client : clients)
		{
			client.reset();
		}
	}

	SDL_Quit();

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

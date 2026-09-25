// Real, non-SDL-window integration test for Milestone 2 Section 4's final checkpoint:
// Timeline-scaled message rate AND server-authoritative platform consistency. Requires
// a real running production server at BootstrapEndpoint. Not CTest-registered — same
// rationale as the other Game/src/*IntegrationTest.cpp files (a real multi-second/timing
// scenario, not a pure deterministic unit; ServerPlatformTest.cpp covers the pure,
// deterministic half of the platform work separately).
//
// This drives three real NetworkClient instances, each paced by its own
// Engine::Timeline at a different scale (0.5x/1x/2x), reproducing exactly the same
// accumulator scheme Game::UpdateNetworking uses (see NetworkRate.h for the shared
// constant). This harness has no SDL window/Renderer, so it cannot reuse Game itself —
// it reimplements that one small scheme directly, the same way
// NetworkClientIntegrationTest/MultiClientIntegrationTest already reimplement small
// pieces of Game's networking glue rather than dragging in SDL.
//
// Engine::Timeline's default (real-time) anchor reads SDL_GetTicks(), so this process
// still calls a minimal SDL_Init(0)/SDL_Quit() (no video, no window) to make that
// portable across platforms rather than relying on it happening to work uninitialized.

#include "NetworkClient.h"
#include "NetworkRate.h"

#include "Engine/Network/Protocol.h"
#include "Engine/Time/Timeline.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <thread>

namespace
{
	constexpr const char* BootstrapEndpoint = "tcp://127.0.0.1:5556";
	constexpr int ClientCount = 3;
	constexpr std::array<double, ClientCount> TimelineScales{ 0.5, 1.0, 2.0 };
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
		Check(allConnected, "AllThreeClients_BecomeConnectedWithinTimeout");

		std::array<Engine::PlayerId, ClientCount> ids{};
		for (int i = 0; i < ClientCount; ++i)
		{
			ids[i] = clients[i]->GetLocalPlayerId();
		}

		// One independent Timeline per client, each fixed at its own scale for the
		// whole observation window — exactly modeling three players who each pressed a
		// different Timeline-scale key before this window began.
		std::array<Engine::Timeline, ClientCount> timelines;
		for (int i = 0; i < ClientCount; ++i)
		{
			timelines[i].SetScale(TimelineScales[i]);
		}

		std::array<float, ClientCount> accumulators{};

		auto observationStart = std::chrono::steady_clock::now();
		std::uint64_t loopIterations = 0;

		while (std::chrono::duration<double>(std::chrono::steady_clock::now() - observationStart).count() <
		       ObservationSeconds)
		{
			++loopIterations;
			for (int i = 0; i < ClientCount; ++i)
			{
				// Exactly Game::UpdateNetworking's scheme: accumulate Timeline-scaled
				// deltaTime, publish at most once per interval crossed, collapse any
				// extra crossed intervals via fmod rather than bursting.
				float deltaTime = static_cast<float>(timelines[i].GetDeltaTime());
				accumulators[i] += deltaTime;
				if (accumulators[i] >= NetworkUpdateIntervalSeconds)
				{
					accumulators[i] = std::fmod(accumulators[i], NetworkUpdateIntervalSeconds);
					clients[i]->PublishState(Engine::PlayerState{ ids[i], 0.0f, 0.0f, 0.0f, 0.0f });
				}
			}
		}

		double actualDuration = std::chrono::duration<double>(std::chrono::steady_clock::now() - observationStart).count();

		std::array<std::uint64_t, ClientCount> counts{};
		std::array<double, ClientCount> rates{};
		for (int i = 0; i < ClientCount; ++i)
		{
			counts[i] = clients[i]->GetSentStateUpdateCount();
			rates[i] = static_cast<double>(counts[i]) / actualDuration;
		}

		std::printf("\n--- Rate test results ---\n");
		std::printf("Observation duration: %.3f s\n", actualDuration);
		double loopRate = static_cast<double>(loopIterations) / actualDuration;
		std::printf("Harness loop: %llu iterations (%.1f iterations/sec)\n",
		            static_cast<unsigned long long>(loopIterations), loopRate);
		for (int i = 0; i < ClientCount; ++i)
		{
			std::printf("Client %d (scale=%.1fx): %llu StateUpdates sent -> %.2f Hz\n", i, TimelineScales[i],
			            static_cast<unsigned long long>(counts[i]), rates[i]);
		}
		if (rates[0] > 0.0)
		{
			std::printf("Ratio 0.5x : 1x : 2x = 1.00 : %.2f : %.2f\n", rates[1] / rates[0], rates[2] / rates[0]);
		}

		// Generous tolerance for scheduler/frame/network jitter — the assignment does
		// not require exact counts, only that scaling roughly doubles/halves the rate.
		Check(rates[1] > rates[0] * 1.5 && rates[1] < rates[0] * 2.5, "OneX_Rate_RoughlyDoubleHalfXRate");
		Check(rates[2] > rates[1] * 1.5 && rates[2] < rates[1] * 2.5, "TwoX_Rate_RoughlyDoubleOneXRate");
		Check(rates[2] >= 50.0, "TwoX_Rate_MeetsApproximately60HzTarget");

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

		// --- Cross-client platform consistency, across three DIFFERENTLY-scaled clients ---
		// One more publish+capture round per client, each bracketed by a local
		// wall-clock timestamp, to check that all three observe the SAME server-time
		// platform trajectory despite having run at 0.5x/1x/2x Timeline scale for the
		// entire window above. This is also the proof that the server platform's path
		// never depended on any client's own Timeline: if it had, three differently
		// Timeline-scaled clients could not agree on one shared trajectory here.
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

		bool allObserved = observations[0].has_value() && observations[1].has_value() && observations[2].has_value();
		Check(allObserved, "AllThreeClients_ObservedAPlatformSnapshot");

		if (allObserved)
		{
			bool sameY = observations[0]->Platform.PositionY == observations[1]->Platform.PositionY &&
			             observations[1]->Platform.PositionY == observations[2]->Platform.PositionY;
			Check(sameY, "PlatformY_IdenticalAcrossAllThreeClients");

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
			Check(consistent, "PlatformPositions_ConsistentWithOneSharedServerTimeTrajectory_AcrossDifferentScales");
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

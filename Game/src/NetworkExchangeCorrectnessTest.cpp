// Correctness validation for the CSC 581 Full-State vs Delta-State exchange strategies
// (pure logic, in-process, no sockets — NetworkPerformanceBenchmark.cpp is the real-
// ZeroMQ counterpart that measures PERFORMANCE using this exact same sender/receiver
// logic). This is the test that must pass before any timing result is trusted: given the
// identical synthetic update sequence, Full-State and Delta-State must reconstruct the
// SAME final object state. Not CTest-registered (spare-parts has no CTest setup);
// follows the same manually invoked PASS/FAIL executable style as the other
// Game/src/*Test.cpp files.

#include "Engine/Network/ExchangeProtocol.h"

#include "ExchangeStateTracking.h"
#include "SyntheticScene.h"

#include <cstdio>
#include <unordered_map>

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

	bool StatesEqual(const Engine::ExchangeObjectState& a, const Engine::ExchangeObjectState& b)
	{
		return a.Id == b.Id && a.PositionX == b.PositionX && a.PositionY == b.PositionY && a.VelocityX == b.VelocityX &&
		       a.VelocityY == b.VelocityY;
	}

	// Runs `iterations` steps of the deterministic synthetic scene through one strategy,
	// end to end (sender change-detection/full-send -> Engine encode -> Engine decode ->
	// receiver apply), and returns the receiver's final tracked state alongside the
	// sender's own ground-truth scene for comparison. `deltaTime` is fixed and small,
	// matching the benchmark's own per-iteration step.
	struct StrategyRunResult
	{
		std::vector<Engine::ExchangeObjectState> groundTruthScene;
		ExchangeReceiverState receiver;
		std::uint64_t totalRecordsSent = 0;
	};

	StrategyRunResult RunStrategy(Engine::ExchangeStrategy strategy, int staticCount, int movingCount, int iterations,
	                               float deltaTime)
	{
		StrategyRunResult result;
		result.groundTruthScene = BuildInitialScene(staticCount, movingCount);
		ExchangeSenderState senderState;

		for (int iteration = 0; iteration < iterations; ++iteration)
		{
			AdvanceMovingObjects(result.groundTruthScene, movingCount, deltaTime);

			std::vector<Engine::ExchangeObjectState> toSend;
			if (strategy == Engine::ExchangeStrategy::FullState)
			{
				toSend = result.groundTruthScene;
			}
			else
			{
				toSend = senderState.ComputeChanged(result.groundTruthScene);
			}

			std::optional<std::vector<std::uint8_t>> encoded =
			    Engine::EncodeExchangeFrame(strategy, static_cast<std::uint32_t>(iteration), toSend);
			if (!encoded.has_value())
			{
				continue; // would only happen above MaxExchangeObjects; not exercised here
			}

			std::optional<Engine::ExchangeFrame> decoded = Engine::DecodeExchangeFrame(*encoded);
			if (!decoded.has_value())
			{
				continue; // should never happen for a frame this same code just encoded
			}

			result.receiver.Apply(*decoded);
			result.totalRecordsSent += decoded->Objects.size();
		}

		return result;
	}
}

int main()
{
	constexpr int StaticCount = 30;
	constexpr int MovingCount = 20;
	constexpr int Iterations = 50;
	constexpr float DeltaTime = 1.0f / 60.0f;

	// --- Full-state round trip: every iteration's receiver state matches the sender's
	// ground truth exactly (checked only at the end here; the per-field equality check
	// below is the real assertion). ---
	StrategyRunResult fullResult =
	    RunStrategy(Engine::ExchangeStrategy::FullState, StaticCount, MovingCount, Iterations, DeltaTime);
	Check(fullResult.receiver.Objects().size() == static_cast<std::size_t>(StaticCount + MovingCount),
	      "FullState_RoundTrip_ReceiverTracksEveryObject");

	// --- Delta round trip ---
	StrategyRunResult deltaResult =
	    RunStrategy(Engine::ExchangeStrategy::DeltaState, StaticCount, MovingCount, Iterations, DeltaTime);
	Check(deltaResult.receiver.Objects().size() == static_cast<std::size_t>(StaticCount + MovingCount),
	      "DeltaState_RoundTrip_ReceiverEventuallyTracksEveryObject");

	// --- Unchanged object omission / changed object inclusion ---
	// Iteration 0 must carry every object (nothing has a "last sent" value yet); every
	// iteration after that must carry exactly the moving objects (all of them have
	// nonzero velocity, so all of them change every step) and none of the static ones.
	{
		ExchangeSenderState senderState;
		std::vector<Engine::ExchangeObjectState> scene = BuildInitialScene(StaticCount, MovingCount);

		AdvanceMovingObjects(scene, MovingCount, DeltaTime);
		std::vector<Engine::ExchangeObjectState> firstChanged = senderState.ComputeChanged(scene);
		Check(firstChanged.size() == static_cast<std::size_t>(StaticCount + MovingCount),
		      "DeltaState_FirstIteration_SendsCompleteInitialState");

		AdvanceMovingObjects(scene, MovingCount, DeltaTime);
		std::vector<Engine::ExchangeObjectState> secondChanged = senderState.ComputeChanged(scene);
		Check(secondChanged.size() == static_cast<std::size_t>(MovingCount),
		      "DeltaState_SubsequentIteration_OmitsUnchangedStaticObjects");
		bool allMoving = true;
		for (const Engine::ExchangeObjectState& object : secondChanged)
		{
			if (object.Id <= static_cast<std::uint32_t>(StaticCount))
			{
				allMoving = false;
			}
		}
		Check(allMoving, "DeltaState_SubsequentIteration_IncludesOnlyChangedMovingObjects");
	}

	// --- Reconstruction equivalence: Full-State receiver == Delta-State receiver ==
	// ground truth, for every object field, after the same number of iterations applied
	// to the same initial scene. This is the core semantic requirement: both strategies
	// must reproduce equivalent final object state. ---
	{
		bool allEqual = fullResult.groundTruthScene.size() == deltaResult.groundTruthScene.size();
		for (std::size_t i = 0; allEqual && i < fullResult.groundTruthScene.size(); ++i)
		{
			allEqual = StatesEqual(fullResult.groundTruthScene[i], deltaResult.groundTruthScene[i]);
		}
		Check(allEqual, "GroundTruth_IdenticalBetweenStrategies_SameDeterministicUpdateSequence");

		const auto& fullObjects = fullResult.receiver.Objects();
		const auto& deltaObjects = deltaResult.receiver.Objects();
		bool receiversMatch = fullObjects.size() == deltaObjects.size();
		for (const auto& [id, fullState] : fullObjects)
		{
			if (!receiversMatch)
			{
				break;
			}
			auto it = deltaObjects.find(id);
			receiversMatch = it != deltaObjects.end() && StatesEqual(it->second, fullState);
		}
		Check(receiversMatch, "FullAndDeltaReceivers_ReconstructEquivalentFinalState");

		// And against the sender's own ground truth, not just against each other.
		bool matchesGroundTruth = fullObjects.size() == fullResult.groundTruthScene.size();
		for (const Engine::ExchangeObjectState& truth : fullResult.groundTruthScene)
		{
			if (!matchesGroundTruth)
			{
				break;
			}
			auto it = fullObjects.find(truth.Id);
			matchesGroundTruth = it != fullObjects.end() && StatesEqual(it->second, truth);
		}
		Check(matchesGroundTruth, "FullStateReceiver_MatchesSenderGroundTruthExactly");
	}

	// --- No object-id collisions: StaticCount+MovingCount distinct ids never merge into
	// fewer tracked entries. ---
	Check(fullResult.receiver.Objects().size() == static_cast<std::size_t>(StaticCount + MovingCount) &&
	          deltaResult.receiver.Objects().size() == static_cast<std::size_t>(StaticCount + MovingCount),
	      "NoObjectIdCollisions_EveryDistinctIdTrackedSeparately");

	// --- Counts are correct: Delta's total records sent across the whole run is exactly
	// (every object once, on iteration 0) + (every moving object, on each subsequent
	// iteration) — a closed-form check independent of the equivalence check above. ---
	std::uint64_t expectedDeltaRecords = static_cast<std::uint64_t>(StaticCount + MovingCount) +
	                                      static_cast<std::uint64_t>(MovingCount) * static_cast<std::uint64_t>(Iterations - 1);
	Check(deltaResult.totalRecordsSent == expectedDeltaRecords, "DeltaState_TotalRecordsSent_MatchesClosedFormExpectation");

	// Full-State's total is exactly (every object) * iterations — no omission logic at all.
	std::uint64_t expectedFullRecords =
	    static_cast<std::uint64_t>(StaticCount + MovingCount) * static_cast<std::uint64_t>(Iterations);
	Check(fullResult.totalRecordsSent == expectedFullRecords, "FullState_TotalRecordsSent_MatchesClosedFormExpectation");

	// Delta sends strictly fewer total records than Full for this mixed static/moving
	// scene — the whole point of the strategy; if this ever failed it would mean the
	// "omit unchanged objects" behavior regressed.
	Check(deltaResult.totalRecordsSent < fullResult.totalRecordsSent,
	      "DeltaState_SendsFewerTotalRecordsThanFullState_ForAMixedScene");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

// CSC 581 performance checkpoint: real-ZeroMQ benchmark comparing the Full-State and
// Delta-State object-exchange strategies (see Engine/Network/ExchangeProtocol.h and
// ExchangeStateTracking.h) under three experiment families (object-count scaling,
// client-count scaling, static/moving scene composition).
//
// Topology (identical for both strategies — the data FORMAT is the only independent
// variable): one PUB socket (the "server"/publisher) binds; `client_count` SUB sockets
// each run on their own thread, connect, and subscribe to everything — the same
// fire-and-forget PUB/SUB pattern PeerClient already uses in the real game. Each socket
// is created, used, and destroyed entirely on one thread (thread-affine, matching
// Engine::Socket's own contract).
//
// Timing boundary: wall-clock elapsed from the start of the measured loop (first
// encode+send of the real iteration sequence) until the SLOWEST receiver has fully
// decoded and applied the LAST iteration's frame — i.e. the complete
// encode+send+receive+decode+apply path, not just encoding. A short warmup burst (sent
// before timing starts) establishes every subscriber's connection first, avoiding
// ZeroMQ's PUB/SUB "slow joiner" effect from being measured as part of every condition.
//
// Byte/message accounting: PUB/SUB fans one logical send out to every connected
// subscriber over its own connection, so `total_messages_sent`/`total_bytes_sent` are
// counted at the WIRE level (iterations * client_count messages; publisher-side encoded
// bytes summed over all iterations, * client_count) — this is what actually changes as
// client_count grows in Experiment 2, which is the point of that experiment. See the
// benchmark README for the exact definition.

#include "Engine/Network/ExchangeProtocol.h"
#include "Engine/Network/Socket.h"

#include "BenchmarkStats.h"
#include "ExchangeStateTracking.h"
#include "SyntheticScene.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// --- Tunables (ENGINEERING_SPEC.md §9: no magic numbers) ---

	// Chosen empirically (see benchmarks/README.md): large enough that per-iteration
	// ZeroMQ/syscall overhead dominates timer resolution noise, small enough that the
	// full suite (3 experiments x conditions x 2 strategies x 5 repetitions) completes
	// in a practical amount of time even at 500 objects x 4 receivers.
	constexpr int DefaultIterations = 20000;
	constexpr int DefaultRepetitions = 5;
	constexpr float FixedDeltaTime = 1.0f / 60.0f;

	constexpr int ReferenceClientCount = 4;
	constexpr int ReferenceMovingCount = 100;

	// Warmup: a burst of sentinel frames (ignored by receivers) sent until every receiver
	// has observed at least one, bounded by a generous timeout, plus a short fixed settle
	// delay — standard PUB/SUB "slow joiner" mitigation. None of this is included in the
	// measured elapsed time.
	constexpr std::uint32_t WarmupSentinelSeq = 0xFFFFFFFFu;
	constexpr std::chrono::milliseconds WarmupBurstInterval{ 5 };
	constexpr std::chrono::milliseconds WarmupTimeout{ 2000 };
	constexpr std::chrono::milliseconds WarmupSettleDelay{ 50 };

	// A receiver thread that has made no progress for this long is considered hung/failed
	// (e.g. a dropped frame despite warmup) rather than waited on forever.
	constexpr std::chrono::seconds ReceiverHangTimeout{ 20 };

	constexpr std::uint16_t FirstBenchmarkPort = 17000;

	// ZeroMQ's default high-water-mark (1000 outstanding messages) silently drops frames
	// once a sustained high-rate PUB/SUB exchange exceeds it — exactly what this
	// benchmark does at its larger iteration counts. Set well above the largest
	// --iterations value this benchmark is ever run with (see Engine::Socket::
	// SetHighWaterMark and SocketPubSubTest.cpp's "C" case for the underlying Engine fix
	// and its own proof).
	constexpr int SocketHighWaterMark = 200000;

	int g_PortCounter = 0;

	int NextPort()
	{
		return FirstBenchmarkPort + (g_PortCounter++);
	}

	struct ReceiverResult
	{
		std::chrono::steady_clock::time_point DoneAt;
		bool Succeeded = false;
	};

	// Runs entirely on its own thread: owns its SUB socket for its whole lifetime. Drains
	// and ignores warmup sentinel frames, applies every real frame to a local
	// ExchangeReceiverState (exercising the actual receiver logic the correctness test
	// also covers, not a stub), and reports back the moment it finished the Nth real
	// iteration.
	void ReceiverThreadMain(std::string endpoint, int expectedIterations, std::atomic<int>& warmupObservedCount,
	                         std::promise<ReceiverResult> resultPromise)
	{
		Engine::Socket socket(Engine::SocketRole::Subscribe);
		socket.SetHighWaterMark(SocketHighWaterMark);
		socket.Connect(endpoint);
		socket.Subscribe("");

		ExchangeReceiverState receiverState;
		int realReceived = 0;
		auto deadline = std::chrono::steady_clock::now() + ReceiverHangTimeout;

		while (true)
		{
			if (std::chrono::steady_clock::now() > deadline)
			{
				resultPromise.set_value(ReceiverResult{ std::chrono::steady_clock::now(), false });
				return;
			}

			std::optional<std::string> message = socket.TryReceive();
			if (!message.has_value())
			{
				std::this_thread::sleep_for(std::chrono::microseconds(100));
				continue;
			}

			std::vector<std::uint8_t> bytes(message->begin(), message->end());
			std::optional<Engine::ExchangeFrame> frame = Engine::DecodeExchangeFrame(bytes);
			if (!frame.has_value())
			{
				continue; // defensive; the publisher never sends anything malformed
			}

			if (frame->IterationSeq == WarmupSentinelSeq)
			{
				warmupObservedCount.fetch_add(1);
				continue;
			}

			receiverState.Apply(*frame);
			++realReceived;
			deadline = std::chrono::steady_clock::now() + ReceiverHangTimeout;

			if (realReceived == expectedIterations)
			{
				resultPromise.set_value(ReceiverResult{ std::chrono::steady_clock::now(), true });
				return;
			}
		}
	}

	struct ConditionConfig
	{
		std::string Experiment;
		Engine::ExchangeStrategy Strategy;
		int ClientCount;
		int StaticCount;
		int MovingCount;
		int Iterations;
	};

	struct RunMetrics
	{
		double ElapsedMs;
		std::uint64_t TotalMessagesSent;
		std::uint64_t TotalBytesSent;
	};

	const char* StrategyName(Engine::ExchangeStrategy strategy)
	{
		return strategy == Engine::ExchangeStrategy::FullState ? "Full" : "Delta";
	}

	RunMetrics RunOneRepetition(const ConditionConfig& config)
	{
		std::string endpoint = "tcp://127.0.0.1:" + std::to_string(NextPort());

		Engine::Socket publisher(Engine::SocketRole::Publish);
		publisher.SetHighWaterMark(SocketHighWaterMark);
		publisher.Bind(endpoint);

		std::atomic<int> warmupObservedCount{ 0 };
		std::vector<std::thread> receiverThreads;
		std::vector<std::future<ReceiverResult>> receiverFutures;
		receiverThreads.reserve(static_cast<std::size_t>(config.ClientCount));
		receiverFutures.reserve(static_cast<std::size_t>(config.ClientCount));

		for (int i = 0; i < config.ClientCount; ++i)
		{
			std::promise<ReceiverResult> promise;
			receiverFutures.push_back(promise.get_future());
			receiverThreads.emplace_back(ReceiverThreadMain, endpoint, config.Iterations, std::ref(warmupObservedCount),
			                              std::move(promise));
		}

		// Warmup burst: establishes every subscriber's connection before any timed frame
		// is sent (PUB/SUB "slow joiner" mitigation), bounded by WarmupTimeout so a
		// genuinely stuck receiver does not hang the whole suite.
		std::optional<std::vector<std::uint8_t>> warmupFrame =
		    Engine::EncodeExchangeFrame(Engine::ExchangeStrategy::FullState, WarmupSentinelSeq, {});
		auto warmupDeadline = std::chrono::steady_clock::now() + WarmupTimeout;
		while (warmupObservedCount.load() < config.ClientCount && std::chrono::steady_clock::now() < warmupDeadline)
		{
			publisher.Send(std::string(warmupFrame->begin(), warmupFrame->end()));
			std::this_thread::sleep_for(WarmupBurstInterval);
		}
		std::this_thread::sleep_for(WarmupSettleDelay);

		std::vector<Engine::ExchangeObjectState> scene = BuildInitialScene(config.StaticCount, config.MovingCount);
		ExchangeSenderState senderState;
		std::uint64_t publisherBytes = 0;

		auto start = std::chrono::steady_clock::now();

		for (int iteration = 0; iteration < config.Iterations; ++iteration)
		{
			AdvanceMovingObjects(scene, config.MovingCount, FixedDeltaTime);

			std::vector<Engine::ExchangeObjectState> toSend =
			    (config.Strategy == Engine::ExchangeStrategy::FullState) ? scene : senderState.ComputeChanged(scene);

			std::optional<std::vector<std::uint8_t>> encoded =
			    Engine::EncodeExchangeFrame(config.Strategy, static_cast<std::uint32_t>(iteration), toSend);
			if (!encoded.has_value())
			{
				throw std::runtime_error("EncodeExchangeFrame failed (object count exceeded MaxExchangeObjects)");
			}

			publisher.Send(std::string(encoded->begin(), encoded->end()));
			publisherBytes += encoded->size();
		}

		bool allSucceeded = true;
		auto latestDone = start;
		for (std::future<ReceiverResult>& future : receiverFutures)
		{
			ReceiverResult result = future.get();
			allSucceeded = allSucceeded && result.Succeeded;
			if (result.DoneAt > latestDone)
			{
				latestDone = result.DoneAt;
			}
		}
		for (std::thread& thread : receiverThreads)
		{
			thread.join();
		}

		if (!allSucceeded)
		{
			throw std::runtime_error("a receiver did not finish within ReceiverHangTimeout");
		}

		RunMetrics metrics;
		metrics.ElapsedMs = std::chrono::duration<double, std::milli>(latestDone - start).count();
		metrics.TotalMessagesSent =
		    static_cast<std::uint64_t>(config.Iterations) * static_cast<std::uint64_t>(config.ClientCount);
		metrics.TotalBytesSent = publisherBytes * static_cast<std::uint64_t>(config.ClientCount);
		return metrics;
	}

	std::vector<ConditionConfig> BuildExperiment1(int iterations)
	{
		std::vector<ConditionConfig> conditions;
		for (int moving : { 10, 100, 500 })
		{
			for (Engine::ExchangeStrategy strategy : { Engine::ExchangeStrategy::FullState, Engine::ExchangeStrategy::DeltaState })
			{
				conditions.push_back(
				    ConditionConfig{ "ObjectCountScaling", strategy, ReferenceClientCount, 0, moving, iterations });
			}
		}
		return conditions;
	}

	std::vector<ConditionConfig> BuildExperiment2(int iterations)
	{
		std::vector<ConditionConfig> conditions;
		for (int clients : { 1, 2, 4 })
		{
			for (Engine::ExchangeStrategy strategy : { Engine::ExchangeStrategy::FullState, Engine::ExchangeStrategy::DeltaState })
			{
				conditions.push_back(
				    ConditionConfig{ "ClientCountScaling", strategy, clients, 0, ReferenceMovingCount, iterations });
			}
		}
		return conditions;
	}

	std::vector<ConditionConfig> BuildExperiment3(int iterations)
	{
		std::vector<ConditionConfig> conditions;
		for (auto [staticCount, movingCount] : { std::pair{ 500, 0 }, std::pair{ 450, 50 }, std::pair{ 250, 250 },
			                                      std::pair{ 0, 500 } })
		{
			for (Engine::ExchangeStrategy strategy : { Engine::ExchangeStrategy::FullState, Engine::ExchangeStrategy::DeltaState })
			{
				conditions.push_back(ConditionConfig{ "SceneComposition", strategy, ReferenceClientCount, staticCount,
					                                   movingCount, iterations });
			}
		}
		return conditions;
	}

	struct CliOptions
	{
		int Iterations = DefaultIterations;
		int Repetitions = DefaultRepetitions;
		std::string Experiment = "all"; // "1", "2", "3", or "all"
		std::string OutDir = "benchmark_results";
	};

	// Deliberately simple: argv parsing is not the interesting part of this tool.
	CliOptions ParseArgs(int argc, char* argv[])
	{
		CliOptions options;
		for (int i = 1; i < argc; ++i)
		{
			std::string arg = argv[i];
			auto value = [&](const std::string& prefix) -> std::optional<std::string>
			{
				if (arg.rfind(prefix, 0) == 0)
				{
					return arg.substr(prefix.size());
				}
				return std::nullopt;
			};

			if (auto v = value("--iterations=")) options.Iterations = std::stoi(*v);
			else if (auto v = value("--repetitions=")) options.Repetitions = std::stoi(*v);
			else if (auto v = value("--experiment=")) options.Experiment = *v;
			else if (auto v = value("--out-dir=")) options.OutDir = *v;
			else
			{
				std::fprintf(stderr, "Unrecognized argument: %s\n", arg.c_str());
				std::fprintf(stderr,
				             "Usage: NetworkPerformanceBenchmark [--experiment=1|2|3|all] [--iterations=N] "
				             "[--repetitions=N] [--out-dir=path]\n");
				std::exit(1);
			}
		}
		return options;
	}
}

int main(int argc, char* argv[])
{
	CliOptions options = ParseArgs(argc, argv);

	std::vector<ConditionConfig> conditions;
	if (options.Experiment == "1" || options.Experiment == "all")
	{
		std::vector<ConditionConfig> e1 = BuildExperiment1(options.Iterations);
		conditions.insert(conditions.end(), e1.begin(), e1.end());
	}
	if (options.Experiment == "2" || options.Experiment == "all")
	{
		std::vector<ConditionConfig> e2 = BuildExperiment2(options.Iterations);
		conditions.insert(conditions.end(), e2.begin(), e2.end());
	}
	if (options.Experiment == "3" || options.Experiment == "all")
	{
		std::vector<ConditionConfig> e3 = BuildExperiment3(options.Iterations);
		conditions.insert(conditions.end(), e3.begin(), e3.end());
	}

	if (conditions.empty())
	{
		std::fprintf(stderr, "No conditions selected (unrecognized --experiment value: %s)\n", options.Experiment.c_str());
		return 1;
	}

	std::string rawPath = options.OutDir + "/benchmark_raw.csv";
	std::ofstream raw(rawPath, std::ios::out | std::ios::trunc);
	if (!raw)
	{
		std::fprintf(stderr, "Could not open %s for writing\n", rawPath.c_str());
		return 1;
	}
	raw << "experiment,strategy,client_count,static_object_count,moving_object_count,iterations,repetition,elapsed_ms,"
	       "total_messages_sent,total_bytes_sent\n";

	int totalRuns = static_cast<int>(conditions.size()) * options.Repetitions;
	int runIndex = 0;

	for (const ConditionConfig& condition : conditions)
	{
		for (int repetition = 1; repetition <= options.Repetitions; ++repetition)
		{
			++runIndex;
			std::printf("[%d/%d] %s strategy=%s clients=%d static=%d moving=%d iter=%d rep=%d ... ", runIndex, totalRuns,
			            condition.Experiment.c_str(), StrategyName(condition.Strategy), condition.ClientCount,
			            condition.StaticCount, condition.MovingCount, condition.Iterations, repetition);
			std::fflush(stdout);

			RunMetrics metrics = RunOneRepetition(condition);

			std::printf("%.2f ms\n", metrics.ElapsedMs);

			raw << condition.Experiment << ',' << StrategyName(condition.Strategy) << ',' << condition.ClientCount << ','
			    << condition.StaticCount << ',' << condition.MovingCount << ',' << condition.Iterations << ','
			    << repetition << ',' << metrics.ElapsedMs << ',' << metrics.TotalMessagesSent << ','
			    << metrics.TotalBytesSent << '\n';
			raw.flush();
		}
	}

	std::printf("\nWrote %s\n", rawPath.c_str());
	return 0;
}

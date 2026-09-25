// Real, non-SDL integration test proving NetworkClient's Milestone 2 Section 3
// threading contract against a REAL running headless server (spare-parts/build/bin/server
// must already be running at Endpoint before this executable is launched). Not
// CTest-registered — see game-engine's Tests/NetworkIntegration/ for why this style of
// test is manually invoked rather than automated: a real multi-process/timing scenario
// is being proven here, not a pure deterministic unit.
//
// Uses a short, bounded polling helper (never an arbitrary long sleep as the
// correctness mechanism) to wait for genuinely asynchronous conditions.

#include "NetworkClient.h"

#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

namespace
{
	constexpr const char* Endpoint = "tcp://127.0.0.1:5556";
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
	std::optional<NetworkClient> client;

	auto constructStart = std::chrono::steady_clock::now();
	client.emplace(Endpoint);
	auto constructDuration = std::chrono::steady_clock::now() - constructStart;

	// 3. Constructor must return without waiting synchronously for JOIN. A blocking
	// implementation would take at least one real network round trip here; a
	// non-blocking one returns in microseconds regardless of server round-trip time.
	Check(constructDuration < std::chrono::milliseconds(50), "Constructor_ReturnsWithoutBlockingOnJoin");

	// 4. Poll boundedly until connected.
	bool connected = WaitUntil([&client]() { return client->IsConnected(); }, PollTimeoutMs, PollIntervalMs);
	Check(connected, "Client_BecomesConnectedWithinTimeout");

	// 5. Nonzero PlayerId.
	Engine::PlayerId id = client->GetLocalPlayerId();
	Check(id != 0, "Client_AssignedNonZeroPlayerId");

	// 6/7. Publish a known state; verify it reaches the real server and comes back in
	// our own next Snapshot (the server always echoes the full current roster,
	// including the sender itself).
	Engine::PlayerState known{ id, 123.0f, 456.0f, 7.0f, 8.0f };
	client->PublishState(known);

	bool sawOwnState = WaitUntil(
	    [&client, id]()
	    {
		    std::optional<Engine::Snapshot> snapshot = client->GetLatestSnapshot();
		    if (!snapshot.has_value())
		    {
			    return false;
		    }
		    for (const Engine::PlayerState& state : snapshot->Roster)
		    {
			    if (state.Id == id && state.PositionX == 123.0f && state.PositionY == 456.0f &&
			        state.VelocityX == 7.0f && state.VelocityY == 8.0f)
			    {
				    return true;
			    }
		    }
		    return false;
	    },
	    PollTimeoutMs, PollIntervalMs);
	Check(sawOwnState, "PublishedState_ReachesRealServer_AndReturnsInSnapshot");

	// 8. Publish many states rapidly. PublishState must not block on network round
	// trips: a blocking implementation would make 1000 calls take seconds (each
	// waiting on a real Send+Receive); a non-blocking one completes near-instantly.
	auto burstStart = std::chrono::steady_clock::now();
	for (int i = 0; i < 1000; ++i)
	{
		client->PublishState(Engine::PlayerState{ id, static_cast<float>(i), 0.0f, 0.0f, 0.0f });
	}
	auto burstDuration = std::chrono::steady_clock::now() - burstStart;
	Check(burstDuration < std::chrono::milliseconds(200), "PublishState_RapidCalls_DoNotBlockOnNetworkRoundTrips");

	// 9. Destroy against a healthy server; verify join()/shutdown completes promptly
	// (rather than hanging), which requires the worker to have actually sent
	// Leaving=true and received its reply before returning.
	auto destroyStart = std::chrono::steady_clock::now();
	client.reset();
	auto destroyDuration = std::chrono::steady_clock::now() - destroyStart;
	Check(destroyDuration < std::chrono::milliseconds(2000), "Destructor_CompletesPromptlyAgainstHealthyServer");

	// 10. Verify, as a neutral witness joining after the fact, that the server's
	// roster no longer contains the disconnected PlayerId.
	Engine::Socket witness(Engine::SocketRole::Request);
	witness.Connect(Endpoint);
	witness.Send(ToFrame(Engine::EncodeJoinRequest()));
	std::vector<std::uint8_t> witnessReply = ToBytes(witness.Receive());
	std::optional<Engine::Snapshot> witnessSnapshot = Engine::DecodeSnapshot(witnessReply);

	bool idAbsent = witnessSnapshot.has_value();
	if (witnessSnapshot.has_value())
	{
		for (const Engine::PlayerState& state : witnessSnapshot->Roster)
		{
			if (state.Id == id)
			{
				idAbsent = false;
			}
		}
	}
	Check(idAbsent, "Disconnect_RemovesPlayerFromRealServerRoster");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

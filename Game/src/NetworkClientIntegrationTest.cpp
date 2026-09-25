// Real, non-SDL integration test proving NetworkClient's Milestone 2 Section 3/4
// threading and bootstrap+dedicated-session contract against a REAL running production
// server (spare-parts/build/bin/server must already be running at BootstrapEndpoint
// before this executable is launched). Not CTest-registered — see game-engine's
// Tests/NetworkIntegration/ for why this style of test is manually invoked rather than
// automated: a real multi-process/timing scenario is being proven here, not a pure
// deterministic unit.
//
// This exercises the real NetworkClient end to end — it never bypasses it (e.g. via
// SessionTestClient) for the behavior under test. The witness at the end is the one
// exception: it deliberately uses a raw Socket, playing the role of a neutral third
// party independently confirming what the real server's roster looks like after the
// real NetworkClient under test has been destroyed.
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
	client.emplace(BootstrapEndpoint);
	auto constructDuration = std::chrono::steady_clock::now() - constructStart;

	// 1. Constructor must return without waiting synchronously for the bootstrap
	// JOIN/JoinAccepted exchange or the dedicated-session connect. A blocking
	// implementation would take at least one real network round trip here; a
	// non-blocking one returns in microseconds regardless of server round-trip time.
	Check(constructDuration < std::chrono::milliseconds(50), "Constructor_ReturnsWithoutBlockingOnJoin");

	// 2. Poll boundedly until connected.
	bool connected = WaitUntil([&client]() { return client->IsConnected(); }, PollTimeoutMs, PollIntervalMs);
	Check(connected, "Client_BecomesConnectedWithinTimeout");

	// 3. Nonzero PlayerId.
	Engine::PlayerId id = client->GetLocalPlayerId();
	Check(id != 0, "Client_AssignedNonZeroPlayerId");

	// 4. The production server's bootstrap replies ONLY JoinAccepted or Error to JOIN
	// (never a Snapshot — see ServerDispatch::HandleRequest is not used by bootstrap
	// for this reply; server_main.cpp's bootstrap loop only ever sends JoinAccepted or
	// Error), and NetworkClient only ever sets m_Connected=true after successfully
	// decoding a JoinAccepted AND successfully connecting a second, separate dedicated
	// socket to the port it named. There is no other code path that reaches
	// connected=true with a nonzero id, so checks 2 and 3 together are already
	// conclusive proof that the real bootstrap JoinAccepted handoff succeeded — no
	// separate boolean is needed to observe this from outside NetworkClient's API.
	Check(connected && id != 0, "Bootstrap_JoinAcceptedHandoff_Succeeded");

	// 5a. JOIN no longer carries a roster (JoinAccepted has none): before this client
	// has published anything itself, there must be no Snapshot yet. Deterministic, not
	// a race — the worker only ever produces a Snapshot in response to a StateUpdate
	// this test has not sent yet.
	Check(!client->GetLatestSnapshot().has_value(), "NoSnapshot_UntilFirstPublishState");

	// 5b/6. Publish a known state; verify the first Snapshot to arrive reaches the real
	// server and comes back containing exactly that published state (the server always
	// echoes the full current roster, including the sender itself).
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
	Check(sawOwnState, "FirstPublishState_ProducesSnapshot_ContainingPublishedState");

	// 7. Publish many states rapidly. PublishState must not block on network round
	// trips: a blocking implementation would make 1000 calls take seconds (each
	// waiting on a real Send+Receive); a non-blocking one completes near-instantly.
	auto burstStart = std::chrono::steady_clock::now();
	for (int i = 0; i < 1000; ++i)
	{
		client->PublishState(Engine::PlayerState{ id, static_cast<float>(i), 0.0f, 0.0f, 0.0f });
	}
	auto burstDuration = std::chrono::steady_clock::now() - burstStart;
	Check(burstDuration < std::chrono::milliseconds(200), "PublishState_RapidCalls_DoNotBlockOnNetworkRoundTrips");

	// 8. Destroy against a healthy server; verify join()/shutdown completes promptly
	// (rather than hanging), which requires the worker to have actually sent
	// Leaving=true on the dedicated socket and received its reply before returning.
	auto destroyStart = std::chrono::steady_clock::now();
	client.reset();
	auto destroyDuration = std::chrono::steady_clock::now() - destroyStart;
	Check(destroyDuration < std::chrono::milliseconds(2000), "Destructor_SendsLeaving_CompletesPromptlyAgainstHealthyServer");

	// 9. Verify, as a neutral witness joining after the fact, that the server's roster
	// no longer contains the disconnected PlayerId. The witness is a real client of the
	// production topology too (bootstrap JOIN -> JoinAccepted -> dedicated session), but
	// deliberately uses a raw Socket rather than NetworkClient, since it exists only to
	// independently confirm server-side state, not to exercise NetworkClient itself.
	Engine::Socket witnessBootstrap(Engine::SocketRole::Request);
	witnessBootstrap.Connect(BootstrapEndpoint);
	witnessBootstrap.Send(ToFrame(Engine::EncodeJoinRequest()));
	std::vector<std::uint8_t> witnessJoinReply = ToBytes(witnessBootstrap.Receive());

	bool idAbsent = false;
	std::optional<Engine::JoinAccepted> witnessAccepted;
	if (Engine::PeekMessageType(witnessJoinReply) == Engine::MessageType::JoinAccepted)
	{
		witnessAccepted = Engine::DecodeJoinAccepted(witnessJoinReply);
	}

	if (witnessAccepted.has_value())
	{
		// Smallest possible endpoint-port rewrite, independent of NetworkClient's own
		// (unexported) helper -- this witness is meant to verify server state on its
		// own terms, not by trusting the same code path under test.
		std::string dedicatedEndpoint =
		    std::string(BootstrapEndpoint).substr(0, std::string(BootstrapEndpoint).find_last_of(':') + 1) +
		    std::to_string(witnessAccepted->AssignedPort);

		Engine::Socket witnessSession(Engine::SocketRole::Request);
		witnessSession.Connect(dedicatedEndpoint);

		Engine::PlayerState witnessState{ witnessAccepted->AssignedId, 0.0f, 0.0f, 0.0f, 0.0f };
		witnessSession.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ witnessState, false })));
		std::vector<std::uint8_t> witnessReply = ToBytes(witnessSession.Receive());
		std::optional<Engine::Snapshot> witnessSnapshot = Engine::DecodeSnapshot(witnessReply);

		idAbsent = witnessSnapshot.has_value();
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

		// Clean up after the witness itself, so repeated test runs against the same
		// long-running server don't accumulate phantom entries (there is no
		// heartbeat/timeout to reap them otherwise).
		witnessSession.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ witnessState, true })));
		witnessSession.Receive();
	}
	Check(idAbsent, "Disconnect_RemovesPlayerFromRealServerRoster");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

// Real, non-SDL 3-client integration test proving Milestone 2 Section 4's core
// dedicated-session property from the real NetworkClient's side: three independent
// NetworkClient instances, each with its own dedicated server session, can all
// bootstrap-join, publish, and receive snapshots concurrently without interfering with
// each other. Requires a real running production server at BootstrapEndpoint. Not
// CTest-registered — same rationale as NetworkClientIntegrationTest (a real
// multi-process/timing scenario, not a pure deterministic unit).

#include "NetworkClient.h"

#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

namespace
{
	constexpr const char* BootstrapEndpoint = "tcp://127.0.0.1:5556";
	constexpr int PollIntervalMs = 10;
	constexpr int PollTimeoutMs = 3000;
	constexpr int ClientCount = 3;

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
	Check(ids[0] != 0 && ids[1] != 0 && ids[2] != 0, "AllThreeClients_AssignedNonZeroPlayerId");
	Check(ids[0] != ids[1] && ids[0] != ids[2] && ids[1] != ids[2], "AllThreeClients_AssignedUniquePlayerId");

	// Every client's own roster must eventually reflect all three published states.
	// Each client's worker only sends a new StateUpdate when PublishState is called, so
	// a single one-shot publish per client isn't enough: whichever client's exchange the
	// server happens to process first would see its peers still at their JOIN-time
	// neutral placeholder {0,0,0,0}, not yet their real published values. Republishing
	// on every poll iteration (exactly how real gameplay calls PublishState every frame)
	// lets every client's own request eventually land after all three peers already
	// have real registry data. A misrouted/spoofed session (the one failure mode
	// Section 4's dedicated sessions specifically guard against) would show up here as
	// a client's Snapshot getting stuck on an Error reply and never converging, so this
	// WaitUntil is also the mechanical proof that no client ever received UnknownPlayer
	// for its own updates.
	bool allSeeEachOther = WaitUntil(
	    [&]()
	    {
		    for (int i = 0; i < ClientCount; ++i)
		    {
			    clients[i]->PublishState(Engine::PlayerState{ ids[i], static_cast<float>(100 + i),
			                                                   static_cast<float>(200 + i), 1.0f, 0.0f });
		    }

		    for (int viewer = 0; viewer < ClientCount; ++viewer)
		    {
			    std::optional<Engine::Snapshot> snapshot = clients[viewer]->GetLatestSnapshot();
			    if (!snapshot.has_value())
			    {
				    return false;
			    }
			    for (int other = 0; other < ClientCount; ++other)
			    {
				    bool found = false;
				    for (const Engine::PlayerState& state : snapshot->Roster)
				    {
					    if (state.Id == ids[other] && state.PositionX == static_cast<float>(100 + other) &&
					        state.PositionY == static_cast<float>(200 + other))
					    {
						    found = true;
						    break;
					    }
				    }
				    if (!found)
				    {
					    return false;
				    }
			    }
		    }
		    return true;
	    },
	    PollTimeoutMs, PollIntervalMs);
	Check(allSeeEachOther, "AllThreeClients_SeeEachOthersPublishedStateInSnapshot");

	// Destroy all three; each sends Leaving=true on its own dedicated socket. Prompt
	// completion (rather than a hang) further confirms no client's session was left in
	// a broken state by another client's traffic.
	auto destroyStart = std::chrono::steady_clock::now();
	for (std::optional<NetworkClient>& client : clients)
	{
		client.reset();
	}
	auto destroyDuration = std::chrono::steady_clock::now() - destroyStart;
	Check(destroyDuration < std::chrono::milliseconds(3000), "AllThreeClients_DestroyPromptly");

	// Witness: the server is still alive (this JOIN itself proves it), and the roster
	// no longer contains any of the three disconnected ids.
	Engine::Socket witnessBootstrap(Engine::SocketRole::Request);
	witnessBootstrap.Connect(BootstrapEndpoint);
	witnessBootstrap.Send(ToFrame(Engine::EncodeJoinRequest()));
	std::vector<std::uint8_t> witnessJoinReply = ToBytes(witnessBootstrap.Receive());

	bool serverAliveAndClean = false;
	std::optional<Engine::JoinAccepted> witnessAccepted;
	if (Engine::PeekMessageType(witnessJoinReply) == Engine::MessageType::JoinAccepted)
	{
		witnessAccepted = Engine::DecodeJoinAccepted(witnessJoinReply);
	}

	if (witnessAccepted.has_value())
	{
		std::string dedicatedEndpoint =
		    std::string(BootstrapEndpoint).substr(0, std::string(BootstrapEndpoint).find_last_of(':') + 1) +
		    std::to_string(witnessAccepted->AssignedPort);

		Engine::Socket witnessSession(Engine::SocketRole::Request);
		witnessSession.Connect(dedicatedEndpoint);

		Engine::PlayerState witnessState{ witnessAccepted->AssignedId, 0.0f, 0.0f, 0.0f, 0.0f };
		witnessSession.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ witnessState, false })));
		std::vector<std::uint8_t> witnessReply = ToBytes(witnessSession.Receive());
		std::optional<Engine::Snapshot> witnessSnapshot = Engine::DecodeSnapshot(witnessReply);

		serverAliveAndClean = witnessSnapshot.has_value();
		if (witnessSnapshot.has_value())
		{
			for (const Engine::PlayerState& state : witnessSnapshot->Roster)
			{
				if (state.Id == ids[0] || state.Id == ids[1] || state.Id == ids[2])
				{
					serverAliveAndClean = false;
				}
			}
		}

		witnessSession.Send(ToFrame(Engine::EncodeStateUpdate(Engine::StateUpdate{ witnessState, true })));
		witnessSession.Receive();
	}
	Check(serverAliveAndClean, "ServerRemainsAlive_AndRosterHasNoneOfTheThreeDisconnectedIds");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

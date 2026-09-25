// Lightweight, non-SDL test client for Milestone 2 Section 4 checkpoint 2's real
// bootstrap + dedicated-session server topology. Each invocation is one self-contained
// action over a fresh Engine::Socket connection — real Protocol encode/decode, no
// shortcut through PlayerRegistry/HandleSessionRequest directly. This is test tooling
// only, not the future NetworkClient (that migration to JoinAccepted-driven handoff is
// the next checkpoint).
//
// Usage:
//   SessionTestClient join
//       Bootstrap JOIN against tcp://127.0.0.1:5556.
//       Prints "JOINED <id> <sessionPort> <p2pPort>" or "ERROR <code>".
//   SessionTestClient update <port> <id> <posX> <posY> <velX> <velY>
//       Dedicated-session StateUpdate (Leaving=false) against tcp://127.0.0.1:<port>.
//       Prints "SNAPSHOT <recipientId> <rosterCount> <id> <x> <y> <vx> <vy> ... PEERS
//       <peerCount> <peerId> <p2pPort> ..." or "ERROR <code>". Passing a <id> other
//       than the one this port was assigned is how the PlayerId-spoofing scenario is
//       exercised.
//   SessionTestClient leave <port> <id>
//       Dedicated-session StateUpdate (Leaving=true). Same reply format as `update`.
//   SessionTestClient sendjoin <port>
//       Sends a JOIN-encoded frame to a dedicated session port instead of bootstrap
//       (expected to be rejected without ending the session). Same reply format.
//
// Exit code 0 for a decoded JoinAccepted/Snapshot reply, 1 for Error or malformed.

#include "Engine/Network/Protocol.h"
#include "Engine/Network/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace
{
	constexpr const char* BootstrapEndpoint = "tcp://127.0.0.1:5556";

	std::vector<std::uint8_t> ToBytes(const std::string& frame)
	{
		return std::vector<std::uint8_t>(frame.begin(), frame.end());
	}

	std::string ToFrame(const std::vector<std::uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}

	std::string SessionEndpoint(std::uint16_t port)
	{
		return "tcp://127.0.0.1:" + std::to_string(port);
	}

	void PrintSnapshot(const Engine::Snapshot& snapshot)
	{
		std::printf("SNAPSHOT %u %zu", snapshot.RecipientId, snapshot.Roster.size());
		for (const Engine::PlayerState& state : snapshot.Roster)
		{
			std::printf(" %u %f %f %f %f", state.Id, state.PositionX, state.PositionY, state.VelocityX,
			            state.VelocityY);
		}
		// Milestone 2 Section 5 server checkpoint: the peer directory, printed
		// separately from Roster since the two are independent, unordered lists (no
		// index correlation) — tests must look up a peer by Id, never by position here.
		std::printf(" PEERS %zu", snapshot.Peers.size());
		for (const Engine::PeerInfo& peer : snapshot.Peers)
		{
			std::printf(" %u %u", peer.Id, peer.P2pPort);
		}
		std::printf("\n");
	}

	int ReportReply(const std::vector<std::uint8_t>& replyBytes)
	{
		std::optional<Engine::MessageType> type = Engine::PeekMessageType(replyBytes);

		if (type == Engine::MessageType::JoinAccepted)
		{
			std::optional<Engine::JoinAccepted> accepted = Engine::DecodeJoinAccepted(replyBytes);
			if (!accepted.has_value())
			{
				std::fprintf(stderr, "SessionTestClient: malformed JOIN_ACCEPTED reply\n");
				return 1;
			}
			std::printf("JOINED %u %u %u\n", accepted->AssignedId, accepted->AssignedPort, accepted->P2pPort);
			return 0;
		}

		if (type == Engine::MessageType::Snapshot)
		{
			std::optional<Engine::Snapshot> snapshot = Engine::DecodeSnapshot(replyBytes);
			if (!snapshot.has_value())
			{
				std::fprintf(stderr, "SessionTestClient: malformed SNAPSHOT reply\n");
				return 1;
			}
			PrintSnapshot(*snapshot);
			return 0;
		}

		if (type == Engine::MessageType::Error)
		{
			std::optional<Engine::ErrorResponse> error = Engine::DecodeError(replyBytes);
			if (!error.has_value())
			{
				std::fprintf(stderr, "SessionTestClient: malformed ERROR reply\n");
				return 1;
			}
			std::printf("ERROR %u\n", static_cast<unsigned>(error->Code));
			return 1;
		}

		std::fprintf(stderr, "SessionTestClient: unexpected or unrecognized reply\n");
		return 1;
	}

	int SendAndReport(const std::string& endpoint, const std::vector<std::uint8_t>& requestBytes)
	{
		Engine::Socket client(Engine::SocketRole::Request);
		client.Connect(endpoint);
		client.Send(ToFrame(requestBytes));
		std::vector<std::uint8_t> replyBytes = ToBytes(client.Receive());
		return ReportReply(replyBytes);
	}

	void PrintUsage()
	{
		std::fprintf(stderr,
		              "usage: SessionTestClient join\n"
		              "       SessionTestClient update <port> <id> <posX> <posY> <velX> <velY>\n"
		              "       SessionTestClient leave <port> <id>\n"
		              "       SessionTestClient sendjoin <port>\n");
	}
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		PrintUsage();
		return 1;
	}

	std::string action = argv[1];

	if (action == "join" && argc == 2)
	{
		return SendAndReport(BootstrapEndpoint, Engine::EncodeJoinRequest());
	}

	if (action == "update" && argc == 8)
	{
		std::uint16_t port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
		Engine::PlayerState state{ static_cast<Engine::PlayerId>(std::strtoul(argv[3], nullptr, 10)),
			                        std::strtof(argv[4], nullptr), std::strtof(argv[5], nullptr),
			                        std::strtof(argv[6], nullptr), std::strtof(argv[7], nullptr) };
		return SendAndReport(SessionEndpoint(port), Engine::EncodeStateUpdate(Engine::StateUpdate{ state, false }));
	}

	if (action == "leave" && argc == 4)
	{
		std::uint16_t port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
		Engine::PlayerId id = static_cast<Engine::PlayerId>(std::strtoul(argv[3], nullptr, 10));
		Engine::PlayerState state{ id, 0.0f, 0.0f, 0.0f, 0.0f };
		return SendAndReport(SessionEndpoint(port), Engine::EncodeStateUpdate(Engine::StateUpdate{ state, true }));
	}

	if (action == "sendjoin" && argc == 3)
	{
		std::uint16_t port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
		return SendAndReport(SessionEndpoint(port), Engine::EncodeJoinRequest());
	}

	PrintUsage();
	return 1;
}

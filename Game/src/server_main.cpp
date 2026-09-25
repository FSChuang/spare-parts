// Spare Parts headless multiplayer server (Milestone 2 Section 2). No SDL window, no
// Renderer, no Application, no Entity — this executable links only EngineNetwork, so
// it is provably SDL-free at the build-graph level, not just by convention. Runs
// indefinitely, replying to every request, until terminated (Ctrl-C). Owns the single
// PlayerRegistry that is authoritative for which players are currently connected and
// their latest reported state; it never simulates player movement itself.

#include "Engine/Network/PlayerRegistry.h"
#include "Engine/Network/ServerDispatch.h"
#include "Engine/Network/Socket.h"

#include <cstdio>
#include <string>

namespace
{
	constexpr const char* DefaultEndpoint = "tcp://127.0.0.1:5556";
}

int main(int argc, char* argv[])
{
	std::string endpoint = (argc > 1) ? argv[1] : DefaultEndpoint;

	Engine::Socket server(Engine::SocketRole::Reply);
	server.Bind(endpoint);

	Engine::PlayerRegistry registry;

	std::printf("Spare Parts server listening on %s (Ctrl-C to stop)\n", endpoint.c_str());
	std::fflush(stdout);

	for (;;)
	{
		std::string requestFrame = server.Receive();
		std::vector<std::uint8_t> requestBytes(requestFrame.begin(), requestFrame.end());

		std::vector<std::uint8_t> replyBytes = Engine::HandleRequest(registry, requestBytes);

		server.Send(std::string(replyBytes.begin(), replyBytes.end()));
	}
}

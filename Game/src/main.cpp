#include "Engine/Core/Application.h"
#include "Game.h"

#include <SDL3/SDL_main.h>

#include <string>

namespace
{
	// This game's window configuration (ENGINEERING_SPEC.md §9: tunables stay at the top level).
	constexpr int WindowWidth = 1920;
	constexpr int WindowHeight = 1080;
	const char* const WindowTitle = "Spare Parts";

	// Milestone 2 Section 2: all local demo clients connect here by default; override
	// with a command-line argument, e.g. `./main tcp://127.0.0.1:5556`.
	const char* const DefaultServerEndpoint = "tcp://127.0.0.1:5556";
}

int main(int argc, char* argv[])
{
	std::string serverEndpoint = (argc > 1) ? argv[1] : DefaultServerEndpoint;

	Engine::WindowConfig windowConfig{ WindowTitle, WindowWidth, WindowHeight };
	Engine::Application application(windowConfig);

	Game game(serverEndpoint);

	application.Run(
	    [&game](Engine::InputManager& input, float deltaTime) { game.Update(input, deltaTime); },
	    [&game](Engine::Renderer& renderer) { game.Render(renderer); });

	return 0;
}

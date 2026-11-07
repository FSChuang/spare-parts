#include "Engine/Core/Application.h"
#include "Game.h"
#include "GameAssets.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <exception>
#include <string>

namespace
{
	// This game's window configuration (ENGINEERING_SPEC.md §9: tunables stay at the top level).
	constexpr int WindowWidth = 1920;
	constexpr int WindowHeight = 1080;
	const char* const WindowTitle = "Astro Arena";

	// Deep space: what shows wherever the star texture has no star (the engine's default is blue).
	constexpr Engine::Color SpaceClearColor{ 6, 8, 22, 255 };

	// The PNGs sit in an `assets` folder next to the executable (CMake copies them there after
	// building), so the game runs from any working directory.
	const char* const AssetFolderName = "assets/";

	// Milestone 2 Section 2: all local demo clients connect here by default; override
	// with a command-line argument, e.g. `./main tcp://127.0.0.1:5556`.
	const char* const DefaultServerEndpoint = "tcp://127.0.0.1:5556";
}

int main(int argc, char* argv[])
{
	std::string serverEndpoint = (argc > 1) ? argv[1] : DefaultServerEndpoint;

	try
	{
		Engine::WindowConfig windowConfig{ WindowTitle, WindowWidth, WindowHeight };
		Engine::Application application(windowConfig);
		application.GetRenderer().SetClearColor(SpaceClearColor);

		const char* basePath = SDL_GetBasePath();
		GameAssets assets = LoadGameAssets(application.GetRenderer(), std::string(basePath != nullptr ? basePath : "") + AssetFolderName);

		// Every gameplay event is stamped with the game Timeline's logical time, so pausing or
		// scaling the Timeline (P, 1/2/3) affects event timing exactly as it affects physics.
		Game game(serverEndpoint, [&application] { return application.GetGameTimeline().GetTime(); }, assets);

		application.Run(
		    [&game](Engine::InputManager& input, float deltaTime) { game.Update(input, deltaTime); },
		    [&game](Engine::Renderer& renderer) { game.Render(renderer); });
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Astro Arena could not start: %s\n", error.what());
		return 1;
	}

	return 0;
}

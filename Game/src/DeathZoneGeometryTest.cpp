// Bugfix checkpoint: focused tests for the death-zone GEOMETRY itself, using the real
// Engine::GameObject + Engine::IsColliding mechanism — not a parallel raw y-threshold
// rule (Game::ResolveDeathZone() must remain the only death path, and the DeathZone
// GameObject must remain its authoritative representation). Pure logic, no SDL, no
// sockets, no running server.
//
// The position/size constants below must match Game.cpp's own DeathZonePosition/
// DeathZoneSize/WorldHeight/WorldWidth exactly — same intentional duplication pattern as
// ServerPlatformTest.cpp's bound constants (kept local to the test rather than exported,
// since only the test needs to reason about the exact geometry; production code only
// ever goes through Game::ResolveDeathZone()).

#include "Engine/Collision/Collision.h"
#include "Engine/GameObject/GameObject.h"

#include <cstdio>

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

	constexpr float WorldHeight = 1080.0f;
	constexpr float WorldWidth = 1920.0f * 2.0f;
	constexpr Engine::Vector2 DeathZonePosition{ 0.0f, WorldHeight };
	constexpr Engine::Vector2 DeathZoneSize{ WorldWidth, 1000000.0f };

	constexpr Engine::Vector2 PlayerSize{ 50.0f, 50.0f };

	Engine::GameObject MakeDeathZone()
	{
		return Engine::GameObject(1, Engine::ObjectType::DeathZone, DeathZonePosition, DeathZoneSize);
	}

	Engine::GameObject MakePlayerAt(Engine::Vector2 position)
	{
		return Engine::GameObject(2, Engine::ObjectType::Player, position, PlayerSize);
	}
}

int main()
{
	Engine::GameObject deathZone = MakeDeathZone();
	Check(!deathZone.HasRender(), "DeathZone_HasNoRenderProperty_RemainsHidden");

	// A player resting comfortably on the playable area (well above WorldHeight) must
	// NOT be considered dead — the zone must not trigger prematurely over normal
	// gameplay space.
	{
		Engine::GameObject player = MakePlayerAt({ 400.0f, 900.0f });
		Check(!Engine::IsColliding(player, deathZone), "PlayerOnPlayableArea_DoesNotCollideWithDeathZone");
	}

	// A player who has fallen just past the bottom of the world collides with the death
	// zone — the basic "entering the death region triggers death" case.
	{
		Engine::GameObject player = MakePlayerAt({ 400.0f, WorldHeight + 10.0f });
		Check(Engine::IsColliding(player, deathZone), "PlayerJustBelowWorldHeight_CollidesWithDeathZone");
	}

	// Strict AABB semantics are preserved: a player whose bottom edge exactly touches
	// the death zone's top edge (no overlap) does not yet count as colliding.
	{
		Engine::GameObject player = MakePlayerAt({ 400.0f, WorldHeight - PlayerSize.Y });
		Check(!Engine::IsColliding(player, deathZone),
		      "PlayerExactlyTouchingDeathZoneTopEdge_StrictAabb_DoesNotCollide");
	}

	// Regression: tunneling. Gravity has no velocity cap, and a single abnormally slow
	// frame (e.g. four GUI client windows competing for CPU) can produce a large
	// deltaTime, so a falling player can move an extreme distance in one physics step.
	// Simulate a worst-case single-frame displacement — far beyond anything a 400px-deep
	// zone could have caught — landing deep below the world, and confirm the (now
	// effectively bottomless) death zone still catches it.
	{
		constexpr float ExtremeSingleFrameDisplacement = 50000.0f; // comfortably beyond any plausible real jump
		Engine::GameObject player = MakePlayerAt({ 400.0f, WorldHeight + ExtremeSingleFrameDisplacement });
		Check(Engine::IsColliding(player, deathZone),
		      "ExtremeSingleFrameFall_DoesNotTunnelPastDeathZone_RegressionForManualGuiBug");
	}

	// The death zone spans the full world width — a player far along the second screen
	// who falls out of bounds is still caught, not just one near world X = 0.
	{
		Engine::GameObject player = MakePlayerAt({ WorldWidth - 100.0f, WorldHeight + 10.0f });
		Check(Engine::IsColliding(player, deathZone), "PlayerOnSecondScreen_FallingOut_StillCollidesWithDeathZone");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

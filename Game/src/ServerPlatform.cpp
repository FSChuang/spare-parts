#include "ServerPlatform.h"

#include <cmath>
#include <utility>

namespace
{
	// Tunables for the two server-authoritative moving platforms (ENGINEERING_SPEC.md §9:
	// no magic numbers), matching the Milestone 3 two-screen world (each screen 1920 wide,
	// matching Game.cpp's ViewWidth/WindowWidth): the horizontal platform bridges a gap in
	// screen 0 (world X 0-1920), the vertical platform sits in screen 1 (world X 1920-3840).
	constexpr float HorizontalPlatformY = 900.0f;
	constexpr float HorizontalPlatformLeftBound = 900.0f;
	constexpr float HorizontalPlatformRightBound = 1700.0f;
	constexpr float HorizontalPlatformSpeed = 150.0f; // px/s, matches the existing enemy patrol speed

	constexpr float VerticalPlatformX = 2450.0f;
	constexpr float VerticalPlatformTopBound = 500.0f;
	constexpr float VerticalPlatformBottomBound = 900.0f;
	constexpr float VerticalPlatformSpeed = 150.0f; // px/s

	// Shared triangle-wave trajectory along one axis: bounces between lowBound and
	// highBound at `speed`, continuous at both turning points (no jump in position, only
	// in velocity's sign) and at every period wrap. Returns {positionAlongAxis,
	// velocityAlongAxis}. Used by both platforms below so the horizontal (X) and vertical
	// (Y) motions are provably the same deterministic shape, just applied to a different
	// axis with different bounds/speed.
	std::pair<float, float> TriangleWave(float lowBound, float highBound, float speed, float elapsedSeconds)
	{
		float span = highBound - lowBound;
		float halfPeriod = span / speed;
		float period = 2.0f * halfPeriod;

		// std::fmod can return a small negative result for a negative input; guard for it
		// even though `elapsedSeconds` is never expected to be negative in practice.
		float t = std::fmod(elapsedSeconds, period);
		if (t < 0.0f)
		{
			t += period;
		}

		if (t < halfPeriod)
		{
			return { lowBound + speed * t, speed };
		}

		float tSinceTurn = t - halfPeriod;
		return { highBound - speed * tSinceTurn, -speed };
	}
}

Engine::PlatformState ComputePlatformState(std::chrono::steady_clock::time_point serverStart,
                                            std::chrono::steady_clock::time_point now)
{
	float elapsedSeconds = std::chrono::duration<float>(now - serverStart).count();
	auto [positionX, velocityX] =
	    TriangleWave(HorizontalPlatformLeftBound, HorizontalPlatformRightBound, HorizontalPlatformSpeed, elapsedSeconds);
	return Engine::PlatformState{ positionX, HorizontalPlatformY, velocityX, 0.0f };
}

Engine::PlatformState ComputeVerticalPlatformState(std::chrono::steady_clock::time_point serverStart,
                                                    std::chrono::steady_clock::time_point now)
{
	float elapsedSeconds = std::chrono::duration<float>(now - serverStart).count();
	auto [positionY, velocityY] =
	    TriangleWave(VerticalPlatformTopBound, VerticalPlatformBottomBound, VerticalPlatformSpeed, elapsedSeconds);
	return Engine::PlatformState{ VerticalPlatformX, positionY, 0.0f, velocityY };
}

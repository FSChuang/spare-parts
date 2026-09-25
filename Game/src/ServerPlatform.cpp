#include "ServerPlatform.h"

#include <cmath>

namespace
{
	// Tunables for the server-authoritative moving platform, compatible with the
	// existing 1920x1080 world and the existing 1200x40 platform size (ENGINEERING_SPEC.md
	// §9: no magic numbers). PlatformRightBound is chosen so the platform (1200 wide)
	// stays fully on screen across its whole travel range: 620 + 1200 = 1820 < 1920.
	constexpr float PlatformY = 900.0f;           // matches Game.cpp's pre-connection default
	constexpr float PlatformLeftBound = 100.0f;
	constexpr float PlatformRightBound = 620.0f;
	constexpr float PlatformSpeed = 150.0f;       // px/s, matches the existing enemy patrol speed
}

Engine::PlatformState ComputePlatformState(std::chrono::steady_clock::time_point serverStart,
                                            std::chrono::steady_clock::time_point now)
{
	float elapsedSeconds = std::chrono::duration<float>(now - serverStart).count();

	float span = PlatformRightBound - PlatformLeftBound;
	float halfPeriod = span / PlatformSpeed;
	float period = 2.0f * halfPeriod;

	// Triangle wave: leftBound -> rightBound -> leftBound -> ..., continuous at both
	// turning points (no jump in position, only in velocity's sign) and at every period
	// wrap. std::fmod can return a small negative result for a negative input; guard for
	// it even though `now` is never expected to precede `serverStart` in practice.
	float t = std::fmod(elapsedSeconds, period);
	if (t < 0.0f)
	{
		t += period;
	}

	if (t < halfPeriod)
	{
		return Engine::PlatformState{ PlatformLeftBound + PlatformSpeed * t, PlatformY, PlatformSpeed, 0.0f };
	}

	float tSinceTurn = t - halfPeriod;
	return Engine::PlatformState{ PlatformRightBound - PlatformSpeed * tSinceTurn, PlatformY, -PlatformSpeed, 0.0f };
}

// Deterministic unit test for ComputePlatformState (Milestone 2 Section 4 final
// checkpoint). Unlike every other Game/src/*Test.cpp file, this one needs NO running
// server and NO real elapsed time: ComputePlatformState is a pure function of two
// explicit time points, so every case here is constructed from synthetic time points.
// Not CTest-registered (spare-parts has no CTest setup); follows the same manually
// invoked PASS/FAIL executable style as the other Game/src/*Test.cpp files for
// consistency.

#include "ServerPlatform.h"

#include <chrono>
#include <cmath>
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

	bool NearlyEqual(float a, float b, float epsilon)
	{
		return std::fabs(a - b) <= epsilon;
	}

	// Builds an arbitrary, deterministic steady_clock::time_point purely from a float
	// second count — never reads the real clock.
	std::chrono::steady_clock::time_point AtSeconds(float seconds)
	{
		return std::chrono::steady_clock::time_point{} +
		       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
	}
}

int main()
{
	// These must match ServerPlatform.cpp's own constants for the boundary checks below
	// to mean anything; kept local to this test rather than exported, since only the
	// test needs to reason about the exact bounds/speed (production code only ever
	// calls ComputePlatformState(), never these constants directly).
	constexpr float LeftBound = 100.0f;
	constexpr float RightBound = 620.0f;
	constexpr float Speed = 150.0f;
	constexpr float HalfPeriod = (RightBound - LeftBound) / Speed;
	constexpr float Period = 2.0f * HalfPeriod;

	std::chrono::steady_clock::time_point start = AtSeconds(1000.0f); // arbitrary, nonzero epoch

	Engine::PlatformState atStart = ComputePlatformState(start, start);
	Check(NearlyEqual(atStart.PositionX, LeftBound, 0.01f), "AtStart_PositionAtLeftBound");
	Check(atStart.PositionY == 900.0f, "AtStart_PositionYIsConstant");
	Check(atStart.VelocityX > 0.0f, "AtStart_MovingRight");
	Check(atStart.VelocityY == 0.0f, "AtStart_NoVerticalVelocity");

	Engine::PlatformState atRightBound = ComputePlatformState(start, start + std::chrono::duration_cast<
	                                                                            std::chrono::steady_clock::duration>(
	                                                                            std::chrono::duration<float>(HalfPeriod)));
	Check(NearlyEqual(atRightBound.PositionX, RightBound, 1.0f), "AtHalfPeriod_PositionAtRightBound");

	auto AfterStart = [&](float seconds)
	{
		return start +
		       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
	};

	Engine::PlatformState justAfterTurn = ComputePlatformState(start, AfterStart(HalfPeriod + 0.1f));
	Check(justAfterTurn.VelocityX < 0.0f, "JustAfterHalfPeriod_MovingLeft");
	Check(justAfterTurn.PositionX < RightBound, "JustAfterHalfPeriod_PositionMovedBackFromRightBound");

	Engine::PlatformState atFullPeriod = ComputePlatformState(start, AfterStart(Period));
	Check(NearlyEqual(atFullPeriod.PositionX, LeftBound, 1.0f), "AtFullPeriod_ReturnsToLeftBound");
	Check(atFullPeriod.VelocityX > 0.0f, "AtFullPeriod_MovingRightAgainAtNextCycle");

	// Every reachable PositionX must stay within [LeftBound, RightBound] — no overshoot.
	bool everInBounds = true;
	for (float sample = 0.0f; sample < 3.0f * Period; sample += 0.05f)
	{
		Engine::PlatformState state = ComputePlatformState(start, AfterStart(sample));
		if (state.PositionX < LeftBound - 0.01f || state.PositionX > RightBound + 0.01f)
		{
			everInBounds = false;
			break;
		}
	}
	Check(everInBounds, "PositionX_NeverOvershootsBounds_AcrossMultipleCycles");

	// Determinism: same two time points always produce the same result.
	Engine::PlatformState repeatA = ComputePlatformState(start, AfterStart(1.5f));
	Engine::PlatformState repeatB = ComputePlatformState(start, AfterStart(1.5f));
	Check(repeatA.PositionX == repeatB.PositionX && repeatA.VelocityX == repeatB.VelocityX,
	      "Deterministic_SameInputsProduceSameOutput");

	// Independence from client Timeline is structural, not merely tested: the function
	// takes only two time points as input — there is no scale/tic/pause parameter for
	// any client's Timeline to even reach. (See NetworkRateAndPlatformTest.cpp for the
	// real, cross-client, differently-Timeline-scaled proof of this at the integration
	// level.)

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

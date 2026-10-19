// Deterministic unit tests for ComputePlatformState/ComputeVerticalPlatformState
// (Milestone 2 Section 4 final checkpoint; vertical platform added in Milestone 3).
// Unlike every other Game/src/*Test.cpp file, this one needs NO running server and NO
// real elapsed time: both functions are pure functions of two explicit time points, so
// every case here is constructed from synthetic time points. Not CTest-registered
// (spare-parts has no CTest setup); follows the same manually invoked PASS/FAIL
// executable style as the other Game/src/*Test.cpp files for consistency.

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
	// calls the Compute*PlatformState functions, never these constants directly).
	constexpr float LeftBound = 900.0f;
	constexpr float RightBound = 1700.0f;
	constexpr float Speed = 150.0f;
	constexpr float HalfPeriod = (RightBound - LeftBound) / Speed;
	constexpr float Period = 2.0f * HalfPeriod;

	std::chrono::steady_clock::time_point start = AtSeconds(1000.0f); // arbitrary, nonzero epoch

	auto AfterStart = [&](float seconds)
	{
		return start +
		       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
	};

	// --- ComputePlatformState: horizontal oscillation ---

	Engine::PlatformState atStart = ComputePlatformState(start, start);
	Check(NearlyEqual(atStart.PositionX, LeftBound, 0.01f), "AtStart_PositionAtLeftBound");
	Check(atStart.PositionY == 900.0f, "AtStart_PositionYIsConstant");
	Check(atStart.VelocityX > 0.0f, "AtStart_MovingRight");
	Check(atStart.VelocityY == 0.0f, "AtStart_NoVerticalVelocity");

	Engine::PlatformState atRightBound = ComputePlatformState(start, AfterStart(HalfPeriod));
	Check(NearlyEqual(atRightBound.PositionX, RightBound, 1.0f), "AtHalfPeriod_PositionAtRightBound");

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

	// --- ComputeVerticalPlatformState (Milestone 3): vertical oscillation ---
	// Same shape of guarantees as the horizontal platform above, mirrored onto Y instead
	// of X, proving the two patterns are genuinely distinct (different axis) rather than
	// the same motion relabeled.

	constexpr float TopBound = 500.0f;
	constexpr float BottomBound = 900.0f;
	constexpr float VerticalSpeed = 150.0f;
	constexpr float VerticalHalfPeriod = (BottomBound - TopBound) / VerticalSpeed;
	constexpr float VerticalPeriod = 2.0f * VerticalHalfPeriod;
	constexpr float VerticalX = 2450.0f;

	Engine::PlatformState verticalAtStart = ComputeVerticalPlatformState(start, start);
	Check(verticalAtStart.PositionX == VerticalX, "Vertical_PositionXIsConstant");
	Check(NearlyEqual(verticalAtStart.PositionY, TopBound, 0.01f), "Vertical_AtStart_PositionAtTopBound");
	Check(verticalAtStart.VelocityX == 0.0f, "Vertical_NoHorizontalVelocity");
	Check(verticalAtStart.VelocityY > 0.0f, "Vertical_AtStart_MovingDown");

	Engine::PlatformState verticalAtBottom = ComputeVerticalPlatformState(start, AfterStart(VerticalHalfPeriod));
	Check(NearlyEqual(verticalAtBottom.PositionY, BottomBound, 1.0f), "Vertical_AtHalfPeriod_PositionAtBottomBound");

	Engine::PlatformState verticalJustAfterTurn =
	    ComputeVerticalPlatformState(start, AfterStart(VerticalHalfPeriod + 0.1f));
	Check(verticalJustAfterTurn.VelocityY < 0.0f, "Vertical_JustAfterHalfPeriod_MovingUp");

	bool verticalEverInBounds = true;
	for (float sample = 0.0f; sample < 3.0f * VerticalPeriod; sample += 0.05f)
	{
		Engine::PlatformState state = ComputeVerticalPlatformState(start, AfterStart(sample));
		if (state.PositionY < TopBound - 0.01f || state.PositionY > BottomBound + 0.01f)
		{
			verticalEverInBounds = false;
			break;
		}
	}
	Check(verticalEverInBounds, "Vertical_PositionY_NeverOvershootsBounds_AcrossMultipleCycles");

	// The two platforms are independent trajectories, not the same computation reused:
	// at an arbitrary shared sample time, horizontal motion stays on its own axis/bounds
	// and vertical motion stays on its own, distinctly.
	Engine::PlatformState horizontalSample = ComputePlatformState(start, AfterStart(2.25f));
	Engine::PlatformState verticalSample = ComputeVerticalPlatformState(start, AfterStart(2.25f));
	Check(horizontalSample.PositionY == 900.0f && verticalSample.PositionX == VerticalX,
	      "HorizontalAndVertical_StayOnTheirOwnAxis");
	Check(horizontalSample.VelocityY == 0.0f && verticalSample.VelocityX == 0.0f,
	      "HorizontalAndVertical_NeverIntroduceCrossAxisVelocity");

	// Independence from client Timeline is structural, not merely tested: both functions
	// take only two time points as input — there is no scale/tic/pause parameter for any
	// client's Timeline to even reach. (See NetworkRateAndPlatformTest.cpp for the real,
	// cross-client, differently-Timeline-scaled proof of this at the integration level.)

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

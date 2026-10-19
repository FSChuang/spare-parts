// Focused unit tests for GameRules.h (Milestone 3). Pure logic, no SDL, no sockets, no
// running server — follows the same manually invoked PASS/FAIL executable style as the
// other Game/src/*Test.cpp files (spare-parts has no CTest setup).

#include "GameRules.h"

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
}

int main()
{
	// --- SelectSpawnIndex ---

	Check(SelectSpawnIndex(0, 2) == 0, "SelectSpawnIndex_UnassignedSentinel_ResolvesToFirstSpawn");
	Check(SelectSpawnIndex(1, 2) == 1, "SelectSpawnIndex_PlayerOne_ResolvesToSecondSpawn");
	Check(SelectSpawnIndex(2, 2) == 0, "SelectSpawnIndex_PlayerTwo_WrapsBackToFirstSpawn");
	Check(SelectSpawnIndex(1, 2) != SelectSpawnIndex(2, 2), "SelectSpawnIndex_DifferentPlayers_CanGetDifferentSpawns");

	// Determinism: the same PlayerId always resolves to the same spawn index.
	Check(SelectSpawnIndex(7, 3) == SelectSpawnIndex(7, 3), "SelectSpawnIndex_Deterministic_SameInputsProduceSameOutput");

	// Works for spawn counts other than 2 as well.
	Check(SelectSpawnIndex(5, 3) == 2, "SelectSpawnIndex_WorksForThreeSpawns");

	// --- NextCameraOffsetX ---

	Check(NextCameraOffsetX(0.0f, 100.0f, 1920.0f) == 1920.0f,
	      "NextCameraOffsetX_MovingRight_SnapsToSecondScreen");
	Check(NextCameraOffsetX(1920.0f, -100.0f, 1920.0f) == 0.0f,
	      "NextCameraOffsetX_MovingLeft_SnapsToFirstScreen");
	Check(NextCameraOffsetX(0.0f, 0.0f, 1920.0f) == 0.0f,
	      "NextCameraOffsetX_Stationary_LeavesFirstScreenOffsetUnchanged");
	Check(NextCameraOffsetX(1920.0f, 0.0f, 1920.0f) == 1920.0f,
	      "NextCameraOffsetX_Stationary_LeavesSecondScreenOffsetUnchanged");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

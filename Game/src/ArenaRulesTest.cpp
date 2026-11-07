// Tests for the pure arena maths (ArenaRules.h) and spawn table (GameRules.h): heading,
// circle overlap, ship wrap-around, bullet-out-of-arena, centre/top-left conversion, spawn
// selection. No SDL, no sockets, no EventManager. Expected values are written out by hand
// (not recomputed with the production formulas), so these fail if the rules change.

#include "ArenaRules.h"
#include "GameRules.h"

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

	bool Near(float a, float b, float tolerance = 1e-4f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	constexpr float Pi = 3.14159265358979323846f;
	constexpr float R = 24.0f; // a ship radius used throughout
}

int main()
{
	// --- heading convention: 0 = right, positive = clockwise on screen (Y down) ---
	Check(Near(ForwardOf(0.0f).X, 1.0f) && Near(ForwardOf(0.0f).Y, 0.0f), "Forward_HeadingZero_PointsRight");
	Check(Near(ForwardOf(Pi / 2.0f).X, 0.0f) && Near(ForwardOf(Pi / 2.0f).Y, 1.0f),
	      "Forward_PositiveQuarterTurn_PointsDownTheScreen");
	Check(Near(ForwardOf(-Pi / 2.0f).Y, -1.0f), "Forward_NegativeQuarterTurn_PointsUpTheScreen");
	Check(Near(ForwardOf(Pi).X, -1.0f), "Forward_HalfTurn_PointsLeft");

	Check(Near(NormalizeHeading(0.5f), 0.5f), "Normalize_InRange_Unchanged");
	Check(Near(NormalizeHeading(2.0f * Pi + 0.5f), 0.5f), "Normalize_OneTurnPlus_Folds");
	Check(Near(NormalizeHeading(-2.0f * Pi - 0.5f), -0.5f), "Normalize_OneTurnMinus_Folds");
	Check(Near(NormalizeHeading(Pi), -Pi), "Normalize_ExactlyPi_FoldsToMinusPi_HalfOpenRange");
	{
		bool allInRange = true;
		for (float h = -20.0f; h < 20.0f; h += 0.37f)
		{
			float n = NormalizeHeading(h);
			allInRange = allInRange && n >= -Pi && n < Pi;
		}
		Check(allInRange, "Normalize_ManyInputs_AlwaysInHalfOpenRange");
	}

	Check(Near(HeadingTowardArenaCentre({ 480.0f, 540.0f }), 0.0f), "TowardCentre_FromLeftOfCentre_PointsRight");
	Check(Near(HeadingTowardArenaCentre({ 960.0f, 100.0f }), Pi / 2.0f), "TowardCentre_FromAboveCentre_PointsDown");
	Check(Near(HeadingTowardArenaCentre({ 960.0f, 900.0f }), -Pi / 2.0f), "TowardCentre_FromBelowCentre_PointsUp");

	// --- circle overlap (strict) ---
	Check(!CirclesOverlap({ 0.0f, 0.0f }, 5.0f, { 20.0f, 0.0f }, 5.0f), "Circles_Separated_DoNotOverlap");
	Check(!CirclesOverlap({ 0.0f, 0.0f }, 5.0f, { 10.0f, 0.0f }, 5.0f), "Circles_ExactlyTangent_DoNotOverlap");
	Check(CirclesOverlap({ 0.0f, 0.0f }, 5.0f, { 9.0f, 0.0f }, 5.0f), "Circles_Overlapping_Overlap");
	Check(CirclesOverlap({ 7.0f, 7.0f }, 5.0f, { 7.0f, 7.0f }, 24.0f), "Circles_SameCentre_Overlap");
	Check(CirclesOverlap({ 100.0f, 100.0f }, 24.0f, { 100.0f, 124.9f }, 1.0f), "Circles_TinyBulletJustInsideShip_Overlap");
	Check(!CirclesOverlap({ 100.0f, 100.0f }, 24.0f, { 100.0f, 126.0f }, 1.0f), "Circles_TinyBulletJustOutsideShip_NoOverlap");
	// A circle's corner-of-box is NOT inside it: AABB would say hit, a circle must not.
	Check(!CirclesOverlap({ 100.0f, 100.0f }, 24.0f, { 122.0f, 122.0f }, 5.0f), "Circles_BulletAtTheShipsBoxCorner_NoOverlap_WhereAnAabbWouldHit");
	// Centres 5 apart on the diagonal (a 3-4-5 triangle): radii summing to 4.9 miss, 5.1 hit.
	Check(!CirclesOverlap({ 0.0f, 0.0f }, 3.0f, { 3.0f, 4.0f }, 1.9f) &&
	          CirclesOverlap({ 0.0f, 0.0f }, 3.0f, { 3.0f, 4.0f }, 2.1f),
	      "Circles_Diagonal_UsesTrueDistance");

	// --- ship wrap-around (by bounding circle) ---
	{
		Engine::Vector2 inside = WrapShipCentre({ 500.0f, 400.0f }, R);
		Check(inside.X == 500.0f && inside.Y == 400.0f, "Wrap_InsideArena_NoChange");
		Engine::Vector2 touchingLeft = WrapShipCentre({ -R, 400.0f }, R);
		Check(touchingLeft.X == -R, "Wrap_ExactlyTouchingLeftEdge_NotWrappedYet");
		Engine::Vector2 touchingBottom = WrapShipCentre({ 500.0f, Astro::ArenaHeight + R }, R);
		Check(touchingBottom.Y == Astro::ArenaHeight + R, "Wrap_ExactlyTouchingBottomEdge_NotWrappedYet");

		// Left -> right: just past -R reappears just past the right edge (width + 2R further on).
		Engine::Vector2 left = WrapShipCentre({ -25.0f, 400.0f }, R);
		Check(Near(left.X, 1943.0f) && left.Y == 400.0f, "Wrap_LeftToRight");
		Engine::Vector2 right = WrapShipCentre({ 1945.0f, 400.0f }, R);
		Check(Near(right.X, -23.0f) && right.Y == 400.0f, "Wrap_RightToLeft");
		Engine::Vector2 top = WrapShipCentre({ 500.0f, -25.0f }, R);
		Check(top.X == 500.0f && Near(top.Y, 1103.0f), "Wrap_TopToBottom");
		Engine::Vector2 bottom = WrapShipCentre({ 500.0f, 1105.0f }, R);
		Check(bottom.X == 500.0f && Near(bottom.Y, -23.0f), "Wrap_BottomToTop");
		Engine::Vector2 corner = WrapShipCentre({ -30.0f, -30.0f }, R);
		Check(Near(corner.X, 1938.0f) && Near(corner.Y, 1098.0f), "Wrap_BothAxesAtACorner");

		// The reappeared ship is outside the opposite edge (still fully invisible), so it slides in.
		Check(left.X > Astro::ArenaWidth + R - 2.0f * R, "Wrap_ReappearsBeyondTheOppositeEdge");
	}

	// --- bullets leaving the arena ---
	Check(!IsFullyOutsideArena({ 10.0f, 10.0f }, 5.0f), "Outside_InsideArena_False");
	Check(!IsFullyOutsideArena({ -5.0f, 10.0f }, 5.0f), "Outside_TouchingEdge_False");
	Check(IsFullyOutsideArena({ -6.0f, 10.0f }, 5.0f), "Outside_PastLeft_True");
	Check(IsFullyOutsideArena({ 1927.0f, 10.0f }, 5.0f), "Outside_PastRight_True");
	Check(IsFullyOutsideArena({ 10.0f, -6.0f }, 5.0f), "Outside_PastTop_True");
	Check(IsFullyOutsideArena({ 10.0f, 1087.0f }, 5.0f), "Outside_PastBottom_True");

	// --- centre <-> top-left ---
	{
		Engine::GameObject box(1, Engine::ObjectType::Generic, { 100.0f, 200.0f }, { 48.0f, 48.0f });
		Check(CenterOf(box).X == 124.0f && CenterOf(box).Y == 224.0f, "CenterOf_IsTopLeftPlusHalfSize");
		SetCenter(box, { 500.0f, 300.0f });
		Check(box.GetPosition().X == 476.0f && box.GetPosition().Y == 276.0f, "SetCenter_MovesTopLeftAccordingly");
	}

	// --- spawn table ---
	Check(SelectSpawnIndex(0, ArenaSpawnCount) == 0, "SelectSpawnIndex_UnassignedSentinel_ResolvesToFirstSpawn");
	Check(SelectSpawnIndex(1, 2) == 1 && SelectSpawnIndex(2, 2) == 0, "SelectSpawnIndex_WrapsAroundTheSpawnCount");
	Check(SelectSpawnIndex(7, 3) == SelectSpawnIndex(7, 3), "SelectSpawnIndex_Deterministic");
	Check(SelectSpawnIndex(5, 3) == 2, "SelectSpawnIndex_WorksForThreeSpawns");
	{
		bool allInsideAndDistinct = true;
		for (std::size_t a = 0; a < ArenaSpawnCount; ++a)
		{
			Engine::Vector2 c = ArenaSpawnCentre(a);
			allInsideAndDistinct = allInsideAndDistinct && c.X >= Astro::ShipRadius && c.X <= Astro::ArenaWidth - Astro::ShipRadius &&
			                       c.Y >= Astro::ShipRadius && c.Y <= Astro::ArenaHeight - Astro::ShipRadius;
			for (std::size_t b = a + 1; b < ArenaSpawnCount; ++b)
			{
				Engine::Vector2 d = ArenaSpawnCentre(b);
				allInsideAndDistinct = allInsideAndDistinct && (c.X != d.X || c.Y != d.Y);
			}
		}
		Check(allInsideAndDistinct, "SpawnTable_AllFullyOnScreen_AndDistinct");
		Check(ArenaSpawnCentre(0).X < Astro::ArenaWidth / 2.0f && ArenaSpawnCentre(0).Y < Astro::ArenaHeight / 2.0f &&
		          ArenaSpawnCentre(3).X > Astro::ArenaWidth / 2.0f && ArenaSpawnCentre(3).Y > Astro::ArenaHeight / 2.0f,
		      "SpawnTable_CornersAreWhereTheyShouldBe");
		Check(ArenaSpawnCentre(ArenaSpawnCount + 1).X == ArenaSpawnCentre(1).X, "SpawnTable_IndexIsReducedModuloCount");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

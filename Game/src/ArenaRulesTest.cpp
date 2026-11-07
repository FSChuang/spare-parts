// Tests for the pure arena maths (ArenaRules.h), the map (ArenaMap.h), the spawn table
// (GameRules.h) and the drawing decisions (VisualRules.h): heading, circle overlap, circle-vs-
// rectangle, the walls/obstacles/spawn points, bullet-out-of-arena, centre/top-left conversion,
// spawn selection, ship colours, sprite rotation, explosion frames. No SDL, no sockets, no
// EventManager. Expected values are written out by hand
// (not recomputed with the production formulas), so these fail if the rules change.

#include "ArenaMap.h"
#include "ArenaRules.h"
#include "GameRules.h"
#include "VisualRules.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

	Check(Near(SpawnHeadingFor({ 220.0f, 220.0f }), 0.0f) && Near(SpawnHeadingFor({ 220.0f, 860.0f }), 0.0f),
	      "SpawnHeading_LeftHalf_FacesRight");
	Check(Near(SpawnHeadingFor({ 1700.0f, 220.0f }), Pi) && Near(SpawnHeadingFor({ 1700.0f, 860.0f }), Pi),
	      "SpawnHeading_RightHalf_FacesLeft");
	Check(Near(SpawnHeadingFor({ 959.0f, 100.0f }), 0.0f) && Near(SpawnHeadingFor({ 960.0f, 100.0f }), Pi),
	      "SpawnHeading_SplitsExactlyAtTheCentreLine");

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

	// --- circle vs rectangle (rect: top-left (100,100), size 200x50 -> x 100..300, y 100..150) ---
	{
		const Engine::Vector2 rectPos{ 100.0f, 100.0f };
		const Engine::Vector2 rectSize{ 200.0f, 50.0f };
		Check(!CircleVsRect({ 50.0f, 125.0f }, 20.0f, rectPos, rectSize), "CircleRect_Outside_NoOverlap");
		Check(!CircleVsRect({ 80.0f, 125.0f }, 20.0f, rectPos, rectSize), "CircleRect_TangentToTheLeftSide_NoOverlap");
		Check(CircleVsRect({ 81.0f, 125.0f }, 20.0f, rectPos, rectSize), "CircleRect_OverlappingTheLeftSide_Overlap");
		Check(!CircleVsRect({ 200.0f, 170.0f }, 20.0f, rectPos, rectSize), "CircleRect_TangentToTheBottomSide_NoOverlap");
		Check(CircleVsRect({ 200.0f, 169.0f }, 20.0f, rectPos, rectSize), "CircleRect_OverlappingTheBottomSide_Overlap");
		Check(CircleVsRect({ 290.0f, 125.0f }, 5.0f, rectPos, rectSize), "CircleRect_CentreInsideTheRect_Overlap");
		// Corner: the nearest point is the corner (300,150); a circle at (310,160) with r=15 is 14.14 away.
		Check(CircleVsRect({ 310.0f, 160.0f }, 15.0f, rectPos, rectSize), "CircleRect_OverlappingACorner_Overlap");
		Check(!CircleVsRect({ 310.0f, 160.0f }, 14.0f, rectPos, rectSize), "CircleRect_JustMissingACorner_NoOverlap");
		// The classic AABB-vs-circle mistake: the circle's bounding box touches the rect, the circle does not.
		Check(!CircleVsRect({ 320.0f, 170.0f }, 25.0f, rectPos, rectSize), "CircleRect_BoundingBoxOverlapsButCircleDoesNot");
		Check(CircleVsRect({ 200.0f, 125.0f }, 1.0f, rectPos, rectSize), "CircleRect_TinyBulletInside_Overlap");
		Check(!CircleVsRect({ 200.0f, 400.0f }, 5.0f, rectPos, rectSize), "CircleRect_FarAway_NoOverlap");
	}

	// --- the map ---
	{
		const std::vector<SolidRect>& walls = ArenaWalls();
		const std::vector<SolidRect>& obstacles = ArenaObstacles();
		Check(walls.size() == 4, "Map_HasFourOuterWalls");
		Check(obstacles.size() == 5, "Map_HasACentralBlockAndFourBarriers");

		// Walls: the four sides of a frame 70 px in from the screen edge, 32 px thick.
		const SolidRect& top = walls[0];
		const SolidRect& bottom = walls[1];
		const SolidRect& left = walls[2];
		const SolidRect& right = walls[3];
		Check(top.Position.X == 70.0f && top.Position.Y == 70.0f && top.Size.X == 1780.0f && top.Size.Y == 32.0f, "Map_TopWall");
		Check(bottom.Position.X == 70.0f && bottom.Position.Y == 978.0f && bottom.Size.X == 1780.0f && bottom.Size.Y == 32.0f, "Map_BottomWall");
		Check(left.Position.X == 70.0f && left.Position.Y == 70.0f && left.Size.X == 32.0f && left.Size.Y == 940.0f, "Map_LeftWall");
		Check(right.Position.X == 1818.0f && right.Position.Y == 70.0f && right.Size.X == 32.0f && right.Size.Y == 940.0f, "Map_RightWall");

		SolidRect playfield = ArenaPlayfield();
		Check(playfield.Position.X == 102.0f && playfield.Position.Y == 102.0f && playfield.Size.X == 1716.0f && playfield.Size.Y == 876.0f,
		      "Map_PlayfieldIsTheOpenAreaInsideTheWalls");

		// Every obstacle lies fully inside the playfield, clear of the walls.
		bool inside = true;
		for (const SolidRect& o : obstacles)
		{
			inside = inside && o.Position.X >= playfield.Position.X + 100.0f && o.Position.Y >= playfield.Position.Y + 100.0f &&
			         o.Position.X + o.Size.X <= playfield.Position.X + playfield.Size.X - 100.0f &&
			         o.Position.Y + o.Size.Y <= playfield.Position.Y + playfield.Size.Y - 100.0f;
		}
		Check(inside, "Map_ObstaclesLieInsideThePlayfieldWithRoomToFlyAround");

		// The map is symmetric about both centre lines: every solid has a mirror image.
		auto hasMirror = [&](const SolidRect& r, bool flipX, bool flipY)
		{
			Engine::Vector2 mirroredPosition{ flipX ? Astro::ArenaWidth - (r.Position.X + r.Size.X) : r.Position.X,
				                             flipY ? Astro::ArenaHeight - (r.Position.Y + r.Size.Y) : r.Position.Y };
			for (const SolidRect& other : obstacles)
			{
				if (Near(other.Position.X, mirroredPosition.X) && Near(other.Position.Y, mirroredPosition.Y) &&
				    Near(other.Size.X, r.Size.X) && Near(other.Size.Y, r.Size.Y))
				{
					return true;
				}
			}
			return false;
		};
		bool symmetric = true;
		for (const SolidRect& o : obstacles)
		{
			symmetric = symmetric && hasMirror(o, true, false) && hasMirror(o, false, true) && hasMirror(o, true, true);
		}
		Check(symmetric, "Map_ObstaclesAreSymmetricAboutBothCentreLines");
		Check(CircleVsRect({ Astro::ArenaWidth / 2.0f, Astro::ArenaHeight / 2.0f }, 1.0f, obstacles[0].Position, obstacles[0].Size),
		      "Map_TheReactorSitsAtTheArenaCentre");

		// Spawn points: inside the playfield and clear of every wall and obstacle, with a ship's
		// full radius and a margin to spare.
		const float clearance = Astro::ShipRadius + 40.0f;
		bool spawnsSafe = true;
		bool spawnsInside = true;
		for (std::size_t i = 0; i < ArenaSpawnCount; ++i)
		{
			Engine::Vector2 spawn = ArenaSpawnCentre(i);
			spawnsInside = spawnsInside && spawn.X - Astro::ShipRadius > playfield.Position.X &&
			               spawn.X + Astro::ShipRadius < playfield.Position.X + playfield.Size.X &&
			               spawn.Y - Astro::ShipRadius > playfield.Position.Y &&
			               spawn.Y + Astro::ShipRadius < playfield.Position.Y + playfield.Size.Y;
			for (const std::vector<SolidRect>* group : { &walls, &obstacles })
			{
				for (const SolidRect& solid : *group)
				{
					spawnsSafe = spawnsSafe && !CircleVsRect(spawn, clearance, solid.Position, solid.Size);
				}
			}
		}
		Check(spawnsInside, "Map_SpawnPointsAreInsideThePlayfield");
		Check(spawnsSafe, "Map_SpawnPointsClearOfEveryWallAndObstacle_WithAMargin");

		// And a ship leaving a spawn straight ahead (the way it faces) is not about to hit anything
		// within the first second of protection: the lane along its row is clear for 320 px.
		bool laneClear = true;
		for (std::size_t i = 0; i < ArenaSpawnCount; ++i)
		{
			Engine::Vector2 spawn = ArenaSpawnCentre(i);
			float heading = SpawnHeadingFor(spawn);
			for (float travelled = 0.0f; travelled <= Astro::ShipSpeed; travelled += 8.0f)
			{
				Engine::Vector2 at{ spawn.X + std::cos(heading) * travelled, spawn.Y + std::sin(heading) * travelled };
				for (const std::vector<SolidRect>* group : { &walls, &obstacles })
				{
					for (const SolidRect& solid : *group)
					{
						laneClear = laneClear && !CircleVsRect(at, Astro::ShipRadius, solid.Position, solid.Size);
					}
				}
			}
		}
		Check(laneClear, "Map_ShipsCanFlyOffFromEverySpawnForAFullSecondWithoutTouchingAnything");
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

	// --- drawing decisions ---
	Check(ShipVariantFor(1) == 0 && ShipVariantFor(2) == 1 && ShipVariantFor(3) == 2 && ShipVariantFor(4) == 3,
	      "Visual_PlayerIds1To4_GetFourDifferentShips");
	Check(ShipVariantFor(5) == 0 && ShipVariantFor(8) == 3, "Visual_ShipVariantsRepeatBeyondFourPlayers");
	Check(ShipVariantFor(0) == 0, "Visual_UnassignedPlayerIdUsesTheFirstShip");
	Check(ShipVariantFor(1) != ShipVariantFor(2), "Visual_TwoPlayersInAGameLookDifferent");
	Check(Near(ShipSpriteRotation(0.0f), Pi / 2.0f), "Visual_HeadingZeroDrawsTheUpwardArtworkTurnedToFaceRight");
	Check(Near(ShipSpriteRotation(-Pi / 2.0f), 0.0f), "Visual_HeadingUp_DrawsTheArtworkUnrotated");
	Check(Near(ShipSpriteRotation(Pi / 2.0f) - ShipSpriteRotation(0.0f), Pi / 2.0f), "Visual_OffsetIsConstantAcrossHeadings");
	Check(ExplosionFrameAt(-0.1) == -1, "Explosion_BeforeItStarts_NoFrame");
	Check(ExplosionFrameAt(0.0) == 0, "Explosion_AtTheStart_FirstFrame");
	Check(ExplosionFrameAt(0.05) == 0 && ExplosionFrameAt(0.1) == 1, "Explosion_FramesAdvanceEveryTenthOfASecond");
	Check(ExplosionFrameAt(0.55) == 5, "Explosion_JustBeforeTheEnd_LastFrame");
	Check(ExplosionFrameAt(Astro::ExplosionDuration) == -1 && ExplosionFrameAt(10.0) == -1, "Explosion_AfterTheDuration_Expired");
	{
		bool monotonic = true;
		int previous = 0;
		for (double t = 0.0; t < Astro::ExplosionDuration; t += 0.01)
		{
			int frame = ExplosionFrameAt(t);
			monotonic = monotonic && frame >= previous && frame < Astro::ExplosionFrameCount;
			previous = frame;
		}
		Check(monotonic, "Explosion_FramesNeverGoBackwardsAndStayInRange");
	}

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}

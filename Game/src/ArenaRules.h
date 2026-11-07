#pragma once

#include "Engine/Core/Core.h"
#include "Engine/GameObject/GameObject.h"

// Astro arena rules: the tunables and the small pure maths the ship and bullet gameplay share. Spare Parts-specific, SDL-free, socket-free, so every rule
// here is unit tested without a Game. Nothing in this file touches an EventManager.
//
// COORDINATES. The arena is one fixed screen: world coordinates ARE logical screen
// coordinates, there is no camera. The map inside it (walls and obstacles) is ArenaMap.h. X grows right, Y grows DOWN (screen convention).
//
// HEADING. Radians. 0 points right (+X); a POSITIVE heading change turns CLOCKWISE on screen
// (because Y points down, +PI/2 points down). This is also the Renderer's DrawSprite rotation
// convention, so a heading can be handed to it unchanged later. "Rotate right" adds to the
// heading, "rotate left" subtracts from it.
//
// OBJECT POSITIONS. A ship or bullet is a square GameObject (size = 2 * radius), so its
// GameObject position is its TOP-LEFT corner like every other object, while gameplay maths
// works on its CENTRE. CenterOf()/SetCenter() convert.

namespace Astro
{
	constexpr float ArenaWidth = 1920.0f;
	constexpr float ArenaHeight = 1080.0f;

	// Ship
	constexpr float ShipRadius = 24.0f;
	constexpr float ShipSpeed = 320.0f;          // px/s, constant forward speed while alive
	constexpr float ShipRotationSpeed = 3.5f;    // rad/s while a rotate key is held
	constexpr double RespawnDelay = 2.0;         // s between dying and reappearing
	constexpr double InvulnerabilityDuration = 1.0; // s of immunity after a spawn

	// Bullet
	constexpr float BulletRadius = 5.0f;
	constexpr float BulletSpeed = 720.0f;        // px/s
	constexpr float BulletLifetime = 1.4f;       // s (Timeline-scaled, see BulletField)
	constexpr double FireCooldown = 0.3;         // s between shots
	// How far in front of the ship's centre a new bullet appears: just clear of the hull.
	constexpr float MuzzleDistance = ShipRadius + BulletRadius + 2.0f;
}

/// Unit vector for a heading (see the convention above).
Engine::Vector2 ForwardOf(float headingRadians);

/// Heading folded into [-PI, PI), so it never grows without bound.
float NormalizeHeading(float headingRadians);

/// The heading a ship has when it spawns at `position`: a spawn on the left half of the arena
/// faces right (0), one on the right half faces left (PI), so it always flies into the arena along
/// the long axis, away from the wall behind it.
float SpawnHeadingFor(Engine::Vector2 position);

/// Strict circle overlap: circles that merely touch (distance == sum of radii) do NOT
/// overlap, matching Engine::IsColliding's AABB rule.
bool CirclesOverlap(Engine::Vector2 centerA, float radiusA, Engine::Vector2 centerB, float radiusB);

/// Strict overlap of a circle with an axis-aligned rectangle (top-left `rectPosition`, `rectSize`):
/// true if the closest point of the rectangle is closer to the centre than `radius`. A circle that
/// only touches the rectangle (closest point exactly `radius` away) does NOT overlap; a centre
/// inside the rectangle always does.
bool CircleVsRect(Engine::Vector2 center, float radius, Engine::Vector2 rectPosition, Engine::Vector2 rectSize);

/// True once a circle lies entirely outside the arena's logical screen. The walls keep every
/// honest bullet inside; this only catches one that was created outside (e.g. a bogus Shoot).
bool IsFullyOutsideArena(Engine::Vector2 center, float radius);

Engine::Vector2 CenterOf(const Engine::GameObject& object);
void SetCenter(Engine::GameObject& object, Engine::Vector2 center);

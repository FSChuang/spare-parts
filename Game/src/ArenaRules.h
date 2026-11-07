#pragma once

#include "Engine/Core/Core.h"
#include "Engine/GameObject/GameObject.h"

// Astro arena rules (Astro rebuild Checkpoint B): the tunables and the small pure maths the
// ship and bullet gameplay share. Spare Parts-specific, SDL-free, socket-free, so every rule
// here is unit tested without a Game. Nothing in this file touches an EventManager.
//
// COORDINATES. The arena is one fixed screen: world coordinates ARE logical screen
// coordinates, there is no camera. X grows right, Y grows DOWN (screen convention).
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

/// The heading that points from `position` at the arena's centre (used for spawns, so a
/// ship always enters pointing into the arena).
float HeadingTowardArenaCentre(Engine::Vector2 position);

/// Strict circle overlap: circles that merely touch (distance == sum of radii) do NOT
/// overlap, matching Engine::IsColliding's AABB rule.
bool CirclesOverlap(Engine::Vector2 centerA, float radiusA, Engine::Vector2 centerB, float radiusB);

/// Wrap-around for a SHIP, by its bounding circle: once the whole ship is past an edge it
/// reappears just beyond the opposite edge, so it slides in smoothly instead of popping.
/// Concretely the ship wraps when its centre is more than `radius` outside the arena, and
/// moves by one arena width (or height) plus two radii. A ship still touching the arena is
/// never moved.
Engine::Vector2 WrapShipCentre(Engine::Vector2 center, float radius);

/// True once a circle lies entirely outside the arena (a bullet there can never come back,
/// bullets do not wrap).
bool IsFullyOutsideArena(Engine::Vector2 center, float radius);

Engine::Vector2 CenterOf(const Engine::GameObject& object);
void SetCenter(Engine::GameObject& object, Engine::Vector2 center);

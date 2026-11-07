#pragma once

#include "Engine/Core/Core.h"

#include <vector>

// The fixed arena map (final polish run): hardcoded, symmetric, inside the 1920x1080 logical
// screen. Spare Parts-specific, SDL-free. No camera, no scrolling, nothing networked — every client
// builds the same map, so geometry never needs to cross the wire.
//
//   - four OUTER WALLS framing the playfield, inset from the screen edge;
//   - five OBSTACLES: a central reactor block and four barriers on the axes, mirrored about both
//     centre lines (so the map is the same from every spawn);
//   - four SPAWN points near the corners (see GameRules.h), each clear of every solid.
//
// Walls and obstacles are axis-aligned rectangles. Game turns each into a static GameObject of
// type Boundary in the registry, so a ship touching one raises an ordinary Collision event naming
// that object, and BulletField removes any bullet that touches one.

struct SolidRect
{
	Engine::Vector2 Position; // top-left
	Engine::Vector2 Size;
};

namespace Astro
{
	// Outer walls: this far in from the screen edge, this thick.
	constexpr float WallInset = 70.0f;
	constexpr float WallThickness = 32.0f;
}

/// The four outer walls: top, bottom, left, right.
const std::vector<SolidRect>& ArenaWalls();

/// The five obstacles inside the playfield: the central block, then the four barriers.
const std::vector<SolidRect>& ArenaObstacles();

/// The open playfield inside the walls (what a ship may fly in).
SolidRect ArenaPlayfield();

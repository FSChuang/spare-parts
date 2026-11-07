#pragma once

#include <cstdint>

// The small pure decisions behind how the arena is drawn (SDL-free, so they are unit tested; the
// drawing itself is in Game::Render). Gameplay never reads any of this: the ship's gameplay heading
// stays exactly what the simulation made it, and artwork orientation is a separate, drawing-only fact.

namespace Astro
{
	// Ship artwork: four colour variants (one per player slot), drawn this big on the 1920x1080 screen.
	constexpr int ShipVariantCount = 4;
	constexpr float ShipSpriteSize = 48.0f;

	// The ship sprites are authored with the nose pointing UP. Gameplay heading 0 points RIGHT and
	// DrawSprite rotates clockwise, so drawing needs this ONE constant quarter turn added.
	constexpr float SpriteForwardOffsetRadians = 1.57079632679489661923f;

	constexpr float BulletSpriteSize = 18.0f;

	// Explosion: a sheet of 6 square frames, played once over ExplosionDuration seconds of logical time.
	constexpr int ExplosionFrameCount = 6;
	constexpr float ExplosionFramePixels = 64.0f; // size of one frame in the sheet
	constexpr float ExplosionDrawSize = 112.0f;
	constexpr double ExplosionDuration = 0.6;
}

/// Which ship colour a player gets: PlayerId 1,2,3,4 -> variants 0,1,2,3 (then it repeats); the
/// not-yet-assigned id 0 uses variant 0. Deterministic, so every client colours the same player alike.
int ShipVariantFor(std::uint32_t playerId);

/// The rotation to hand DrawSprite for a ship with this gameplay heading (heading + the artwork offset).
float ShipSpriteRotation(float headingRadians);

/// Which explosion frame to show `elapsed` seconds after the death: 0 .. ExplosionFrameCount-1, or -1
/// if the explosion has not started (negative time) or is over.
int ExplosionFrameAt(double elapsed);

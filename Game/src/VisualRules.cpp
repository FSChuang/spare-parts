#include "VisualRules.h"

int ShipVariantFor(std::uint32_t playerId)
{
	return playerId == 0 ? 0 : static_cast<int>((playerId - 1) % Astro::ShipVariantCount);
}

float ShipSpriteRotation(float headingRadians)
{
	return headingRadians + Astro::SpriteForwardOffsetRadians;
}

int ExplosionFrameAt(double elapsed)
{
	if (elapsed < 0.0 || elapsed >= Astro::ExplosionDuration)
	{
		return -1;
	}

	int frame = static_cast<int>(elapsed / (Astro::ExplosionDuration / Astro::ExplosionFrameCount));
	return frame < Astro::ExplosionFrameCount ? frame : Astro::ExplosionFrameCount - 1;
}

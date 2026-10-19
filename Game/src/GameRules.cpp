#include "GameRules.h"

std::size_t SelectSpawnIndex(std::uint32_t playerId, std::size_t spawnCount)
{
	return static_cast<std::size_t>(playerId) % spawnCount;
}

float NextCameraOffsetX(float currentOffsetX, float playerVelocityX, float viewWidth)
{
	if (playerVelocityX > 0.0f)
	{
		return viewWidth;
	}

	if (playerVelocityX < 0.0f)
	{
		return 0.0f;
	}

	return currentOffsetX;
}

#include "GameRules.h"

#include "ArenaRules.h"

namespace
{
	// Distance from the screen edge to a spawn centre: clear of the outer walls (which end 102 px in)
	// and of every obstacle, with room for a ship to leave along the long axis.
	constexpr float SpawnInset = 220.0f;
}

std::size_t SelectSpawnIndex(std::uint32_t playerId, std::size_t spawnCount)
{
	return static_cast<std::size_t>(playerId) % spawnCount;
}

Engine::Vector2 ArenaSpawnCentre(std::size_t index)
{
	const float left = SpawnInset;
	const float right = Astro::ArenaWidth - SpawnInset;
	const float top = SpawnInset;
	const float bottom = Astro::ArenaHeight - SpawnInset;

	switch (index % ArenaSpawnCount)
	{
	case 0:
		return { left, top };
	case 1:
		return { right, top };
	case 2:
		return { left, bottom };
	default:
		return { right, bottom };
	}
}

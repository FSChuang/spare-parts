#pragma once

#include "Engine/Core/Core.h"

#include <cstddef>
#include <cstdint>

// Pure, deterministic gameplay-rule helpers factored out of Game so they can be unit tested
// without constructing a full Game (which would require a live NetworkClient/worker thread).
// Spare Parts-specific, not a generic Engine abstraction. The arena maths itself is in
// ArenaRules.h; this file is about WHERE a player spawns.

/// Picks a deterministic spawn index for a player from its network identity: each
/// distinct PlayerId gets a fixed, repeatable spawn point, so different players spawn at
/// different points with no server involvement. `spawnCount` must be greater than zero.
std::size_t SelectSpawnIndex(std::uint32_t playerId, std::size_t spawnCount);

/// The arena has four spawn points, one near each corner of the playfield, each clear of every wall
/// and obstacle (ArenaMap.h; a test checks it).
constexpr std::size_t ArenaSpawnCount = 4;

/// The centre of spawn point `index` (0..ArenaSpawnCount-1): top-left, top-right,
/// bottom-left, bottom-right. `index` is reduced modulo ArenaSpawnCount.
Engine::Vector2 ArenaSpawnCentre(std::size_t index);

#pragma once

#include <cstddef>
#include <cstdint>

// Milestone 3: pure, deterministic gameplay-rule helpers factored out of Game so they can
// be unit tested without constructing a full Game (which would require a live
// NetworkClient/worker thread). Spare Parts-specific, not a generic Engine abstraction.

/// Picks a deterministic spawn index for a player from its network identity: each
/// distinct PlayerId gets a fixed, repeatable spawn point, so different players spawn at
/// different points with no server involvement — spawn points are deterministic,
/// identical, client-local scene setup. `spawnCount` must be greater than zero.
std::size_t SelectSpawnIndex(std::uint32_t playerId, std::size_t spawnCount);

/// Milestone 3 side-scrolling: decides the next camera offset when the player is
/// currently overlapping the scrolling boundary. Moving right snaps to the second
/// screen, moving left snaps back to the first; standing still on the boundary
/// (velocity == 0) leaves the current offset unchanged, avoiding flicker.
float NextCameraOffsetX(float currentOffsetX, float playerVelocityX, float viewWidth);

/// Bugfix checkpoint: the camera offset appropriate for a player standing at a given
/// world X position, regardless of where the camera was before — used by RespawnPlayer()
/// to snap the camera to the spawn's own screen immediately, rather than leaving it on
/// whichever screen the player died on (UpdateScrolling() only updates the camera while
/// the player is touching the boundary, which a freshly-respawned player is not). Screen
/// 0 is [0, viewWidth), screen 1 is [viewWidth, 2*viewWidth).
float CameraOffsetForWorldX(float worldX, float viewWidth);

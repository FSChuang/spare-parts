#pragma once

#include "Engine/Network/ExchangeProtocol.h"

#include <vector>

// CSC 581 performance checkpoint: deterministic synthetic object states for the Full vs
// Delta exchange-strategy benchmark. No randomness (no random_device) — every object's
// initial state and motion are pure functions of its index, so a run is byte-for-byte
// reproducible across repetitions and across the Full/Delta strategies (both strategies
// must observe the exact same update sequence for the comparison to be meaningful).
// Spare Parts-specific (benchmark tooling), not a generic Engine abstraction.

/// Builds `staticCount + movingCount` objects: ids 1..staticCount are static (fixed
/// position, zero velocity, never advanced by AdvanceMovingObjects), ids
/// staticCount+1..staticCount+movingCount are moving (nonzero velocity, deterministic
/// per-index variation). Objects are returned in id order.
std::vector<Engine::ExchangeObjectState> BuildInitialScene(int staticCount, int movingCount);

/// Advances exactly the moving objects (the last `movingCount` entries of `scene`, by
/// construction of BuildInitialScene) by `deltaTime` using their own velocity — plain
/// Euler integration. Static objects (the first `scene.size() - movingCount` entries)
/// are left untouched. Deterministic: the same (scene, movingCount, deltaTime) sequence
/// always produces the same trajectory.
void AdvanceMovingObjects(std::vector<Engine::ExchangeObjectState>& scene, int movingCount, float deltaTime);

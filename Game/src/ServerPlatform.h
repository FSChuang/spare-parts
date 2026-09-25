#pragma once

#include "Engine/Network/Protocol.h"

#include <chrono>

// Server-authoritative moving platform (Milestone 2 Section 4 final checkpoint): one
// horizontal platform whose position is a pure function of how much real time has
// elapsed on the server. Deliberately spare-parts-only (game-specific world constants
// like world size and platform bounds), never Engine — see CLAUDE.md's engine/game
// boundary.
//
// Pure and stateless: calling this twice with the same two time points always returns
// the same result. No mutable shared state, no accumulated delta, and no dependency on
// any client's Timeline (there is no scale/tic/pause parameter to even pass one in) —
// every dedicated session thread computes the same trajectory from the same
// `serverStart`, independent of which client is asking or how fast that client's own
// Timeline is running.
Engine::PlatformState ComputePlatformState(std::chrono::steady_clock::time_point serverStart,
                                            std::chrono::steady_clock::time_point now);

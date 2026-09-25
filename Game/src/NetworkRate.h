#pragma once

#include <chrono>

// Shared network-rate constants (Milestone 2 Sections 4-5 final checkpoints): both real
// gameplay (Game.cpp) and the rate-test harnesses must agree on these values, so they
// live here once rather than as independently-duplicated magic numbers
// (ENGINEERING_SPEC.md §1: no magic numbers).

// P2P gameplay publish rate at Timeline scale 1x. Since Game::Update's deltaTime is
// already Timeline-scaled, the same accumulator logic naturally halves/doubles the
// real-world publish rate at 0.5x/2x scale without this constant itself changing.
constexpr float NetworkUpdateIntervalSeconds = 1.0f / 30.0f;

// Server-facing session refresh cadence (Milestone 2 Section 5 final integration):
// fixed real time, deliberately independent of Timeline scale or pause — this is what
// keeps membership/peer-directory/platform freshness current even for a paused client.
// ~6.67 Hz.
constexpr std::chrono::milliseconds ServerRefreshInterval{ 150 };

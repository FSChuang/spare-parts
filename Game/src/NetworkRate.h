#pragma once

// Shared base network-tic rate (Milestone 2 Section 4 final checkpoint): both real
// gameplay (Game.cpp) and the Timeline-scaled rate-test harness must agree on this
// value, so it lives here once rather than as two independently-duplicated magic
// numbers (ENGINEERING_SPEC.md §1: no magic numbers).
//
// This is the rate at Timeline scale 1x. Since Game::Update's deltaTime is already
// Timeline-scaled, the same accumulator logic naturally halves/doubles the real-world
// publish rate at 0.5x/2x scale without this constant itself changing.
constexpr float NetworkUpdateIntervalSeconds = 1.0f / 30.0f;

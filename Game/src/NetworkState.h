#pragma once

#include "Engine/GameObject/GameObject.h"
#include "Engine/Network/Protocol.h"

// Milestone 3: the explicit, bounded boundary between the runtime object model
// (Engine::GameObject) and the network wire format (Engine::PlayerState). Spare
// Parts-specific glue, not a generic Engine abstraction — Engine must never know about
// PlayerState, and the network module must never know about GameObject (see CLAUDE.md's
// engine/game boundary; Engine::GameObject deliberately stores network identity as a
// plain std::uint32_t for exactly this reason).
//
// This is the seam a later checkpoint's second network exchange strategy (CSC 581) can
// hook without touching gameplay: a different ToPlayerState()-shaped function (e.g. one
// that extracts input instead of full state) is the only thing that would need to change.

/// Extracts this object's current state as a wire-ready PlayerState. Requires
/// object.HasNetworkId() (every player-type GameObject, local or remote, always has one —
/// see Game.h). A velocity-less object reports zero velocity.
Engine::PlayerState ToPlayerState(const Engine::GameObject& object);

/// Applies a received PlayerState onto an existing GameObject (position + velocity only —
/// identity is assumed already correct, since callers look objects up by network id
/// before calling this).
void ApplyPlayerState(Engine::GameObject& object, const Engine::PlayerState& state);

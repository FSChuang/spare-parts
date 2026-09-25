#pragma once

#include "Engine/Entity/Entity.h"
#include "Engine/Network/Protocol.h"
#include "Engine/Physics/PhysicsSystem.h"

#include "NetworkClient.h"

#include <string>
#include <unordered_map>

namespace Engine
{
	class InputManager;
	class Renderer;
}

// Minimal playable vertical slice for Spare Parts (拼裝求生): a static platform, a
// controllable player affected by gravity, and a patrolling enemy. All game-specific state and
// rules live here, never in Engine/ (ENGINEERING_SPEC.md §0, §5).
//
// Milestone 2 Section 2: the local player is still fully client-simulated (input,
// physics, collisions), unchanged. Remote players are visual replicas only — no local
// input, no local physics, no local collision response; their position/velocity are
// overwritten every frame from the server's latest Snapshot, keyed explicitly by the
// PlayerId the server assigned them (never by roster array index, which is
// intentionally unordered). Platform and enemy remain fully local; only player state
// is networked in Section 2.
class Game
{
public:
	explicit Game(const std::string& serverEndpoint);

	void Update(Engine::InputManager& input, float deltaTime);
	void Render(Engine::Renderer& renderer);

private:
	void UpdatePlayerMovement(Engine::InputManager& input);
	void UpdateEnemyPatrol(float deltaTime);
	void ResolvePlatformCollision();
	void ResolveEnemyCollision();
	void UpdateNetworking();

	Engine::Entity m_Platform;
	Engine::Entity m_Player;
	Engine::Entity m_Enemy;
	Engine::PhysicsSystem m_Physics;
	float m_EnemyPatrolDirection;

	NetworkClient m_Network;
	std::unordered_map<Engine::PlayerId, Engine::Entity> m_RemotePlayers;
};

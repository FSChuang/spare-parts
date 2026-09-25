#include "Game.h"

#include "Engine/Collision/Collision.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Renderer/Renderer.h"

#include "NetworkRate.h"

#include <SDL3/SDL_scancode.h>

#include <cmath>

namespace
{
	// Gameplay tunables (ENGINEERING_SPEC.md §9: configurable data stays at the top level,
	// never buried as unexplained magic numbers).
	constexpr float Gravity = 980.0f;             // px/s^2 downward acceleration
	constexpr float PlayerSpeed = 300.0f;         // px/s horizontal movement speed

	constexpr Engine::Vector2 PlayerSpawnPosition{ 700.0f, 200.0f };
	constexpr Engine::Vector2 PlayerSize{ 50.0f, 50.0f };
	constexpr Engine::Color PlayerColor{ 0, 128, 255, 255 };

	// Pre-connection default only (Milestone 2 Section 4 final checkpoint): once the
	// first real Snapshot arrives, m_Platform's position is overwritten from
	// snapshot.Platform every frame and this constant is never consulted again.
	constexpr Engine::Vector2 PlatformPosition{ 400.0f, 900.0f };
	constexpr Engine::Vector2 PlatformSize{ 1200.0f, 40.0f };
	constexpr Engine::Color PlatformColor{ 120, 120, 120, 255 };

	// Logical/reference game-world height (matches main.cpp's WindowHeight) — this is
	// deliberately NOT read from the current SDL window size, so out-of-bounds respawn
	// behavior stays identical regardless of window resizing/tiling or which renderer
	// scaling mode (Tab) is active; both map the same logical world onto whatever the
	// actual window happens to be.
	constexpr float WorldHeight = 1080.0f;

	constexpr float EnemyPatrolY = 850.0f;
	constexpr float EnemyPatrolLeftBound = 450.0f;
	constexpr float EnemyPatrolRightBound = 1500.0f;
	constexpr float EnemyPatrolSpeed = 150.0f;    // px/s
	constexpr Engine::Vector2 EnemySize{ 50.0f, 50.0f };
	constexpr Engine::Color EnemyColor{ 200, 40, 40, 255 };

	// Visually distinguishes remote players from the local one; purely a demo aid,
	// never used to identify a player (that's always the server-assigned PlayerId).
	constexpr Engine::Color RemotePlayerColor{ 255, 190, 0, 255 };
}

Game::Game(const std::string& serverEndpoint)
	: m_Platform(PlatformPosition, PlatformSize, PlatformColor),
	  m_Player(PlayerSpawnPosition, PlayerSize, PlayerColor),
	  m_Enemy({ EnemyPatrolLeftBound, EnemyPatrolY }, EnemySize, EnemyColor),
	  m_Physics(Gravity),
	  m_EnemyPatrolDirection(1.0f),
	  m_Network(serverEndpoint)
{
}

void Game::Update(Engine::InputManager& input, float deltaTime)
{
	UpdatePlayerMovement(input);
	m_Physics.Update(m_Player, deltaTime);
	ResolvePlatformCollision();
	ResolveOutOfBounds();

	UpdateEnemyPatrol(deltaTime);
	ResolveEnemyCollision();

	// Networking runs last so it always publishes this frame's final player state —
	// never a stale pre-respawn position, whether the respawn just happened above from
	// falling out of bounds or from the enemy collision check right before this call.
	UpdateNetworking(deltaTime);
}

void Game::Render(Engine::Renderer& renderer)
{
	renderer.DrawEntity(m_Platform);
	renderer.DrawEntity(m_Player);
	renderer.DrawEntity(m_Enemy);

	for (const auto& remotePlayer : m_RemotePlayers)
	{
		renderer.DrawEntity(remotePlayer.second);
	}
}

void Game::UpdatePlayerMovement(Engine::InputManager& input)
{
	Engine::Vector2 velocity = m_Player.GetVelocity();

	bool movingLeft = input.IsKeyPressed(SDL_SCANCODE_A) || input.IsKeyPressed(SDL_SCANCODE_LEFT);
	bool movingRight = input.IsKeyPressed(SDL_SCANCODE_D) || input.IsKeyPressed(SDL_SCANCODE_RIGHT);

	if (movingLeft && !movingRight)
	{
		velocity.X = -PlayerSpeed;
	}
	else if (movingRight && !movingLeft)
	{
		velocity.X = PlayerSpeed;
	}
	else
	{
		velocity.X = 0.0f;
	}

	m_Player.SetVelocity(velocity);
}

void Game::UpdateEnemyPatrol(float deltaTime)
{
	Engine::Vector2 velocity{ m_EnemyPatrolDirection * EnemyPatrolSpeed, 0.0f };
	m_Enemy.SetVelocity(velocity);
	m_Enemy.Move({ velocity.X * deltaTime, 0.0f });

	float enemyX = m_Enemy.GetPosition().X;
	if (enemyX <= EnemyPatrolLeftBound)
	{
		m_EnemyPatrolDirection = 1.0f;
	}
	else if (enemyX >= EnemyPatrolRightBound)
	{
		m_EnemyPatrolDirection = -1.0f;
	}
}

void Game::ResolvePlatformCollision()
{
	if (!Engine::IsColliding(m_Player, m_Platform))
	{
		return;
	}

	Engine::Vector2 velocity = m_Player.GetVelocity();
	if (velocity.Y <= 0.0f)
	{
		return;
	}

	Engine::Vector2 position = m_Player.GetPosition();
	position.Y = m_Platform.GetPosition().Y - m_Player.GetSize().Y;
	m_Player.SetPosition(position);

	velocity.Y = 0.0f;
	m_Player.SetVelocity(velocity);
}

void Game::ResolveOutOfBounds()
{
	// Entity position is its top-left corner (see ResolvePlatformCollision's own use of
	// GetPosition().Y as the top edge when landing the player on the platform), so
	// "fully below the world" — not merely partially visible — means even the player's
	// TOP edge has passed the logical world's bottom edge.
	if (m_Player.GetPosition().Y >= WorldHeight)
	{
		RespawnPlayer();
	}
}

void Game::ResolveEnemyCollision()
{
	if (!Engine::IsColliding(m_Player, m_Enemy))
	{
		return;
	}

	RespawnPlayer();
}

void Game::RespawnPlayer()
{
	m_Player.SetPosition(PlayerSpawnPosition);
	m_Player.SetVelocity({ 0.0f, 0.0f });
}

void Game::UpdateNetworking(float deltaTime)
{
	// Milestone 2 Section 4 final checkpoint: publish is throttled to a logical network
	// tic rate (NetworkUpdateIntervalSeconds at Timeline scale 1x), not once per render
	// frame. deltaTime here is already Timeline-scaled (Application computes it from
	// Timeline::GetDeltaTime()), so this single accumulator is what makes 0.5x/1x/2x
	// scale directly halve/double the real-world outbound message rate. While paused,
	// Timeline reports deltaTime == 0, so the accumulator simply never advances and
	// sends naturally stop — no separate pause check is needed here.
	//
	// If deltaTime crosses more than one interval in a single frame (a large 2x-scaled
	// frame, or a frame after a long stall), only the single newest PlayerState is
	// published — never a burst of historical ones — and std::fmod collapses the
	// accumulator back into range while preserving the fractional remainder, exactly
	// mirroring how PublishState's own single-slot mailbox already discards anything
	// but the latest value.
	m_NetworkUpdateAccumulator += deltaTime;
	if (m_NetworkUpdateAccumulator >= NetworkUpdateIntervalSeconds)
	{
		m_NetworkUpdateAccumulator = std::fmod(m_NetworkUpdateAccumulator, NetworkUpdateIntervalSeconds);

		Engine::Vector2 position = m_Player.GetPosition();
		Engine::Vector2 velocity = m_Player.GetVelocity();
		Engine::PlayerState localState{ m_Network.GetLocalPlayerId(), position.X, position.Y, velocity.X,
			                             velocity.Y };

		// Non-blocking (Milestone 2 Section 3): PublishState never touches the
		// network, it only replaces the latest not-yet-sent state for the worker
		// thread to pick up.
		m_Network.PublishState(localState);
	}

	// Snapshot application (remote roster AND the shared platform) must remain
	// available every frame regardless of the publish throttle above — remote
	// rendering is never held back to the local send rate. If nothing has arrived yet
	// (still connecting, JOIN no longer carries an initial roster, or the last exchange
	// errored), keep showing the last known state rather than clearing/hiding anything.
	std::optional<Engine::Snapshot> snapshot = m_Network.GetLatestSnapshot();
	if (!snapshot.has_value())
	{
		return;
	}

	// The shared moving platform is a visual/world replica of server state: its
	// position is overwritten wholesale from the server's own authoritative
	// computation, never locally advanced with this client's deltaTime.
	m_Platform.SetPosition({ snapshot->Platform.PositionX, snapshot->Platform.PositionY });
	m_Platform.SetVelocity({ snapshot->Platform.VelocityX, snapshot->Platform.VelocityY });

	const std::vector<Engine::PlayerState>& roster = snapshot->Roster;

	std::unordered_map<Engine::PlayerId, Engine::PlayerState> latestRemoteStates;
	for (const Engine::PlayerState& state : roster)
	{
		if (state.Id != m_Network.GetLocalPlayerId())
		{
			latestRemoteStates[state.Id] = state;
		}
	}

	// A PlayerId absent from this snapshot has disconnected (or hasn't joined) —
	// remove its entity so late-join and clean-disconnect are both naturally reflected.
	for (auto it = m_RemotePlayers.begin(); it != m_RemotePlayers.end();)
	{
		if (latestRemoteStates.find(it->first) == latestRemoteStates.end())
		{
			it = m_RemotePlayers.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (const auto& entry : latestRemoteStates)
	{
		Engine::PlayerId id = entry.first;
		const Engine::PlayerState& state = entry.second;

		auto existingRemotePlayer = m_RemotePlayers.find(id);
		if (existingRemotePlayer == m_RemotePlayers.end())
		{
			Engine::Entity remoteEntity({ state.PositionX, state.PositionY }, PlayerSize, RemotePlayerColor);
			remoteEntity.SetVelocity({ state.VelocityX, state.VelocityY });
			m_RemotePlayers.emplace(id, remoteEntity);
		}
		else
		{
			existingRemotePlayer->second.SetPosition({ state.PositionX, state.PositionY });
			existingRemotePlayer->second.SetVelocity({ state.VelocityX, state.VelocityY });
		}
	}
}

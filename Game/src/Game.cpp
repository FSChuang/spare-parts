#include "Game.h"

#include "Engine/Collision/Collision.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Renderer/Renderer.h"

#include "NetworkRate.h"

#include <SDL3/SDL_scancode.h>

#include <chrono>
#include <cmath>
#include <unordered_set>

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
	  m_Network(serverEndpoint),
	  // Set in the past by exactly one interval, so the very first Update() call
	  // triggers an immediate server refresh rather than waiting a full interval
	  // first — steady_clock's own epoch is unspecified, so this is computed relative
	  // to "now" rather than assumed to already be far enough in the past.
	  m_LastServerRefreshTime(std::chrono::steady_clock::now() - ServerRefreshInterval)
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
	// Milestone 2 Section 5 final integration: PeerClient can only be constructed once
	// NetworkClient's asynchronous bootstrap handshake has actually completed — its
	// PlayerId and P2P port are not known synchronously at Game construction time.
	// Created exactly once; never destroyed/recreated while this Game instance lives.
	// Constructing only starts PeerClient's worker thread; it never waits for peer
	// traffic, so this never blocks Game/main thread.
	if (!m_PeerClient && m_Network.IsConnected())
	{
		Engine::PlayerId localId = m_Network.GetLocalPlayerId();
		std::uint16_t localP2pPort = m_Network.GetLocalP2pPort();
		if (localId != 0 && localP2pPort != 0)
		{
			m_PeerClient = std::make_unique<PeerClient>(localId, localP2pPort);
		}
	}

	Engine::Vector2 position = m_Player.GetPosition();
	Engine::Vector2 velocity = m_Player.GetVelocity();
	Engine::PlayerState localState{ m_Network.GetLocalPlayerId(), position.X, position.Y, velocity.X, velocity.Y };

	// P2P gameplay publish: Timeline-scaled, exactly as Section 4 established
	// (0.5x/1x/2x -> ~15/30/60 Hz), now redirected to PeerClient instead of the server.
	// deltaTime here is already Timeline-scaled (Application computes it from
	// Timeline::GetDeltaTime()), so this accumulator is what makes Timeline scale
	// directly halve/double the real-world P2P publish rate. While paused, Timeline
	// reports deltaTime == 0, so the accumulator simply never advances and P2P sends
	// naturally stop — no separate pause check needed. If deltaTime crosses more than
	// one interval in a single frame, only the single newest state is published — never
	// a burst — and std::fmod collapses the accumulator while preserving the
	// fractional remainder.
	m_NetworkUpdateAccumulator += deltaTime;
	if (m_NetworkUpdateAccumulator >= NetworkUpdateIntervalSeconds)
	{
		m_NetworkUpdateAccumulator = std::fmod(m_NetworkUpdateAccumulator, NetworkUpdateIntervalSeconds);
		if (m_PeerClient)
		{
			m_PeerClient->PublishState(localState);
		}
	}

	// Server-facing session refresh: fixed real-time cadence, deliberately independent
	// of both Timeline scale AND pause — a paused player should still keep its server
	// membership/peer-directory/platform freshness current. This is what keeps
	// Snapshot.Roster/Peers/Platform arriving even while the P2P accumulator above is
	// frozen at deltaTime == 0. The PlayerState sent here is advisory/membership data
	// only from this point on — nothing below ever reads it back for remote rendering.
	auto now = std::chrono::steady_clock::now();
	if (now - m_LastServerRefreshTime >= ServerRefreshInterval)
	{
		m_LastServerRefreshTime = now;
		m_Network.PublishState(localState);
	}

	// Snapshot application (shared platform, membership, peer directory) must remain
	// available every frame regardless of either publish schedule above — remote
	// rendering is never held back to either send rate. If nothing has arrived yet
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

	// Peer directory: hand the server's current PlayerId -> P2P port mapping straight
	// to PeerClient. PeerClient already filters out the local player's own id; Game
	// never assumes anything about vector order here.
	if (m_PeerClient)
	{
		m_PeerClient->UpdatePeers(snapshot->Peers);
	}

	// Roster is membership ONLY, as of this final integration: it decides WHICH remote
	// PlayerIds are allowed to exist, never WHERE they are. No PlayerState field from
	// Roster is read below except Id — remote transform comes exclusively from
	// PeerClient::GetLatestPeerStates() further down.
	Engine::PlayerId localId = m_Network.GetLocalPlayerId();
	std::unordered_set<Engine::PlayerId> activeRemoteIds;
	for (const Engine::PlayerState& state : snapshot->Roster)
	{
		if (state.Id != localId)
		{
			activeRemoteIds.insert(state.Id);
		}
	}

	// A PlayerId absent from server membership has disconnected (or hasn't joined) —
	// remove its entity so late-join and clean-disconnect are both naturally reflected.
	// Server membership has authority over EXISTENCE even if PeerClient still happens to
	// hold a stale last-known state for it.
	for (auto it = m_RemotePlayers.begin(); it != m_RemotePlayers.end();)
	{
		if (activeRemoteIds.find(it->first) == activeRemoteIds.end())
		{
			it = m_RemotePlayers.erase(it);
		}
		else
		{
			++it;
		}
	}

	if (!m_PeerClient)
	{
		return;
	}

	std::unordered_map<Engine::PlayerId, Engine::PlayerState> latestPeerStates = m_PeerClient->GetLatestPeerStates();
	for (Engine::PlayerId id : activeRemoteIds)
	{
		auto peerStateIt = latestPeerStates.find(id);
		if (peerStateIt == latestPeerStates.end())
		{
			// A currently-active member with no P2P state yet (its PUB/SUB
			// subscription may not have finished establishing) simply has no entity
			// yet — never created from server data as a placeholder.
			continue;
		}

		const Engine::PlayerState& state = peerStateIt->second;
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

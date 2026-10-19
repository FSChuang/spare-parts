#include "Game.h"

#include "Engine/Collision/Collision.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Renderer/Renderer.h"

#include "GameRules.h"
#include "NetworkRate.h"
#include "NetworkState.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	// Gameplay tunables (ENGINEERING_SPEC.md §9: configurable data stays at the top level,
	// never buried as unexplained magic numbers).
	constexpr float Gravity = 980.0f;             // px/s^2 downward acceleration
	constexpr float PlayerSpeed = 300.0f;         // px/s horizontal movement speed
	constexpr float JumpSpeed = 650.0f;           // px/s upward (negative Y) impulse applied on jump

	constexpr Engine::Vector2 PlayerSize{ 50.0f, 50.0f };
	constexpr Engine::Color PlayerColor{ 0, 128, 255, 255 };

	// The world is two 1920-wide screens placed side by side (Milestone 3 side-scrolling).
	// ViewWidth matches main.cpp's WindowWidth/Renderer's reference resolution.
	constexpr float ViewWidth = 1920.0f;
	constexpr float WorldWidth = ViewWidth * 2.0f;
	// Logical/reference game-world height (matches main.cpp's WindowHeight) — this is
	// deliberately NOT read from the current SDL window size, so death-zone/respawn
	// behavior stays identical regardless of window resizing/tiling or which renderer
	// scaling mode (Tab) is active; both map the same logical world onto whatever the
	// actual window happens to be.
	constexpr float WorldHeight = 1080.0f;

	// --- Static platforms (Milestone 3): three, each a visibly distinct position, size,
	// and color. Floor is the starting ground (screen 0); the two ledges sit in screen 1,
	// giving the player a reason to cross the scrolling boundary to reach them. ---
	constexpr Engine::Vector2 StaticFloorPosition{ 0.0f, 1000.0f };
	constexpr Engine::Vector2 StaticFloorSize{ 850.0f, 40.0f };
	constexpr Engine::Color StaticFloorColor{ 130, 130, 130, 255 };

	constexpr Engine::Vector2 StaticLedgePosition{ 2100.0f, 820.0f };
	constexpr Engine::Vector2 StaticLedgeSize{ 220.0f, 30.0f };
	constexpr Engine::Color StaticLedgeColor{ 150, 100, 50, 255 };

	constexpr Engine::Vector2 StaticHighLedgePosition{ 2700.0f, 650.0f };
	constexpr Engine::Vector2 StaticHighLedgeSize{ 160.0f, 25.0f };
	constexpr Engine::Color StaticHighLedgeColor{ 60, 160, 160, 255 };

	// --- Moving platforms (Milestone 3): two visibly distinct patterns, both
	// server-authoritative (see ServerPlatform.h/.cpp in the server executable). These
	// positions are pre-connection defaults only, matching ServerPlatform.cpp's own
	// bound constants — once the first real Snapshot arrives, each platform's position
	// is overwritten every frame from snapshot.Platform/Platform2 and these constants are
	// never consulted again.
	constexpr Engine::Vector2 HorizontalPlatformDefaultPosition{ 900.0f, 900.0f };
	constexpr Engine::Vector2 HorizontalPlatformSize{ 220.0f, 30.0f };
	constexpr Engine::Color HorizontalPlatformColor{ 90, 90, 200, 255 };

	constexpr Engine::Vector2 VerticalPlatformDefaultPosition{ 2450.0f, 500.0f };
	constexpr Engine::Vector2 VerticalPlatformSize{ 150.0f, 30.0f };
	constexpr Engine::Color VerticalPlatformColor{ 200, 90, 160, 255 };

	// --- Hidden objects (Milestone 3): spawn points, the death zone, and the scrolling
	// boundary all have no render property, so Renderer::DrawObject silently skips them
	// — they still fully participate in collision via Transform data alone. ---
	constexpr Engine::Vector2 SpawnPointSize{ 1.0f, 1.0f }; // a point, not an area; size is unused by anything
	constexpr Engine::Vector2 SpawnAPosition{ 80.0f, 900.0f };  // above the starting floor, screen 0
	constexpr Engine::Vector2 SpawnBPosition{ 2150.0f, 750.0f }; // above the near ledge, screen 1

	// Spans the full world width, well below the playable area — falling past the bottom
	// of any platform eventually collides with this instead of a raw "if y > threshold"
	// check (Milestone 3: formalizes the old Milestone 2 fall-off rule as a real,
	// collidable GameObject).
	constexpr Engine::Vector2 DeathZonePosition{ 0.0f, WorldHeight };
	constexpr Engine::Vector2 DeathZoneSize{ WorldWidth, 400.0f };

	// A thin vertical strip at the world-space seam between the two screens. Crossing it
	// snaps the presentational camera offset between the two screens — see
	// Game::UpdateScrolling().
	constexpr Engine::Vector2 BoundarySize{ 10.0f, WorldHeight };
	constexpr Engine::Vector2 BoundaryPosition{ ViewWidth - BoundarySize.X / 2.0f, 0.0f };

	constexpr float EnemyPatrolY = 850.0f;
	constexpr float EnemyPatrolLeftBound = 450.0f;
	constexpr float EnemyPatrolRightBound = 1500.0f;
	constexpr float EnemyPatrolSpeed = 150.0f;    // px/s
	constexpr Engine::Vector2 EnemySize{ 50.0f, 50.0f };
	constexpr Engine::Color EnemyColor{ 200, 40, 40, 255 };

	// Visually distinguishes remote players from the local one; purely a demo aid,
	// never used to identify a player (that's always the server-assigned PlayerId).
	constexpr Engine::Color RemotePlayerColor{ 255, 190, 0, 255 };

	// Local-player sentinel network id before NetworkClient's handshake has assigned a
	// real one (Engine::Network/Protocol.h: PlayerId 0 is reserved "unassigned"). The
	// local player GameObject always has a network-id property, even before a real id is
	// known, so ToPlayerState() never needs a special case for "not connected yet".
	constexpr std::uint32_t UnassignedNetworkId = 0;
}

Game::Game(const std::string& serverEndpoint)
	: m_Physics(Gravity),
	  m_EnemyPatrolDirection(1.0f),
	  m_Network(serverEndpoint),
	  // Set in the past by exactly one interval, so the very first Update() call
	  // triggers an immediate server refresh rather than waiting a full interval
	  // first — steady_clock's own epoch is unspecified, so this is computed relative
	  // to "now" rather than assumed to already be far enough in the past.
	  m_LastServerRefreshTime(std::chrono::steady_clock::now() - ServerRefreshInterval)
{
	CreateMovingPlatforms();
	CreateStaticPlatforms();
	CreateSpawnPoints();
	CreateDeathZones();
	CreateBoundaries();

	Engine::GameObject& player = m_Objects.Create(Engine::ObjectType::Player, SelectSpawnPosition(), PlayerSize);
	player.SetRender(PlayerColor);
	player.SetVelocity({ 0.0f, 0.0f });
	player.SetNetworkId(UnassignedNetworkId);
	m_LocalPlayerObjectId = player.GetId();

	Engine::GameObject& enemy =
	    m_Objects.Create(Engine::ObjectType::Generic, { EnemyPatrolLeftBound, EnemyPatrolY }, EnemySize);
	enemy.SetRender(EnemyColor);
	m_EnemyObjectId = enemy.GetId();
}

void Game::CreateMovingPlatforms()
{
	Engine::GameObject& horizontal =
	    m_Objects.Create(Engine::ObjectType::Platform, HorizontalPlatformDefaultPosition, HorizontalPlatformSize);
	horizontal.SetRender(HorizontalPlatformColor);
	m_PlatformObjectId = horizontal.GetId();

	Engine::GameObject& vertical =
	    m_Objects.Create(Engine::ObjectType::Platform, VerticalPlatformDefaultPosition, VerticalPlatformSize);
	vertical.SetRender(VerticalPlatformColor);
	m_VerticalPlatformObjectId = vertical.GetId();
}

void Game::CreateStaticPlatforms()
{
	m_Objects.Create(Engine::ObjectType::Platform, StaticFloorPosition, StaticFloorSize).SetRender(StaticFloorColor);
	m_Objects.Create(Engine::ObjectType::Platform, StaticLedgePosition, StaticLedgeSize).SetRender(StaticLedgeColor);
	m_Objects.Create(Engine::ObjectType::Platform, StaticHighLedgePosition, StaticHighLedgeSize)
	    .SetRender(StaticHighLedgeColor);
}

void Game::CreateSpawnPoints()
{
	m_Objects.Create(Engine::ObjectType::SpawnPoint, SpawnAPosition, SpawnPointSize);
	m_Objects.Create(Engine::ObjectType::SpawnPoint, SpawnBPosition, SpawnPointSize);
}

void Game::CreateDeathZones()
{
	Engine::GameObject& deathZone = m_Objects.Create(Engine::ObjectType::DeathZone, DeathZonePosition, DeathZoneSize);
	m_DeathZoneObjectId = deathZone.GetId();
}

void Game::CreateBoundaries()
{
	Engine::GameObject& boundary = m_Objects.Create(Engine::ObjectType::Boundary, BoundaryPosition, BoundarySize);
	m_BoundaryObjectId = boundary.GetId();
}

Engine::Vector2 Game::SelectSpawnPosition() const
{
	std::vector<std::pair<Engine::ObjectId, Engine::Vector2>> spawns;
	for (const auto& [id, object] : m_Objects)
	{
		if (object.GetType() == Engine::ObjectType::SpawnPoint)
		{
			spawns.emplace_back(id, object.GetPosition());
		}
	}

	ENGINE_ASSERT(!spawns.empty(), "Game requires at least one spawn point");

	// Sorted by ObjectId (assigned sequentially at scene-construction time, identical on
	// every client) rather than relying on GameObjectRegistry's own unspecified
	// iteration order, so "spawn index N" means the same world position on every client.
	std::sort(spawns.begin(), spawns.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

	// Before a PlayerId is known (the unassigned sentinel 0), SelectSpawnIndex naturally
	// resolves to spawns[0].
	Engine::PlayerId localId = m_Network.GetLocalPlayerId();
	std::size_t index = SelectSpawnIndex(localId, spawns.size());
	return spawns[index].second;
}

void Game::Update(Engine::InputManager& input, float deltaTime)
{
	UpdatePlayerMovement(input);
	m_Physics.Update(*m_Objects.Find(m_LocalPlayerObjectId), deltaTime);
	ResolvePlatformLanding();
	ResolveDeathZone();

	UpdateEnemyPatrol(deltaTime);
	ResolveEnemyCollision();

	UpdateScrolling();

	// Networking runs last so it always publishes this frame's final player state —
	// never a stale pre-respawn position, whether the respawn just happened above from
	// entering the death zone or from the enemy collision check right before this call.
	UpdateNetworking(deltaTime);
}

void Game::Render(Engine::Renderer& renderer)
{
	// Every object draws itself (or doesn't): Engine::Renderer::DrawObject silently skips
	// any object with no render property (spawn points, the death zone, the boundary),
	// so no special case is needed here for them. Each visible object is drawn from a
	// COPY translated by -m_CameraOffsetX — the registry's own world-coordinate
	// GameObjects (what physics/collision/networking operate on) are never modified for
	// presentation purposes.
	for (auto& [id, object] : m_Objects)
	{
		if (!object.HasRender())
		{
			continue;
		}

		Engine::GameObject view = object;
		view.SetPosition({ object.GetPosition().X - m_CameraOffsetX, object.GetPosition().Y });
		renderer.DrawObject(view);
	}
}

void Game::UpdatePlayerMovement(Engine::InputManager& input)
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::Vector2 velocity = player.GetVelocity();

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

	// Edge-triggered (IsKeyJustPressed, not IsKeyPressed) and gated on m_IsGrounded (set
	// by the END of the PREVIOUS frame's ResolvePlatformLanding()) — holding the key
	// down never queues repeated jumps, and there is no mid-air jump at all.
	bool jumpPressed = input.IsKeyJustPressed(SDL_SCANCODE_SPACE) || input.IsKeyJustPressed(SDL_SCANCODE_W) ||
	                    input.IsKeyJustPressed(SDL_SCANCODE_UP);
	if (jumpPressed && m_IsGrounded)
	{
		velocity.Y = -JumpSpeed;
	}

	player.SetVelocity(velocity);
}

void Game::UpdateEnemyPatrol(float deltaTime)
{
	Engine::GameObject& enemy = *m_Objects.Find(m_EnemyObjectId);

	Engine::Vector2 velocity{ m_EnemyPatrolDirection * EnemyPatrolSpeed, 0.0f };
	enemy.SetVelocity(velocity);
	enemy.Move({ velocity.X * deltaTime, 0.0f });

	float enemyX = enemy.GetPosition().X;
	if (enemyX <= EnemyPatrolLeftBound)
	{
		m_EnemyPatrolDirection = 1.0f;
	}
	else if (enemyX >= EnemyPatrolRightBound)
	{
		m_EnemyPatrolDirection = -1.0f;
	}
}

void Game::ResolvePlatformLanding()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::Vector2 velocity = player.GetVelocity();

	m_IsGrounded = false;

	// Landing (and therefore becoming grounded) only ever happens while falling. Moving
	// upward (e.g. the frame right after a jump) never resolves as a landing — same
	// behavior as the original single-platform logic, just generalized to check every
	// Platform-typed object instead of exactly one.
	if (velocity.Y <= 0.0f)
	{
		return;
	}

	for (auto& [id, object] : m_Objects)
	{
		if (object.GetType() != Engine::ObjectType::Platform || !Engine::IsColliding(player, object))
		{
			continue;
		}

		Engine::Vector2 position = player.GetPosition();
		position.Y = object.GetPosition().Y - player.GetSize().Y;
		player.SetPosition(position);

		velocity.Y = 0.0f;
		player.SetVelocity(velocity);
		m_IsGrounded = true;
		// Lands on the first colliding platform found; with no overlapping platforms by
		// design, this is deterministic. Milestone 3 deliberately does not implement
		// moving-platform carrying (horizontal attachment) — the original single-platform
		// behavior never did either.
		break;
	}
}

void Game::ResolveDeathZone()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::GameObject& deathZone = *m_Objects.Find(m_DeathZoneObjectId);

	if (Engine::IsColliding(player, deathZone))
	{
		RespawnPlayer();
	}
}

void Game::ResolveEnemyCollision()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::GameObject& enemy = *m_Objects.Find(m_EnemyObjectId);

	if (!Engine::IsColliding(player, enemy))
	{
		return;
	}

	RespawnPlayer();
}

void Game::RespawnPlayer()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	player.SetPosition(SelectSpawnPosition());
	player.SetVelocity({ 0.0f, 0.0f });
	m_IsGrounded = false;
}

void Game::UpdateScrolling()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::GameObject& boundary = *m_Objects.Find(m_BoundaryObjectId);

	if (!Engine::IsColliding(player, boundary))
	{
		return;
	}

	m_CameraOffsetX = NextCameraOffsetX(m_CameraOffsetX, player.GetVelocity().X, ViewWidth);
}

void Game::UpdateNetworking(float deltaTime)
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);

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
			player.SetNetworkId(localId);
			m_PeerClient = std::make_unique<PeerClient>(localId, localP2pPort);
		}
	}

	// The local player's network-relevant state, extracted from its GameObject through
	// the explicit Milestone 3 boundary (see NetworkState.h) rather than built from a
	// second, independent player-state representation.
	Engine::PlayerState localState = ToPlayerState(player);

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
	// Snapshot.Roster/Peers/Platform(2) arriving even while the P2P accumulator above is
	// frozen at deltaTime == 0. The PlayerState sent here is advisory/membership data
	// only from this point on — nothing below ever reads it back for remote rendering.
	auto now = std::chrono::steady_clock::now();
	if (now - m_LastServerRefreshTime >= ServerRefreshInterval)
	{
		m_LastServerRefreshTime = now;
		m_Network.PublishState(localState);
	}

	// Snapshot application (shared platforms, membership, peer directory) must remain
	// available every frame regardless of either publish schedule above — remote
	// rendering is never held back to either send rate. If nothing has arrived yet
	// (still connecting, JOIN no longer carries an initial roster, or the last exchange
	// errored), keep showing the last known state rather than clearing/hiding anything.
	std::optional<Engine::Snapshot> snapshot = m_Network.GetLatestSnapshot();
	if (!snapshot.has_value())
	{
		return;
	}

	// Both shared moving platforms are visual/world replicas of server state: their
	// positions are overwritten wholesale from the server's own authoritative
	// computation, never locally advanced with this client's deltaTime.
	Engine::GameObject& horizontalPlatform = *m_Objects.Find(m_PlatformObjectId);
	horizontalPlatform.SetPosition({ snapshot->Platform.PositionX, snapshot->Platform.PositionY });
	horizontalPlatform.SetVelocity({ snapshot->Platform.VelocityX, snapshot->Platform.VelocityY });

	Engine::GameObject& verticalPlatform = *m_Objects.Find(m_VerticalPlatformObjectId);
	verticalPlatform.SetPosition({ snapshot->Platform2.PositionX, snapshot->Platform2.PositionY });
	verticalPlatform.SetVelocity({ snapshot->Platform2.VelocityX, snapshot->Platform2.VelocityY });

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
	// remove its GameObject so late-join and clean-disconnect are both naturally
	// reflected. Server membership has authority over EXISTENCE even if PeerClient
	// still happens to hold a stale last-known state for it. The local player's own
	// object is excluded by id, never by type/network-id filtering, since it is the
	// only other GameObject that ever carries a network identity.
	std::vector<Engine::ObjectId> staleRemoteObjects;
	for (auto& [id, object] : m_Objects)
	{
		if (id == m_LocalPlayerObjectId || !object.HasNetworkId())
		{
			continue;
		}
		if (activeRemoteIds.find(object.GetNetworkId()) == activeRemoteIds.end())
		{
			staleRemoteObjects.push_back(id);
		}
	}
	for (Engine::ObjectId id : staleRemoteObjects)
	{
		m_Objects.Remove(id);
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
			// subscription may not have finished establishing) simply has no
			// GameObject yet — never created from server data as a placeholder.
			continue;
		}

		const Engine::PlayerState& state = peerStateIt->second;
		Engine::GameObject* remotePlayer = m_Objects.FindByNetworkId(id);
		if (remotePlayer == nullptr)
		{
			Engine::GameObject& created =
			    m_Objects.Create(Engine::ObjectType::Player, { state.PositionX, state.PositionY }, PlayerSize);
			created.SetRender(RemotePlayerColor);
			created.SetVelocity({ state.VelocityX, state.VelocityY });
			created.SetNetworkId(id);
		}
		else
		{
			ApplyPlayerState(*remotePlayer, state);
		}
	}
}

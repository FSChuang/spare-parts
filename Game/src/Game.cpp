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
#include <cstdio>
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
	// collidable GameObject). Bugfix checkpoint: made effectively bottomless (was 400px
	// deep) so a single abnormally large physics step — gravity has no velocity cap, and
	// a slow frame (e.g. four GUI client windows competing for CPU) can produce a large
	// deltaTime — cannot tunnel the player all the way through a merely-400px-deep zone
	// and fall forever, invisible, with no collision ever firing again. Still an ordinary
	// bounded AABB GameObject tested by the same Engine::IsColliding as everything else —
	// no new death rule, no raw y-threshold check.
	constexpr Engine::Vector2 DeathZonePosition{ 0.0f, WorldHeight };
	constexpr Engine::Vector2 DeathZoneSize{ WorldWidth, 1000000.0f };

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

	// A remote player is drawn in this color while its "recently died" marker is active
	// (Milestone 4 networked Death event) — purely presentational, never networked.
	constexpr Engine::Color RemotePlayerDeadColor{ 255, 255, 255, 255 };

	// The local player is drawn in this color while a replay is playing, so it is obvious
	// that what is on screen is the recording and not live play.
	constexpr Engine::Color ReplayPlayerColor{ 0, 220, 120, 255 };

	// Replay controls (Milestone 4). F-keys, so they never clash with movement, jump, or the
	// engine's own Tab / P / 1-2-3 debug keys. (On a Mac laptop hold fn if the OS claims them.)
	constexpr SDL_Scancode ReplayStartKey = SDL_SCANCODE_F5;
	constexpr SDL_Scancode ReplayStopKey = SDL_SCANCODE_F6;
	constexpr SDL_Scancode ReplayPlayKey = SDL_SCANCODE_F7;

	// Local-player sentinel network id before NetworkClient's handshake has assigned a
	// real one (Engine::Network/Protocol.h: PlayerId 0 is reserved "unassigned"). The
	// local player GameObject always has a network-id property, even before a real id is
	// known, so ToPlayerState() never needs a special case for "not connected yet".
	constexpr std::uint32_t UnassignedNetworkId = 0;
}

Game::Game(const std::string& serverEndpoint, std::function<double()> timeSource)
	: m_Events(std::move(timeSource)),
	  m_Physics(Gravity),
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

	SubscribeEventLogging();

	m_LocalPlayer = std::make_unique<LocalPlayerController>(
	    m_Events, m_Objects, LocalPlayerObjects{ m_LocalPlayerObjectId, m_EnemyObjectId, m_DeathZoneObjectId },
	    LocalPlayerConfig{ PlayerSpeed, JumpSpeed, ViewWidth }, [this] { return SelectSpawnPosition(); });

	// The initial spawn also goes through the event system: the player object is created at
	// its designated position above (a GameObject has to exist before an event can name it),
	// and this Spawn event — handled in the first Update()'s input phase — is what formally
	// spawns it (zero velocity, not grounded, camera on the spawn's screen). SelectSpawnPosition()
	// still resolves via the not-yet-assigned PlayerId here; that pre-existing behavior is
	// deliberately unchanged.
	m_LocalPlayer->RequestSpawn(m_Objects.Find(m_LocalPlayerObjectId)->GetPosition());

	// The local player's own Death events are announced to peers through m_PeerClient, which
	// only exists once the server handshake has completed — before that there is nobody to
	// tell. (PublishEvent just queues; the PeerClient worker does the sending.)
	m_PeerEvents = std::make_unique<PeerEventBridge>(
	    m_Events, m_Objects, m_LocalPlayerObjectId, timeSource,
	    [this](Engine::NetworkEventKind kind)
	    {
		    if (m_PeerClient)
		    {
			    m_PeerClient->PublishEvent(kind);
		    }
	    });

	m_Replay = std::make_unique<ReplayController>(
	    m_Events, m_Objects, *m_LocalPlayer, *m_PeerEvents,
	    ReplayObjects{ m_LocalPlayerObjectId, m_PlatformObjectId, m_VerticalPlatformObjectId, m_EnemyObjectId },
	    std::move(timeSource));
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
	// Frame order, with two deliberate event-dispatch phases:
	//
	//  1. BeginFrame        allow a new death this frame
	//  2. PollInputEvents   replay keys (F5/F6/F7) and held-key CHANGES / fresh jump presses
	//                       become events
	//  3. DispatchDue #1    replay control takes effect; input effects apply BEFORE physics
	//                       (move intent, jump impulse); the initial Spawn is delivered on the
	//                       first frame; while a replay plays, its re-scheduled events are
	//                       delivered here (observed only)
	//
	//  LIVE (not replaying):
	//  4. movement + physics integration (direct, not events)
	//  5. enemy patrol      direct local simulation, before the hazard test so the player
	//                       is tested against the enemy's position this frame
	//  6. landing + hazard detection (direct geometry); a NEW landing, or touching the death
	//                       zone / enemy, raises a Collision event
	//  7. DispatchDue #2    the whole chain runs now, in one call: Collision -> (Death ->
	//                       Spawn). A death never waits for the next frame, and the
	//                       respawned position/camera are final before networking reads them
	//  8. scrolling         camera snap on boundary contact (direct presentation math)
	//  (While a replay PLAYS, 4-8 are skipped: the recorded frames own the player, the
	//   platforms, the enemy and the camera, and nothing live may touch them.)
	//
	//  9. networking        publishes this frame's final player state (the saved live state,
	//                       not the replay, while one plays)
	// 10. UpdatePlayback    applies the recorded frame for the elapsed playback time, and
	//                       ends playback (restoring the live state) when the span is over;
	//                       it runs AFTER networking so the recording wins over the live
	//                       platform positions networking just wrote
	// 11. CaptureFrame      while recording, stores this frame's final state
	m_LocalPlayer->BeginFrame();

	PollInputEvents(input);
	m_Events.DispatchDue();

	if (!m_Replay->IsPlaying())
	{
		m_LocalPlayer->ApplyHorizontalMovement();
		m_Physics.Update(*m_Objects.Find(m_LocalPlayerObjectId), deltaTime);

		UpdateEnemyPatrol(deltaTime);

		m_LocalPlayer->ResolvePlatformLanding();
		m_LocalPlayer->DetectHazardCollisions();
		m_Events.DispatchDue();

		UpdateScrolling();
	}

	// Networking runs after the local simulation so it always publishes this frame's final
	// player state — never a stale pre-respawn position.
	UpdateNetworking(deltaTime);

	m_Replay->UpdatePlayback();
	m_Replay->CaptureFrame();
}

void Game::Render(Engine::Renderer& renderer)
{
	// Every object draws itself (or doesn't): Engine::Renderer::DrawObject silently skips
	// any object with no render property (spawn points, the death zone, the boundary),
	// so no special case is needed here for them. Each visible object is drawn from a
	// COPY translated by -the camera offset — the registry's own world-coordinate
	// GameObjects (what physics/collision/networking operate on) are never modified for
	// presentation purposes.
	const float cameraOffsetX = m_LocalPlayer->CameraOffsetX();
	for (auto& [id, object] : m_Objects)
	{
		if (!object.HasRender())
		{
			continue;
		}

		Engine::GameObject view = object;
		view.SetPosition({ object.GetPosition().X - cameraOffsetX, object.GetPosition().Y });
		if (m_PeerEvents->IsMarkedDead(id))
		{
			view.SetRender(RemotePlayerDeadColor);
		}
		else if (id == m_LocalPlayerObjectId && m_Replay->IsPlaying())
		{
			view.SetRender(ReplayPlayerColor);
		}
		renderer.DrawObject(view);
	}
}

void Game::PollInputEvents(Engine::InputManager& input)
{
	bool movingLeft = input.IsKeyPressed(SDL_SCANCODE_A) || input.IsKeyPressed(SDL_SCANCODE_LEFT);
	bool movingRight = input.IsKeyPressed(SDL_SCANCODE_D) || input.IsKeyPressed(SDL_SCANCODE_RIGHT);

	// Jump is edge-triggered (IsKeyJustPressed, not IsKeyPressed): holding the key never
	// queues repeated jumps. Whether the jump actually happens (grounded only) is decided by
	// the Jump event's handler.
	bool jumpPressed = input.IsKeyJustPressed(SDL_SCANCODE_SPACE) || input.IsKeyJustPressed(SDL_SCANCODE_W) ||
	                    input.IsKeyJustPressed(SDL_SCANCODE_UP);

	m_LocalPlayer->RaiseInputEvents(movingLeft, movingRight, jumpPressed);

	// Replay controls are raised as EVENTS (never calling the recorder directly); the
	// ReplayController / ReplayRecorder react when they are dispatched.
	if (input.IsKeyJustPressed(ReplayStartKey))
	{
		m_Replay->RequestStartRecording();
	}
	if (input.IsKeyJustPressed(ReplayStopKey))
	{
		m_Replay->RequestStopRecording();
	}
	if (input.IsKeyJustPressed(ReplayPlayKey))
	{
		m_Replay->RequestPlayback();
	}
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

void Game::UpdateScrolling()
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalPlayerObjectId);
	Engine::GameObject& boundary = *m_Objects.Find(m_BoundaryObjectId);

	if (!Engine::IsColliding(player, boundary))
	{
		return;
	}

	m_LocalPlayer->SetCameraOffsetX(
	    NextCameraOffsetX(m_LocalPlayer->CameraOffsetX(), player.GetVelocity().X, ViewWidth));
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

	// Milestone 4 Checkpoint 3: received peer event notifications. Drained here on the main
	// thread (PeerClient's worker only queues them) and raised into m_Events, which stamps
	// them with THIS client's logical time; they are dispatched at the start of the next
	// Update(). Remote objects used to resolve them are the ones applied by earlier frames
	// below, so a notification that beats its sender's first state is simply ignored.
	if (m_PeerClient)
	{
		for (const Engine::NetworkEvent& networkEvent : m_PeerClient->PollEvents())
		{
			m_PeerEvents->HandleNetworkEvent(networkEvent);
		}
	}
	m_PeerEvents->PruneMarkers();

	// The local player's network-relevant state, extracted from its GameObject through
	// the explicit Milestone 3 boundary (see NetworkState.h) rather than built from a
	// second, independent player-state representation.
	Engine::PlayerState localState = ToPlayerState(player);
	if (m_Replay->IsPlaying())
	{
		// The player GameObject currently shows the recording. Peers must never see that as
		// movement, so keep publishing where the real player stood when playback began.
		const GameplaySample& live = m_Replay->SavedLiveSample();
		localState.PositionX = live.PlayerPosition.X;
		localState.PositionY = live.PlayerPosition.Y;
		localState.VelocityX = live.PlayerVelocity.X;
		localState.VelocityY = live.PlayerVelocity.Y;
	}

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

void Game::SubscribeEventLogging()
{
	// One concise line per event, so a manual run can show all four required event types
	// firing. Input and Collision are transition-only (not per-frame), and Death/Spawn are
	// rare, so this never floods the console. Gameplay does not depend on any of it.
	auto logEvent = [this](const Engine::Event& event)
	{
		// Events dispatched while a replay plays are the recording being re-run.
		std::printf("[Event t=%.2f%s] ", event.Timestamp, (m_Replay && m_Replay->IsPlaying()) ? " replay" : "");
		switch (event.Type)
		{
		case Engine::EventType::Input:
		{
			const auto& input = std::get<Engine::InputEventData>(event.Payload);
			const char* action = input.Action == Engine::InputAction::MoveLeft    ? "MoveLeft"
			                     : input.Action == Engine::InputAction::MoveRight ? "MoveRight"
			                                                                      : "Jump";
			std::printf("Input %s %s\n", action, input.Pressed ? "pressed" : "released");
			break;
		}
		case Engine::EventType::Collision:
		{
			const auto& collision = std::get<Engine::CollisionEventData>(event.Payload);
			std::printf("Collision object %u with object %u\n", collision.A, collision.B);
			break;
		}
		case Engine::EventType::Death:
		{
			const auto& death = std::get<Engine::DeathEventData>(event.Payload);
			std::printf("Death object %u (cause object %u)\n", death.Subject, death.Cause);
			break;
		}
		case Engine::EventType::Spawn:
		{
			const auto& spawn = std::get<Engine::SpawnEventData>(event.Payload);
			std::printf("Spawn object %u at (%.0f, %.0f)\n", spawn.Subject, spawn.Position.X, spawn.Position.Y);
			break;
		}
		case Engine::EventType::ReplayStart:
			std::printf("ReplayStart\n");
			break;
		case Engine::EventType::ReplayStop:
			std::printf("ReplayStop\n");
			break;
		case Engine::EventType::ReplayPlay:
			std::printf("ReplayPlay\n");
			break;
		}
		std::fflush(stdout);
	};

	for (Engine::EventType type : { Engine::EventType::Input, Engine::EventType::Collision,
	                                 Engine::EventType::Death, Engine::EventType::Spawn,
	                                 Engine::EventType::ReplayStart, Engine::EventType::ReplayStop,
	                                 Engine::EventType::ReplayPlay })
	{
		m_EventLogSubscriptions.emplace_back(m_Events, type, logEvent);
	}
}

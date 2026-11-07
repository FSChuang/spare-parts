#include "Game.h"

#include "Engine/Input/InputManager.h"
#include "Engine/Renderer/Renderer.h"

#include "ArenaMap.h"
#include "ArenaRules.h"
#include "GameRules.h"
#include "NetworkRate.h"
#include "NetworkState.h"
#include "VisualRules.h"

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
	// A ship (local or remote) is a square object whose inscribed circle is its collision shape.
	constexpr Engine::Vector2 ShipSize{ 2.0f * Astro::ShipRadius, 2.0f * Astro::ShipRadius };

	// An invulnerable ship blinks: it is drawn for half of every period and skipped for the other half.
	constexpr double BlinkPeriodSeconds = 0.2;

	// The playfield floor and its faint grid (drawn over the star background, under the walls).
	constexpr Engine::Color FloorColor{ 10, 14, 38, 205 };
	constexpr Engine::Color GridColor{ 70, 100, 170, 34 };
	constexpr float GridSpacing = 120.0f;
	constexpr float GridLineWidth = 2.0f;

	// Wall and obstacle artwork: a 32x32 tile repeated over each solid, plus a bright rim so the
	// edge of every solid reads clearly. Walls and obstacles get different rim colors.
	constexpr float WallTilePixels = 32.0f;
	constexpr float RimWidth = 3.0f;
	constexpr Engine::Color WallRimColor{ 80, 210, 255, 255 };
	constexpr Engine::Color ObstacleRimColor{ 255, 190, 80, 255 };

	// The star background is a tile of this size, repeated across the whole screen.
	constexpr float StarTilePixels = 512.0f;

	// Below this squared speed a remote ship is considered stopped, so noise cannot spin its heading.
	constexpr float MinimumHeadingSpeedSquared = 1.0f;

	// Keys. Rotation and fire map onto the Engine's game-neutral input actions.
	constexpr SDL_Scancode FireKey = SDL_SCANCODE_SPACE;

	// Replay controls. F-keys, so they never clash with rotate/fire or the engine's own Tab / P /
	// 1-2-3 debug keys. (On a Mac laptop hold fn if the OS claims them.)
	constexpr SDL_Scancode ReplayStartKey = SDL_SCANCODE_F5;
	constexpr SDL_Scancode ReplayStopKey = SDL_SCANCODE_F6;
	constexpr SDL_Scancode ReplayPlayKey = SDL_SCANCODE_F7;

	// Local-player sentinel network id before NetworkClient's handshake has assigned a
	// real one (Protocol.h: PlayerId 0 is reserved "unassigned"). The local ship always has a
	// network-id property, so ToPlayerState() never needs a special case for "not connected yet".
	constexpr std::uint32_t UnassignedNetworkId = 0;
}

Game::Game(const std::string& serverEndpoint, std::function<double()> timeSource, const GameAssets& assets)
	: m_Events(timeSource),
	  m_TimeSource(std::move(timeSource)),
	  m_Assets(assets),
	  m_Physics(0.0f),
	  m_Network(serverEndpoint),
	  // Set in the past by exactly one interval, so the very first Update() call
	  // triggers an immediate server refresh rather than waiting a full interval
	  // first — steady_clock's own epoch is unspecified, so this is computed relative
	  // to "now" rather than assumed to already be far enough in the past.
	  m_LastServerRefreshTime(std::chrono::steady_clock::now() - ServerRefreshInterval),
	  m_Bullets(m_Objects, m_Physics, BulletConfig{ Astro::BulletSpeed, Astro::BulletLifetime, Astro::BulletRadius })
{
	CreateArenaSolids();

	Engine::GameObject& ship = m_Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
	SetCenter(ship, SelectSpawnCentre());
	ship.SetVelocity({ 0.0f, 0.0f });
	ship.SetNetworkId(UnassignedNetworkId);
	m_LocalShipObjectId = ship.GetId();

	SubscribeEventLogging();
	SubscribeExplosions();

	m_Ship = std::make_unique<ShipController>(
	    m_Events, m_Objects, m_Bullets, m_LocalShipObjectId,
	    ShipConfig{ Astro::ShipSpeed, Astro::ShipRotationSpeed, Astro::ShipRadius, Astro::MuzzleDistance,
	                Astro::FireCooldown, Astro::RespawnDelay, Astro::InvulnerabilityDuration },
	    [this] { return SelectSpawnCentre(); }, m_TimeSource);

	// A ship starts dead. The initial spawn goes through the event system too: this Spawn event,
	// handled in the first Update()'s input phase, is what formally brings the ship to life.
	m_Ship->RequestSpawn(SelectSpawnCentre());

	// The local ship's own Death, Spawn and Shoot are announced to peers through m_PeerClient,
	// which only exists once the server handshake has completed — before that there is nobody to
	// tell. (PublishEvent just queues; the PeerClient worker does the sending.)
	m_PeerEvents = std::make_unique<PeerEventBridge>(
	    m_Events, m_Objects, m_Bullets, m_LocalShipObjectId,
	    [this](const Engine::NetworkEvent& event)
	    {
		    if (m_PeerClient)
		    {
			    m_PeerClient->PublishEvent(event);
		    }
	    });

	// Every bullet the local ship really fires is announced to peers (never a blocked shot).
	m_Ship->SetShotListener([this](Engine::Vector2 origin, float heading) { m_PeerEvents->AnnounceShot(origin, heading); });

	m_Replay = std::make_unique<ReplayController>(m_Events, m_Objects, m_Bullets, *m_Ship, *m_PeerEvents,
	                                              m_LocalShipObjectId, m_TimeSource);
}

void Game::CreateArenaSolids()
{
	// Every wall and obstacle becomes a static Boundary object, so touching one is an ordinary
	// Collision event naming it, and BulletField can find them. No render property: they are
	// drawn from the map data (DrawMap), not through DrawObject.
	for (const std::vector<SolidRect>* group : { &ArenaWalls(), &ArenaObstacles() })
	{
		for (const SolidRect& rect : *group)
		{
			m_Objects.Create(Engine::ObjectType::Boundary, rect.Position, rect.Size);
		}
	}
}

Engine::Vector2 Game::SelectSpawnCentre() const
{
	// Before a PlayerId is known (the unassigned sentinel 0), SelectSpawnIndex naturally
	// resolves to the first spawn point.
	Engine::PlayerId localId = m_Network.GetLocalPlayerId();
	return ArenaSpawnCentre(SelectSpawnIndex(localId, ArenaSpawnCount));
}

void Game::Update(Engine::InputManager& input, float deltaTime)
{
	// Frame order, with two deliberate event-dispatch phases:
	//
	//  1. BeginFrame        allow a new death this frame
	//  2. PollInputEvents   held rotate-key CHANGES and fresh fire presses become Input events;
	//                       F5/F6/F7 raise the replay control events
	//  3. DispatchDue #1    replay control takes effect; input takes effect BEFORE movement
	//                       (rotation intent, a shot is fired); the initial Spawn, and a respawn
	//                       whose delay has elapsed, are delivered here too; while a replay plays,
	//                       its re-scheduled events are delivered here (observed only)
	//
	//  LIVE (not replaying):
	//  4. ApplySteering     rotate by the intent, set the forward velocity (zero if dead)
	//  5. physics           integrate the ship's velocity (zero gravity)
	//  6. bullets           move in short steps, age, and are spent on a wall/obstacle or at the
	//                       end of their life
	//  7. hit detection     the ship touching a wall/obstacle, or a bullet overlapping it, raises a
	//                       Collision event (detection kills nothing)
	//  8. DispatchDue #2    the whole chain runs now, in one call: Collision -> Death. The Spawn it
	//                       schedules is `RespawnDelay` in the future, so it is NOT due here. A death
	//                       never waits for the next frame, and networking reads the final state
	//  (While a replay PLAYS, 4-8 are skipped: the recorded frames own the ship and its bullets,
	//   and the live BulletField is not touched at all — it is frozen exactly as it was.)
	//
	//  9. networking        publishes this frame's final ship state (the saved live state, not the
	//                       replay, while one plays); remote players stay live
	// 10. UpdatePlayback    shows the recorded frame for the elapsed playback time, and ends
	//                       playback (restoring the live ship) when the span is over
	// 11. CaptureFrame      while recording, stores this frame's final state
	//
	// A frame with deltaTime == 0 (the Timeline reads whole milliseconds) changes no heading and
	// moves nothing, but still delivers any due events.
	m_Ship->BeginFrame();

	PollInputEvents(input);
	m_Events.DispatchDue();

	if (!m_Replay->IsPlaying())
	{
		m_Ship->ApplySteering(deltaTime);
		m_Physics.Update(*m_Objects.Find(m_LocalShipObjectId), deltaTime);

		m_Bullets.Update(deltaTime);
		m_Ship->DetectSolidHits();
		m_Ship->DetectBulletHits();
		m_Events.DispatchDue();
	}

	// Networking runs after the local simulation so it always publishes this frame's final
	// state — never a stale pre-respawn position.
	UpdateNetworking(deltaTime);

	// Forget explosions that have finished playing.
	const double now = m_TimeSource();
	m_Explosions.erase(std::remove_if(m_Explosions.begin(), m_Explosions.end(),
	                                  [now](const Explosion& explosion) { return ExplosionFrameAt(now - explosion.StartTime) < 0 && now >= explosion.StartTime; }),
	                   m_Explosions.end());

	m_Replay->UpdatePlayback();
	m_Replay->CaptureFrame();
}

void Game::Render(Engine::Renderer& renderer)
{
	// Painter's order, back to front (no layers: each call just draws over what came before):
	//   1. stars         the space background, tiled over the whole screen
	//   2. floor + grid  the playfield inside the walls
	//   3. map           walls, then obstacles
	//   4. bullets       live ones, or — while a replay plays — the replay's recorded ones
	//   5. ships         remote ships, then the local one on top (green ghost during a replay)
	//   6. explosions    over everything, where ships died
	const double now = m_TimeSource();
	const bool replaying = m_Replay->IsPlaying();

	DrawBackground(renderer);
	DrawMap(renderer);

	const float bulletSize = Astro::BulletSpriteSize;
	auto drawBullet = [&](Engine::Vector2 centre)
	{
		renderer.DrawSprite(m_Assets.Bullet, { centre.X - bulletSize / 2.0f, centre.Y - bulletSize / 2.0f },
		                    { bulletSize, bulletSize });
	};
	if (replaying)
	{
		// The recorded bullets: bare positions from the sample, never live objects. (The live
		// bullets are frozen and not drawn.)
		for (const ReplayBullet& bullet : m_Replay->CurrentSample().Bullets)
		{
			drawBullet(bullet.Centre);
		}
	}
	else
	{
		for (const Bullet& bullet : m_Bullets.All())
		{
			if (const Engine::GameObject* object = m_Objects.Find(bullet.Id))
			{
				drawBullet(CenterOf(*object));
			}
		}
	}

	for (auto& [id, object] : m_Objects)
	{
		if (id == m_LocalShipObjectId || !object.HasNetworkId() || !m_PeerEvents->IsRemoteAlive(id))
		{
			continue; // not a remote ship, or one that is dead (hidden until its Spawn arrives)
		}

		auto heading = m_RemoteHeadings.find(id);
		DrawShip(renderer, m_Assets.Ships[ShipVariantFor(object.GetNetworkId())], CenterOf(object),
		         heading != m_RemoteHeadings.end() ? heading->second : 0.0f);
	}

	if (m_Ship->IsAlive()) // a dead ship is not drawn (while replaying: the recorded ship is dead)
	{
		const Engine::GameObject& local = *m_Objects.Find(m_LocalShipObjectId);
		bool invulnerable = replaying ? m_Replay->CurrentSample().Invulnerable : m_Ship->IsInvulnerable();
		bool blinkedOut = invulnerable && std::fmod(now, BlinkPeriodSeconds) >= BlinkPeriodSeconds / 2.0;
		if (!blinkedOut)
		{
			Engine::TextureId texture =
			    replaying ? m_Assets.ReplayShip : m_Assets.Ships[ShipVariantFor(local.GetNetworkId())];
			DrawShip(renderer, texture, CenterOf(local), m_Ship->Heading());
		}
	}

	DrawExplosions(renderer, now);
}

void Game::DrawShip(Engine::Renderer& renderer, Engine::TextureId texture, Engine::Vector2 centre, float heading) const
{
	// Gameplay heading is untouched; only the drawing adds the artwork's quarter-turn.
	const float size = Astro::ShipSpriteSize;
	renderer.DrawSprite(texture, { centre.X - size / 2.0f, centre.Y - size / 2.0f }, { size, size },
	                    ShipSpriteRotation(heading));
}

void Game::DrawBackground(Engine::Renderer& renderer) const
{
	for (float y = 0.0f; y < Astro::ArenaHeight; y += StarTilePixels)
	{
		for (float x = 0.0f; x < Astro::ArenaWidth; x += StarTilePixels)
		{
			renderer.DrawSprite(m_Assets.Stars, { x, y }, { StarTilePixels, StarTilePixels });
		}
	}

	auto fill = [&renderer](Engine::Vector2 position, Engine::Vector2 size, Engine::Color color)
	{
		Engine::GameObject rect(0, Engine::ObjectType::Generic, position, size);
		rect.SetRender(color);
		renderer.DrawObject(rect);
	};

	const SolidRect floor = ArenaPlayfield();
	fill(floor.Position, floor.Size, FloorColor);
	for (float x = floor.Position.X + GridSpacing; x < floor.Position.X + floor.Size.X; x += GridSpacing)
	{
		fill({ x - GridLineWidth / 2.0f, floor.Position.Y }, { GridLineWidth, floor.Size.Y }, GridColor);
	}
	for (float y = floor.Position.Y + GridSpacing; y < floor.Position.Y + floor.Size.Y; y += GridSpacing)
	{
		fill({ floor.Position.X, y - GridLineWidth / 2.0f }, { floor.Size.X, GridLineWidth }, GridColor);
	}
}

void Game::DrawMap(Engine::Renderer& renderer) const
{
	auto drawSolid = [&](const SolidRect& rect, Engine::Color rim)
	{
		// The tile, repeated; the last tile in a row/column is cropped to fit (the tile is drawn 1:1,
		// so a crop of w x h screen units is a crop of w x h texture pixels).
		for (float y = 0.0f; y < rect.Size.Y; y += WallTilePixels)
		{
			for (float x = 0.0f; x < rect.Size.X; x += WallTilePixels)
			{
				float w = std::fmin(WallTilePixels, rect.Size.X - x);
				float h = std::fmin(WallTilePixels, rect.Size.Y - y);
				renderer.DrawSprite(m_Assets.Wall, { rect.Position.X + x, rect.Position.Y + y }, { w, h }, 0.0f,
				                    Engine::SourceRect{ 0.0f, 0.0f, w, h });
			}
		}

		// The bright rim, on the outside edge of the collision rectangle.
		auto edge = [&](Engine::Vector2 position, Engine::Vector2 size)
		{
			Engine::GameObject bar(0, Engine::ObjectType::Generic, position, size);
			bar.SetRender(rim);
			renderer.DrawObject(bar);
		};
		edge(rect.Position, { rect.Size.X, RimWidth });
		edge({ rect.Position.X, rect.Position.Y + rect.Size.Y - RimWidth }, { rect.Size.X, RimWidth });
		edge(rect.Position, { RimWidth, rect.Size.Y });
		edge({ rect.Position.X + rect.Size.X - RimWidth, rect.Position.Y }, { RimWidth, rect.Size.Y });
	};

	for (const SolidRect& wall : ArenaWalls())
	{
		drawSolid(wall, WallRimColor);
	}
	for (const SolidRect& obstacle : ArenaObstacles())
	{
		drawSolid(obstacle, ObstacleRimColor);
	}
}

void Game::DrawExplosions(Engine::Renderer& renderer, double now) const
{
	const float size = Astro::ExplosionDrawSize;
	for (const Explosion& explosion : m_Explosions)
	{
		int frame = ExplosionFrameAt(now - explosion.StartTime);
		if (frame < 0)
		{
			continue;
		}

		const float pixels = Astro::ExplosionFramePixels;
		renderer.DrawSprite(m_Assets.Explosion, { explosion.Centre.X - size / 2.0f, explosion.Centre.Y - size / 2.0f },
		                    { size, size }, 0.0f,
		                    Engine::SourceRect{ pixels * static_cast<float>(frame), 0.0f, pixels, pixels });
	}
}

void Game::SubscribeExplosions()
{
	// A ship that dies — the local one, a remote one, or the local one in a replay — explodes where
	// it was. The Death event is the one signal for all three (the bridge raises it for remote
	// players too). Presentation only: nothing here touches gameplay.
	m_EventLogSubscriptions.emplace_back(
	    m_Events, Engine::EventType::Death,
	    [this](const Engine::Event& event)
	    {
		    const auto& death = std::get<Engine::DeathEventData>(event.Payload);
		    const Engine::GameObject* subject = m_Objects.Find(death.Subject);
		    if (subject != nullptr && subject->GetType() == Engine::ObjectType::Player)
		    {
			    m_Explosions.push_back({ CenterOf(*subject), event.Timestamp });
		    }
	    });
}

void Game::PollInputEvents(Engine::InputManager& input)
{
	bool rotatingLeft = input.IsKeyPressed(SDL_SCANCODE_A) || input.IsKeyPressed(SDL_SCANCODE_LEFT);
	bool rotatingRight = input.IsKeyPressed(SDL_SCANCODE_D) || input.IsKeyPressed(SDL_SCANCODE_RIGHT);

	// Fire is edge-triggered (IsKeyJustPressed, not IsKeyPressed): holding the key never
	// queues repeated shots. Whether a shot actually happens (alive, cooldown) is decided by
	// the ActionPrimary event's handler.
	bool firePressed = input.IsKeyJustPressed(FireKey);

	m_Ship->RaiseInputEvents(rotatingLeft, rotatingRight, firePressed);

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

void Game::UpdateNetworking(float deltaTime)
{
	Engine::GameObject& player = *m_Objects.Find(m_LocalShipObjectId);

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

			// The first spawn (at construction) could not know this player's id, so every client
			// began at the same point. Now that it is known, spawn once more at the point this
			// id owns, so two players never start on top of each other. (A Spawn event, like any
			// other; the ship simply starts its invulnerability window again.)
			m_Ship->RequestSpawn(SelectSpawnCentre());
		}
	}

	// Received peer event notifications, drained here on the main thread (PeerClient's worker only
	// queues them, in arrival order). A Shoot becomes a bullet in m_Bullets right away; a Death or
	// Spawn is raised into m_Events, which stamps it with THIS client's logical time, and is
	// dispatched at the start of the next Update(). Remote objects used to resolve a Death/Spawn
	// are the ones applied by earlier frames below, so one that beats its sender's first state is
	// simply ignored.
	if (m_PeerClient)
	{
		for (const Engine::NetworkEvent& networkEvent : m_PeerClient->PollEvents())
		{
			m_PeerEvents->HandleNetworkEvent(networkEvent);
		}
	}
	m_PeerEvents->PruneRemoved();

	// The local player's network-relevant state, extracted from its GameObject through
	// the explicit Milestone 3 boundary (see NetworkState.h) rather than built from a
	// second, independent player-state representation.
	Engine::PlayerState localState = ToPlayerState(player);
	if (m_Replay->IsPlaying())
	{
		// The ship object currently shows the recording. Peers must never see that as movement,
		// so keep publishing where the real ship stood when playback began.
		const ShipSnapshot& live = m_Replay->SavedLiveShip();
		localState.PositionX = live.Position.X;
		localState.PositionY = live.Position.Y;
		localState.VelocityX = live.Velocity.X;
		localState.VelocityY = live.Velocity.Y;
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
		if (id == m_LocalShipObjectId || !object.HasNetworkId())
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
		m_RemoteHeadings.erase(id);
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
			    m_Objects.Create(Engine::ObjectType::Player, { state.PositionX, state.PositionY }, ShipSize);
			created.SetVelocity({ state.VelocityX, state.VelocityY });
			created.SetNetworkId(id);
			remotePlayer = &created;
		}
		else
		{
			ApplyPlayerState(*remotePlayer, state);
		}

		// A remote ship's heading is not on the wire: it flies straight ahead at constant
		// speed, so its velocity direction IS its heading. A (nearly) stopped ship keeps the
		// last heading it had.
		if (state.VelocityX * state.VelocityX + state.VelocityY * state.VelocityY > MinimumHeadingSpeedSquared)
		{
			m_RemoteHeadings[remotePlayer->GetId()] = std::atan2(state.VelocityY, state.VelocityX);
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
			                                                                      : "ActionPrimary";
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

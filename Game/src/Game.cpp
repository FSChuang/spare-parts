#include "Game.h"

#include "Engine/Input/InputManager.h"
#include "Engine/Renderer/Renderer.h"

#include "ArenaRules.h"
#include "GameRules.h"
#include "NetworkRate.h"
#include "NetworkState.h"

#include <SDL3/SDL_scancode.h>

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

	constexpr Engine::Color LocalShipColor{ 0, 170, 255, 255 };
	constexpr Engine::Color RemoteShipColor{ 255, 150, 40, 255 };
	// A remote ship is drawn in this color while its "recently died" marker is active
	// (Milestone 4 networked Death event) — purely presentational, never networked.
	constexpr Engine::Color RemoteShipDeadColor{ 255, 255, 255, 255 };
	constexpr Engine::Color NoseColor{ 255, 255, 255, 255 };

	// The nose marker: a small square centred on the ship's rim, so rotation is obvious.
	constexpr float NoseSize = 14.0f;

	// An invulnerable ship blinks: alternating full and dim alpha.
	constexpr double BlinkPeriodSeconds = 0.2;
	constexpr std::uint8_t BlinkDimAlpha = 70;

	// Below this squared speed a remote ship is considered stopped, so noise cannot spin its heading.
	constexpr float MinimumHeadingSpeedSquared = 1.0f;

	// Keys. Rotation and fire map onto the Engine's game-neutral input actions.
	constexpr SDL_Scancode FireKey = SDL_SCANCODE_SPACE;
	// Temporary dev aid until remote players can shoot: drops a bullet nobody owns on the ship.
	constexpr SDL_Scancode DebugHitKey = SDL_SCANCODE_K;

	// Local-player sentinel network id before NetworkClient's handshake has assigned a
	// real one (Protocol.h: PlayerId 0 is reserved "unassigned"). The local ship always has a
	// network-id property, so ToPlayerState() never needs a special case for "not connected yet".
	constexpr std::uint32_t UnassignedNetworkId = 0;
}

Game::Game(const std::string& serverEndpoint, std::function<double()> timeSource)
	: m_Events(timeSource),
	  m_TimeSource(std::move(timeSource)),
	  m_Physics(0.0f),
	  m_Network(serverEndpoint),
	  // Set in the past by exactly one interval, so the very first Update() call
	  // triggers an immediate server refresh rather than waiting a full interval
	  // first — steady_clock's own epoch is unspecified, so this is computed relative
	  // to "now" rather than assumed to already be far enough in the past.
	  m_LastServerRefreshTime(std::chrono::steady_clock::now() - ServerRefreshInterval),
	  m_Bullets(m_Objects, m_Physics, BulletConfig{ Astro::BulletSpeed, Astro::BulletLifetime, Astro::BulletRadius })
{
	Engine::GameObject& ship = m_Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
	SetCenter(ship, SelectSpawnCentre());
	ship.SetRender(LocalShipColor);
	ship.SetVelocity({ 0.0f, 0.0f });
	ship.SetNetworkId(UnassignedNetworkId);
	m_LocalShipObjectId = ship.GetId();

	SubscribeEventLogging();

	m_Ship = std::make_unique<ShipController>(
	    m_Events, m_Objects, m_Bullets, m_LocalShipObjectId,
	    ShipConfig{ Astro::ShipSpeed, Astro::ShipRotationSpeed, Astro::ShipRadius, Astro::MuzzleDistance,
	                Astro::FireCooldown, Astro::RespawnDelay, Astro::InvulnerabilityDuration },
	    [this] { return SelectSpawnCentre(); }, m_TimeSource);

	// A ship starts dead. The initial spawn goes through the event system too: this Spawn event,
	// handled in the first Update()'s input phase, is what formally brings the ship to life.
	m_Ship->RequestSpawn(SelectSpawnCentre());

	// The local ship's own Death events are announced to peers through m_PeerClient, which
	// only exists once the server handshake has completed — before that there is nobody to
	// tell. (PublishEvent just queues; the PeerClient worker does the sending.)
	m_PeerEvents = std::make_unique<PeerEventBridge>(
	    m_Events, m_Objects, m_LocalShipObjectId, m_TimeSource,
	    [this](Engine::NetworkEventKind kind)
	    {
		    if (m_PeerClient)
		    {
			    m_PeerClient->PublishEvent(kind);
		    }
	    });
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
	//  2. PollInputEvents   held rotate-key CHANGES and fresh fire presses become Input events
	//  3. DispatchDue #1    input takes effect BEFORE movement (rotation intent, a shot is
	//                       fired); the initial Spawn, and a respawn whose delay has elapsed,
	//                       are delivered here too — so a ship that just came back moves this frame
	//  4. ApplySteering     rotate by the intent, set the forward velocity (zero if dead)
	//  5. physics           integrate the ship's velocity (zero gravity)
	//  6. wrap              a ship that fully left the arena reappears on the opposite side
	//  7. bullets           move, age, expire (no wrap)
	//  8. hit detection     a bullet overlapping the ship raises a Collision event (kills nothing)
	//  9. DispatchDue #2    the whole chain runs now, in one call: Collision -> Death. The Spawn it
	//                       schedules is `RespawnDelay` in the future, so it is NOT due here. A death
	//                       never waits for the next frame, and networking reads the final state
	// 10. networking        publishes this frame's final ship state
	//
	// A frame with deltaTime == 0 (the Timeline reads whole milliseconds) changes no heading and
	// moves nothing, but still delivers any due events.
	m_Ship->BeginFrame();

	PollInputEvents(input);
	m_Events.DispatchDue();

	m_Ship->ApplySteering(deltaTime);
	m_Physics.Update(*m_Objects.Find(m_LocalShipObjectId), deltaTime);
	m_Ship->WrapAroundArena();

	m_Bullets.Update(deltaTime);
	m_Ship->DetectBulletHits();
	m_Events.DispatchDue();

	// Networking runs after the local simulation so it always publishes this frame's final
	// state — never a stale pre-respawn position.
	UpdateNetworking(deltaTime);
}

void Game::Render(Engine::Renderer& renderer)
{
	const double now = m_TimeSource();

	for (auto& [id, object] : m_Objects)
	{
		if (id == m_LocalShipObjectId)
		{
			if (!m_Ship->IsAlive())
			{
				continue; // a dead ship is not drawn
			}

			Engine::Color color = LocalShipColor;
			if (m_Ship->IsInvulnerable() && std::fmod(now, BlinkPeriodSeconds) < BlinkPeriodSeconds / 2.0)
			{
				color.A = BlinkDimAlpha;
			}
			DrawShip(renderer, CenterOf(object), m_Ship->Heading(), color);
		}
		else if (object.HasNetworkId())
		{
			auto heading = m_RemoteHeadings.find(id);
			Engine::Color color = m_PeerEvents->IsMarkedDead(id) ? RemoteShipDeadColor : RemoteShipColor;
			DrawShip(renderer, CenterOf(object), heading != m_RemoteHeadings.end() ? heading->second : 0.0f, color);
		}
		else
		{
			renderer.DrawObject(object); // a bullet
		}
	}
}

void Game::DrawShip(Engine::Renderer& renderer, Engine::Vector2 centre, float heading, Engine::Color bodyColor) const
{
	Engine::GameObject body(0, Engine::ObjectType::Generic, { 0.0f, 0.0f }, ShipSize);
	SetCenter(body, centre);
	body.SetRender(bodyColor);
	renderer.DrawObject(body);

	// The nose sits on the rim in the heading direction.
	Engine::Vector2 forward = ForwardOf(heading);
	Engine::GameObject nose(0, Engine::ObjectType::Generic, { 0.0f, 0.0f }, { NoseSize, NoseSize });
	SetCenter(nose, { centre.X + forward.X * Astro::ShipRadius, centre.Y + forward.Y * Astro::ShipRadius });
	Engine::Color noseColor = NoseColor;
	noseColor.A = bodyColor.A;
	nose.SetRender(noseColor);
	renderer.DrawObject(nose);
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

	if (input.IsKeyJustPressed(DebugHitKey))
	{
		m_Bullets.Fire(CenterOf(*m_Objects.Find(m_LocalShipObjectId)), 0.0f, Engine::NoObjectId);
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
			created.SetRender(RemoteShipColor);
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
	auto logEvent = [](const Engine::Event& event)
	{
		std::printf("[Event t=%.2f] ", event.Timestamp);
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
		default:
			break; // replay control events are not subscribed to here
		}
		std::fflush(stdout);
	};

	for (Engine::EventType type : { Engine::EventType::Input, Engine::EventType::Collision,
	                                 Engine::EventType::Death, Engine::EventType::Spawn })
	{
		m_EventLogSubscriptions.emplace_back(m_Events, type, logEvent);
	}
}

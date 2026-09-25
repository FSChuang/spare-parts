// Spare Parts headless multiplayer server (Milestone 2 Section 4 checkpoint 2). No SDL
// window, no Renderer, no Application, no Entity — this executable links only
// EngineNetwork (+ Threads for its session worker threads), so it is provably SDL-free
// at the build-graph level, not just by convention.
//
// Topology: one bootstrap REP socket (JOIN only) hands each joining client a dedicated
// REP port from a fixed pool, backed by its own session thread. This is what makes one
// stalled/blocked client structurally unable to affect any other client or new JOINs —
// each session thread's blocking Receive() is entirely its own.
//
// PlatformState (Milestone 2 Section 4 final checkpoint): every successful Snapshot now
// carries the real, server-authoritative moving platform, computed by the pure,
// stateless ComputePlatformState() (see ServerPlatform.h/.cpp) from `serverStartTime`
// (established once here, in main(), before the bootstrap loop starts) and the current
// real time. No platform thread, no platform mutex, no mutable shared platform state —
// every session thread computes the same trajectory independently, and it never depends
// on any client's Timeline.
//
// KNOWN LIMITATION (accepted for this checkpoint): if a client vanishes without ever
// sending Leaving=true, its session thread stays blocked in Receive() forever, and its
// PlayerRegistry entry + dedicated port + registry slot remain permanently occupied.
// No heartbeat, receive timeout, lease, or crash detector is implemented to bound this.

#include "Engine/Network/PlayerRegistry.h"
#include "Engine/Network/ServerDispatch.h"
#include "Engine/Network/Socket.h"

#include "ServerPlatform.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr const char* DefaultBootstrapEndpoint = "tcp://*:5556";
	constexpr std::uint16_t FirstSessionPort = 5557;
	// Fixed pool: 5557..5564 inclusive, exactly Engine::MaxPlayers ports. Bootstrap is
	// the only thread that ever touches this — no mutex needed for it specifically.
	std::array<bool, Engine::MaxPlayers> g_PortInUse{};

	std::string ToFrame(const std::vector<std::uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}

	// Lowest-numbered free port first, so a just-released port is the next one handed
	// out again — deterministic, not merely "some free port".
	std::optional<std::uint16_t> AllocatePort()
	{
		for (std::size_t i = 0; i < g_PortInUse.size(); ++i)
		{
			if (!g_PortInUse[i])
			{
				g_PortInUse[i] = true;
				return static_cast<std::uint16_t>(FirstSessionPort + i);
			}
		}
		return std::nullopt;
	}

	void ReleasePort(std::uint16_t port)
	{
		std::size_t index = static_cast<std::size_t>(port - FirstSessionPort);
		g_PortInUse[index] = false;
	}

	// Overwrites a Snapshot reply's Platform field with the real, current
	// server-authoritative value. Engine::HandleSessionRequest deliberately knows
	// nothing about real time or spare-parts world constants (CLAUDE.md: the engine/game
	// boundary is sacred) — it always encodes a neutral placeholder, so this is the
	// smallest possible spare-parts-side fix-up: decode, overwrite one field, re-encode.
	// Error replies carry no Platform field at all, so they pass through unchanged.
	std::vector<std::uint8_t> InjectCurrentPlatformState(const std::vector<std::uint8_t>& replyBytes,
	                                                       std::chrono::steady_clock::time_point serverStartTime)
	{
		if (Engine::PeekMessageType(replyBytes) != Engine::MessageType::Snapshot)
		{
			return replyBytes;
		}

		std::optional<Engine::Snapshot> snapshot = Engine::DecodeSnapshot(replyBytes);
		if (!snapshot.has_value())
		{
			// Defensive only: HandleSessionRequest's own Snapshot encoding is always
			// well-formed, so this branch is not expected to be reachable.
			return replyBytes;
		}

		snapshot->Platform = ComputePlatformState(serverStartTime, std::chrono::steady_clock::now());

		// The roster that produced `replyBytes` already fit within MaxPlayers (it came
		// from HandleSessionRequest's own successful encode), and only the Platform
		// field changed, so this re-encode cannot newly exceed that bound.
		return *Engine::EncodeSnapshot(*snapshot);
	}

	// Bootstrap/control-side owner of one dedicated session's resources. Bootstrap owns
	// every SessionRecord for the server's entire lifetime; a session thread may only
	// update `Finished` (via the shared_ptr, so it survives independently of this
	// struct possibly relocating in a std::vector) — it must never touch its own
	// std::thread or erase its own record.
	struct SessionRecord
	{
		std::thread Worker;
		std::shared_ptr<std::atomic<bool>> Finished;
		std::uint16_t Port;
		Engine::PlayerId PlayerId;
	};

	// Runs entirely on one dedicated session thread for one already-assigned player.
	// Binds its own REP socket, reports bind success/failure exactly once via
	// `readyPromise` (bootstrap blocks on the paired future before replying to the
	// client's JOIN — this is what prevents the JoinAccepted-before-bind race, with no
	// arbitrary sleep), then loops handling only StateUpdate traffic for `playerId`
	// until it observes a genuine, correctly-identified Leaving=true.
	void SessionMain(std::uint16_t port, Engine::PlayerId playerId, Engine::PlayerRegistry& registry,
	                  std::mutex& registryMutex, std::promise<bool> readyPromise,
	                  std::shared_ptr<std::atomic<bool>> finished,
	                  std::chrono::steady_clock::time_point serverStartTime)
	{
		std::string endpoint = "tcp://*:" + std::to_string(port);
		std::optional<Engine::Socket> socket;

		try
		{
			socket.emplace(Engine::SocketRole::Reply);
			socket->Bind(endpoint);
		}
		catch (...)
		{
			socket.reset();
			readyPromise.set_value(false);
			finished->store(true);
			return;
		}

		readyPromise.set_value(true);

		for (;;)
		{
			std::string requestFrame = socket->Receive();
			std::vector<std::uint8_t> requestBytes(requestFrame.begin(), requestFrame.end());

			// Determined from the REQUEST itself, never inferred from the reply we are
			// about to send — a malformed or spoofed "Leaving" attempt must not end the
			// session (see the malformed/wrong-type/spoof requirements).
			std::optional<Engine::StateUpdate> update = Engine::DecodeStateUpdate(requestBytes);
			bool validLeaving = update.has_value() && update->State.Id == playerId && update->Leaving;

			std::vector<std::uint8_t> replyBytes;
			{
				std::lock_guard<std::mutex> lock(registryMutex);
				replyBytes = Engine::HandleSessionRequest(registry, playerId, requestBytes);
			}
			// Registry lock released before Send() — never held across ZeroMQ I/O.

			// Platform injection needs no lock: it only reads local `replyBytes` and the
			// immutable `serverStartTime`, and samples steady_clock — no shared mutable
			// state at all.
			replyBytes = InjectCurrentPlatformState(replyBytes, serverStartTime);

			socket->Send(ToFrame(replyBytes));

			if (validLeaving)
			{
				break;
			}
		}

		// Destroy the socket before signaling completion: a released port must never be
		// reused while this session's REP socket might still be bound to it.
		socket.reset();
		finished->store(true);
	}

	// Bootstrap-thread-only: joins and releases the port of every session that has
	// marked itself finished. Opportunistic — swept once per JOIN received, not on any
	// separate schedule/thread, which is acceptable at this checkpoint's scale. Safe
	// regardless of how long this is deferred: a finished thread stays valid and
	// joinable, and its player was already removed from the registry by its own valid
	// Leaving before it set `Finished`.
	void ReapFinishedSessions(std::vector<SessionRecord>& sessions)
	{
		for (auto it = sessions.begin(); it != sessions.end();)
		{
			if (it->Finished->load())
			{
				it->Worker.join();
				ReleasePort(it->Port);
				it = sessions.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

int main(int argc, char* argv[])
{
	std::string bootstrapEndpoint = (argc > 1) ? argv[1] : DefaultBootstrapEndpoint;

	// Established once, before the bootstrap loop starts; every dedicated session
	// thread derives the server-authoritative platform's position from this same,
	// immutable value — no mutex needed for it, since it is never modified after this
	// line (Milestone 2 Section 4 final checkpoint).
	const auto serverStartTime = std::chrono::steady_clock::now();

	Engine::Socket bootstrap(Engine::SocketRole::Reply);
	bootstrap.Bind(bootstrapEndpoint);

	Engine::PlayerRegistry registry;
	std::mutex registryMutex;
	std::vector<SessionRecord> sessions;

	std::printf("Spare Parts server listening (bootstrap) on %s (Ctrl-C to stop)\n", bootstrapEndpoint.c_str());
	std::fflush(stdout);

	for (;;)
	{
		std::string requestFrame = bootstrap.Receive();
		std::vector<std::uint8_t> requestBytes(requestFrame.begin(), requestFrame.end());

		ReapFinishedSessions(sessions);

		std::optional<Engine::MessageType> type = Engine::PeekMessageType(requestBytes);
		if (type != Engine::MessageType::Join || !Engine::DecodeJoinRequest(requestBytes).has_value())
		{
			// Bootstrap accepts only JOIN; anything else (including routine
			// StateUpdate traffic, which belongs on a dedicated session port) is
			// malformed from bootstrap's point of view.
			bootstrap.Send(ToFrame(Engine::EncodeError(Engine::ErrorCode::MalformedRequest)));
			continue;
		}

		std::optional<std::uint16_t> port = AllocatePort();
		if (!port.has_value())
		{
			bootstrap.Send(ToFrame(Engine::EncodeError(Engine::ErrorCode::RegistryFull)));
			continue;
		}

		std::optional<Engine::PlayerId> playerId;
		{
			std::lock_guard<std::mutex> lock(registryMutex);
			playerId = registry.AssignPlayer();
		}
		if (!playerId.has_value())
		{
			// Rollback: release the port already reserved. In practice unreachable —
			// the port pool and PlayerRegistry are both capped at Engine::MaxPlayers
			// and only ever move together through this exact path — but kept as an
			// explicit, correct rollback rather than assuming that invariant forever.
			ReleasePort(*port);
			bootstrap.Send(ToFrame(Engine::EncodeError(Engine::ErrorCode::RegistryFull)));
			continue;
		}

		std::promise<bool> readyPromise;
		std::future<bool> readyFuture = readyPromise.get_future();
		std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);

		std::thread worker(SessionMain, *port, *playerId, std::ref(registry), std::ref(registryMutex),
		                    std::move(readyPromise), finished, serverStartTime);

		// Blocks until the session reports bind success/failure — no registry lock
		// held here, and no arbitrary sleep: this is the deterministic readiness
		// handoff that prevents sending JoinAccepted before the dedicated socket is
		// actually bound and able to receive.
		bool bindSucceeded = readyFuture.get();

		if (!bindSucceeded)
		{
			// The session already returned once it reported failure, so this join is
			// immediate — not a stall risk. Roll back both allocations cleanly.
			worker.join();
			{
				std::lock_guard<std::mutex> lock(registryMutex);
				registry.RemovePlayer(*playerId);
			}
			ReleasePort(*port);
			bootstrap.Send(ToFrame(Engine::EncodeError(Engine::ErrorCode::RegistryFull)));
			continue;
		}

		sessions.push_back(SessionRecord{ std::move(worker), finished, *port, *playerId });

		Engine::JoinAccepted accepted{ *playerId, *port };
		bootstrap.Send(ToFrame(Engine::EncodeJoinAccepted(accepted)));
	}
}

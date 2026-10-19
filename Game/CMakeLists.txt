# An individual game that runs on the shared engine. Each teammate has their
# own Game target; this one is yours. It may use engine systems freely but must
# not put reusable logic here (that belongs in Engine/).

# Milestone 2 Section 3: NetworkClient now owns a std::thread. AppleClang/libc++
# links this with no extra flag, but glibc/libstdc++ toolchains (e.g. CI's
# ubuntu-latest) need pthread linked explicitly.
find_package(Threads REQUIRED)

add_executable(main
    src/Game.cpp
    src/GameRules.cpp
    src/NetworkClient.cpp
    src/NetworkState.cpp
    src/PeerClient.cpp
    src/main.cpp
)

# Linking Engine also brings in SDL3 transitively (Engine links it PUBLIC).
# EngineNetwork brings in cppzmq/libzmq (Milestone 2 Section 2: the SDL client is now
# a real persistent network client, not optional).
target_link_libraries(main PRIVATE Engine EngineNetwork Threads::Threads)

# The headless multiplayer server (Milestone 2 Section 2, dedicated-session topology
# added in Section 4 checkpoint 2, server-authoritative moving platform added in
# Section 4's final checkpoint). Deliberately links EngineNetwork + EngineCore but never
# Engine itself, so no SDL3 — it owns no window/renderer. EngineCore brings in
# Engine::GameObject/GameObjectRegistry (Milestone 3: the server represents the shared
# platform and connected players through the same runtime object model the client uses),
# which has no SDL dependency of its own (see game-engine/Engine/CMakeLists.txt). Spawns
# one std::thread per dedicated client session, hence Threads::Threads.
add_executable(server
    src/server_main.cpp
    src/ServerPlatform.cpp
)
target_link_libraries(server PRIVATE EngineNetwork EngineCore Threads::Threads)

# Deterministic unit test for ComputePlatformState (see ServerPlatformTest.cpp) — pure
# function of two explicit time points, needs no running server and no real elapsed
# time. Links only EngineNetwork, same as `server` itself.
add_executable(ServerPlatformTest
    src/ServerPlatform.cpp
    src/ServerPlatformTest.cpp
)
target_link_libraries(ServerPlatformTest PRIVATE EngineNetwork)

# Deterministic unit test for GameRules.h (Milestone 3: spawn selection, camera-scroll
# threshold decision) — pure logic, no SDL, no sockets, no running server.
add_executable(GameRulesTest
    src/GameRules.cpp
    src/GameRulesTest.cpp
)

# Real, non-SDL integration test for NetworkClient's Milestone 2 Section 3 threading
# contract (see NetworkClientIntegrationTest.cpp for what it proves and why it isn't
# CTest-registered). Requires a real `server` already running at its endpoint.
add_executable(NetworkClientIntegrationTest
    src/NetworkClient.cpp
    src/NetworkClientIntegrationTest.cpp
)
target_link_libraries(NetworkClientIntegrationTest PRIVATE EngineNetwork Threads::Threads)

# Real, non-SDL 3-client integration test proving Section 4's dedicated-session
# isolation from the real NetworkClient's side (see MultiClientIntegrationTest.cpp).
# Requires a real running production server; not CTest-registered, same rationale as
# NetworkClientIntegrationTest.
add_executable(MultiClientIntegrationTest
    src/NetworkClient.cpp
    src/MultiClientIntegrationTest.cpp
)
target_link_libraries(MultiClientIntegrationTest PRIVATE EngineNetwork Threads::Threads)

# Lightweight, non-SDL test tooling for Milestone 2 Section 4 checkpoint 2's real
# bootstrap + dedicated-session server topology (see SessionTestClient.cpp for exact
# usage). This is NOT the real NetworkClient — that migration to JoinAccepted-driven
# handoff is the next checkpoint.
add_executable(SessionTestClient
    src/SessionTestClient.cpp
)
target_link_libraries(SessionTestClient PRIVATE EngineNetwork)

# Real, non-SDL-window integration test for Milestone 2 Section 4's final checkpoint:
# Timeline-scaled message rate and cross-client platform consistency (see
# NetworkRateAndPlatformTest.cpp). Needs Engine (for Engine::Timeline, which brings in
# SDL3 transitively — this test calls a minimal SDL_Init(0)/SDL_Quit(), no window).
# Requires a real running production server; not CTest-registered, same rationale as
# the other Game/src/*IntegrationTest.cpp files.
add_executable(NetworkRateAndPlatformTest
    src/NetworkClient.cpp
    src/PeerClient.cpp
    src/NetworkRateAndPlatformTest.cpp
)
target_link_libraries(NetworkRateAndPlatformTest PRIVATE Engine EngineNetwork Threads::Threads)

# Real, non-SDL integration test for PeerClient (Milestone 2 Section 5 PeerClient
# checkpoint — see PeerClientIntegrationTest.cpp). Needs no running production server:
# PeerClient is pure peer-to-peer, proven here purely between real PeerClient instances
# over real ZeroMQ PUB/SUB. Not CTest-registered, same rationale as the other
# Game/src/*IntegrationTest.cpp files. PeerClient is NOT yet linked into `main` —
# this checkpoint proves it independently, before any Game integration.
add_executable(PeerClientIntegrationTest
    src/PeerClient.cpp
    src/PeerClientIntegrationTest.cpp
)
target_link_libraries(PeerClientIntegrationTest PRIVATE EngineNetwork Threads::Threads)

# Real, non-SDL integration test for Milestone 2 Section 5's final integration: the
# server-Roster/P2P directness proof and the required real multi-client scenario (see
# P2pIntegrationTest.cpp). Requires a real running production server; not
# CTest-registered, same rationale as the other Game/src/*IntegrationTest.cpp files.
add_executable(P2pIntegrationTest
    src/NetworkClient.cpp
    src/PeerClient.cpp
    src/P2pIntegrationTest.cpp
)
target_link_libraries(P2pIntegrationTest PRIVATE EngineNetwork Threads::Threads)

# CSC 581 performance checkpoint: Full-State vs Delta-State object-exchange strategies.
# BenchmarkStatsTest has no Engine dependency at all (pure arithmetic).
add_executable(BenchmarkStatsTest
    src/BenchmarkStats.cpp
    src/BenchmarkStatsTest.cpp
)

# Correctness validation, pure logic (no sockets) — must pass before any benchmark
# timing result is trusted. Links EngineNetworkCore only (ExchangeProtocol has no
# ZeroMQ dependency).
add_executable(NetworkExchangeCorrectnessTest
    src/ExchangeStateTracking.cpp
    src/SyntheticScene.cpp
    src/NetworkExchangeCorrectnessTest.cpp
)
target_link_libraries(NetworkExchangeCorrectnessTest PRIVATE EngineNetworkCore)

# The real-ZeroMQ benchmark executable: one PUB publisher thread + client_count SUB
# receiver threads, measuring the complete encode/send/receive/decode/apply path.
add_executable(NetworkPerformanceBenchmark
    src/BenchmarkStats.cpp
    src/ExchangeStateTracking.cpp
    src/SyntheticScene.cpp
    src/NetworkPerformanceBenchmark.cpp
)
target_link_libraries(NetworkPerformanceBenchmark PRIVATE EngineNetwork Threads::Threads)

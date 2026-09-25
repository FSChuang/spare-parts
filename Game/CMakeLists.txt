# An individual game that runs on the shared engine. Each teammate has their
# own Game target; this one is yours. It may use engine systems freely but must
# not put reusable logic here (that belongs in Engine/).

# Milestone 2 Section 3: NetworkClient now owns a std::thread. AppleClang/libc++
# links this with no extra flag, but glibc/libstdc++ toolchains (e.g. CI's
# ubuntu-latest) need pthread linked explicitly.
find_package(Threads REQUIRED)

add_executable(main
    src/Game.cpp
    src/NetworkClient.cpp
    src/main.cpp
)

# Linking Engine also brings in SDL3 transitively (Engine links it PUBLIC).
# EngineNetwork brings in cppzmq/libzmq (Milestone 2 Section 2: the SDL client is now
# a real persistent network client, not optional).
target_link_libraries(main PRIVATE Engine EngineNetwork Threads::Threads)

# The headless multiplayer server (Milestone 2 Section 2, dedicated-session topology
# added in Section 4 checkpoint 2). Deliberately links only EngineNetwork — no Engine,
# so no SDL3 — since it owns no window/renderer/entities. Now spawns one std::thread per
# dedicated client session, hence Threads::Threads.
add_executable(server
    src/server_main.cpp
)
target_link_libraries(server PRIVATE EngineNetwork Threads::Threads)

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

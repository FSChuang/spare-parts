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

# The headless multiplayer server (Milestone 2 Section 2). Deliberately links only
# EngineNetwork — no Engine, so no SDL3 — since it owns no window/renderer/entities.
add_executable(server
    src/server_main.cpp
)
target_link_libraries(server PRIVATE EngineNetwork)

# Real, non-SDL integration test for NetworkClient's Milestone 2 Section 3 threading
# contract (see NetworkClientIntegrationTest.cpp for what it proves and why it isn't
# CTest-registered). Requires a real `server` already running at its endpoint.
add_executable(NetworkClientIntegrationTest
    src/NetworkClient.cpp
    src/NetworkClientIntegrationTest.cpp
)
target_link_libraries(NetworkClientIntegrationTest PRIVATE EngineNetwork Threads::Threads)

# An individual game that runs on the shared engine. Each teammate has their
# own Game target; this one is yours. It may use engine systems freely but must
# not put reusable logic here (that belongs in Engine/).

add_executable(main
    src/Game.cpp
    src/NetworkClient.cpp
    src/main.cpp
)

# Linking Engine also brings in SDL3 transitively (Engine links it PUBLIC).
# EngineNetwork brings in cppzmq/libzmq (Milestone 2 Section 2: the SDL client is now
# a real persistent network client, not optional).
target_link_libraries(main PRIVATE Engine EngineNetwork)

# The headless multiplayer server (Milestone 2 Section 2). Deliberately links only
# EngineNetwork — no Engine, so no SDL3 — since it owns no window/renderer/entities.
add_executable(server
    src/server_main.cpp
)
target_link_libraries(server PRIVATE EngineNetwork)

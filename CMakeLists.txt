cmake_minimum_required(VERSION 3.16)
project(SpareParts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All executables land in build/bin so assets paths are predictable across targets.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Engine/ is a synchronized mirror of FSChuang/game-engine's Engine/ (see
# .github/workflows/sync-engine.yml). Never edit it manually here.
add_subdirectory(Engine)

# The individual game, authoritative in this repository.
add_subdirectory(Game)

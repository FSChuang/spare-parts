# Spare Parts

The individual game built on top of the shared [FSChuang/game-engine](https://github.com/FSChuang/game-engine).

## Repository layout

- `Game/` — this game's code. Authoritative here; edit freely.
- `Engine/` — a synchronized, read-only mirror of `game-engine`'s `Engine/`.
  **Do not edit `Engine/` manually** — changes are overwritten automatically. To
  change engine behavior, make the change in `FSChuang/game-engine` instead; it
  syncs here automatically on every push to `game-engine`'s `main` branch (see
  `.github/workflows/sync-engine.yml`).

## Build & run

```bash
mkdir build && cd build
cmake ..
make
./bin/main
```

## Requirements

- CMake 3.16+
- A C++17 compiler
- SDL3  (`brew install sdl3` on macOS / `sudo apt install libsdl3-dev` on Ubuntu)

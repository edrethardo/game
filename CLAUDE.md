# DungeonEngine

## Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
./build/dungeon_game
```

## Architecture
Data-driven hybrid: JSON configs (`assets/config/`) define game data, C++ systems consume them at runtime. Pool-based allocation for entities, projectiles, world items. Namespace-based systems (ItemLoader, ItemGen, Combat, etc.) with stateless functions + state objects.

## Directory Map
- `src/core/` — Types, math, memory, logging
- `src/renderer/` — OpenGL 3.3 rendering, materials, meshes, shaders, camera
- `src/engine/` — Main engine class, game loop, system orchestration
- `src/game/` — Gameplay: items, weapons, combat, projectiles, entities, skills, AI
- `src/world/` — Level grid, collision, raycasting, level generation
- `src/net/` — ENet networking, packets, snapshots
- `src/platform/` — SDL2 abstraction
- `assets/config/` — JSON definitions (items, weapons, affixes, skills, enemies)
- `assets/meshes/` — OBJ models
- `assets/textures/` — PNG textures (42px tiles)
- `assets/shaders/` — GLSL vertex/fragment shaders

## Conventions
- C++17, no exceptions in hot paths
- Structs for data, enums for types, namespaces for system functions
- Static arrays with max constants (MAX_ENTITIES, MAX_PROJECTILES, etc.)
- All code changes must include inline comments for non-obvious logic
- When adding/modifying structs, keep JSON schema and loader in sync

## Targets
- Nintendo Switch + low-end PC (Core 2 Quad baseline)
- 60 FPS, 16.6ms frame budget
- OpenGL 3.3, max 300-500 draw calls
- Visual style: Barony-like low-poly

# DungeonEngine

Custom C++17 dungeon-crawler engine. Barony-style low-poly visuals, Hellgate-London-style loot/skills.
Targets: Nintendo Switch + low-end PC (Core 2 Quad). 60 FPS, 16.6 ms budget, OpenGL 3.3, 300–500 draw calls max.

## Knowledge skills (load detail on demand)

This file is intentionally lean — it holds only what's relevant every session (build,
architecture, layout, conventions). Detailed reference lives in two project skills; **invoke
the matching skill instead of expecting the detail inline here:**

- **`engine-reference`** — type/constant cheat sheet & `MAX_*` caps, the per-frame game loop,
  data lifecycles (hit feedback, entity/projectile/item drop, server-authoritative loot),
  JSON config schemas (items/affixes/skills/weapons/enemies/bosses), networking internals
  (server/client tick, snapshot quantization, packet sizing), and in-game debug keys.
- **`engine-how-to`** — adding content (items, affixes, weapons, enemies, bosses, enemy roles,
  skills, materials, levels), asset-generation conventions, and pitfalls/gotchas.

(Both skills live in `.claude/skills/`. They were extracted from this file; keep them in
sync — see Conventions.)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
./build/dungeon_game
```

Release: `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel`.
SDL2 is fetched via `fetch_sdl2.sh` if missing. Single binary, no install step.

## Architecture

**Data-driven hybrid.** JSON in `assets/config/` defines content (items, affixes, skills, enemies, weapons, materials). C++ systems load defs at startup into fixed-size arrays and consume them at runtime. Asset name strings are resolved to integer IDs (mesh IDs, material IDs) once after init — runtime code never touches strings.

**Pool allocation, no heap in hot paths.** Entities, projectiles, world items, materials, meshes all live in static arrays sized by `MAX_*` constants in their headers. No `new`/`delete` per frame. A 1 MB `FrameAllocator` (`src/core/frame_allocator.h`) is reset each frame for transient scratch.

**Namespace + struct.** Systems are namespaces of free functions (`Combat`, `EntitySystem`, `ItemGen`, `Net`, `Renderer`, ...). State lives in plain structs that callers pass in. No singletons except a couple of file-scope globals (logger, profiler, frame allocator, MaterialSystem table).

**Fixed-timestep dispatch.** `Engine::run` (`src/engine/engine.cpp:417`) accumulates real frame time and steps `update(1/60)` up to 4 times per frame, then renders once with an `alpha` interpolation factor. `update()` switches on `GameState` (`MENU`, `LOBBY_*`, `IN_GAME`), and inside `IN_GAME` switches on `NetRole` (`NONE`/`SERVER`/`CLIENT`) into `singleplayerUpdate` / `serverUpdate` / `clientUpdate`. All three live in `engine.cpp`.

**Split-screen (couch co-op).** Up to `MAX_LOCAL_PLAYERS` (=2) local players, mutually exclusive with networking (`m_splitPlayerCount` forced to 1 when `NetRole != NONE`). Per-player state in `m_localPlayers[]`/`m_cameras[]`/… is copied into "active aliases" (`m_localPlayer`/`m_camera`/…) by `swapInPlayer(idx)` before each player's `gameUpdate` and back by `swapOutPlayer(idx)`; shared world systems run once per frame in `Engine::tickSharedSystems` after the per-player loop. (Full swap-macro / shared-tick mechanics: `engine-reference`; the per-player-field gotcha: `engine-how-to`.)

**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 20 Hz (every 3 ticks). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 100 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)

## Directory Map

| Dir | Role |
|---|---|
| `src/core/` | Types (`u8`/`f32` aliases), math (Vec/Mat4), pools, logging, profiler, frame allocator, asserts |
| `src/platform/` | SDL2 abstraction: window, input (kb/mouse/gamepad), wall-clock |
| `src/renderer/` | OpenGL 3.3: shaders, meshes, OBJ loader, materials/textures, camera, frustum, debug-draw, HUD, font, minimap |
| `src/world/` | Cell grid, BSP level gen, geometry meshing, raycast (DDA), collision (move-and-slide), combat queries (cone/raycast/AABB) |
| `src/game/` | Player, entities (enemy NPCs), projectiles, weapons, combat resolution, items+affixes+inventory, skills, enemy AI FSM |
| `src/engine/` | Top-level `Engine` class, game loop, system orchestration, mode/lobby/menu logic |
| `src/net/` | ENet wrapper, packet read/write, input ring buffer, snapshot serialization, server, client (prediction + interpolation) |
| `assets/config/` | JSON content: `items.json`, `affixes.json`, `skills.json`, `weapons.json`, `enemies.json` |
| `assets/materials.json` | Material → texture+tint table (loaded at init) |
| `assets/meshes/` | Wavefront `.obj` (low-poly enemies, weapons, props) |
| `assets/textures/` | 42×42 PNG tiles (suffix `_42`); a few non-tile textures (skins) |
| `assets/shaders/` | GLSL: `basic` (lit textured), `unlit` (HUD/debug), `debug` |
| `tools/` | Auxiliary scripts/utilities |
| `phase*.md`, `plan.md` | Design docs (historical) |

## Conventions

- **Always document code changes.** Every code change must include inline comments explaining non-obvious logic, and the top of each substantive `.cpp` should have a brief block describing what the file is for and how it fits into the systems described here. **Keep docs in sync:** update **CLAUDE.md** for architecture / conventions / directory changes, and update the matching knowledge skill — `engine-reference` for types/constants, the game loop, data lifecycles, JSON schemas, networking, or debug keys; `engine-how-to` for add-things recipes, asset conventions, or pitfalls.
- **C++17**, no exceptions in hot paths (the JSON loaders catch and log).
- Plain structs for data, enums for types (`enum struct ... : u8` with `COUNT` sentinel where useful), namespaces for systems.
- Static arrays sized by `MAX_*` constants — bump the constant rather than allocating dynamically.
- `u8`/`u16`/`u32`/`f32` aliases from `core/types.h` are used everywhere — avoid raw `int`/`float` in new code.
- All code changes must include inline comments for **non-obvious** logic only — don't re-document what the names already say. The "why" matters, not the "what".
- Keep JSON schema and loader in sync when you add or modify a struct field.
- Prefer `Vec3` math operators in `core/math.h` over manual component arithmetic.
- For new GPU resources, pair `init`/`shutdown` (or `create`/`destroy`) and call them from `Engine::init`/`Engine::shutdown`.
- Profiling: wrap a section with `PROFILE_SCOPE(idx, "name")` from `core/profiler.h`. Indices 0–15.

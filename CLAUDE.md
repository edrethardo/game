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

**Steam vs non-Steam (itch.io) builds.** Steamworks is **opt-in via `USE_STEAM`** so one tree feeds both
stores. The **default build is Steam-free** (itch.io / DRM-free — zero `libsteam_api` dependency;
`src/platform/steam.cpp` compiles to no-ops). For the **Steam release**, unzip the Steamworks SDK into
`external/steamworks/` (gitignored, proprietary) and add `-DUSE_STEAM=ON`:
`cmake -B build-steam -DCMAKE_BUILD_TYPE=Release -DUSE_STEAM=ON && cmake --build build-steam` — this links
`libsteam_api` (mingw derives an import lib from the DLL via `gendef`/`dlltool`), copies the runtime lib +
a dev `tools/steam_appid.txt` next to the binary, and enables relay networking + matchmaking. Never
enabled on Switch. (Steam Cloud config for saves is separate — `docs/steam_cloud.md`.) **CI**
(`.github/workflows/build.yml`) builds BOTH variants for Windows/Linux/macOS on a tag push; the Steam jobs
clone the SDK from a **private companion repo** (`github.com/edrethardo/steamworks-sdk`, holding
`public/` + `redistributable_bin/`) into `external/steamworks/` using the repo secret `STEAMWORKS_SDK_TOKEN`
(a fine-grained PAT, Contents: read) — itch jobs never need it. Keeping the SDK in a *private* repo (not the
public tree) satisfies Valve's no-public-redistribution rule. The Steam **networking transport** (relay + `ISteamMatchmaking` lobbies/invites/browser/
quickmatch) lives in `src/net/net.cpp` (`Transport::STEAM` branch) + `src/platform/steam.{h,cpp}`; App ID
is `4819550`.

**Releasing to stores.** `docs/DEPLOYMENT.md` is the release-agent runbook: which CI artifact goes to which
Steam depot / itch channel / Switch console, the `steamcmd`/`butler`/`nxlink` steps, the beta→default
promotion gate, and the required secrets. Releases are tag-driven — `tools/release.sh <bump> --push` cuts a
`v*` tag and CI publishes the 8 zips (`itch`/`steam` × Win/Linux-22.04/Linux-24.04/macOS) to a GitHub Release.

## Testing

doctest-based unit tests live in `tests/` (mirrors `src/` structure). The framework is vendored as a single header at `external/doctest/doctest.h`.

```bash
cmake --build build --target dungeon_tests   # build only the test binary
./build/tests/dungeon_tests                   # run the full suite
./build/tests/dungeon_tests -tc="*ClockSync*" # filter to matching cases
ctest --test-dir build --output-on-failure   # CTest wrapper
```

`BUILD_TESTS=ON` is the default for desktop; Switch builds skip tests entirely. Opt out with `cmake -B build -DBUILD_TESTS=OFF` for a game-only desktop build.

**TDD workflow.** The netplay rewrite (M1+) is test-first: write a `TEST_CASE` in `tests/<subsystem>/test_<unit>.cpp` describing the next behavior, watch it fail with a specific assertion, implement until it passes, refactor, commit. doctest's expression decomposition (`REQUIRE(a == b)` shows both values on failure) and `doctest::Approx` (for floating-point) are the primary tools.

**Adding a test that touches production code.** Extend the `add_executable(dungeon_tests ...)` source list in `tests/CMakeLists.txt` with both the new test file AND any production `.cpp` it links against. When the list gets unwieldy, refactor `src/` into a `dungeon_core` library that both `DungeonEngine` and `dungeon_tests` link.

**Scope.** Forward-only on new code — no backfill tests for existing combat/AI/item-gen/render. See [docs/superpowers/specs/2026-05-31-test-framework-design.md](docs/superpowers/specs/2026-05-31-test-framework-design.md) for the rationale.

**CI runs the suite** (`ctest`) on the native Linux + macOS jobs. It did not until 2026-07-14 — the tests were built and thrown away, so a red test could not stop a release. The Windows job cross-compiles and cannot execute its own binary, so it still only builds.

**Assets are GENERATED, not committed** (`assets/meshes/*.obj` is gitignored). The mesh table is `src/engine/asset_manifest.h`; `tools/build_assets.py` **hard-fails** if the engine names a mesh it doesn't generate, so a mesh added to one and not the other can no longer ship as an invisible fallback cube. Adding a mesh means editing **both**. (Full trap: `engine-how-to` → Pitfalls.)

## Architecture

**Data-driven hybrid.** JSON in `assets/config/` defines content (items, affixes, skills, enemies, weapons, materials). C++ systems load defs at startup into fixed-size arrays and consume them at runtime. Asset name strings are resolved to integer IDs (mesh IDs, material IDs) once after init — runtime code never touches strings.

**Pool allocation, no heap in hot paths.** Entities, projectiles, world items, materials, meshes all live in static arrays sized by `MAX_*` constants in their headers. No `new`/`delete` per frame. A 1 MB `FrameAllocator` (`src/core/frame_allocator.h`) is reset each frame for transient scratch.

**Namespace + struct.** Systems are namespaces of free functions (`Combat`, `EntitySystem`, `ItemGen`, `Net`, `Renderer`, ...). State lives in plain structs that callers pass in. No singletons except a couple of file-scope globals (logger, profiler, frame allocator, MaterialSystem table).

**Fixed-timestep dispatch.** `Engine::run` (`src/engine/engine.cpp:417`) accumulates real frame time and steps `update(1/60)` up to 4 times per frame, then renders once with an `alpha` interpolation factor. `update()` switches on `GameState` (`MENU`, `LOBBY_*`, `IN_GAME`), and inside `IN_GAME` switches on `NetRole` (`NONE`/`SERVER`/`CLIENT`) into `singleplayerUpdate` / `serverUpdate` / `clientUpdate`. All three live in `engine.cpp`.

**Split-screen (couch co-op).** Up to `MAX_LOCAL_PLAYERS` (=2) local players. Historically mutually exclusive with networking (`m_splitPlayerCount` forced to 1 when `NetRole != NONE`); the in-progress **online couch co-op** feature (two local players sharing one connection) is the deliberate exception, gated by the `m_netCouch` flag — Phase 1 (host-couch: the host plays split-screen while remotes join, slots 0+1 host-local) has landed, client-couch is pending. See `~/.claude/plans/i-have-the-issue-wild-bentley.md`. Per-player state in `m_localPlayers[]`/`m_cameras[]`/… is copied into "active aliases" (`m_localPlayer`/`m_camera`/…) by `swapInPlayer(idx)` before each player's `gameUpdate` and back by `swapOutPlayer(idx)`; shared world systems run once per frame in `Engine::tickSharedSystems` after the per-player loop. (Full swap-macro / shared-tick mechanics: `engine-reference`; the per-player-field gotcha: `engine-how-to`.)

**Persistence is per-character.** Each save file (`save_NN.dat`) holds exactly ONE character (`playerCount=1`); the per-lane destination is `m_playerSaveSlot[lane]` and `saveAllCharacters()` writes each active lane to its own slot. In couch co-op both players pick their own slot (New or Continue) in the menu lobby and the shared dungeon runs on **Player 1's floor**; mixed New/Continue lanes work because the menu prepares each lane and calls `startGame(mode, lanesPrepared=true)` (skipping the NEW_GAME wipe). Legacy `playerCount=2` bundle saves still load (and migrate to per-character on the next save). The on-disk layout is **versioned** (`SAVE_VERSION`, currently 3 = GLOVES slot + attack-speed cache in `PlayerInventory`): readers accept the previous version via a `Legacy*V<n>` mirror struct in `engine_persist.cpp`, and `static_assert`s pin the serialized struct sizes — any layout change MUST bump the version and extend the legacy readers. (Save format, menu flow, no-downgrade guard: `engine-reference`.)

**User-data location (desktop) + Steam Cloud.** Saves, `difficulty_unlock.dat`, and the `controls.json`/`audio.json`/`video.cfg` prefs are written into the per-user dir `SDL_GetPrefPath("EdRethardo","DungeonEngine")` via `Platform::userDataPath()` (`src/platform/user_paths.{h,cpp}`), NOT the install/working dir — so Steam **Auto-Cloud** can sync them (config: `docs/steam_cloud.md`). On `__SWITCH__` `userDataDir()` is `""` (CWD = app storage), so Switch is unchanged. A one-time `migrateLegacyUserData()` (in `Engine::init`) copies pre-relocation files from the CWD/exe dir into the pref dir **only when the destination is absent** (never destructive). `saveCharacter` writes **atomically** (temp + `Platform::atomicReplace`) so an interrupted save can't corrupt an existing slot. `ORG`/`APP` are frozen — changing them orphans saves.

**`controls.json` is a versioned format too.** Bindings are serialized **by `GameAction` enum ordinal**, so enum members are renamed in place or appended before `COUNT` — **never inserted or removed**, or every existing player's bindings re-map onto the wrong actions. Changing an existing action's *default* binding additionally requires bumping `BINDINGS_REV` (the `CFG_BINDINGS_REV` sentinel row) and repairing that action in `loadBindings`, because a saved file's stale row otherwise silently beats the new default. Details + the rebind-UI range caveat: `engine-reference`.

**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 60 Hz (every tick). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 33 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)

**Netplay stack (rewrite COMPLETE — M0–M14 + the 2026-07 hardening pass).** Server-authoritative + client prediction with **rollback-replay reconciliation**: on a mispredict the client rewinds to the server's acked state and re-applies every stored input through the same per-input step the server drain runs (`updateNetPlayerFromInput` movementOnly + `moveAndSlide`), committing to BOTH player mirrors (`m_localPlayer` alias AND `m_players[slot]` — an alias-only write is erased next tick by `syncNetPlayerToLocalPlayer`). Under input starvation the server **coasts** a remote on its last input for ≤250 ms and CLAIMS the ticks (advances `lastProcessedInputTick`) so snapshots stay time-consistent; a separate `m_lastActivationTick` watermark fires late-arriving activation edges exactly once. Delta compression is **ack-driven with a named baseline**: every delta names the snapshot tick it's encoded against (client acks via `NetInput.ackedSnapshotTick`, server deltas against its 32-deep global history ring, client decodes from its 32-deep ring) — never assume "baseline = last sent". `PROTOCOL_VERSION` 14. Verification rig: `--net-loss <0-90> --net-latency <ms> --bot-walk` + the F9 net-graph (`[NET-GRAPH]` 1 Hz log: rtt/div/idelay/KB/s/snap-Hz/baseline-age). Measured envelope: 15% loss + 100 ms RTT → 0 hard snaps, deltas engaged (~9 KB/s vs 39 full). (Wire/tick details: `engine-reference`.)

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
| `assets/config/` | JSON content: `items.json`, `affixes.json`, `skills.json`, `weapons.json`, `enemies.json`, `events.json` |
| `assets/materials.json` | Material → texture+tint table (loaded at init) |
| `assets/meshes/` | Wavefront `.obj` (low-poly enemies, weapons, props) |
| `assets/textures/` | 42×42 PNG tiles (suffix `_42`); a few non-tile textures (skins) |
| `assets/shaders/` | GLSL: `basic` (lit textured), `unlit` (HUD/debug), `debug` |
| `tools/` | Auxiliary scripts/utilities (incl. `gen_steam_capsules.py` → `store/steam/`) |
| `store/` | Marketing/store assets (Steam capsules); not loaded by the engine |
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

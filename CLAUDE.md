# DungeonEngine

Custom C++17 dungeon-crawler engine. Barony-style low-poly visuals, Hellgate-London-style loot/skills.
Targets: Nintendo Switch + low-end PC (Core 2 Quad). 60 FPS, 16.6 ms budget, OpenGL 3.3, 300–500 draw calls max.

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

**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 20 Hz (every 3 ticks). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 100 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets.

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

## Key Types and Constants (cheat sheet)

| Type | Header | Notes |
|---|---|---|
| `Engine` | `engine/engine.h` | Owns all pools, defs, networking state |
| `Player` | `game/player.h` | Local-only struct (camera, lock, health). Singleplayer authority |
| `NetPlayer` | `net/net_player.h` | Server-authoritative player. Slot 0 = host. `eyePos()` adds eyeHeight |
| `Entity` | `game/entity.h` | Enemy NPC. `flags` bitmask: `ENT_ACTIVE/FLYING/DEAD`. `aiState`: `IDLE/CHASE/ATTACK/FLYBY/DEAD`. `enemyRole` is a u8 bitmask (see `EnemyRole` namespace). `bossDefIdx` links to `BossDefTable`. |
| `BossDef` / `BossDefTable` | `game/boss_def.h` | Boss definition loaded from `bosses.json`. Stats, role bitmask, AI personality, skill, projectile, loot guarantee. |
| `BossPersonality` | `game/boss_def.h` | `BERSERKER/KITER/TELEPORTER/DUELIST` — overrides FSM state selection for bosses |
| `Projectile` | `game/projectile.h` | `projFlags`: `PROJ_ORB/ORB_SHARD/GRAVITY/SPLASH` |
| `ItemDef` / `ItemInstance` | `game/item.h` | Static template vs rolled runtime item. `defId == 0xFFFF` ⇒ empty |
| `AffixDef` / `Affix` | `game/item.h` | `validSlots` is a bitmask of `ItemSlot` values |
| `WeaponDef` / `WeaponState` | `game/weapon.h` | `WeaponType`: MELEE/HITSCAN/PROJECTILE selects path in `Combat` |
| `SkillDef` / `SkillState` | `game/item.h` | One `SkillState` per player (cooldown + energy) |
| `LevelGrid` / `GridCell` | `world/level_grid.h` | Cell flags `CELL_SOLID/FLOOR/CEILING`. Heights in quarter-units |
| `WorldSnapshot`, `SnapPlayer/Entity/Projectile` | `net/snapshot.h` | Quantized server-to-client state |
| `NetInput` | `net/net_player.h` | Client→server input: `INPUT_FORWARD/BACKWARD/LEFT/RIGHT/JUMP/FIRE/LOCK` flags |
| `AABB` | `renderer/frustum.h` | Min/max box for collision and frustum culling |

Important caps (search the header for the constant if you need to grow it):
`MAX_PLAYERS=4`, `MAX_ENTITIES=128`, `MAX_PROJECTILES=1024` (512 on Switch), `MAX_ITEM_DEFS=64`,
`MAX_AFFIX_DEFS=32`, `MAX_AFFIXES_PER_ITEM=4`, `MAX_INVENTORY_ITEMS=24`,
`MAX_SKILL_DEFS=16`, `MAX_WORLD_ITEMS=32`, `MAX_WEAPON_DEFS=16`, `MAX_MATERIALS=64`,
`MAX_MESH_DEFS=32` (Engine-local), `MAX_LEVEL_SECTIONS=64`, `MAX_DUNGEON_ROOMS=32`,
`SECTION_SIZE=16` cells, `NET_TICK_RATE=60`, `SNAPSHOT_RATE=20`, `INPUT_BUFFER_SIZE=64`.

## Game Loop (per frame)

1. `Clock::update`, reset frame allocator + alloc tracker.
2. `Window::pollEvents` → SDL pump.
3. `Net::poll` (if networked) — fires `onSnapshot`/`onInput`/`onEvent`/`onPlayerJoin`/`onPlayerLeft` callbacks set during `Engine::init`.
4. While `accumulator >= 1/60`: `Input::update`, then `update(1/60)` — dispatches by `GameState`/`NetRole`.
5. `render(alpha)` — single render pass with interpolation alpha for visual smoothing.
6. Profiler frame record + per-second stats log.

`update` steps (in `singleplayerUpdate`, with server/client variants similar):
`PlayerController` → `Collision::moveAndSlide` → target lock → weapon fire → `EnemyAI::update` → `ProjectileSystem::update` → `EntitySystem::tickTimers` → `WorldItemSystem::update` → `SkillSystem::update`/`updateOrbProjectiles`/`updateMeteors` → skill activation → pickup checks → `Minimap::updateVisited`.

## Data Lifecycles

**Game start.** `Engine::startGame(GameStart mode)` (`engine_startgame.cpp`) builds the level and resets per-floor state. The `mode` makes player-progression intent explicit instead of inferring it from floor/difficulty/inventory: `NEW_GAME` wipes inventory/skills/quickbar, grants the class starting loadout via `equipStartingLoadout(playerIdx)`, and resets HP to class base; `CONTINUE` leaves inventory/skills/HP untouched (a `loadGame` already restored them — used by menu Continue, network client join, and death-screen reload); `DESCEND` keeps the current run's gear/HP into the next floor (used by `FLOOR_TRANSITION`, including the floor-50→1 difficulty loop). Never reintroduce the old "infer from empty weapon slot" heuristic — pass the right mode at the call site.

**Hit feedback.** `Combat::applyDamage` classifies every hit into an `ImpactTier` (LIGHT/HEAVY/CRIT/KILL — `game/hit_feedback.h`) and fires the matching recipe from the tunable `kHitTiers` table: camera shake + blood/spark/debris/smoke particles inline (combat holds the FX pointers via `setFXTargets`) and a tier-tagged damage number (crit/kill styled). **Knockback** is applied authoritatively in `applyDamage` (size/boss-resisted, only with a `damageOrigin`) so it syncs over the network; `EnemyAI::update` yields while `Entity.knockbackTimer > 0` so the impulse is visible, and `tickTimers` decays it. Crits are **weapon data** (`WeaponDef.critChance`/`critMult`, set per weapon subtype in `buildWeaponDef` — daggers highest, all others a 5% baseline) **rolled inside** `Combat::fireMelee`/`fireHitscan`/`fireProjectile`, which pass `isCrit` to `applyDamage` (projectiles carry `Projectile.isCrit` to their direct hit). No crit logic in `engine_combat`. **Taking damage** (`applyDamageToPlayer`) drives the red `Player.hurtVignette` (per-hit, damage-scaled, decays each frame) rendered as a **radial edge vignette** (`vignette.frag` via `renderPostOverlays`, intensity in the quad alpha) — combined at render time with a *steady* (non-flashing) low-HP glow computed from current HP. **No oscillation anywhere — photosensitivity-safe (WCAG 2.3.1); never a full-screen red sheet.** Plus the pre-existing camera kick / hit sound / `hitIndicators` directional arcs, and `Input::rumble` (incl. a light low-HP nag). The vignette is cleared on death so none lingers into respawn. Tune all feel from the `kHitTiers` table. Hit-stop is deferred (the `hitStopMs` field is reserved, 0 in v1). Full spec/plan: `docs/superpowers/specs/2026-05-23-hit-feedback-design.md`.

**Entity.** `EntitySystem::spawn` returns `EntityHandle{index, generation}`. Use `handleValid` / `handleGet` (free helpers in `entity.h`) — never index the pool directly across frames; entity slots get reused and `generation` invalidates stale handles. `Combat::applyDamage` flips `ENT_DEAD` and starts `deathTimer`; `tickTimers` later calls the death callback (`Combat::setDeathCallback`) and frees the slot. The engine sets a callback in `Engine::init` that rolls a 40% loot drop via `ItemGen::rollItem` and spawns a `WorldItem`.

**Projectile.** `Combat::fireProjectile` reserves a slot in `ProjectilePool`. `ProjectileSystem::update` integrates motion (with optional gravity), DDA-collides against the grid, and AABB-tests every active entity (and the player if `!fromPlayer`). On hit, applies damage + splash if `PROJ_SPLASH`. Frozen-Orb projectiles are special-cased: `SkillSystem::updateOrbProjectiles` ticks `subTimer` and spawns shards.

**Item drop.** Enemy dies → death callback rolls `ItemInstance` (via `ItemGen::rollRarity` then `rollAffixes` filtered by `ItemSlot`) → `WorldItemSystem::spawn` puts it in the world with `ownerSlot` (the killer) for 3 s then free-for-all. `Inventory::addToBackpack` moves it to backpack on pickup; `Inventory::equip` swaps backpack ↔ slot and calls `recalculateStats`. Stats are cached on `PlayerInventory.bonus*` fields and consumed by `Inventory::getEffectiveWeapon` (which builds a per-call `WeaponDef` merging base + affixes) and `Inventory::getEffectiveMaxHealth`.

**Loot is SERVER-AUTHORITATIVE (N5).** Only `NetRole::NONE` (SP/split) and `NetRole::SERVER` (host) roll/spawn drops: the death-callback orchestrator in `engine_init_callbacks.cpp` runs the cosmetic preamble for everyone but **returns early on `NetRole::CLIENT`** before any loot phase (`handleFirstKillDrop`/`handleBossLootDrop`/`handleNormalLootDrop`/`handleOnKillRingPassives`), so clients never roll their own `std::rand` drops. World items are replicated in the snapshot (`SnapWorldItem`, see *Snapshot quantization*) and the client mirrors them into its local `m_worldItems` each frame via `Client::mirrorWorldItems` (called in `clientNetPost`) — the renderer and pickup-aim code read `m_worldItems` directly, so loot appears/disappears in lockstep with the server (no interpolation; items are static). **Pickups are server-validated:** the client's `updatePlayerPickup` (CLIENT branch) picks the aimed item and sends `CL_PICKUP_ITEM` (header(4)+uid u32(4), reliable) via `Engine::sendPickupRequest` instead of removing it locally; the server dispatches it through `Net::setOnPickup` → `serverHandlePacket` → `Engine::onPickup` → `Engine::handlePickupRequest`, which re-checks proximity (≤3.5 m XZ vs the authoritative `NetPlayer.position`) + ownership, moves the item into that slot's inventory (auto-equipping an empty weapon slot), and frees the world slot — the removal propagates to all clients in the next snapshot. **Globes stay auto-pickup:** the host services a remote client's globe pickup in `serverNetPost` (the client is a "remote" slot there); clients never consume globes locally. SP/host local pickup behavior is unchanged.

**Networking (server tick).** Receive `NetInput` from clients into per-slot `InputRingBuffer`. For each player slot, apply latest input via `PlayerController::updateNetPlayerFromInput` → `Collision::moveAndSlide` → handle weapon fire/skills against authoritative state. Update entities and projectiles. Every 3rd tick (20 Hz), `Server::sendSnapshot` builds and broadcasts a `WorldSnapshot`. `lastInputTick[slot]` rides along so clients know which input has been processed.

**Networking (client tick).** Capture local input (`Client::captureAndSendInput`) → store in prediction history → predict the local player. When a snapshot arrives (`Client::receiveSnapshot`), `reconcile` compares server position vs predicted — if delta exceeds threshold, snap and replay buffered inputs. `interpolateRemotePlayers/Entities/Projectiles` lerp between two recent snapshots with a 100 ms delay for smooth remote motion; `mirrorWorldItems` copies the loot list directly. **N4 caveat (NOT fully fixed):** the client STILL runs the full authoritative sim locally on `m_entities`/`m_projectiles` in `gameUpdate` (`EnemyAI::update`, `ProjectileSystem::update`, `EntitySystem::tickTimers`, `SquadSystem::update`, client weapon fire) — a redundant "ghost" world. Rendering is unaffected (the client renders from `m_renderInterp.entities`/`.projectiles`, not `m_entities`), and loot no longer diverges (the death callback's loot phases are gated off on clients). De-duplicating this is **deferred** because the client prediction-collision obstacle list in `clientNetPre` is built from `m_entities`; gating the sim off would freeze those at spawn positions, so the obstacle source must first be moved to the interpolated snapshot entities (`m_renderInterp.entities`). See *Pitfalls*.

**Snapshot quantization.** Emitted wire sizes (bytes actually serialized, *not* `sizeof` the padded structs): `SnapPlayer`=29 B, `SnapEntity`=20 B, `SnapProjectile`=18 B, `SnapWorldItem`=14 B. These exact per-record sizes are hand-mirrored by the `SNAP_*_WIRE` constants near the top of `snapshot.cpp` — the serializer pre-computes how many records fit `MAX_SNAPSHOT_SIZE` from them and the writer lambdas (`w8`/`w16`/`w32`) do NOT bounds-check per write, so **if you add a field to a record you MUST bump the matching `SNAP_*_WIRE` to the new byte count or you introduce a buffer overflow.** **Drop priority (highest→lowest): players → entities → projectiles → world items.** Under a tight budget, world items drop first (last written); `buildFromState` orders entities/projectiles nearest-player-first so a capped list keeps the most-visible records. The snapshot header is **9 B** (`serverTick` u32 + `playerCount` u8 + `entityCount` u8 + `worldItemCount` u8 + `projectileCount` u16); `SNAP_FIXED_BYTES = 4 (packet hdr) + 9 (snap hdr) + MAX_PLAYERS*4 (input ticks) = 29 B`. `SnapWorldItem` = `slotIndex` u8 (pool slot, client mirrors directly) + `rarity` u8 + `defId` u16 + `uid` **u32** (full id — no overflow truncation) + packed pos u16×3 = 14 B; clients copy it straight into `m_worldItems[slotIndex]` for render/aim. `SnapEntity.bossStatus` packs `minionShield` (bit0) + `bossPhase` (bits1-3) so clients render an invuln/sealed boss as un-killable (reused the old alignment byte → size unchanged). `SnapProjectile` carries `flags.bit2=isCrit`, `projFlags`, `meshId`, and `radiusQ` (radius × 100, 0-2.55 m) so skill/boss projectiles render with the right look on clients; `lightColor` is NOT on the wire — the client reconstructs a glow color from `projFlags` in `Client::interpolateProjectiles`. At 8 KB the budget holds 4 players + 128 entities + ~304 projectiles + 32 world items (448 B always fits in practice). Positions packed to u16 over [-128, 128] m (~4 mm precision); velocities over [-30, 30] m/s; angles over [-π, π]. See `Quantize::pack*/unpack*` in `net/packet.h`.

**Snapshot packet sizing & overflow safety.** Full PC worst case is 28 fixed (4 B packet hdr + 8 B snap hdr + 4×4 B input-tick) + 4×29 + 128×20 + 1024×15 ≈ 18 KB — far over one UDP datagram. The snapshot buffer is `MAX_SNAPSHOT_SIZE` (`net/packet.h`, 8 KB: holds all players + all entities + ~365 projectiles), **separate from** `MAX_PACKET_SIZE` (4 KB, still used by the inline `PacketWriter` for small packets like client input). `Snapshot::serialize` (`net/snapshot.cpp`) is **overflow-safe**: it pre-computes how many records fit under the byte budget in priority order (players > entities > projectiles) and writes the count header with the *actual* counts, so a packet's declared counts can never exceed the bytes present (the old code wrote counts first then silently dropped trailing records → clients read phantom all-zero records at pos ≈ −128 m). `buildFromState` pre-sorts entities/projectiles nearest-player-first so any priority drop sheds the farthest (least-visible) records. Snapshots are broadcast via `Net::broadcastSnapshot` (`net/net.cpp`), which uses `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` on channel 1 — over-MTU payloads fragment into MTU-sized **unreliable** pieces (no retransmit latency; a lost fragment just drops that one snapshot, next arrives in 50 ms). This replaces the old `broadcastUnreliable` (`ENET_PACKET_FLAG_UNSEQUENCED`) for snapshots: an over-MTU *unsequenced* packet would have been silently downgraded to *reliable* fragments by ENet (peer.c), adding retransmit latency at 20 Hz. `broadcastUnreliable` remains for any sub-MTU unsequenced use.

## Asset Conventions

- **Always use tools to generate assets.** Meshes are generated via `tools/gen_mesh.py`, textures via `tools/gen_texture.py`, skins via `tools/gen_skin.py`, skill icons via `tools/gen_skill_icons.py`. Run `tools/build_assets.py` to rebuild all assets. Never hand-author `.obj` or `.png` files — the tools ensure correct format, naming, and sizing.
- **Textures**: tile textures end in `_42.png` (42×42 px), e.g. `stone_wall_42.png`. Skins/non-tile textures are exceptions (e.g. `bat_skin_42.png` is also 42 but "skin" naming).
- **Materials** (`assets/materials.json`): each entry has `id` (must match array index), `name`, optional `texture` (path under `assets/textures/`), optional `tint` `[r,g,b,a]`. Material 0 is the default fallback. Code looks up by name via `MaterialSystem::getIdByName`.
- **Meshes** (`assets/meshes/*.obj`): triangulated, +Y up, units in metres. Loaded at engine init by name into the `Engine::m_meshDefs` registry; mesh 0 is always a unit cube fallback. Names referenced from JSON (item `mesh` field, enemy `meshName`).
- **Shaders** (`assets/shaders/*.{vert,frag}`): `basic` is the lit textured shader for level/entities/items. `unlit` is for HUD/billboards. `debug` for `DebugDraw`. `vignette` (reuses `unlit.vert`) is the fullscreen radial red damage vignette drawn in `Engine::renderPostOverlays` via `drawScreenQuad`; intensity rides in the quad's alpha and the fragment shader does the corner-weighted falloff — it never flashes (photosensitivity-safe).

## JSON Config Schemas

`assets/config/items.json` — array under `"items"`. Per entry:
```
name (str), slot ("weapon"|"offhand"|"helmet"|"armor"|"boots"|"ring"),
weaponType ("melee"|"hitscan"|"projectile"), weaponSubtype (e.g. "sword"),
mesh (mesh registry name), material (material name),
baseDamage, baseRange, baseCooldown, baseConeAngle, baseProjectileSpeed,
baseProjectileRadius, baseRecoil, baseHealth,
minLevel, maxLevel, maxRarity ("common"|"magic"|"rare"|"legendary"),
legendarySkill ("frozen_orb"|"chain_lightning"|"meteor_strike"|"blood_nova"|"phase_dash"),
dropWeight (float, default 1.0)
```
Loader: `ItemLoader::loadItemDefs` (`src/game/item.cpp:98`). Mesh+material strings are resolved to IDs after init by `ItemLoader::resolveVisuals` and a loop in `Engine::init`.

`assets/config/affixes.json` — array under `"affixes"`. Per entry: `type` (one of the `AffixType` enum values, snake_case), `name`, `slots` (array of slot strings — converted to bitmask), `minValue`, `maxValue`. Loader: `ItemLoader::loadAffixDefs`.

`assets/config/skills.json` — array under `"skills"`. Common fields: `id` (matches `SkillId` enum), `name`, `cooldown`, `energyCost`, `damage`. Per-skill specifics: orb (`orbDamage`/`shardDamage`/`shardCount`/`shardInterval`/`orbSpeed`/`shardSpeed`/`orbRadius`/`shardRadius`/`duration`/`angleStepDeg`), chain (`bounces`/`bounceRange`/`damageFalloff`), nova (`healthCostPct`/`radius`), meteor (`delay`/`radius`), dash (`distance`/`corridorWidth`/`invulnDuration`).

`assets/config/weapons.json` — currently a static fallback table; the live weapon table is built in code by `initWeaponTable` in `game/weapon.h`. Weapon stats actually used in-game come from equipped `ItemInstance`s via `Inventory::getEffectiveWeapon`.

`assets/config/enemies.json` — array under `"enemies"`. 36 enemy types across 5 tiers. Per entry: `name`, `tier` (1-5), `meshName`, `materialName`, `health`, `moveSpeed`, `detectionRange`, `attackRange`, `attackCooldown`, `damage`, `flying` (bool), `halfExtents` `[x,y,z]`, `role` (string → `EnemyRole` bitmask), `aiPreference` (string → initial `AIState`), `onHitEffect` (0-4), `onHitDuration`, `onHitDps`, `dropWeight`.

`assets/config/bosses.json` — array under `"bosses"`. 10 boss definitions on milestone floors (5,10,...50). Per entry: `name`, `floor`, `isMajor`, stats (`baseHp`, `baseDmg`, `speed`, etc.), `meshName`, `matName`, `weaponName`, `roles` (array of role strings → bitmask), `personality` (string → `BossPersonality`), `skillId` (skill name string), `enrageFactor`, `minionShield`, `onHitEffect`, `projectile` (sub-object), `lootGuarantee` ("rare"/"legendary"), `bonusDrops`, `limbConfig`. Loader: `BossLoader::load` (`game/boss_loader.cpp`).

**Boss mesh rework (visual pass):** Each of the 8 non-Butcher/Ygara bosses now has a dedicated mesh instead of reusing shared bodies: `lich` (Sethrak, floor 15), `warden` (Malachar, floor 20), `spider_queen` (Ixara, floor 25), `korvath` (floor 30), `azhar` (floor 35), `diablo` (floor 40), `nyx` (Nyx, floor 45), `reaper` (Grim Reaper, floor 50). Limb configs 5 (`s_bossLichConfig`) and 6 (`s_bossNyxConfig`) were added to `limb_system.cpp`. Configs 4 (Reaper), 5, and 6 hide walking-leg limbs (slots 0-1 return mesh id 0) because their body OBJs include a full-length robe/cloak that would clip through separate leg geometry.

## How to Add Things

**New item**: append to `assets/config/items.json`. If it needs a new mesh, drop the `.obj` into `assets/meshes/` and add it to the `kMeshes` table in `Engine::init` (`engine.cpp:175`). If it needs a new material, add an entry in `assets/materials.json`. Increase `MAX_ITEM_DEFS` if you exceed 64.

**New affix type**: add to `AffixType` enum in `game/item.h`, parse it in `loadAffixDefs` (`game/item.cpp`), wire it into `Inventory::recalculateStats` so the bonus actually applies, and consume the bonus where relevant (e.g. `getEffectiveWeapon`, `getEffectiveMaxHealth`).

**New weapon type/subtype**: add subtype enum value in `game/weapon.h`. To affect combat behavior beyond the three existing `WeaponType`s (MELEE/HITSCAN/PROJECTILE), modify `Combat::fireMelee`/`fireHitscan`/`fireProjectile` and the dispatch in `Engine::handleWeaponFireForPlayer`.

**New enemy type**: append to `assets/config/enemies.json` with a `tier`, `role`, `aiPreference`, and stats. If it needs a new mesh, drop the `.obj` into `assets/meshes/` and register it in `Engine::init`'s `kMeshes`. Add a material in `materials.json` for its skin. The `role` field is a string matching an `EnemyRole` bitmask value (`"normal"`, `"charger"`, `"ranged_caster"`, `"bomber"`, `"shield_bearer"`, `"ambush"`, `"summoner"`, `"healer"`, `"aura"`). AI behavior is driven by the role — see `enemy_ai.cpp` for role-specific state selection.

**New boss**: append to `assets/config/bosses.json` with floor, stats, role bitmask (array of role strings), personality (`"berserker"`, `"kiter"`, `"teleporter"`, `"duelist"`), optional skill ID, projectile definition, and loot guarantee. Boss loader is `BossLoader::load` in `game/boss_loader.cpp`. Personality-driven AI is in `BossAI::update` (`game/boss_ai.cpp`). Boss loot guarantee is checked in the death callback in `engine_init.cpp`.

**New enemy role**: add a bitmask constant to the `EnemyRole` namespace in `game/entity.h`, add behavior in `enemy_ai.cpp`'s per-entity loop, and parse the role string in the enemy JSON loader.

**New skill**: add to `SkillId` in `game/item.h`, add fields to `SkillDef` if needed, parse in `loadSkillDefs`, add an activation branch in `SkillSystem::tryActivate` (`game/skill.cpp`). Per-tick logic that needs to persist (orb shards, meteor delay) goes in `updateOrbProjectiles` / `updateMeteors`. Add an entry in `assets/config/skills.json` and reference from a legendary item via `legendarySkill`.

**New material**: edit `assets/materials.json`. ID must equal array index. Look up at runtime by name with `MaterialSystem::getIdByName`. Tint blends with sampled texture color (1,1,1,1 = unmodified).

**New level layout**: procedural BSP gen in `LevelGen::generate` (`world/level_gen.cpp`) is the production path — pass a seed and grid dimensions. `LevelGen::generateTestDungeon` is a hardcoded fallback. Hand-authored levels can be loaded via `LevelLoader::loadFromJson` (`world/level_loader.h`) but no level JSON files ship by default.

## Networking Quick Reference

- **ENet** under the hood (`src/net/net.cpp`). Default port 7777. Protocol version is checked on `CL_JOIN_REQUEST`.
- Packets prefixed with `PacketHeader{type, flags, seq}`. Types in `NetPacketType`: `CL_INPUT`, `CL_JOIN_REQUEST`, `SV_JOIN_ACCEPT/REJECT/SNAPSHOT/EVENT/PLAYER_LEFT/LEVEL_SEED`.
- Server seeds clients with the per-run dungeon seed in `SV_JOIN_ACCEPT` (along with the current floor + difficulty) so both ends generate the identical level. Hosts and clients regenerate locally — the level itself is never sent over the wire. The dungeon seed is a dedicated **per-run seed** (`m_level.levelSeed`), minted from entropy on `NEW_GAME`, persisted in saves, and folded with floor + difficulty in `startGame` (`dungeonSeed = levelSeed + floor*7919 + difficulty*104729`) — deliberately **isolated from the global `std::rand()`** used by gameplay (loot/procs/spawns) so host and client stay in sync regardless of differing gameplay RNG draws.
- **Client floor transitions are now wired (server-authoritative descent).** `Engine::updateFloorDoor` (`engine_update.cpp`) gates the exit door by role: a `NetRole::CLIENT` never self-descends/regenerates (it returns early — that would desync from the still-on-floor-N host); only `NetRole::NONE` (singleplayer/split-screen) and `NetRole::SERVER` (host) run the local descend. When the host commits a floor change it broadcasts **`SV_LEVEL_SEED (0x15)`** — a 12-byte reliable packet, same layout as `SV_JOIN_ACCEPT`: `PacketHeader`(4) + `floor` u8 + `difficulty` u8 + 2 reserved + `seed` u32 (LE). `Net::broadcastLevelSeed` sends it with the **final** floor/difficulty (covering the floor-50→1 loop where `difficulty++` and `currentFloor=1`); `clientHandlePacket` parses it and fires `Net::OnLevelSeedFn` → `Engine::onLevelSeed` (registered in the `startGame` CLIENT branch). `onLevelSeed` (client-only guard) adopts `(floor, difficulty, seed)` into `m_level`/`m_difficulty`, then drives the client into the same `FLOOR_TRANSITION` → `startGame(GameStart::DESCEND)` path the host uses, regenerating the identical next dungeon. HP/inventory/floor stay server-authoritative via snapshots; this control message only resyncs the LEVEL each side builds. **Descent is host-initiated only** — remote-client-initiated descent (a client requesting a descend at the door) is deferred (would need a small `CL_*` request). Brief residual: between the host's broadcast and the client finishing regen, the host is one floor ahead; snapshots from the new floor are merely buffered (not applied to entities) until the client re-enters `IN_GAME`, so there's no wrong-geometry render.
- Use `PacketWriter`/`PacketReader` (`net/packet.h`) for serialization. `Quantize::*` for bounded floats.
- Register Engine static callbacks via `Net::setOn*` before `Net::poll`.
- **`CL_JOIN_REQUEST` layout = `PacketHeader`(4) + `version` u32(4) + `chosenClass` u8(1) = 9 bytes.** The class byte is the joiner's `PlayerClass` (the Join menu now runs class-selection before connecting; the client calls `Net::setLocalPlayerClass` before `connectToServer`). The server reads it in `serverHandlePacket` and passes it to the join callback; the size guard accepts ≥ 8 bytes (the class byte is optional — `0xFF`/absent ⇒ Warrior). **`Engine::onPlayerJoin` signature is `(u8 slot, u8 classId)`** — the `Net::OnPlayerJoinFn` typedef carries the class; `onPlayerJoin` validates `classId < CLASS_COUNT` (else Warrior) and uses it for `np.playerClass`, health, energy, and the starting-loadout grant. A fresh joining client also mirrors that class's starting loadout locally in the `startGame` CLIENT branch so both ends agree on a new run. **Mid-run inventory replication (a joiner seeing their server-side gear in an in-progress game) is still TODO via `SV_INVENTORY_SYNC` — defined but not dispatched.**
- **Idle-handshake reaping (N12):** a peer that completes the ENet handshake but never sends `CL_JOIN_REQUEST` is dropped after `CONNECTING_TIMEOUT_MS` (5000 ms) by a sweep at the end of `Net::poll` (timestamped via `enet_time_get()` in `NetPlayerSlot.connectTimeMs`, cleared on ACTIVE), so it can't hold a slot until ENet's default timeout. `enet_peer_timeout` is also tightened on connect to reap silently-dead joined peers.

## Conventions

- **Always document code changes.** Every code change must include inline comments explaining non-obvious logic, and the top of each substantive `.cpp` should have a brief block describing what the file is for and how it fits into the systems described in this file. Update CLAUDE.md whenever you change the architecture, add a subsystem, or change a JSON schema.
- **C++17**, no exceptions in hot paths (the JSON loaders catch and log).
- Plain structs for data, enums for types (`enum struct ... : u8` with `COUNT` sentinel where useful), namespaces for systems.
- Static arrays sized by `MAX_*` constants — bump the constant rather than allocating dynamically.
- `u8`/`u16`/`u32`/`f32` aliases from `core/types.h` are used everywhere — avoid raw `int`/`float` in new code.
- All code changes must include inline comments for **non-obvious** logic only — don't re-document what the names already say. The "why" matters, not the "what".
- Keep JSON schema and loader in sync when you add or modify a struct field.
- Prefer `Vec3` math operators in `core/math.h` over manual component arithmetic.
- For new GPU resources, pair `init`/`shutdown` (or `create`/`destroy`) and call them from `Engine::init`/`Engine::shutdown`.
- Profiling: wrap a section with `PROFILE_SCOPE(idx, "name")` from `core/profiler.h`. Indices 0–15.

## Debug Keys (in-game, singleplayer + listen-server host)

| Key | Action |
|---|---|
| `Esc` | Back to menu (in lobby) / quit (in-game) |
| `1`/`2`/`3` | Select weapon (legacy slots; 1=Sword, 2=Pistol, 3=Fireball) |
| `Tab` | Toggle inventory screen (releases mouse) |
| `E` | Pickup nearest world item |
| `Right click` | Activate equipped legendary skill |
| `F1` | Toggle `DebugDraw` overlay (entity AABBs, rays) |
| `F2` | Toggle noclip |
| `F3` | Toggle profiler overlay |
| `F4` | Spawn 10 enemies in a ring around player |
| `F5` | Spawn 50 enemies |
| `F6` | Toggle Switch constraint mode (60 m far plane, 960×540) |
| `F7` | (see `engine.cpp:770` — engine-version-dependent dev toggle) |

## Pitfalls / Gotchas

- **Entity handles vs raw indices.** Always use `EntityHandle` + `handleValid`/`handleGet` for cross-frame references. Indices alone go stale when slots get reused.
- **Item visual resolution timing.** `ItemDef.meshId`/`materialId` are zero until `ItemLoader::resolveVisuals` runs *after* `MaterialSystem::init` and the mesh registry is filled. Don't render items earlier in init.
- **`EnemyRole` is a bitmask, not an enum.** Use `e.enemyRole & EnemyRole::SUMMONER`, not `==`. Bosses can combine multiple roles.
- **Spawn-calm window.** Every floor opens with `GameConst::SPAWN_CALM_SECONDS` (0.4 s) of calm: `m_spawnCalmTimer` (reset in `startGame`, ticked once per frame in `update()`'s `IN_GAME` case) is passed as `spawnCalm` to `EnemyAI::update`. While set, hostiles skip the `IDLE→CHASE` auto-detection (`enemy_ai_states.cpp`) and friendly NPCs hold position instead of marching out (`enemy_ai.cpp`). It ends early when the player fires (`handleWeaponFire` zeroes it). **Damage-driven aggro is never gated** — `Combat::applyDamage` sets `aiState = CHASE` directly, so hitting an enemy always wakes it (and neighbours within 6 m) even mid-calm.
- **Boss loot** is guaranteed in the death callback (`engine_init.cpp`). Mini-bosses drop rare+, major bosses drop legendary. This runs *before* the normal loot path and returns early.
- **Minion shield** requires `entity.minionShield = true` (set at spawn from `BossDef`) AND alive minions with `spawnerIdx == bossIdx`. The check is in `Combat::applyDamage`.
- **Death callback drops loot** via a global `s_engine` pointer (file-scope in `engine.cpp`). If you swap the singleton or move loot-drop, update both.
- **Listen-server host plays as slot 0.** `Engine::onPlayerJoin` is not called for the host; slot 0 is initialised inside `startGame`. Don't put host-init logic in the join callback.
- **Snapshot quantization range** clamps positions to ±128 m. Keep level/grid bounds inside that range or clients see jitter.
- **`assets/config/weapons.json` is shadowed** by the inline `initWeaponTable` (`game/weapon.h`). Editing the JSON alone has no effect; either remove the inline table or load JSON in `Engine::init`.
- **Legendary skills require equipping the right item** — `legendarySkillId` on the equipped weapon's `ItemDef` becomes the player's `activeSkill`; without a matching item, right-click does nothing.
- **OBJ loader expects triangles + per-vertex normals.** Quad meshes will load with garbage normals. Triangulate on export.

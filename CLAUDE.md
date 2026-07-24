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

**Balance lab.** `tests/balance/` holds a repeatable balance model (spec:
`docs/superpowers/specs/2026-07-22-balance-lab-design.md`): typical-equipment player power
(Monte-Carlo through the real `ItemGen`/`BuildScore`/`Inventory` code) vs enemy/boss curves
(the real `GameConst` spawn multipliers) per (difficulty, floor, build cell). Always-on sanity
pins run with the suite; the full CSV report is env-gated:
`BALANCE_REPORT=out.csv ./build/tests/dungeon_tests -tc="*balance report*"`, then
`python3 tools/balance_chart.py out.csv -o out.html` for the chart page. Four single-source
extractions exist FOR the lab — the sustained-DPS cycle (`game/weapon_dps.h`, shared with
`build_score.h`), `Combat::armorMitigation` (inline in `combat.h`), `kClassDefs`
(`game/class_defs.cpp`), and `enemyTierForFloor` (`enemy_def.h`, shared with the spawner) —
re-inlining any of them re-creates the scorer-drift bug the 2026-07-22 loot fixes cleaned up.
Its first run caught a real gap (no non-legendary wand at levels 39-50 → Void Scepter).
Phase 2 (pending): chosen target bands become REQUIREs in `test_balance_lab.cpp` so CI fails
when a content/constant change knocks a floor out of band.

**Assets are GENERATED, not committed** (`assets/meshes/*.obj` is gitignored). The mesh table is `src/engine/asset_manifest.h`; `tools/build_assets.py` **hard-fails** if the engine names a mesh it doesn't generate, so a mesh added to one and not the other can no longer ship as an invisible fallback cube. Adding a mesh means editing **both**. (Full trap: `engine-how-to` → Pitfalls.)

## Architecture

**Data-driven hybrid.** JSON in `assets/config/` defines content (items, affixes, skills, enemies, weapons, materials). C++ systems load defs at startup into fixed-size arrays and consume them at runtime. Asset name strings are resolved to integer IDs (mesh IDs, material IDs) once after init — runtime code never touches strings.

**Pool allocation, no heap in hot paths.** Entities, projectiles, world items, materials, meshes all live in static arrays sized by `MAX_*` constants in their headers. No `new`/`delete` per frame. A 1 MB `FrameAllocator` (`src/core/frame_allocator.h`) is reset each frame for transient scratch.

**Namespace + struct.** Systems are namespaces of free functions (`Combat`, `EntitySystem`, `ItemGen`, `Net`, `Renderer`, ...). State lives in plain structs that callers pass in. No singletons except a couple of file-scope globals (logger, profiler, frame allocator, MaterialSystem table).

**Fixed-timestep dispatch.** `Engine::run` (`src/engine/engine.cpp:417`) accumulates real frame time and steps `update(1/60)` up to 4 times per frame, then renders once with an `alpha` interpolation factor. `update()` switches on `GameState` (`MENU`, `LOBBY_*`, `IN_GAME`), and inside `IN_GAME` switches on `NetRole` (`NONE`/`SERVER`/`CLIENT`) into `singleplayerUpdate` / `serverUpdate` / `clientUpdate`. All three live in `engine.cpp`.

**Split-screen (couch co-op).** Up to `MAX_LOCAL_PLAYERS` (=2) local players. Historically mutually exclusive with networking (`m_splitPlayerCount` forced to 1 when `NetRole != NONE`); **online couch co-op** (two local players sharing one connection) is the deliberate exception, gated by the `m_netCouch` flag — BOTH directions have landed: host-couch (`startCouchGame`, host split-screen + remotes join, slots 0+1 host-local) and client-couch (`beginCouchJoin`, join carries localCount+class2, accept carries slot2, lanes map to server slots via `m_clientNetSlot[]`/`activeNetSlot()`). **Every couch-capable client→server packet carries a target-slot byte the server validates by peer ownership** (`routeToOwnedSlot` in net.cpp — inputs/fire since v6; the whole request family — pickup/drop/pet/meteor/interact/respawn/inventory-sync — since v18). A new client→server request that skips this re-creates the v17 bug where the couch client's P2 had every pickup validated against P1 and credited to P1's inventory. Per-player state in `m_localPlayers[]`/`m_cameras[]`/… is copied into "active aliases" (`m_localPlayer`/`m_camera`/…) by `swapInPlayer(idx)` before each player's `gameUpdate` and back by `swapOutPlayer(idx)`; shared world systems run once per frame in `Engine::tickSharedSystems` after the per-player loop. (Full swap-macro / shared-tick mechanics: `engine-reference`; the per-player-field gotcha: `engine-how-to`.)

**Town hub + account stash.** Beating the Dungeon Engine unlocks an outdoor TOWN (sentinel floor 98, `engine_town.cpp` — deterministic build on the Source-chamber rails; credits roll into it, a cleared save's Continue lands there, its portal opens the Free-Play select, `--town` is the dev door). The **stash** is 5 pages × 48 slots shared by ALL characters: pure logic in `game/stash.h` (tested), storage in `stash.dat` (versioned atomic sidecar — `save_NN.dat` untouched), UI rides the inventory screen (`InventoryUI::stashLayout` single-sources draw + hit-test). It is navigable by **mouse AND controller/keyboard** (`INV_PANEL_STASH` cursor — D-pad/WASD moves, A/E transfers, LB/RB flip pages); the mouse-only version was dead on Switch and for couch P2. Both paths share the withdraw/deposit helpers so they can't diverge.

**Arena mode (PvP).** A main-menu "Arena Mode" starts an FFA deathmatch (first to 10, 3 s auto-respawn)
on a deterministic floor-97 colosseum (`engine_arena.cpp`, town rails; rules in `game/arena.h`,
tested). The layout is a **two-story Quake / MPH Combat Hall** (44×44, 4-fold symmetric): a ground pit
(crates, wall-midpoint jump pads, central 1.5 m tower via four ramps) under TWO dueling 3.0 m
vantages — a **perimeter sniper balcony** you stand on AND walk under (covered arcade beneath
holds the spawn bays; corner slab **stairwells** + pads go up; open inner edge to drop/fire),
and the tower's crown at the same height. Verticality rides three opt-in cell flags —
`CELL_LEDGE` (jump-gated risers), `CELL_JUMPPAD` (launch pads), and **`CELL_PLATFORM`**
(real walk-under second-story slabs: story-selecting collision via
`LevelGridSystem::effectiveFloorHeight`, slab-aware `Raycast::cast` so nothing shoots through a
balcony floor, mesher top/underside/rim quads). All three are deterministic seed-built geometry,
so they replicate in co-op with **no wire change / no PROTOCOL bump**.
**Mostly PvP-only** (enemies never *jump*, so LEDGES stay dead ends for them — but they DO ride **jump pads**: see "Enemies use jump pads" below; boss "arenas" in `engine_spawn.cpp`
use plain walkable tiers so adds follow). Host Arena rides the normal host chain, Local Versus the couch
chain; joiners use plain Join Game (sentinel-floor routing). **All PvP damage flows through `Combat::pvp*` helpers against a registry that is
only populated inside the arena's authoritative tick window** (`arenaBeginPvpWindow`/`arenaEndPvpWindow`) —
each landed hit applies atomically via `Engine::pvpApplyHit` (fresh remote-view seed → `applyDamageToPlayer`
→ writeback), so blocking/perfect-block/armor work vs players and PvE never pays more than one branch. A new
player-facing damage source MUST call a `Combat::pvp*` helper beside its entity query or it silently does
nothing in PvP. The arena is a progression firewall: no XP, no loot, no drops, **no saves** (`saveCharacter`
hard-refuses in-arena — a save would stamp floor 97 into the header). `PROTOCOL_VERSION` 20
(ARENA_KILL/ARENA_SCORES/ARENA_OVER events); 21 adds player-facing CC (see Crowd control below). Dev
doors: `--arena` (host), `--arena-couch`. **Worlds entered
WITHOUT `startGame`** (arena, town cleared-Continue, sentinel joins) **must wire net callbacks via
`wireServerNet()`/`wireClientNet()`** — extracted from `startGame` precisely because those paths used to
leave hosts unable to seat joiners and joining clients deaf. **They must ALSO seed the host's own
NetPlayer slot** (`m_players[activeNetSlot()]` — `active`/`slotIndex`/HP/class/moveSpeed): `startGame`
is the only path that does it (`onPlayerJoin` refuses slot 0 by design), so a Continue host reaching the
arena via `enterArena` used to leave slot 0 a default `NetPlayer{}` — the pad-seating loop (gated on
`active`) skipped it and the first frame's `syncNetPlayerToLocalPlayer` stamped `{0,0,0}`/health-100 over
the pad, wedging the host in the corner wall (the "arena spawns out of bounds" bug; intermittent because a
prior `startGame` in the same process leaves the slot active). The **Steam server browser** advertises
floor 97 while `m_level.inArena` so the lobby row reads "Arena" — `enterArena` re-publishes on entry
(`updateSteamLobbyRoster`) since it bypasses the usual `FLOOR_TRANSITION` republish.

**Two-story PvE floors (`VERTICAL_HALL` — "The Stacked Loop").** A 5th structural layout style
(`world/level_gen.cpp` `carveVerticalHall`, weighted-rolled on **floor-6+ non-boss** floors, forced to a
**52-grid** by `startGame`; `--vhall` dev door). NOT a single arena — a Quake **location-based** topology: a **LOOP of nine
distinct areas** laid out 3×3 and stacked across two stories, circled by a route that spirals up and down.
The **four CORNERS** are ground rooms (floor 0); the **four MID-SIDES** are **BALCONIES**
(`CELL_PLATFORM` slabs @ 3 m — walk ON top, walk UNDER the arcade beneath); the **CENTRE** is an open
**VOID** (ground, no balcony) that every balcony overlooks and drops into (the Quake sightline + a
cross-level shortcut). The ground story carries **INTERIOR WALLS** (all slab-guarded, so a wall can
never rise through a balcony/ramp/catwalk cell — where a run crosses under a ramp tail the skipped
cells become 3 m **archways**): **DOORWAY walls** with a rolled 3-wide door on the 8 corner↔arcade
seams turn the open bands into rooms you enter through doors, and short free-standing **COVER runs**
(plus the original pillars) break the corner/void sightlines — the arcade↔void seams stay open (the
overlook IS the floor), and the arcades can't take walls by construction (every arcade cell carries
the balcony slab). Sealing is impossible-by-test, not by hope: the room-CENTRE clear opens a full
**3×3** (a single-cell clear once left a centre open but walled in — a 1-cell pocket), and
`test_vertical_hall.cpp` BFS-floods endpoint→all 9 centres + all 4 ramp feet across 64 seeds × both
grid sizes. A **PINWHEEL of four RAMPS** climbs each corner up to the next balcony (graduated
slab; the enemies' chase-up route and yours after a drop). **Each ramp runs to its band's edge**
(no fixed length — the old `VH_RAMP=12` was sized for the 44-grid's 14-cell bands, so after the
44→52 bump every ramp top hung 2-4 cells short of its balcony: a 3 m dead-end in the air, caught by
the ramp-top flood test). The exit balcony is always served by the
ramp at the **DIAGONAL corner** (`mSel = (cSel+2)%4` — the old either-far-mid roll could put the
serving ramp one band from spawn, collapsing the loop to a 5-second walk); two **CATWALKS** cross the void @ 3 m linking
opposite balconies — one **INTACT** (the high road), one **BROKEN** with a 2-cell **jump** gap — so the
UPPER story is its own connected loop that crosses at the centre; **ONE spawn-side JUMP-PAD** in the
void flings you back up (player-only — two pads under the catwalk crossing were a pad→catwalk→exit
taxi that skipped the loop; the single pad is recovery near the START, the ring still gets walked). Circle the ring and you continuously **ASCEND** (corner→ramp→balcony) and **DESCEND**
(balcony→drop→void); the exit sits on the FAR side AND the opposite STORY, so every floor forces a full
traversal and a level change (a coin-flip picks ascend vs descend). The **lower story is one fully-connected
floor** (corners + void + arcades → reachability guaranteed); the upper story hangs off it via the ramps +
catwalks. `spawnBalconyPos`/`exitBalconyPos` are explicit positions applied in `startGame` (upper = y 3 m,
ground = y 0; a `clearPad` opens any wall/pillar that rolled onto a ground endpoint). **Both stories are
densely held**: `spawnFloorNests` seats 2-3 ranged snipers PLUS 2 melee guards per balcony on
slab-VERIFIED ring-searched seats (blind offsets could land off the band-edge ramp top and dump the
spawn to the ground story), skipping the one portal nearest the spawn endpoint (a pack seated at the
spawn balcony's own ramp top face-camped the spawn — measured 150→46 HP in seconds); ground enemies
spawn via `spawnFloorEnemies` with the style opted OUT of the spawn-neighbour shield + raised per-area
caps (the nine 16-18 m areas are all bbox-adjacent, so the generic skip-neighbours/halve-2-hops rule
hollowed the floor to ~15 enemies; now ~85 total, doors + distance are the shield) and chase UP the ramps.
Enemies traverse both stories via story-aware `snapEntityToFloor` (`effectiveFloorHeight` not raw
`getFloorHeight`) + the ramp **`StoryPortal`** CHASE routing (`world/story_nav.h`, `DungeonResult.portals`;
`nullptr`/portal-count-0 inert elsewhere). Slab/ramp/collision primitives pinned by
`tests/world/test_platform.cpp`, the layout invariants by `tests/world/test_vertical_hall.cpp` (44 AND
52); seed-built + server-authoritative → **no wire/save change**. Walls put worst-case draw calls at
~455-480 on the 52-grid (was ~320-345) — inside the 500 budget, but the Switch number is unmeasured. (Earlier "Stacked Hall" split-pit design lives in
`scratchpad/stacked_hall_geometry.cpp` for a possible 2nd PvP arena.) Design/plan:
`docs/superpowers/plans/2026-07-20-two-story-vertical-hall-pve.md`.

**Four-story PvE floors (`FOUR_STORY` — "The Descent").** A 6th structural layout style (`world/level_gen.cpp` `carveFourStory`, forced to a 44-grid; `--fourstory` dev door). It is **SCHEDULED, not rolled**: every floor ending in **9** (9/19/29/39/49) is a Descent maze and no other floor ever is, so it lands as a predictable landmark in a run rather than a surprise — none of those are boss floors, so there is no clash, and `pickLayoutStyle` returns it before the weight table, which no longer carries a FOUR_STORY column. A **MAZE stacked four stories deep on one
footprint**: the L0 ground plus three `CELL_PLATFORM` slab stories at 3/6/9 m. A braided
recursive-backtracker maze (3-wide corridors, 1-cell walls, ~34% wall bulk) is carved ONCE as
full-height `CELL_SOLID` and **shared by all four stories**, so every level is the same labyrinth
re-read at a new height (braiding reopens ~1 in 5 walls — a perfect maze has one route between any two
points, which reads as tedious and gives combat nowhere to flow). You spawn on **L3 in one corner** and
must reach the **L0 exit diagonally opposite**; the only way down is through the floor, and the split
is derived from the movement physics rather than tuned by feel — at the 6 m/s base speed a jump reaches
2.4 m and a 0.6 m body needs gap+0.6 of clearance, so:
**DROP HOLES** (≥2 cells across) **cannot** be cleared → a committed one-story fall;
**JUMP GAPS** (exactly 1 cell) can → clear them or lose a story;
**JUMP PADS** sit under ~1 in 3 drop holes as RETURN LIFTS — fall through, land a story down, get flung back up through the hole you came from (a pad fires the instant you are grounded, so a pad under *every* hole would bounce you straight back and make descending a fight with the level; the rest stay clean descents). They also fill dead-end nodes (the whole 3x3, not a centre cell — a 1x1 pad is unspottable down a dark corridor) and lift ~two stories so a bad fall is recoverable; the launch is capped to real headroom so a pad can never throw you through the ceiling.
There are no ramps or stairs — `portalCount` stays 0 and descent is **one-way**. Hole density **thins
with depth** (18/12/7%): the top story hands out ways down, the last one makes you hunt.
**Express shafts are impossible by construction** — a hole is punched at level L only where the slab at
L+1 is intact, so no column is ever open through two stories; the grid itself is the ledger (a holed
cell simply has no slab at that height), so the rule costs no memory and can't drift from the geometry.
`spawnFloorHoleSnipers` seats ranged enemies on intact-slab ledges overlooking each hole (the seat is
SEARCHED, not offset — on a labyrinth a fixed "+X of the hole" buries them in walls), and
`spawnFloorEnemies` rejects any candidate cell whose `effectiveFloorHeight` at the room story isn't
within `PLATFORM_STEP_TOLERANCE` of it, so nothing seeds over a hole and instantly snaps down.
Built on the multi-slab foundation (up to `MAX_PLATFORMS_PER_CELL`=3 slabs per cell → 4 walkable
stories); seed-built + server-authoritative → **no wire/save change from the geometry itself**. The
floor DID force `MAX_ENTITIES` 128→192 (**`PROTOCOL_VERSION` 24**) because four stories want ~4x a flat
floor's population and 128 silently starved decorations/NPCs/adds/summons. Invariants pinned by
`tests/world/test_four_story.cpp` — including that the floor **is a maze at all** (wall bulk 20–60%),
the guard that would have caught the open-plain first draft. Design/plan:
`docs/superpowers/plans/2026-07-21-four-story-descent-floor.md`.

**Enemies use jump pads to climb.** Ground enemies have no vertical physics — they are hard-snapped
to the floor every frame — so following a player UP a story needed a real ballistic arc. Two halves:
**physics** (`entityMoveAndSlide`) launches a grounded enemy off a `CELL_JUMPPAD` and then integrates
gravity, moving Y freely until the feet reach the surface `effectiveFloorHeight` picks for the height
they have fallen to (so they land on the slab they were thrown onto, not the ground); and **routing**
(`StoryNav::nearestPadGoal` over `DungeonResult.jumpPads[]`, recorded by the generator like
`StoryPortal` is) sends a chaser to the nearest pad when its target is a storey up. This is what makes
the Descent's cross-story chase work at all — those floors have `portalCount == 0` (no ramps), so a
pad is the ONLY way up and without it an enemy loses anyone who drops a level.
`velocity.y != 0` on a non-flying entity IS the airborne state (flags are full; knockback is XZ-only),
and **`snapEntityToFloor` returns early for such an entity** — it is called from half a dozen places
per frame and any one of them would otherwise cancel the arc. The launch fires only when the target is
genuinely **above** (`StoryNav::targetIsAbove`, 1.5 m so a crate doesn't count), using the chase
target `entityMoveAndSlide` was already passed and ignoring: firing on mere contact turns a pad into a
popcorn machine, since an idle enemy has no air control, lands on the same pad and bounces forever.
Physics is universal (any enemy on any pad with upward intent), routing is opt-in per style via the
recorded pads — the Stacked Loop deliberately records none, keeping its void pads the player's
shortcut off the ramps. **They also VAULT jump gaps**: a grounded chaser about to step into a gap
probes `StoryNav::planVault` (pure, tested — a drop one cell ahead AND a same-height landing within
`VAULT_MAX_CELLS`) and, if viable, leaps with a fixed `VAULT_SPEED` lunge (6 m/s — own walk speed
would drop the median 3.5 m/s enemy short of the ~1.6 m a 1-cell gap needs); if a gap is ahead but
un-landable (a lake, a real drop) it REFUSES the step instead of suiciding off the lip, unless the
target is genuinely below. While airborne over a gap the horizontal speed is floored at `VAULT_SPEED`
— the FSM rewrites `e.velocity` every frame, which would otherwise strip the lunge mid-arc. In
`entityMoveAndSlide` these grounded checks are SEQUENTIAL, not an else-if chain off the pad branch:
the first cut consumed the chain slot and silently broke pad launches for enemies whose target was
above. Verified live on floor 9: 72 vaults + 28 pad launches in 30 s, and 0/0 on a flat floor.

**Hellforge LAVA floors (a FEW of 31-40).** `LevelGen::isLavaFloor(levelSeed, floor)` picks a few of the tier's floors (seed-derived, integer-only, so host and client agree — it changes GEOMETRY); the rest stay stone. **Never a BOSS floor** (35 and 40): `spawnFloorBoss` expands a room into an arena and rebuilds the mesh *after* the theme pass poured the lava, so the two would fight over the same cells, and a milestone fight staged in a lava sea is not the encounter that was designed. x9 floors are Descent mazes and stacked styles are excluded too, so in practice the eligible floors are 31-34 and 36-38. `--lava` forces it on any 31-40 floor, and because the theme skips stacked styles it also forces a flat style so the door can't silently no-op. On a molten floor the walls MELT. `applyLavaTheme` (engine_startgame.cpp,
run in the theme block after the carve) turns every INTERIOR `CELL_SOLID` cell into a **walkable**
`CELL_LAVA` surface, so a Hellforge floor reads as islands of stone in a molten sea with **no
sightline blockers left** — a deliberate trade of readable cover for drama. The outer ring stays
solid (it is the only thing between the player and walking off the map). Lava **burns the player
only** (`LAVA_DPS` 45/s in `engine_update_player.cpp`, via `applyDamageToPlayer` so armour/i-frames
and hit feedback all behave; no `attackerPos`, so it can't be blocked or knock you back) **and SETS
YOU ON FIRE** — it refreshes the standard `burnTimer`/`burnDps` status (3 s @ 15/s, `fmaxf` so
standing in it can't stack an unbounded timer), so contact damage stops when you leave but the
burn does not: a botched jump costs ~45 more over the next 3 s. Riding the existing status means
the HUD row, the i-frame clear and the snapshot replication all come for free — **monsters
are immune and wade through to flank you**, which is the whole asymmetry of the tier. Damage is gated
on `LevelGridSystem::feetInLava` (cell is lava AND feet at/below the surface), so being **airborne
over it is free**: a 1-cell vein is clearable (1 m needs 1.6 m of the 2.4 m jump reach) while a wide
lake is not. Melting alone leaves ~95% of the lava as impassable LAKE (measured), so the pass also
lays **stepping-stone causeways** — dashed stone bridges every 7 cells with 1-cell hops — turning the
sea into a network of optional shortcuts you can SEE before you commit. Spawn and exit both get a
cleared stone pad (the exit's forces a mesh rebuild, since it is positioned after the mesh is built).
The minimap paints lava hot orange in its own branch **before** the floor branch — lava carries
`CELL_FLOOR`, so without it the map would show a lake as safe grey. **Stacked-slab styles
(VERTICAL_HALL / FOUR_STORY) are excluded**: their slabs exist only on non-solid cells, so melting the
walls would punch a hole down to lava through every upper story — a different (possibly great, but
untested) level design. Pinned by `tests/world/test_lava.cpp`; grid-derived from floor+seed, so it
replicates with **no wire or save change**.

**Jump-pad strength is per-cell data.** `GridCell::jumpPadQ` (launch speed in quarter m/s, 0 = the
global `JUMPPAD_LAUNCH`) so a map can author pads stronger than the default without rebalancing every
map that already exists — the Arena and VERTICAL_HALL size their 3 m balconies to the default 3.6 m
apex, and a global buff would fling players clean over them. `Collision::jumpPadSpeed` returns the
speed (0 = not on a pad) and is **story-aware** (`effectiveFloorHeight`, not `getFloorHeight` — with
the base-floor read a pad on ANY slab story could never fire).

**Crowd control (CC-Resistance + player stun).** All player crowd control (stun/slow/freeze — poison/burn/
curse are damage, not CC) routes through ONE choke, `Combat::applyCCToPlayer`, wrapping the pure
`CrowdControl::resolveCC` (`game/crowd_control.h`, tested): a **perfect dodge (any roll's i-frames) or a
perfect block ALWAYS negates** incoming CC (universal — every class, PvE + PvP; both are timing feats, always
rewarded) → else **tenacity** (`player.ccResist`) → **PvP-only stun diminishing returns** (100→50→25%→immune
within 8 s, per-victim, so two CC classes can't perma-lock). A direct player CC-timer write bypasses resist + DR — always call the
choke. **CC Resistance** is one unified affix (`AffixType::CC_RESIST`, "of Steadfastness"/"of Footing"),
**summed on demand** (`Inventory::ccResist`, no cached field → no save bump, the armor/thorns pattern),
capped 60%, stamped into the transient `Player.ccResist` (re-stamped authoritatively in `pvpApplyHit`
from `m_inventories[slot]`). **Player stun is PvP-only** (the Arena; enemies never hard-stun you)
and is *action-lock, camera-free* — movement/fire/class+helmet skills/dodge suppressed both locally and on
the wire (`captureLocalInput`), sparing BOOT_SKILL (Break Free) + inventory; replicated like
`shadowDanceTimer` (NetPlayer field + seed/writeback + `SnapPlayer.flags` bit2 + `stunTimerQ` on the reused
`reserved0` byte) so a stunned client predicts its own lock and reconciles with no rubber-band. The
**Steadfast Greaves** (legendary boots, `SkillId::BREAK_FREE` on the F/`BOOT_SKILL` rail) add the
boots-only escapes (dodge WHILE stunned + clear already-active CC on the roll — `ccDodgeImmune`; the plain
i-frame negate is universal) plus an F cleanse (they roll CC-resist affixes like any boots — no forced
stat). **PvP block** is energy-drained (no turtle) with NO perfect-block cooldown (a perfect block is always
rewarded); a held block stops damage but the CC lands. Class CC (baseline-up fairness): Ranger Barrage slow-zone, Marksman Explosive knockback+stagger,
Tinkerer Detonate EMP stun, Wanderer Deflect stagger — each with a `Combat::pvp*` twin. `PROTOCOL_VERSION`
21. (Internals: `engine-reference`; the pitfalls: `engine-how-to`.)

**Auto Loot & Equip (the second play style).** Chosen at character creation — after class select
BOTH players get a "How do you want to play?" chooser (P1 subState 23, couch P2 24 on their own pad;
21/22 were taken by lobby-code entry and the Arena chooser) — and toggleable any time from the
inventory screen. **Classic** is the game as it was. **Auto** vacuums nearby loot (2.5 m, the
interact vertical bound so it never pulls through a floor; sentinels keep their own flows; CLIENT
lanes ride the server-validated `CL_PICKUP_ITEM`, so no wire change) and wears anything the selected
build scores as an upgrade. The build is one cell of a **3x3 grid** in the inventory (rows
Tanky/Moderate/Glass Cannon, cols Magic/Melee/Ranged; `InventoryUI::buildGridLayout` single-sources
draw + hit-test; controller reaches it as `INV_PANEL_BUILD` in the shoulder cycle — STASH moved to
panel id 5). Scoring is the pure, tested `game/build_score.h`: stat-derived (base stats + rolled
affixes — no authored tags), **weapons scored on SUSTAINED DPS, mirroring `getEffectiveWeapon`** (cooldown divided by attack
speed AND cut by CDR — the engine applies CDR to the weapon swing; clip weapons pay the reload
cycle `shots*cd + reload`, which reload%/clip% rolls buy back — a Pistol's sustained output is ~29%
under its burst; projectile weapons credit projectile-speed rolls as hit reliability; per-hit had
ranked a Heavy Crossbow 3.5x a Rusty Dagger whose real DPS is higher), hard weapon-family gate per column, **defense in EFFECTIVE-HP terms** (the engine's armor formula linearizes to +armor% of a 150-HP reference pool; %HP likewise; regen/life-on-hit/**lifesteal count as TANKINESS** — healing over a 10 s reference fight — not offense), **spell rolls + cooldown reduction multiply a per-column reference cast-DPS** (70 for Magic, 15 elsewhere — CDR is 1/(1-c) casts, real DPS on a caster), 1:3 / 1.5:1.5 / 3:1
offense:defense row weights, 5% upgrade hysteresis, rarity tiebreak. Selecting a cell re-gears the
whole bag on the spot (`autoEquipBackpack`); every auto-equip goes through `sendInventorySync` (the
v16 couch lesson) and announces itself in chat (silent gear changes read as items vanishing). The inventory reasons over **all nine builds**, not just the active one:
loot is only picked up if it would improve some build's best-fieldable gear (`worthPickingUp` —
worse and near-duplicate loot stays on the ground), a slow housekeeping pass (5 s, and after every
pickup) **discards bag items dominated for every build** (`isKeeper`, the >=-flavoured twin of the
pickup filter — the asymmetry is what prevents keep/drop churn), and when another cell's achievable
total (`bestBuildCell`/`gearScoreForCell`) beats the active build by >10% the player gets a one-line
chat nudge ("Better gear for Tanky Ranged…"), cooldown-limited and re-armed only when the suggestion
changes. **Row weights all sum to 4** — this is load-bearing for the nudge, which compares totals
ACROSS cells; Moderate at 1.5/1.5 (sum 3) made the middle row lose by construction (measured 2x
artifact on the starting loadout). **Minipets bypass the scorer entirely**: a `petSummon` def is
always `worthPickingUp`, always a keeper, and **scores 0 as gear** — the last part matters, because
its LEGENDARY rarity otherwise leaked a phantom tiebreak score into the RING column (a bag pet was
"the best fieldable ring" for a fresh character, suppressing real ring pickups). A genuinely full
bag **evicts its lowest max-over-all-cells item**
(Aaron's call — never pauses; pets + quickbar-assigned gear exempt, like "drop all"). Persisted as `PlayerInventory.autoMode`/`buildCell` — **SAVE_VERSION 4**
(v3 readable via mirror; pre-v4 characters load as classic). EVERY `Inventory::init` on a lane that
already chose (NEW_GAME wipe, `equipFreshLane`, the client-join wipe) explicitly preserves the two
fields — the chooser runs BEFORE the wipes, which would otherwise silently undo it. `--autoloot` is
the dev door. Inert in the arena. Spec: `docs/superpowers/specs/2026-07-22-auto-loot-equip-design.md`.

**Unfocused = no input, but the game keeps running.** The window can sit on a second screen playing
itself while the player works in another app. `Window::pollEvents()` pushes SDL's
`SDL_WINDOW_INPUT_FOCUS` flag into `Input::setWindowFocused()` once per frame (polled, not latched off
FOCUS_GAINED/LOST — a missed event then can't strand the gate, and nothing needs seeding at startup);
the rules themselves are pure + unit-tested in `platform/input_focus.h`. While unfocused,
`Input::update` zeroes the keyboard/mouse-button snapshots and the mouse delta, which is the ONE choke
that kills `isActionDown`/`isActionPressed`, `isKey*`, `isMouseButton*`, `getMouseDelta` **and** the
`humanActivityThisFrame` latch together — and relative mouse mode is released so the cursor belongs to
the desktop again. That last part is the actual bug, not a nicety: SDL's X11 `XI_RawMotion` handler
gates only on `mouse->relative_mode` and **never on focus**, so an unfocused game kept eating raw
pointer motion from the whole desktop, which fed the aim AND tripped Autoplay's takeover latch — the
bot handed control to a "human" who was typing somewhere else and the game stood still (measured: 482
benched ticks in an 8 s tab-out). `setRelativeMouseMode`/`setCursorVisible` now record what the GAME
wants and `applyMouseMode()` pushes `want && focused` to SDL, so focusing again restores exactly the
mode the current screen asked for (gameplay yes, menu no). Pending delta is dropped on BOTH edges so
a cursor journey made in another app can't snap the aim on the way back in. **Not gated:** the
Autoplay bot overlay (it is OR'd in ahead of the device read — the bot must keep playing), the frame
loop (no focus pause/throttle: 60 FPS unfocused, verified), and **gamepads** (SDL reads pads as
background devices and picking one up is unambiguous intent to play). Fails OPEN until focus has been
seen once, so a headless/no-WM X server can't leave the game input-dead. Watch it beside other apps in
**Windowed** or **Borderless** (Options → Display); exclusive **Fullscreen** is the one mode that isn't
meant for this.

**Autoplay mode (AFK bot).** A main-menu "Autoplay" row and the `--autoplay` dev door start a
**singleplayer, lane-0-only** run (v1) where a bot plays a full character: navigate, fight per the
build doctrine, loot (it force-enables **Auto Loot & Equip** as its gear brain), descend, auto-respawn,
and ladder difficulty — all through the **exact human input path**. The seam is a synthetic-input
overlay: `Input::setBotHeld(action,on)` arms a per-`GameAction` bit that `checkActionRaw` OR's into
`isActionDown`/`isActionPressed` (input.cpp) ABOVE the real-device read, so every consumer (movement,
fire, skills, potion, block, dodge, interact) is driven with zero call-site changes and a real keypress
overrides on the same frame. `Input::humanActivityThisFrame()` is the takeover trigger — real gameplay
activity hands control to the human instantly and the bot resumes after `AutoplayControl::RESUME_SECONDS`
(2 s) idle (UI navigation never counts). The **decision core is pure and unit-tested** (`src/game/autoplay_*`:
`bot_input` overlay bitset, `control` takeover latch, `doctrine` build-cell→playstyle table, `nav` hazard
veto + descend gate, `combat` target/aim/fire/kite, `intent` BotView/BotIntent structs, `brain` the
survive>fight>descend>travel priority machine) — engine-free so it tests on hand-built `BotView`s. The
**engine driver** (`engine_autoplay.cpp`) is the only place it touches live state: once per tick
`updateAutoplay` runs the takeover latch, then `buildBotView` snapshots player/weapon/nav-flow/hostiles,
then `Autoplay::decide` returns a `BotIntent` that `applyBotIntent` maps back onto held `GameAction`s + a
yaw/pitch write. **Freeze carve-out** (`botMayAct()`): the bot keeps FIGHTING under an open inventory (so
you can re-gear mid-fight — the whole point), but pause / character-inspect / options / menagerie freeze
it; movement + interact are suppressed while ANY UI is open so a moving bot can't jitter the inventory
cursor. **Descend seam:** the bot HOLDS `GameAction::PICKUP` through the real interact arbitration
(`updatePlayerPickup` → `updateFloorDoor`) — a direct `m_descendRequested` write is erased because
`updatePlayerPickup` re-derives that flag from the button's tap/hold each tick. Because a continuous hold
is consumed ONCE (`Interact::poll` latches `consumed`) and a HOLD reaches a **shrine sharing the exit's
interact range** BEFORE the exit, the driver **PULSES** PICKUP (`Autoplay::descendPulseHeld`, `autoplay_nav.h`:
hold >0.35 s to fire, release a beat to clear the latch, repeat) so one cycle spends the shrine and the next
descends — a plain continuous hold wedged the bot next to a used shrine forever. **TOWN portal:** the hub is
the one world the brain cannot express — no floor door (so `onNormalFloor` is false and `decide` returns an
empty intent) and a flow field aimed at the PLAZA CENTRE, not the portal — so an AFK run used to park there
forever. The DRIVER owns it (`autoplayTownStep`, gated on `m_level.inTown`; the ARENA and the SOURCE CHAMBER
still idle): a pure `Autoplay::planTownPortal` (`autoplay_nav.h`, tested) beelines XZ at `townPortalPos`
through the same hazard veto + ±45°/±90° fan, STOPS at 1.5 m (inside the 2 m trigger — the portal is a HOLD
target, and walking onto its centre at 6 m/s blows through the window exactly as the floor door did), and
takes it with the SAME `descendPulseHeld` pulse for the same reason (a held PICKUP fires once and the plaza's
stash chest can eat it). A **CLEARED** hero's portal opens the Free-Play select and leaves `IN_GAME` — where
the driver does not tick — so `updateTownPortal` arms a one-shot `m_autoplayFreePlayTimer` (0.75 s) that
makes the subState-14 handler run its OWN confirm body (calling the path, not synthesizing MENU_CONFIRM,
which a menu edge/repeat could swallow) on whatever the screen already shows; it disarms the instant a human
holds control or touches the screen. A mid-run hero's portal needs none of that — it goes straight to
`startGame(CONTINUE)`. Measured live: ~0.55 s from town arrival to portal taken, both branches. The **build doctrine**
(`doctrineFor`) turns the Auto-Loot 3×3 build cell into a playstyle: the column sets the engagement band
(×weaponRange) and the row sets risk posture (potion threshold, block vs proactive-dodge, cover, high-ground).
A FRESH Autoplay hero **seeds that cell's column from its CLASS** (`Autoplay::defaultCellForClass`,
`autoplay_doctrine.h`: Magic / Melee / Ranged, Moderate row) — `PlayerInventory`'s untouched default is
Moderate/**Melee** for everyone, so Auto-Equip used to put a sword on a Sorcerer and the bot played melee
with a caster. The table is explicit, not derived from `ClassDef::preferredWeapon` (which cannot separate
Magic from Ranged — Sorcerer and Ranger are both `PROJECTILE`); each entry matches the family
`BuildScore::weaponInFamily` puts that class's STARTING weapon in, so the first auto-equip pass keeps the
weapon the class was born with. Only a **fresh** character is seeded (`enterAutoplayRun(freshCharacter)`,
threaded from `GameStart::NEW_GAME` / `!m_menu.p1Continue`) — a Continue keeps the cell its player chose in
the inventory grid, and a re-seed there would silently re-gear their hero.
**Target LOS is WORLD-ONLY** (`Raycast::cast`, the slab-aware grid DDA the melee cone's LOS gate and the
enemy AI's `hasLOSToPoint` already use) — NOT `CombatQuery::raycast`, which sweeps world AND entities and
returns the NEAREST hit. Reading "the nearest hit wasn't WORLD" as a clear line is wrong the moment another
enemy stands between the bot and an occluding wall: the nearest hit becomes an ENTITY, the wall stops
counting as an occluder, and the bot "sees" — and shoots — straight through it (measured ~1600 target-ticks
of that per 2-minute run). Bodies must never make geometry disappear; whether an intervening enemy should
block the SHOT is a separate question, and the answer is no (the projectile just hits it). The cast also
runs in a SECOND pass over the nearest-16 survivors, so a 90-enemy floor pays 16 casts, not 90.
The FIGHT branch only engages within an **engagement ceiling** `max(engageMax×weaponRange, THREAT_RADIUS=12 m)`
— a target beyond it falls through to DESCEND/TRAVEL so a distant straggler can't drag the bot off the
exit route (this was the dense-`VERTICAL_HALL`-floor stall: an unbounded FIGHT chased 16-21 m foes forever).
**`engageMin` governs MOVEMENT ONLY, never fire**: the bot shoots anything with LOS inside
`engageMax×weaponRange` *including* what is inside its kite floor, and backs away at the same time —
that is what kiting IS. Gating fire on the full band made a swarmed caster/ranged bot backpedal forever
without shooting. That fix needs a real range to work at all, so `buildBotView` runs the weapon through
**`Autoplay::botWeaponRange`**: melee/hitscan use their authored `baseRange`, but **every PROJECTILE
weapon in items.json authors NO range** (it carries a projectile SPEED instead — the shot flies until it
hits or its 3 s lifetime expires), so the raw 0 multiplied the whole doctrine band to zero and NO wand or
bow could ever fire — the other half of the "sorcerers stuck on floor 1" bug. Projectile range is derived
as `speed × 3 s`, capped at 24 m (2× THREAT_RADIUS; an uncapped 29 m/s bolt would demand a 47 m kite floor
in a 15 m room). The bot also **CASTS ITS CLASS SKILLS**: `buildBotView` fills `BotView.castableSkill[4]`
by mirroring the real activation gates one for one (slot holds a skill / unlocked at the EFFECTIVE floor /
energy pool covers the cost — health for `BLOOD_NOVA` / `GameConst::cooldownReady` on the slot's tick
watermark), and `decideCombat` presses the lowest castable slot whenever it is engaging, so a Magic build
plays its build instead of poking with a wand. Availability is mirrored rather than guessed precisely
because a press that no-ops is worse than no press. The **EQUIPMENT legendary rails ride the same
contract**: `BotView.bootCastable` / `helmetCastable` mirror `handleEquipmentSkillActivation`'s gates
(the slot is BOUND to a skill — i.e. a LEGENDARY is equipped there — the shared pool covers the cost, the
tick cooldown has elapsed; the helmet is stun-gated and the boots deliberately are NOT, because
`BOOT_SKILL` is the Break Free rail and escaping a stun is its purpose), and `decideCombat` presses both
whenever it is engaging. `BotIntent` carried these two flags and the driver had them wired to
`GameAction::BOOT_SKILL`/`HELMET_SKILL` from day one, but **nothing ever set them** — the bot wore its
legendaries and never once cast them. The bind is written later in the same tick, so the view reads last
tick's value: one tick of lag on the frame a legendary is equipped, which can only ever cast late, never
wrongly.
**Combat FEEL** rests on three rules that keep the bot from reading as a machine. (1) **Aim is EASED and
RATE-LIMITED, never snapped.** `decideCombat` still emits the DESIRED lead-corrected aim; `applyBotIntent`
eases the player onto it with `Autoplay::stepAngle` (pure/tested — shortest arc across the ±π seam, and
`fmodf`-folded because the engine never re-wraps `Player::yaw`), plus a sub-degree deterministic
`aimWobble` (tick-driven sinusoids, never `rand()`). Two limits stack, and BOTH halves matter: an
error-**PROPORTIONAL** approach (gain 6/s, integrated as `1-exp(-gain*dt)` so the curve is identical at
any tick rate) makes the crosshair DECELERATE as it converges, under a two-point **RATE CAP** (2.8 rad/s
fine tracking / 5.6 rad/s for a >1 rad acquisition flick) that governs the far field. The first pass was
a hard cap alone at 7/14 rad/s and still read as an aimbot — a constant-velocity sweep that stops dead on
arrival is a machine signature no matter how slow you make it, which is why the ease is not optional.
The gain also sets the steady-state **tracking lag** (lag = target's angular rate / gain) — that is what
the smoothness costs. Measured live (3 seeds × 120 s, fixed level seed): peak yaw delta 1.13 rad/tick
snapped → 0.233 rate-capped → **0.093** eased; a 10° correction now takes ~0.4 s of shrinking steps
instead of one 0.093 rad step and a stop. The honest trade: vs the 7/14 pass, Marksman kills 31.7→23.0
and Warrior 71.0→57.0 per 2 min, floors reached 4.3→4.0 and 8.3→7.3 — ~20-27% fewer kills and ~1 floor,
deliberately accepted for the look. Raising the gain to 9 does NOT buy it back (measured: a wash), so the
cost lives in the caps, not the ease. **The ease forces a FIRE GATE**, and it is not optional either:
`decideCombat` decides `fire` from the DESIRED aim, so with a lagging crosshair the bot pulled the trigger
mid-turn and sprayed every wall it was sweeping across ("ranged is shooting through walls" — 22% of its
shots had geometry between muzzle and target, at a mean yaw error of **0.47 rad / 27°**). `applyBotIntent`
therefore re-checks the ACTUAL (post-step) aim against the intent with `Autoplay::aimOnTarget` and holds
FIRE until the crosshair has arrived — measured 22% → **4.7%**, with shots at an OCCLUDED target going to
exactly **0** and kills/floors unchanged. The tolerance has a floor and a ceiling and both are load-bearing:
it must exceed the ease's steady-state tracking lag plus the wobble (0.4 rad/s ÷ gain 6 + 0.011 ≈ 0.078)
or a strafing enemy MUTES the bot, hence `FIRE_ALIGN_RAD` 0.09; and **melee needs its own, looser one**
(`FIRE_ALIGN_MELEE_RAD` 0.45, pitch ignored) because a swing is a 70° cone judged HORIZONTALLY
(`queryConeSorted`'s `horizontalCone`), so a melee bot waiting for pinpoint alignment would stand there
not swinging (live: 97.5% of wanted swings still fire). (2) **A ranged enemy it is closing on gets CHARGED with a roll** — ~4 m of travel plus 0.3 s of
i-frames crosses the firing lane better than walking. `out.moveFwd` is forced on the same tick because
`computeRollDirection` reads the WASD held THAT tick, and the roll is gated on already facing the target
(the roll uses the CURRENT yaw, which now lags). Melee enemies are excluded — they close the gap for you,
so spending the roll wastes the i-frames you want when they arrive. (3) **Block is a TAP timed into the
perfect window, never a hold.** `Combat::classifyBlock` grades by hold time, so a held block only ever
earns BLOCKED (0.5×) while paying 0.4× move speed; `Autoplay::swingIsLanding` raises it only when a
**melee** attacker's `attackTimer` (which counts DOWN to the swing) is inside `PERFECT_BLOCK_LEAD` 0.15 s
and it is in its own reach, and `BotView.blockHeld` forces a release past 0.2 s so the next raise re-opens
a fresh window. Two engine facts make it narrower than it looks: a ranged enemy's timer says when the SHOT
LEAVES, not when it lands (the flight time blows the window), and the STRAFE-state ranged fire only resets
that timer when it HAS LOS, so an enemy holding a shot behind cover drifts it unboundedly negative and
reads as forever-about-to-swing — that was 213 of 275 raises before the `attackTimer > 0` gate. Live after:
21 raises, 8 hits landed on the shield, 8 PERFECT / 0 blocked.
**AIM STEADINESS — the camera is the player's camera.** The eased aim above is only half the problem: the
bot's camera IS the player camera, so a DESIRED aim that jumps is a screen that shakes ("the aim is still
sometimes super shaky, that needs to go"). Instrumenting the desired vs applied yaw per tick, tagged by
which branch produced it, found the shake is **never jitter in any one signal — it is the aim's SOURCE
changing**: in the worst windows the nearest hostile's LOS raycast toggled on **45-57 of every 60 ticks**,
dropping the brain out of FIGHT into TRAVEL **23-28 times a second** with a ~55° swing each time, while the
raw target BEARING moved <2°/tick. Lead-point jitter, target thrash and the wobble measured as
non-contributors (0.0-1.9°/tick, ~0 switches/s, 0.5°/s respectively). Three fixes, in order of effect:
(a) **TARGET LOS GRACE** — `pickTarget` holds a BLIND sticky target for `TARGET_LOS_GRACE` (0.4 s, driver-timed
into `BotView::targetBlindGrace`), but only while NOTHING ELSE is visible; a real rival still steals focus
instantly, and firing is untouched because `decideCombat` already gates the trigger on `t.hasLOS` (held blind
targets are TRACKED, never SHOT AT). (b) **TRAVEL-HEADING COMMIT** — the driver commits the post-veto
`flowDir` for 0.4 s, released early only when the committed step stops being `stepAllowed`, when the fresh
heading is >120° away (a real route change; a 45/90° disagreement IS the boundary toggle being damped), or
when there is no heading at all. Both the flow byte and the ±45/±90 detour fan flip as the bot drifts over a
cell boundary, which was 8-16 large heading changes/s. (c) **AIM DEADZONE** — `AIM_DEADZONE_RAD` (0.016 rad
≈ 0.9°, pinned by test to stay under `FIRE_ALIGN_RAD` or it would mute the bot): inside it the aim HOLDS.
Direction REVERSALS, not magnitude, are what read as shaky. A low-pass on the desired aim was deliberately
NOT added — it smears a discrete 60° branch step over ~5 ticks without reducing how often it happens, and a
second lag stage in series with the ease would push the steady-state tracking error past `FIRE_ALIGN_RAD`
and mute fire on crossing targets. Measured, paired 2-min live runs (same binary, fix on/off): mean
|Δdesired yaw| **5.0 → 1.9°/tick** and applied-yaw reversals **4.5 → 1.2/s** (Marksman), **2.8 → 2.2** and
**1.6 → 0.9/s** (Warrior); floors reached identical, damage taken −16%/−33%, kills within run-to-run noise.
**Combat POLICY rules from watching the bot play.** (1) **KITE ONLY FROM MELEE, AND ONLY INSIDE 4 m.** The
`dist < engageMin` back-off is gated on `!t.isRanged`: backing away buys spacing from something that must
REACH you, but an archer shoots across the retreat, so the same backpedal surrenders ground, drags the
bot's own aim off target, and changes nothing about the incoming fire. Against a ranged target inside the
band the bot HOLDS and shoots (live: 20% of all ticks were backpedal-from-ranged → **0**, and Marksman
kills/2 min went 9.3 → 19.5 because it was finally standing still long enough to hit things). It is
additionally floored in METRES by `KITE_HOLD_GROUND_M` (4 m, Aaron's number: "make it so ranged won't run
away when it has at least 4m distance") — `engageMin` is a FRACTION of weapon reach, which for a ranged
build works out near 11-13 m, so "inside the kite floor" meant the bot retreated from melee enemies it had
already outranged several times over. Below 4 m the fractional rule is back in charge.
(2) **DODGE ON AN INCOMING SWING, NOT ON PROXIMITY, AND ON A REAL LEASH.** The proactive roll used to
fire whenever an enemy was inside 0.6× the kite floor — for a ranged doctrine that is most of every fight.
`Autoplay::swingIsIncoming` now shapes it like the block tap (a MELEE attacker inside its own reach with
`attackTimer` under `DODGE_LEAD` 0.30 s — a longer lead than the block's 0.15 s because a roll is slower
to commit), it scans ALL targets like the block does, and the DRIVER holds a multi-second leash on top
(`Doctrine::dodgeCooldownSec` — 4 s, Glass Cannon 2.5 s — since the engine's own 1 s dodge cooldown is a
balance number, not a behaviour one). The OFFENSIVE gap-closer charge rides its own longer
`GAP_CLOSE_COOLDOWN` (6 s) so it reads as a rush, not a stutter of hops; `BotIntent::dodgeIsGapClose`
tells the driver which leash to charge. Live: Marksman **23.1 → 3.3** rolls/min, Warrior 8.4 → 4.9.
(3) **STICKY TARGETS.** `pickTarget` used to return the nearest LOS target every tick with no memory, so
similar-range hostiles made the crosshair flip forever (and with the eased aim it never settles —
"make it so ranged doesn't try to rapidly switch between enemies"). The driver remembers the engaged
enemy by IDENTITY (`BotTarget::id` = the packed entity handle — the array is re-sorted by distance every
tick, so an index is not an identity), and the policy keeps it unless it is gone / blind / past
`engageCeiling` / **unreachable while a rival is inside `weaponRange`**, or a rival is ≥30% closer
(`TARGET_SWITCH_GAIN`) AND `TARGET_MIN_DWELL` (1.5 s) has elapsed. The first four release IMMEDIATELY —
the dwell must never pin the bot to something it cannot shoot. The reachability release is load-bearing
for MELEE: without it a warrior commutes across the room to a held far target while three enemies chew on
it (measured −23% kills; with it, −12%). `engageCeiling`/`THREAT_RADIUS` are single-sourced in
`autoplay_combat.h` because the brain's FIGHT gate and this release MUST agree — if they disagree the bot
holds a target the brain refuses to engage and falls through to TRAVEL with a live enemy on top of it.
Live: mean seconds on one target Marksman 2.8 → 3.4, Warrior 1.7 → 2.3 (a melee bot's switch rate is
floored by its own kill rate — a dead target forces a re-pick — so stickiness has little headroom there).
(4) **NO DIAGONAL CORNER-CUTTING** in the hazard veto — see the veto-scope paragraph below.
(5) **RANGED BUILDS BLOCK, AND THE SHIELD TIMES INBOUND SHOTS.** `doctrineFor` now forces `blocks` on for
the whole **Ranged column** (it was off for Moderate/Ranged and Glass/Ranged): a PERFECT block negates ALL
damage, it is a pure timing feat the game always rewards, and blocking does NOT gate firing — the only
cost is 0.4× move speed for the ~0.15 s the tap lasts, so there is no build it is wrong for. That is only
useful against archers if the raise can be TIMED, and `swingIsLanding` refuses ranged attackers by design
(their `attackTimer` marks the LAUNCH, not the impact — the flight time blows the window). So the driver
scans the live projectile pool for **hostile** shots (`!fromPlayer`) that are genuinely closing — the time
of closest approach along the shot's own velocity must be in the future AND the miss distance at that
moment inside the player's body, which is what stops a stray bolt crossing the room from causing a turtle —
and reports the soonest as `BotView.incomingProjectileEta`; `decideCombat` raises inside
`PERFECT_BLOCK_LEAD`. That ETA is a real impact clock, so it can be timed exactly like a melee swing.
(6) **STRAFE AND JUMP.** `BotIntent::moveLeft`/`moveRight`/`jump` were plumbed all the way to the input
overlay and **never set by anything** — the bot only ever moved fore/aft, so it stood still inside its band
and ate every arrow. Against a RANGED target it is holding ground for (not closing, not kiting), the policy
now side-steps, flipping sides every `STRAFE_FLIP_TICKS` (66 ≈ 1.1 s; a constant slide walks out of the band
and a straight line is what a leading shooter wants). It rides the CURRENT yaw, so unlike the backpedal it
does not drag the crosshair off target. The **driver owns the safety check** and is authoritative for every
strafe producer (policy and unstick alike): it re-derives the world direction from the player's actual yaw,
runs `stepAllowed`, and REVERSES to the other side before giving up. A JUMP is pulsed on
`kitingJumpTick` (~2.2 s period, 5-tick pulse) while kiting or strafing — it breaks a shooter's vertical
lead and clears a 1-cell gap or lava vein — never while CLOSING (an airborne bot cannot steer). The same
pulse is now also part of the **escape ladder**: that ladder only ever tried new HEADINGS, and a body caught
on a lip or the inside of a corner needs to leave the ground, because move-and-slide keeps refusing the same
blocked axis at the same height forever.
**Story routing** for stacked/lava floors is folded into `flowDir` in `buildBotView` BEFORE the hazard veto
(`StoryNav` ramps for VERTICAL_HALL, `Autoplay::pickDropHole` for FOUR_STORY, lava rides the lava-aware veto).
**The Descent (FOUR_STORY) needed two rules of its own**, both from a measured 150 s trace in which neither a
marksman nor a warrior ever left floor 1 (marksman: closest approach to the L0 exit 13.9 m, 8 unplanned
climbs back up; warrior: never reached L0 at all — so "melee manages" was not what the numbers said).
(a) **RETURN-LIFT PADS.** About one drop hole in three has a `CELL_JUMPPAD` on the surface a story below it
(`PAD_HOLE_ONE_IN` in `level_gen.cpp`), deliberately, as a way back up. A pad fires the INSTANT you are
grounded, so dropping through such a hole is undone before the bot can decide anything — and from back up
there, the nearest hole is that same one. A closed loop the bot could neither see nor escape.
`Autoplay::pickDropHole` (pure, tested) skips them, reading the flag straight off the GRID at the hole's own
XZ (a pad cell carries the flag for its whole column, and `DungeonResult::jumpPads[]` is capped at
`MAX_JUMP_PADS` while a floor can hold more), and falls back to a padded hole only when nothing clean exists
on the story — on the deepest story holes are 7% dense, and a bounce still beats standing still.
Among the survivors it takes the **NEAREST**, and that is a measured decision: a first version scored holes
by "walk to it PLUS the walk from it to the exit" to stop the descent wandering in XZ, and it was strictly
worse (it chose holes 15-22 m off, and since the travel heading is a straight line with a ±45/±90 fan and
NOT a path, the bot beelined into a maze wall and never left L3). Only a LOCAL goal is steerable on a
labyrinth; the landing XZ needs no managing, because once the bot is on L0 the ordinary flat exit flow field
routes it to the door. Live after: L0 occupancy 3.5% → 43% of samples, upward bounces 8 → 2, exit distance
51 → 11.5 m.
(b) **CROSS-STORY TARGETS DO NOT HOLD FIGHT.** On a stacked floor a hostile can have clear LOS through a
drop hole or off a balcony rim and still be somewhere the bot cannot walk — and a ranged build's engagement
ceiling is its full weapon reach (measured 30-35 m for a revolver), so such a target kept FIGHT alive from
right across the floor while FIGHT never routes. `Autoplay::sameStory` gates `pickTarget` (both the scan
and the sticky release, which must agree) on `|target feet − bot feet| ≤ STORY_GAP` — but ONLY when
`BotView.stackedFloor`, so no flat floor changes behaviour, and never for a FLYER, since bats and drones
hover 1.5-2.5 m above their target by design and a hovering enemy is not an unreachable ledge sniper.
2.6 m rather than the 3 m story pitch, to clear a ranged flyer's ~2.1 m feet.
Know the **veto's scope**: `Autoplay::stepAllowed` (off-map / wall / grounded-in-lava) is applied in
`buildBotView` to `flowDir` — the TRAVEL heading — plus, at the very end of `updateAutoplay`, to the
LATERAL strafe (whoever produced it: the combat policy or the unstick helper). `applyBotIntent` itself
vetoes nothing, so the FIGHT branch's fore/aft kite/close movement stays unvetoed by design (short,
reactive, enemy-derived); the escape backstop calls `stepAllowed` on its own headings. A NEW nav source
must either fold into `flowDir` upstream of that check or call `stepAllowed` on its own heading.
The veto enforces **NO CORNER CUTTING**: a step that crosses BOTH grid axes needs the diagonal cell AND
both orthogonal component cells, the same rule `Pathfinder::findPath` applies to its 8-connected expansion.
The original point-sampled only the destination, so it happily approved squeezing through a wall's shared
corner that the bot's ~0.3 m body cannot fit through — the bot pressing itself into corners and wedging
("it tries to cut corners too often and gets stuck in the corner"). Because of that rule the `buildBotView`
detour fan had to widen from ±45° to **±45° then ±90°**: when a CARDINAL heading is blocked by a wall dead
ahead, both ±45° candidates are diagonals whose orthogonal component IS that wall cell, so a ±45°-only
ladder would hand every wall-ahead to the 4 s stuck-override. ±90° is the square sidestep that rounds the
corner along the grid. Note the flow field itself is CARDINAL-only (`buildFlowField` expands 4-connected
for exactly this reason), so on flat floors the rule fires rarely — it earns its keep on `escapeHeading`'s
explicit diagonals and the unstick strafe.
The driver's other **backstops** ride on top, so an AFK bot is never found permanently idle: a loot-settle dwell,
low-HP health-globe detours, a **combat break-off** (an in-band fight that lands no damage arms a 1.5 s
relocation leg — walk the flow heading with fire OFF to de-fixate, or, when there is no heading at all
because the bot is BOXED, strafe around the target while FIRING, which is the only way out), an
**exit-progress watchdog** (a 4 s window with no kills AND <1 m closed on the door latches "bull to the
exit": A*-routed first leg, fire through anything on the path, stop inside 1.5 m so the interact hold can
land), and an **escalating escape** for a geometry wedge (>4 s without XZ progress: lateral ±90/180 nudge →
8-direction safe-step search away from the wedge anchor → a short A* leg toward the door, each heading
re-checked against `stepAllowed`).
**A remedy may only STAND STILL where the descend can actually fire.** The exit-wedge remedy engaged at
2.5 m while `updateFloorDoor` descends at **2.0 m**, so between the two it planted the bot holding a button
that could never fire — and standing still IS "no progress", so the remedy re-armed itself forever
(measured: 73 consecutive seconds frozen beside an open exit). `Autoplay::DESCEND_RADIUS` / `DESCEND_STOP_M`
(1.9 m) single-source that, with a `static_assert` + test pinning stop < radius; outside the radius the
remedy WALKS THE LAST METRE IN (interact still held) instead of parking, bounded by the no-progress timer so
it can never shadow the geometry escape.
And **LOOK BEHIND at 3 s** — the first rung, before the 4 s geometry ladder. The dungeon's stone gargoyles
(`EnemyRole::AMBUSH`) sit in `AIState::DORMANT` under a **weeping-angel** rule: `enemy_ai_states.cpp` wakes
one only when a player is in range AND **nobody is watching it**, and `Combat::applyDamage` returns early on
a dormant AMBUSH enemy so it cannot be shot awake either. A gargoyle is an ordinary hostile in
`buildBotView`'s target list, so the bot AIMS AT IT — which is exactly what pins it asleep — and then fires
at it forever for zero damage: a standoff that by construction can never clear itself. So the driver turns
the aim 180° (`Autoplay::lookBehindYaw`, through the normal smoother so it reads as a look over the
shoulder) for `LOOK_BEHIND_HOLD` (1.2 s — longer than the smoother's own ~0.9 s half-turn, or the bot never
actually faces away), movement and fire dropped, **one-shot per stuck episode** (the latch re-arms only on
real progress, so it can never spin). It cannot fire during a real fight: the no-progress timer is held at
zero while the bot deals damage or moves >0.5 m. Live: 1-4 look-behinds per 2-minute run, and runs where it
fired often never needed the escape ladder at all. **Auto-respawn**
re-enters the run ~1.5 s after a solo death (entrance spawn); the **difficulty ladder** is the existing
floor-50→next-difficulty flow. **No save-format change** — `m_autoplayActive` and the backstop timers are
all transient. This bot is the **empirical playtest rig the balance-lab spec deferred** — it runs the real
combat/loot/nav loop end to end; a natural follow-up is emitting per-floor metrics from the driver.
**Where v1 actually stands (measured, 2026-07-24).** On **flat** floors (`rooms` / `cavern` / `gauntlet` /
`hub`) the bot plays unattended for all three archetypes — in ~2-minute `--autoplay --new` runs a Sorcerer
and a Warrior each reached floor 5 and a Marksman floor 4, casting class skills, auto-equipping and
descending, with zero deaths and no permanent stall. The **known v1 limits are the hard floors, and they
are limits, not solid ground**: on the stacked styles (`VERTICAL_HALL`, `FOUR_STORY`) and the lava floors
the routing is CORRECT and the bot is never permanently wedged (the escape ladder guarantees that), but
their enemy density and vertical topology mean it can be **slow and may not complete such a floor
quickly** — a 100 s `--vhall` and a 100 s `--lava` spot check had it fighting (51 / 42 skill casts) and
looting throughout, and descending neither. Specifically, VH **balcony story-routing oscillates on some
seeds** (the ramp goal flips as the bot crosses the band edge). Treat "descends every floor type promptly"
as unproven — flat-floor progress is the claim the runs support.

**Persistence is per-character.** Each save file (`save_NN.dat`) holds exactly ONE character (`playerCount=1`); the per-lane destination is `m_playerSaveSlot[lane]` and `saveAllCharacters()` writes each active lane to its own slot. In couch co-op both players pick their own slot (New or Continue) in the menu lobby and the shared dungeon runs on **Player 1's floor**; mixed New/Continue lanes work because the menu prepares each lane and calls `startGame(mode, lanesPrepared=true)` (skipping the NEW_GAME wipe). Legacy `playerCount=2` bundle saves still load (and migrate to per-character on the next save). The on-disk layout is **versioned** (`SAVE_VERSION`, currently 3 = GLOVES slot + attack-speed cache in `PlayerInventory`): readers accept the previous version via a `Legacy*V<n>` mirror struct in `engine_persist.cpp`, and `static_assert`s pin the serialized struct sizes — any layout change MUST bump the version and extend the legacy readers. (Save format, menu flow, no-downgrade guard: `engine-reference`.)

**User-data location (desktop) + Steam Cloud.** Saves, `difficulty_unlock.dat`, `menagerie.dat` (pet-collection progress), and the `controls.json`/`audio.json`/`video.cfg` prefs are written into the per-user dir `SDL_GetPrefPath("EdRethardo","DungeonEngine")` via `Platform::userDataPath()` (`src/platform/user_paths.{h,cpp}`), NOT the install/working dir — so Steam **Auto-Cloud** can sync them (config: `docs/steam_cloud.md`). On `__SWITCH__` `userDataDir()` is `""` (CWD = app storage), so Switch is unchanged. A one-time `migrateLegacyUserData()` (in `Engine::init`) copies pre-relocation files from the CWD/exe dir into the pref dir **only when the destination is absent** (never destructive). `saveCharacter` writes **atomically** (temp + `Platform::atomicReplace`) so an interrupted save can't corrupt an existing slot. `ORG`/`APP` are frozen — changing them orphans saves.

**`controls.json` is a versioned format too.** Bindings are serialized **by `GameAction` enum ordinal**, so enum members are renamed in place or appended before `COUNT` — **never inserted or removed**, or every existing player's bindings re-map onto the wrong actions. Changing an existing action's *default* binding additionally requires bumping `BINDINGS_REV` (the `CFG_BINDINGS_REV` sentinel row) and repairing that action in `loadBindings`, because a saved file's stale row otherwise silently beats the new default. Details + the rebind-UI range caveat: `engine-reference`.

**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 60 Hz (every tick). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 33 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)

**Netplay stack (rewrite COMPLETE — M0–M14 + the 2026-07 hardening pass).** Server-authoritative + client prediction with **rollback-replay reconciliation**: on a mispredict the client rewinds to the server's acked state and re-applies every stored input through the same per-input step the server drain runs (`updateNetPlayerFromInput` movementOnly + `moveAndSlide`), committing to BOTH player mirrors (`m_localPlayer` alias AND `m_players[slot]` — an alias-only write is erased next tick by `syncNetPlayerToLocalPlayer`). Under input starvation the server **coasts** a remote on its last input for ≤250 ms and CLAIMS the ticks (advances `lastProcessedInputTick`) so snapshots stay time-consistent; a separate `m_lastActivationTick` watermark fires late-arriving activation edges exactly once. Delta compression is **ack-driven with a named baseline**: every delta names the snapshot tick it's encoded against (client acks via `NetInput.ackedSnapshotTick`, server deltas against its 64-deep global history ring, client decodes from its 64-deep ring) — never assume "baseline = last sent". `PROTOCOL_VERSION` 24 (v19: the dead lock-on input bit became `INPUT_BLOCK` — the server simulates blocking for remotes: damage negation, perfect-block window, 0.4× move slow; `SnapPlayer.flags` bits 5–7 carry Static Charge stacks; v20: Arena PvP — sentinel floor 97 + the ARENA_KILL/ARENA_SCORES/ARENA_OVER events; v21: player-facing CC — `SnapPlayer.flags` bit2 = stunned + `stunTimerQ` on the reused `reserved0` byte; v22: `INPUT_WINDOW_SIZE` 8→15 so input redundancy spans the full 250 ms coast; **v23: arena/PvP authority fixes** — remote PvP projectile/chakram/AoE hits now PERSIST (they were erased by the shared-remote-view write-back race: `applyRemotePlayerViews` wrote a pre-pass-seeded view back over the health `pvpApplyHit` committed mid-pass; fixed by composing the PvP hit onto the same shared view via `m_sharedRemoteView[]`) — and, chakram-specific, a **bounce frame no longer drops its hit**: the ricochet is DEFERRED to the loop tail so the disc's pre-bounce segment is still swept for a player/entity (`pendingBounce` in `ProjectileSystem::update`; the old code `continue`d at the bounce and skipped that frame's collision). A Continue host now seats its OWN arena NetPlayer slot; behavior-only, no wire struct change, but old peers still carry the bugs so the version gate rejects them; **v24: `MAX_ENTITIES` 128→192** for the four-story Descent floor — `WorldSnapshot` carries `SnapEntity[MAX_ENTITIES]` and the per-slot unchanged bitmask is now `ENTITY_MASK_BYTES` (24 B, **derived** from `MAX_ENTITIES` so the two can't drift — a mask narrower than the pool silently resends its high slots forever, which is exactly the bug the old 64-bit-over-128 mask had), so both the full and delta layouts changed; idle cost is the 8 extra mask bytes, ~0.47 KB/s). Verification rig: `--net-loss <0-90> --net-latency <ms> --net-jitter <ms> --bot-walk` + the F9 net-graph (`[NET-GRAPH]` 1 Hz log: rtt/div/idelay/KB/s/snap-Hz/baseline-age). Measured envelope: 15% loss + 100 ms RTT → 0 hard snaps, deltas engaged (~9 KB/s vs 39 full). 2026-07 audit hardening: the **64-tick** snapshot history (both sides) keeps deltas alive past 300+ ms RTT (was a measured 12–25% full-snapshot fallback at 32), fire lag-comp rewinds honestly to **24 ticks**, **PvP victims are now lag-compensated like entities via a per-slot player-pose ring** (arena hit-reg), and outbound is `Net::flush()`ed every frame (~33 ms of hidden RTT recovered). For long-haul links (e.g. Germany↔New Zealand, ~150 ms one-way + jitter) the adaptive interp buffer and the server lag-comp rewind share `LagComp::MAX_INTERP_DELAY_MS` (250 ms) as the wire/rewind TRUST ceiling — but the jitter estimator's own 3× outage clamp caps its realistic *steady* delay near ~116 ms, so idelay rides spikes there, NOT up toward 250; `--net-jitter` reproduces the condition locally (constant `--net-latency` alone never engages the adaptive buffer). (Wire/tick details: `engine-reference`.)

## Directory Map

| Dir | Role |
|---|---|
| `src/core/` | Types (`u8`/`f32` aliases), math (Vec/Mat4), pools, logging, profiler, frame allocator, asserts |
| `src/platform/` | SDL2 abstraction: window, input (kb/mouse/gamepad), wall-clock |
| `src/renderer/` | OpenGL 3.3: shaders, meshes, OBJ loader, materials/textures, camera, frustum, debug-draw, HUD, font, minimap |
| `src/world/` | Cell grid, structural level gen (6 layout styles: BSP rooms / cavern / gauntlet / hub / vertical-hall two-story / four-story descent maze, seed-picked per floor), geometry meshing, raycast (DDA), collision (move-and-slide), combat queries (cone/raycast/AABB) |
| `src/game/` | Player, entities (enemy NPCs), projectiles, weapons, combat resolution, items+affixes+inventory, skills, enemy AI FSM |
| `src/engine/` | Top-level `Engine` class, game loop, system orchestration, mode/lobby/menu logic |
| `src/net/` | ENet wrapper, packet read/write, input ring buffer, snapshot serialization, server, client (prediction + interpolation) |
| `assets/config/` | JSON content: `items.json`, `affixes.json`, `skills.json`, `weapons.json`, `enemies.json`, `events.json` |
| `assets/materials.json` | Material → texture+tint table (loaded at init) |
| `assets/meshes/` | Wavefront `.obj` (low-poly enemies, weapons, props) |
| `assets/textures/` | 32×32 PNG tiles (`_NN` suffix = generator SEED, not size — most use 42); skins/non-tile too |
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

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
44-grid; `--vhall` dev door). NOT a single arena — a Quake **location-based** topology: a **LOOP of nine
distinct areas** laid out 3×3 and stacked across two stories, circled by a route that spirals up and down.
The **four CORNERS** are ground rooms (floor 0, cover **PILLARS**); the **four MID-SIDES** are **BALCONIES**
(`CELL_PLATFORM` slabs @ 3 m — walk ON top, walk UNDER the arcade beneath); the **CENTRE** is an open
**VOID** (ground, no balcony) that every balcony overlooks and drops into (the Quake sightline + a
cross-level shortcut). A **PINWHEEL of four RAMPS** climbs each corner up to the next balcony (graduated
slab; the enemies' chase-up route and yours after a drop); two **CATWALKS** cross the void @ 3 m linking
opposite balconies — one **INTACT** (the high road), one **BROKEN** with a 2-cell **jump** gap — so the
UPPER story is its own connected loop that crosses at the centre; **JUMP-PADS** in the void fling you back
up (player-only). Circle the ring and you continuously **ASCEND** (corner→ramp→balcony) and **DESCEND**
(balcony→drop→void); the exit sits on the FAR side AND the opposite STORY, so every floor forces a full
traversal and a level change (a coin-flip picks ascend vs descend). The **lower story is one fully-connected
floor** (corners + void + arcades → reachability guaranteed); the upper story hangs off it via the ramps +
catwalks. `spawnBalconyPos`/`exitBalconyPos` are explicit positions applied in `startGame` (upper = y 3 m,
ground = y 0; a `clearPad` opens any pillar that rolled onto a ground endpoint). Ranged "sniper nests" hold
the balconies and plunge-fire into the void (`spawnFloorNests` at the **four** ramp tops); regular enemies
spawn on the ground story (corners + void + arcades) and chase UP the ramps.
Enemies traverse both stories via story-aware `snapEntityToFloor` (`effectiveFloorHeight` not raw
`getFloorHeight`) + the ramp **`StoryPortal`** CHASE routing (`world/story_nav.h`, `DungeonResult.portals`;
`nullptr`/portal-count-0 inert elsewhere). Slab/ramp/collision primitives pinned by
`tests/world/test_platform.cpp`, the layout invariants by `tests/world/test_vertical_hall.cpp`; seed-built
+ server-authoritative → **no wire/save change**. (Earlier "Stacked Hall" split-pit design lives in
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

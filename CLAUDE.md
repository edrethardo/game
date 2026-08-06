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

**DIFFICULTY PASS 2026-07-30 — and the balance lab was measuring the wrong thing.** Aaron: "make the
game more difficult, increase mob HP and damage accordingly." Two findings, the second more important
than the change itself.
**(1) The lab omitted `difficultyHealthBump`.** `balance_lab.cpp` applied `floorHealthMult` and
`difficultyDamageBump` but NOT the per-tier HEALTH bump — under a comment claiming to be "the exact
spawn-time scaling path (engine_spawn.cpp)", which the engine applies at every spawn site. So every
deep-tier HP/TTK figure the lab ever produced was understated by the whole bump — **3x for Nightmare,
1.5x for Hell** — and its own model test re-derived the same wrong formula, so it PINNED the omission
instead of catching it. A model test only catches an omission if it is written against the ENGINE's
site, not the lab's. Corrected, and the test now mirrors the engine.
**(2) Normal was inverted, the deep tiers were not too easy.** With the lab fixed: Normal trash TTK
fell 0.86 s (floor 1) -> **0.33 s** (floor 50) while hits-to-die ROSE 3.5 -> 11-17, i.e. deep Normal
was by far the safest place in the game; Nightmare and Hell were already at **1.4-2.9 hits-to-die**,
with **Hell floor 5 at 0.99 — a literal one-shot**. So "increase HP and damage" could not mean a
uniform raise: the deep tiers have no headroom on the DAMAGE axis at all.
The pass therefore: **HP slope 0.12 -> 0.345** (solved against the measured 26.9x player-DPS growth
over Normal, so TTK stops collapsing), **damage slope 0.24 -> 0.40**, both deep-tier damage bumps
**RE-SOLVED to hold their totals EXACTLY** (NM 7.05 -> 4.30, Hell 12.045 -> 7.31), and the increase
for those tiers taken purely on **HP (+25% each)** — which walks the HP-over-damage ratio AWAY from
the one-shot boundary (Hell 1.52 -> 1.90). Measured after: Normal TTK 0.86/1.84/1.34/1.12/0.84 across
floors 1/10/25/40/50 (was 0.86/0.93/0.56/0.44/0.33) with hits-to-die 3.5-6.8 (was 3.5-17.5);
Nightmare and Hell a clean uniform **x1.25 TTK with damage unchanged**.
**`floorHealthMult` is now SPLIT at Normal's last floor** — linear governs Normal, compounding alone
governs the deep tiers. The old unconditional `max(linear, compounding)` was safe only because the
0.12 slope was too shallow to reach; at 0.345 it governed to effective floor ~92 and dragged early
Nightmare to **6.6 s TTK against 1.7 hits-to-die** (sponge and glass cannon at once). Clamping the
floor ARGUMENT instead was worse and subtler: it pinned Nightmare's first ~25 floors at a constant
17.9x while player DPS climbed, re-creating the inverted curve INSIDE Nightmare. Both were caught by
measuring, not by reasoning. Consequence: the raw function now steps DOWN at effective floor 51 and
must NOT be asserted monotonic on its own — it is never used alone, and the test now pins the PRODUCT
with the tier bump, walked in progression order (rises within a tier, steps UP into Nightmare, and
keeps the deliberate ~0.52x Nightmare->Hell dip). **Still open:** Hell floor 5 at 0.99 hits-to-die
predates this pass and is untouched by it — but see the correction directly below before treating
that number as a statement about the GAME.

**"Hell one-shots you" was an OVERSTATEMENT — the 0.99 is a gear-MODEL artifact, corrected
2026-08-01.** Aaron pushed back on it ("were the bots oneshot?") and the answer is no. The lab figure
itself is real and reproduces exactly (Hell floor 5, Tanky/Magic: 0.99), but three things were being
read into it that the data does not support. (1) **It is a three-floor DIP, not the tier.** Hell floor
1 is 2.15 hits-to-die, the minimum is **0.81 at floor 4**, floor 7 is back to 1.45, and floors 40-50
average **2.27**. (2) **The dip is the PLAYER model, not enemy scaling.** Across Hell floors 1-10 the
enemy hit is nearly flat (7792 -> 8477, +9%) while the modeled player's EHP collapses 16737 -> 7769
(-54%) and then recovers — because the lab equips from a **4-effective-floor window** (`WINDOW_FLOORS`)
and `ItemGen` wraps the drop POOL every 50 levels, so at Hell floor 4 the whole window is wrapped-level
1-4 starter base items. It models a player who arrived in Hell wearing nothing from Nightmare; a real
player (and every soak bot) carries their Nightmare-50 gear across, which the lab cannot express. This
is the known "Hell gear lottery" wearing a scarier hat. (3) **Empirically the bots are not one-shot:**
across soak13's Hell deaths the median bot was at **64%** (rogue) / **37%** (wanderer) HP one second
before dying, and only 30% / 7% of deaths came from above 90% — inside a full second holding up to 16
attackers, which is many hits, not one. Consequence for tuning: the deep tiers DO have some damage
headroom (Hell endgame ~2.3 hits-to-die, and the player's post-hit i-frame grace stretches that in real
play); what they do not have is room for a blanket multiplier applied without measuring. Discount the
entry-floor numbers as a model artifact rather than designing around them.

**INFERNO — the 4th difficulty tier, and MYTHIC, the rarity above legendary (2026-08-02).** Aaron:
"implement Inferno and the higher than legendary Tier". Both ride existing byte fields, so no struct
grew anywhere.
**The tier** is a pure scaling step on the existing ladder: Hell floor 50 now PROMOTES to Inferno
(effective floors 151-200) instead of rolling the credits, and only Inferno 50 ends the run.
`FreePlay::DIFFICULTY_COUNT` is 4 and `FINAL_DIFFICULTY` names the last rung, because the ladder
bound, the save-load clamp, the unlock-file sanitize, the `--difficulty` range and the balance sweep
all keyed off a literal `2` — five silent caps, of which the save clamp was the dangerous one (an
Inferno save loaded as NORMAL, keeping its gear and losing 150 effective floors). Tier names are
single-sourced in `FreePlay::difficultyName`: there were two separate `const char* diffNames[3]`
literals, and a 4th tier would have indexed both off the end. **`saveCleared` deliberately still
tests `difficulty >= 2`** — every hero who beat Hell before Inferno existed is stored as difficulty 2
/ floor 51+, and that predicate is what grants them the town and Free-Play; raising it would have
retroactively un-cleared all of them. The Source shard gate moved `== 2` -> `>= 2` for the same
reason (an equality gate would stop the secret boss's key dropping in the one tier players hunt it in).
**The numbers were MEASURED, not assumed.** The analytic solve (a x2.54 damage step over Hell,
mirroring Hell's step over Nightmare) gave 13.93 and the lab measured it at **1.49 hits-to-die** across
the endgame — hotter than agreed, because player power does not grow between tiers by the same factor
the enemy curve does. Re-solved by ratio to **11.53**, which measures **1.80** over floors 40-50
(Hell 2.29, Nightmare 3.04, Normal 6.94). The HP bump is **0.935** — below 1.0, which looks alarming
and is the same shape as Hell's 1.875 being half of Nightmare's 3.75: the compounding floor term
carries more of each successive tier, so the flat lever shrinks while the total still rises (HP-over-
damage 1.90 at Hell-50 -> 2.53 at Inferno-50, i.e. spongier faster than lethal). Aaron's standing
call — "1.8 is fine since the player has the grace period" — is the target the tier is solved to; the
post-hit i-frame window is what makes ~1.8 a fight rather than a coin flip.
**MYTHIC** is `Rarity::MYTHIC`, appended ABOVE legendary (value 4) — appended, never inserted, because
the ordinal is the serialized value and a middle insert would silently reinterpret every existing save
and in-flight packet. It drops **only in Inferno**, and is **carved OUT of the legendary slice**
(`MYTHIC_SHARE_OF_LEGENDARY` = 25%) rather than added beside it, so the top-of-table payout rate is
unchanged and every lower tier's rates are byte-identical. A mythic IS one of the named uniques rolled
harder: it draws from the **legendary def pool** (no def authors `maxRarity: mythic`, so a literal
window test would find nothing, degrade the tier, and the rarity would never drop while looking
implemented), guarantees the full affix count instead of rolling 3-4, and rolls affixes and base stats
above the legendary ceiling (`MYTHIC_AFFIX_POWER` 1.25, `MYTHIC_BASE_POWER` 1.15). That "one extra
power" shape was chosen over a 5th affix slot precisely to avoid growing `ItemInstance` — a power step,
not a layout change. Its colour is **Diablo 2's unique tan (#C7B377)**, Aaron's call; deliberately a
dustier hue beside legendary's bright gold.
**`isLegendaryOrBetter()` is the contract that keeps them equal.** ~25 sites open-coded
`== Rarity::LEGENDARY` — granted skills (weapon/armor/ring/offhand/gloves/boots/helmet), never
despawning, eviction immunity, the minimap marker, the doubled tooltip border, the material swap, the
Auto-Loot pickup bonus, `isDefinitiveBest` — and any one missed would have made a mythic strictly WORSE
than a legendary. 27 call sites now go through the helper; adding a tier above the top means auditing
exactly its callers. `BuildScore`'s tiebreak (`2.0f * (f32)rarity`) picks up the step for free.
**Versions:** `PROTOCOL_VERSION` 25 -> **26** and `SAVE_VERSION` 4 -> **5**, both for VALUE ranges on
unchanged layouts. The save bump is the load-bearing one: without it an older binary would open an
Inferno hero, clamp them to Normal, and write that back on the next autosave — a silent, unrecoverable
demotion. v4 stays readable and shares the direct-read path (keying that path on `== SAVE_VERSION`
alone is a trap when a bump adds no fields: every v4 save would have failed to load).

**DEAD LEGENDARIES: two weapons granted a skill their slot could not fire (found + fixed
2026-08-02).** Aaron, on seeing a Mythic Shadow Stiletto drop: "the name of the shadow stilettos
skill and what it does don't match". They did not — because it did NOTHING. A weapon's
`legendarySkill` fires as an **on-hit PROC**, and the proc rail is a `switch` over a FIXED set of
`SkillId`s; the **Phase Saber** (defId 3) and **Shadow Stiletto** (defId 6) both carried
`phase_dash`, which has no case on that rail. Each rolled its 20%, found the SkillDef (phase_dash IS
in skills.json), entered the switch and fell straight through `default:`. Both are `minRarity
legendary`, so the dead rail was the ONLY one they ever engaged — and the tooltip plus the equip
skill-bar advertised "Teleports forward through enemies" the whole time. An unhandled enum in a
switch is legal C++: silent at compile time and at load.
**The weapon rail is THREE switches, not one** — melee/hitscan (`engine_combat.cpp`), projectile
(`engine_init_callbacks.cpp`), and a co-op REMOTE twin (`engine_combat.cpp`, for a guest's hits) —
and they support DIFFERENT sets. `arc_fire` is melee-only; `shadow_ricochet` was projectile-only.
So "implemented" is never a property of a skill, always of a (skill, rail) pair, and a fix that
touches one rail leaves the same item dead one seat over in co-op.
**The fix is two real proc effects** (Aaron: "Phase dash would be awful on a weapon. Make it better,
have 2 weapon proc effects"). **Shadow Stiletto -> `shadow_ricochet`** (30% on hit: two shadow bolts
seek OTHER nearby enemies and can re-proc, so a stiletto rewards fighting inside a group) — the
effect already existed on the projectile rail and is now on the melee one too. **Phase Saber -> the
new `PHASE_REND`** (25% on hit: the edge phases onward and rends a wall-stopped 6 m corridor BEYOND
the target) — Phase Dash's corridor damage with the teleport REMOVED, deliberately: a proc fires
mid-swing on a random hit, and yanking the player 6 m forward whenever a die came up is exactly what
made the skill awful on a weapon. Both are added to the melee rail AND the remote twin.
**Pinned so it cannot recur:** "every legendary's granted skill is live on its slot's rail" in
`test_legendary_pool.cpp` encodes each rail's real capability set and walks all 51 granting items —
verified by sabotage (restoring `phase_dash` on the Phase Saber fails it BY NAME). When a rail learns
a skill, extend the table there. The remote twin's missing VOID_ZONE case was CLOSED 2026-08-02 (a guest's melee/hitscan void weapon
would have done nothing), and the guard was corrected with it: its first version modelled only TWO
rails and therefore certified VOID_ZONE as handled everywhere — a guard wrong in the safe-looking
direction is worse than none. It now enumerates all three (local melee/hitscan, remote twin,
projectile) and requires a melee/hitscan skill to satisfy BOTH of its rails; sabotage-verified
against exactly the case it used to miss. Still open: `SECOND_WIND` has a complete ring
implementation that NO item grants.
**Both new procs are now verified IN PLAY**, not just structurally: SHADOW_RICOCHET fired in two
soak instances, and PHASE_REND was confirmed by temporarily making the Warrior start with a
legendary Phase Saber (reverted immediately) — `[PROC] first fire: weapon skill id 56` within
seconds of the first swing.

**THE FULL BAG THRASHED FOREVER (found + fixed 2026-08-02).** `autoEvictWorst` made room by
evicting the bag's worst item for WHATEVER was being picked up — with no comparison between the two —
and it drops the victim 1.2 m in front of the player, well inside the 2.5 m auto-loot vacuum. With a
permanently full bag that closes a loop: evict X, take Y, next pass sees X on the ground,
`worthPickingUp` still says yes, evict something, take X, forever. Measured in the soak as the SAME
item (a mythic Void Talons, ilvl 188) spawning **699 times in 26 minutes**, one every ~2.2 s — the bot
spending its loot pass swapping two items back and forth instead of playing. It had been invisible
because nothing logged a drop; the new `[MYTHIC]` line made one churning item self-report, and the
raw count (1218 lines for **20 distinct items**) is what exposed it.
**The exchange must be a strict UPGRADE**: `autoEvictWorst(lane, incomingScore)` evicts nothing
unless the incoming item beats the victim's rank, so every accepted swap raises the bag's total and a
bag can only improve finitely often — the loop terminates by construction. When it is not an upgrade
the pickup is declined and the item stays on the ground, which is the documented intent anyway
("worse and near-duplicate loot stays on the ground"). The victim's rank keeps its +1e6
best-in-slot protection while the incoming score is a plain `maxCellScore`, so a protected piece is
effectively never traded for loose loot — losing the only item that can field a build is worse than
walking past a drop. NOTE the nuance against the older "never pauses" rule: the bot still never
stalls, but a full bag now SKIPS an item that is not better than what it would displace.

**Level previewer (`tools/level_preview.py`, 2026-08-03).** Dumps any generated layout as ASCII
without launching the game — `tools/level_preview.py wilderness --seeds 1-6 --side-by-side`. It shells
out to an env-gated case in the test binary (`LEVEL_PREVIEW=<style>:<seed>:<size>`, the BALANCE_REPORT
pattern) which runs the REAL `LevelGen::generate`, so there is no second implementation to drift. The
glyphs answer the questions you actually have while authoring a layout: `' '` is open sky vs `'.'`
roofed (the one property an outdoor zone must get right and which is otherwise invisible), `'='` is a
walk-under slab, `'^'` a jump pad, `'?'` a cell that is neither solid nor floor — a generator bug —
and `0-9/a-z` mark ROOM CENTRES, since every placement consumer (enemies, shrines, chests, lights,
bosses) works from those and "the anchor landed inside a rock" has shipped twice
(VERTICAL_HALL's cover pillar, FOUR_STORY's maze walls). Before it, judging a layout meant booting the
game and walking around, which is slow, unrepeatable, and useless for comparing seeds.

**THE OVERWORLD — Act 1, "The Blood Buffer to Whitechapel" (2026-08-03).** The town is no longer a
cul-de-sac: its north gate opens onto a connected outdoor world, a Diablo 2 Act 1 homage that ends at
a boarded Underground station where the Rogue Monastery would be — the door to a later Hellgate
London arc. **Post-INFERNO content** (`FreePlay::overworldUnlocked`, a SEPARATE predicate from
`saveCleared` on purpose: raising `saveCleared`'s "Hell or deeper" threshold would silently strip the
town and Free-Play from every pre-Inferno hero).
**A zone is a level on a SENTINEL FLOOR (52-96)** — that one decision is the architecture. The floor
byte is already the world's identity on the wire (`SV_LEVEL_SEED`) and in the save header, so an
open world of connected areas costs **no protocol change, no save change for zone identity, and zero
geometry traffic**: a client rebuilds a zone from the same six bytes (floor, difficulty, seed) it
already uses for a dungeon floor. Floors 1-50 are the dungeon, 51 the cleared marker, 97/98/99
arena/town/Source; 52-96 were simply free.
**Why connected zones and not one big world** — three fixed buffers make the alternative unshippable,
and none of them announce themselves: the minimap's `s_visited`/`s_pixelData` are 64x64 statics that
**truncate silently**, the spatial grid projectile collision uses spans only +/-128 m (entities
outside it become **unhittable**), and the cavern generator hard-bails to BSP above 64x64. On top of
that the largest dungeon level already runs 455-480 of the 500 draw-call budget. Measured: a zone
runs **48-80 draw calls** at a locked 60 FPS, so the shape has real headroom.
**Terrain is SEEDED, landmarks are NOT** (`LayoutStyle::WILDERNESS` + `engine_zone.cpp` anchors).
The generator is the inverse of every other style — the interior starts OPEN and it ADDS rock clumps,
because that is what outdoor terrain is, and because an empty plain is the worst case for a
frustum-only renderer with nothing to occlude. Edge gates, waypoints and POI mouths are stamped after
the carve on cleared pads, so a landmark can never generate walled in (the bug VERTICAL_HALL and
FOUR_STORY each shipped). `WILDERNESS` is deliberately absent from the dungeon weight table, pinned
in both directions by `test_level_gen.cpp`.
**`enterWorld()` (engine_world.cpp) collapses the four-times-copy-pasted entry ritual** — pools,
flags, host-slot seed, placement + lane persist, seating, net wiring, seed broadcast. Every omission
of one of those steps has shipped as a bug, and **`enterTown` was missing the host `NetPlayer` seed**
(masked only because the menu happens to call `startGame` first); a fifth entry point would have
multiplied the trap.
**Waypoints** are `WAYPOINT_ID` world items — the sentinel trick buys spawning, replication and
server-side validation for free. They are the ONE sentinel that is **not consumed on use**. Discovery
is **per character**, a `u64` bit per `ZONES[]` row, appended to the per-player save block
(**SAVE_VERSION 6**; a v5 save reads 0 = "found none", which is right — those heroes predate the
overworld). The travel list reuses the town portal's menu-over-a-live-world flow (substate **25** —
23/24 are the Auto-Loot choosers, a collision that would have hijacked character creation).
**Autoplay STOPS here** (Aaron: the bot's remit ends at Inferno): entering a zone ends the run with
the orderly logged exit, rather than leaving a bot idling in content it cannot express.
**EACH ACT DRAWS ITS OWN BESTIARY** (`EnemyDef::act`, `"act": 1|2`; 0 = the dungeon). The acts spawn
**tier 5** exactly like the deepest dungeon floors, so before this tag the two pools were the SAME
pool: TristRAM fielded Void Heralds and Act 2's tube-dwellers, and a Zombie Process could turn up on
Hell floor 45. Filtered in `collectTierDefs` beside `unique` — the one choke every random roster goes
through — and the `act` parameter is **required, never defaulted**, because a default of 0 is exactly
how the dungeon would silently start drawing overworld monsters again the day a call site forgets it.
Pinned in `test_ai_preference.cpp`: each act must keep a rollable pool of its own (an act with an
empty pool spawns an EMPTY ZONE, which reads as a broken level and nothing else would catch it), and
the dungeon must keep its deep tier after the acts were carved out of it.
**Two new EnemyRoles, and the role mask is now `u16`** — the original eight filled every bit of the
byte. `role` is not on the snapshot wire (a guest learns behaviour by watching), so the widening is
local: no protocol bump. **ROUT** (D2's Fallen) breaks and runs below a third health, then rallies
after 3 s and fights to the death — ONE break per lifetime, because a pack that can rout repeatedly
re-triggers on every hit and the fight becomes a chase with no combat in it. It reuses `AIState::FLEE`
rather than RETREAT, and that is the whole reason it needs a role: RETREAT auto-exits to CHASE
whenever a player is inside detectionRange, so an enemy routing FROM the player would turn and charge.
**SPLITTER** dies into two copies of the enemy its `spawnEnemy` names — the SAME field the breeder
path reads, with the opposite lifetime (a breeder spawns repeatedly while alive, a splitter once at
death), so `tickBreeders` skips splitters explicitly or a Merge Conflict sheds halves while you fight
it. The recursion terminates in one step by construction (the half is a different, non-splitting def)
rather than by a depth counter JSON could mis-set, and a test pins that plus "the half exists" and
"the half is in the same act" — an unresolved name would just make the gimmick silently not happen.
**ROUT cost Entity a dedicated `routTimer`, and that is the lesson.** The first version used
`kiteTimer` on the reasoning that a fleeing enemy is not being kited — but it is ALSO the summoner's
curse cooldown AND the CHASE anti-kite accumulator, both rewritten every tick, so the rout fired and
then never rallied. Measured live, not reasoned about. `sizeof(Entity)` 536 -> **544**; it is a POOL
struct, never serialized, so growing it costs static memory and nothing on the wire.
**`enemyType` is now authored, not only inferred.** `inferEnemyType` is a mesh-NAME match table whose
default is SKELETON — a humanoid limb rig. That is right for the original roster (torsos the rig
completes) and wrong for every voxel model that already contains its own legs and wings, which is the
entire overworld bestiary: unread, the Escalator Hound grows a second set of arms and the Rubber Duck
sprouts legs. All 18 overworld defs author `"enemyType": "generic"` (no rig at all); an absent field
still falls back to inference, so no existing def changed.
**A skin's grid is the mesh's REAL filled voxel extent, not the nominal 7x16.** `add_voxel_model`
derives `tex_w`/`tex_h` from the `filled` set itself (`u = (gx-min_gx+0.5)/grid_w`), so a skin sized
to the nominal grid is STRETCHED across the model and every carefully placed band lands somewhere
else. The ten new skins were sized against measured extents (they range 4x4 to 9x16) — measure the
generator, do not trust the docstring.
**The bestiary winks, it does not copy.** Four originals whose joke is that the dungeon is software:
**Zombie Process** (never reaped, one eye still lit), **The Garbage Collector** (a robed reclaimer
that re-allocates your kills — `["summoner","healer"]`, composed from shipped roles so it costs no
new code), **Bit Rat** (bit rot: its spine ridge is visibly missing every third voxel and its texture
degrades to magenta/cyan toward the tail), **Legacy Archer** (deprecated, still firing). Zones spawn
**tier 5** — post-Inferno heroes would find tier-1 wildlife to be scenery — and a `peaceful` zone
(TristRAM) spawns none, which is what makes it read as a refuge. Every enemy needs a matching
`Mini <name>` pet def or `test_pet_item.cpp` fails: the jackpot can roll any enemy.
**ZONE BOSSES are placed BY NAME, and a named boss must opt OUT of the random roster.**
`ZoneDef::boss` names one enemies.json entry that `spawnZoneContents` spawns at the zone CENTRE with
`isBoss` set (health bar, nameplate, loot guarantee) — deliberately not through `spawnFloorBoss`,
which keys off bosses.json BY FLOOR and expands a room into an arena, neither of which a zone has.
Four exist: **The Garbage Collector** (55, 1800 HP — D2's Blood Raven), **Griswald, the Unfinished
Build** (57, 4200 HP — Act 1's finale, in the ruins of the village he used to serve), **The Perpetual
Commuter** (64, 2600 HP) and **Signal Failure** (66, 5200 HP — the arc's last fight). The floor/
difficulty scaling every dungeon spawn pays is deliberately NOT re-applied: the defs are authored at
post-Inferno numbers already.
An act boss is an ordinary enemies.json row (it needs a mesh, a skin, roles), so **nothing stopped
its tier's spawn pool from ALSO rolling it** — and nothing did: a single TristRAM held **two**
Griswalds, Act 2's final boss turned up in an Act 1 field as trash, and both would have appeared on
deep DUNGEON floors too (they are tier 5). `EnemyDef::unique` (`"unique": true`) is the opt-out, and
`collectTierDefs` is the single choke that honours it — every random roster in the game (trash
spawns, VHALL balcony nests, Descent hole snipers, the zone spawner) goes through that one function,
so the fix lands everywhere at once. The DATA is pinned as tightly as the mechanism
(`test_ai_preference.cpp`): every zone boss must exist in enemies.json AND be `unique`, every
`unique` def must be some zone's boss (an unplaced one is dead content that can never spawn at all),
and every SLAY quest must name a zone boss — otherwise the objective has no guaranteed target and the
act cannot be finished. TristRAM is **not** peaceful: D2's Tristram is a massacre you walk into, so
the one place the parody could not afford to be safe isn't.
**Waypoints follow D2's Act 1 placement, which is a design choice and not an oversight**: Cold
Storage, the Field of Unmerged Branches, the Deadlock Woods and Whitechapel Terminal carry one —
**the Blood Buffer and TristRAM deliberately do NOT**. D2's Blood Moor has none because the first
walk out of town is the tutorial, and its Tristram has none because you arrive by portal and leave in
a hurry. Both hold here for the same reasons.
**TRISTRAM IS REACHED BY A PORTAL, NOT BY WALKING (2026-08-06, Aaron: "make the map and the quests
like in act 1 including the Portal in the Stony Field").** D2's Tristram is not a place on the road —
it is a RED PORTAL raised at the Cairn Stones in the Stony Field, which is why the town is a
massacre you drop into and hurry out of rather than somewhere you pass through. Ours was an ordinary
border crossing between the Field of Unmerged Branches and the Deadlock Woods, which flattened the
whole beat. The Act 1 road is now D2's: Blood Moor -> Cold Plains -> **Stony Field** -> Dark Wood ->
the Monastery gate, with the Den of Evil, the Burial Grounds and **TristRAM** all hanging OFF it as
portal-only side areas (`neighbour` all NO_LINK, reached by `poiFloor`/`returnFloor`). The quest in
the Stony Field is the Cairn Stones beat — *Align the Standing Stones*, whose joke is that monuments
to abandoned features cannot agree with one another.
**It exposed a bug that already existed in the dens.** A portal-entered zone has no shared border, so
`zoneArrivalPos` fell through to the zone CENTRE — and `spawnZoneContents` clears the centre pad for
the named BOSS and dropped the return gate there too. Entering the Deprecated Graveyard therefore put
the player ON TOP of The Garbage Collector with the way home underneath them; the map change would
have done the same to TristRAM. The return gate and the portal arrival now stand
`RETURN_GATE_OFFSET` (8 m) south of centre with their ground cleared, measured at **10.0 m from the
boss** in both zones: you step out of the portal, the way back is at your back, and the thing you
came for is across the ruins.

**QUESTS (`game/quest_def.h`)** are D2's Act 1 chain, beat for beat, renamed: *Free the Allocation*
(clear the Den), *The Rebaser* (the graveyard keeps bringing its history back), *Align the Standing
Stones* (the Cairn Stones, which open the way to TristRAM), ***The Search for Deckard Cache*** (the pun the act was built around, and now the act's
CLIMAX — a SLAY on Griswald in the TristRAM ruins), and *Terminal Access* at the station, a REACH
that is deliberately the EPILOGUE and not a second climax competing with the first. Both halves of
that ending are pinned by test, because an act whose last beat is "arrive somewhere" has no payoff
and an act with two finales has a muddled one. Deliberately NOT a quest engine — no dialogue, no journal UI, no prerequisite
graph. Each quest is a place, one of three triggers the engine can already observe (CLEAR_ZONE /
SLAY / REACH), and a per-character bit in the same v6 save tail as the waypoint mask (widened while
v6 is still unreleased, which is free; adding a v7 later would mean a second conditional read in
every reader forever). The SLAY hook sits at `handleDeathPreamble` — the ONE choke every enemy death
funnels through — so no kill route (a proc, a pet, a thorns reflect) can miss an objective, and
CLEAR_ZONE is POLLED rather than event-driven because "the last one just died" is a property of the
pool, not of any single death. Offers and completions both LOG as well as printing to chat: a
chat-only line is invisible to a soak, which is exactly how the credits park and the dead legendaries
stayed hidden.
**The town's NORTH GATE is always carved, for every hero** — the town is deterministic geometry that
host and client each rebuild from the sentinel seed, so making the opening conditional on a save's
unlock state would let two peers build DIFFERENT towns and desync the moment one walked where the
other saw a wall. The Inferno check lives in the host-authoritative transition instead, with a
throttled chat line so a locked gate explains itself rather than reading as a bug.
**ACT 2 — "HELLGATE: LOCALHOST" (floors 60-66).** The Hellgate London parody, entered by the
ESCALATOR at Whitechapel Terminal (its `poiFloor`, not a north edge — you descend into the
Underground). London fell, the survivors live in the stations, and the tunnels belong to whatever came
through the rift: *The Northbound Stack* -> **Null Terminus** (the hub: the last platform with the
lights on, the act's only `peaceful` ground) -> *The Circle Line (Infinite Loop)* -> *Threadneedle
Street (Unsafe)* -> *Bank Station (Overdrawn)* -> *Piccadilly Circus (Buffer Overflow)* -> **Hellgate:
Localhost**. London's own names are half programming puns already — a terminus IS a terminal, the
Circle Line IS a loop, Threadneedle Street is an address in the City — so the parody mostly just has
to notice.
**Zones now choose their TERRAIN** (`ZoneDef::terrain`, an intent enum mapped to a LayoutStyle in
engine_zone.cpp) instead of inferring it: `TUNNEL` -> GAUNTLET, because a tube line genuinely IS a
serpentine chain of platforms, and `STATION` -> HUB, a concourse with passages off it. Reusing the
shipped generators keeps every layout invariant they are already tested for, rather than writing an
"underground" generator from scratch. `ZoneDef::underground` decides whether the ceiling is STRIPPED
(surface: sky + daylight clear colour) or KEPT — after a whole act of open country, the roof coming
down is the tonal shift into Act 2, and it is what makes the tunnels claustrophobic rather than merely
dark. Act 2 keeps ONE surface zone (Threadneedle Street) so the contrast still lands.
**ACT 2 GOT ACT 1'S SHAPE (2026-08-06).** The road is the tunnel line — Northbound Stack -> Null
Terminus -> Circle Line -> Threadneedle -> Piccadilly — and everything else hangs OFF it through a
portal: **Bank Station** off the Circle Line, and now **Hellgate: Localhost off Piccadilly Circus**.
The gate is the one thing in Hellgate London you never simply walk through, so the act's finale is
no longer a border crossing: Piccadilly's new quest ***Privilege Escalation*** is the rift-opening
beat, Act 2's answer to Act 1's Cairn Stones, and "Buffer Overflow" was already the zone's joke so
forcing it open is the obvious escalation.
**The finale LOST its waypoint, and a test is why.** Making it portal-only turned it into an
interior, which tripped "an interior must never carry a waypoint" — the rule that keeps fast travel
meaning something. The right answer was the DATA, not the rule: TristRAM has no waypoint either, and
in both acts the parent zone carries one, so dying at the boss costs the same single hop (waypoint to
the parent, portal back in). The two acts now mirror each other exactly.
**Act 2 quests** follow Hellgate's shape: *Signal Restored* (find the survivors), *Break the Loop*,
*Insufficient Funds* (Bank), ***Privilege Escalation*** (force the rift at Piccadilly), and ***Kill
-9*** at the rift itself. `Quest::actComplete` is now ACT-SCOPED —
a global "all quests done" check would have silently stopped announcing Act 1 the day Act 2's quests
were added, which is the quiet kind of regression a growing chain invites.
**Act 2 bestiary**, same wink-not-copy rule: **The Perpetual Commuter** (a demon in the ruin of a
suit, fused to its briefcase, still walking the route), **Mind The Gap** (the thing that lives in the
platform gap — an `ambush` enemy, so it is literally scenery until nobody is looking at it, and the
Underground warning line is painted along its lip), and **Signal Failure** (a hovering broken signal
mast, the act's rift boss, showing red AND green at once because that is what a signal failure is).
Act 2's roster fills out with **Escalator Hound** (a `charger` whose spine is a run of comb-plate
steps — it only ever runs one direction), **Turnstile Wraith** (a spectre fused to a ticket barrier,
still trying to touch in; a `ranged_caster` whose hit SLOWS you, because being held at the barrier is
the joke), **Fare Evader** (a `rout` imp caught mid-vault, which bolts the moment it is hurt) and
**Rail Replacement** (the `shield_bearer` brute of rail and sleeper — the service that replaces the
service). Act 1 likewise gains **Null Pointer** (D2's Fallen; the head is a hollow ring you can see
the level through, and it `rout`s), **Hot Reloader** (the Fallen Shaman: a `summoner` that breeds
Null Pointers straight back in), **Core Dump** (the Foul Crow, trailing the memory it spilled), and
two ORIGINALS — **The Merge Conflict** (two mismatched half-bodies on a `<<<<<<<` seam, the palette
literally a diff, which `splitter`s into two **Detached HEAD**s — a severed head that turns out to be
a PINK UNICORN's, muzzle and horn and a six-band RAINBOW trailing where the neck should be, because
the git joke lands harder when the head is absurd. Its rainbow is built one band PER GRID ROW on
purpose: `add_voxel_model` maps the skin by (gx, gy), so a band sharing a row with anything else
would bleed its colour across it) and **The Rubber Duck** (enormous,
serene, an `aura` enemy, coloured exactly like the bath toy — you are supposed to explain your
problem to it).
**`MESH_DEF_CAPACITY` 112 -> 128**: the seven new enemy meshes pushed the registry past its cap, and
the `static_assert` in `asset_manifest.h` is what caught it. Without that guard the loader silently
drops the TAIL of the table and those enemies render as fallback CUBES — the same failure that once
turned six limb meshes into cubes and gave every spider mandibles.
**Dev doors:** `--zone <52-96>`. Layout iteration without booting the game: `tools/level_preview.py`.

**SECOND REVIEW PASS: EVERY WAYPOINT AND ACT ENTRANCE DESPAWNED AFTER 60 SECONDS (2026-08-05).**
The worst bug of either review, and invisible to every test that existed. World items spawn with a
60 s `lifetime`, and `WorldItemSystem::update` exempts fixtures from decay via a HAND-LISTED set —
legendaries, shrines, Source shards, chests, stash, pets. The overworld's **WAYPOINT_ID and
ZONE_GATE_ID were never added to it**, so a minute after entering any zone every waypoint and every
POI mouth simply evaporated: fast travel dead, the Den of Evil and the Act 2 descent unreachable to
anyone who did not sprint straight there. `isSentinelItem` DID cover them (so the world-item eviction
rule spared them correctly), which is what made the gap so easy to miss — the two lists disagreed.
Nothing caught it because `zone_smoke.py` holds a zone for six seconds.
Fixed by deriving the rule instead of listing it: **a fixture is any sentinel EXCEPT the globe** (the
one sentinel that is genuinely consumable loot). A sentinel added tomorrow is therefore exempt BY
DEFAULT, which is the safe direction — the hand-listed set had already been wrong three times, with
shrines and Source shards each added retroactively after they evaporated in play. Pinned by a test
that simulates 120 s per sentinel type; sabotage (restoring the hand-listed rule) fails it by name.

**ADVERSARIAL REVIEW OF THE OVERWORLD WORK (2026-08-05) — three real defects, all the same shape.**
Aaron asked for a review of the uncommitted work; attacking it found one severe bug and two leaks,
and every one of them was a value that lived in two places instead of being derived from one.
**(1) ZONE BOSSES WOULD HAVE ONE-SHOT THE PLAYER.** Making them scale (above) was right, but it
exposed that their authored DAMAGE was wrong by the game's own convention. The dungeon's bosses carry
**3.7-37.4x trash HP but only 0.78-2.22x trash DAMAGE** — the late ones (Grim Reaper, The Dungeon
Engine, Korvath) hit for LESS per swing than a mob, deliberately: a boss is an attrition fight and
the game is balanced to ~1.8 hits-to-die from ordinary trash, so a hard-hitting boss is simply a
one-shot. The zone bosses were authored at **2.36-4.03x**. Retuned into the dungeon's own band
(damage 1.50-2.00x, HP 12-35x, Signal Failure kept below The Dungeon Engine's 37.4x ceiling) and
pinned by a test that reads the RATIOS off the live rosters rather than hard-coding them; sabotage
(restoring Signal Failure's 145 damage) fails it by name. It went unnoticed because the bosses had
been spawning UNSCALED — harmless for the wrong reason.
**(2) THE HELLFORGE SURCHARGE LEAKED INTO THE ACTS.** `m_level.lavaFloor` is cleared ONLY by
startGame, and it feeds `hellforgeHpMult`/`hellforgeDamageMult` inside `spawnFloorEnemies` — the very
path a zone spawns through. Reach the overworld after a molten dungeon floor and every zone enemy
silently took +50% HP and +30% damage. INTERMITTENT by construction (it depends where you had just
been), which is the worst kind. Measured: zone hpMult **3038 -> 4557** with the flag stale. Now
cleared in `worldClearLevelFlags`, exactly like the town portal that leaked there a day earlier.
**(3) LAYOUT STYLE was inherited too.** `m_level.layoutStyle` was never set by a zone, so a zone
behaved like whatever dungeon floor preceded it — the AI grants CAVERN-style open floors a x1.5
detection bubble, and the nav/autoplay paths branch on the stacked styles. `buildZoneLevel` now
records the style it actually generated, and the shared clear resets it for town/arena/Source.
**Two smaller hardenings from the same pass:** `atomicReplace` now REFUSES rather than truncates when
a save path is too long (a truncated `.bak` name could have collided two slots' backups), and its
comment no longer overstates the guarantee — nothing in the game reads `.bak`, so that recovery is
manual, not automatic. And `queryNeighbors` asserts its buffer is `SGRID_QUERY_MAX`: overflow is
appended LAST, so a caller passing a smaller array would truncate away precisely the entities the
overflow list exists to rescue, silently restoring the unhittable-enemy bug.
**The pattern worth keeping:** every defect in this pass was one fact stored twice — trigger band vs
arrival inset, arrival position vs cleared ground, boss ratios vs authored numbers, world state vs
the entry that owns it. The fixes that hold are the ones that DERIVE the second from the first.

**RESPAWNING OUT OF BOUNDS IN A ZONE — a regression from the ping-pong fix (2026-08-05).** Moving
the arrival point clear of the re-trigger band (2.5 -> `EDGE_ARRIVE_INSET` 5 m) pushed it PAST the
ground the gate carve had cleared: `zoneOpenGate` opened the border cells and then cleared a single
pad two cells in, so anything beyond that was whatever the generator put there. Measured across all
15 zones: TristRAM's north gate and the Circle Line's west gate dropped the player INSIDE solid rock,
and a body spawned in geometry is shoved out — through the border. The gate now carves a CORRIDOR
from the opening inward to `EDGE_ARRIVE_INSET + 1` cell, derived from the same constant so moving the
arrival can never again outrun the ground cleared for it. Verified over every reachable gate in all
15 zones (edges with NO_LINK are skipped — no gate is carved and nobody can arrive through one):
**0 solid arrivals**, down from 2.
The shape of this is worth remembering: the first fix was correct AND introduced a second bug,
because it changed a position without changing the geometry that position depends on. Two constants
in different files described the same thing.
**The HUD names the ZONE outdoors** rather than printing "Floor 57". The floor byte is a sentinel
identifying which world this is, not a depth, so a floor number is meaningless to a player standing
in TristRAM. The label buffer went 32 -> 64 bytes with it: the longest name ("The Den of Evil
(Franchise Location #2)") is 39 characters and snprintf would have truncated it safely but visibly.

**THE OVERWORLD PING-PONGED BETWEEN ZONES 52 TIMES A SECOND (found from Aaron's report, fixed
2026-08-05).** "I get teleported around when walking and the enemies are doing that lightspeed thing
around me, ignoring me." Two symptoms, ONE cause, and it is an off-by-epsilon:
`EDGE_TRIGGER_BAND` is 2.5 m and the edge test is `p.z <= EDGE_TRIGGER_BAND`, while `zoneGatePos`
placed an arriving player at a literal `2.5` — EXACTLY on the threshold. So arrival satisfied the
trigger on its first tick and sent the player straight back out through the gate they had just come
through, which by definition links back where they came from. Measured with a probe that marches the
player north: **1290 transitions in 25 s** (52 full world rebuilds per second) against **2** with the
fix. Both symptoms fall out of that one loop — the player is re-placed 52x/s (teleporting), and every
rebuild RESPAWNS the enemy pool at fresh positions in a fresh IDLE state, which is exactly "moving
lightspeed around me and ignoring me". It also explains assorted overworld weirdness that looked like
separate bugs.
Fixed in two layers, deliberately. The ARITHMETIC: arrival is now `EDGE_ARRIVE_INSET`
(= 2 x EDGE_TRIGGER_BAND), derived from the band with a `static_assert` that it exceeds it, so the
two can never drift apart again — the literal `2.5` duplicated in a second place is what broke it.
The ROBUSTNESS: `m_zoneEdgeArmed` DISARMS transitions on every zone entry and re-arms only once the
player stands clear of every border band. Geometry alone would be enough today; the latch is what
survives a future gate position, a different grid size, or a knockback that leaves a player inside a
band on arrival. This is the first bug in the overworld found by simply WALKING it — the surface that
has been flagged as untested since the acts were built.

**THE TOWN'S DUNGEON PORTAL FOLLOWED YOU INTO EVERY ZONE (found from Aaron's report, fixed
2026-08-04).** "There is an entrance to the dungeon in every area." `m_level.townPortalActive` was
set by `enterTown` and cleared ad-hoc in `startGame` and `enterArena` — but NOT in
`worldClearLevelFlags()`, the shared step whose entire purpose is to turn every "which special world
am I in" flag off. So the one entry that reached a world THROUGH that function and did not re-assert
the flag — `enterZone` — inherited it: the town's to-dungeon portal stayed live, rendered and
interactable, in all fifteen zones of both acts, and taking it would have launched a dungeon run
from inside Act 1.
This is the SECOND leak of exactly this shape; the function's own comment already cites the exit
portal staying live in the town. That is the argument for the single clear rather than per-site
ones: `enterTown` re-asserts the flag a few lines later, so putting it in the shared clear costs
nothing and every future entry gets it off for free. Verified by a discriminating test rather than a
convenient one — `--zone` runs `startGame` first, which already zeroes the flag, so the obvious
check passes either way; forcing the flag TRUE immediately before `enterZone` (which is what
arriving from the town does) shows 0 with the fix and 1 with it removed.

**THE ROGUE TELEPORTED CONSTANTLY — a mobility skill was being used as a damage skill (fixed
2026-08-04).** Aaron, watching a Rogue on VERTICAL_HALL: "he teleports and changes doctrine all the
time." Two separate causes, and the first is a general rule the code already knew but applied to only
one rail.
**(1) The in-range skill dump had no mobility carve-out.** `decideCombat`'s "every class dumps
biggest-first" loop excluded COUNTER skills and nothing else, so the highest castable slot won even
when that slot was a blink. The Rogue carries **SHADOW_STEP — 15 m, 3 s cooldown — in slot 1**, so
once Shadow Dance and Poison Cloud were cooling it became "biggest castable" every three seconds and
threw the bot 15 m off the enemy it was mid-fight with, forever. The EQUIPMENT rail had withheld
Phase Dash in range since it shipped, for exactly this reason ("blinking 6 m while already on top of
an enemy overshoots past it") — class skills never got the same treatment.
The rule is **DISTANCE, not category**: `BotView::skillGapDist[]` now carries `SkillDef.distance`
beside `skillIsGapClose[]`, and a gap-close is withheld from the IN-RANGE dump only when its blink is
longer than the weapon's reach. A Paladin's 3 m dash-smite is his filler and keeps firing at blade
range; excluding gap-closes wholesale would have muted him instead. The out-of-reach gap-close branch
is untouched — that is what the skill is FOR. Measured on the same 90 s VHALL window: kills **28 ->
40**, deaths **7 -> 5**. Pinned by two tests and verified by sabotage (removing the guard makes the
bot pick the 15 m blink and fails them by name).
**(2) The better-build nudge is silent under autoplay.** "Better gear for X — switch builds in the
Inventory" is advice to a human who is not at the controls: the bot re-gears itself but never changes
build CELL, so it can never act on it. Worse, the re-arm guard remembers only the LAST suggestion, so
two builds that leapfrog each other pass it every time — measured on the Rogue as Glass Cannon Melee
-> Glass Cannon Ranged -> Tanky Melee, a permanent stream. Suppressed while the bot is in control
(3 lines -> 0). The underlying oscillation still affects HUMAN players and is untouched: the guard
wants to key on the notified SCORE, not just the cell.

**SWITCH SAVES SILENTLY STOPPED OVERWRITING AFTER THE FIRST ONE (found from Aaron's report,
fixed 2026-08-04).** "I exited with Save and Quit but it didn't overwrite the old save." Every
character save is written to a temp file and promoted over the real slot by
`Platform::atomicReplace`, and `saveCharacter` treats a false return as "keep the previous save" —
so a failed promotion is INVISIBLE at runtime and only shows up when the player reloads.
`atomicReplace` special-cased `_WIN32` (where `rename` will not clobber an existing file, hence
`MoveFileEx`) and used plain POSIX `rename` everywhere else. **The Switch writes to a FAT32 SD card
through libnx, which has the WINDOWS semantics, not POSIX**: renaming onto an existing file fails.
So the FIRST save of a slot worked — no destination yet — and every save after it failed the replace
and kept the old file. The bug is invisible to a single round trip, which is why it survived: you
have to save the same slot TWICE to see it.
Fixed with a FAT-safe fallback that runs only when the plain rename fails, so desktop keeps the
genuinely atomic path: move the existing file ASIDE to `.bak`, move the temp into place, drop the
`.bak` — at every instant either the destination or the backup exists. The obvious one-liner
(`remove(dst)` then rename) was rejected because it has a window where the only copy of the save is
gone. **`atomicReplace` moved to `platform/atomic_file.h`** (header-only, SDL-free) purely so it
could be TESTED: it used to live in `user_paths.cpp`, which pulls in SDL for the pref-path lookup,
and the test binary has no SDL — so the one primitive every save depends on had no test at all.
`tests/platform/test_atomic_replace.cpp` pins overwrite, five repeated saves (the bug starts at the
second), and that a failed promotion leaves the existing save intact; verified by SABOTAGE — making
the header refuse to rename onto an existing file reproduces the Switch failure exactly and fails
the tests by name.

**OCCLUSION CULLING (`world/visibility.h`, 2026-08-04) — the fix that DID work.** The renderer culls
by FRUSTUM only, which on a maze or a stacked hall rejects almost nothing: everything in front of you
is "visible" even when a wall or a storey's floor is in the way. `Visibility` answers "might this be
seen from the eye" with `Raycast::cast` — the SAME slab-aware grid DDA the melee LOS gate, the enemy
AI and the bot already use, so there is no second notion of visibility to drift, and platform slabs
are understood for free (a storey below you is correctly hidden by the floor between).
Two consumers: `LevelMeshSystem::submitAll` (per SECTION, 9 sample points — its `grid`/`eye` args are
optional, so town/arena keep the frustum-only path) and the entity render loop (per ENTITY, 5 points
— centre, head, feet and a LATERAL pair taken perpendicular to the line of sight, which is what keeps
a shoulder showing through a doorway from popping).
**It is conservative by construction, because the failure mode inverts**: instead of drawing too much
you make an enemy VANISH. Hence a `MIN_CULL_DISTANCE` (6 m) inside which nothing is ever culled, ANY
sample passing means visible, degenerate inputs answer visible, and **4-frame hysteresis** in both
consumers so a single unlucky sample on a moving body cannot blink it out. `tests/world/
test_visibility.cpp` pins the safe direction specifically — shoulder-through-a-doorway, corner-of-a-
box, close-range-behind-cover and degenerate inputs must ALL come back visible.
**Measured, paired A/B from ONE binary (`VIS_CULL_OFF=1`), both arms run concurrently, ~60 samples:**
VHALL draw calls median **332 -> 160**, p90 **607 -> 237**, max **777 -> 361**; FOUR_STORY median
**416 -> 183**, p90 **621 -> 321**, max **702 -> 369**. Roughly halved everywhere, and the PEAK is
back inside the 500 budget on both stacked styles (it was 55% over). FPS and the CPU profile are
unchanged — the added rays are a grid DDA, the same thing the bot already runs 16 of per tick.
**Still to confirm ON DEVICE**: every number here is desktop draw-call accounting. The Switch was not
on the homebrew menu, so the handheld frame rate this was meant to fix has never been measured before
OR after — and the docked number, which is what would tell CPU-submission-bound from GPU-bound, has
never been taken at all (the Switch CPU runs at 1020 MHz in BOTH modes, so a handheld-only drop
cannot be submission cost). **A depth pre-pass (the other half of the plan) is deliberately NOT in
yet**: it doubles draw calls to kill overdraw, and the cull has already removed most of the hidden
geometry that WAS the overdraw, so adding it blind risks repeating the Y-banding mistake. Measure the
device first.

**SWITCH HANDHELD RUNS THE STACKED FLOORS AT 30 FPS — profiled 2026-08-04, and the obvious fix
MEASURED WORSE.** Aaron reported it and asked for a real fix rather than a band-aid. Profiling
settles WHERE the frame goes, and it is not where it looked.
**It is not the CPU, and it is not Autoplay.** Across VERTICAL_HALL / FOUR_STORY / a flat floor the
sim costs `Update` 0.03-0.11 ms, `AI` 0.04-0.19 ms, `Projectiles` ~0.01 ms. Even at a Switch CPU's
~8x disadvantage that is ~2 ms of a 33 ms budget, bot included. The enemy count hurts as DRAW CALLS,
not as simulation.
**It is draw calls, and the level's frustum cull rejects literally nothing.** Level submeshes
submitted per second are PERFECTLY CONSTANT — 5220 on VHALL, 3300 on FOUR_STORY, identical every
second no matter where the camera points (87 and 55 per frame). The cause is structural: a section
is 16x16 cells in XZ but its AABB spans the WHOLE floor height, so on a stacked floor the camera is
inside the box and the test can never fail. Totals reached **548 draw calls on FOUR_STORY and 532 on
VHALL** against a 300-500 budget, while a flat floor sits at 130-160 — and handheld clocks the GPU
~2.5x below docked, which is the shape of "docked fine, handheld half".
**The Y-BANDED SECTION SPLIT WAS IMPLEMENTED, MEASURED, AND REVERTED — do not retry this shape.**
The idea: band sections by storey pitch (3 m) so each AABB is tight in Y and the frustum can reject
storeys the camera is not on; flat floors produce one band and are untouched. Paired A/B, ONE binary
with a `BAND_OFF` env switch, both arms run CONCURRENTLY so GPU load is symmetric, ~52 samples each:
VHALL median **273 -> 281**, p90 **409 -> 481**, max **586 -> 662**; FOUR_STORY median **247 -> 365**,
p90 **484 -> 714**, max **620 -> 924**. Worse at every percentile, ~48% worse on the Descent. The
reason is plain in hindsight: splitting a section multiplies its per-material submeshes by the band
count (9 -> 36 sections on FOUR_STORY), while a 16x16 m footprint at 3 m band height is so flat that
a 60-degree vertical frustum swallows every band within ~10 m — so the draw calls multiply and
almost nothing is ever rejected. **Tighter bounds do not help when the camera can see all of them.**
**Where the real fix has to come from, per the same measurements:** entity submits dominate —
**165 draw calls per frame from 114 entities** on FOUR_STORY (vs 55 for the whole level), because a
body plus each articulated limb is its own draw call. Batching/instancing entities that share a mesh
and material, or tightening the existing limb LOD, targets the actual majority of the frame; level
geometry cannot get under budget on its own. **Still unverified on the device**: the console was not
on the homebrew menu, so every number here is desktop draw-call accounting, and it remains possible
the handheld limiter is fill rate rather than draw-call count — which would point somewhere else
again. Get a baseline from the device before the next attempt.

**TWO SILENT-DROP BUGS, both found by soak14's warning stream (2026-08-04).** Neither was new; both
had been quietly losing things for a long time, and both were logged at a level nobody reads.
**(1) The spatial grid made enemies UNHITTABLE.** Projectile collision queries `SpatialGrid`
instead of scanning the pool, so an entity the grid does not return cannot be shot. It lost them two
ways: a cell that filled past `SGRID_PER_CELL` (16) discarded the remainder, and an entity outside
the grid's +/-128 m span was skipped entirely. Measured: **5643 overflow events in 3 h across 7 of 9
classes, up to 15 entities at once** — a tight pack was intermittently immune to every projectile in
the game, reported only by a debug-build `LOG_WARN`. There was a SECOND truncation on the same path:
the projectile candidate buffers were a hand-typed `u16 nearby[72]` under the comment "3x3 cells x 8
per cell max" and stayed 72 when `SGRID_PER_CELL` was raised to 16, so a dense 3x3 block (144) was
silently halved on every query. Fixed with an **overflow list** every query appends — cell cap and
world span now affect PERF, never visibility — and `SGRID_QUERY_MAX` (derived, `9*SGRID_PER_CELL +
MAX_ENTITIES`) sizes the buffers, so they cannot drift from the grid again. Pinned by
`tests/world/test_spatial_grid.cpp` (pack a cell, stand outside the world, do both, kill half the
pack) — all four fail on the old code by construction.
**(2) The world-item pool discarded the NEW drop.** `MAX_WORLD_ITEMS` is 64 and legendaries/mythics
NEVER despawn, so a deep floor accumulates them until every slot is taken — and from then on
`spawn()` refused whatever arrived next, which is as likely to be a mythic as one of the sixty
commons sitting there waiting out a 60 s timer. **1448 losses in 3 h**, entirely on deep floors (a
class that never left Normal saw zero). Now a **STRICT UPGRADE** eviction, mirroring the backpack's
`autoEvictWorst`: make room by dropping the cheapest thing present, but only when it is genuinely
worse. Equal rarity is declined — equal is not better, and allowing it re-creates the churn shape
that once respawned one item 699 times in 26 minutes. Sentinels (shard/shrine/chest/stash/waypoint/
gate) and pet consumables are never evictable, which is why `spawn()` gained an optional def table;
`spawnEssential` now shares the same `findEvictable` ranking instead of open-coding a second,
lifetime-only one that would have taken the Source shard's slot first. **No wire change** —
`MAX_WORLD_ITEMS` is in `SnapWorldItem`'s layout, so raising the cap would be a PROTOCOL bump and is
a separate decision; the policy fix needs neither.

**TOP-OF-TABLE PAYOUT HALVED (2026-08-04, Aaron's call).** Legendary base 2% -> **1%**, ramp
0.5 -> **0.25%/level**, ceilings 3/4.5/6/7.5 -> **1.5/2.25/3/3.75%** per difficulty tier, and
`MYTHIC_SHARE_OF_LEGENDARY` 25% -> **20%** so the top rarity falls by more than the halving alone
(Inferno mythic 1.875% -> 0.75%). All four numbers are named constants in `item.h` and
`ItemGen::legendaryCeiling(tier)` is shared by the roll AND by the test that pins it — the ceiling
used to be the literal `7.5f` in both places, which is exactly how a rate change leaves a stale
assertion passing for the wrong reason. This also relieves bug (2) above at the source: fewer
never-despawning legendaries means the 64-slot pool saturates far more slowly.
**It exposed a real content gap, which was PRE-EXISTING.** The balance lab's "every build cell
fields a weapon" test started failing — but measured across 1200 windows the MAGIC column starves
**5.7% of the time on the OLD rates and 6.7% on the new**, so the cut revealed a thin spot rather
than creating one (the test's 5 fixed trials had simply never landed on one). Cause is structural,
not a band gap: the Magic column is served by a SINGLE weapon subtype (`wand`, 6.3 total dropWeight)
while Melee spans four subtypes (21) and Ranged nine (43) — so a drop window can contain no wand at
all. Fixed by re-weighting the non-legendary wands **x3** (relative weights preserved — Arcane Staff
was deliberately 0.7), which puts the Magic column at 18.9 against Melee's 21. Measured sweep:
starvation **66 -> 12 -> 2 -> 0** per 1200 windows at dropWeight 1.0 / 1.6 / 2.2 / 3.0. The trade is
that every class now sees more wands in the general loot stream, since drops are not class-filtered.

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
**The CSV is locale-hardened (2026-08-01) and its numbers can now be trusted on any machine.**
`fprintf("%.2f")` follows `LC_NUMERIC`, so on a comma-decimal machine (de_DE here) every float was
written as `8096,0` — a comma inside a comma-separated file. The 33 columns became ~50, every column
after the first float shifted, and `balance_chart.py` clamped the unparseable values to 0. Nothing
warned; a deep-tier figure read off such a file went into this document as fact. Both CSV writers now
pin `LC_NUMERIC` to "C" for the write and restore it after (the guard lives in the WRITER, not the
call site, so `LC_ALL=C ./dungeon_tests` fixing one invocation can't leave the trap armed for the
next). `csvQuote` additionally DROPS commas and semicolons from the boss-name label: quoting alone is valid
CSV and `csv.DictReader` handles it, but "Ygara, the Broodqueen" split 162 of 1350 rows into 34 fields
under any `awk -F,`, which once misread `ttkBoss` as `hitsToDie` and produced "70 hits to die". Every
row is now exactly 33 fields for every parser.
**Two DIALECTS, because `,`/`.` is not universal** (`BALANCE_CSV_DIALECT=de`): the default
international form (`,` separates, `.` decimals — what the chart tool, CI and awk want) and the German
one (`;` separates, `,` decimals) that a de/fr/nl spreadsheet opens with a double-click instead of
dumping every row into column A. Those are the only two SELF-CONSISTENT combinations; the bug above
was the third. Rows are always FORMATTED in the C locale and translated afterwards, so the output
depends on the dialect alone and never on the machine. `tools/balance_chart.py` SNIFFS the delimiter
from the header and swaps the decimal mark, so it reads either file and both parse to identical
values (verified: 1350/1350 rows equal). The label sanitation drops BOTH delimiters for this reason —
a first cut rewrote the comma to a semicolon and was instantly wrong in German mode, having injected
that dialect's own separator into the data. Pinned by "balance CSV is locale-independent", "balance
CSV German dialect uses ; and comma decimals", and "boss labels carry no CSV punctuation" (which fails
the day a boss name gains a `.` — the German swap is blind and would render it "St, Ulrich") in
`test_balance_lab.cpp`; the locale guard was verified by sabotage.
Phase 2 (pending): chosen target bands become REQUIREs in `test_balance_lab.cpp` so CI fails
when a content/constant change knocks a floor out of band.

**Assets are GENERATED, not committed** (`assets/meshes/*.obj` is gitignored). The mesh table is `src/engine/asset_manifest.h`; `tools/build_assets.py` **hard-fails** if the engine names a mesh it doesn't generate, so a mesh added to one and not the other can no longer ship as an invisible fallback cube. Adding a mesh means editing **both**. (Full trap: `engine-how-to` → Pitfalls.)

## Architecture

**Data-driven hybrid.** JSON in `assets/config/` defines content (items, affixes, skills, enemies, weapons, materials). C++ systems load defs at startup into fixed-size arrays and consume them at runtime. Asset name strings are resolved to integer IDs (mesh IDs, material IDs) once after init — runtime code never touches strings.

**Level meshes MUST be freed before a rebuild.** `LevelMeshSystem::buildAll` is called from SIX sites —
every floor build plus the in-place rebuilds (exit pad, boss arena, lava theme) — and `buildSection`
resets `submeshCount` and overwrites each `SectionSubmesh` with a fresh `MeshSystem::create`. For most
of the project's life `destroyAll` was called from exactly ONE place (`engine_init.cpp`, at shutdown),
so every rebuild ORPHANED a whole floor's worth of VAO/VBO/IBO. Measured by sampling `VmRSS` against
floors cleared: **+2.85 MB/floor, monotonic, never plateauing** (120 -> 149 MB over 10 floors; an
autoplay soak reached ~360 MB). `buildAll` now calls `destroyAll(outSections, maxSects)` on entry —
the full array, not just this build's section count, so the tail is reclaimed when a smaller grid
follows a larger one (grids vary 44/48/52). After: RSS **PLATEAUS** — over a 28-floor / 5-boss run it
rose 118 -> 138 MB in exactly FOUR discrete steps (+6.3/+2.8/+6.3/+4.0) and then sat at 138.4 MB for
the last TEN floors, +0.0 MB each, two boss spawns included. Those steps are lazy first-use loading
(depth-tier themes/assets, a larger grid), NOT per-floor cost: they do not align with boss floors, and
the marginal cost of a floor at steady state is zero. Read a per-floor AVERAGE with suspicion here —
"+0.74 MB/floor" is just those four warm-up steps amortized over 27 floors and describes nothing real.
A new call site needs no extra care; adding a rebuild path that bypasses `buildAll` does.

**`AllocationTracker`'s "bytes still live at shutdown" is NOT a leak measurement** — do not chase it.
`operator new`/`new[]` always add to `s_liveBytes`, but the UNSIZED `operator delete`/`delete[]` free
without subtracting (only the sized overloads do), which its own comment admits ("the byte count will
drift"). The number therefore grows with allocation CHURN, so it correlates with floors cleared whether
or not anything leaks — it pointed at a real leak once, by luck, and would have pointed at one anyway.
Ground truth for a leak is process **RSS sampled against a progress counter**; C-side `malloc`/`calloc`
(e.g. all of `level_grid.cpp`) is invisible to the tracker entirely.

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

**Melee weapon throw (short-click).** A quick TAP of Fire on an equipped MELEE weapon hurls it as a
projectile (`Engine::spawnWeaponThrow`) — HOLDING Fire does the normal swing. It deals the effective
weapon damage with one crit roll, flies as the weapon's own mesh, and applies a brief slow on hit
(rides `Projectile::freezeDuration` → `Entity.freezeTimer`, tenacity-scaled). Cooldown is a FIXED
1.5 s that CDR never touches — a plain float timer (`m_weaponThrowCd[lane]`) for the local/host/SP +
client-prediction path, a tick watermark (`NetPlayer::weaponThrowLastActivationTick` +
`GameConst::cooldownReady`) for server-authoritative remotes. **The client owns the tap-vs-hold
arbitration** (pure, tested `game/weapon_throw.h` — decide()/cooldownTicks()/slowDuration()); the
server never re-derives it because swings/throws reach it as explicit requests/edges (`CL_FIRE_WEAPON`
/ `INPUT_EX_THROW`), so a tap sends the throw edge and a hold sends the fire request — never both. To
keep melee responsive, the ~0.2 s tap-disambiguation window only applies while the throw is OFF
cooldown; on cooldown a press swings instantly. Full co-op: the throw is client-predicted (a ghost, as
the THROWAWAY legendary does) and reconciled via ownerSlot+clientTick; `INPUT_EX_THROW` (extFlags bit
6) drives it — **`PROTOCOL_VERSION` 25**, no snapshot layout change (reuses the projectile wire).
**Tenacity is a live-but-zero hook**: `Entity.ccResist` defaults 0 for every enemy, so the slow is
scaled by `CrowdControl::scaleDuration` to the full 0.2 s today; populating enemy resist later needs
no code change. A **visible amber recharge bar** sits below the crosshair (mirrors the dodge-roll
cooldown bar), shown only while a melee weapon is equipped and the throw is cooling.
**The Autoplay bot throws too.** It drives the same buttons a human does, and `applyBotIntent` HOLDS
Fire — which the arbiter reads as a swing — so throwing means emitting a synthetic short **TAP**:
release (to zero `decide`'s hold accumulator, since an auto-firing bot is far past `TAP_SEC`), a brief
press, then release (the release is what throws). That sequencer is `WeaponThrow::botTap*` +
`m_autoplayThrowSeq`, and while it runs it OWNS the Fire button, or the ordinary auto-fire hold would
leak through and turn the throw back into a swing. Policy is the pure `WeaponThrow::botShouldThrow`:
a **RANGED** enemy with LOS, beyond swing reach (`BOT_THROW_MIN_MUL`× weapon range) and inside
`BOT_THROW_MAX_M` — a melee attacker is excluded because it closes the gap for you, so the weapon is
better kept in hand. Leashed (`BOT_THROW_LEASH` 6 s) so it reads as an occasional flourish; measured
live at ~1 throw/13 s. The **THROWAWAY legendary** gets the same treatment on its own 8 s leash: that
gun toss fires whenever a reload TRIGGERS and its damage scales with the rounds left, while a manual
reload is only legal below a full clip — so the bot pulses RELOAD at exactly **clipSize-1**, the
highest-damage toss the rules allow.

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
ranked a Heavy Crossbow 3.5x a Rusty Dagger whose real DPS is higher; **Ranged-column weapons carry a
per-hit TIEBREAK** — `weaponSkillScaleBonus`, because Marksman/Ranger skills scale off the weapon's
per-hit damage (`s_weaponDamage` in `skill_marksman/ranger.cpp`), so between two comparable-DPS ranged
weapons the harder hitter wins, without ever overturning a real DPS gap), hard weapon-family gate per column, **a CLASS preferred-weapon term the COLUMN cannot express** (the
Ranged column holds BOTH the gun/HITSCAN and bow-thrown/PROJECTILE families, so a Combat Engineer and
a Ranger share a column while only one of them earns the +20% on any given weapon — without it the
scorer ranked a bow and a pistol on raw stats and could hand the engineer the bow, silently costing
20% damage; `WeaponType::COUNT` is the "no class known" default, and every entry point —
`score`/`bestSlotScore`/`gearScoreForCell`/`worthPickingUp`/`isKeeper` — takes it so the equip rule,
the pickup filter and the prune can't disagree and churn), **defense in EFFECTIVE-HP terms** (the engine's armor formula linearizes to +armor% of a 150-HP reference pool; %HP likewise; regen/life-on-hit/**lifesteal count as TANKINESS** — healing over a 10 s reference fight — not offense), **spell rolls + cooldown reduction multiply a per-column reference cast-DPS** (70 for Magic, 15 elsewhere — CDR is 1/(1-c) casts, real DPS on a caster; **CDR on a NON-weapon — a helmet, a ring — is ALSO credited as weapon-cooldown acceleration**, since `getEffectiveWeapon` divides the weapon cooldown by 1-CDR for every class, which is what lets a Glass Cannon prefer a CDR helmet over a defensive one), **legendary GRANTED SKILLS scored by their real DPS-equivalent** (`skillOffense`/`skillDefense` reduce the actual `SkillDef` — damage/cooldown/bounces — to the weapon-DPS currency under a "30 s on a dummy" lens, precomputed onto `ItemDef::legendarySkillOffense/Defense` at load in `engine_init_assets.cpp` so the pure scorer never needs the skill table; only counts at LEGENDARY rarity; a stat-less reactive skill with no `SkillDef` — Mirror Aegis's projectile parry, "super strong" per Aaron — is hand-valued in the resolver, the one exception to stat-derivation because there is nothing to read), **the CLASS's preferred-weapon +20%** (`BuildScore::CLASS_PREFERRED_MULT`, folded into `perHit` so it
reaches both the DPS term and the ranged skill-scale tiebreak, mirroring `wpn.damage *= 1.2f`), **3.5:0.5 Glass / 2:2 Moderate / 1:3 Tanky**
offense:defense row weights (Glass sharpened from 3:1 so it RELIABLY picks the damage piece over a max-roll defensive one), 5% upgrade hysteresis, rarity tiebreak. Selecting a cell re-gears the
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
**The definitive-best override:** the legendary **Phase Dash boots** (Swift Boots — slot BOOTS,
`legendarySkill phase_dash`) are an absolute — a blink with i-frames trumps any stat roll — so
`BuildScore::isDefinitiveBest` makes the auto-looter wear them over any NON-Phase-Dash boots and never be
displaced by one (`autoEquipIfUpgrade`). The protection is for the **BEST pair only**, not every copy: a
better-rolled Phase Dash boots DISPLACES a worse one (two definitive boots fall through to the normal
hysteresis compare), and a worse DUPLICATE is treated as ordinary gear — `worthPickingUp` grabs a Phase
Dash boots only when we field none or a strictly better one, `isKeeper` keeps only the best fielded pair,
and `autoEvictWorst` may evict a worse duplicate (all three via `bestFieldedDefinitiveScore`). Deliberately
a SLOT-level override, NOT a giant `score()` bonus: inflating `score()` would swamp the cross-cell
better-build nudge (it sums whole-build totals and would go blind if one shared slot carried a huge
constant), so `score()` stays honest and the "always" lives in the per-slot pick.

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

**The bot now PLAYS The Source, and dying there no longer throws you out of the world (2026-07-30).**
Three separate bugs in the secret-boss chamber, all found by the same 3 h couch soak. (1) **The bot
idled in it.** `BotView::onNormalFloor` lumped the Source chamber in with town/arena as "a world the
brain cannot express", and the brain returns an EMPTY intent when that is false — so three of nine
soak sessions collected all ten shards across a full Hell run, opened the portal on floor 50, walked
in, and then stood still for the remaining ~2 hours. They were the ONLY silent sessions and the only
ones that entered; earning the secret fight and then refusing to play it is the worst of both
outcomes. The chamber is a FIGHT, not a traversal, and nothing else had to change to support it:
`floorDoorActive` is false by construction (so DESCEND stays disarmed), the flow field is seeded at
the centre where the Engine stands (so TRAVEL walks toward the fight), and `pickTarget` already skips
an invulnerable target (so while the Engine is shielded the bot fights the adds). Measured: **0 -> 376
combat actions** on entry. (2) **`enterSourceChamber` wrote the spawn to `m_localPlayer` only** — the
swap ALIAS — and relied on being called solely from inside the per-player pass, where `swapOutPlayer`
persists it. From anywhere else the next frame's `swapInPlayer` restores the stale floor-50 position,
which in the freshly-built chamber grid is **outside the world**. `enterTown` and the CLIENT mirror
both already had the persist line; the host path did not. (3) **`spawnPosition` was never re-seeded**,
and that one is human-facing, not just a bot problem: it is the RESPAWN anchor every revive path
teleports to, and it is otherwise written only by `startGame` — so **dying in the secret boss fight
put any player outside the world with no way back**. `enterArena` already re-seeds its pads; The Source
was the one relocating world that did not. `--source` is the new dev door (it needs `startGame` FIRST,
unlike `--town`/`--arena`, because the transition wipes the current floor and moves the live player in)
— without it the one world an autoplay bot could enter but not play was effectively untestable, which
is why all three of these survived until a soak reached it three times by accident.

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
descends — a plain continuous hold wedged the bot next to a used shrine forever. **SHRINE detour:** a shrine
is a free timed buff sitting in the level, so on FLAT non-lava floors `buildBotView` steers travel onto the
nearest active one within an 8 m detour (folded into `flowDir` like the globe/boss detours — TRAVEL-only, an
LOS enemy still preempts it), and `updateAutoplay` holds interact (the same `descendPulseHeld` pulse — a hold
routes to the shrine over the exit, one hold consumes it) once in reach, stopping inside the 1.2 m grab
radius. Live: 2-4 shrines used per short run. **H = instant handoff, window STAYS VISIBLE:** pressing `H` in an
autoplay run calls `m_autoplayControl.forceBot()` (skip the 2 s resume window), arms a ~4 s
`m_autoplayHandoffGrace`, and calls `Input::releaseCursorOnce()`. The **grace is load-bearing**: the takeover
latch hands control to the human on the FIRST input it sees, so without it the bot handed straight back the
instant the player moved the mouse toward another window — "handover with H doesn't work". While the grace is
live, `humanActivityThisFrame()` is forced false into the latch, so the bot keeps control through the
switch-away; once the player clicks off, the game is unfocused and input is gated anyway. `forceBot` runs
AFTER the latch tick so the H keystroke's own activity can't undo it. `releaseCursorOnce` frees the pointer
that relative-mouse mode locks to the window centre so the player can click/alt-tab to another window WITHOUT
the game being minimised (the earlier `Window::minimize()` HID the window; the player wanted to keep watching
it play) — a one-shot that leaves the WANTED mouse mode (`s_relativeMode`) intact, so the aim still works if
the player tabs back and reclaims control, unlike `setRelativeMouseMode(false)` which would leave it dead.
**TOWN portal:** the hub is
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
**A live milestone BOSS is the one exception** — the exit is SEALED until it dies (`mayDescend` refuses,
and every exit-seeking watchdog is gated OFF while a boss lives), so a boss with LOS is engaged and closed
on from ANY range rather than ignored for being across its arena. Both the FIGHT gate and `pickTarget`'s
sticky-range release exempt `BotTarget::isBoss` (they MUST agree). And because the exit portal sits at the
boss room's CENTRE (where the boss spawns) but a major boss's arena is 4× its room, the bot used to reach
the sealed door and idle with the boss beyond the ceiling; travel on a boss floor is now **GOAL
SUBSTITUTION** — the boss-seeded wall-aware `RouteField` IS the travel field while the boss lives, with a
clear-line straight bearing only as the last-metres fallback (see "BOSS FLOORS" further down for the full
design; two earlier assist-shaped versions are recorded there as measured failures). The clear-line rule's
lesson stands: a straight bearing through a WALL jams the bot into it (measured: frozen 8 m from the boss —
"it wants to navigate to the boss even behind a wall"). Live: killed The Butcher (floor 5) and Ygara
(floor 10, 20×24 arena) and descended past each. **A fleeing LOOT GOBLIN is rushed the same way and harder** —
`BotTarget::isLootGoblin` wins `pickTarget` outright (nearest visible one, bypassing stickiness/ceiling),
is engaged from any range, and `decideCombat` chases it flat out (`moveFwd`, never kite/strafe) since it
never attacks and its escape clock is running. **INVULNERABLE enemies are never the shot target** —
`pickTarget` skips any `BotTarget::invulnerable` (a dormant AMBUSH gargoyle, an entombed boss, the shielded
Engine — mirrored from `Combat::applyDamage`'s early returns; `minionShield` is only 75% reduction so it
stays a target), which stops the bot wasting shots and, for a gargoyle, stops the stare that keeps it
asleep. They stay in the list for the block/dodge scans (they can still attack).
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
watermark), plus `BotView.skillIsAoe[4]` from the `SkillDef` (shards / bounces / multi-projectile / a ≥3 m
blast). **Skill SELECTION** (`decideCombat`) is no longer "lowest castable slot" — that left a Sorcerer
spamming Fireball while its deep pool and Frozen Orb / Chain / Meteor went unused. Now: a **GROUP**
(`GROUP_MIN`=3 hostiles within `GROUP_RADIUS`=6 m of the aim target) fires the **biggest castable AoE**, any
class; else **EVERY class dumps biggest-first** — 2026-07-31, Aaron's design; the earlier
martial-keeps-the-cheap-filler rule is RETIRED (see "CLASS AI: every class dumps its kit" further down for
the in-reach/in-band gates and the reactive COUNTER-skill carve-out). Verified live: a floor-12 Sorcerer
selects Frozen Orb (slot 1) ~89% of casts. **The class-skill press is PULSED, not held** (`applyBotIntent`,
even ticks only): activation is edge-triggered (`isActionPressed`), and because *some* skill (cheap Fireball)
is almost always castable the button would stay held every engaging tick and the edge would fire **once per
fight** — the "not aggressive enough for a Sorcerer" bug. Pulsing makes every other tick a fresh press edge
(30/s, far above any cooldown), so the engine's own per-skill cooldown becomes the true cast rate (live:
energy drained 150→32 under sustained casting, vs staying full before). BOOT/HELMET skills need no pulse —
each is a single slot that self-releases the instant it goes on cooldown. Availability is mirrored rather
than guessed precisely because a press that no-ops is worse than no press. **GAP-CLOSE skills** (a teleport/
dash — Holy Smite, Shadow Step/Strike, Phase Dash: `SkillDef.distance > 0`, flagged `skillIsGapClose[]` /
`bootIsGapClose`) are cast to CLOSE the gap to a target BEYOND reach: `decideCombat` fires one when it is
walking up to the target (`moveFwd`) and already roughly facing it (the blink follows the facing), which
is OUT of range and so complementary to the in-range damage skills. A gap-close EQUIPMENT skill (Phase Dash
on the always-worn Swift Boots) is withheld from the in-range block — blinking 6 m while already on top of
an enemy overshoots past it. The **EQUIPMENT legendary rails ride the same
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
**PERFECT-BLOCK PACING (feel — the driver, not the pure tap).** Perfect-blocking EVERY swing reads as a
machine ("the bot is too good at perfect blocking"). The driver now paces it: the bot lands a STREAK of
perfect blocks (`BLOCK_STREAK_MIN..MAX` = 2-3 in a row, re-rolled each cycle), then takes a human LAPSE — an
"unreliable" window (`BLOCK_UNRELIABLE_SEC` 2.5 s) where only `BLOCK_UNRELIABLE_PCT` (40%) of swings still
land a perfect block and the rest are mistimed (the block is dropped, so the hit is eaten) — then it is
sharp again. Applied in `updateAutoplay` right before `applyBotIntent`, per SWING (a raise want spans ~9
ticks, so it acts on the rising edge and latches the outcome via `m_autoplayBlockSuppress`); deterministic
(tick-hashed, no rand, replay-safe). Measured (fresh warrior): perfect-block rate ~100% → **76%**, the
streak→lapse cycle plainly visible. Constants in `autoplay_combat.h`, state in `engine.h`.
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
**The FLAT-FLOOR FREEZE was a three-defect chain in that machinery, each hiding the next (found by tracing a
frozen bot; verified fresh Sorcerer floor 4→13).** A "freeze" is really a **swarm-kite LIVELOCK**: the bot
kites a swarm of FLYING enemies it can't close on, which drags it off the exit; `kills` flatline while it
churns, so it read as dead-stuck. (1) **The exit bull released on any `combatProgress`** — the swarm always
takes a little chip damage, so the bull dropped the instant the bot fired and the kiting bounced it back off
the door it was 4 m from (measured: `distToDoor` oscillating 4↔26 m for minutes). Now the bull is a
**committed latch**: once it engages (only after 4 s of proven no-approach) it holds until the bot descends;
chip damage no longer talks it back into the swarm. (2) **The bull then drove into a WALL** — on a maze its
A* first-leg gives up after `MAX_ASTAR_SEARCH` (256) cells and fell back to a straight BEE-LINE to the door,
into geometry; now it falls back to the **uncapped, wall-aware exit FLOW FIELD** instead. (3) **The
stuck-detector was fooled by OSCILLATION** — sliding along a wall / orbiting a pin moves >0.5 m every tick,
so `progressed` re-anchored forever and the escape ladder never engaged; a **SLOW anchor**
(`m_autoplaySlow*`) now checks NET travel over ~2.5 s and, only when the bot is also dealing no damage (a
stationary real fight still counts), lets the no-progress timer climb so the escape ladder fires. Plus
**SURVIVE is now sacred** — a low-HP potion the brain wanted is re-asserted AFTER the whole remedy chain
(`decidedPotion`), so the now-longer committed shove can never march the bot to its death holding an undrunk
potion. (The FOUR_STORY return-lift STORY-BOUNCE — a ranged build cycling stories 9→6→3→0→9 on a Descent
floor — is a SEPARATE, still-open descent-geometry problem, not this flat-floor livelock.)
**The bull is a LAST RESORT, not a run-to-the-exit default — the bot must FIGHT its way through floors.**
Three tunings enforce that. (a) The exit-progress window is **16 s, not 4 s**: 4 s of no-progress is not
"can't get past", it is "this enemy is not a pushover" (an armored foe, a kiting build repositioning, an
add), and a short window bailed the bot straight out of winnable-but-slow fights. The window RESETS on any
damage dealt, so a real fight never accumulates it; only a bot that deals no damage AND makes no exit
approach for 16 straight seconds — a genuine unkillable-swarm livelock — trips it. (b) Once latched the
bull **releases on a KILL** (`killedThisTick` = `targetCount` fell): a kill means combat is viable again, so
control hands back to the FIGHT branch — it stays committed only while chipping something that will not die.
(c) A **flat-floor PUNCH-THROUGH**: while the bull walks a FLAT floor, a body-blocking swarm can shove a
fragile build in circles (moving 15 m of churn, never CLOSING on the door — the netStuck slow anchor misses
it because it IS moving), so the bull DODGES toward the exit (i-frames + a ~4 m lunge slide past the bodies,
pulsed at the engine dodge CD, overriding the balance leash). Dying mid-punch is fine — routing OUT is the
goal. Stacked floors are excluded (a horizontal roll could carry the bot off a balcony/ramp edge; VHALL
climb-roam and FOUR_STORY bounce are separate up-routing problems). Measured, paired 10-min runs: raising
4 s→16 s dropped bull-active from **7-10% of ticks to 0%** across Marksman/Warrior/Sorcerer while kills held
(136-304) and floors reached held/improved — the bot fights the whole time and the shove only fires on a
true livelock.
**A remedy may only STAND STILL where the descend can actually fire.** The exit-wedge remedy engaged at
2.5 m while `updateFloorDoor` descends at **2.0 m**, so between the two it planted the bot holding a button
that could never fire — and standing still IS "no progress", so the remedy re-armed itself forever
(measured: 73 consecutive seconds frozen beside an open exit). `Autoplay::DESCEND_RADIUS` / `DESCEND_STOP_M`
(1.9 m) single-source that, with a `static_assert` + test pinning stop < radius; outside the radius the
remedy WALKS THE LAST METRE IN (interact still held) instead of parking, bounded by the no-progress timer so
it can never shadow the geometry escape.
And **LOOK BEHIND at 3 s — floors 1-10 ONLY** (its whole job is the early-floor gargoyle standoff, and off
those floors the spin-around reads as odd; the escape ladder handles other wedges). The first rung, before
the 4 s geometry ladder. The dungeon's stone gargoyles
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
**Descent (FOUR_STORY) routing — the maze needs a FIELD, not a bearing.** The bot's travel goal on a
Descent floor is a hole in its own story's slab, and the first two attempts at steering to one both
failed the same way: they produced headings that pointed at walls, and the bot scraped along them
("the time goes right now looking and hugging the walls and corners"). A straight-line bearing at the
nearest hole is hopeless on a braided maze (measured over 150 s: the distance to the chosen hole GREW
on 61 samples and shrank on 53 — a random walk). Plain 2D **A\* is exact** here in principle, since all
four stories share one full-height wall skeleton — but `Pathfinder::findPath` gives up after
`MAX_ASTAR_SEARCH` (256) closed cells, which on 3-wide corridors is barely a dozen cells of travel, so a
hole across the floor returned nothing and the code fell back to the wall-pointing bearing. So routing is
a **BFS flow field seeded from this story's clean drop holes** (`game/autoplay_descent.{h,cpp}`,
`Autoplay::DescentField`), rebuilt only when the bot changes story or floor (~2k cells, three times a
floor). The staleness stamp for both story fields (this one and the VHALL field) is the floor's **seed
identity** (`levelSeed + floor*7919 + difficulty*104729`, the same fold `startGame` builds the dungeon
from), NOT the bare floor number: floor numbers repeat across runs and difficulty tiers while the forced
grid sizes and fixed 3 m story pitches also match, so a floor-number stamp resurrected the PREVIOUS
maze's field and routed the bot on geometry that no longer existed. It mirrors
`LevelGridSystem::buildFlowField` — same encoding, 4-connected expansion, and the
**steer-at-the-next-cell-CENTRE** readout, which is the anti-wall-hug rule — and differs in two things:
it is seeded from holes, and it **excludes `CELL_JUMPPAD` cells entirely**. Three further rules make it
work, each from a measured failure:
- **The story reference is the SLAB UNDERFOOT, not raw feet-Y** (`Autoplay::botStoryY`, via
  `effectiveFloorHeight` with the step tolerance subtracted back off so a jump can't report the storey
  above). The bot jumps constantly, and matching holes on `|surfaceY - pos.y| <= 0.4` rejected every hole
  on its own storey for the whole flight — 21-27% of all ticks had no hole pick at all, 100% of them
  airborne, and the router silently went dark.
- **…and that reference is HELD with hysteresis** (`Autoplay::commitBotStory`, driver state
  `m_autoplayDescentStory`, reset per floor). Even reading the slab underfoot, `botStoryY` is a knife-edge
  at a drop-hole LIP: a 0.2 m dip in feet-Y (or a few cm of XZ drift onto the hole cell) drops the raw
  reading a full storey, because the storey's own slab then sits just outside `effectiveFloorHeight`'s
  tolerance window. The field reseeds per storey and the two seedings point OPPOSITE ways, so the raw
  reading alone left the bot **oscillating at the very hole it should drop into** — measured live as
  **three builds each frozen 19-37 min on a FOUR_STORY floor** (Sorcerer floor 19, Marksman floor 29;
  the melee Warrior stumbled into holes by closing distance and got through). `commitBotStory` only moves
  the committed storey once the bot is SOLIDLY standing on a different one (feet within `kOnStoryBand`
  0.6 m of that storey's slab), so a lip flicker is ignored and a genuine fall commits only on landing —
  which is exactly the descent.
- **The field is seeded EVERYWHERE the bot can be — three seeding rules, each a measured un-freeze.**
  The single-pass, holes-only field left the router BLIND in three spots, and in each the bot fell back to
  the shared flat exit field, which does not dodge pads and on an upper storey routes toward ABOVE the L0
  door — so the bot froze or bounced instead of descending (measured on the committed hysteresis-only
  binary: Sorcerer 51 min on floor 9, Warrior 42 min on floor 19). (a) **TWO TIERS.** Tier 1 floods from
  clean holes over non-pad cells (the preferred descent); tier 2 re-floods from the whole tier-1 network
  ALLOWING pads, filling any pocket the pad-exclusion severed from the holes (a severed cell read 0xFF
  forever → no heading → froze next to a pad). Tier 2 only touches cells tier 1 left unreachable, so an
  unsevered floor is untouched. (b) **L0 SEEDS FROM THE EXIT.** A storey with no holes is L0 (the bottom);
  seed the field from the exit DOOR instead of returning invalid, so L0 routing is a pad-avoiding walk to
  the door — the shared flat field launched the bot back UP a return lift the instant it reached L0 (the
  last-metre stall). (c) These make the field VALID on every storey, so the bot always has a pad-avoiding
  descent heading. Residual: in HEAVY combat a fragile build's un-vetoed FIGHT movement can still push it
  onto a pad and bounce it up — slower, but it descends; the geared (combat-trivial) bot now descends
  Descent floors cleanly where it used to stall forever.
- **Jump pads are a HAZARD on this floor and only this floor** (`stepAllowed(..., avoidPads)` for travel,
  `Autoplay::padAhead` for combat movement). A pad lifts ~two stories and fires the instant you are
  grounded, so one kiting step onto one throws away a descent: a run that had reached L0 and closed to
  21 m of the exit ended up spending 61% of its time back on L2. The pad veto is **BODY-AWARE**: the launch
  (`Collision::jumpPadSpeed`) fires when the ~0.35 m halfWidth FOOTPRINT overlaps any pad, not just when
  the centre does, so a centre-cell-only veto let the bot clip a return lift with its SIDE and get flung up
  — `cellPassable` now tests the footprint corners to match. Carve-outs: the veto stands down while the bot
  is ALREADY on a pad (else a 3x3 pad node boxes it in) and on a `paddedOnly` storey, where every way down
  is a return lift and refusing them would leave the bot circling a hole it may not enter.
- **A kill-agnostic FLOOR-STALL watchdog.** The existing exit watchdog restarts on every point of damage
  dealt; on a floor carrying four stories of enemies the bot deals damage continuously, so it **latched 0%
  of the time** across three measured runs while the bot fired on 50%+ of ticks and never left floor 1.
  The long window asks only "have you got closer to the way out in the last **20 s**" (`distToDoor` is 3D, so
  descending a storey counts outright) and arms the existing combat break-off leg — not the exit bull,
  which A\*-routes in XZ and above L0 would march the bot to a spot three stories over the exit. The window
  is **20 s** (was 6 s): because it is kill-agnostic, a 6 s window fired mid-fight against a STRONGER enemy
  (a champion/elite legitimately takes many seconds to kill) and made the bot "randomly disengage" from
  exactly those fights; 20 s lets any real fight resolve first so it fires only on a true livelock.

**A resting body reported AIRBORNE every other tick (`onGround`), and it gated more than it looked
(measured 2026-07-29).** Found while tracing a VERTICAL_HALL stall: a bot standing perfectly still on
flat ground — `position.y` pinned, `velocity.y` never positive — was grounded on exactly **150 of every
300 ticks**. `Collision::moveAndSlide` applied gravity only when `!onGround`, so a grounded body had
`velocity.y == 0` -> `delta.y == 0` -> the swept Y position equalled the current one and did not overlap
the grid, so the landing branch that re-sets `onGround` never ran and the unconditional
`onGround = false` at the top of the Y axis stood; the next tick applied gravity, overlapped, landed,
set it, and it alternated forever. Gravity is now integrated **unconditionally** — the landing snap
re-zeroes it, so a resting body neither sinks nor drifts and still reads `velocity.y == 0` after the
call. Player overloads ONLY: the entity path deliberately treats `velocity.y != 0` as the airborne
state, and always-on gravity there would break enemy floor-snapping. It matters because `onGround`
**gates** the jump (a request is dropped unless grounded), the Autoplay VHALL fall veto (off on the odd
ticks, so a "protected" bot could still step off a balcony), and the grounded-only navigation rolls —
each ran at half rate or worse — and it is snapshotted for co-op. Pinned by
`tests/world/test_grounded.cpp` (the failing case was exactly 30/60). A paired A/B on two frozen
binaries (6 classes x 30 min, same GPU load) measured **63 floors vs 59** — a wash, so it costs nothing
in traversal. An earlier single-arm comparison suggested a 34% regression; that was seed noise, and the
per-class swings (3 vs 14, 7 vs 16) show why a controlled arm was needed.

**You cannot walk under the low end of a ramp — one rule, three consumers
(`LevelGridSystem::bodyPinnedUnderSlab`, 2026-07-29).** A VERTICAL_HALL ramp is a graduated slab that
descends to head height and below, so the ground beneath its low end is a trap. `VHallField` had always
excluded those cells; **nothing else had the rule**, and each gap was a live bug. (a) The Autoplay
travel veto (`cellPassable`) did not, so every producer that steers by `stepAllowed` rather than by the
field — the escape ladder's 8-direction search, the +-45/+-90 detour fan, the jump-pad beeline — walked
the bot under the stairs. (b) The teleport landing resolver did not: `footprintClear` only asks whether
cells are SOLID, and a dash is flattened to XZ, so `desired.y` is the caster's own feet height, the
story selector resolves to the ground story UNDER the slab, and the caster is teleported inside the
stairs — a **human-facing** bug (Paladin Holy Smite; Shadow Step/Strike share the resolver), fixed by
rejecting such candidates and marching further back. (c) The FIGHT branch's close/kite movement is
deliberately UNVETOED, so a melee build chasing a hostile walked itself in anyway — so the pinch check
is also applied per-component to the FINAL intent, whichever producer wrote it (this is geometry that
swallows the body, not a tactical hazard to weigh), plus `Autoplay::unpinDirection` to walk a body that
is ALREADY under one back out (from underneath every direction reads as refused, so vetoing alone would
pin it for good; it ring-searches and takes the CLOSEST free cell per ring, because scan order returns a
corner diagonal when a straight step across the band is nearer and leaves sooner). The rule is
multi-slab aware (the ceiling that matters is the lowest slab underside ABOVE the feet, so a FOUR_STORY
body on the 6 m slab is measured against the 9 m one) and story-aware (fine when standing ON the slab),
and it is naturally inert wherever slabs sit at 3 m+. `BODY_CLEARANCE` = 0.8 m, single-sourced.

**A gap-close must not fire at a target only reachable by FALLING.** The fall veto stops the bot
WALKING off a balcony to reach a cross-gap enemy, but a teleport/dash bypasses the veto entirely — so
Holy Smite / Shadow Step / Phase Dash blinked across the very gap the veto (and the melee sidearm)
exist to hold it back from. `BotTarget::onlyReachableByFall` is computed ONCE in `buildBotView` and read
by BOTH consumers, so the sidearm (which draws a gun FOR such targets) and the gap-close suppression
(which withholds the blink AT them) can never disagree. Movement is untouched — it still closes, aims
and fires; only the teleport and the charging roll are withheld. VHALL-upper-exit only. This also
explains the "runs away from ranged enemies but also gap-closes" report: the sidearm flips the whole
doctrine (Melee `engageMin` 0.00 / `engageMax` 0.60 on a ~4.3 m reach -> everything past ~2.6 m is
"close in"; Ranged 0.55/1.00 on a ~24 m derived reach -> a 13-24 m hold-and-strafe band), so the bot
alternates charge/hold on the sidearm's 3 s dwell + 5 s cooldown. A melee build never actually
backpedals (`engageMin` 0.00); what reads as retreating is the STRAFE, made one-sided by the fall veto.

**The melee throw has its OWN viewmodel animation.** It used to set `attackAnimT` — the SWING timer —
so the viewmodel played the weapon's full per-subtype swing for an attack that never connects; on a
claymore that is its entire right-to-left horizontal sweep, which reads as the animation being broken.
`ViewmodelState::throwAnimT` now drives a wind-up -> whip -> release, and the hand goes EMPTY at
`THROW_ANIM_RELEASE` (the same trick the THROWAWAY legendary uses) so the weapon in the air is the one
that just left it. The trigger clears `attackAnimT` so a mid-arc swing cannot fight it. In flight, a
thrown MELEE weapon is drawn near its real size with a heavier tumble instead of being normalised to
the same 0.4 m as a knife or molotov (a hurled claymore rendered dart-sized); derived from the
already-replicated `meshId` via a mask built once at asset-resolve time, so it is client-side with **no
wire change**.

**COUCH CO-OP AUTOPLAY — every local lane gets its OWN bot (2026-07-29).** Autoplay was
singleplayer/lane-0-only because ALL of its ~84 state members were single-instance, even though
`updateAutoplay` already ran inside the per-lane swap loop — two lanes simply stomped each other's
target, timers, commits and flow fields. 66 of them now live in an `Engine::AutoplayLane` struct
reached through `ap()`, which keys off `m_localPlayerIndex` (the same alias mechanism the player
state uses), so no call site needed a lane argument. What deliberately stays GLOBAL: `m_autoplayActive`,
the human-takeover latch + handoff grace (there is one human), the run/floor telemetry, and the
pad-cell cache (floor geometry, identical for both lanes). Three couch-specific traps came with it,
each a real bug: the three heap nav fields are per-lane so **shutdown must free EVERY lane** (freeing
only `ap()` leaks lane 1's on every couch run); `enterAutoplayRun` seeds each lane's gear brain and
build cell from **that lane's own class** (seeding lane 1 from lane 0 is the Sorcerer-with-a-sword bug
one lane over); and the melee-throw wire latch `m_pendingThrowEdge` had to become **per-lane** —
`clientNetPre` consumes it inside its per-lane loop while `handleWeaponFire` sets it later in the same
frame, so a single bool meant the first lane sent next frame swallowed whichever lane latched, i.e.
**online couch P2's throw was credited to P1** (the v17 pickup bug's exact shape) and two throws in one
frame lost one. Split-screen also has **no GAME_OVER screen** — a dead lane sits in its own branch
waiting for a JUMP press with `gameUpdate` SKIPPED, so the bot is not running for it and can never
press anything; the per-lane dead clock synthesises the respawn (1.5 s bot / 5 s if control reads as
human, then `forceBot`). Dev door `--autoplay-couch [class]` (lane 1 defaults to Marksman so the pair
is melee + ranged); the menu's Autoplay row now also arms on the "Start Local Co-op" and
"Host online together" branches, which previously armed nothing at all.

**The VHALL "stuck on the stairs" stall — and the WRONG GEOMETRY STORY it was fixed against
(2026-07-29, corrected 2026-07-30).** Bots pinned at `y = 2.49-2.50` against a 3.0 m balcony — moving,
valid route, distance-to-door frozen, for entire floors. This paragraph used to explain it as: "a
ramp's final step onto the balcony is ~0.5 m and `STEP_UP_HEIGHT` is 0.4 m, so that step is unwalkable
by construction and a hop is the only way up." **That is FALSE.** `carveVerticalHall`'s `ramp()` raises
the graduated slab by exactly **1 qu = 0.25 m per cell** — half the step-up threshold — so every step of
every ramp is walkable and the top reaches the balcony height exactly; dumping the real slab profile
shows `0.25 0.50 ... 2.75 3.00 3.00`, no step over 0.25 m, and no ramp cell pinned under a slab. There
is no unwalkable riser and never was. The geometry now states itself in
`tests/world/test_vertical_hall.cpp` ("ramps are walkable end to end", 24 seed/size combinations) so
the theory cannot be resurrected by reading this file. The `vhClimbing` margin fix (`- 0.5f` -> `- 0.1f`,
plus dropping the stale absolute `pos.y < 1.5f` gate and the 4-in-72 duty cycle) is kept — it measured
1/8 -> 7/8 stall-free runs — but its EXPLANATION was wrong, so treat that A/B as "this helped", not as
evidence for a riser that does not exist. Two stale gates on the same hop had to go with it: an absolute
`pos.y < 1.5f` (obsoleted by `vhOnRamp`, which is what actually stops bunny-hopping the flat approach)
and a duty cycle of 4-in-72 ticks, far too thin on the final riser where the bot is grounded ~half the
time and combat owns most ticks — it now pulses 4-in-8 within one riser of the exit storey and stays
slow everywhere else. Measured: runs with ZERO VHALL stalls 1/8 -> **7/8**, max floor 7 -> 14.
**Two earlier fixes in the same area were mine and were wrong-but-instructive:** a "stage at the ramp
foot" pass drove a STRAIGHT LINE at the staging point, which is not wall-aware and near a ramp passes
UNDER the slab (refused by the under-slab pinch veto) — alignment is now a lateral correction blended
onto the field's route instead; and the fall veto's resultant check cleared ALL FOUR components on
failure, which froze bots solid on a balcony whose route crosses a catwalk (72% of stall samples had
`mv=0`). The veto now restores the field's step when it would otherwise zero every direction — the
two-story field cannot route off an edge, so when it and the 1-cell lookahead disagree, the lookahead
is wrong. And `wouldFall` treats **a slab ABOVE the feet as a climb, never a drop** (`effectiveFloorHeight`
returns the ground for anything past the 0.4 m step window, which made a 0.5 m riser read as a 2.5 m fall).

**The VHALL ramp pin is a FIELD-ROUTE defect, and the obvious fix for it measured 5x WORSE
(2026-07-30).** With the enriched autopsy the pin finally showed its mechanism: on a pinned cell the
ROUTED heading (`fdir`) flips between **+1.00 and -1.00 tick to tick, in exact lockstep with
`vhOnRamp`** — y stepping 2.25 <-> 2.50, under 0.5 m of travel per 3 s, `rem=vh-commit`, and (this is
the part that killed the tempting explanations) **`near2=0`, i.e. no enemy within 2 m, and
`rise=+0.25`, a perfectly walkable step**. It is not a body block, not an unwalkable riser, and not a
missing rescue. Two producers want opposite directions: `LevelGridSystem`'s two-story VHallField
routes back DOWN that ramp (the bot mounted a NON-exit ramp), while `Autoplay::rampApproachDir` — an
anti-drift assist — hard-codes the UP-ramp axis and the call site OVERWRITES the routed heading with
it. The assist's enable gate (`along <= L + 1`) toggles at the ramp top, so the bot alternates.

**The obvious fix is wrong.** Making the assist take its direction from `dot(flowDir, rampUpAxis)` —
so a routed descent actually descends — was implemented, unit-tested, and A/B'd properly (ONE binary
plus an env kill-switch, 6 classes x both arms, run concurrently so GPU load is symmetric, 25 min).
Result, fix ON vs OFF: **floors reached 21 vs 51** (less than half), **kills 635 vs 1946**, VHALL
upper-exit pinned samples **2302 vs 1270**, worst floor dwell 1472 vs 1378 s — and OFF wins **5 of the
6 classes** individually (sorcerer 2 vs 13, paladin 6 vs 18, warrior 2 vs 8). **REVERTED.** The always-up override
is evidently acting as a RATCHET that eventually walks the bot onto the balcony; remove it and the bot
faithfully follows a route that climbs a ramp and comes straight back down. So the defect is UPSTREAM
— the two-story field emitting a down-the-ramp heading from a cell the bot should be climbing — and
the fix belongs in VHallField's routing, not in relaxing the assist. Do not retry the "assist follows
the field" shape without measuring it.

**A/B METHOD, learned the hard way here:** build BOTH arms from ONE binary with an env kill-switch.
The first attempt built the control by `git stash`-ing the source, which also reverted this file's
telemetry — so the control emitted no `[STALL]` lines at all and the comparison measured the
instrumentation rather than the behaviour (it read as "0 stalls, fix infinitely worse"). Also: run
both arms CONCURRENTLY (a sequential A/B on a busy box measures the box), cap at ~12 instances (16
drops the GPU to ~36 FPS, and a fixed-timestep sim then buys less sim-time per wall-second), and
check the arms actually launched — a shell `$var` that expands to `FOO=1` is a command word, not an
assignment, so one arm silently ran with the switch unset.

**The VHALL ramp pin was a LIMIT CYCLE in `belowExit`, not a routing failure (2026-07-30).** The
enriched autopsy made it unmistakable at scale: in a 67-minute couch soak, **1044 of 1312 pinned stall
samples (80%) were VERTICAL_HALL**, and **1021 of them sat at feet y = 2.47-2.50 m** against a 3.0 m
exit storey — with the route valid on 100% of ticks, no enemy within 2 m on 100%, geometry ahead clear
on 100%, and `door=0` on ZERO samples. Nothing was blocking and nothing was missing.
`belowExit` gates the ramp-climb assist and read `pos.y < floorDoorPos.y - 0.5f` — a threshold of
**exactly 2.50 m** on a 3.0 m exit. Below it the assist pushed UP the ramp; at it the assist switched
OFF and the VHallField's own heading took over, which on a non-exit ramp points back DOWN; the bot
stepped to 2.25, the assist re-engaged, and it climbed to 2.50 again. **It could never cross its own
gate.** The escape ladder was armed (`npt > 4 s`) on 99% of those samples and could not help, because
the bot was not stuck — it was being steered in a circle. Margin 0.5 -> 0.1 m.
This is the SAME failure shape as the `vhClimbing` margin fixed the day before, on a DIFFERENT flag,
and it is worth stating as a rule: **on a stacked floor, any "am I below the exit storey" test wants a
hair of tolerance, not half a metre** — a margin comparable to a storey pitch or a slab thickness will
sooner or later land exactly on a real surface height and become a trap.
It also explains why the earlier `rampApproachDir` experiment measured 5x WORSE: that change made the
assist steer DOWN on the 2.25-2.49 half of the cycle, accelerating the very loop this margin creates.
**Verification note, honestly:** two paired A/Bs failed to REPRODUCE the pin after the fix — fresh
characters on the rebalanced build die before reaching a ramp top, and the geared save rolled
ground-exit floors — so the evidence for this fix is the exact arithmetic match between the threshold
and 1021 measured samples, plus the soak that follows it, NOT an A/B.

**The boss stall was a SEEK RADIUS one metre too small (2026-07-30).** The second routing failure the
soak exposed: 251 pinned samples, **all on floor 30**, with `bossG=1` (exit sealed by a live milestone
boss), a valid heading on 100% of ticks, and `d2d` frozen at **~28.9 m**. The boss seek was gated on
`dBoss < 25 m` — so at 29 m it never engaged, travel kept aiming at a door that CANNOT open until the
boss dies, and the bot ground down an endless supply of adds instead. One session spent **1096 s** on
that floor. The distance gate is now gone entirely: while a milestone boss lives there is nothing else
on the floor to walk toward, so distance cannot be a reason not to seek it. This only sets the TRAVEL
heading — the FIGHT branch still preempts for anything nearby, and the clear-line vs wall-aware
route-field split is untouched, so the "don't beeline through a wall" behaviour still holds.
Note the DISARM asymmetry between the two stalls, because it explains why neither rescued itself: on
the VHALL pin `npt` was ABOVE the 4 s escape threshold on 99% of samples (armed and ineffective), while
on the boss floor it NEVER reached 4 s (chip damage on the adds reset it every second). A rescue keyed
on a no-progress timer is blind to both.

**VHALL NAVIGATION WAS REWORKED AT THE ROOT (2026-07-30) — the paragraphs above this one describing
ramp-climb machinery are HISTORY, not current behaviour.** Aaron: "find a way to fix this on a
conceptual level; we are doing something fundamentally wrong." Two defects, both verified: (1) the
correct two-story `VHallField` was the LOWEST-priority travel-heading writer, under four stacked
assists (always-up ramp anti-drift, ground centreline blend, jump-pad beeline, hop-pulse machinery)
— each a compensation for the previous one's failure; direction-following is memoryless, and on a
stacked floor a local error changes your STORY, which teleports you elsewhere in the route graph and
the per-tick re-read silently redirects. (2) The broken catwalk's 2-cell gap genuinely ISOLATES the
W balcony at 3 m (the "all four balconies interconnect up top" claim was false — the gap is on the W
arm), and the exit is on W ~25% of floors, so wrong-ramp climbs legitimately route back DOWN — which
is why the earlier "assist follows the field" fix measured 5x worse. The fix: `carveVerticalHall`
RECORDS the gap as `DungeonResult::jumpLinks` (from the carve's own variables; property-tested incl.
the negative pin that W is isolated without them); `VHallField` runs a small-cost Dijkstra with the
links as cost-3 edges + a per-node u16 `dist`; and a pure NODE-COMMITTED FOLLOWER
(`autoplay_vhall.{h,cpp}`) executes it — point-servo at a latched node's centre, grounded-only
releases (arrived / dead node / story change / WALK-only leash / advanced-past-by-dist), and a
triple-gated jump across the gap (on the lip node ∧ real facing arrived ∧ `StoryNav::planVault`
viable; 2.5 s blocked-jump timeout + 1.5 s backoff; airborne ticks steer at the landing and NEVER
re-read the route — the story read over the gap says GROUND). Driver: nothing else writes the VHALL
travel heading; the travel-commit is bypassed there (the node latch replaces it); the fall veto
stands down ONLY on the takeoff tick (whose own gates are stricter); dodge/block suppressed for that
tick only. All four assists + the pad cache/goal + `vhClimbing`/`vhOnRamp` and both hop-pulse sites
are DELETED. Measured: A/B (one binary + env switch since stripped, 6 classes x both arms, 25 min,
--vhall) floors 71 vs 49, kills 2223 vs 1535, upper-exit pins 132.5 vs 223/bot-h, worst dwell 874 vs
1425 s, 4 of 6 classes; confirming 2 h couch soak: VHALL fell from **78% of all pinned stall samples
to 6%**, **8/9 sessions reached floor 50** (was 6/9), residual VHALL pins are entrance death-cycling
on the rebalanced difficulty (all WALK-mode, ground story, no enemy contact), zero pins in the jump
modes. `[STALL]` now carries `vd=` (remaining route cost — frozen vd with large net travel is the
circling detector) and `fm=` (follower mode). Plan: `~/.claude/plans/rosy-percolating-wreath.md`.

**The GAUNTLET escape-ladder livelock (found 2026-07-30, FIXED 2026-07-31).** With VHALL fixed, the
worst remaining floor was 7102 s on a gauntlet floor: BOTH couch lanes pinned in a 1-cell pocket
8-10 m from an OPEN door, route valid (`fdir` steady toward it), `rem=escape` on 908/908 samples —
the escape ladder owned the intent for two hours, emitting alternating +-z lateral nudges
perpendicular to the route while both bots fired at 4 targets they never damaged (`npt` climbed to
7074 s: the false-LOS standoff shape). The ladder never tried the route direction and never gave up.
A four-link chain, each hiding the next: the false-LOS standoff pinned `npt` high, the escape
heading was demoted to a strafe-side HINT by the unstick helper, the strafe axis happened to be
walled, and the exit BULL — the one remedy built for exactly that pocket — sat BELOW the escape
branch in the else-if chain, unreachable while the ladder churned. Two fixes in
`engine_autoplay.cpp`: a LATCHED bull now PREEMPTS the escape branch (`!(exitBull && doorActive &&
!bossGate)` on the escape condition), and `unstickCombatMove` gained a `commitWalk` mode (armed by
the escape site once `noProgressTimer > 10 s`) that WALKS the preferred heading as commanded WASD
instead of demoting it to a strafe hint. Verified: sp_soak1 gauntlet pins 906 -> 43 and the
`rem=escape` monopoly gone; soak13 (3 h, 9 classes) had `rem=escape` on ZERO of 2302 stall samples.

**BOSS FLOORS: the boss is the travel GOAL, healers die first, and the bot commits to closing
(2026-07-31 pass).** A milestone boss seals the exit, so the fight IS the floor; four coordinated
changes make the bot treat it that way. (1) **Goal SUBSTITUTION, not a seek assist** (`buildBotView`):
while the boss lives, the wall-aware boss-seeded `RouteField` IS the travel field (the exit field
takes over the tick it dies); the straight bearing survives only as the last-metres fallback when the
route field is invalid AND the line is clear. Two prior shapes both failed and are recorded in the
code: the 25 m seek radius left the at-door park (4256 s at eff65, bot ON the sealed door), and just
DROPPING the radius A/B'd WORSE (57 -> 80 s median boss-floor dwell) because an always-on straight
seek fought the exit field — the defect was WHICH FIELD ruled, not the assist's tuning. (2)
**Healer-first / boss-focus `pickTarget`** (Aaron: "clear all healing enemies first and then Focus
the Boss"): on a boss floor the nearest visible HEALER/SUMMONER (`EnemyRole` mask on `BotTarget
::isHealer`) wins outright, then an unshielded BOSS beats nearer ordinary adds; a SHIELDED boss stays
deprioritized (adds drop the shield) with a lone-shielded fallback. The brain's FIGHT gate exempts
healers on boss floors from the engagement ceiling like bosses. (3) **Boss movement FILL**: soak11
made the residual self-describing — 273 boss-gated pins, dB median 40 m, `mv=0` on 232 — FIGHT emits
WASD only when kiting/closing/strafing, so a bot fighting in place never walks the heading toward the
one enemy that opens the exit. When the intent carries no movement on a boss-gated floor, the feet
are filled toward the boss (flowDir, else the RouteField re-read directly). Verified soak12:
boss-gated pins 273 -> 0. (4) **Boss CLOSING COMMIT** (`autoplay_combat.h` `bossCommit*` +
driver window): soak13's residual — a RANGED doctrine strafes its add-band, so it always HAS WASD and
the fill never fires; the ranger orbited one NM-25 boss floor for 78 min with dB frozen at 44-45 m
and no boss LOS, because the adds never run dry. A 20 s window tracks closing on the boss; under
2 m of approach (and boss not already fightable) LATCHES a commit that overrides the feet toward the
boss route over a live FIGHT intent (combat stays the brain's — the descend-commit shape), tagged
`rem=boss-cmt`. RELEASE requires LOS inside `min(weaponRange, THREAT_RADIUS)` — raw LOS at 40 m
would flap, since a boss behind 16 nearer adds never enters the nearest-16 target list; at 12 m it
is inside both the fire band and the scan, so normal targeting holds the fight. The warrior standing
ON Korvath (dB=1, firing, simply out-DPS'd) never latches by construction — that is a BALANCE wall,
not a movement problem. `[STALL]` carries `dB=`/`bL=` (boss distance / clear line) on boss floors.

**CLASS AI: every class dumps its kit (2026-07-31, Aaron's design).** Skill selection no longer
defaults to the cheap slot-0 filler for martial builds. The rules, in order (`decideCombat`):
summons first (unchanged); a GROUP fires the biggest castable AoE (unchanged); then EVERY class
dumps BIGGEST-FIRST — a melee build IN REACH of its target and a ranged build IN BAND cast their
highest castable slot (a melee build closing to a target beyond weapon reach still uses the cheap
filler — the big hit lands when it can connect); COUNTER skills (`BotView.skillIsCounter`, today
Wanderer's DEFLECT) are withheld from the dump and cast reactively on the block triggers
(`swingIsLanding` / `incomingProjectileEta < PERFECT_BLOCK_LEAD`) — a timed counter, per Aaron.
Two old test pins encoding cheap-first were rewritten to pin the new policy. Measured: paladin
Divine Judgment 0 -> 9 casts/100 s; the paladin now plays gap-close -> dump -> block as designed.

**The ENDING no longer strands an autoplay run (2026-07-31).** CREDITS and VICTORY are non-gameplay
screens (`updateAutoplay`/`logStats` only run IN_GAME), so a victory used to convert the healthiest
sessions into silence — 3 of 9 soak10 sessions beat the game in 56-82 min and then parked on the
credits for the rest of the soak (the death-screen strand's exact shape). Both screens now
auto-advance under `m_autoplayActive` on a bounded countdown (10 s bot / 45 s if control reads
human — the credits free the cursor, so a stray motion flips the takeover latch; a gate would
re-strand), with `forceBot()` on the way back into a world and a LOG LINE at each advance
(`[AUTOPLAY] credits auto-advance` / `ending advance -> town|menu`) so a soak can tell a bot-advanced
ending from a park. The engine-slain ending rolls into the TOWN (portal -> Free-Play -> next run, the
existing machinery); the STANDARD ending now **rolls into a fresh run** (`Engine::autoplayNextRun`,
2026-08-01) instead of ending at the menu — beating the game was the single best outcome the mode can
produce and also the one that stopped it playing. Three rules make that safe rather than merely
automatic. (1) **Never overwrite the champion**: the finished hero is saved FIRST, then the new run
takes the first FREE slot — and `scanSaveSlots()` is re-run before picking, which is load-bearing, not
hygiene, because `m_saveSlots` is otherwise only refreshed by the MENU: a hero that started as a New
Game in slot 12 wrote save_12 during play, and against a stale scan that slot still reads free, so the
rule would hand the new run the champion's own file (a CLI launch, which never scans at all, would
read every slot free and land on slot 1). With the list full — or on a run that never had a slot
(slot 0 is `saveCharacter`'s own "don't save" sentinel) — the new run plays UNSAVED; losing a run is
recoverable, clobbering a character is not. Couch lanes are collision-checked against each other,
since nothing is written yet when both are assigned. (2) **The class ROTATES per lane** — an endless
loop replaying one class is a worse demo and a much worse soak than one that walks the roster (the
100x wdps spread below is exactly what a single-class loop hides). (3) **Only a BOT-advanced ending
continues**: a key press is a human saying "I'm done" (ESC especially), and answering that by starting
a new run would be the game refusing to be quit; online sessions still end at the menu, where the
teardown disconnects them (a host silently re-rolling a dungeon would strand its guests).
**`--victory` is the new dev door** (composes with `--autoplay`/`--autoplay-couch`): it builds the
world and rolls the standard ending on the spot. Same rationale as `--source` — the ending otherwise
costs a full 50-floor clear, which is why the credits park went unnoticed until a 3 h soak produced
exactly one victory. Verified live through it: credits -> victory -> next run, Warrior->Ranger solo
and Paladin+Marksman->Combat Engineer+Tinkerer couch, both bots playing on within seconds; and both
slot branches (list full -> unsaved; slot free -> claimed) with every other save byte-identical after.

**What soak13 says (2026-07-31, 3 h, all 9 classes, one SP instance each).** Zero crashes, zero
silent strands, `deaths==revives` held, `rem=escape` extinct, wedge rescue live. The headline is
CLASS POWER, not navigation: final wdps spread 367 (Tinkerer) to 38371 (Wanderer) — 100x. Rogue
BEAT THE GAME (Hell 50) in 79 min; Wanderer reached Hell 45 with 6438 kills; Marksman cleared
Normal; Ranger NM 27, Paladin NM 24 — while Sorcerer ended on Normal 49, CE Normal 41, Warrior
Normal 30 (115 min AT Korvath, 444/459 stall samples at dB=1 bL=1 fire=1 — the melee DAMAGE WALL:
AI blameless, cannot out-DPS the rebalanced boss), Tinkerer Normal 23 with 705 deaths (88 min on
one floor, wdps 367). The two SUMMON classes are the two weakest, which recasts the standing
"CE always in the stuck pair" mystery as CLASS WEAKNESS (death-cycling churn, `rem=brain/breakoff`),
not a nav loop. VHALL residual = the known entrance death-cycling only (all fm=WALK). Open, in
order: the summon-class power gap, the Korvath damage wall, standard-ending continuation.

**A stalled floor now explains itself.** `[STALL]` (`engine_autoplay.cpp`, shipped ON, no env gate)
fires once a single floor has run over 5 minutes. It exists because the last several stalls each cost a
rebuild-with-a-tracer and a half-hour re-run to diagnose. The first version reported only route /
commanded movement / grounded / height / distance-to-door / targets / fire / no-progress timer / style,
and a 3 h couch soak proved that **is not enough to diagnose anything**: it cannot tell a body that is
WEDGED from one walking a 1 m loop, cannot say whether the step the route wants is even possible, and
cannot say which producer is driving. So it now also reports **net XZ travel between dumps** (wedge vs
churn), the **geometry under and ahead of the body** (`cell` / `surf` / `ahead` / `rise`, flagged
`NEEDS-JUMP` past `STEP_UP_HEIGHT` and `WALL` if solid — "can the body make the step it is being asked
to make?"), **hostiles within 2 m and 1.2 m** (body-block vs geometry), the **routed heading vs the
heading the WASD actually encode** (`fdir` / `mdir` — a producer conflict shows as these disagreeing or
reversing), the VHALL ramp flags, and a **`rem=` tag naming the branch that owns the intent**
(`brain` / `vh-commit` / `descent-cmt` / `escape` / `bull` / `breakoff` / `wedge` / `descend`), so
"the rescue never fired" and "the rescue fired and did not work" stop looking identical. The dump timer
is **per-lane** (it was a function-local `static`, i.e. one timer shared by both couch lanes, so a couch
stall log was two half-sampled bots). `AUTOPLAY_STALL_SEC=<n>` overrides the 5-minute gate for a repro
run (and under 300 it samples every 3 s instead of 15, because a 15 s sample aliases an oscillation
away). The added fields immediately overturned TWO standing explanations: the unwalkable-riser story
above, and a fresh body-blocking hypothesis of my own — `near2=0` on 9 of 9 pinned samples killed it
before it became a fix.

**Couch soaks were reporting DOUBLE every duration (fixed 2026-07-30).** `m_autoplayRunTime` /
`m_autoplayFloorTime` / `m_autoplayHbTimer` are deliberately GLOBAL (one run, one floor, one clock),
but `updateAutoplay` runs once per LOCAL LANE — so with two lanes every one of them advanced at 2x real
time. A 3 h couch soak reported `elapsed=21525`, the "30 s" heartbeat fired every 15 s, per-floor dwell
read double, and the `[STALL]` autopsy's 5-minute gate tripped after 2.5 real minutes. They now advance
once per FRAME (`m_localPlayerIndex == 0`). Any duration read off a couch soak logged before this is
2x too large — halve it before comparing against a singleplayer run.

**The AFK revive could STRAND a run on the death screen — the real "autoplay gets stuck" (measured
2026-07-28).** An 8-run back-to-back Descent stress test had **6 of 8 runs go completely silent for
128-271 s** — no FPS line, no bot telemetry — which reads exactly like a frozen bot. It was not a
freeze and not a nav bug: `logStats()` and `updateAutoplay` BOTH only run in `IN_GAME`, so a run
parked on the GAME_OVER screen produces precisely that silence. The bot had died and never revived.
Cause: entering the death screen **frees the mouse cursor** (the options are clickable), so the very
next pointer motion trips the human-takeover latch; the AFK revive countdown was gated on a LIVE
`m_autoplayControl.botInControl()`, so it simply stopped — instrumented as
`botInControl=0 respawnTimer=0.00` held until the process was killed. The countdown now keys off
`m_autoplayBotDeath`, **latched at the arm site**, and the revive calls `forceBot()` on its way back
into `IN_GAME` (the cursor-freed latch would otherwise leave the revived bot standing at the
entrance). Measured on the same 8-run test: **dead-stall runs 6/8 -> 0/8, revives 0/19 deaths ->
60/60, floors cleared 9 -> 24, kills 287 -> 829.**

**...and the ARM had the same hole — found only by a 1-hour soak.** Latching at the arm site fixed the
latch flipping AT/AFTER death, but the arm was ITSELF gated on `botInControl()`, so a run whose latch
had already flipped BEFORE it died armed nothing, logged nothing, and stranded exactly as before. In a
1 h / 9-run soak two runs traced **HP 1087 -> 825 -> 439 -> 155 -> silence for the remaining 15 min,
with NO death line at all** — the stranding deaths were precisely the ones invisible in the logs,
which is what hid this. The arm is now **unconditional for any autoplay death**, and the human intent
is preserved by the countdown's LENGTH instead of by a gate: a bot death revives in 1.5 s, a death
while control reads as human waits `HUMAN_REVIVE_SEC` (15 s) — far longer than a real player needs to
click, still bounded, so an unattended run can never strand. The death is logged unconditionally too,
which makes **deaths == revives a meaningful invariant** to assert in soaks. Verified: a 15-min 9-run
soak gave **156/156 revives, 0 silences, 0 crashes**, and the 15 s branch (which the soak never
triggered naturally) was force-exercised to **13/13 revives at exactly 15 s**.
This bug dominated every "it gets stuck" report; the nav fixes below are what got the bot DOWN once it
stopped dying permanently.
**Soak-rig gotchas:** a log-age/mtime "stall" check is USELESS against a game whose stdout is a FILE —
libc block-buffers it, so a busy process can look frozen for 15 min (use `stdbuf -oL`, or judge gaps
from TIMESTAMPS inside a flushed log). And `logStats()`/`updateAutoplay` only run in `IN_GAME`, so a
run parked on ANY non-gameplay screen goes totally silent while still burning CPU — silence is not a
hang, and a hang is not the only thing that produces silence.

**Descent hole-commit — walk it in, and ROLL if walking isn't doing it (measured 2026-07-28).**
Instrumenting four runs against the drop holes showed two opposite failures: ranged builds spent
**79-86% of their ticks within a METRE of a hole and 82% of them airborne** and still did not go down
(hovering at the lip, where the body's own half-width catches the slab edge while the field codes the
cell `0xFE` — "at the goal" — and stops steering), while melee builds had only **2%** of their ticks
within a metre of a hole at all. So within `kHoleSteer` (6 m) of the nearest way down
(`Autoplay::nearestDropHole`, pure) the driver aims the FEET at the hole centre — which also drags the
melee builds the last few metres in — and inside `kHoleRoll` (2.5 m) spends a **dodge ROLL** at it, a
committed ~4 m lunge that carries the body clear of the lip. The roll direction comes from the WASD
held THAT tick (`computeRollDirection`), so the movement keys are set on the same tick, exactly as the
gap-closer roll does; combat is untouched, grounded-only, and it is rate-limited by the engine's own
~1 s dodge cooldown rather than the doctrine leash (this is navigation, not defence).

**Descent FLOOR PARK — get off the return lift (measured 2026-07-28).** Runs still parked on a single
Descent floor for 6-13 minutes: alive, healthy, killing, never descending. Tracing every bot that had
been on one floor for 3+ minutes (260 samples) says what it is: **all 260 on FOUR_STORY**, target list
pinned at its 16 cap in **260/260**, the descend commit latched in **260/260**, **47% of ticks
AIRBORNE** and **32% with no heading** — and the worst trace shows distance-to-exit oscillating
between two fixed values (15.2 m / 37.6 m) with `grounded` flipping for **740 s**. That is the
**RETURN-LIFT STORY BOUNCE**: drop through a hole, land on the pad beneath it, get flung back up,
repeat. Pads are excluded from the descent flood on purpose, so a bot on one can be left with no
route, and a bot with no route just waits there to be relaunched. `Autoplay::padEscapeDirection`
(ring-searches for the nearest routable NON-pad cell) is the fallback when a padded bot has no
heading — the one case the field cannot express, "leave where you are". Measured A/B (9 runs × 25 min,
same class matrix): **floors 132 -> 152, max floor 19 -> 25, park samples 260 -> 142 (-45%), worst
single floor 760 s -> 382 s (-50%)**. **Reduced, NOT eliminated** — 142 park samples remain, so a
second mechanism is still in play. Note for whoever picks that up: `descentDirection` does NOT always
return zero on a pad cell (the tier-2 recovery flood routes THROUGH pads, so a pad cell can carry a
code) — a unit test caught that assumption. (The residual was then traced and fixed — see the WEDGE
paragraph below. The guess recorded here, "the hazard veto zeroing `flowDir`", turned out to be HALF
right and is a good illustration of why the tracer went in before the fix: it is one of the two
mechanisms, and the OTHER one is its exact opposite.)

**The residual Descent park is a GEOMETRY WEDGE, and every rescue was disarmed by COMBAT (measured
2026-07-29).** Tracing the parks directly (a per-tick classifier over 9 runs, then a paired 4-class
A/B) killed all three standing hypotheses at once: on a parked bot the descent heading was **valid on
100% of ticks**, the bot was **not on a pad**, and `launches` was **frozen** — so it is neither a
routing failure nor the return-lift bounce. What the traces show instead is **two symptoms of one
failure**, and they are exact opposites, which is why no single detector had ever caught both:
- **WEDGED** (warrior/ranger): a valid heading, the veto never firing, the descend commit latched,
  movement commanded on 269-789 of 300-900 ticks — and **0.0-0.1 m of travel per 5 s window**, with
  Y pinned to the centimetre. (It also read **50% of ticks AIRBORNE**, which looked like a body
  balanced on a slab LIP — that turned out to be an ARTEFACT of the grounded bug below, present on
  ANY resting body. The wedge and its A/B stand; the lip reading does not.)
- **BOXED IN** (tinkerer): the router hands back a real heading (`raw` non-zero every tick) and the
  hazard veto refuses it **and all four fan detours**, so `flowDir` is zeroed and the brain commands
  **no movement at all** — `distToDoor` frozen at **9.6 m** for a whole 25-minute run. A dead-end node
  whose only way out crosses a jump pad does exactly this.

**Why nothing rescued either:** the escape ladder needs `m_autoplayNoProgressTimer > 4 s`, and the
`combatProgress` branch **zeroes that timer on any chip damage**. A Descent floor holds four stories
of enemies and keeps the target list pinned at its 16 cap, so the bot chips something every second and
the timer never climbs (measured 0.0-3.2 s, never reaching 4). The 20 s floor-stall watchdog does not
help either: it arms `m_autoplayDescentCommit`, which is already latched and is itself the thing being
ignored. **A wedge is POSITIONAL, so it must be detected positionally** — the fix is deliberately
independent of every combat signal. `Autoplay::wedgeDetected` (commanding movement + <0.6 m of **3D**
travel over a 2 s window — 3D so a fall or a pad ride counts as progress and is never called a wedge)
and `Autoplay::boxedDetected` (a real heading vetoed to nothing for ≥90% of the window) are pure and
unit-tested, including that **each is blind to the other's case**, which is the whole reason both
exist. Either one fires an escape burst that overrides EVERY movement producer above it (commit,
movement fill, hole commit, FIGHT): a **JUMP** (the half that actually clears a lip — a body refused on
one axis has to leave the ground, and on a Descent floor falling is the objective anyway) plus an
**escalating sideways heading** (`wedgeEscapeAngle`: +90°, -90°, 180°, ±45° — sidestep FIRST, since
reversing immediately walks the bot back down the route it just travelled). "Vetoed to nothing" is a
much sharper trigger than "isn't moving": a bot holding position in a fight has no heading to veto,
and a bot at the exit is already excluded by the descend hold, so neither can be mistaken for a park.
Measured, paired A/B (4 classes × {on, off}, 25 min each, same binary): **park windows 185 -> 51
(-72%)**, **max floor 59 -> 71 total with every class improving** (marksman 12->17, tinkerer 14->19,
warrior 20->21, sorcerer 13->14), **worst single floor 607 s -> 173 s (-71%)**, and **deaths 170 -> 146
despite reaching deeper floors**; `deaths == revives` held 8/8. Round 1 (wedge only) fixed
warrior/sorcerer and left the tinkerer at 232 vs 230 park windows — that null result is what exposed
the second symptom, so **do not read a partial A/B win as done**. FOUR_STORY-scoped (where it was
measured; the mechanism is style-agnostic but VERTICAL_HALL has its own fall veto a blind sidestep
would fight). Driver + pure nav only — **no wire/save change**. The per-floor escape count now rides
the `[TELEM-HB]` heartbeat as `wedges=`: a long `secs_fl` with a climbing count is a bot fighting
GEOMETRY, with zero it is fighting ENEMIES — the one distinction that cost several instrumented runs
to establish, so it is worth a field in the log rather than a rebuild with a tracer.

**Descent stall pass — never stand still with a route (measured 2026-07-28).** Descent floors still
stalled intermittently. Instrumenting six runs found it is NOT a routing failure: the bot held **NO
movement key on 57% of ticks, and in 267 of 267 of those a VALID descent heading existed** — it always
knew the way down and simply did not walk. A Descent floor carries four storeys of enemies, so the
target scan is pinned at its cap (16 visible, permanently) and the FIGHT branch owns the intent; that
branch emits movement only when kiting/closing/strafing, so an in-band target it is neither closing on
nor kiting from yields zero WASD. **Both rescues are disarmed by the same conditions**: the escape
ladder needs `m_autoplayNoProgressTimer > 4 s` but the combat-progress branch pins it at 0 while the
bot chips the swarm (measured max 0.5-0.6 s), and the 20 s floor-stall window that arms
`m_autoplayDescentCommit` resets whenever the bot drifts 2 m doorward mid-fight. Two driver fixes,
FOUR_STORY-scoped: (1) **movement fill** — whenever the intent carries no movement at all and a
descent heading exists, walk it (the commit's own faceAndGo decomposition; combat untouched — only the
FEET are filled in). Runs BEFORE the descend pulse, since that pulse drops `in.descend` on its release
beat and reading the flag after it would walk the bot off the door it is opening; skipped at
`atDescentGoal` (standing on the hole — it is about to fall). (2) **a latched commit must not SHADOW
the escape ladder** — the commit sits above the ladder in the else-if chain and is held for the rest of
the floor, so once latched it swallowed even the ticks where it had no heading AND was not at the door,
i.e. exactly when the bot was wedged (measured: a **22 s dead stop** with `flow=0`, `cmt=1`, the
no-progress timer climbing past 9 s and the ladder never firing). It is now entered only when it can
act. Measured, controlled A/B (same binary, env-gated, 3 fresh-warrior runs/arm, ~470 s of
descent-floor time each): **floors descended 0 -> 6**, **frozen >=6 s 7.5% -> 0%**, not-moving
33% -> 10%, time on the bottom storey 9.7% -> 30.4%. Driver-only; no wire/save change.

**Descent finish pass — pad recovery + fight-your-way-DOWN (measured 2026-07-26).** A Descent floor the
geared paladin never finished (bounced above the exit, then stood in the swarm) now completes ~3/6 in
140 s, via four driver/field fixes (FOUR_STORY-scoped; no wire/save change). (1) **paddedOnly-on-L0 bug**
(`autoplay_descent.cpp`): pass 1 of the seed loop sets `paddedOnly=true` BEFORE it knows it will seed
anything, and on L0 (no slab below → no holes) it seeds nothing and falls through to exit-seeding with
`paddedOnly` stuck true — which stood the driver's pad veto DOWN on L0, so the bot walked straight onto the
return-lift pads under the L1 holes and got flung back up ("jumped above the exit and too dumb to drop
again"). The exit-seed block now clears the flag. (2) **`descentNextIsPad`** (`autoplay_descent.h`): when
the field's NEXT routed step IS a pad — a return lift severing a pocket, or a pad blocking the exit corridor
(the "jump pad not perfectly placed" case) — the two pad-avoidance vetoes stand down for that one crossing,
so the bot takes the bounce and re-routes instead of freezing next to the pad it must cross. (3) **FOUR_STORY
DESCEND COMMIT** (`m_autoplayDescentCommit`, the Descent twin of the VHALL climb commit): on a dense floor
the bot stood in a 16-target swarm with every WASD zero, fighting instead of descending (the real "too dumb
to drop"). The floor-stall watchdog now latches a persistent commit instead of the too-short 3 s break-off —
KEEP the brain's combat (aim/fire/dodge/block/skill/potion), only OVERRIDE the WASD feet toward the descent
field heading (faceAndGo decomposition), so it FIGHTS ITS WAY DOWN to the next hole; held until it leaves the
floor. (4) **Descend-at-door**: that commit branch SHADOWS the normal `atDoor` descend, so it must fire the
interact itself — without it the bot reached the L0 door (0.7 m, field flow=0) and stood there fighting the
swarm forever, never pressing descend. All measured on the geared paladin save (combat isolated); the
non-finishers all reach L0 and are only slowed by combat density, not stalled.

**TRAVEL movement is a WASD decomposition, not "hold W"** (`autoplay_brain.cpp` `faceAndGo`). The aim is
deliberately EASED, so the facing lags the heading for a few tenths of a second after every turn; holding
W through that lag walks the bot wherever it happened to be pointing, which in a 3-wide corridor is the
wall. The heading is projected onto the CURRENT forward/right basis (matching `player.cpp` exactly) so the
bot steps sideways out of the corner immediately and straightens as the turn completes. This affects
**every** layout, and flat floors measured slightly better after it, not worse.

**What a 3 h / 9-session COUCH soak says (2026-07-30, 18 bot-played characters, ~27 GPU-hours).**
The run is healthy in the ways that are easy to get wrong and broken in one specific way. **RSS is
flat** — 118-125 MB -> 139-143 MB per process over 3 h, plateauing, no leak (the `AllocationTracker`
"1.3 GB still live at shutdown" is exactly the churn artefact documented above; do not chase it).
**Six of nine sessions reached Hell floor 50** (eff=150), 46,719 kills, `deaths == revives` held. But
**36% of the entire soak was five bots stuck on ONE floor each**: 4568 / 8242 / 5095 / 7310 / 9268 s of
single-floor dwell (durations halved for the couch double-count fixed above). Of 5003 `[STALL]`
samples, **VERTICAL_HALL is 78%** — cavern 13%, descent 6%, and rooms/hub/gauntlet ~2.5% combined, so
the flat styles are essentially solved and the stacked ones are not. The three worst floors were all
VHALL upper-exit, and the cleanest of them had **`tgts=0`** — no enemies at all, the bot commanding
movement for 2.6 hours against pure geometry with the no-progress timer at **6828 s** while the escape
ladder's threshold is 4 s. A rescue being "armed" is not the same as it working.

**Where v1 actually stands (measured, 2026-07-24).** On **flat** floors (`rooms` / `cavern` / `gauntlet` /
`hub`) the bot plays unattended for all three archetypes — in ~2-minute `--autoplay --new` runs a Warrior
and a Sorcerer each reached floor 6 and a Marksman floor 4, casting class skills, auto-equipping and
descending, with zero deaths and no permanent stall. On **FOUR_STORY** the routing above turned a floor the
bot never finished into one it descends: across three fixed seeds it now reaches L0 and spends 33-42% of its
time there (was 0-3%), closing to 2-4 m of the exit, and one seed cleared two floors in 130 s — but it does
**not yet reliably finish a Descent floor inside ~2 minutes**, and the remaining cost is COMBAT, not
routing (the bot fires on 45-50% of ticks on a floor holding four stories of enemies). On
**VERTICAL_HALL** the bot now CLIMBS (see the ramp note below) — it reliably mounts the ramp, crests onto
the exit balcony, and closes toward the door — where before it could not get up an exit-upstairs floor at
all; but like FOUR_STORY it does not reliably FINISH one, and for the same reason (the balcony snipers pull
it into a fight before it reaches the door). Lava is unchanged and still slow. Treat "descends every floor
type promptly" as unproven — the shared unsolved problem is combat density on a floor the bot must TRAVERSE,
not the routing.

**VERTICAL_HALL stair climbing — a committed ramp crossing with a jump assist (measured 2026-07-24).** An
exit-upstairs VH floor was unfinishable: the bot could not get up the ramp. Four distinct bugs, each found
by tracing:
- **The crossing must be COMMITTED to one ramp, not re-picked per tick** (`m_autoplayVhPortal`), and "am I
  done" is a FEET-HEIGHT test (`StoryNav::feetOnStory`), NOT `onUpperStory` — a ramp is a graduated slab, so
  the per-cell slab test reports "upper" from the first riser, and the old code stopped routing the instant
  the climb began, fell back to the flat ground field, and got pulled back off. This also killed the
  documented per-tick goal oscillation.
- **`climbing` is fixed by the exit story (`exitUpper`), NOT `exitY > feetY`.** The height test flips false
  the exact tick the bot crests at 3 m, which made the far end read as the FOOT (so the arrival latch never
  fired) and made `portalRouteGoal` aim back DOWN the ramp. In VH spawn and exit are always opposite
  stories, so "exit is upper" == "this is a climb" for the whole crossing.
- **The release tolerance is TIGHT (0.5 m) and gated on reaching the ramp TOP cell** (`m_autoplayVhCrossed`
  latches only when on the exit story AND within 2.5 m of `highPos` XZ). The 2D flat flow field cannot
  represent two stories (a balcony cell and the ground beneath it are ONE node), so releasing while still on
  the ramp — or on the balcony trusting that field — walks the bot off the open inner edge back down.
  Once crossed, on the upper story the bot BEELINES to the door (a mid-balcony is a convex slab, so the
  straight line from ramp top to door centre stays on it) rather than using the flat field.
- **A climb-assist JUMP** (`m_autoplayVhClimbing`, pulsed ~1.2 s while climbing and below 1.5 m, gated on
  grounded + not fighting): the ramps are narrow 2-wide graduated slabs and the eased-aim walk drifts the
  bot off the strip and slides it back before it crests — on some seeds it never passed ~1 m of a 3 m climb.
  A hop carries it up over the risers. The flag is defaulted false every tick (only the VH climb branch
  re-arms it) so a following flat floor can't inherit it and jump spuriously.

**Autoplay stays on its story and draws a sidearm — VHALL upper-exit only (2026-07-25).** Two coordinated
rules keep the bot from wandering off to shoot ranged enemies, and above all from falling, during a
VERTICAL_HALL climb. Both are scoped to **VHALL floors whose exit is UPPER** (`floorDoorPos.y > 1.5f`):
FOUR_STORY descends BY falling through drop holes and is untouched, and a ground-exit VHALL wants the bot to
drop OFF its balcony to descend, so the fall protection there would only get in the way. (1) **FALL VETO** —
EVERY movement producer is checked against `Autoplay::wouldFall` (`autoplay_nav.h`: the destination cell's
`effectiveFloorHeight` sits more than a step below the feet) per WASD component. On this floor type no
movement ever WANTS a fall — the two-story field only emits height-continuous steps — so the veto covers
FIGHT's kite/close/strafe, TRAVEL, and the escape ladder alike. It was originally gated on a FIGHT-only
`BotIntent::engaging` flag, which the 2026-07-25 adversarial review found left two holes (the escape ladder
could walk a wedged bot off the balcony it had just climbed, and a stale committed travel heading crossed
the rim unchecked); the gate was dropped and the now-consumerless flag removed. The **travel-heading
commit** carries its own matching release: on a VHALL upper-exit floor a committed heading that `wouldFall`
is dropped immediately — the field can't point off an edge, but the commit replays a heading up to 0.4 s
old, and a mid-ramp heading held through the crest ran straight across the 2-cell rim (the residual "climbs
then drops"). (2) **MELEE RANGED SIDEARM** — a melee build on that climb that
meets a hostile it can only reach by falling (out of melee reach AND the step toward it would fall) equips
the best ranged weapon already in its backpack (`BuildScore::bestRangedBackpackIdx` — a melee build keeps
ranged weapons because `worthPickingUp` reasons over all nine build cells) via `Inventory::equip`, fires from
where it stands, and switches back to melee when the trigger clears (min 3 s dwell, 5 s between switches).
The trigger is judged with the **stashed MELEE weapon's reach + the persisted build cell** even while the
sidearm is worn (`m_autoplaySidearmMeleeRange`) — judging it with the live view's numbers (a pistol's reach)
cleared it the instant the sidearm was drawn, collapsing "keep it while the trigger holds" into a blind
3 s-on/5 s-off duty cycle. `exitAutoplayRun` restores the melee weapon and clears the flag (and the
auto-equip suppression is additionally gated on `m_autoplayActive`), so a run that ends mid-swap can no
longer leave a NORMAL game's lane-0 re-gearing suppressed.
While the sidearm is worn: `getEffectiveWeapon` already makes the weapon view ranged, `buildBotView`
overrides the doctrine cell to the Ranged column (`Autoplay::rangedCellFor`) so the brain holds ground
instead of walking a gun into melee, and `autoEquipBackpack` is hard-suppressed (`m_autoplaySidearmActive`)
so a pickup can't re-gear the melee weapon back. The switch is **opportunistic** — if the bag holds no ranged
weapon the sidearm simply never triggers and the fall veto alone keeps the bot safe (it holds and shoots
melee at air until the combat break-off watchdog relocates it). All transient — SP lane 0, no save/PROTOCOL
change. Verified live: on a VHALL upper-exit warrior the trigger fires on cross-gap balcony enemies, and with
a ranged weapon in the bag the full draw→fire-ranged→stow cycle runs (weapon view flips to ranged on 10/10
active samples).

**VHALL climb pass — climb reliably, fight your way out, don't freeze (measured 2026-07-26).** Four
driver fixes, all gated to VHALL **upper-exit** floors (`floorDoorPos.y > 1.5`) so nothing else regresses;
driver-only, no wire/save change. (1) **Airborne fall-veto carve-out** — the per-component fall veto is now
gated on `m_localPlayer.onGround`: `wouldFall` reads `feetY` at the jump apex, so while AIRBORNE every
neighbour resolves far below the feet and all four directions veto at once, freezing the bot mid-air — the
commit's jump pulse became a POGO in place (measured stuck at d2d≈40 m, `on=0`, `fwd=0`). The veto only ever
needed to stop a GROUNDED step off a ledge; in the air the "drop" is illusory and freezing just stops the bot
steering to a landing (an airborne drift off an edge is fine — dying is, freezing isn't). (2) **Ramp-proximity
hop gate** (`m_autoplayVhOnRamp`) — the climb hop was gated only on `pos.y < 1.5` (true across the whole flat
void), so the bot bunny-hopped the entire approach ("bunnyhopping while approaching the pad doesn't work");
now it fires only on a ramp slab, so the bot WALKS grounded to the ramp / onto a void pad and only pogos up
the risers. (3) **Fight-while-committing** — the `m_autoplayVhCommit` body no longer clobbers the intent to a
bare walk+fire (the bot "just [ran] for the exit without a care" and died); it KEEPS the brain's aim / fire /
dodge / block / class-skill / potion and only OVERRIDES the WASD feet toward the exit (faceAndGo
decomposition), so it fights its way out — measured **deaths 0**, combat active on **52-70%** of commit ticks.
(4) **On-slab centreline anti-drift** (`Autoplay::rampApproachDir` / `rampSegDistXZ`, `autoplay_nav.h`,
unit-tested) — the story-aware VHallField mounts the ramp but the eased-aim walk drifts off the narrow 2-wide
slab and slides back ("94% airborne, never crests"); `rampApproachDir` centres the bot on the nearest ramp
ONLY once it is confirmed on a slab (`pos.y > 0.5`), which makes it climb onto a balcony on **every** run
(`max_pos.y` ~3.0, was 0). Using a centreline/RouteField to ROUTE to the ramp instead of just anti-drift
regressed every time (trapped under the slab, or stalled the mount). **Measured (geared paladin, combat
removed as a factor):** climbs every run, fights back, never freezes/dies, **descends ~25-33%**. **Still
open:** the upper-story CROSSING — the VHallField often mounts a NON-exit ramp, and reaching the exit balcony
then needs a catwalk cross (one is BROKEN with a 2-cell gap the fall veto won't jump), so the bot falls to the
ground under the door and re-climbs; it finishes when it happens to climb the exit-serving ramp (top on the
exit balcony, no catwalk). Fix needs a design step — vault the broken catwalk (reuse enemy `StoryNav::planVault`)
or bias the mount onto the exit ramp — tracked in `docs/superpowers/specs/2026-07-26-stacked-floor-routing-concept.md` §9.

**Where the residual shake and wall-scraping come from (measured by source, 2026-07-24).** Instrumenting
every producer of the desired aim and tagging each tick by which one wrote it settles a question that had
been guessed at twice: **the FIGHT branch (`decideCombat`) is both**. It owns 45-50% of all ticks and turns
in **7.0 direction REVERSALS per second on stacked floors** (1.6-2.1/s on flat) at only ~2.6 deg a tick —
which is exactly the reported symptom, "a fast moving but low movement jitter"; magnitude is not the
problem, sign changes are. It is also the top wall-scraper: on **17-18% of its ticks the bot holds a
movement key and moves less than 2 cm**, because FIGHT's kite/close/strafe movement is deliberately NOT
hazard-vetoed (see the veto-scope paragraph). By contrast the raw flow fields produce enormous SINGLE
steps — `flowDirection` 151 deg and the descent field 111 deg per tick, both being 4-connected cardinal
fields — but reach the aim on only 1.5-2.5% of ticks, because the travel-heading commit absorbs them; the
commit itself runs at 2.05 deg and 2.5 reversals/s. So: aim jitter is a COMBAT-aim problem, and it was
then narrowed to a single term (below), not a navigation one.

**The ranged shake is the PROJECTILE-LEAD term, and it is fixed by smoothing the target's VELOCITY, not
the aim (measured 2026-07-24).** Splitting a projectile-weapon shot's desired yaw into "with lead" vs
"aim at centre" showed the lead is the amplifier: `decideCombat` aims a projectile weapon at
`t.pos + t.vel*timeToHit`, an enemy FSM rewrites `velocity` every frame, and at ranged distances
`timeToHit` is 0.5-1.5 s, so it multiplies that per-frame noise into a lead point metres wide — the desired
yaw moved **23.8 deg/tick WITH lead against 2.4 without** in the worst window (a 10x amplification), at
11-22 reversals/s, which is exactly "ultra high frequency low amplitude". It is ranged-ONLY because melee
and hitscan aim straight at `t.pos`. The fix is a per-target exponential average of the velocity
(`m_autoplayVelId[]`/`m_autoplayVelEma[]` in `buildBotView`, tau 0.15 s, matched by entity id, rebuilt
each tick so a target that leaves drops its history) BEFORE the brain leads with it. This is explicitly NOT
a low-pass on the desired aim — that was tried and rejected earlier because a second lag stage in series
with the ease pushes the steady-state tracking error past `FIRE_ALIGN_RAD` and mutes fire on crossing
targets. The target's BEARING stays instantaneous; only the velocity ESTIMATE is filtered, and a short
average is a strictly BETTER estimate of sustained motion than one frame's sample, so the lead gets more
accurate, not laggier. Measured after: the with-lead yaw tracks the no-lead yaw within a fraction of a
degree everywhere (worst 23.8 -> ~2), the applied camera yaw sits at 0.6-2.4 reversals/s, and kills/floors
are unchanged. The residual reversals still visible in the RAW desired signal (up to 22/s on some sorcerer
windows) are absorbed by the pre-existing `AIM_DEADZONE_RAD` and never reach the camera — which is what the
deadzone is for.

**Persistence is per-character.** Each save file (`save_NN.dat`) holds exactly ONE character (`playerCount=1`); the per-lane destination is `m_playerSaveSlot[lane]` and `saveAllCharacters()` writes each active lane to its own slot. In couch co-op both players pick their own slot (New or Continue) in the menu lobby and the shared dungeon runs on **Player 1's floor**; mixed New/Continue lanes work because the menu prepares each lane and calls `startGame(mode, lanesPrepared=true)` (skipping the NEW_GAME wipe). Legacy `playerCount=2` bundle saves still load (and migrate to per-character on the next save). The on-disk layout is **versioned** (`SAVE_VERSION`, currently 3 = GLOVES slot + attack-speed cache in `PlayerInventory`): readers accept the previous version via a `Legacy*V<n>` mirror struct in `engine_persist.cpp`, and `static_assert`s pin the serialized struct sizes — any layout change MUST bump the version and extend the legacy readers. (Save format, menu flow, no-downgrade guard: `engine-reference`.)

**User-data location (desktop) + Steam Cloud.** Saves, `difficulty_unlock.dat`, `menagerie.dat` (pet-collection progress), and the `controls.json`/`audio.json`/`video.cfg` prefs are written into the per-user dir `SDL_GetPrefPath("EdRethardo","DungeonEngine")` via `Platform::userDataPath()` (`src/platform/user_paths.{h,cpp}`), NOT the install/working dir — so Steam **Auto-Cloud** can sync them (config: `docs/steam_cloud.md`). On `__SWITCH__` `userDataDir()` is `""` (CWD = app storage), so Switch is unchanged. A one-time `migrateLegacyUserData()` (in `Engine::init`) copies pre-relocation files from the CWD/exe dir into the pref dir **only when the destination is absent** (never destructive). `saveCharacter` writes **atomically** (temp + `Platform::atomicReplace`) so an interrupted save can't corrupt an existing slot. `ORG`/`APP` are frozen — changing them orphans saves.

**`controls.json` is a versioned format too.** Bindings are serialized **by `GameAction` enum ordinal**, so enum members are renamed in place or appended before `COUNT` — **never inserted or removed**, or every existing player's bindings re-map onto the wrong actions. Changing an existing action's *default* binding additionally requires bumping `BINDINGS_REV` (the `CFG_BINDINGS_REV` sentinel row) and repairing that action in `loadBindings`, because a saved file's stale row otherwise silently beats the new default. Details + the rebind-UI range caveat: `engine-reference`.

**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 60 Hz (every tick). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 33 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)

**Netplay stack (rewrite COMPLETE — M0–M14 + the 2026-07 hardening pass).** Server-authoritative + client prediction with **rollback-replay reconciliation**: on a mispredict the client rewinds to the server's acked state and re-applies every stored input through the same per-input step the server drain runs (`updateNetPlayerFromInput` movementOnly + `moveAndSlide`), committing to BOTH player mirrors (`m_localPlayer` alias AND `m_players[slot]` — an alias-only write is erased next tick by `syncNetPlayerToLocalPlayer`). Under input starvation the server **coasts** a remote on its last input for ≤250 ms and CLAIMS the ticks (advances `lastProcessedInputTick`) so snapshots stay time-consistent; a separate `m_lastActivationTick` watermark fires late-arriving activation edges exactly once. Delta compression is **ack-driven with a named baseline**: every delta names the snapshot tick it's encoded against (client acks via `NetInput.ackedSnapshotTick`, server deltas against its 64-deep global history ring, client decodes from its 64-deep ring) — never assume "baseline = last sent". `PROTOCOL_VERSION` 25 (v19: the dead lock-on input bit became `INPUT_BLOCK` — the server simulates blocking for remotes: damage negation, perfect-block window, 0.4× move slow; `SnapPlayer.flags` bits 5–7 carry Static Charge stacks; v20: Arena PvP — sentinel floor 97 + the ARENA_KILL/ARENA_SCORES/ARENA_OVER events; v21: player-facing CC — `SnapPlayer.flags` bit2 = stunned + `stunTimerQ` on the reused `reserved0` byte; v22: `INPUT_WINDOW_SIZE` 8→15 so input redundancy spans the full 250 ms coast; **v23: arena/PvP authority fixes** — remote PvP projectile/chakram/AoE hits now PERSIST (they were erased by the shared-remote-view write-back race: `applyRemotePlayerViews` wrote a pre-pass-seeded view back over the health `pvpApplyHit` committed mid-pass; fixed by composing the PvP hit onto the same shared view via `m_sharedRemoteView[]`) — and, chakram-specific, a **bounce frame no longer drops its hit**: the ricochet is DEFERRED to the loop tail so the disc's pre-bounce segment is still swept for a player/entity (`pendingBounce` in `ProjectileSystem::update`; the old code `continue`d at the bounce and skipped that frame's collision). A Continue host now seats its OWN arena NetPlayer slot; behavior-only, no wire struct change, but old peers still carry the bugs so the version gate rejects them; **v24: `MAX_ENTITIES` 128→192** for the four-story Descent floor — `WorldSnapshot` carries `SnapEntity[MAX_ENTITIES]` and the per-slot unchanged bitmask is now `ENTITY_MASK_BYTES` (24 B, **derived** from `MAX_ENTITIES` so the two can't drift — a mask narrower than the pool silently resends its high slots forever, which is exactly the bug the old 64-bit-over-128 mask had), so both the full and delta layouts changed; idle cost is the 8 extra mask bytes, ~0.47 KB/s; **v25: `INPUT_EX_THROW`** (extFlags bit 6) — the melee weapon-throw edge, reusing the free bit + the existing projectile snapshot, so no struct grew but a v24 peer can't send/handle it). Verification rig: `--net-loss <0-90> --net-latency <ms> --net-jitter <ms> --bot-walk` + the F9 net-graph (`[NET-GRAPH]` 1 Hz log: rtt/div/idelay/KB/s/snap-Hz/baseline-age). Measured envelope: 15% loss + 100 ms RTT → 0 hard snaps, deltas engaged (~9 KB/s vs 39 full). 2026-07 audit hardening: the **64-tick** snapshot history (both sides) keeps deltas alive past 300+ ms RTT (was a measured 12–25% full-snapshot fallback at 32), fire lag-comp rewinds honestly to **24 ticks**, **PvP victims are now lag-compensated like entities via a per-slot player-pose ring** (arena hit-reg), and outbound is `Net::flush()`ed every frame (~33 ms of hidden RTT recovered). For long-haul links (e.g. Germany↔New Zealand, ~150 ms one-way + jitter) the adaptive interp buffer and the server lag-comp rewind share `LagComp::MAX_INTERP_DELAY_MS` (250 ms) as the wire/rewind TRUST ceiling — but the jitter estimator's own 3× outage clamp caps its realistic *steady* delay near ~116 ms, so idelay rides spikes there, NOT up toward 250; `--net-jitter` reproduces the condition locally (constant `--net-latency` alone never engages the adaptive buffer). (Wire/tick details: `engine-reference`.)

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

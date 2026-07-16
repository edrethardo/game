# Legendary Armor & Shield Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 4 new legendary items — Capacitor Mail, Hemophage Shroud (armor 16–28 / 32–50), Thunderwall, Mirror Aegis (offhand 16–28 / 32–50) — with distinct effects, plus the co-op block fix (INPUT_BLOCK on the wire) that makes the shield effects real for guests. Spec: `docs/superpowers/specs/2026-07-16-legendary-armor-shields-design.md`.

**Architecture:** New passive `SkillId`s dispatched on the existing armor-aura and perfect-block hooks; pure helpers (`StaticCharge::accumulate`, `Combat::classifyBlock`, `ProjectileSystem::reflectAsParry`) carry the testable logic; `NetPlayer` mirrors + `serverNetPost` remote branches follow the Blood-Nova/Frenzy precedent; block state rides the dead `INPUT_LOCK` bit; charge stacks ride 3 spare `SnapPlayer.flags` bits. `PROTOCOL_VERSION` → 19.

**Tech Stack:** C++17, doctest, JSON content (`items.json`/`materials.json`), `tools/gen_status_icons.py`.

**Standing constraints:** items.json is APPEND-ONLY (defId = array index, persisted in saves). SkillId is serialized by ordinal — append before `COUNT`, never insert. No save-format change (verified byte-identical 2026-07-16; SAVE_VERSION stays 3). No anti-mash mechanics (user decision). Commit steps below assume the session has commit authorization — if not granted, skip every commit step and leave the work uncommitted.

---

## File map

| File | Change |
|---|---|
| `src/game/item.h` | +3 `SkillId`s before `COUNT` |
| `src/game/item_loader.cpp:77` | +3 rows in `skillIdFromString` |
| `assets/config/items.json` | +4 defs (196–199) |
| `assets/materials.json` | +4 tint materials (161–164) |
| `src/game/static_charge.h` | NEW — pure stack accumulator |
| `src/game/combat.h` / `combat.cpp` | `BlockOutcome` + `classifyBlock`; `applyDamageToPlayer` returns it; callback gains `attackerIdx` |
| `src/game/projectile.h` / `projectile.cpp` | `reflectAsParry`; parry branch in `tryHitPlayer`; `REFLECTED` result |
| `src/game/player.h` | `offhandSkill`, `chargeStacks`, `chargeTimer`, `hemoTickTimer` |
| `src/net/net_player.h` | same 4 fields + `lastDamageAttackerIdx`; `INPUT_LOCK`→`INPUT_BLOCK` |
| `src/game/player.cpp` | capture INPUT_BLOCK; block state + 0.4× slow in `updateNetPlayerFromInput` |
| `src/engine/engine.h` | declare `staticDischarge`, `hemophageAuraTick` |
| `src/engine/engine_update_skills.cpp` | offhand cache; host STATIC_CHARGE/HEMOPHAGE; both helpers |
| `src/engine/engine_net.cpp` | `np.offhandSkill`; remote STATIC_CHARGE/HEMOPHAGE; client charge adoption |
| `src/engine/engine.cpp` | seed/writeBack mirrors for the new fields (+`bloodNovaCooldown`) |
| `src/engine/engine_init_callbacks.cpp` | perfect-block callback rework (offhandSkill dispatch) |
| `src/net/snapshot.cpp` | charge stacks into `SnapPlayer.flags` bits 5–7 |
| `src/net/net.h` | `PROTOCOL_VERSION` 19 |
| `src/renderer/hud_inventory.cpp` | names + descriptions + CHAIN_LIGHTNING offhand override |
| `src/engine/engine_hud.cpp` + `src/renderer/hud_status.cpp` + `tools/gen_status_icons.py` | CHG status row 11 + glyph |
| `tests/game/test_static_charge.cpp`, `tests/game/test_block_outcome.cpp`, `tests/game/test_projectile_parry.cpp`, `tests/net/test_input_wire.cpp`, `tests/CMakeLists.txt` | new/extended tests |

---

### Task 1: SkillIds + loader + tooltips

**Files:** Modify `src/game/item.h` (SkillId tail, after `FRENZY`), `src/game/item_loader.cpp:77-90`, `src/renderer/hud_inventory.cpp` (`skillDisplayName` ~line 105, `skillDescription` ~line 137).

- [ ] **Step 1: Append the SkillIds** — in `item.h`, between the `FRENZY` line and `COUNT`:

```cpp
    // Armor/shield passives (2026-07-16 legendary batch) — dispatched on the armor-aura
    // tick and the perfect-block callback; none is castable, so none has a skills.json def.
    STATIC_CHARGE,      // Capacitor Mail: hits taken build 5 stacks -> chain-lightning discharge
    HEMOPHAGE,          // Hemophage Shroud: 4m life-drain aura, heals the wearer
    PROJECTILE_PARRY,   // Mirror Aegis: perfect-blocked projectiles reflect at 2x
```

- [ ] **Step 2: Loader mapping** — in `item_loader.cpp` `skillIdFromString`, next to the `blood_nova` row:

```cpp
    if (s == "static_charge")    return SkillId::STATIC_CHARGE;
    if (s == "hemophage")        return SkillId::HEMOPHAGE;
    if (s == "projectile_parry") return SkillId::PROJECTILE_PARRY;
```

- [ ] **Step 3: Names** — in `hud_inventory.cpp` `skillDisplayName`, before `default`:

```cpp
        case SkillId::STATIC_CHARGE:    return "Static Charge";
        case SkillId::HEMOPHAGE:        return "Hemophage";
        case SkillId::PROJECTILE_PARRY: return "Mirror Parry";
```

- [ ] **Step 4: Descriptions** — in `skillDescription`: (a) slot override next to the BLOOD_NOVA override block (CHAIN_LIGHTNING has a SkillDef, so tier 2 would otherwise show the castable text on the shield):

```cpp
    // Thunderwall: chain_lightning on an OFFHAND is a perfect-block riposte, not a cast.
    if (id == SkillId::CHAIN_LIGHTNING && slot == ItemSlot::OFFHAND) {
        return "On perfect block: arc lightning\nthrough your attacker.";
    }
```

(b) fallback cases (these three have NO def — tier 3 is their only source):

```cpp
        case SkillId::STATIC_CHARGE:    return "Hits you take build charge. At 5\nstacks: discharge chain lightning.";
        case SkillId::HEMOPHAGE:        return "Enemies within 4m constantly\nbleed life to you.";
        case SkillId::PROJECTILE_PARRY: return "Perfectly blocked projectiles\nreflect back at double damage.";
```

- [ ] **Step 5: Build** — `cmake --build build` → clean.
- [ ] **Step 6: Commit** — `git add -A src/game/item.h src/game/item_loader.cpp src/renderer/hud_inventory.cpp && git commit -m "feat(items): STATIC_CHARGE/HEMOPHAGE/PROJECTILE_PARRY skill ids + tooltips"`

### Task 2: Item defs + materials

**Files:** Modify `assets/config/items.json` (append inside `"items"` array — defIds 196–199), `assets/materials.json` (append — ids 161–164; `MAX_MATERIALS` is 192, headroom fine).

- [ ] **Step 1: materials.json** — append (id MUST equal array index):

```json
{"id": 161, "name": "armor_capacitor",    "texture": "armor_leather_tex_42.png", "tint": [0.45, 0.65, 0.95, 1.0]},
{"id": 162, "name": "armor_hemophage",    "texture": "armor_leather_tex_42.png", "tint": [0.55, 0.10, 0.15, 1.0]},
{"id": 163, "name": "shield_thunderwall", "texture": "ring_gold_tex_42.png",     "tint": [0.35, 0.55, 1.00, 1.0]},
{"id": 164, "name": "shield_mirror",      "texture": "ring_gold_tex_42.png",     "tint": [0.85, 0.92, 1.00, 1.0]}
```

- [ ] **Step 2: items.json** — append (Demonhide/Aegis template; the legendary-pool lint requires `minRarity` legendary on every skilled rollable def):

```json
{"name": "Capacitor Mail",   "slot": "armor",   "mesh": "armor",  "material": "armor_capacitor",    "baseHealth": 55.0, "minLevel": 16, "maxLevel": 28, "maxRarity": "legendary", "legendarySkill": "static_charge",    "dropWeight": 0.5, "minRarity": "legendary"},
{"name": "Hemophage Shroud", "slot": "armor",   "mesh": "armor",  "material": "armor_hemophage",    "baseHealth": 90.0, "minLevel": 32, "maxLevel": 50, "maxRarity": "legendary", "legendarySkill": "hemophage",        "dropWeight": 0.5, "minRarity": "legendary"},
{"name": "Thunderwall",      "slot": "offhand", "mesh": "shield", "material": "shield_thunderwall", "baseHealth": 46.0, "minLevel": 16, "maxLevel": 28, "maxRarity": "legendary", "legendarySkill": "chain_lightning",  "dropWeight": 0.5, "minRarity": "legendary"},
{"name": "Mirror Aegis",     "slot": "offhand", "mesh": "shield", "material": "shield_mirror",      "baseHealth": 75.0, "minLevel": 32, "maxLevel": 50, "maxRarity": "legendary", "legendarySkill": "projectile_parry", "dropWeight": 0.5, "minRarity": "legendary"}
```

- [ ] **Step 3: Verify the content lint** — `./build/tests/dungeon_tests -tc="*items.json*"` → PASS (the unique-marking lint reads the JSON directly and now covers the 4 new defs). Then run the game briefly (`./build/src/DungeonEngine --new warrior`, quit) and confirm the log line says `resolved visuals for 200 item defs` with no `LOG_WARN` about unresolved materials/skills.
- [ ] **Step 4: Commit** — `git add assets/config/items.json assets/materials.json && git commit -m "feat(items): Capacitor Mail, Hemophage Shroud, Thunderwall, Mirror Aegis defs"`

### Task 3: BlockOutcome (TDD)

**Files:** Modify `src/game/combat.h`, `src/game/combat.cpp:391-400`; Create `tests/game/test_block_outcome.cpp`; Modify `tests/CMakeLists.txt`.

- [ ] **Step 1: Failing test** — `tests/game/test_block_outcome.cpp`:

```cpp
// test_block_outcome.cpp — perfect-block classification is the trigger for every legendary
// shield effect, so its boundary is pinned: the perfect window is blockTimer < 0.2s STRICT.
#include <doctest/doctest.h>
#include "game/combat.h"

TEST_CASE("classifyBlock: not blocking is NONE regardless of timer") {
    CHECK(Combat::classifyBlock(false, 0.0f) == Combat::BlockOutcome::NONE);
    CHECK(Combat::classifyBlock(false, 5.0f) == Combat::BlockOutcome::NONE);
}
TEST_CASE("classifyBlock: first 0.2s of a raise is PERFECT, after that BLOCKED") {
    CHECK(Combat::classifyBlock(true, 0.0f)  == Combat::BlockOutcome::PERFECT);
    CHECK(Combat::classifyBlock(true, 0.19f) == Combat::BlockOutcome::PERFECT);
    CHECK(Combat::classifyBlock(true, 0.2f)  == Combat::BlockOutcome::BLOCKED);  // strict <
    CHECK(Combat::classifyBlock(true, 3.0f)  == Combat::BlockOutcome::BLOCKED);
}
```

Register in `tests/CMakeLists.txt` after `game/test_teleport_dest.cpp`: `game/test_block_outcome.cpp   # perfect-block window: strict <0.2s, the legendary-shield trigger` (header-only — no production .cpp to link).

- [ ] **Step 2: Run** — `./build/tests/dungeon_tests -tc="*classifyBlock*"` after configuring → FAIL (does not compile: `classifyBlock` undefined). Build failure counts as the red step.
- [ ] **Step 3: Implement** — in `combat.h` inside `namespace Combat`, above `applyDamageToPlayer`:

```cpp
    // How a hit interacted with the shield. PERFECT (raise-to-hit < 0.2s) negates all damage
    // and triggers the legendary-shield effects; BLOCKED halves it. Pure — the single source
    // for the window, used by applyDamageToPlayer and pinned by test_block_outcome.cpp.
    enum struct BlockOutcome : u8 { NONE, BLOCKED, PERFECT };
    inline BlockOutcome classifyBlock(bool blocking, f32 blockTimer) {
        if (!blocking) return BlockOutcome::NONE;
        return (blockTimer < 0.2f) ? BlockOutcome::PERFECT : BlockOutcome::BLOCKED;
    }
```

Change `applyDamageToPlayer`'s declared return type `void` → `BlockOutcome` (doc: "returns how the hit was blocked so the projectile path can reflect on PERFECT"). In `combat.cpp`, change the definition to match; every existing early `return;` becomes `return BlockOutcome::NONE;`; the block branch becomes:

```cpp
    BlockOutcome blockOutcome = classifyBlock(player.blocking, player.blockTimer);
    if (blockOutcome == BlockOutcome::PERFECT) {
        damage = 0.0f;
        if (s_perfectBlockCallback) s_perfectBlockCallback(player, attackerIdx);
    } else if (blockOutcome == BlockOutcome::BLOCKED) {
        damage *= 0.5f;
    }
```

and the function's final line becomes `return blockOutcome;`. (The callback signature change lands in Task 8 — for THIS task keep `s_perfectBlockCallback(player)` compiling by deferring the `attackerIdx` argument: pass nothing yet, i.e. keep the old call `s_perfectBlockCallback(player);` here and let Task 8 extend it. All other callers of `applyDamageToPlayer` ignore the return — no changes needed.)

- [ ] **Step 4: Run** — `./build/tests/dungeon_tests -tc="*classifyBlock*"` → PASS; full build clean.
- [ ] **Step 5: Commit** — `git commit -am "feat(combat): BlockOutcome return from applyDamageToPlayer (pure classifyBlock)"`

### Task 4: Player/NetPlayer fields + caching + view mirrors

**Files:** Modify `src/game/player.h` (~83 and ~110), `src/net/net_player.h` (frenzy region ~145), `src/engine/engine_update_skills.cpp` (`tickPassiveEquipment`, after the ring block ~line 152), `src/engine/engine_net.cpp` (~726), `src/engine/engine.cpp` (`seedRemoteView` ~1290, `writeBackRemoteView` ~1363).

- [ ] **Step 1: Player fields** — next to `ringPassive` (player.h:83):

```cpp
    u8   offhandSkill     = 0;    // SkillId of equipped legendary shield (0 = none) — read by the
                                  // perfect-block callback + projectile parry, which only get a Player&
```

next to the frenzy pair (player.h:110):

```cpp
    u8   chargeStacks     = 0;     // Static Charge (Capacitor Mail): hits-taken stacks, max 5
    f32  chargeTimer      = 0.0f;  // 10s window from last hit; 0 drops the stacks
    f32  hemoTickTimer    = 0.0f;  // Hemophage aura: 0.5s drain-tick accumulator
```

- [ ] **Step 2: NetPlayer fields** — next to `frenzyStacks`/`frenzyTimer` in net_player.h:

```cpp
    // Legendary armor/shield passives for a REMOTE (2026-07-16 batch; mirrors the Player fields;
    // not on the wire except chargeStacks, which rides SnapPlayer.flags bits 5-7 for the HUD).
    u8   offhandSkill      = 0;
    u8   chargeStacks      = 0;
    f32  chargeTimer       = 0.0f;
    f32  hemoTickTimer     = 0.0f;
    // Attacker of the last hit, written back from the view like lastDamageTaken — lets the
    // Static Charge discharge and Thunderwall riposte aim at who actually hit a remote.
    u16  lastDamageAttackerIdx = 0xFFFF;
```

- [ ] **Step 3: Local cache** — in `tickPassiveEquipment` after the ring block:

```cpp
    // Offhand passive (legendary shield identity). Stamped onto the Player like ringPassive,
    // because the perfect-block callback and projectile parry receive only a Player& —
    // the local player here; remote views get it via serverNetPost + seedRemoteView.
    {
        const ItemInstance& off = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        m_localPlayer.offhandSkill = static_cast<u8>((!isItemEmpty(off) && off.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[off.defId].legendarySkillId : SkillId::NONE);
    }
```

- [ ] **Step 4: Server cache** — in `engine_net.cpp` after the `np.ringPassive` assignment (~726):

```cpp
        const ItemInstance& offItem = m_inventories[pi].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        np.offhandSkill = static_cast<u8>((!isItemEmpty(offItem) && offItem.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[offItem.defId].legendarySkillId : SkillId::NONE);
```

- [ ] **Step 5: View mirrors** — in `seedRemoteView` (engine.cpp, after the `v.secondWindCooldown` line):

```cpp
    // Legendary armor/shield passives (2026-07-16): the block callback + parry read these off
    // the view; bloodNovaCooldown round-trips so a remote Aegis of Blood can't re-detonate
    // every block (the old reason it was pinned to the local player).
    v.offhandSkill      = np.offhandSkill;
    v.chargeStacks      = np.chargeStacks;
    v.chargeTimer       = np.chargeTimer;
    v.hemoTickTimer     = np.hemoTickTimer;
    v.bloodNovaCooldown = np.bloodNovaCooldown;
```

in `writeBackRemoteView` (with the other write-backs, near where `np.lastDamageTaken` is persisted):

```cpp
    np.chargeStacks          = v.chargeStacks;
    np.chargeTimer           = v.chargeTimer;
    np.hemoTickTimer         = v.hemoTickTimer;
    np.bloodNovaCooldown     = v.bloodNovaCooldown;
    np.lastDamageAttackerIdx = v.lastDamageAttackerIdx;
```

(If `writeBackRemoteView` does not currently persist `v.lastDamageTaken` → `np.lastDamageTaken`, it does — find it and put these beside it.)

- [ ] **Step 6: Build + full suite** — `cmake --build build && ./build/tests/dungeon_tests` → green.
- [ ] **Step 7: Commit** — `git commit -am "feat(net): offhandSkill/charge/hemophage state on Player+NetPlayer with view mirrors"`

### Task 5: INPUT_BLOCK on the wire (TDD)

**Files:** Modify `src/net/net_player.h:46-48`, `src/game/player.cpp` (`captureLocalInput` ~430, `updateNetPlayerFromInput` ~345), `tests/net/test_input_wire.cpp`, `src/net/net.h:80`.

- [ ] **Step 1: Failing test** — append to `test_input_wire.cpp`:

```cpp
TEST_CASE("INPUT_BLOCK: occupies the old reserved lock bit and survives the wire") {
    CHECK(INPUT_BLOCK == (1 << 6));   // reuses INPUT_LOCK's slot — wire layout unchanged (v19)
    NetInput in{}; in.clientTick = 9; in.moveFlags = INPUT_FORWARD | INPUT_BLOCK;
    NetInput window[1] = { in };
    u8 buf[128];
    u32 written = InputWire::serialize(window, 1, buf);
    NetInput out[4]; u32 count = 0;
    InputWire::deserialize(buf, written, out, count);
    REQUIRE(count == 1);
    CHECK((out[0].moveFlags & INPUT_BLOCK) != 0);
    CHECK((out[0].moveFlags & INPUT_FORWARD) != 0);
}
```

(Match the serialize/deserialize call shapes used by the existing "InputWindow: serializes and deserializes 4 inputs cleanly" case in this file — copy its exact function names/arity.)

- [ ] **Step 2: Run** — `-tc="*INPUT_BLOCK*"` → FAIL (INPUT_BLOCK undefined).
- [ ] **Step 3: Rename the bit** — in net_player.h replace the `INPUT_LOCK` definition + comment with:

```cpp
// Blocking (Ctrl / LT held). Reuses the dead lock-on bit (reserved-always-0 since the feature
// was cut) so the flags byte layout — and the wire format — is unchanged; v19 gives it meaning.
static constexpr u8 INPUT_BLOCK    = 1 << 6;
```

Fix the stale `INPUT_LOCK` comment in `player.cpp:~432` while there (it now documents the block bit).

- [ ] **Step 4: Capture** — in `captureLocalInput`, with the other `isActionDown` rows (BEFORE the `s_botWalk` override so the bot stays deterministic):

```cpp
    if (Input::isActionDown(GameAction::BLOCK))      flags |= INPUT_BLOCK;
```

- [ ] **Step 5: Server drain + prediction replay** — in `updateNetPlayerFromInput`, after the `markSpeedStacks` speed block and before the yaw assignment:

```cpp
    // Blocking. State is input-derived (INPUT_BLOCK) so a reconcile replay reproduces it
    // exactly. The 0.4x slow MUST mirror the local branch in engine_update.cpp (blocking
    // slows) or every blocking guest rubber-bands. blockTimer advances on live processing
    // only — it feeds perfect-block classification, which never runs during a replay.
    const bool wantsBlock = (input.moveFlags & INPUT_BLOCK) != 0;
    if (!movementOnly) {
        if (wantsBlock && !np.blocking) np.blockTimer = 0.0f;   // raise edge: perfect window opens
        else if (wantsBlock)            np.blockTimer += dt;
        np.blocking = wantsBlock;
    }
    if (wantsBlock) effectiveSpeed *= 0.4f;
```

- [ ] **Step 6: Protocol bump** — net.h:80 `PROTOCOL_VERSION` 18 → 19, append to its comment block: `// v19: INPUT_BLOCK (old reserved lock bit) feeds server-side blocking; SnapPlayer.flags bits 5-7 carry Static Charge stacks. No packet layout change — bump is same-build insurance.`
- [ ] **Step 7: Run** — new test PASSES; full suite green; build clean.
- [ ] **Step 8: Commit** — `git commit -am "feat(net): block state on the wire (INPUT_BLOCK) + server-side block sim, PROTOCOL 19"`

### Task 6: Static Charge (TDD)

**Files:** Create `src/game/static_charge.h`, `tests/game/test_static_charge.cpp`; Modify `tests/CMakeLists.txt`, `src/engine/engine.h` (private method decls), `src/engine/engine_update_skills.cpp` (host wiring + `staticDischarge`), `src/engine/engine_net.cpp` (remote wiring + client adoption), `src/net/snapshot.cpp:48`, `src/engine/engine_hud.cpp` (statuses[] tail ~889), `tools/gen_status_icons.py`, `src/renderer/hud_status.cpp`.

- [ ] **Step 1: Failing test** — `tests/game/test_static_charge.cpp`:

```cpp
// test_static_charge.cpp — Capacitor Mail stack logic. One pure helper feeds all four call
// sites (host/remote x tick/discharge), so this is the whole behavior surface.
#include <doctest/doctest.h>
#include "game/static_charge.h"

TEST_CASE("StaticCharge: hits build stacks, the 5th discharges and resets") {
    u8 s = 0; f32 t = 0.0f;
    for (int i = 0; i < 4; i++) CHECK_FALSE(StaticCharge::accumulate(s, t, true, 0.016f));
    CHECK(s == 4);
    CHECK(t == doctest::Approx(StaticCharge::WINDOW_SEC));
    CHECK(StaticCharge::accumulate(s, t, true, 0.016f));   // 5th hit -> discharge
    CHECK(s == 0); CHECK(t == 0.0f);                        // reset after discharge
}
TEST_CASE("StaticCharge: stacks decay when the window runs out") {
    u8 s = 0; f32 t = 0.0f;
    StaticCharge::accumulate(s, t, true, 0.016f);
    CHECK(s == 1);
    StaticCharge::accumulate(s, t, false, StaticCharge::WINDOW_SEC + 1.0f);
    CHECK(s == 0); CHECK(t == 0.0f);
}
TEST_CASE("StaticCharge: quiet ticks change nothing") {
    u8 s = 2; f32 t = 5.0f;
    CHECK_FALSE(StaticCharge::accumulate(s, t, false, 0.016f));
    CHECK(s == 2); CHECK(t == doctest::Approx(5.0f - 0.016f));
}
```

Register in `tests/CMakeLists.txt`: `game/test_static_charge.cpp   # Capacitor Mail stack window (header-only)`.

- [ ] **Step 2: Run** → FAIL (header missing).
- [ ] **Step 3: Implement** — `src/game/static_charge.h`:

```cpp
#pragma once
// static_charge.h — Capacitor Mail (STATIC_CHARGE armor passive) stack logic. Pure so the four
// call sites (host + remote, in tickArmorRingPassives and serverNetPost) cannot drift and the
// tests can pin it without linking the engine.
#include "core/types.h"

namespace StaticCharge {
    constexpr u8  MAX_STACKS = 5;
    constexpr f32 WINDOW_SEC = 10.0f;   // stacks drop this long after the last hit

    // Advance the window and absorb this tick's "was struck" signal. Returns true exactly when
    // the new hit fills the 5th stack — the caller discharges (chain lightning at the attacker)
    // and the stacks are already reset here.
    inline bool accumulate(u8& stacks, f32& timer, bool struck, f32 dt) {
        if (timer > 0.0f) {
            timer -= dt;
            if (timer <= 0.0f) { timer = 0.0f; stacks = 0; }
        }
        if (!struck) return false;
        stacks++;
        timer = WINDOW_SEC;
        if (stacks >= MAX_STACKS) { stacks = 0; timer = 0.0f; return true; }
        return false;
    }
}
```

- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: The discharge helper** — declare in `engine.h` (private, near `detonateBloodNova`):

```cpp
    void staticDischarge(Vec3 pos, u8 wearerSlot, u16 attackerIdx); // Capacitor Mail / Thunderwall
    void hemophageAuraTick(Vec3 pos, u8 wearerSlot, f32& tickTimer, f32& health, f32 maxHealth, f32 dt);
```

Implement `staticDischarge` in `engine_update_skills.cpp` (add `#include "game/skill_internal.h"` and `#include "game/static_charge.h"` at the top):

```cpp
// ---------------------------------------------------------------------------
// staticDischarge — fire a full chain lightning at whoever hit the wearer (Capacitor Mail's
// 5-stack discharge; Thunderwall's perfect-block riposte reuses it). Server/SP only: damage
// is authoritative (N5). Falls back to the nearest enemy within 5m when the hit had no source
// entity (projectiles/AoE stamp attackerIdx 0xFFFF), thorns-style.
// ---------------------------------------------------------------------------
void Engine::staticDischarge(Vec3 pos, u8 wearerSlot, u16 attackerIdx) {
    if (m_netRole == NetRole::CLIENT) return;

    Vec3 target; bool found = false;
    if (attackerIdx < MAX_ENTITIES) {
        const Entity& ae = m_entities.entities[attackerIdx];
        if ((ae.flags & ENT_ACTIVE) && !(ae.flags & ENT_DEAD) && !(ae.flags & ENT_FRIENDLY)) {
            target = ae.position; found = true;
        }
    }
    if (!found) {   // sourceless hit: nearest living hostile within 5m
        f32 best = 25.0f;
        for (u32 a = 0; a < m_entities.activeCount; a++) {
            const Entity& e = m_entities.entities[m_entities.activeList[a]];
            if ((e.flags & ENT_DEAD) || (e.flags & ENT_FRIENDLY) || e.enemyType == EnemyType::PROP) continue;
            Vec3 d = e.position - pos;
            f32 d2 = d.x*d.x + d.z*d.z;
            if (d2 < best) { best = d2; target = e.position; found = true; }
        }
    }
    if (!found) return;

    const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, SkillId::CHAIN_LIGHTNING);
    if (!def) return;
    Vec3 origin = pos + Vec3{0, 1.2f, 0};
    Vec3 dir    = normalize(target - origin);
    // Neutral scaling: a proc doesn't ride whoever cast a class skill last. Kill credit rides
    // setAttackingPlayer (the projectile.cpp save/restore pattern).
    SkillSystem::setClassDamageMult(1.0f);
    SkillSystem::setSkillPower(0.0f);
    u8 prev = Combat::getAttackingPlayer();
    Combat::setAttackingPlayer(wearerSlot);
    fireChainLightning(origin, dir, def, m_level.grid, m_entities);
    Combat::setAttackingPlayer(prev);
}
```

- [ ] **Step 6: Host wiring** — in `tickArmorRingPassives`, next to the Blood Nova on-struck block (~line 412, BEFORE `lastDamageTaken` is cleared at the function tail):

```cpp
    // Static Charge (Capacitor Mail): being struck charges the armor; the 5th stack discharges
    // chain lightning into the attacker. Same lastDamageTaken source as Blood Nova above — and
    // like it, inert on a CLIENT (a guest's stacks tick server-side; the HUD adopts them from
    // SnapPlayer.flags in clientNetPost).
    if (m_armorAura == SkillId::STATIC_CHARGE) {
        if (StaticCharge::accumulate(m_localPlayer.chargeStacks, m_localPlayer.chargeTimer,
                                     m_localPlayer.lastDamageTaken > 0.0f, dt))
            staticDischarge(m_localPlayer.position, activeNetSlot(), m_localPlayer.lastDamageAttackerIdx);
    }
```

- [ ] **Step 7: Remote wiring** — in `engine_net.cpp` inside the existing `if (pi >= m_splitPlayerCount)` armor block, after the Blood Nova lines and BEFORE `np.lastDamageTaken = 0.0f;` (add `#include "game/static_charge.h"`):

```cpp
            // Static Charge for a REMOTE (host lanes run theirs in tickArmorRingPassives).
            if (np.armorAura == SkillId::STATIC_CHARGE) {
                if (StaticCharge::accumulate(np.chargeStacks, np.chargeTimer,
                                             np.lastDamageTaken > 0.0f, dt))
                    staticDischarge(np.position, np.slotIndex, np.lastDamageAttackerIdx);
            }
```

- [ ] **Step 8: Replicate + adopt** — snapshot.cpp, after `if (np.blocking) flags |= (1 << 4);`:

```cpp
        // Static Charge stacks (0-5) in bits 5-7 — the pose byte's spare bits.
        // (statusFlags' 5-6 belong to the shrine-buff type; do NOT move these there.)
        flags |= static_cast<u8>((np.chargeStacks & 0x07u) << 5);
```

engine_net.cpp `clientNetPost`, directly after the shrine-buff adoption block (~562):

```cpp
    // Static Charge stacks — ADOPT, don't predict (same reasoning as the shrine buff above):
    // they accumulate from server-authoritative damage. chargeTimer just keeps the HUD row lit.
    m_localPlayer.chargeStacks = static_cast<u8>((sp->flags >> 5) & 0x07u);
    m_localPlayer.chargeTimer  = (m_localPlayer.chargeStacks > 0) ? 1.0f : 0.0f;
```

- [ ] **Step 9: HUD row + glyph** — engine_hud.cpp statuses[]: append as the LAST row (row index 11 — the icon table is row-index-keyed):

```cpp
                // Capacitor Mail charge (row 11): timer drives visibility, value shows stacks.
                {"CHG", {0.45f, 0.80f, 1.00f}, m_localPlayer.chargeTimer,
                    m_localPlayer.chargeStacks > 0 ? static_cast<f32>(m_localPlayer.chargeStacks) : -1.0f},
```

`tools/gen_status_icons.py` ICONS list: append a `("Capacitor", [art], [palette])` entry matching the shrine entries' exact tuple shape — art (a lightning bolt):

```python
    # --- Capacitor Mail: a charge bolt — electric blue on a steel outline. ---
    ("Capacitor", [
        "....44..",
        "...44...",
        "..44....",
        ".444444.",
        "...44...",
        "..44....",
        ".44.....",
        ".4......",
    ], [
        (0.15, 0.25, 0.35),   # 1 dark steel
        (0.30, 0.50, 0.70),   # 2 mid blue
        (0.55, 0.80, 1.00),   # 3 bright edge
        (0.75, 0.95, 1.00),   # 4 the bolt
    ]),
```

(If the existing entries carry a 5th palette tuple for index 0, prepend `(0.0, 0.0, 0.0)` — copy the neighbors' length; the generator/emitter will fail loudly on a mismatch.) Run `python3 tools/gen_status_icons.py` → regenerates `src/renderer/status_icons_data.h` (committed by design). Then hud_status.cpp: `STATUS_ICON_COUNT` 11 → 12; append `&kIconCapacitor[0][0]` to `icons[]`; extend `getStatusColors`'s generated-palette branch:

```cpp
        const Vec3* pal = (idx == 8)  ? kPalShrinePower
                        : (idx == 9)  ? kPalShrineSpeed
                        : (idx == 10) ? kPalShrineVitality
                                      : kPalCapacitor;
```

- [ ] **Step 10: Build + suite** → green. **Commit** — `git commit -am "feat(game): Capacitor Mail — Static Charge stacks, discharge, HUD row + wire bits"`

### Task 7: Hemophage aura

**Files:** Modify `src/engine/engine_update_skills.cpp` (helper + host wiring), `src/engine/engine_net.cpp` (remote wiring).

- [ ] **Step 1: Helper** — in engine_update_skills.cpp:

```cpp
// ---------------------------------------------------------------------------
// hemophageAuraTick — Hemophage Shroud: enemies within 4m bleed 3 damage every 0.5s (6 dps)
// to the wearer, who heals for the total. Ticked (not per-frame) to bound applyDamage calls;
// heals are ratio-safe on the wire (health up, maxHealth untouched — no HP-bar lurch).
// Server/SP only: the client's entity pool is a discarded ghost (N5).
// ---------------------------------------------------------------------------
void Engine::hemophageAuraTick(Vec3 pos, u8 wearerSlot, f32& tickTimer, f32& health,
                               f32 maxHealth, f32 dt) {
    if (m_netRole == NetRole::CLIENT) return;
    tickTimer -= dt;
    if (tickTimer > 0.0f) return;
    tickTimer = 0.5f;

    constexpr f32 DRAIN_PER_TICK = 3.0f;   // x2 ticks/s = the spec's 6 dps per enemy
    u8 prev = Combat::getAttackingPlayer();
    Combat::setAttackingPlayer(wearerSlot);  // drain kills credit (and reserve loot to) the wearer
    f32 drained = 0.0f;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& ent = m_entities.entities[idx];
        if ((ent.flags & ENT_DEAD) || (ent.flags & ENT_FRIENDLY)) continue;
        if (ent.enemyType == EnemyType::PROP) continue;
        Vec3 d = ent.position - pos;
        if (d.x*d.x + d.z*d.z > 16.0f) continue;   // 4m XZ radius, squared
        EntityHandle h; h.index = static_cast<u16>(idx); h.generation = ent.generation;
        Combat::applyDamage(m_entities, h, DRAIN_PER_TICK, &pos);
        drained += DRAIN_PER_TICK;
    }
    Combat::setAttackingPlayer(prev);
    if (drained > 0.0f && health > 0.0f) health = fminf(health + drained, maxHealth);
}
```

- [ ] **Step 2: Host wiring** — in `tickArmorRingPassives`, directly under the Static Charge block from Task 6:

```cpp
    // Hemophage (Hemophage Shroud): 4m life-drain aura. Damaging (NOT idempotent like the
    // burn/freeze aura timers), so host lanes run it here and remotes ONLY in serverNetPost.
    if (m_armorAura == SkillId::HEMOPHAGE)
        hemophageAuraTick(m_localPlayer.position, activeNetSlot(), m_localPlayer.hemoTickTimer,
                          m_localPlayer.health, m_localPlayer.maxHealth, dt);
```

- [ ] **Step 3: Remote wiring** — engine_net.cpp, inside the same `pi >= m_splitPlayerCount` block as Task 7's Static Charge lines:

```cpp
            if (np.armorAura == SkillId::HEMOPHAGE)
                hemophageAuraTick(np.position, np.slotIndex, np.hemoTickTimer,
                                  np.health, np.maxHealth, dt);
```

- [ ] **Step 4: Build + suite** → green. **Commit** — `git commit -am "feat(game): Hemophage Shroud — 4m life-drain aura, host + remote"`

### Task 8: Perfect-block callback rework (Thunderwall + remote-capable dispatch)

**Files:** Modify `src/game/combat.h:150`, `src/game/combat.cpp:395`, `src/engine/engine_init_callbacks.cpp:456-503`.

- [ ] **Step 1: Signature** — combat.h:

```cpp
    // Perfect block callback — fired for the BLOCKER (local player on host/SP, or a server-side
    // remote view: identity via Player::netSlot, shield via Player::offhandSkill). attackerIdx
    // is the striking entity (0xFFFF for projectiles/AoE), so ripostes can aim.
    using PerfectBlockCallback = void(*)(Player& player, u16 attackerIdx);
```

combat.cpp block branch (from Task 3): the call becomes `s_perfectBlockCallback(player, attackerIdx);`.

- [ ] **Step 2: Rework the callback** — replace the body in engine_init_callbacks.cpp:457-503 with:

```cpp
    // Perfect block callback — legendary shield effects, dispatched on the BLOCKER's cached
    // offhand skill (stamped by tickPassiveEquipment locally, serverNetPost + seedRemoteView
    // for remote views). The old inventory lookup used m_localPlayerIndex, so a remote's
    // block read the HOST's shield; and bloodNovaCooldown now round-trips through NetPlayer,
    // so the old "view resets the cooldown every frame" hazard (the reason this was pinned
    // to &m_localPlayer) is gone. Fires server-side only in netplay — a CLIENT never runs
    // applyDamageToPlayer on itself; guests see the result via snapshots + FX events.
    Combat::setPerfectBlockCallback([](Player& player, u16 attackerIdx) {
        if (!s_engine) return;
        switch (static_cast<SkillId>(player.offhandSkill)) {
            case SkillId::NONE:
                return;                       // no legendary shield — no proc
            case SkillId::BLOOD_NOVA:         // Aegis of Blood: sacrifice 20% health, erupt
                s_engine->detonateBloodNova(player.position, player.netSlot,
                                            player.health, player.bloodNovaCooldown);
                return;
            case SkillId::CHAIN_LIGHTNING:    // Thunderwall: riposte lightning at the attacker
                s_engine->staticDischarge(player.position, player.netSlot, attackerIdx);
                return;
            case SkillId::PROJECTILE_PARRY:
                // Mirror Aegis: projectile blocks reflect at the projectile-hit site (which
                // passes attackerIdx 0xFFFF). A MELEE perfect block has no projectile to send
                // back — fall through to the generic freeze bash instead (per spec).
                if (attackerIdx >= MAX_ENTITIES) return;
                break;
            default:
                break;                        // any other legendary shield: generic freeze bash
        }
        for (u32 a = 0; a < s_engine->m_entities.activeCount; a++) {
            u32 idx = s_engine->m_entities.activeList[a];
            Entity& ent = s_engine->m_entities.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;
            if (ent.enemyType == EnemyType::PROP) continue;
            f32 dist = length(ent.position - player.position);
            if (dist < 3.0f) ent.freezeTimer = 1.0f;
        }
        for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
            if (!s_engine->m_fx.novaFX[ni].active) {
                s_engine->m_fx.novaFX[ni] = {player.position, 3.0f, 0.4f, true, Vec3{0.8f, 0.8f, 1.0f}};
                break;
            }
        }
    });
```

- [ ] **Step 3: Build + suite** → green (the compiler finds any remaining 1-arg callback callers). **Commit** — `git commit -am "feat(game): Thunderwall riposte + per-shield perfect-block dispatch (remote-capable)"`

### Task 9: Mirror Aegis projectile parry (TDD)

**Files:** Create `tests/game/test_projectile_parry.cpp`; Modify `tests/CMakeLists.txt`, `src/game/projectile.h`, `src/game/projectile.cpp` (`tryHitPlayer` :102-137 + result handling :419-429).

- [ ] **Step 1: Failing test** — `tests/game/test_projectile_parry.cpp`:

```cpp
// test_projectile_parry.cpp — Mirror Aegis reflect: the projectile survives, flips sides,
// reverses course, doubles damage, and sheds the enemy's on-hit rider.
#include <doctest/doctest.h>
#include "game/projectile.h"

TEST_CASE("reflectAsParry: flips owner/side, reverses velocity, doubles damage") {
    Projectile p{};
    p.velocity = {10.0f, 2.0f, -4.0f};
    p.damage = 25.0f; p.fromPlayer = false; p.ownerSlot = 0xFF;
    p.lifetime = 0.05f;                       // nearly spent on arrival
    p.onHitEffect = 1; p.onHitDuration = 3.0f; // enemy poison rider
    ProjectileSystem::reflectAsParry(p, /*newOwnerSlot=*/2);
    CHECK(p.velocity.x == doctest::Approx(-10.0f));
    CHECK(p.velocity.y == doctest::Approx(-2.0f));
    CHECK(p.velocity.z == doctest::Approx(4.0f));
    CHECK(p.fromPlayer);                      // never re-tested vs players — can't re-hit blocker
    CHECK(p.ownerSlot == 2);                  // kills credit the blocker
    CHECK(p.damage == doctest::Approx(50.0f));
    CHECK(p.lifetime == doctest::Approx(3.0f)); // fresh flight window for the return trip
    CHECK(p.onHitEffect == 0);
    CHECK(p.onHitDuration == 0.0f);
}
```

Register in `tests/CMakeLists.txt`: `game/test_projectile_parry.cpp   # Mirror Aegis reflect (header-only)`.

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** — in projectile.h inside `namespace ProjectileSystem`:

```cpp
    // Mirror Aegis perfect-block parry: flip the projectile to the blocker and send it back the
    // way it came at double damage. fromPlayer=true removes it from every player AABB pass, so
    // a reflected shot can never re-hit the blocker or a teammate; the enemy's on-hit rider
    // (poison/slow/...) dies with the parry. Pure — pinned by test_projectile_parry.cpp.
    inline void reflectAsParry(Projectile& p, u8 newOwnerSlot) {
        p.velocity      = p.velocity * -1.0f;
        p.fromPlayer    = true;
        p.ownerSlot     = newOwnerSlot;
        p.damage       *= 2.0f;
        p.lifetime      = 3.0f;
        p.onHitEffect   = 0;
        p.onHitDuration = 0.0f;
    }
```

- [ ] **Step 4: Wire into tryHitPlayer** — extend the local enum: `enum class PlayerHitResult { MISS, DEFLECTED, HIT, REFLECTED };`. Replace the plain `Combat::applyDamageToPlayer(...)` call (:120) with:

```cpp
    Combat::BlockOutcome outcome = Combat::applyDamageToPlayer(player, p.damage, &p.position);
    // Mirror Aegis: a PERFECT block reflects the projectile instead of eating it. Placed before
    // the on-hit status below — a parried shot must not also poison/slow the blocker.
    if (outcome == Combat::BlockOutcome::PERFECT &&
        player.offhandSkill == static_cast<u8>(SkillId::PROJECTILE_PARRY)) {
        ProjectileSystem::reflectAsParry(p, player.netSlot);
        return PlayerHitResult::REFLECTED;
    }
```

and in the update loop's result handling (:427-428) add: `if (r == PlayerHitResult::REFLECTED) { continue; } // projectile lives on, now player-owned`. (`#include "game/combat.h"` and `#include "game/item.h"` in projectile.cpp if not already present.)

- [ ] **Step 5: Build + suite** → green. **Commit** — `git commit -am "feat(game): Mirror Aegis — perfect-blocked projectiles reflect at 2x"`

### Task 10: Docs sync

**Files:** Modify `CLAUDE.md` (netplay paragraph: PROTOCOL 18 → 19 with the v19 one-liner), `.claude/skills/engine-reference/SKILL.md` (SkillId additions; INPUT_BLOCK meaning of bit 6; SnapPlayer.flags bits 5–7; status row 11 + STATUS_ICON_COUNT 12; the four new items + their effects; blocking now server-simulated), `.claude/skills/engine-how-to/SKILL.md` (extend the "player buff not on NetPlayer" pitfall with: `SnapPlayer` has TWO flag bytes — `statusFlags` bits 5–6 are the shrine type, the pose byte `flags` holds bits 5–7 spare; check BOTH before claiming a bit).

- [ ] **Step 1: Apply the three doc edits above.**
- [ ] **Step 2: Commit** — `git commit -am "docs: sync CLAUDE.md + engine skills for legendary armor/shield batch (v19)"`

### Task 11: Full verification

- [ ] **Step 1: Build matrix** — `cmake --build build && cmake --build build-rel && cmake --build build-steam` → all clean (the 4 known `-Wunused-result` warnings in release are pre-existing).
- [ ] **Step 2: Full suite** — `./build/tests/dungeon_tests` → all green.
- [ ] **Step 3: SP smoke** — `./build/src/DungeonEngine --new warrior` from the repo root: confirm 200 defs resolve, no unresolved-material/skill warnings, HUD/menu fine.
- [ ] **Step 4: Co-op soak** — two instances: `./build/src/DungeonEngine --host --new warrior --lan --net-loss 10 --net-latency 50` + `./build/src/DungeonEngine --join 127.0.0.1 --new sorcerer --bot-walk`; let it run ≥60 s. Expect `[NET-GRAPH]` div=0, no errors, join accepted (v19 both sides).
- [ ] **Step 5: Record the playtest checklist** in the pending-playtest memory (guest wears each of the 4 items; guest blocks without rubber-band; guest perfect-block procs show on both screens; guest CHG stacks visible; reflected-projectile kill credits the blocker; known cosmetic parity gap: chain-lightning arcs from a remote's proc render host-side only, matching remote casts today).

---

## Self-review notes

- Spec coverage: items (T2), SkillIds/tooltips (T1), Static Charge incl. HUD + wire bits (T6), Hemophage (T7), Thunderwall + callback rework (T8), Mirror Aegis + BlockOutcome (T3, T9), INPUT_BLOCK + block sim + slow + PROTOCOL 19 (T5), view mirrors incl. `bloodNovaCooldown` remote-Aegis fix (T4), docs (T10), verification (T11). No spec item unowned.
- The `s_perfectBlockCallback(player)` 1-arg call is deliberately kept through Tasks 3–7 and extended in Task 8 — tasks stay individually compilable.
- Type consistency: `offhandSkill`/`ringPassive` are `u8` casts of `SkillId` on `Player` (established pattern); `chargeStacks` u8 / `chargeTimer` f32 everywhere; `lastDamageAttackerIdx` u16 with 0xFFFF sentinel matching `Player`.

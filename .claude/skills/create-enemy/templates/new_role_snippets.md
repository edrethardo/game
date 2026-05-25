# New EnemyRole + gimmick — code snippets

Apply these **only** when the user chose gimmick approach B/Hybrid (Step 8). All four edits
are needed together. `role` is **not** serialized in the snapshot, so widening it is a safe
local-only change — there are no `net/` edits.

Replace `<role>` / `<ROLE>` with your snake_case / UPPER_CASE name.

## 1. Add the role constant — `src/game/entity.h` (~line 51, `EnemyRole` namespace)

The 8 existing bits `0x01..0x80` fill a `u8`; the next bit is `0x100`, which requires `u16`
(do edit 2). Add:

```cpp
namespace EnemyRole {
    // ... existing NORMAL..SHIELD_BEARER ...
    constexpr u16 <ROLE> = 0x100;  // <one-line description of the gimmick>
}
```

## 2. Widen the role type `u8` → `u16` (three sites)

```cpp
// src/game/entity.h  (~line 147) — the per-entity runtime role
u16 enemyRole = EnemyRole::NORMAL;   // was: u8

// src/game/enemy_def.h  (~line 31) — the def loaded from JSON
u16  role = 0;                        // was: u8   (EnemyRole bitmask)

// src/game/enemy_loader.cpp — parseRole() return type AND its local mask
static u16 parseRole(const std::string& s) {   // was: static u8
    ...
}
// ...inside load(), the combined-roles branch:
u16 mask = EnemyRole::NORMAL;        // was: u8 mask
for (auto& r : entry["role"]) mask |= parseRole(r.get<std::string>());
def.role = mask;
```

Also make sure the existing `EnemyRole::NORMAL..SHIELD_BEARER` constants are still `u8`-valued
(they are) — only the *fields* and *parse function* widen; the OR-ing is unaffected.

## 3. Map the role string — `src/game/enemy_loader.cpp` (`parseRole`, ~line 30)

```cpp
    if (s == "<role>")        return EnemyRole::<ROLE>;
```

## 4. Add the behavior branch — `src/game/enemy_ai_roles.cpp`

Add inside `applyRoleModifiers(...)`, before the final `return AIStep::Continue;`. Copy the
shape of CHARGER (~line 168) or SUMMONER (~line 28). Function signature for reference:

```cpp
AIStep applyRoleModifiers(Entity& e, u32 i, EntityPool& pool,
                          Player& player, Player* targetPlayer,
                          const LevelGrid& grid, f32 dt,
                          f32 dist, Vec3 playerEye);
```

```cpp
    // <ROLE>: <describe the gimmick and WHY it works this way>
    if (e.enemyRole & EnemyRole::<ROLE>) {
        // Reuse a multi-purpose scratch timer instead of adding a new Entity field:
        //   tacticalTimer / kiteTimer / sprintTimer / animTimer (f32), hasRetreated (bool)
        e.tacticalTimer -= dt;
        if (e.tacticalTimer <= 0.0f) {
            e.tacticalTimer = 3.0f;          // cooldown between activations
            // TODO: the gimmick. Examples of what existing roles do:
            //   - spawn/raise entities (SUMMONER): scan pool, EntitySystem::spawn / clear ENT_DEAD
            //   - buff allies (AURA/HEALER): loop nearby entities, adjust moveSpeed/health
            //   - movement change (CHARGER): set e.moveSpeed / e.aiState
            //   - hurt the player: targetPlayer->... (guard targetPlayer != nullptr)
        }
        // Return AIStep::BreakLoop only if THIS entity died this tick (see CHARGER+BOMBER).
    }
```

Available `Entity` scratch state (no new fields needed): `tacticalTimer`, `kiteTimer`,
`sprintTimer`, `hasRetreated`, `animTimer`, `attackAnimT`, `resurrectCount`, plus
`aiState`, `moveSpeed`/`baseMoveSpeed`, `attackCooldown`. Guard `targetPlayer` for null.

## 5. Doc-sync

Add the new role to the role list in the `engine-reference` skill (per the CLAUDE.md doc-sync
rule). Then continue with Step 9 (build assets) and Step 10 (compile).

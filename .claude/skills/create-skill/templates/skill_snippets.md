# New skill — code snippets

Replace `<NAME>` (UPPER_SNAKE enum), `<name>` (snake_case id), `<Name>` (CamelCase icon).
Example: `INFERNO_BURST` / `inferno_burst` / `InfernoBurst`.

## 1. SkillId enum — `src/game/item.h` (~line 77, before `COUNT`)

```cpp
enum struct SkillId : u8 {
    NONE = 0,
    FROZEN_ORB, CHAIN_LIGHTNING, METEOR_STRIKE, BLOOD_NOVA, PHASE_DASH,
    // ... class skills ...
    <NAME>,          // APPEND — order is wire/save-significant
    COUNT
};
```

## 2. SkillDef fields — `src/game/item.h` (`SkillDef`, ~line 297) — only if needed

```cpp
    f32 <param1> = 0.0f;   // e.g. burstRadius
    f32 <param2> = 0.0f;   // e.g. burstInterval
```
(Many skills reuse the common fields: `cooldown`, `energyCost`, `damage`, `radius`, `duration`,
`projectileSpeed`, `projectileCount`.)

## 3. Loader — `src/game/item_loader.cpp`

`skillIdFromString` (~line 56):
```cpp
    if (s == "<name>") return SkillId::<NAME>;
```
`loadSkillDefs` (~line 309) — parse any new fields by exact JSON name:
```cpp
    if (entry.contains("<param1>")) def.<param1> = entry.value("<param1>", 0.0f);
```

## 4. Fire function — `src/game/skill_legendary.cpp` (+ decl in `src/game/skill_internal.h`)

Model on `fireChainLightning` / `fireMeteorStrike`. Use the file-scope scalers
`s_classDmgMult` / `s_skillPower` for level scaling, and the VFX callbacks if relevant.
```cpp
void fire<Name>(Vec3 origin, Vec3 direction, const SkillDef* def,
                const LevelGrid& grid, EntityPool& entities) {
    // INSTANTANEOUS: query entities / raycast / spawn projectiles now, apply damage.
    //   e.g. CombatQuery::queryConeSorted(...), Combat::applyDamage(...),
    //        ProjectileSystem::spawn(...)
    // PERSISTENT: slot state into a file-scope pool/timer (see s_meteors) and let
    //   SkillSystem::updateMeteors / updateOrbProjectiles tick it.
}
```
Declaration in `skill_internal.h`:
```cpp
void fire<Name>(Vec3 origin, Vec3 direction, const SkillDef* def,
                const LevelGrid& grid, EntityPool& entities);
```

## 5. Dispatch + feedback — `src/game/skill_system.cpp`

`tryActivate` switch (~line 320):
```cpp
    case SkillId::<NAME>:
        fire<Name>(eyePos, forward, def, grid, entities);
        break;
```
Also add cases in `playActivationSound` (~line 164) and `spawnActivationParticles` (~line 241)
so activation has audio/VFX.

## 6. Per-tick (persistent skills only) — `src/game/skill_system.cpp`

Tick your file-scope state in `SkillSystem::update` (or `updateMeteors`/`updateOrbProjectiles`),
copying the `s_bombardmentTimer` / Frozen-Orb shard patterns.

## 7. Doc-sync

Add the skill to the `engine-reference` skill's skill list. Then do the icon (Step 4 of the
SKILL.md) and compile.

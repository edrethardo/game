# New affix type — code snippets

Five edits. Replace `<TYPE>` (UPPER_SNAKE enum), `<type>` (snake_case JSON string),
`<bonusField>` (camelCase), and `<value-or-pct>`. Example used below: `ATTACK_SPEED_PCT`.

## 1. Enum — `src/game/item.h` (~line 42, before `COUNT`)

```cpp
enum struct AffixType : u8 {
    // ... existing values, keep order ...
    ENERGY_FLAT,
    <TYPE>,          // e.g. ATTACK_SPEED_PCT — append; never reuse _REMOVED_ slots
    COUNT
};
```

## 2. String → enum — `src/game/item_loader.cpp` (`affixTypeFromString`, ~line 133)

```cpp
    if (s == "<type>" || s == "<TYPE>") return AffixType::<TYPE>;
```
(Insert before the final `return AffixType::DAMAGE_FLAT;` fallback.)

## 3. Bonus field — `src/game/item.h`

```cpp
// PlayerInventory (~line 388), with the other bonus* fields:
f32 bonus<BonusField> = 0.0f;     // e.g. bonusAttackSpeedPct

// NpcEquipment (~line 360) — ADD ONLY IF NPCs should also get this affix.
```

## 4. Accumulate + zero-init — `src/game/inventory.cpp`

**Shared (player + NPC)** — add a case in `accumulateCommonAffix()` (~line 29):
```cpp
        case AffixType::<TYPE>: e.bonus<BonusField> += affix.value; break;
```

**Player-only** — instead add to the player-only switch in `recalculateStats()` (~line 74):
```cpp
        case AffixType::<TYPE>: inv.bonus<BonusField> += affix.value; break;
```

Either way, zero-init at the top of `recalculateStats()` (~line 52), and in
`recalculateNpcStats()` too if you made it shared:
```cpp
    inv.bonus<BonusField> = 0.0f;
```

## 5. Consume the bonus (the step that makes it real)

Pick the site that matches the stat. Examples of existing consumption:
```cpp
// Weapon stats — buildWeaponDef() in src/game/inventory.cpp (~line 207):
wd.cooldown = def.baseCooldown * (1.0f - inv.bonusCooldownReduction);
wd.damage   = rawDamage * (1.0f + inv.bonusDamagePct / 100.0f);

// Max health — getEffectiveMaxHealth() (~line 269):
return (baseMaxHealth + totalHealthFlat) * (1.0f + inv.bonusHealthPct / 100.0f);

// Movement — engine_startgame.cpp (~line 165):
e.moveSpeed += equip.bonusMoveSpeed;

// Life-on-hit — engine_combat.cpp (~line 415): reads inv.bonusLifeOnHit on each hit.
```
Add the analogous line for `bonus<BonusField>`. Comment the *why* of the math. If it's a brand
new gameplay stat (e.g. resistance), apply it where that stat is used (e.g. incoming-damage
calc in `engine_combat.cpp`).

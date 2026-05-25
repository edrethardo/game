# New WeaponSubtype / firing behavior — code snippets (case B only)

Replace `<SUBTYPE>` (UPPER) / `<subtype>` (snake_case). Example: `LANCE` / `lance`.

## 1. Enum — `src/game/weapon.h` (`WeaponSubtype`, ~line 18, before `COUNT`)

```cpp
enum struct WeaponSubtype : u8 {
    NONE = 0,
    SWORD, DAGGER, AXE, CLAYMORE, CLEAVER,
    PISTOL, SMG, CARBINE, REVOLVER,
    BOW, CROSSBOW, THROWING_KNIFE, MOLOTOV, WAND,
    <SUBTYPE>,        // append before COUNT
    COUNT
};
```

## 2. String → enum — `src/game/item_loader.cpp` (`weaponSubtypeFromString`, ~line 38)

```cpp
    if (s == "<subtype>" || s == "<SUBTYPE>") return WeaponSubtype::<SUBTYPE>;
```

## 3. Crit profile — `src/game/inventory.cpp` (`buildWeaponDef`, ~line 244)

```cpp
    if (def.weaponSubtype == WeaponSubtype::DAGGER) {
        wd.critChance = 0.20f; wd.critMult = 2.5f;
    } else if (def.weaponSubtype == WeaponSubtype::<SUBTYPE>) {
        wd.critChance = 0.08f; wd.critMult = 2.2f;   // TODO: your subtype's crit feel
    } else {
        wd.critChance = 0.05f; wd.critMult = 2.0f;   // baseline
    }
```

## 4. Firing behavior (only if it fires differently)

The three weapon *types* (MELEE/HITSCAN/PROJECTILE) already have code paths; a new *subtype*
only needs firing code if its behavior differs from its type's default. If so:

- Branch on `def.weaponSubtype` inside `Combat::fireMelee` / `fireHitscan` / `fireProjectile`
  (`src/game/combat.cpp`), or
- Branch in the dispatch `Engine::handleWeaponFireForPlayer` (`src/engine/engine_combat.cpp`).

Keep crit handling inside the `fire*` functions (they roll crit and pass `isCrit` to
`Combat::applyDamage`) — don't add crit logic in `engine_combat`.

## 5. Doc-sync

Add the new subtype to the weapon notes in the `engine-reference` skill. Then build assets
(if you made a mesh/skin) and compile.

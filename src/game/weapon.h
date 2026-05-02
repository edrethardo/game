#pragma once

#include "core/types.h"

// --- Weapon classification ---
// WeaponType determines attack execution path in Combat namespace:
//   MELEE      -> cone AoE check via fireMelee()
//   HITSCAN    -> raycast trace via fireHitscan()
//   PROJECTILE -> spawns flying projectile via fireProjectile()
enum struct WeaponType : u8 {
    MELEE,
    HITSCAN,
    PROJECTILE,
};

// Weapon subtype determines visual identity and stat profile within each WeaponType.
// Each subtype maps to a distinct mesh + material for rendering.
enum struct WeaponSubtype : u8 {
    NONE = 0,
    // Melee subtypes
    SWORD, DAGGER, AXE,
    // Hitscan subtypes
    PISTOL, SMG, CARBINE, REVOLVER,
    // Projectile subtypes
    BOW, CROSSBOW, THROWING_KNIFE, MOLOTOV, WAND,
    COUNT
};

// Static weapon template. For items, the effective weapon is computed at runtime
// by Inventory::getEffectiveWeapon() which applies item affixes to base stats.
struct WeaponDef {
    const char* name;
    WeaponType  type;
    f32  damage;
    f32  range;
    f32  cooldown;        // seconds between attacks
    f32  coneAngleDeg;    // melee: swing arc in degrees (e.g., 70)
    f32  projectileSpeed; // only for PROJECTILE
    f32  projectileRadius;
    f32  recoilKick;      // pitch kick on fire (radians)
};

// Per-player mutable weapon state (cooldown tracking, recoil decay)
struct WeaponState {
    u8  currentWeapon = 0;
    f32 cooldownTimer = 0.0f;
    f32 recoilOffset  = 0.0f; // current recoil (decays per frame)
};

// First-person viewmodel animation state
struct ViewmodelState {
    f32 bobTimer    = 0.0f;  // increments while walking
    f32 swayYaw     = 0.0f;  // camera look sway (lerps toward 0)
    f32 swayPitch   = 0.0f;
    f32 recoilKick  = 0.0f;  // decays after fire
    f32 attackAnimT = 0.0f;  // melee swing countdown
    f32 fireShakeTimer = 0.0f; // ranged weapon vibration countdown
};

static constexpr u32 MAX_WEAPON_DEFS = 16;

// Predefined weapon table
inline void initWeaponTable(WeaponDef* defs, u32& count) {
    count = 0;

    // 0: Sword (melee)
    defs[count++] = {
        "Sword", WeaponType::MELEE,
        25.0f,   // damage
        2.5f,    // range
        0.4f,    // cooldown
        70.0f,   // cone angle degrees
        0.0f, 0.0f,
        0.01f    // slight camera punch
    };

    // 1: Pistol (hitscan)
    defs[count++] = {
        "Pistol", WeaponType::HITSCAN,
        15.0f,   // damage
        50.0f,   // range
        0.2f,    // cooldown
        0.0f,    // pinpoint
        0.0f, 0.0f,
        0.015f
    };

    // 2: Fireball (projectile)
    defs[count++] = {
        "Fireball", WeaponType::PROJECTILE,
        30.0f,   // damage
        0.0f,    // range unused (lifetime-based)
        0.6f,    // cooldown
        0.0f,
        15.0f,   // projectile speed
        0.15f,   // projectile radius
        0.02f
    };
}

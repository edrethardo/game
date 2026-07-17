// skill_sorcerer.cpp — fire* helpers for the Sorcerer class skills:
// Fireball (lobbed explosive projectile with splash damage).
// Called from SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

// Lobbed explosive projectile with splash damage on impact.
void fireFireball(Vec3 origin, Vec3 forward, const SkillDef* def,
                  ProjectilePool& pool)
{
    f32 speed       = def->projectileSpeed > 0.0f ? def->projectileSpeed : 18.0f;
    f32 damage      = spellScaled((def->damage > 0.0f ? def->damage : 35.0f));
    f32 splashR     = def->radius          > 0.0f ? def->radius          : 2.0f;
    f32 splashDmg   = damage * 0.6f;  // 60% damage in splash zone
    // PROJ_SPLASH + slight gravity arc for feel
    u16 slot = ProjectileSystem::spawn(pool, origin, forward, speed, damage,
                                       0.2f, 3.0f, true, PROJ_SPLASH);
    if (slot != 0xFFFF) {
        Projectile& p  = pool.projectiles[slot];
        p.splashRadius = splashR;
        p.splashDamage = splashDmg;
        p.gravity      = 4.0f;
        p.lightColor   = {1.0f, 0.4f, 0.1f}; // orange fire glow
    }
    LOG_INFO("Fireball launched");
}

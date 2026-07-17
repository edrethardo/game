// skill_paladin.cpp — fire* helpers for the Paladin class skills:
// HolySmite, HolyBombardment, HolyNova, DivineJudgment.
//
// Cross-boundary state (s_bombardmentTimer etc. and s_holyNovaTimer etc.) is
// defined in skill_system.cpp and declared extern in skill_internal.h.
// updateMeteors (skill_system.cpp) ticks the bombardment, and update() ticks
// the holy nova delayed second hit — so both sets of statics must be extern.

#include "game/skill_internal.h"

// Dash-smite: 3m forward dash, stops on first enemy, spawns gold judgment pillar.
// Heals 8 flat + 20% of damage dealt.
void fireHolySmite(Vec3 origin, Vec3 forward, const SkillDef* def,
                   EntityPool& entities, Player& player, const LevelGrid& grid)
{
    // Flatten to XZ plane for dash direction
    Vec3 dashDir = {forward.x, 0.0f, forward.z};
    if (lengthSq(dashDir) < 0.001f) dashDir = {0.0f, 0.0f, -1.0f};
    else dashDir = normalize(dashDir);

    f32 dashDist = def->distance > 0.0f ? def->distance : 3.0f;

    // Raycast for wall obstruction
    Vec3 rayOrigin = player.position + Vec3{0.0f, 0.5f, 0.0f};
    RayHit wallHit = Raycast::cast(grid, rayOrigin, dashDir, dashDist);
    f32 actualDist = wallHit.hit ? (wallHit.distance - 0.5f) : dashDist;
    if (actualDist < 0.5f) actualDist = 0.5f;

    Vec3 startPos = player.position;

    // Find first enemy in a narrow cone along the dash path
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, startPos + Vec3{0.0f, 0.5f, 0.0f}, dashDir,
        cosf(radians(15.0f)), actualDist, hits, dists, MAX_ENTITIES);

    f32 dashDmg = spellScaled(def->damage);
    f32 pillarDmg = spellScaled(15.0f);
    Vec3 pillarPos = startPos + dashDir * actualDist; // default: end of dash
    f32 totalDmg = 0.0f;

    // Stop on first hostile enemy
    for (u32 i = 0; i < hitCount; i++) {
        Entity* ent = handleGet(entities, hits[i]);
        if (!ent || (ent->flags & ENT_FRIENDLY)) continue;
        // Dash to this enemy's position
        actualDist = dists[i];
        pillarPos = ent->position;
        // Deal dash damage + stun
        Combat::applyDamage(entities, hits[i], dashDmg);
        ent->stunTimer = fmaxf(ent->stunTimer, 0.3f);
        totalDmg += dashDmg;
        break;
    }

    // Teleport to the dash endpoint — resolved, because actualDist on an enemy hit is the
    // distance to its CENTER (inside the body; inescapable inside anything Butcher-sized),
    // and the wall ray above is a thin center ray a footprint can still graze past. The
    // resolver backs off along the dash line to the nearest spot the paladin actually fits.
    Vec3 endPos = Teleport::resolveDest(grid, entities, startPos,
                                        startPos + dashDir * actualDist);
    player.position = endPos;

    // Spawn gold judgment pillar at target
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position    = pillarPos;
            s_meteors[i].damage      = pillarDmg;
            s_meteors[i].radius      = def->radius > 0.0f ? def->radius : 1.5f;
            s_meteors[i].timer       = 0.1f;
            s_meteors[i].active      = true;
            s_meteors[i].healsPlayer = true;
            s_meteors[i].caster      = s_castingPlayer;
            s_meteors[i].color       = {1.0f, 0.9f, 0.3f};
            totalDmg += pillarDmg; // anticipated pillar damage for heal calc
            break;
        }
    }

    // Flat heal + 20% of damage dealt
    // Deliberately NOT spellScaled: the gear "+spell damage" FLAT must not inflate a heal
    // (the gear % rides s_classDmgMult, which has always scaled damage and heals together).
    f32 heal = 8.0f * s_classDmgMult + totalDmg * 0.2f;
    player.health = fminf(player.health + heal, player.maxHealth);

    // Gold dash trail visual
    if (s_dashCallback) s_dashCallback(startPos, endPos);
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Holy Smite: dashed %.1fm, dealt %.0f dmg, healed %.0f", actualDist, totalDmg, heal);
}

// Holy Bombardment: places a 6m judgment zone. For 4s, gold pillars rain on enemies.
// Each pillar hit heals 3% max HP. Ground scorch zone for residual DPS.
void fireHolyBombardment(Vec3 origin, const SkillDef* def, Player& player)
{
    // Per-caster slot (N3) — `s_castingPlayer` is the canonical net-slot index.
    const u8 cp = s_castingPlayer;
    s_bombardmentRadius[cp] = def->radius > 0.0f ? def->radius : 6.0f;
    s_bombardmentDamage[cp] = spellScaled((def->damage > 0.0f ? def->damage : 18.0f));
    s_bombardmentCenter[cp] = origin;
    s_bombardmentTimer [cp] = def->duration > 0.0f ? def->duration : 4.0f;
    s_bombardmentAccum [cp] = 0.0f;

    // Scorch zone for residual ground DPS
    f32 scorchDps = spellScaled(4.0f);
    if (s_scorchCallback) s_scorchCallback(origin, s_bombardmentRadius[cp], 4.0f, scorchDps);

    // Gold nova ring on activation
    if (s_novaCallback) s_novaCallback(origin, s_bombardmentRadius[cp], {1.0f, 0.9f, 0.3f});
    if (s_screenShake) s_screenShake->trigger(0.05f, 0.3f);
    LOG_INFO("Holy Bombardment: judgment zone active for 4s");
}

// Holy Nova: instant 360° dual-hit AoE. Ring wave damages + heals instantly,
// particle wave deals second hit after 0.3s. Both heal allies (NPCs + players).
void fireHolyNova(Vec3 origin, const SkillDef* def,
                  EntityPool& entities, Player& player)
{
    f32 radius = def->radius > 0.0f ? def->radius : 5.0f;
    f32 ringDmg = spellScaled(def->damage);
    f32 healPct = def->allyHealPct > 0.0f ? def->allyHealPct : 0.08f;

    // Ring wave (instant) — damage enemies, heal allies
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    u32 enemiesHit = 0;
    u32 alliesHealed = 0;
    for (u32 i = 0; i < hitCount; i++) {
        Entity* ent = handleGet(entities, hits[i]);
        if (!ent) continue;
        if (ent->flags & ENT_FRIENDLY) {
            // Heal friendly NPCs (turrets, summons)
            ent->health = fminf(ent->health + ent->maxHealth * healPct, ent->maxHealth);
            alliesHealed++;
        } else {
            Combat::applyDamage(entities, hits[i], ringDmg);
            enemiesHit++;
        }
    }

    // Expanding gold ring nova (this IS the ring damage visual)
    if (s_novaCallback) s_novaCallback(origin, radius, {1.0f, 0.85f, 0.3f});

    // 144 golden particles in a sphere burst (3 rings)
    if (s_particlePool) {
        // Ring 1: 64 horizontal
        for (u32 p = 0; p < 64; p++) {
            f32 angle = (6.2832f / 64.0f) * p;
            f32 speed = 8.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 4.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed, 0.2f, sinf(angle) * speed};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.04f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.04f;
            tp.baseAlpha = 0.9f;
            tp.r = 255; tp.g = 200; tp.b = 50;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Ring 2: 48 upward-angled
        for (u32 p = 0; p < 48; p++) {
            f32 angle = (6.2832f / 48.0f) * p;
            f32 speed = 9.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 3.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed * 0.7f, speed * 0.5f, sinf(angle) * speed * 0.7f};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.05f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.03f;
            tp.baseAlpha = 0.85f;
            tp.r = 255; tp.g = 210; tp.b = 40;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Ring 3: 32 downward-angled
        for (u32 p = 0; p < 32; p++) {
            f32 angle = (6.2832f / 32.0f) * p;
            f32 speed = 10.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 2.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed * 0.8f, -speed * 0.3f, sinf(angle) * speed * 0.8f};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.04f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.04f;
            tp.baseAlpha = 0.8f;
            tp.r = 255; tp.g = 180; tp.b = 30;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Upward sparks
        ParticleSystem::spawnSparks(*s_particlePool, origin, {0.0f, 1.0f, 0.0f}, 8);
    }

    // Set up delayed second hit (particle wave arrives 0.3s later). Per-caster (N3).
    const u8 cp = s_castingPlayer;
    s_holyNovaTimer  [cp] = 0.3f;
    s_holyNovaCenter [cp] = origin;
    s_holyNovaDamage2[cp] = spellScaled((def->secondaryDamage > 0.0f ? def->secondaryDamage : 20.0f));
    s_holyNovaRadius [cp] = radius;
    (void)healPct;        // ring heal already applied above; second-hit healPct field was dead

    if (s_screenShake) s_screenShake->trigger(0.08f, 0.4f);
    LOG_INFO("Holy Nova: ring hit %u enemies, healed %u allies", enemiesHit, alliesHealed);
}

// Divine Judgment: cleanse + invuln + AoE stun + 3 massive judgment pillars on nearest enemies.
// Heals 10% max HP on activation + 15% per kill from pillars.
void fireDivineJudgment(Player& player, EntityPool& entities, const SkillDef* def)
{
    // Phase 1: Cleanse all debuffs
    player.slowTimer   = 0.0f;
    player.poisonTimer = 0.0f;
    player.poisonDps   = 0.0f;
    player.burnTimer   = 0.0f;
    player.burnDps     = 0.0f;
    player.freezeTimer = 0.0f;

    // 10% max HP heal + invulnerability
    player.health = fminf(player.health + player.maxHealth * 0.10f, player.maxHealth);
    player.invulnTimer = 1.5f;

    // AoE stun — 1.5s on nearby hostiles
    f32 stunRadius = 5.0f;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        Vec3 diff = e.position - player.position;
        f32 dist2 = diff.x*diff.x + diff.z*diff.z;
        if (dist2 < stunRadius * stunRadius) {
            e.stunTimer = 1.5f;
        }
    }

    // Massive gold nova visual
    if (s_novaCallback) s_novaCallback(player.position, stunRadius, {1.0f, 0.95f, 0.4f});
    if (s_particlePool) ParticleSystem::spawnExplosion(*s_particlePool, player.position, 3.0f);

    // Phase 2: 3 judgment pillars on nearest enemies (10m range)
    EntityHandle nearest[MAX_ENTITIES];
    f32          nearDists[MAX_ENTITIES];
    u32 found = CombatQuery::queryConeSorted(
        entities, player.position, {0.0f, 0.0f, -1.0f}, -1.0f, 10.0f,
        nearest, nearDists, MAX_ENTITIES);

    // Filter to hostiles only, pick up to 3
    f32 pillarDmg = spellScaled((def->damage > 0.0f ? def->damage : 60.0f));
    f32 pillarRadius = def->radius > 0.0f ? def->radius : 2.5f;
    u8 pillarsSpawned = 0;
    Vec3 pillarPositions[4]; // origin + up to 3 targets for chain visual
    pillarPositions[0] = player.position;

    for (u32 i = 0; i < found && pillarsSpawned < 3; i++) {
        Entity* ent = handleGet(entities, nearest[i]);
        if (!ent || (ent->flags & ENT_FRIENDLY)) continue;

        for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
            if (!s_meteors[m].active) {
                s_meteors[m].position    = ent->position;
                s_meteors[m].damage      = pillarDmg;
                s_meteors[m].radius      = pillarRadius;
                s_meteors[m].timer       = 0.15f + pillarsSpawned * 0.15f;
                s_meteors[m].active      = true;
                s_meteors[m].healsPlayer = true; // kill-heal checked in updateMeteors
                s_meteors[m].caster      = s_castingPlayer;
                s_meteors[m].color       = {1.0f, 0.95f, 0.4f};
                pillarPositions[pillarsSpawned + 1] = ent->position;
                pillarsSpawned++;
                break;
            }
        }
    }

    // Chain visual connecting pillar positions (divine triangle seal)
    if (s_chainCallback && pillarsSpawned > 0) {
        s_chainCallback(pillarPositions, pillarsSpawned + 1);
    }

    if (s_screenShake) s_screenShake->trigger(0.15f, 0.7f);
    LOG_INFO("Divine Judgment: invuln 1.5s, stunned nearby, %u pillars spawned", pillarsSpawned);
}

// skill_system.cpp — Public SkillSystem API: init, update, tryActivate,
// updateOrbProjectiles, updateMeteors, setters, overcharge state, and all
// cross-boundary module-scope state (defined here, extern'd in skill_internal.h).
//
// The actual fire* helpers are defined in the per-class family .cpp files:
//   skill_legendary.cpp, skill_warrior.cpp, skill_ranger.cpp, skill_sorcerer.cpp,
//   skill_rogue.cpp, skill_paladin.cpp, skill_engineer.cpp, skill_marksman.cpp,
//   skill_tinkerer.cpp.
// All of them are reached from tryActivate's dispatch switch below via the
// declarations in skill_internal.h.

#include "game/skill_internal.h"

// ---------------------------------------------------------------------------
// Cross-boundary state definitions (one definition; extern'd elsewhere)
// ---------------------------------------------------------------------------

// Not static — extern'd by engine.cpp for rendering the targeting circles
PendingMeteor s_meteors[MAX_PENDING_METEORS];

ParticlePool* s_particlePool = nullptr;

// Skill power scaling (0.0 = base, 1.0 = max). Set by engine before tryActivate.
// Class skills get 0.0; legendary item skills get a value scaled by item level.
f32 s_skillPower = 0.0f;

// Class skill damage multiplier — scales damage/heal numbers by effective floor.
// Set to floor-based mult for class skills, 1.0 for item skills.
f32 s_classDmgMult = 1.0f;

// Equipped weapon base damage — set by engine before marksman/ranger skill activation.
f32 s_weaponDamage = 10.0f;

// Arrow mesh ID for Volley (bolt reuses s_boltMeshId set by setBoltMeshId)
u8 s_arrowMeshId = 0;

ScreenShake*  s_screenShake  = nullptr;
SkillSystem::NovaCallback       s_novaCallback       = nullptr;
SkillSystem::DashCallback       s_dashCallback       = nullptr;
SkillSystem::ScorchCallback     s_scorchCallback     = nullptr;
SkillSystem::DroneSpawnCallback s_droneSpawnCallback = nullptr;
SkillSystem::ChainCallback      s_chainCallback      = nullptr;
SkillSystem::BeamCallback       s_beamCallback       = nullptr;
SkillSystem::ReloadCallback     s_reloadCallback     = nullptr;
u8 s_boltMeshId     = 0;    // set by engine during init for shock bolt projectiles
u8 s_shockBoltMatId = 0;

// Overcharged Magazine state (Marksman buff)
f32 s_overchargeTimer = 0.0f;
u8  s_overchargeShots = 0;

// Holy Bombardment persistent state — set by fireHolyBombardment, ticked by updateMeteors
f32  s_bombardmentTimer  = 0.0f;
Vec3 s_bombardmentCenter = {0,0,0};
f32  s_bombardmentAccum  = 0.0f;
f32  s_bombardmentDamage = 0.0f;
f32  s_bombardmentRadius = 0.0f;

// Holy Nova delayed second hit state — set by fireHolyNova, ticked by update()
f32  s_holyNovaTimer   = 0.0f;
Vec3 s_holyNovaCenter  = {0,0,0};
f32  s_holyNovaDamage2 = 0.0f;
f32  s_holyNovaRadius  = 0.0f;
f32  s_holyNovaHealPct = 0.0f;

// ---------------------------------------------------------------------------
// Setters (called by engine during init and before each activation)
// ---------------------------------------------------------------------------

void SkillSystem::setSkillPower(f32 power)       { s_skillPower    = power; }
void SkillSystem::setClassDamageMult(f32 mult)   { s_classDmgMult  = mult;  }
void SkillSystem::setWeaponDamage(f32 dmg)       { s_weaponDamage  = dmg;   }
void SkillSystem::setArrowMeshIds(u8 arrow, u8 /*bolt*/) { s_arrowMeshId = arrow; }
void SkillSystem::setNovaCallback(NovaCallback cb)           { s_novaCallback       = cb; }
void SkillSystem::setDashCallback(DashCallback cb)           { s_dashCallback       = cb; }
void SkillSystem::setScorchCallback(ScorchCallback cb)       { s_scorchCallback     = cb; }
void SkillSystem::setDroneSpawnCallback(DroneSpawnCallback cb) { s_droneSpawnCallback = cb; }
void SkillSystem::setChainCallback(ChainCallback cb)         { s_chainCallback      = cb; }
void SkillSystem::setBeamCallback(BeamCallback cb)           { s_beamCallback       = cb; }
void SkillSystem::setReloadCallback(ReloadCallback cb)       { s_reloadCallback     = cb; }
void SkillSystem::setBoltMeshId(u8 meshId, u8 matId)         { s_boltMeshId = meshId; s_shockBoltMatId = matId; }
void SkillSystem::setFXTargets(ParticlePool* particles, ScreenShake* shake) {
    s_particlePool = particles;
    s_screenShake  = shake;
}

// ---------------------------------------------------------------------------
// Overcharge state (Marksman buff)
// ---------------------------------------------------------------------------

bool SkillSystem::isOvercharged() { return s_overchargeTimer > 0.0f && s_overchargeShots > 0; }
void SkillSystem::consumeOverchargeShot() {
    if (s_overchargeShots > 0) s_overchargeShots--;
    if (s_overchargeShots == 0) s_overchargeTimer = 0.0f;
}
void SkillSystem::tickOvercharge(f32 dt) {
    if (s_overchargeTimer > 0.0f) {
        s_overchargeTimer -= dt;
        if (s_overchargeTimer <= 0.0f) { s_overchargeTimer = 0.0f; s_overchargeShots = 0; }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SkillSystem::init() {
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        s_meteors[i] = {};
    }
}

const SkillDef* SkillSystem::findSkillDef(const SkillDef* defs, u32 count, SkillId id) {
    for (u32 i = 0; i < count; i++) {
        if (defs[i].id == id) return &defs[i];
    }
    return nullptr;
}

void SkillSystem::update(SkillState& ss, f32 dt) {
    // Energy regen: 10/sec
    ss.energy += 10.0f * dt;
    if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;

    // Cooldown tick
    if (ss.cooldownTimer > 0.0f) {
        ss.cooldownTimer -= dt;
        if (ss.cooldownTimer < 0.0f) ss.cooldownTimer = 0.0f;
    }

    // Holy Nova delayed second hit (particle wave) — ticked here since no entity access needed
    if (s_holyNovaTimer > 0.0f) {
        s_holyNovaTimer -= dt;
        if (s_holyNovaTimer <= 0.0f) {
            s_holyNovaTimer = 0.0f;
            // Spawn a PendingMeteor for the delayed AoE (resolved in updateMeteors)
            for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                if (!s_meteors[m].active) {
                    s_meteors[m].position    = s_holyNovaCenter;
                    s_meteors[m].damage      = s_holyNovaDamage2;
                    s_meteors[m].radius      = s_holyNovaRadius;
                    s_meteors[m].timer       = 0.001f; // triggers next frame
                    s_meteors[m].active      = true;
                    s_meteors[m].healsPlayer = true;
                    s_meteors[m].color       = {1.0f, 0.85f, 0.3f};
                    break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// File-scope helpers extracted from tryActivate — called in the same order,
// verbatim logic. Static so they don't leak into the header.
// ---------------------------------------------------------------------------

// Play the themed activation sound for a skill.
// Only reads the SkillId — no other state needed.
static void playActivationSound(SkillId id)
{
    switch (id) {
    case SkillId::FIREBALL:
    case SkillId::CONSECRATION:
    case SkillId::HOLY_BOMBARDMENT:
        AudioSystem::play(SfxId::SKILL_FIRE); break;
    case SkillId::FROZEN_ORB:
        AudioSystem::play(SfxId::SKILL_ICE); break;
    case SkillId::CHAIN_LIGHTNING:
    case SkillId::SHOCK_BOLT:
    case SkillId::TESLA_COIL:
        AudioSystem::play(SfxId::SKILL_LIGHTNING); break;
    case SkillId::BLOOD_NOVA:
    case SkillId::POISON_CLOUD:
    case SkillId::POISON_ARROW:
        AudioSystem::play(SfxId::SKILL_BLOOD); break;
    case SkillId::PHASE_DASH:
    case SkillId::SHADOW_STRIKE:
    case SkillId::SHADOW_STEP:
    case SkillId::SHADOW_SHOT:
        AudioSystem::play(SfxId::SKILL_DASH); break;
    case SkillId::FAN_OF_KNIVES:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::SHADOW_DANCE:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    case SkillId::HOLY_SMITE:
        AudioSystem::play(SfxId::SKILL_STUN); break;   // thunderous divine impact
    case SkillId::DIVINE_SHIELD:
    case SkillId::DIVINE_JUDGMENT:
        AudioSystem::play(SfxId::SKILL_HEAL); break;
    case SkillId::HOLY_NOVA:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::WAR_CRY:
    case SkillId::MECH_OVERDRIVE:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    case SkillId::DEPLOY_TURRET:
    case SkillId::COMBAT_DRONE:
    case SkillId::SWARM_DRONES:
    case SkillId::SWARM_DEPLOY:
    case SkillId::SWARM_QUEEN:
        AudioSystem::play(SfxId::SKILL_SUMMON); break;
    case SkillId::OVERCLOCK:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    case SkillId::DETONATE_SWARM:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::METEOR_STRIKE:
    case SkillId::EXPLOSIVE_ROUND:
    case SkillId::EARTHQUAKE:
    case SkillId::STUN_GRENADE:
    case SkillId::RAIN_OF_ARROWS:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::THUNDERCLAP:
    case SkillId::CLEAVE:
    case SkillId::WHIRLWIND:
        AudioSystem::play(SfxId::SKILL_STUN); break;
    case SkillId::MULTI_SHOT:
    case SkillId::AIMED_SHOT:
    case SkillId::RAPID_FIRE:
    case SkillId::HEADSHOT:
    case SkillId::KNIFE_BURST:
    case SkillId::BARRAGE:
    case SkillId::PIERCING_SHOT:
        AudioSystem::play(SfxId::WEAPON_BOW, 0.5f); break;
    case SkillId::VOLLEY:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::MARK_PREY:
        AudioSystem::play(SfxId::SKILL_BLOOD); break;
    case SkillId::OVERCHARGED_MAGAZINE:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    default: break;
    }
}

// Spawn the cast-point particle burst for a skill.
// Uses s_particlePool and s_screenShake (file-scope); eyePos/forward define
// the muzzle position for directional effects.
static void spawnActivationParticles(SkillId id, Vec3 eyePos, Vec3 forward)
{
    if (!s_particlePool) return;

    Vec3 castPos = eyePos + forward * 0.5f;
    switch (id) {
    case SkillId::FIREBALL:     // trails handled by projectile system
    case SkillId::SHOCK_BOLT:   // trails handled by projectile system
    case SkillId::FROZEN_ORB:   // trails handled by projectile system
        break;
    case SkillId::CONSECRATION:
    case SkillId::HOLY_BOMBARDMENT:
        ParticleSystem::spawnMagicBurst(*s_particlePool, castPos, 255, 210, 50, 12);
        break;
    case SkillId::HOLY_NOVA:
    case SkillId::DIVINE_JUDGMENT:
        break; // particles spawned inside the fire functions
    case SkillId::CHAIN_LIGHTNING:
    case SkillId::TESLA_COIL:
        ParticleSystem::spawnSparks(*s_particlePool, castPos, forward, 6);
        break;
    case SkillId::BLOOD_NOVA:
        break;
    case SkillId::PHASE_DASH:
    case SkillId::SHADOW_STRIKE:
    case SkillId::SHADOW_STEP:
        ParticleSystem::spawnSmoke(*s_particlePool, eyePos, 8);
        break;
    case SkillId::FAN_OF_KNIVES:
    case SkillId::SHADOW_DANCE:
        break; // VFX handled inside fire functions
    case SkillId::EARTHQUAKE:
        ParticleSystem::spawnExplosion(*s_particlePool, eyePos + forward * 2.0f, 2.0f);
        if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
        break;
    case SkillId::EXPLOSIVE_ROUND:
    case SkillId::AIMED_SHOT:
    case SkillId::HEADSHOT:
    case SkillId::OVERCHARGED_MAGAZINE:
        break; // VFX handled inside fire functions
    case SkillId::METEOR_STRIKE:
        break; // no cast-time VFX — explosion + smoke spawn on impact in updateMeteors
    case SkillId::THUNDERCLAP:
        ParticleSystem::spawnSparks(*s_particlePool, eyePos, {0.0f, 1.0f, 0.0f}, 10);
        if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------

bool SkillSystem::tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                               Vec3 eyePos, Vec3 forward, f32 /*yaw*/,
                               ProjectilePool& projectiles, EntityPool& entities,
                               const LevelGrid& grid, Player& player,
                               f32 cooldownReduction)
{
    if (ss.activeSkill == SkillId::NONE)  return false;
    if (ss.cooldownTimer > 0.0f)          return false;

    const SkillDef* def = findSkillDef(skillDefs, skillDefCount, ss.activeSkill);
    if (!def) return false;

    // Energy / resource check
    if (ss.activeSkill == SkillId::BLOOD_NOVA) {
        f32 cost = player.health * def->healthCostPct;
        if (player.health <= cost + 1.0f) return false; // refuse to suicide
        player.health         -= cost;
        player.damageFlashTimer = 0.1f;
    } else {
        if (ss.energy < def->energyCost) return false;
        ss.energy -= def->energyCost;
    }

    ss.cooldownTimer = def->cooldown * (1.0f - cooldownReduction);
    if (ss.cooldownTimer < 0.05f) ss.cooldownTimer = 0.05f; // hard minimum

    // Play skill activation sound — mapped by theme so each skill has audio feedback
    playActivationSound(ss.activeSkill);

    // Particle burst on activation — spawned at muzzle/cast point for immediate feedback
    spawnActivationParticles(ss.activeSkill, eyePos, forward);

    switch (ss.activeSkill) {
    // ---- Legacy legendary skills ----
    case SkillId::FROZEN_ORB:
        fireFrozenOrb(eyePos, forward, def, projectiles);
        break;
    case SkillId::CHAIN_LIGHTNING:
        fireChainLightning(eyePos, forward, def, grid, entities);
        break;
    case SkillId::BLOOD_NOVA:
        fireBloodNova(eyePos, def, entities);
        break;
    case SkillId::METEOR_STRIKE:
        fireMeteorStrike(eyePos, forward, def, grid);
        break;
    case SkillId::PHASE_DASH:
        firePhaseDash(eyePos, forward, def, grid, entities, player);
        break;

    // ---- Warrior ----
    case SkillId::CLEAVE:
        fireCleave(eyePos, forward, def, entities, player);
        break;
    case SkillId::THUNDERCLAP:
        fireThunderclap(eyePos, def, entities);
        break;
    case SkillId::WAR_CRY:
        fireWarCry(eyePos, def, entities, player);
        break;
    case SkillId::WHIRLWIND:
        fireWhirlwind(eyePos, def, entities);
        break;
    case SkillId::EARTHQUAKE:
        fireEarthquake(eyePos, def, entities);
        break;

    // ---- Ranger ----
    case SkillId::MULTI_SHOT:
        fireMultiShot(eyePos, forward, def, projectiles);
        break;
    case SkillId::RAIN_OF_ARROWS:
        fireRainOfArrows(eyePos, forward, def, grid, entities);
        break;
    case SkillId::POISON_ARROW:
        firePoisonArrow(eyePos, forward, def, projectiles);
        break;
    case SkillId::SHADOW_SHOT:
        fireShadowShot(eyePos, forward, def, grid, entities);
        break;
    case SkillId::VOLLEY:
        fireVolley(eyePos, forward, def, grid, projectiles);
        break;
    case SkillId::PIERCING_SHOT:
        firePiercingShot(eyePos, forward, def, grid, entities);
        break;
    case SkillId::BARRAGE:
        fireBarrage(eyePos, forward, def, projectiles);
        break;
    case SkillId::MARK_PREY:
        fireMarkPrey(eyePos, forward, def, entities);
        break;

    // ---- Sorcerer ----
    case SkillId::FIREBALL:
        fireFireball(eyePos, forward, def, projectiles);
        break;

    // ---- Rogue ----
    case SkillId::KNIFE_BURST:
        fireKnifeBurst(eyePos, forward, def, projectiles);
        break;
    case SkillId::FAN_OF_KNIVES:
        fireFanOfKnives(eyePos, forward, def, projectiles, player);
        break;
    case SkillId::POISON_CLOUD:
        firePoisonCloud(eyePos, forward, def, grid, entities, player);
        break;
    case SkillId::SHADOW_STRIKE:
        fireShadowStrike(eyePos, forward, def, entities, player);
        break;
    case SkillId::SHADOW_STEP:
        fireShadowStep(eyePos, forward, def, grid, entities, player);
        break;
    case SkillId::SHADOW_DANCE:
        fireShadowDance(player);
        break;

    // ---- Paladin ----
    case SkillId::HOLY_SMITE:
        fireHolySmite(eyePos, forward, def, entities, player, grid);
        break;
    case SkillId::CONSECRATION:
    case SkillId::HOLY_BOMBARDMENT:
        fireHolyBombardment(eyePos, def, player);
        break;
    case SkillId::HOLY_NOVA:
        fireHolyNova(eyePos, def, entities, player);
        break;
    case SkillId::DIVINE_SHIELD:
    case SkillId::DIVINE_JUDGMENT:
        fireDivineJudgment(player, entities, def);
        break;

    // ---- Combat Engineer ----
    case SkillId::SHOCK_BOLT:
        fireShockBolt(eyePos, forward, def, projectiles);
        break;
    case SkillId::DEPLOY_TURRET:
        fireDeployTurret(eyePos, forward, entities);
        break;
    case SkillId::TESLA_COIL:
        fireTeslaCoil(eyePos, def, entities, projectiles);
        break;
    case SkillId::MECH_OVERDRIVE:
        fireMechOverdrive(player);
        break;

    // ---- Marksman ----
    case SkillId::AIMED_SHOT:
        fireAimedShot(eyePos, forward, def, grid, entities);
        break;
    case SkillId::EXPLOSIVE_ROUND:
        fireExplosiveRound(eyePos, forward, def, grid, entities);
        break;
    case SkillId::RAPID_FIRE:
    case SkillId::OVERCHARGED_MAGAZINE:
        fireOverchargedMagazine(eyePos, forward);
        break;
    case SkillId::HEADSHOT:
        fireHeadshot(eyePos, forward, def, grid, entities, ss);
        break;

    // ---- Tinkerer ----
    case SkillId::COMBAT_DRONE:
        fireCombatDrone(eyePos, forward, entities);
        break;
    case SkillId::SWARM_DRONES:
        fireSwarmDrones(eyePos, forward, entities);
        break;
    case SkillId::STUN_GRENADE:
        fireStunGrenade(eyePos, forward, def, projectiles);
        break;
    case SkillId::SWARM_DEPLOY:
        fireSwarmDeploy(eyePos, forward, entities);
        break;
    case SkillId::OVERCLOCK:
        fireOverclock(eyePos, def, entities);
        break;
    case SkillId::DETONATE_SWARM:
        fireDetonateSwarm(def, entities);
        break;
    case SkillId::SWARM_QUEEN:
        fireSwarmQueen(eyePos, forward);
        break;

    // ---- Wanderer ----
    case SkillId::DEFLECT: {
        // Open the absorb window — all hits are accumulated, then burst-released
        // as 8 projectiles per absorbed hit carrying the total damage
        player.deflectTimer = def->activeWindow; // 0.4s
        player.deflectAbsorbed = 0.0f;
        player.deflectHitCount = 0;
        break;
    }
    case SkillId::EXPLOIT_WEAKNESS: {
        // AoE mark: raycast to find aim point, then mark all enemies within 5m radius
        // First find where the player is aiming (first entity or wall hit)
        EntityHandle aimHits[1];
        f32 aimDists[1];
        u32 aimCount = CombatQuery::queryConeSorted(
            entities, eyePos, forward, cosf(radians(5.0f)), 30.0f,
            aimHits, aimDists, 1);
        Vec3 markCenter;
        if (aimCount > 0) {
            Entity* aimTarget = handleGet(entities, aimHits[0]);
            if (aimTarget) markCenter = aimTarget->position;
            else markCenter = eyePos + forward * 10.0f;
        } else {
            // No entity hit — place mark at 10m ahead on the ground
            markCenter = eyePos + forward * 10.0f;
        }
        // Mark all enemies within 5m of the aim point
        u32 marked = 0;
        for (u32 a = 0; a < entities.activeCount; a++) {
            u32 idx = entities.activeList[a];
            Entity& e = entities.entities[idx];
            if (e.flags & ENT_DEAD) continue;
            if (e.flags & ENT_FRIENDLY) continue;
            if (e.enemyType == EnemyType::PROP) continue;
            f32 d = length(e.position - markCenter);
            if (d <= 5.0f) {
                e.markPreyDmgMult = def->damageMultiplier; // 1.6
                e.markPreyTimer = def->markDuration;       // 5s
                // Per-enemy visual: orange sparks burst on mark
                if (s_particlePool) {
                    ParticleSystem::spawnMagicBurst(*s_particlePool, e.position + Vec3{0, e.halfExtents.y, 0}, 255, 140, 0, 8);
                }
                marked++;
            }
        }
        player.markTimer = def->markDuration;
        player.markedEntityIdx = 0xFFFF;
        if (marked == 0) {
            return false; // nothing to mark — don't consume cooldown
        }
        // Cast visual: orange nova ring at the mark center
        if (s_novaCallback) {
            s_novaCallback(markCenter, 5.0f, Vec3{1.0f, 0.6f, 0.0f});
        }
        break;
    }
    case SkillId::DEATHS_DANCE: {
        // Activate ultimate — AoE slash on each dodge-through is handled separately
        player.deathsDanceTimer = def->duration;
        break;
    }
    case SkillId::ADRENALINE_SURGE:
        // Passive — no activation needed; stacks are awarded by the dodge-through callback
        break;

    default:
        return false;
    }

    return true;
}

void SkillSystem::updateOrbProjectiles(ProjectilePool& pool,
                                        const SkillDef* skillDefs, u32 skillDefCount,
                                        f32 dt)
{
    const SkillDef* def = findSkillDef(skillDefs, skillDefCount, SkillId::FROZEN_ORB);
    if (!def) return;

    u32 seen = 0;
    for (u32 i = 0; i < MAX_PROJECTILES && seen < pool.activeCount; i++) {
        Projectile& p = pool.projectiles[i];
        if (!p.active) continue;
        seen++;
        if (!(p.projFlags & 1)) continue; // not an orb

        p.subTimer += dt;

        while (p.subTimer >= def->shardInterval) {
            p.subTimer -= def->shardInterval;

            // Spawn shards in random spherical directions (2x count for dense burst)
            u8 burstCount = def->shardCount * 2;
            for (u8 s = 0; s < burstCount; s++) {
                // Random phi (azimuth) and theta (elevation) for full sphere coverage
                f32 phi   = (rand() / static_cast<f32>(RAND_MAX)) * 6.28318f;
                f32 theta = (rand() / static_cast<f32>(RAND_MAX)) * 3.14159f - 1.5708f; // -90 to +90 deg
                Vec3 shardDir = {cosf(theta) * cosf(phi), sinf(theta), cosf(theta) * sinf(phi)};

                // Shard damage scales: classDmgMult (stored on orb) + skillPower (item level)
                f32 orbClassMult = p.damage > 0.0f ? p.damage : 1.0f; // stored at spawn
                f32 scaledShardDmg = def->shardDamage * orbClassMult * (1.0f + s_skillPower);
                u16 shardIdx = ProjectileSystem::spawn(pool, p.position, shardDir, def->shardSpeed,
                                        scaledShardDmg, def->shardRadius, 0.6f, true);

                // Mark the freshly spawned shard (bit 1 = isOrbShard)
                if (shardIdx != 0xFFFF) {
                    pool.projectiles[shardIdx].projFlags = PROJ_ORB_SHARD;
                    pool.projectiles[shardIdx].lightColor = {0.2f, 0.5f, 0.8f}; // light blue
                }
            }

            // Rotate the spoke angle for the spirograph effect
            p.orbAngle += radians(def->angleStepDeg);
        }
    }
}

void SkillSystem::updateMeteors(EntityPool& entities, Player& player, f32 dt) {
    // Holy Bombardment tick — needs entity access for smart targeting
    if (s_bombardmentTimer > 0.0f) {
        s_bombardmentTimer -= dt;
        s_bombardmentAccum += dt;
        while (s_bombardmentAccum >= 0.4f) {
            s_bombardmentAccum -= 0.4f;

            // Target nearest hostile enemy in zone
            Vec3 pillarPos = s_bombardmentCenter;
            f32 bestDist2 = s_bombardmentRadius * s_bombardmentRadius;
            bool foundTarget = false;
            for (u32 a = 0; a < entities.activeCount; a++) {
                u32 i = entities.activeList[a];
                Entity& e = entities.entities[i];
                if (e.flags & ENT_DEAD) continue;
                if (e.flags & ENT_FRIENDLY) continue;
                Vec3 diff = e.position - s_bombardmentCenter;
                f32 d2 = diff.x*diff.x + diff.z*diff.z;
                if (d2 < bestDist2) {
                    // Pick a random enemy (cycle via rand to avoid always targeting the same one)
                    if (!foundTarget || (std::rand() % 3 == 0)) {
                        pillarPos = e.position;
                        foundTarget = true;
                    }
                }
            }
            if (!foundTarget) {
                // No enemies — random position in zone for area denial visual
                f32 angle = (std::rand() / static_cast<f32>(RAND_MAX)) * 6.2832f;
                f32 dist  = (std::rand() / static_cast<f32>(RAND_MAX)) * s_bombardmentRadius * 0.8f;
                pillarPos.x += cosf(angle) * dist;
                pillarPos.z += sinf(angle) * dist;
            }

            for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                if (!s_meteors[m].active) {
                    s_meteors[m].position    = pillarPos;
                    s_meteors[m].damage      = s_bombardmentDamage;
                    s_meteors[m].radius      = 1.5f;
                    s_meteors[m].timer       = 0.3f;
                    s_meteors[m].active      = true;
                    s_meteors[m].healsPlayer = true;
                    s_meteors[m].color       = {1.0f, 0.9f, 0.3f};
                    break;
                }
            }
            if (s_screenShake) s_screenShake->trigger(0.03f, 0.15f);
        }
        if (s_bombardmentTimer <= 0.0f) s_bombardmentTimer = 0.0f;
    }

    // Process pending meteors (fire + holy pillar impacts)
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        PendingMeteor& m = s_meteors[i];
        if (!m.active) continue;

        m.timer -= dt;
        if (m.timer <= 0.0f) {
            // Explode — damage enemies, heal allies within blast radius
            EntityHandle hits[MAX_ENTITIES];
            f32          dists[MAX_ENTITIES];
            u32 hitCount = CombatQuery::queryConeSorted(
                entities, m.position, {0.0f, -1.0f, 0.0f}, -1.0f, m.radius,
                hits, dists, MAX_ENTITIES);

            u32 enemiesHit = 0;
            for (u32 j = 0; j < hitCount; j++) {
                Entity* ent = handleGet(entities, hits[j]);
                if (!ent) continue;
                if (m.healsPlayer && (ent->flags & ENT_FRIENDLY)) {
                    // Holy pillar heals allies
                    ent->health = fminf(ent->health + ent->maxHealth * 0.08f, ent->maxHealth);
                } else {
                    // Check if kill for Divine Judgment bonus
                    f32 hpBefore = ent->health;
                    Combat::applyDamage(entities, hits[j], m.damage);
                    if (m.healsPlayer && hpBefore > 0.0f && ent->health <= 0.0f) {
                        // Kill heal: 15% max HP
                        player.health = fminf(player.health + player.maxHealth * 0.15f, player.maxHealth);
                    }
                    enemiesHit++;
                }
            }

            // Holy pillar heals player 3% max HP on hit
            if (m.healsPlayer && enemiesHit > 0) {
                player.health = fminf(player.health + player.maxHealth * 0.03f, player.maxHealth);
            }

            m.active = false;

            if (m.healsPlayer) {
                // Gold holy pillar impact VFX
                if (s_novaCallback) s_novaCallback(m.position, m.radius, m.color);
                if (s_particlePool) {
                    ParticleSystem::spawnMagicBurst(*s_particlePool, m.position, 255, 210, 50, 8);
                }
            } else {
                // Fire meteor impact VFX (original behavior)
                if (s_novaCallback) s_novaCallback(m.position, m.radius, {1.0f, 0.5f, 0.1f});
                if (s_scorchCallback) s_scorchCallback(m.position, m.radius, 2.0f, m.damage * 0.3f);
                if (s_particlePool) {
                    ParticleSystem::spawnExplosion(*s_particlePool, m.position, m.radius);
                    ParticleSystem::spawnSmoke(*s_particlePool, m.position, 12);
                    ParticleSystem::spawnSmoke(*s_particlePool, m.position + Vec3{0, 0.5f, 0}, 8);
                }
                if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
            }
            LOG_INFO("Meteor/pillar struck: hit %u enemies", enemiesHit);
        }
    }
}

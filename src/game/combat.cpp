#include "game/combat.h"
#include "game/hit_feedback.h"
#include "game/player.h"
#include "game/projectile.h"
#include "world/combat_query.h"
#include "renderer/particles.h"
#include "renderer/camera.h"  // for ScreenShake
#include "core/log.h"
#include <cmath>
#include <cstdlib>  // std::rand — used for crit rolls in fire* functions

static Combat::DamageNumberCallback   s_damageNumberCallback   = nullptr;
static Combat::DeathCallback           s_deathCallback           = nullptr;
static Combat::PerfectBlockCallback    s_perfectBlockCallback    = nullptr;
static Combat::DodgeThroughCallback    s_dodgeThroughCallback    = nullptr;
static Combat::OnKillFn                s_onKill                  = nullptr;
static ParticlePool* s_particlePool = nullptr;
static ScreenShake*  s_screenShake  = nullptr;
// (L8) Player slot credited for the current damage source (0xFF = none/environmental).
// Set by the engine around weapon fire and by projectile.cpp per projectile; stamped onto
// Entity::killerSlot in killEntity so loot drops can be reserved to the killer.
static u8 s_attackingPlayer = 0xFF;
// D1.1 — Weapon mesh ID for the current attack context. Set by the engine alongside
// setAttackingPlayer so kill events can include which weapon landed the killing blow.
static u8 s_killWeaponMeshId = 0;
// D1.1 — isCrit flag of the hit that most recently caused a transition through applyDamage
// into killEntity. Set by applyDamage before calling killEntity so the kill callback has
// accurate crit info. Reset to 0 after each killEntity call.
static u8 s_pendingKillIsCrit = 0;

void Combat::setAttackingPlayer(u8 slot) { s_attackingPlayer = slot; }
u8   Combat::getAttackingPlayer()        { return s_attackingPlayer; }

void Combat::setKillWeaponMeshId(u8 meshId) { s_killWeaponMeshId = meshId; }
u8   Combat::getKillWeaponMeshId()          { return s_killWeaponMeshId; }

void Combat::setOnKill(OnKillFn fn) { s_onKill = fn; }

void Combat::setDamageNumberCallback(DamageNumberCallback cb) {
    s_damageNumberCallback = cb;
}

void Combat::spawnDamageNumber(Vec3 position, f32 amount) {
    // Skills that call this directly (e.g. riposte, drone) never produce crits or kills
    // through this path, so both flags are false.
    if (s_damageNumberCallback) s_damageNumberCallback(position, amount, false, false);
}

void Combat::setDeathCallback(DeathCallback cb) {
    s_deathCallback = cb;
}

void Combat::setPerfectBlockCallback(PerfectBlockCallback cb) {
    s_perfectBlockCallback = cb;
}

void Combat::setDodgeThroughCallback(DodgeThroughCallback cb) {
    s_dodgeThroughCallback = cb;
}

void Combat::setFXTargets(ParticlePool* particles, ScreenShake* shake) {
    s_particlePool = particles;
    s_screenShake  = shake;
}

void Combat::applyDamage(EntityPool& pool, EntityHandle target, f32 damage,
                          const Vec3* damageOrigin, bool isCrit) {
    Entity* e = handleGet(pool, target);
    if (!e) return;
    if (e->flags & ENT_DEAD) return;

    // Entombed boss (Malachar's false-death channel) is fully invulnerable —
    // hits register as a flash but deal no damage until the channel ends.
    if (e->bossPhase == BossPhase::ENTOMBING) {
        e->flashTimer = 0.12f;
        return;
    }

    // Shield Bearer frontal damage reduction — forces player to flank
    if ((e->enemyRole & EnemyRole::SHIELD_BEARER) && damageOrigin) {
        Vec3 toSource = *damageOrigin - e->position;
        f32 len = sqrtf(toSource.x * toSource.x + toSource.z * toSource.z);
        if (len > 0.001f) {
            Vec3 dirToSource = {toSource.x / len, 0.0f, toSource.z / len};
            // Entity facing direction from yaw
            Vec3 facing = {sinf(e->yaw), 0.0f, cosf(e->yaw)};
            f32 d = dirToSource.x * facing.x + dirToSource.z * facing.z;
            // dot < -0.3 means damage came from the front (facing toward source)
            if (d < -0.3f) damage *= 0.5f;
        }
    }

    // Minion Shield: boss takes 75% reduced damage while alive minions exist
    if (e->minionShield && e->bossDefIdx != 0xFF && e->spawnerIdx == 0xFFFF) {
        bool hasAliveMinion = false;
        for (u32 a = 0; a < pool.activeCount; a++) {
            const Entity& m = pool.entities[pool.activeList[a]];
            if (m.spawnerIdx == target.index && !(m.flags & ENT_DEAD)) {
                hasAliveMinion = true;
                break;
            }
        }
        if (hasAliveMinion) damage *= 0.25f;
    }

    // Paladin passive: 25% damage reduction
    if (e->npcClass == NpcClass::PALADIN) damage *= 0.75f;
    // Mark Prey: marked targets take amplified damage
    if (e->markPreyDmgMult > 1.0f) damage *= e->markPreyDmgMult;

    // If this IDLE enemy survives the hit, alert nearby hostiles within 6m
    bool wasIdle = (e->aiState == AIState::IDLE);

    e->health -= damage;
    e->flashTimer = 0.12f;
    e->provoked = true; // any hit provokes: a boss now engages even if the attacker is outside its leash arena

    // --- Hit feedback: classify impact tier and fire the matching recipe ---
    bool isKill = (e->health <= 0.0f);
    ImpactTier tier = classifyTier(damage, e->maxHealth, isCrit, isKill);
    const HitFeedbackTier& fx = kHitTiers[static_cast<u32>(tier)];
    Vec3 hitPos = e->position + Vec3{0, 0.5f, 0};

    // Camera shake (trigger() keeps the stronger of current/new, so spam is fine).
    if (s_screenShake && fx.shakeIntensity > 0.0f)
        s_screenShake->trigger(fx.shakeIntensity, fx.shakeDuration);

    // Particles. Blood/sparks fly back along the hit direction; debris/smoke on kills.
    if (s_particlePool) {
        Vec3 back = {0, 1, 0};
        if (damageOrigin) {
            Vec3 d = e->position - *damageOrigin; d.y = 0.0f;
            f32 len = sqrtf(d.x*d.x + d.z*d.z);
            // Normalise to get the knockback-back direction; fall back to up if origin coincides
            if (len > 0.001f) back = Vec3{d.x/len, 0.4f, d.z/len};
        }
        if (fx.bloodCount)  ParticleSystem::spawnBlood(*s_particlePool, hitPos, back, fx.bloodCount);
        if (fx.sparkCount)  ParticleSystem::spawnSparks(*s_particlePool, hitPos, back, fx.sparkCount);
        if (fx.debrisCount) ParticleSystem::spawnDebris(*s_particlePool, hitPos, fx.debrisCount);
        if (fx.smoke)       ParticleSystem::spawnSmoke(*s_particlePool, hitPos, 4);
    }

    // Fire the damage number callback with crit/kill flags so the renderer can style them.
    if (s_damageNumberCallback) s_damageNumberCallback(hitPos, damage, isCrit, isKill);

    // Damage alert: first hit on an unalerted enemy that survives wakes neighbors
    if (wasIdle && e->health > 0.0f) {
        e->aiState = AIState::CHASE;
        constexpr f32 ALERT_RADIUS_SQ = 6.0f * 6.0f;
        for (u32 a = 0; a < pool.activeCount; a++) {
            Entity& n = pool.entities[pool.activeList[a]];
            if (n.aiState != AIState::IDLE) continue;
            if (n.flags & ENT_FRIENDLY) continue;
            Vec3 d = n.position - e->position;
            if (d.x*d.x + d.z*d.z < ALERT_RADIUS_SQ) {
                n.aiState = AIState::CHASE;
            }
        }
    }

    // --- Knockback (authoritative): push the victim along the hit direction. ---
    // Only when we know where the hit came from (no DoT/environmental knockback).
    if (fx.knockback > 0.0f && damageOrigin) {
        Vec3 push = e->position - *damageOrigin; push.y = 0.0f;
        f32 len = sqrtf(push.x*push.x + push.z*push.z);
        if (len > 0.001f) {
            // Size/mass resistance: larger enemies move less; bosses barely budge.
            f32 sizeResist = 0.5f / (e->halfExtents.x + e->halfExtents.z + 0.001f);
            if (sizeResist > 1.0f) sizeResist = 1.0f;
            if (e->bossDefIdx != 0xFF) sizeResist *= 0.1f; // bosses ~immovable
            f32 impulse = fx.knockback * sizeResist;
            e->velocity.x += (push.x / len) * impulse;
            e->velocity.z += (push.z / len) * impulse;
            e->knockbackTimer = 0.25f;
        }
    }

    if (e->health <= 0.0f) {
        // False death (Malachar): the first lethal hit doesn't kill — he survives
        // at 60% HP and entombs himself. The AI tick (enemy_ai_boss.cpp) drives the
        // channel/guardian-summon; sprintTimer < 0 is the "channel not yet started"
        // sentinel it watches for. Only fires once (ARMED → ENTOMBING).
        if (e->bossPhase == BossPhase::ARMED) {
            e->health     = e->maxHealth * 0.6f;
            e->bossPhase  = BossPhase::ENTOMBING;
            e->sprintTimer = -1.0f;
            e->flashTimer = 0.2f;
        } else {
            // D1.1 — Pass isCrit down so killEntity's s_onKill callback carries it.
            s_pendingKillIsCrit = isCrit ? 1 : 0;
            killEntity(pool, target);
        }
    }
}

// Canonical death transition + death-callback dispatch. Single source of truth so
// every kill path (applyDamage and the DoT/environmental sources) drops loot and
// fires squad/death procs identically.
void Combat::killEntity(EntityPool& pool, EntityHandle target) {
    Entity* e = handleGet(pool, target);
    if (!e || (e->flags & ENT_DEAD)) return;
    e->health     = 0.0f;
    e->flags     |= ENT_DEAD;
    e->aiState    = AIState::DEAD;
    e->deathTimer = 1.0f;
    e->velocity   = {0, 0, 0};
    e->killerSlot = s_attackingPlayer; // (L8) credit the kill before the loot callback reads it

    // D1.1 — Emit kill event before the death callback so the emitter fires at the same
    // point the entity transitions to dead. victimType=0 (entity). s_pendingKillIsCrit is
    // set by applyDamage for damage-path kills; direct killEntity calls (DoT/environment)
    // leave it 0 (no crit context available). Reset after use so the next kill starts clean.
    if (s_onKill) {
        s_onKill(s_attackingPlayer, /*victimType=*/0,
                 static_cast<u16>(target.index),
                 s_killWeaponMeshId, s_pendingKillIsCrit);
    }
    s_pendingKillIsCrit = 0; // reset for next call

    if (s_deathCallback) {
        s_deathCallback(pool, target.index, e->position);
    }
}

void Combat::applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos,
                                   u16 attackerIdx) {
    // Dodge-through detection: if mid-roll and an attack connects, trigger riposte.
    // This fires regardless of i-frame state — the roll itself is the dodge.
    if (player.dodgeState.rolling) {
        if (s_dodgeThroughCallback && attackerIdx != 0xFFFF) {
            s_dodgeThroughCallback(attackerIdx, attackerPos ? *attackerPos : Vec3{0, 0, 0});
        }
        // During i-frames (first 0.3s), block all damage
        if (player.invulnTimer > 0.0f) return;
        // Recovery frames (last 0.2s): damage still goes through below
    }

    // Non-roll invulnerability (respawn/floor entry grace period)
    if (player.invulnTimer > 0.0f) return;

    // Wanderer Deflect: full immunity — absorb raw damage and hit count.
    // When window expires, fires 8 projectiles per absorbed hit.
    if (player.deflectTimer > 0.0f) {
        player.deflectAbsorbed += damage;
        player.deflectHitCount++;
        return; // fully immune, no damage applied
    }

    // Class passive damage reduction (e.g. Warrior 30%)
    damage *= (1.0f - player.damageReduction);

    // Necromancer curse: +5% damage taken per stack
    if (player.curseStacks > 0) {
        damage *= (1.0f + player.curseStacks * 0.05f);
    }

    if (player.blocking) {
        if (player.blockTimer < 0.2f) {
            // Perfect block — negate all damage, trigger shield bash via callback
            damage = 0.0f;
            if (s_perfectBlockCallback) s_perfectBlockCallback(player);
        } else {
            // Normal block — halve damage
            damage *= 0.5f;
        }
    }

    player.health -= damage;
    player.damageFlashTimer = 0.15f;
    player.hitShakeTimer = 0.15f;
    // Track damage taken this frame for ring passives (thorns, etc.)
    player.lastDamageTaken = damage;

    // Near-death grace ("lifesaver"): when a hit drops the player into critical HP
    // (<20%) — OR would have killed them outright (a one-shot) — grant a brief invisible
    // i-frame so a follow-up hit can't instantly finish them; a lethal hit is survived at
    // 1 HP. It is a ONE-SHOT: consumed on use, then re-earned only by recovering to >=40%
    // max HP. So once you spend it, healing back to, say, 30% does NOT refill it — you must
    // reach 40%+ to be protected again, and staying in the danger zone never refills it.
    {
        f32 critThresh   = player.maxHealth * 0.20f;  // danger zone: i-frame may fire below this
        f32 rearmThresh  = player.maxHealth * 0.40f;  // must recover above this to re-earn it
        f32 healthBefore = player.health + damage;     // HP just before this hit landed
        if (healthBefore >= rearmThresh) player.lifesaverArmed = true;  // were healthy -> (re)armed
        // Fires when a hit drops us into the danger zone OR would have killed us outright (a one-shot).
        // `lifesaverArmed` is set only from >=40% HP, so this can only save you from a healthy state.
        if (player.lifesaverArmed && player.health < critThresh) {
            if (player.health <= 0.0f) player.health = 1.0f; // cheat death: survive the otherwise-lethal hit at 1 HP
            if (player.invulnTimer < 0.3f) player.invulnTimer = 0.3f;
            player.lifesaverArmed = false;  // consume; re-earn by healing to >=40% HP
        }
    }

    // Red hurt vignette, scaled by the size of the hit relative to max HP.
    // Larger hits push vignette higher; clamp so it never whiteouts the screen.
    if (damage > 0.0f) {
        f32 frac = damage / (player.maxHealth > 0.0f ? player.maxHealth : 100.0f);
        f32 v = 0.15f + frac * 0.6f;  // small floor so light hits still register, scales with damage
        if (v > 0.85f) v = 0.85f;
        if (v > player.hurtVignette) player.hurtVignette = v;
    }

    // Record hit direction for CS-style directional indicator
    if (attackerPos && damage > 0.0f) {
        Vec3 toAttacker = *attackerPos - player.position;
        f32 worldAngle = atan2f(toAttacker.x, toAttacker.z);
        f32 relAngle = worldAngle - player.yaw;
        for (u32 i = 0; i < Player::MAX_HIT_INDICATORS; i++) {
            if (player.hitIndicators[i].timer <= 0.0f) {
                player.hitIndicators[i] = {relAngle, 0.8f};
                break;
            }
        }
    }

    if (player.health <= 0.0f) {
        player.health = 0.0f;
    }
}

AttackResult Combat::fireMelee(const WeaponDef& weapon,
                                Vec3 eyePos, Vec3 forward,
                                EntityPool& pool)
{
    AttackResult result;
    result.didFire = true;

    f32 halfAngle = radians(weapon.coneAngleDeg * 0.5f);
    f32 cosCone   = cosf(halfAngle);

    EntityHandle hits[MAX_ENTITIES];
    f32 distances[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        pool, eyePos, forward, cosCone, weapon.range,
        hits, distances, MAX_ENTITIES, /*horizontalCone=*/true);

    // Roll crit once for the whole swing — all cone hits share the same outcome.
    // Using 0..9999 * 0.0001 gives a uniform [0,1) float without float-modulo artifacts.
    bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
    f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
    for (u32 i = 0; i < hitCount; i++) {
        // applyDamage owns all on-hit FX (tier-driven shake + blood/sparks via the
        // hit-feedback recipe); crit selects the CRIT tier. No inline FX here —
        // that would double up with the tier system.
        applyDamage(pool, hits[i], dmg, &eyePos, crit);
    }

    result.entitiesHit = hitCount;
    if (hitCount > 0) {
        result.hitEntity = true;
        Entity* e = handleGet(pool, hits[0]);
        if (e) {
            result.hitPosition = e->position;
            result.hitDistance  = distances[0];
        }
        // M10.2: store per-hit handles so the server can emit SV_DAMAGE_DONE per hit.
        u32 storeCount = hitCount < MAX_ATTACK_HITS ? hitCount : MAX_ATTACK_HITS;
        for (u32 i = 0; i < storeCount; i++) result.hitHandles[i] = hits[i];
    }

    return result;
}

AttackResult Combat::fireHitscan(const WeaponDef& weapon,
                                  Vec3 eyePos, Vec3 forward,
                                  const LevelGrid& grid,
                                  EntityPool& pool)
{
    AttackResult result;
    result.didFire = true;

    CombatHit hit = CombatQuery::raycast(grid, pool, eyePos, forward, weapon.range);

    if (hit.hit) {
        result.hitPosition = hit.position;
        result.hitNormal   = hit.normal;
        result.hitDistance  = hit.distance;

        if (hit.type == CombatHit::ENTITY) {
            result.hitEntity   = true;
            result.entitiesHit = 1;
            result.hitHandles[0] = hit.entityHandle; // M10.2: store for SV_DAMAGE_DONE
            // Roll crit per shot — hitscan always hits exactly one target.
            bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
            f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
            applyDamage(pool, hit.entityHandle, dmg, &eyePos, crit);
        } else {
            result.hitWorld = true;
            // Debris chips on wall hit
            if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, hit.position, 4);
        }
    }

    return result;
}

u16 Combat::fireProjectile(const WeaponDef& weapon,
                            Vec3 eyePos, Vec3 forward,
                            ProjectilePool& projectiles,
                            u8 extraFlags)
{
    // Small forward nudge so the projectile clears the player's own collision box.
    // Callers already offset eyePos to the weapon tip, so 1.0m was too far and
    // caused projectiles to skip over close enemies.
    Vec3 spawnPos = eyePos + forward * 0.3f;
    // Roll crit at spawn; multiply damage into the projectile so the hit path is crit-unaware.
    bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
    f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
    u16 idx = ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                       weapon.projectileSpeed, dmg,
                                       weapon.projectileRadius, 3.0f, true, extraFlags);
    if (idx != 0xFFFF) {
        projectiles.projectiles[idx].isCrit    = crit;
        projectiles.projectiles[idx].ownerSlot = s_attackingPlayer; // (L8) credit the firer
    }
    return idx;
}

u16 Combat::fireProjectile(const WeaponDef& weapon,
                            Vec3 eyePos, Vec3 forward,
                            ProjectilePool& projectiles,
                            f32 gravity, f32 splashRadius, f32 splashDamage)
{
    Vec3 spawnPos = eyePos + forward * 0.3f;
    // Splash/gravity projectiles (e.g. molotov) can still crit; the direct-hit damage
    // is multiplied, but splash damage is kept at base (AoE splash does not crit — see
    // projectile.cpp). isCrit is stored so the hit call in projectile.cpp can trigger
    // the CRIT feedback tier on the primary target.
    bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
    f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
    u16 idx = ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                       weapon.projectileSpeed, dmg,
                                       weapon.projectileRadius, 5.0f, true);
    if (idx != 0xFFFF) {
        Projectile& p = projectiles.projectiles[idx];
        p.gravity      = gravity;
        p.splashRadius = splashRadius;
        p.splashDamage = splashDamage;
        p.isCrit       = crit;
        p.ownerSlot    = s_attackingPlayer; // (L8) credit the firer
        if (gravity > 0.0f) p.projFlags |= PROJ_GRAVITY;
        if (splashRadius > 0.0f) p.projFlags |= PROJ_SPLASH;
    }
    return idx;
}

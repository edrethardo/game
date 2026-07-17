#include "game/combat.h"
#include "game/floor_event.h"   // Goblin::ESCAPE_SECONDS (a hit starts its escape clock)
#include "game/champion.h"  // champion affix behaviours (VAMPIRIC / SHIELDING / HEALTH_LINK)
#include "game/hit_feedback.h"
#include "game/player.h"
#include "game/projectile.h"
#include "world/combat_query.h"
#include "world/raycast.h"     // grid DDA — pvpRay wall occlusion
#include "world/collision.h"   // PLAYER_HALF_WIDTH / PLAYER_HEIGHT — PvP target boxes
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
// The authoritative entity pool. applyDamageToPlayer receives only an attackerIdx, so without this
// there is no way to reach the attacker — which is what the VAMPIRIC champion affix needs in order
// to heal the thing that just hit you. Set once in Engine::init; null on a client (where the whole
// damage path is gated off anyway), so every use is null-checked.
static EntityPool*   s_entityPool   = nullptr;
// (L8) Player slot credited for the current damage source (0xFF = none/environmental).
// Set by the engine around weapon fire and by projectile.cpp per projectile; stamped onto
// Entity::killerSlot in killEntity so loot drops can be reserved to the killer.
static u8 s_attackingPlayer = 0xFF;
// Last enemy each player damaged (see Combat::getLastHitEntity). 4 = MAX_PLAYERS; combat.cpp
// deliberately doesn't include net.h, so the size is pinned by a static_assert at the use site.
static constexpr u8 kMaxTrackedPlayers = 4;
static EntityHandle s_lastHitEntity[kMaxTrackedPlayers] = {};
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

void Combat::setEntityPool(EntityPool* pool) { s_entityPool = pool; }

EntityHandle Combat::getLastHitEntity(u8 slot) {
    if (slot >= kMaxTrackedPlayers) return EntityHandle{};
    return s_lastHitEntity[slot];
}
void Combat::clearLastHitEntities() {
    for (u8 i = 0; i < kMaxTrackedPlayers; i++) s_lastHitEntity[i] = EntityHandle{};
}

void Combat::applyDamage(EntityPool& pool, EntityHandle target, f32 damage,
                          const Vec3* damageOrigin, bool isCrit) {
    Entity* e = handleGet(pool, target);
    if (!e) return;
    if (e->flags & ENT_DEAD) return;

    // Cosmetic pets (mini loot goblin) are invulnerable. Hostiles never TARGET them
    // (ENT_UNTARGETABLE), but splash/AoE and stray friendly fire funnel through here —
    // one gate makes the companion unkillable by construction instead of by targeting luck.
    if ((e->flags & ENT_FRIENDLY) && e->npcClass == NpcClass::PET) return;

    // STONE SLEEP: a dormant AMBUSH-role enemy (the gargoyle) is literally a statue — fully
    // invulnerable. Deliberately ABOVE every flash/flinch/knockback/damage-number line below,
    // so a hit leaves no mark at all: flashTimer is exactly what the DORMANT state reads as
    // "combat nearby", so even a cosmetic flash would let players shoot statues awake from
    // safety. The only wake path is the weeping-angel rule (someone in aggro range while
    // nobody watches — enemy_ai_states.cpp DORMANT). Mimics are DORMANT too but carry no
    // AMBUSH role, so they stay hittable — whacking the suspicious chest still springs it.
    if (e->aiState == AIState::DORMANT && (e->enemyRole & EnemyRole::AMBUSH)) return;

    // A loot goblin bolts the moment it is HIT — not before. It spawns IDLE, guarding its hoard, and
    // this is what starts the chase: the escape clock only begins ticking now, so the goblin cannot
    // quietly time out and vanish while the player is still two rooms away and has never seen it.
    // Placed at the very top of the single funnel every player-sourced hit passes through, so it
    // fires for a sword, an arrow, a skill or a burning floor alike — before any of the immunity or
    // shield branches below can return early and swallow the provocation.
    if ((e->flags & ENT_LOOT_GOBLIN) && e->aiState != AIState::FLEE) {
        e->aiState   = AIState::FLEE;
        e->lifeTimer = Goblin::ESCAPE_SECONDS;
    }

    // Entombed boss (Malachar's false-death channel) is fully invulnerable —
    // hits register as a flash but deal no damage until the channel ends.
    if (e->bossPhase == BossPhase::ENTOMBING) {
        e->flashTimer = 0.12f;
        return;
    }

    // The Dungeon Engine superboss: fully immune while any wave-boss it summoned is alive. The
    // player must clear those adds to crack its shield. Gated on isEngine (only the Engine is
    // shielded) — the predicate itself is keyed on spawnerIdx, which the summoned wave-bosses carry
    // (they have isEngine == false, so they always take full damage). See updateEngineBoss.
    if (e->isEngine && Combat::engineShieldActive(pool, target.index)) {
        e->flashTimer = 0.12f;   // registers a hit, deals no damage
        return;
    }

    // --- Champion: SHIELDING ---
    // A recurring immunity window. The point is to punish pure burst-DPS: you have to notice the
    // shield is up and stop feeding damage into it. The cycle is derived from the entity's own
    // animTimer rather than a new timer field, so it needs no extra state (Entity has no spare
    // bytes) and stays a pure function of data the host already ticks. `minionShield` is set as the
    // TELL because it is already on the wire and already drives a blue-white shimmer in the
    // renderer — so the guest sees the shield without any new replication.
    if ((e->flags & ENT_CHAMPION) && (e->champAffixes & ChampAffix::SHIELDING)) {
        const f32 period = Champion::SHIELDING_UP_SEC + Champion::SHIELDING_GAP_SEC;
        const f32 phase  = fmodf(e->animTimer, period);
        const bool up    = (phase < Champion::SHIELDING_UP_SEC);
        e->minionShield  = up;   // replicated tell (bossStatus bit0)
        if (up) {
            e->flashTimer = 0.12f;   // the hit registers, but deals nothing
            return;
        }
    }

    // --- Champion: HEALTH_LINK ---
    // Damage to the leader is shared with its living minions, so the pack cannot be defeated by
    // focusing the big one — you have to clear the escort first. Redirected damage is dealt as a
    // real hit on each minion (recursion is impossible: minions carry no affixes, so they can never
    // re-enter this branch).
    if ((e->flags & ENT_CHAMPION) && (e->champAffixes & ChampAffix::HEALTH_LINK)) {
        u16 minions[MAX_ENTITIES];
        u32 minionCount = 0;
        for (u32 a = 0; a < pool.activeCount && minionCount < MAX_ENTITIES; a++) {
            const u32 mi = pool.activeList[a];
            const Entity& m = pool.entities[mi];
            if (m.champLeaderIdx == target.index && !(m.flags & ENT_DEAD))
                minions[minionCount++] = static_cast<u16>(mi);
        }
        if (minionCount > 0) {
            const f32 shared = damage * Champion::HEALTH_LINK_SHARE;
            damage -= shared;                                 // the leader eats the remainder
            const f32 each = shared / static_cast<f32>(minionCount);
            for (u32 i = 0; i < minionCount; i++) {
                EntityHandle mh{ minions[i], pool.entities[minions[i]].generation };
                applyDamage(pool, mh, each, damageOrigin, false);
            }
        }
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

    // Remember what this player just hit — the health bar falls back to it when the crosshair
    // drifts off. Recorded here because this is the single point every player-sourced hit passes
    // through; doing it at the call sites is how a new damage source silently stops reporting.
    if (s_attackingPlayer < kMaxTrackedPlayers)
        s_lastHitEntity[s_attackingPlayer] = target;

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

    // Damage alert: first hit on an unalerted enemy that survives wakes neighbors.
    // Both wakes enter the authored combat opener (a sniped strafer should kite, not
    // beeline) — preferredCombatState is the pure map; no grid here, so no FLANK opener.
    if (wasIdle && e->health > 0.0f) {
        e->aiState = preferredCombatState(*e);
        constexpr f32 ALERT_RADIUS_SQ = 6.0f * 6.0f;
        for (u32 a = 0; a < pool.activeCount; a++) {
            Entity& n = pool.entities[pool.activeList[a]];
            if (n.aiState != AIState::IDLE) continue;
            if (n.flags & ENT_FRIENDLY) continue;
            Vec3 d = n.position - e->position;
            if (d.x*d.x + d.z*d.z < ALERT_RADIUS_SQ) {
                n.aiState = preferredCombatState(n);
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

f32 Combat::armorMitigation(f32 armor) {
    if (armor <= 0.0f) return 0.0f;
    f32 mit = armor / (armor + 100.0f); // diminishing returns: 100 armor = 50%
    return (mit > 0.80f) ? 0.80f : mit;  // hard cap so a stacked build can't become invulnerable
}

// The single choke for player crowd control — thin wrapper over the pure CrowdControl::resolveCC
// (immunity/dodge negate → tenacity → PvP stun DR). Reads the victim's defensive state off the
// Player and raises the chosen timer (fmaxf so a longer CC never gets shortened by a weaker one).
void Combat::applyCCToPlayer(Player& p, CcType type, f32 duration, bool isPvp) {
    CrowdControl::CcKind kind = (type == CcType::STUN)   ? CrowdControl::CcKind::STUN
                             : (type == CcType::SLOW)    ? CrowdControl::CcKind::SLOW
                                                         : CrowdControl::CcKind::FREEZE;
    const bool immune      = p.ccImmuneTimer > 0.0f;
    // A perfect DODGE (any roll's i-frames — `dodgeState.rolling`, the same window the dodge-through
    // absorb uses) and a perfect BLOCK ALWAYS negate incoming CC. Both are hard timing feats, so they
    // are always rewarded — universal (every class, PvE AND PvP), not gated on the Steadfast Greaves.
    // (`ccDodgeImmune` now only powers the boots-only escapes: dodging WHILE stunned + clearing
    // already-active CC on the roll — things a base dodge can't do.)
    const bool dodgeNegate = p.dodgeState.rolling;
    const bool blockNegate = (classifyBlock(p.blocking, p.blockTimer) == BlockOutcome::PERFECT);
    CrowdControl::CcResult r = CrowdControl::resolveCC(kind, duration, p.ccResist, immune,
                                                       dodgeNegate || blockNegate, p.stunDr, isPvp);
    if (!r.apply) return;
    switch (type) {
        case CcType::STUN:   p.stunTimer   = fmaxf(p.stunTimer,   r.duration); break;
        case CcType::SLOW:   p.slowTimer   = fmaxf(p.slowTimer,   r.duration); break;
        case CcType::FREEZE: p.freezeTimer = fmaxf(p.freezeTimer, r.duration); break;
    }
}

Combat::BlockOutcome Combat::applyDamageToPlayer(Player& player, f32 damage,
                                                 const Vec3* attackerPos, u16 attackerIdx) {
    // --- Champion: VAMPIRIC ---
    // The champion heals for a share of the damage it deals, so trading blows with it is a losing
    // proposition — you have to out-damage the lifesteal or disengage. Hooked here, at the single
    // point every enemy-to-player hit funnels through, rather than at the individual AI attack call
    // sites: doing it per-site is how you end up with an affix that works for melee and silently
    // does nothing for a ranged champion. Runs before mitigation so it steals the swing's full
    // value, which is also what the player sees in the damage number.
    if (s_entityPool && attackerIdx < MAX_ENTITIES) {
        Entity& att = s_entityPool->entities[attackerIdx];
        if ((att.flags & ENT_ACTIVE) && !(att.flags & ENT_DEAD) &&
            (att.flags & ENT_CHAMPION) && (att.champAffixes & ChampAffix::VAMPIRIC)) {
            att.health += damage * Champion::VAMPIRIC_HEAL_PCT;
            if (att.health > att.maxHealth) att.health = att.maxHealth;
        }
    }

    // Dodge-through detection: if mid-roll and an attack connects, trigger riposte + Adrenaline.
    // This fires regardless of i-frame state — the roll itself is the dodge. Fire for ANY attack
    // that lands mid-roll: melee (valid attackerIdx → riposte counter-hit) AND projectiles/AoE
    // (attackerIdx 0xFFFF → no riposte, but still a dodge-through that fuels Adrenaline Surge).
    // Previously gated on `attackerIdx != 0xFFFF`, which silently excluded every projectile dodge
    // — the main thing a Wanderer rolls through — so adrenaline stacks almost never built. The
    // callback is robust to 0xFFFF: its riposte/Exploit-Weakness blocks are guarded by
    // `attackerIdx < MAX_ENTITIES`, so they self-skip and only the stack grant runs.
    if (player.dodgeState.rolling) {
        if (s_dodgeThroughCallback) {
            s_dodgeThroughCallback(player, attackerIdx, attackerPos ? *attackerPos : Vec3{0, 0, 0});
        }
        // During i-frames (first 0.3s), block all damage
        if (player.invulnTimer > 0.0f) return BlockOutcome::NONE;
        // Recovery frames (last 0.2s): damage still goes through below
    }

    // Non-roll invulnerability (respawn/floor entry grace period)
    if (player.invulnTimer > 0.0f) return BlockOutcome::NONE;

    // Wanderer Deflect: full immunity — absorb raw damage and hit count.
    // When window expires, fires 8 projectiles per absorbed hit.
    if (player.deflectTimer > 0.0f) {
        player.deflectAbsorbed += damage;
        player.deflectHitCount++;
        return BlockOutcome::NONE; // fully immune, no damage applied
    }

    // Class passive damage reduction (e.g. Warrior 30%)
    damage *= (1.0f - player.damageReduction);

    // Necromancer curse: +5% damage taken per stack
    if (player.curseStacks > 0) {
        damage *= (1.0f + player.curseStacks * 0.05f);
    }

    const BlockOutcome blockOutcome = classifyBlock(player.blocking, player.blockTimer);
    if (blockOutcome == BlockOutcome::PERFECT) {
        // Perfect block — negate all damage, trigger the legendary-shield effect via callback
        damage = 0.0f;
        if (s_perfectBlockCallback) s_perfectBlockCallback(player, attackerIdx);
    } else if (blockOutcome == BlockOutcome::BLOCKED) {
        // Normal block — halve damage
        damage *= 0.5f;
    }

    // Armor (defensive pack): flat rating → diminishing-returns mitigation (armorMitigation),
    // applied after the class/block reductions and capped at 80% so a stacked armor build can't
    // become invulnerable. armorRating is refreshed each frame from equipped affixes in
    // tickPassiveEquipment. Direct hits only — poison/burn DoT is intentionally unmitigated.
    damage *= (1.0f - armorMitigation(player.armorRating));

    player.health -= damage;
    player.damageFlashTimer = 0.15f;
    player.hitShakeTimer = 0.15f;
    // Track damage taken this frame for ring passives (thorns, etc.) and remember who dealt it so
    // thorns can reflect at the actual attacker (0xFFFF for sources with no attacker entity).
    player.lastDamageTaken = damage;
    player.lastDamageAttackerIdx = attackerIdx;
    // Killing-blow bookkeeping (never frame-cleared, unlike the thorns pair above — those are
    // consumed and zeroed by the retaliation pass): only overwrite when the hit HAS a source
    // entity, so an environmental tick can't erase the real killer between hit and death check.
    if (attackerIdx < MAX_ENTITIES) player.lastAttackerEntity = attackerIdx;

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
            player.graceInvuln = true;      // tag so the "clear grace once healthy" rule can cancel THIS invuln
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
    return blockOutcome;
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
        // Brief hit-flinch ("hitstop") so a melee connect has weight. At a ~0.4 s swing
        // cadence a 0.06 s stun is a flinch, never a stun-lock. Bosses ignore it (they
        // already shrug off knockback) so it can't trivialise milestone fights.
        Entity* he = handleGet(pool, hits[i]);
        if (he && he->bossDefIdx == 0xFF) he->stunTimer = fmaxf(he->stunTimer, 0.06f);
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

// ---------------------------------------------------------------------------------------------
// PvP (Arena mode) — the combatant registry + weapon/AoE geometry vs players.
//
// The engine registers the tick's combatants while the arena's authoritative PvP window is open
// (Engine::arenaBeginPvpWindow → arenaEndPvpWindow); outside it the registry is empty and every
// helper is a no-op, which is the entire PvE-safety story: no arena checks are needed at any
// call site. All landed hits funnel through applyDamageToPlayer, so blocking, perfect blocks,
// armor and i-frames treat a rival player exactly like a monster — and every attempt stamps
// lastHitByPlayerSlot on the victim, which is what the deathmatch loop turns into kill credit.
// ---------------------------------------------------------------------------------------------

static Combat::PvpTarget  s_pvpTargets[8];   // bound is MAX_PLAYERS(4); slack is harmless
static u32                s_pvpTargetCount = 0;
static Combat::PvpApplyFn s_pvpApplyFn     = nullptr;

void Combat::setPvpTargets(const PvpTarget* targets, u32 count) {
    if (!targets) count = 0;
    if (count > 8) count = 8;
    for (u32 i = 0; i < count; i++) s_pvpTargets[i] = targets[i];
    s_pvpTargetCount = count;
}

bool Combat::pvpActive() { return s_pvpTargetCount > 0; }

const Combat::PvpTarget* Combat::pvpTargets(u32& countOut) {
    countOut = s_pvpTargetCount;
    return s_pvpTargets;
}

void Combat::setPvpApply(PvpApplyFn fn) { s_pvpApplyFn = fn; }

Combat::PvpHitOutcome Combat::pvpApply(u8 slot, const PvpHit& hit) {
    if (!s_pvpApplyFn) return PvpHitOutcome{};
    return s_pvpApplyFn(slot, hit);
}

// Shared landing tail for the geometry helpers below: skip corpses, hand the hit to the
// engine's atomic apply (full player-damage pipeline + kill-credit stamp), refresh the
// snapshot's health so a second hit in the same tick sees the first one.
static bool pvpLand(Combat::PvpTarget& t, f32 damage, const Vec3& origin, u8 attackerSlot) {
    if (!t.view || t.view->health <= 0.0f) return false;
    Combat::PvpHit hit{damage, origin, attackerSlot, /*projectile=*/false, 0, 0.0f};
    Combat::PvpHitOutcome out = Combat::pvpApply(t.slot, hit);
    t.view->health = out.newHealth;
    return true;
}

u32 Combat::pvpCone(const WeaponDef& weapon, Vec3 origin, Vec3 forward, u8 attackerSlot) {
    if (s_pvpTargetCount == 0) return 0;
    // Horizontal cone, matching the entity melee query (queryConeSorted horizontalCone=true):
    // flatten both the aim and the to-target vector so pitch doesn't shrink the swing.
    Vec3 flat = {forward.x, 0.0f, forward.z};
    f32 flatLen = sqrtf(flat.x * flat.x + flat.z * flat.z);
    if (flatLen < 0.0001f) return 0;
    flat = flat * (1.0f / flatLen);
    f32 cosCone = cosf(radians(weapon.coneAngleDeg * 0.5f));
    // One crit roll for the whole swing — the fireMelee convention.
    bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
    f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
    u32 hits = 0;
    for (u32 i = 0; i < s_pvpTargetCount; i++) {
        PvpTarget& t = s_pvpTargets[i];
        if (t.slot == attackerSlot || !t.view) continue;
        Vec3 to = t.view->position - origin;
        to.y = 0.0f;
        f32 dist = sqrtf(to.x * to.x + to.z * to.z);
        if (dist > weapon.range + PLAYER_HALF_WIDTH) continue;
        if (dist > 0.001f && (to.x * flat.x + to.z * flat.z) / dist < cosCone) continue;
        if (pvpLand(t, dmg, origin, attackerSlot)) hits++;
    }
    return hits;
}

bool Combat::pvpRay(const WeaponDef& weapon, Vec3 origin, Vec3 forward, const LevelGrid& grid,
                    u8 attackerSlot, Vec3* outHitPos) {
    if (s_pvpTargetCount == 0) return false;
    f32 bestT = weapon.range;
    s32 best  = -1;
    for (u32 i = 0; i < s_pvpTargetCount; i++) {
        PvpTarget& t = s_pvpTargets[i];
        if (t.slot == attackerSlot || !t.view || t.view->health <= 0.0f) continue;
        AABB box = {
            t.view->position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
            t.view->position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
        };
        f32 tHit; Vec3 n;
        if (CombatQuery::rayVsAABB(origin, forward, box, tHit, n) && tHit >= 0.0f && tHit < bestT) {
            bestT = tHit;
            best  = static_cast<s32>(i);
        }
    }
    if (best < 0) return false;
    // Wall occlusion: cover must actually stop bullets. The entity hitscan gets this from
    // CombatQuery::raycast; here the grid DDA answers "wall before the player?" directly.
    RayHit wall = Raycast::cast(grid, origin, forward, bestT);
    if (wall.hit && wall.distance < bestT) return false;
    bool crit = ((std::rand() % 10000) * 0.0001f) < weapon.critChance;
    f32  dmg  = crit ? weapon.damage * weapon.critMult : weapon.damage;
    if (!pvpLand(s_pvpTargets[best], dmg, origin, attackerSlot)) return false;
    if (outHitPos) *outHitPos = origin + forward * bestT;
    return true;
}

u32 Combat::pvpRadius(Vec3 center, f32 radius, f32 damage, u8 attackerSlot) {
    if (s_pvpTargetCount == 0) return 0;
    if (attackerSlot == 0xFF) attackerSlot = s_attackingPlayer;  // skill paths maintain the ambient slot
    u32 hits = 0;
    for (u32 i = 0; i < s_pvpTargetCount; i++) {
        PvpTarget& t = s_pvpTargets[i];
        if (t.slot == attackerSlot || !t.view) continue;
        // Chest-height distance so a blast centered at feet or head still counts, padded by
        // the body half-width — the same forgiveness the entity radius queries get from their
        // center-position tests against fatter AABBs.
        Vec3 d = (t.view->position + Vec3{0.0f, PLAYER_HEIGHT * 0.5f, 0.0f}) - center;
        f32 reach = radius + PLAYER_HALF_WIDTH;
        if (d.x * d.x + d.y * d.y + d.z * d.z > reach * reach) continue;
        if (pvpLand(t, damage, center, attackerSlot)) hits++;
    }
    return hits;
}

u32 Combat::pvpRadiusHit(Vec3 center, f32 radius, const PvpHit& proto) {
    if (s_pvpTargetCount == 0) return 0;
    u8 attackerSlot = proto.attackerSlot;
    if (attackerSlot == 0xFF) attackerSlot = s_attackingPlayer;  // skill paths maintain the ambient slot
    u32 hits = 0;
    for (u32 i = 0; i < s_pvpTargetCount; i++) {
        PvpTarget& t = s_pvpTargets[i];
        if (t.slot == attackerSlot || !t.view || t.view->health <= 0.0f) continue;
        Vec3 d = (t.view->position + Vec3{0.0f, PLAYER_HEIGHT * 0.5f, 0.0f}) - center;  // chest height
        f32 reach = radius + PLAYER_HALF_WIDTH;
        if (d.x * d.x + d.y * d.y + d.z * d.z > reach * reach) continue;
        PvpHit hit = proto;
        hit.origin = center;               // knockback pushes outward from the blast
        hit.attackerSlot = attackerSlot;
        Combat::PvpHitOutcome out = Combat::pvpApply(t.slot, hit);
        t.view->health = out.newHealth;    // refresh (may be 0-damage CC, but keep the pattern)
        hits++;
    }
    return hits;
}

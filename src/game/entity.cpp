#include "game/entity.h"
#include "game/combat.h"        // Combat::killEntity — DoT deaths must drop loot too
#include "game/game_constants.h"
#include "core/log.h"

void EntitySystem::init(EntityPool& pool) {
    pool.freeCount = MAX_ENTITIES;
    pool.activeCount = 0;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        pool.entities[i] = {};
        pool.freeList[i] = static_cast<u16>(MAX_ENTITIES - 1 - i);
    }
    LOG_INFO("EntityPool initialized (%u slots)", MAX_ENTITIES);
}

EntityHandle EntitySystem::spawn(EntityPool& pool, Vec3 position, Vec3 halfExtents,
                                  bool flying, f32 health, f32 moveSpeed,
                                  f32 detectionRange, f32 attackRange,
                                  f32 attackCooldown, f32 damage)
{
    if (pool.freeCount == 0) {
        LOG_WARN("EntityPool: no free slots");
        return {};
    }

    u16 idx = pool.freeList[--pool.freeCount];
    Entity& e = pool.entities[idx];
    e.generation++;
    e.flags        = ENT_ACTIVE | (flying ? ENT_FLYING : 0);
    e.position     = position;
    e.homePosition = position;
    e.velocity     = {0,0,0};
    e.yaw          = 0.0f;
    e.halfExtents  = halfExtents;
    e.health       = health;
    e.maxHealth    = health;
    e.moveSpeed    = moveSpeed * GameConst::SPEED_MULT;
    e.detectionRange = detectionRange;
    e.attackRange  = attackRange;
    e.attackCooldown = attackCooldown;
    e.attackTimer  = 0.0f;
    e.damage       = damage;
    e.aiState      = AIState::IDLE;
    e.flashTimer   = 0.0f;
    e.deathTimer   = 0.0f;
    e.ownerLocalPlayer = 0; // default P1; friendly spawn sites override (pool slots aren't zeroed)
    // A recycled slot must not stay tethered to a player or keep an NPC/pet class: a drone
    // reusing a dead pet's slot would inherit PET (follow-only + damage-immune) since drone
    // spawn sites rely on the NONE default rather than setting npcClass explicitly.
    e.ownerNetSlot = 0xFF;
    e.npcClass     = NpcClass::NONE;
    // Pool slots are reused without zeroing, so clear identity fields that callers
    // may not override (spiderlings set enemyType but not enemyRole, etc.). Defaulting
    // to GENERIC/NORMAL prevents a respawned slot from inheriting a stale summoner role.
    e.enemyType = EnemyType::GENERIC;
    e.enemyRole = EnemyRole::NORMAL;
    // Combat opener resets to plain CHASE — only the enemies.json spawn path stamps an authored
    // preference, so a drone/summon recycling a strafer's slot must not inherit its opener.
    e.aiPreference = static_cast<u8>(AIState::CHASE);
    // Reused slots must not inherit a dead boss's identity (it would lock the floor exit,
    // grant knockback immunity, or trigger Malachar's false-death on a normal enemy).
    e.isBoss       = false;
    e.bossDefIdx   = 0xFF;
    e.bossPhase    = BossPhase::NONE;
    // A recycled slot must not keep the previous occupant's live speech bubble: a monster
    // spawning into the slot of one killed mid-sentence displayed the corpse's line for the
    // rest of the 2.4 s window ("old and wrong speech bubbles" — on the HOST; the client-side
    // interp mirror has its own recycle guard in client.cpp).
    e.speechText   = nullptr;
    e.speechTimer  = 0.0f;
    e.minionShield = false;
    e.leashRadius  = 0.0f;
    e.provoked     = false;
    // Same hazard for the secret-superboss fields: a recycled Engine slot would otherwise stay
    // damage-immune (isEngine), and a recycled wave-add/guardian slot would keep a stale spawnerIdx
    // and be mis-counted as some boss's living minion. Callers that summon set these AFTER spawn.
    e.isEngine     = false;
    e.spawnerIdx   = 0xFFFF;
    // Status effects must not carry over to a recycled slot: a freed enemy can still have a live
    // DoT/CC timer when its slot is reused (e.g. a 3s bleed outlasting the 1s death timer), which
    // would phantom-damage/freeze the fresh spawn and — for DoT — mis-credit its kill via
    // poison/burnSrcSlot. Clear them all.
    e.poisonTimer = e.poisonDps = e.burnTimer = e.burnDps = 0.0f;
    e.poisonSrcSlot = e.burnSrcSlot = 0xFF;
    e.freezeTimer = e.stunTimer = 0.0f;
    // Same hazard again for champions: a recycled slot would otherwise stay a phantom champion —
    // still tinted/scaled and still running Molten/Vampiric/etc. on a plain enemy. The ENT_CHAMPION
    // bit itself is cleared with the rest of `flags` by the caller, but these two are not.
    e.champAffixes   = 0;
    e.champLeaderIdx = 0xFFFF;
    e.champNameIdx   = 0;
    e.enemyDefIdx    = 0xFF;   // recycled slot must not inherit the previous monster's identity
    e.lifeTimer      = 0.0f;   // recycled slot must not inherit a goblin's escape countdown

    // Add to active list
    pool.activeList[pool.activeCount++] = idx;

    return {idx, e.generation};
}

static void removeFromActiveList(EntityPool& pool, u16 idx) {
    for (u32 i = 0; i < pool.activeCount; i++) {
        if (pool.activeList[i] == idx) {
            // Swap with last
            pool.activeList[i] = pool.activeList[pool.activeCount - 1];
            pool.activeCount--;
            return;
        }
    }
}

void EntitySystem::despawn(EntityPool& pool, EntityHandle handle) {
    if (!handleValid(pool, handle)) return;
    Entity& e = pool.entities[handle.index];
    e.flags = 0;
    removeFromActiveList(pool, handle.index);
    pool.freeList[pool.freeCount++] = handle.index;
}

void EntitySystem::tickTimers(EntityPool& pool, f32 dt) {
    for (u32 a = 0; a < pool.activeCount; ) {
        u32 i = pool.activeList[a];
        Entity& e = pool.entities[i];

        if (e.flashTimer > 0.0f) e.flashTimer -= dt;

        // Generic despawn countdown (the loot goblin's escape). Expiry is deliberately NOT routed
        // through Combat::killEntity — that always fires the death/loot callback, and a goblin that
        // got away must pay out nothing. Setting ENT_DEAD + a tiny deathTimer reuses the normal
        // free-the-slot path (the Swarm Queen expires the same way).
        if (e.lifeTimer > 0.0f && !(e.flags & ENT_DEAD)) {
            e.lifeTimer -= dt;
            if (e.lifeTimer <= 0.0f) {
                e.flags     |= ENT_DEAD;
                e.aiState    = AIState::DEAD;
                e.deathTimer = 0.01f;
            }
        }

        if (e.knockbackTimer > 0.0f) {
            e.knockbackTimer -= dt;
            // Decay the horizontal push so the enemy settles in ~0.25s.
            e.velocity.x *= 0.85f;
            e.velocity.z *= 0.85f;
            if (e.knockbackTimer <= 0.0f) { e.velocity.x = 0.0f; e.velocity.z = 0.0f; }
        }

        // The Dungeon Engine superboss must be immune to DoT while shielded, exactly like direct
        // hits (Combat::applyDamage). Without this, poison/burn would bleed the shielded Engine and
        // could even tick it to 0 mid-wave — firing VICTORY while its adds are still alive. Timers
        // still count down (the debuff just deals no damage during the shield window).
        bool engineShielded = e.isEngine && Combat::engineShieldActive(pool, static_cast<u16>(i));

        // Tick status effects (poison/burn deal DoT, freeze checked at move time)
        if (e.poisonTimer > 0.0f) {
            e.poisonTimer -= dt;
            if (!engineShielded) e.health -= e.poisonDps * dt;
            // Route death through killEntity so DoT kills still drop loot / fire procs, and credit
            // the player who applied the poison (mana-on-kill / loot / kill-feed) via the global
            // attacker slot — the shared tick reset it to 0xFF, so set it just for this kill.
            if (e.health <= 0.0f) {
                Combat::setAttackingPlayer(e.poisonSrcSlot);
                Combat::killEntity(pool, {static_cast<u16>(i), e.generation});
                Combat::setAttackingPlayer(0xFF);
            }
        }
        if (e.burnTimer > 0.0f) {
            e.burnTimer -= dt;
            if (!engineShielded) e.health -= e.burnDps * dt;
            if (e.health <= 0.0f) {
                Combat::setAttackingPlayer(e.burnSrcSlot);
                Combat::killEntity(pool, {static_cast<u16>(i), e.generation});
                Combat::setAttackingPlayer(0xFF);
            }
        }
        if (e.freezeTimer > 0.0f) e.freezeTimer -= dt;
        if (e.stunTimer > 0.0f) e.stunTimer -= dt;
        // Mark Prey: tick down and reset damage multiplier when expired
        if (e.markPreyTimer > 0.0f) {
            e.markPreyTimer -= dt;
            if (e.markPreyTimer <= 0.0f) { e.markPreyTimer = 0.0f; e.markPreyDmgMult = 1.0f; }
        }

        if (e.flags & ENT_DEAD) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) {
                e.flags = 0;
                pool.freeList[pool.freeCount++] = static_cast<u16>(i);
                // Swap-remove from active list
                pool.activeList[a] = pool.activeList[pool.activeCount - 1];
                pool.activeCount--;
                continue; // re-check same index
            }
        }
        a++;
    }
}

u32 EntitySystem::activeCount(const EntityPool& pool) {
    return pool.activeCount;
}

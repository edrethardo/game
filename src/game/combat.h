#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/weapon.h"
#include "world/level_grid.h"

struct Player;
struct ProjectilePool;
struct ParticlePool;
struct ScreenShake;

// Result of a player attack. Melee can hit multiple entities (cone query);
// hitscan hits first entity or wall (raycast); projectile spawns are fire-and-forget.
// Combat hit result from last player attack (for feedback)
// M10.2: AttackResult now carries per-hit entity handles so the server can emit
// SV_DAMAGE_DONE for each confirmed hit. entitiesHit is the count; hitHandles[0]
// is always the primary (hitscan/melee first hit); MAX_ATTACK_HITS bounds the array.
static constexpr u32 MAX_ATTACK_HITS = 16;

struct AttackResult {
    bool  didFire     = false;
    bool  hitEntity   = false;
    bool  hitWorld    = false;
    Vec3  hitPosition = {0,0,0};
    Vec3  hitNormal   = {0,0,0};
    f32   hitDistance  = 0.0f;
    u32   entitiesHit = 0; // melee can hit multiple
    // M10.2: entity handles for each confirmed hit (populated by fireMelee / fireHitscan)
    EntityHandle hitHandles[MAX_ATTACK_HITS] = {};
};

namespace Combat {
    // The Dungeon Engine secret superboss is damage-immune while any wave-boss it summoned is alive
    // (clear the adds to crack its shield). Returns true iff some active entity carries
    // spawnerIdx == engineIdx and isn't dead. Factored out of applyDamage so it is independently
    // testable. IMPORTANT: this is keyed on spawnerIdx, NOT isEngine/isBoss — the summoned wave-adds
    // also carry isBoss/bossDefIdx, so the caller must gate this on target.isEngine (only the Engine
    // is shielded; its adds are always damageable). See updateEngineBoss + ~/.claude/plans.
    inline bool engineShieldActive(const EntityPool& pool, u16 engineIdx) {
        for (u32 a = 0; a < pool.activeCount; a++) {
            const Entity& m = pool.entities[pool.activeList[a]];
            if (m.spawnerIdx == engineIdx && !(m.flags & ENT_DEAD)) return true;
        }
        return false;
    }

    // Apply damage to an entity. Handles health, flash, death transition.
    // Optional damageOrigin enables directional checks (e.g. shield bearer frontal reduction).
    void applyDamage(EntityPool& pool, EntityHandle target, f32 damage,
                     const Vec3* damageOrigin = nullptr, bool isCrit = false);

    // Transition an entity to its death state and fire the death callback (loot
    // drop, squad alert, death procs). Use this for damage that bypasses
    // applyDamage — environmental/DoT sources (poison, burn, scorch) — so those
    // kills still drop loot. No-op if the entity is already dead. Does NOT spawn a
    // damage number (DoTs tick every frame and would otherwise spam).
    void killEntity(EntityPool& pool, EntityHandle target);

    // How a hit interacted with the shield. PERFECT (raise-to-hit < 0.2s) negates all damage
    // and triggers the legendary-shield effects; BLOCKED halves it. Pure — the single source
    // for the window, consumed by applyDamageToPlayer and pinned by test_block_outcome.cpp.
    enum struct BlockOutcome : u8 { NONE, BLOCKED, PERFECT };
    inline BlockOutcome classifyBlock(bool blocking, f32 blockTimer) {
        if (!blocking) return BlockOutcome::NONE;
        return (blockTimer < 0.2f) ? BlockOutcome::PERFECT : BlockOutcome::BLOCKED;
    }

    // Apply damage to the player. Optional attackerPos enables directional indicator.
    // attackerIdx: entity pool index of the attacker (0xFFFF if unknown/environmental).
    // Used by dodge-through detection to fire riposte counter-hits, and stamped onto the player so
    // thorns can reflect at the actual attacker.
    // Returns how the shield handled the hit (NONE for i-frame/deflect early-outs too), so the
    // projectile path can reflect on PERFECT (Mirror Aegis) instead of consuming the shot.
    BlockOutcome applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos = nullptr,
                                     u16 attackerIdx = 0xFFFF);

    // Defensive pack — armor rating → fraction of incoming damage mitigated, on a diminishing-
    // returns curve `armor / (armor + 100)` hard-capped at 0.80. Pure function (no engine state),
    // exposed for unit testing. 100 armor = 50% reduction; the cap stops stacked armor reaching
    // invulnerability. Negative/zero armor yields 0.
    // Header-inline so tests-only code (the balance lab's EHP conversion) links the real curve
    // without dragging combat.cpp's entity/projectile world into the test binary.
    inline f32 armorMitigation(f32 armor) {
        if (armor <= 0.0f) return 0.0f;
        f32 mit = armor / (armor + 100.0f); // diminishing returns: 100 armor = 50%
        return (mit > 0.80f) ? 0.80f : mit;  // hard cap so a stacked build can't become invulnerable
    }

    // The three crowd-control kinds the CC-Resistance stat governs (poison/burn/curse are damage,
    // not CC, and keep their direct timer writes).
    enum struct CcType : u8 { STUN, SLOW, FREEZE };
    // The SINGLE entry point for applying crowd control to a player. Applies tenacity
    // (player.ccResist), an immunity / dodge-i-frame negate, and PvP-only stun diminishing returns
    // (via CrowdControl::resolveCC), then raises the chosen timer. Every CC source (enemy hits,
    // projectile onHit, arena PvP) MUST route through this — a direct timer write bypasses resist
    // and DR and re-creates the perma-lock the ladder exists to prevent. isPvp gates the stun DR.
    void applyCCToPlayer(Player& p, CcType type, f32 duration, bool isPvp);

    // --- PvP (Arena mode, sentinel floor 97) ------------------------------------------------
    // The engine registers the tick's combatants here while the arena's authoritative window is
    // open (Engine::arenaBeginPvpWindow / arenaEndPvpWindow); the list is EMPTY everywhere else,
    // so every pvp* helper below is a free no-op in PvE — call sites need no arena checks beyond
    // an optional pvpActive() guard for clarity. Views must stay valid for the whole window
    // (remote players are seeded Player views, written back to their NetPlayers at window end).
    // Every landed hit goes through applyDamageToPlayer — block/perfect-block/armor/i-frames all
    // work against players — and stamps view->lastHitByPlayerSlot for arena kill credit.
    struct PvpTarget { Player* view; u8 slot; };
    void setPvpTargets(const PvpTarget* targets, u32 count);   // nullptr/0 clears
    bool pvpActive();
    // Raw registry access for call sites that resolve their own geometry (the projectile AABB
    // test lives in projectile.cpp beside tryHitPlayer, its enemy-projectile twin).
    const PvpTarget* pvpTargets(u32& countOut);
    // A landed PvP hit is applied ATOMICALLY by the engine (Engine::pvpApplyHit): for a remote
    // victim it seeds a FRESH view, lands the hit, and writes straight back to the NetPlayer.
    // The registry views above are geometry snapshots only — never written back — because other
    // systems (remote activations, the shared AI/projectile view pass) run their own seed→
    // writeback cycles inside the PvP window, and a held view written back later would rewind
    // their position/heal updates. newHealth lets the caller refresh its snapshot.
    struct PvpHit {
        f32  damage;
        Vec3 origin;
        u8   attackerSlot;
        bool projectile;     // gates Wanderer Deflect (absorbs projectiles only)
        u8   onHitEffect;    // 0 none, 1 poison, 2 slow, 3 burn, 4 freeze, 5 stun (class-CC sources)
        f32  onHitDuration;
        f32  stunDuration = 0.0f;  // onHitEffect==5: resisted + DR'd on the victim (applyCCToPlayer)
        f32  knockback    = 0.0f;  // Marksman Explosive Round: displacement impulse (NOT a timed CC)
    };
    struct PvpHitOutcome {
        BlockOutcome block     = BlockOutcome::NONE;
        bool         deflected = false;
        f32          newHealth = 0.0f;
    };
    using PvpApplyFn = PvpHitOutcome(*)(u8 slot, const PvpHit& hit);
    void setPvpApply(PvpApplyFn fn);
    PvpHitOutcome pvpApply(u8 slot, const PvpHit& hit);   // forwards to the engine fn
    // Weapon geometry vs players. attackerSlot is passed EXPLICITLY (never inferred from
    // getAttackingPlayer — the ambient value is only maintained on projectile/skill paths).
    // pvpCone: horizontal melee cone, one shared crit roll (the fireMelee convention).
    // pvpRay: nearest player AABB along the ray, wall-occluded via the grid DDA; outHitPos
    // receives the impact point for tracer/spark FX. Both return whether anyone was hit.
    u32  pvpCone(const WeaponDef& weapon, Vec3 origin, Vec3 forward, const LevelGrid& grid,
                 u8 attackerSlot);
    bool pvpRay(const WeaponDef& weapon, Vec3 origin, Vec3 forward, const LevelGrid& grid,
                u8 attackerSlot, Vec3* outHitPos);
    // Area damage vs players (skill AoE sites: meteors, novas, dashes, splash). No LOS gate —
    // matches the entity AoE queries, and cover already limits who stands in the blast.
    // attackerSlot 0xFF = "use getAttackingPlayer()" (the skill paths maintain it).
    u32  pvpRadius(Vec3 center, f32 radius, f32 damage, u8 attackerSlot = 0xFF);
    // Area CROWD CONTROL / knockback vs players (the class-CC AoE sites: Ranger slow zone, Tinkerer
    // EMP stun, Marksman knockback blast). Lands a templated PvpHit — its onHitEffect/onHitDuration/
    // stunDuration/knockback (and any damage) — on every rival player in range. The proto's origin is
    // overridden to `center` so knockback pushes outward. proto.attackerSlot 0xFF = getAttackingPlayer.
    u32  pvpRadiusHit(Vec3 center, f32 radius, const PvpHit& proto);

    // Dodge-through callback: called when damage is blocked during a dodge roll
    // `victim` is the player who dodged — the local Player OR a server-side remote view
    // (identity via Player::netSlot). The old (attackerIdx, attackerPos)-only shape forced the
    // handler to assume m_localPlayer, so a guest's dodge-through fired the HOST's riposte and
    // fed the HOST's adrenaline.
    using DodgeThroughCallback = void(*)(Player& victim, u16 attackerIdx, Vec3 attackerPos);
    void setDodgeThroughCallback(DodgeThroughCallback cb);

    // Execute a melee attack (cone check, damage all in cone).
    // Crit is rolled internally from weapon.critChance — see combat.cpp.
    // grid is the LOS gate: the swing can't reach through a wall/floor/ceiling.
    AttackResult fireMelee(const WeaponDef& weapon,
                           Vec3 eyePos, Vec3 forward,
                           const LevelGrid& grid,
                           EntityPool& pool);

    // Execute a hitscan attack (raycast, damage first entity hit).
    AttackResult fireHitscan(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             const LevelGrid& grid,
                             EntityPool& pool);

    // Spawn a projectile. Returns pool slot index (0xFFFF if full).
    u16 fireProjectile(const WeaponDef& weapon,
                       Vec3 eyePos, Vec3 forward,
                       ProjectilePool& projectiles,
                       u8 extraFlags = 0);

    // Spawn a projectile with gravity and/or splash behavior (for molotov etc.)
    u16 fireProjectile(const WeaponDef& weapon,
                       Vec3 eyePos, Vec3 forward,
                       ProjectilePool& projectiles,
                       f32 gravity, f32 splashRadius, f32 splashDamage);

    // Damage number callback — auto-fires on every applyDamage call.
    // isCrit/isKill are threaded through so the renderer can style those hits distinctly.
    using DamageNumberCallback = void(*)(Vec3 position, f32 amount, bool isCrit, bool isKill);
    void setDamageNumberCallback(DamageNumberCallback cb);
    // Manually spawn a floating damage number (for skills that bypass applyDamage).
    // Crits are not applicable at this call site, so isCrit/isKill are always false.
    void spawnDamageNumber(Vec3 position, f32 amount);

    // Death callback — called when an entity dies, before pool cleanup
    using DeathCallback = void(*)(EntityPool& pool, u16 entityIndex, Vec3 position);
    void setDeathCallback(DeathCallback cb);

    // (L8) Player slot currently credited as the attacker. The engine sets it around each
    // player's weapon fire (and projectile.cpp restores each projectile's ownerSlot around
    // its damage); killEntity stamps it onto Entity::killerSlot so loot can be reserved to
    // the killer. 0xFF = none/environmental (free-for-all drop).
    void setAttackingPlayer(u8 slot);

    // The last enemy this player damaged. Feeds the target health bar's fallback: you keep seeing
    // what you just hit even after your crosshair drifts off it (Diablo 2 behaviour). Returns an
    // invalid handle if that player hasn't hit anything.
    // Recorded inside applyDamage — the one point every player-sourced hit funnels through, so a
    // new damage source can't forget to report itself.
    EntityHandle getLastHitEntity(u8 slot);
    void         clearLastHitEntities();   // on floor change: last floor's target must not persist
    u8   getAttackingPlayer();

    // D1.1 — Weapon mesh ID of the currently active weapon, for kill-event attribution.
    // Set by the engine around each weapon-fire pass (mirrors the setAttackingPlayer pattern).
    // 0 = unknown/unarmed. Only the SERVER emits SV_KILL so non-server callers can leave it 0.
    void setKillWeaponMeshId(u8 meshId);
    u8   getKillWeaponMeshId();

    // D1.1 — Kill callback, fired from killEntity with full attribution context.
    // killerSlot: net slot of attacker (0xFF = environmental).
    // victimType: 0=entity, 1=player (this overload always fires with victimType=0).
    // victimIdx: entity pool index.
    // weaponMeshId: from getKillWeaponMeshId() at time of kill.
    // isCrit: whether the killing blow was a critical hit.
    using OnKillFn = void(*)(u8 killerSlot, u8 victimType, u16 victimIdx,
                             u8 weaponMeshId, u8 isCrit);
    void setOnKill(OnKillFn fn);

    // Perfect block callback — fired for the BLOCKER (local player on host/SP, or a server-side
    // remote view: identity via Player::netSlot, shield via Player::offhandSkill). attackerIdx
    // is the striking entity (0xFFFF for projectiles/AoE), so ripostes can aim at it.
    using PerfectBlockCallback = void(*)(Player& player, u16 attackerIdx);
    void setPerfectBlockCallback(PerfectBlockCallback cb);

    // Wire in the particle pool and screen shake so combat events emit visual FX.
    // Both pointers are stored as file-scope statics in combat.cpp.
    void setFXTargets(ParticlePool* particles, ScreenShake* shake);
    // The authoritative entity pool, so applyDamageToPlayer (which only gets an attackerIdx) can
    // reach the attacker — needed by the VAMPIRIC champion affix. Set once from Engine::init.
    void setEntityPool(EntityPool* pool);
}

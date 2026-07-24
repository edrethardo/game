// engine_autoplay.cpp — the Autoplay ENGINE DRIVER: the spine that makes the armed bot come alive.
//
// The pure decision core (game/autoplay_*.h + autoplay_brain.cpp) is engine-free and unit-tested;
// this file is the ONLY place it touches live Engine state. Once per sim tick gameUpdate calls
// Engine::updateAutoplay, which:
//   1. maintains the human/bot takeover latch (AutoplayControl) — a real gameplay keypress grabs
//      control instantly, the bot resumes after an idle window; UI navigation never counts;
//   2. when the bot holds control (and no hard-freeze UI is up), builds a read-only Autoplay::BotView
//      snapshot of the player / weapon / nav flow-field / hostiles from the live engine, then
//   3. runs the pure Autoplay::decide(view) and applies the returned BotIntent as a yaw/pitch write
//      plus synthetic held GameActions via the Input overlay.
//
// The bot IS synthetic input: because every action flows through the SAME consumers a human's keys
// drive (PlayerController movement + jump assist, handleWeaponFire, the skill/potion/block gates,
// updateFloorDoor's descend), every existing system works unchanged — no bot-specific combat code.
//
// SCOPE (Task 8a): flat BSP/cavern/hub floors. The flow field IS the travel goal here; per-style
// vertical routing (ramps / drop-holes / catwalks / lava causeways) and low-hp globe detours are the
// follow-up (8b) — until then flowDir is the raw flow-field heading and the globe list stays empty.
#include "engine/engine.h"
#include "platform/input.h"
#include "world/combat_query.h"
#include "world/level_grid.h"
#include "game/autoplay_nav.h"   // Autoplay::stepAllowed — the travel hazard veto
#include "game/game_constants.h"
#include <cmath>

// How many nearest hostiles the driver hands the brain each tick (pickTarget scans this small set).
static constexpr u32 kMaxTargets = 16;

// Rotate a flat (XZ) heading by `a` radians about +Y. Used by the hazard veto to try ±45° detours
// around a wall/lava/edge cell the raw flow heading would step into.
static Vec3 rotateY_XZ(Vec3 v, f32 a) {
    const f32 c = cosf(a), s = sinf(a);
    return Vec3{v.x * c - v.z * s, 0.0f, v.x * s + v.z * c};
}

// One tick of the Autoplay driver. Called from gameUpdate BEFORE the input-consuming blocks so the
// bot's yaw + held actions are already set when PlayerController / fire / skills read them.
void Engine::updateAutoplay(f32 dt) {
    if (!m_autoplayActive) return;

    // Takeover latch. Activity while a blocking UI is open must NOT grab control (browsing the build
    // in the inventory is the whole point of "keep fighting while I re-gear"), so uiOpen mirrors
    // gameplayInputFrozen()'s screen set and is passed to the latch, which freezes on it.
    const bool uiOpen = m_inventoryOpen || m_characterScreenOpen || m_menu.confirmQuit
                     || m_menu.optionsFromPause || m_menagerieOpen;
    m_autoplayControl.tick(Input::humanActivityThisFrame(), uiOpen, dt);

    // Human is driving (or resuming window still counting down): drop any synthetic held actions so
    // the real device is the only input, and get out of the way.
    if (!m_autoplayControl.botInControl()) { Input::clearBotHeld(); return; }
    // Hard-freeze UI up (pause / character inspect / options / menagerie): the world is frozen in SP
    // and the bot must not act. botMayAct() already excludes those but still allows the inventory.
    if (!botMayAct()) { Input::clearBotHeld(); return; }

    Autoplay::BotView v = buildBotView();
    Autoplay::BotIntent in = Autoplay::decide(v);
    applyBotIntent(in);
}

// Fill the read-only decision snapshot from live engine state (lane 0 — the only Autoplay lane).
Autoplay::BotView Engine::buildBotView() {
    Autoplay::BotView v{};

    // --- self ---
    v.pos       = m_localPlayer.position;
    v.yaw       = m_localPlayer.yaw;
    v.pitch     = m_localPlayer.pitch;
    v.eyeHeight = m_localPlayer.eyeHeight;
    v.hp        = m_localPlayer.health;
    v.maxHp     = m_localPlayer.maxHealth;
    v.energy    = m_skillStates[m_localPlayerIndex].energy;
    v.maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;
    v.stunned   = m_localPlayer.stunTimer > 0.0f;
    v.rolling   = m_localPlayer.dodgeState.rolling;
    v.onGround  = m_localPlayer.onGround;
    v.dodgeCooldown = m_localPlayer.dodgeState.cooldownTimer;

    // potionReady — replicate the tick-based gate the potion heal itself uses (engine_update.cpp) so
    // the bot only asks to drink when the press would actually fire.
    {
        const f32 cdr    = m_inventories[m_localPlayerIndex].bonusCooldownReduction * 0.1f;
        const u32 cdTk   = static_cast<u32>(GameConst::POTION_COOLDOWN * (1.0f - cdr) * 60.0f + 0.5f);
        v.potionReady    = GameConst::cooldownReady(currentLocalTick(), m_potionLastActivationTick, cdTk);
    }

    // --- weapon (effective, incl. affixes) ---
    // Mirror getEffectiveWeapon; MELEE/HITSCAN carry no projectile lead (projSpeed 0), only PROJECTILE.
    const WeaponDef w = Inventory::getEffectiveWeapon(m_inventories[0], m_itemDefs, m_weaponDefs[0]);
    v.weaponRange     = w.range;
    v.weaponProjSpeed = (w.type == WeaponType::PROJECTILE) ? w.projectileSpeed : 0.0f;
    v.weaponIsMelee   = (w.type == WeaponType::MELEE);
    v.buildCell       = m_inventories[0].buildCell;

    // --- world gate: idle in town / arena / the Source, and only travel while an ordinary exit exists ---
    v.onNormalFloor = !(m_level.inTown || m_level.inArena || m_level.inSourceChamber) && m_level.floorDoorActive;

    // --- nav: flow field toward the exit ---
    // flowDirection returns {0,0,0} both at the exit AND on an unreachable cell; the raw flow byte
    // disambiguates (0xFE = at exit, 0xFF = unreachable) so the brain can tell "arrived" from "stuck".
    v.flowDir   = LevelGridSystem::flowDirection(m_level.grid, m_localPlayer.position);
    v.flowValid = false;
    v.atExit    = false;
    {
        u32 gx, gz;
        if (m_level.grid.flowDir &&
            LevelGridSystem::worldToGrid(m_level.grid, m_localPlayer.position, gx, gz)) {
            const u8 byte = m_level.grid.flowDir[gz * m_level.grid.width + gx];
            v.atExit    = (byte == 0xFE);
            v.flowValid = (byte != 0xFF);
        }
    }
    // Hazard veto on the TRAVEL heading (8a flat-floor safety): never let the flow step the bot into a
    // wall, off the map, or grounded into lava. Try the flow heading first, then ±45°, else stop.
    if (lengthSq(v.flowDir) > 1e-6f) {
        const f32 feetY = m_localPlayer.position.y;
        if (!Autoplay::stepAllowed(m_level.grid, v.pos, feetY, v.flowDir, m_level.lavaFloor)) {
            const Vec3 left  = rotateY_XZ(v.flowDir,  0.7853981634f);   // +45°
            const Vec3 right = rotateY_XZ(v.flowDir, -0.7853981634f);   // -45°
            if      (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, left,  m_level.lavaFloor)) v.flowDir = left;
            else if (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, right, m_level.lavaFloor)) v.flowDir = right;
            else v.flowDir = Vec3{0, 0, 0};   // boxed in: stop (8b adds smarter recovery)
        }
    }

    // --- descend gate context (consumed by the brain's mayDescend mirror) ---
    v.doorActive  = m_level.floorDoorActive;
    v.distToDoor  = length(m_level.floorDoorPos - m_localPlayer.position);
    v.hasBoss     = m_level.floorHasBoss;
    v.bossAlive   = floorBossAlive();

    // --- targets: nearest-first hostiles with width-aware LOS from the bot's eye ---
    static Autoplay::BotTarget s_targets[kMaxTargets];
    const Vec3 eye = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    u32 n = 0;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        // Same skip set CombatQuery uses (combat_query.cpp) — dead / friendly / props / burrowed.
        if (e.flags & ENT_DEAD)     continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        if (e.flags & ENT_BURROWED) continue;

        Autoplay::BotTarget t{};
        t.pos    = e.position;               // AABB centre (aim point)
        t.vel    = Vec3{e.velocity.x, 0.0f, e.velocity.z};   // XZ only, for projectile lead
        t.dist   = length(e.position - eye);
        t.hp     = e.health;
        t.isBoss = e.isBoss;
        // LOS: a WORLD hit before the target's centre blocks it (the DDA is slab-aware, so a balcony
        // floor occludes an enemy above). An entity/floor hit at/after the centre does not.
        const Vec3 toT = e.position - eye;
        const f32  d   = length(toT);
        if (d < 1e-4f) {
            t.hasLOS = true;   // on top of it
        } else {
            CombatHit hit = CombatQuery::raycast(m_level.grid, m_entities, eye, normalize(toT), d);
            t.hasLOS = !(hit.hit && hit.type == CombatHit::WORLD);
        }

        // Insert nearest-first into the fixed cap (simple insertion — the pool is small).
        u32 pos = n;
        if (n < kMaxTargets) n++;
        else if (t.dist >= s_targets[kMaxTargets - 1].dist) continue;   // full + farther: drop
        else pos = kMaxTargets - 1;
        while (pos > 0 && s_targets[pos - 1].dist > t.dist) { s_targets[pos] = s_targets[pos - 1]; pos--; }
        s_targets[pos] = t;
    }
    v.targets     = s_targets;
    v.targetCount = n;

    // --- globes/pickups: 8b (low-hp detours). Empty for 8a. ---
    v.globes     = nullptr;
    v.globeCount = 0;
    return v;
}

// Translate one BotIntent into a yaw/pitch write + synthetic held GameActions. Clears last tick's
// held set first (so a no-longer-wanted action releases), then arms exactly this tick's actions.
void Engine::applyBotIntent(const Autoplay::BotIntent& in) {
    Input::clearBotHeld();

    m_localPlayer.yaw   = in.aimYaw;
    // Clamp pitch to the same ±89° applyMovement enforces (a straight-down/up aim would gimbal look).
    constexpr f32 kMaxPitch = 89.0f * 3.14159265f / 180.0f;
    f32 pitch = in.aimPitch;
    if (pitch >  kMaxPitch) pitch =  kMaxPitch;
    if (pitch < -kMaxPitch) pitch = -kMaxPitch;
    m_localPlayer.pitch = pitch;

    Input::setBotHeld(GameAction::MOVE_FORWARD,  in.moveFwd);
    Input::setBotHeld(GameAction::MOVE_BACKWARD, in.moveBack);
    Input::setBotHeld(GameAction::MOVE_LEFT,     in.moveLeft);
    Input::setBotHeld(GameAction::MOVE_RIGHT,    in.moveRight);
    Input::setBotHeld(GameAction::JUMP,   in.jump);
    Input::setBotHeld(GameAction::FIRE,   in.fire);
    Input::setBotHeld(GameAction::BLOCK,  in.block);
    Input::setBotHeld(GameAction::DODGE,  in.dodge);
    Input::setBotHeld(GameAction::POTION, in.potion);
    Input::setBotHeld(GameAction::RELOAD, in.reload);
    // Class skill: select the slot (SKILL_n) AND press CLASS_SKILL on the same tick — the selection
    // loop runs before the activation in handleClassSkillActivation, so both land in one call.
    if (in.classSkillSlot >= 0) {
        const GameAction slot = static_cast<GameAction>(
            static_cast<u8>(GameAction::SKILL_1) + static_cast<u8>(in.classSkillSlot));
        Input::setBotHeld(slot, true);
        Input::setBotHeld(GameAction::CLASS_SKILL, true);
    }
    Input::setBotHeld(GameAction::BOOT_SKILL,   in.bootSkill);
    Input::setBotHeld(GameAction::HELMET_SKILL, in.helmetSkill);

    // Descend: HOLD the interact button (PICKUP), exactly as a human does at the exit. A direct
    // m_descendRequested write is useless here — updatePlayerPickup (which runs later this tick, before
    // updateFloorDoor) RESETS the flag and re-derives it from the PICKUP button's tap/hold arbitration,
    // so the flag has to come through that button. The exit is a HOLD target (loot wins a tap), and the
    // brain holds in.descend every tick at the door, so after INTERACT_HOLD_SEC the hold fires and
    // updateFloorDoor descends. (in.interact — globe/chest taps — is an 8b concern; unused here.)
    Input::setBotHeld(GameAction::PICKUP, in.descend);
}

// Disarm the bot when a run ends to the menu — immediate so the synthetic-input overlay is not left
// armed under the menu (a stale held action could otherwise leak into menu navigation). The main-menu
// confirm reset also clears m_autoplayActive; this covers the in-game quit / death-quit / victory exits.
void Engine::exitAutoplayRun() {
    m_autoplayActive = false;
    Input::setBotOverlayActive(false);   // also clears any held synthetic actions (input.cpp)
    m_autoplayRespawnTimer = 0.0f;
}

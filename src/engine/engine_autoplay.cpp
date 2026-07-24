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
// SCOPE (Task 8b): flat floors ride the raw flow field (8a); on STACKED styles buildBotView folds the
// per-style vertical goal into flowDir BEFORE the hazard veto — a VERTICAL_HALL bot climbs the
// diagonal-corner ramp to the opposite-story exit balcony, a FOUR_STORY "Descent" bot steers to the
// nearest same-story drop-hole and falls toward L0, and lava floors lean on the (lava-aware) veto to
// hug the stone causeways. Three driver backstops ride on top: an anti-livelock stuck-override (force
// the descend when wedged at a contested door; a lateral nudge when wedged on geometry), a loot-settle
// dwell (hold briefly after a fight so the auto-loot vacuum collects), and low-hp health-globe detours.
#include "engine/engine.h"
#include "platform/input.h"
#include "world/combat_query.h"
#include "world/level_grid.h"
#include "world/story_nav.h"      // StoryNav::onUpperStory / nearestPortalGoal — per-style vertical routing
#include "game/autoplay_nav.h"    // Autoplay::stepAllowed — the travel hazard veto
#include "game/autoplay_combat.h" // Autoplay::dirToAim / doctrineFor — nudge heading + in-band fight test
#include "game/item.h"            // GLOBE_HEALTH_ID / m_worldItems — low-hp globe detours
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
    //
    // CRUCIALLY it ALSO includes the UI-toggle press EDGES this frame. updateAutoplay runs BEFORE the
    // inventory-toggle handler flips m_inventoryOpen later in the same gameUpdate, so on the frame a
    // human taps Tab: m_inventoryOpen is still false but humanActivityThisFrame() is true (Tab down).
    // Without the edge terms the latch would read {open=false, active=true} and hand control to the
    // human — then the inventory opens and freezes the latch there, so control never returns and the
    // "fight while I re-gear" carve-out could never engage. Counting the toggle key AS the UI (the bot
    // never presses these in 8a, so the reads reflect the human) keeps the bot in control across the
    // open/close, and the latch stays frozen (bot-controlled) while the screen is up.
    const bool uiOpen = m_inventoryOpen || m_characterScreenOpen || m_menu.confirmQuit
                     || m_menu.optionsFromPause || m_menagerieOpen
                     || Input::isActionPressed(GameAction::INVENTORY)
                     || Input::isActionPressed(GameAction::CHARACTER_SCREEN)
                     || Input::isActionPressed(GameAction::PAUSE);
    m_autoplayControl.tick(Input::humanActivityThisFrame(), uiOpen, dt);

    // Human is driving (or resuming window still counting down): drop any synthetic held actions so
    // the real device is the only input, and get out of the way.
    if (!m_autoplayControl.botInControl()) { Input::clearBotHeld(); return; }
    // Hard-freeze UI up (pause / character inspect / options / menagerie): the world is frozen in SP
    // and the bot must not act. botMayAct() already excludes those but still allows the inventory.
    if (!botMayAct()) { Input::clearBotHeld(); return; }

    Autoplay::BotView v = buildBotView();
    Autoplay::BotIntent in = Autoplay::decide(v);

    // --- 8b driver backstops applied on top of the pure decision -----------------------------------
    // (1) LOOT-SETTLE dwell. When a fight just ended (hostile count fell to zero), hold position for a
    // beat so the auto-loot vacuum can sweep the drops before the bot walks off them. We only gate the
    // forward move; the vacuum/equip/prune are existing systems. Armed on the >0->0 edge, capped ~3 s.
    if (v.targetCount == 0 && m_autoplayLastTargetCount > 0)
        m_autoplayLootDwell = fminf(m_autoplayLootDwell + 1.5f, 3.0f);
    if (m_autoplayLootDwell > 0.0f) m_autoplayLootDwell -= dt;
    m_autoplayLastTargetCount = v.targetCount;
    if (m_autoplayLootDwell > 0.0f && v.targetCount == 0)
        in.moveFwd = in.moveBack = in.moveLeft = in.moveRight = false;   // dwell: let loot settle

    // (2) STUCK detection (anti-livelock backstop; should almost never fire in normal play). Progress
    // is XZ distance from a re-anchored point. It only accrues while the bot is NOT locked in an
    // in-band fight (a bot dancing around an in-range target is working, not wedged) and not dwelling —
    // so an unreachable OUT-of-band straggler at the exit still lets the timer climb (the livelock).
    {
        const Vec3 p  = m_localPlayer.position;
        const f32  dx = p.x - m_autoplayLastPos.x, dz = p.z - m_autoplayLastPos.z;
        const bool progressed = (dx * dx + dz * dz) > 0.25f;   // > 0.5 m from the anchor
        const Autoplay::Doctrine doc = Autoplay::doctrineFor(v.buildCell);
        bool inBandFight = false;
        for (u32 i = 0; i < v.targetCount; i++) {
            const Autoplay::BotTarget& t = v.targets[i];
            if (t.hasLOS && t.dist >= doc.engageMin * v.weaponRange &&
                            t.dist <= doc.engageMax * v.weaponRange) { inBandFight = true; break; }
        }
        if (progressed) { m_autoplayLastPos = p; m_autoplayNoProgressTimer = 0.0f; }
        else if (!inBandFight && m_autoplayLootDwell <= 0.0f) m_autoplayNoProgressTimer += dt;
    }
    const bool stuck = m_autoplayNoProgressTimer > 4.0f;

    // Remedy A (priority) — exit-loiter livelock: wedged near a contested door with the boss dead. An
    // unreachable LOS straggler keeps FIGHT active but the bot can't close, so force the descend (hold
    // PICKUP, drop fire/move) — the interact-hold completes over the next few ticks and we leave.
    const bool bossGate = v.hasBoss && v.bossAlive;
    if (stuck && v.doorActive && v.distToDoor < 2.5f && !bossGate) {
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        in.descend = true;
    } else if (stuck || m_autoplayNudgeTimer > 0.0f) {
        // Remedy B — wedged on geometry elsewhere: nudge laterally for ~0.5 s, then re-evaluate.
        if (stuck && m_autoplayNudgeTimer <= 0.0f) m_autoplayNudgeTimer = 0.5f;
        if (m_autoplayNudgeTimer > 0.0f) {
            m_autoplayNudgeTimer -= dt;
            // Base heading: the travel heading if we have one, else the bot's facing. Rotate to a
            // lateral/back direction and take the first whose one-cell step is hazard-safe.
            Vec3 base = v.flowDir;
            if (lengthSq(base) < 1e-6f)
                base = Vec3{-sinf(m_localPlayer.yaw), 0.0f, -cosf(m_localPlayer.yaw)};
            const f32 feetY = m_localPlayer.position.y;
            const f32 kAngles[3] = {1.5707963f, -1.5707963f, 3.14159265f};   // +90°, -90°, 180°
            Vec3 esc{0, 0, 0};
            for (u32 i = 0; i < 3; i++) {
                const Vec3 cand = rotateY_XZ(base, kAngles[i]);
                if (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, cand, m_level.lavaFloor)) {
                    esc = cand; break;
                }
            }
            if (lengthSq(esc) > 1e-6f) {
                f32 yaw, pitch; Autoplay::dirToAim(esc, yaw, pitch);
                in = Autoplay::BotIntent{};
                in.aimYaw = yaw; in.aimPitch = 0.0f; in.moveFwd = true;
            }
        }
    }

    // (3) DESCEND PICKUP PULSE. The exit is a HOLD target, but a HOLD reaches a SHRINE sharing the
    // exit's interact range FIRST; the bot holds PICKUP continuously, so Interact::poll fires once
    // (spending the shrine), latches `consumed`, and never re-fires to reach the exit — a permanent
    // wedge. So we release + re-hold in a pulse (autoplay_nav.h descendPulseHeld): one cycle spends
    // the shrine, the next descends. Only bites the descend intent; combat/movement are untouched.
    if (in.descend) {
        m_autoplayDescendPulse += dt;
        if (!Autoplay::descendPulseHeld(m_autoplayDescendPulse)) in.descend = false;   // release beat
    } else {
        m_autoplayDescendPulse = 0.0f;
    }

    applyBotIntent(in, uiOpen);
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

    // --- 8b: low-HP HEALTH-globe detour list (nearest-first) ---
    // When hurt and the potion is on cooldown, list nearby health globes so we can steer over one
    // (3 m walk-over pickup, no action). Only health globes (energy globes don't heal); collected here
    // (before the story/globe steer below) so the steer can consult them. When the potion is ready the
    // brain drinks (SURVIVE beats TRAVEL), so the list is empty and no steer happens.
    static Vec3 s_globes[8];
    static f32  s_globeD2[8];
    u32 gc = 0;
    if (v.hp < v.maxHp * 0.5f && !v.potionReady) {
        for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
            const WorldItem& wi = m_worldItems.items[i];
            if (!wi.active || wi.item.defId != GLOBE_HEALTH_ID) continue;
            const Vec3 to = wi.position - m_localPlayer.position;
            const f32  d2 = lengthSq(to);
            if (d2 > 8.0f * 8.0f) continue;                    // out of detour range
            u32 slot = gc;                                     // nearest-first insertion into the cap
            if (gc < 8) gc++;
            else if (d2 >= s_globeD2[7]) continue;
            else slot = 7;
            while (slot > 0 && s_globeD2[slot - 1] > d2) {
                s_globeD2[slot] = s_globeD2[slot - 1]; s_globes[slot] = s_globes[slot - 1]; slot--;
            }
            s_globeD2[slot] = d2; s_globes[slot] = wi.position;
        }
    }
    v.globes     = (gc > 0) ? s_globes : nullptr;
    v.globeCount = gc;

    // --- 8b: per-style VERTICAL routing folded into flowDir BEFORE the hazard veto ---
    // Flat styles (BSP/CAVERN/GAUNTLET/HUB, non-lava) fall straight through — the flat flow field IS
    // the travel goal and this block is a no-op. Stacked styles can't express "climb that ramp" /
    // "drop through that hole" in a 2D flow byte, so steer the heading toward the right vertical
    // landmark; the veto below still guards the resulting one-cell step.
    {
        const DungeonResult& dg  = m_level.dungeon;
        const Vec3           pos = m_localPlayer.position;
        if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL) {
            // The exit is a balcony door on the OPPOSITE story. On the wrong story, walk to the nearest
            // ramp END on our own story (nearestPortalGoal → the diagonal-corner ramp; plain walking
            // climbs the graduated slab). On the SAME story, keep the flat heading to the door.
            const bool botUpper  = StoryNav::onUpperStory(m_level.grid, pos, pos.y);
            const bool exitUpper = m_level.floorDoorPos.y > 1.5f;
            if (botUpper != exitUpper) {
                const Vec3 goal = StoryNav::nearestPortalGoal(dg, pos, botUpper, exitUpper);
                const Vec3 to{goal.x - pos.x, 0.0f, goal.z - pos.z};
                if (lengthSq(to) > 1e-6f) v.flowDir = normalize(to);
            }
        } else if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) {
            // The Descent: the exit is always DOWN. Steer to the nearest drop-hole whose pierced slab
            // TOP (surfaceY) matches the story the bot's feet are on — a hole it can actually enter.
            // Stepping onto it drops a story (gravity does the rest). No same-story hole → keep the
            // flat heading and reposition along the maze.
            f32  bestD2 = 1e30f;
            Vec3 goal{0, 0, 0};
            for (u8 i = 0; i < dg.dropHoleCount; i++) {
                if (fabsf(dg.dropHoles[i].surfaceY - pos.y) > PLATFORM_STEP_TOLERANCE) continue;
                const f32 dx = dg.dropHoles[i].pos.x - pos.x, dz = dg.dropHoles[i].pos.z - pos.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; goal = dg.dropHoles[i].pos; }
            }
            if (bestD2 < 1e29f) {
                const Vec3 to{goal.x - pos.x, 0.0f, goal.z - pos.z};
                if (lengthSq(to) > 1e-6f) v.flowDir = normalize(to);
            }
        }
        // Lava floors get no vertical goal — the veto below (lava-aware) keeps the bot off the lakes
        // and rides the stone causeways the flat flow field already routes along.

        // Survival first: when low-hp with a globe in reach, override the travel/story heading toward
        // the nearest globe. The brain only consults flowDir in its TRAVEL branch (FIGHT/DESCEND ignore
        // it), so an LOS enemy still takes priority — this only bites when the bot would otherwise just
        // be walking to the exit.
        if (gc > 0) {
            const Vec3 to{s_globes[0].x - pos.x, 0.0f, s_globes[0].z - pos.z};
            if (lengthSq(to) > 1e-6f) v.flowDir = normalize(to);
        }
    }

    // Hazard veto on the (possibly story/globe-steered) TRAVEL heading: never let it step the bot into
    // a wall, off the map, or grounded into lava. Try the heading first, then ±45°, else stop (the
    // driver's stuck-override in updateAutoplay recovers a boxed-in bot).
    if (lengthSq(v.flowDir) > 1e-6f) {
        const f32 feetY = m_localPlayer.position.y;
        if (!Autoplay::stepAllowed(m_level.grid, v.pos, feetY, v.flowDir, m_level.lavaFloor)) {
            const Vec3 left  = rotateY_XZ(v.flowDir,  0.7853981634f);   // +45°
            const Vec3 right = rotateY_XZ(v.flowDir, -0.7853981634f);   // -45°
            if      (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, left,  m_level.lavaFloor)) v.flowDir = left;
            else if (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, right, m_level.lavaFloor)) v.flowDir = right;
            else v.flowDir = Vec3{0, 0, 0};   // boxed in: stop (stuck-override recovers)
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

    // (globes were collected above, before the nav steer that consumes them.)
    return v;
}

// Translate one BotIntent into a yaw/pitch write + synthetic held GameActions. Clears last tick's
// held set first (so a no-longer-wanted action releases), then arms exactly this tick's actions.
// When uiOpen, the movement/nav actions are SUPPRESSED (see below) but combat is kept.
void Engine::applyBotIntent(const Autoplay::BotIntent& in, bool uiOpen) {
    Input::clearBotHeld();

    m_localPlayer.yaw   = in.aimYaw;
    // Clamp pitch to the same ±89° applyMovement enforces (a straight-down/up aim would gimbal look).
    constexpr f32 kMaxPitch = 89.0f * 3.14159265f / 180.0f;
    f32 pitch = in.aimPitch;
    if (pitch >  kMaxPitch) pitch =  kMaxPitch;
    if (pitch < -kMaxPitch) pitch = -kMaxPitch;
    m_localPlayer.pitch = pitch;

    // Movement / jump / interact are SUPPRESSED while a UI screen is open. The inventory cursor nav
    // (engine_inventory.cpp) reads the very same MOVE_* actions via isActionPressed, which merges the
    // bot overlay — so a moving bot would jitter the cursor the human is trying to use. Keeping combat
    // live below means the bot fights IN PLACE under an open inventory ("keep fighting while I re-gear")
    // with no cursor interference.
    Input::setBotHeld(GameAction::MOVE_FORWARD,  in.moveFwd  && !uiOpen);
    Input::setBotHeld(GameAction::MOVE_BACKWARD, in.moveBack && !uiOpen);
    Input::setBotHeld(GameAction::MOVE_LEFT,     in.moveLeft && !uiOpen);
    Input::setBotHeld(GameAction::MOVE_RIGHT,    in.moveRight && !uiOpen);
    Input::setBotHeld(GameAction::JUMP,   in.jump && !uiOpen);
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
    // Also suppressed while a UI is open (a nav/interact action, like movement above).
    Input::setBotHeld(GameAction::PICKUP, in.descend && !uiOpen);
}

// Disarm the bot when a run ends to the menu — immediate so the synthetic-input overlay is not left
// armed under the menu (a stale held action could otherwise leak into menu navigation). The main-menu
// confirm reset also clears m_autoplayActive; this covers the in-game quit / death-quit / victory exits.
void Engine::exitAutoplayRun() {
    m_autoplayActive = false;
    Input::setBotOverlayActive(false);   // also clears any held synthetic actions (input.cpp)
    m_autoplayRespawnTimer = 0.0f;
}

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
// hug the stone causeways.
//
// ANTI-LIVELOCK BACKSTOPS ride on top, in strict priority order, so an unattended bot ALWAYS finishes a
// floor. Progress is defined UNIFORMLY across travel and combat: the bot is making progress if it MOVED
// (> 0.5 m) OR it DEALT DAMAGE. That definition is the whole fix for the shipping bug — the old detector
// exempted any in-band fight outright, so a bot firing at a target it could never kill (cover/doorway/
// elevation blocks the shots even though the LOS raycast to the centre reads clear) suppressed its own
// stuck timer and stood there forever. The ladder:
//   LOOK-BEHIND (3 s, before everything) -> turn 180 deg once per stuck episode. A dormant gargoyle
//                                     wakes ONLY while unobserved and cannot be shot awake, so the bot
//                                     staring at one is a wedge that can never clear (autoplay_nav.h
//                                     LOOK_BEHIND_*). Looking is cheaper than walking, so it goes first.
//   A  wedged AT the exit          -> stand still and force the descend hold INSIDE the real 2 m
//                                     descend radius; between that and 2.5 m, walk the last metre in
//                                     (standing still out there held a button that could never fire).
//   B  wedged on geometry          -> ESCALATING escape (lateral nudge -> 8-direction safe-step search
//                                     away from the wedge -> a short A* leg toward the exit).
//   B2 EXIT BULL                   -> the exit-progress watchdog: the bot is MOVING but never arriving
//                                     (orbiting/spiralling, or kited off the door by a swarm it refuses
//                                     to shoot). Bull to the door A*-routed, firing through bodies, and
//                                     STOP inside the descend radius so the interact-hold can land.
//                                     Ranked below B on purpose — when physically wedged, walking at the
//                                     door only presses into the wall and B never gets to run.
//   C  combat BREAK-OFF            -> a stalled (no-damage) in-band fight: walk past toward the exit when
//                                     a flow heading exists, else strafe around the target while FIRING.
// Plus a loot-settle dwell (hold briefly after a fight so the auto-loot vacuum collects) and low-hp
// health-globe detours. An anti-stall move must never holster the guns while an enemy is in reach — that
// is how the bot once froze for 60 s against two body-blocking enemies it silently refused to shoot.
//
// The driver also owns two pieces of COMBAT MEMORY the pure policy deliberately does not: the bot-side
// DODGE LEASHES (m_autoplayDodgeCd / m_autoplayGapCloseCd — the engine's 1 s dodge cooldown is a balance
// number, and a bot that rolls whenever it is legal reads as panic) and the STICKY TARGET (the engaged
// enemy's identity + how long it has been engaged, so the crosshair stops flipping between similar-range
// hostiles). Both reach the policy as plain booleans/indices on BotView, keeping the brain engine-free.
//
// AIM STEADINESS is a driver concern for the same reason — it is all MEMORY. The bot's camera IS the
// player camera, so a desired aim that jumps is a screen that shakes, and the measurement said the jumps
// were never "jitter" in any single signal: they were the aim's SOURCE changing. Three pieces of state
// answer that, and all three live here: the TARGET LOS GRACE (m_autoplayTargetBlind — a target's LOS
// raycast flickers, and releasing on the flicker threw the brain between FIGHT and TRAVEL ~25 times a
// second), the TRAVEL-HEADING COMMIT (m_autoplayTravelDir/Hold — the flow byte and the detour fan both
// toggle across a cell boundary), and the aim DEADZONE in applyBotIntent. Measured on paired 2-minute
// live runs: mean |per-tick change of the desired yaw| 5.0 deg -> 1.9, applied-yaw direction reversals
// 4.5/s -> 1.2 (marksman); 2.8 -> 2.2 and 1.6/s -> 0.9 (warrior).
#include "engine/engine.h"
#include "core/log.h"            // LOG_INFO — the [TELEM] metrics; included explicitly, not via a
                                 // transitive header (that only compiled locally; CI's chain lacks it)
#include "platform/input.h"
#include <SDL_scancode.h>        // SDL_SCANCODE_H
#include "world/raycast.h"        // Raycast::cast — the WORLD-ONLY (slab-aware) DDA behind the target LOS test
#include "world/level_grid.h"
#include "world/story_nav.h"      // StoryNav::onUpperStory / nearestPortalGoal — per-style vertical routing
#include "world/pathfinder.h"     // Pathfinder::findPath — Stage-3 escape's short A* leg toward the exit
#include "game/autoplay_nav.h"    // Autoplay::stepAllowed / escapeHeading — the travel hazard veto + 8-dir escape
#include "game/autoplay_combat.h" // Autoplay::dirToAim / doctrineFor — nudge heading + in-band fight test
#include "game/combat.h"          // Combat::engineShieldActive — mirror the damage-immune (invulnerable) checks
#include "game/item.h"            // GLOBE_HEALTH_ID / m_worldItems — low-hp globe detours
#include "game/build_score.h"     // BuildScore::bestRangedBackpackIdx — the melee build's sidearm pick
#include "game/skill.h"           // findSkillDef / computeCooldownTicks — mirror the real cast gates
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

// Anti-stall COMBAT relocation, shared by the break-off (Remedy C) and the geometry escape (Remedy B).
// The bot is stalled — usually with a shootable target it isn't killing (shots blocked by cover/angle,
// or one/two enemies body-blocking it against a wall). Break the stall WITHOUT ever holstering the
// guns: aim + fire at the nearest LOS target and STRAFE around it, biased toward `hint` (the exit flow,
// or — when there is no flow — away from the wedge anchor) so the bot simultaneously (a) keeps damaging
// whatever pins it, (b) changes its firing angle so a blocked shot can connect, and (c) drifts past the
// enemy toward the exit. Falls back to a plain forward walk along `hint` when nothing is shootable.
//
// This is the fix for the observed 60 s freeze: the OLD break-off/escape CLEARED fire and tried to walk
// straight to the exit, so two body-blocking enemies it refused to shoot pinned it forever while it
// silently pressed into them and its HP regenerated. Returns an intent whose move/fire flags are empty
// only when there is genuinely nothing to do (no target AND no heading) — the caller then keeps its
// current intent rather than forcing a no-op.
static Autoplay::BotIntent unstickCombatMove(const Autoplay::BotView& v, Vec3 hint,
                                             const LevelGrid& grid, f32 feetY, bool lavaFloor,
                                             Vec3 anchor, Vec3 selfPos, f32 selfYaw) {
    Autoplay::BotIntent out{};
    out.aimYaw = selfYaw; out.aimPitch = 0.0f;

    // Nearest LOS target within engage reach (the doctrine's fire band, or the 12 m threat radius so a
    // short-reach build still shoots a genuine body-blocker).
    const Autoplay::Doctrine doc = Autoplay::doctrineFor(v.buildCell);
    const f32 reach = fmaxf(doc.engageMax * v.weaponRange, 12.0f);
    s32 ti = -1; f32 bestD = 1e9f;
    for (u32 i = 0; i < v.targetCount; i++) {
        if (!v.targets[i].hasLOS || v.targets[i].dist > reach) continue;
        if (v.targets[i].dist < bestD) { bestD = v.targets[i].dist; ti = (s32)i; }
    }

    // Preferred net-progress direction: the exit flow if we have one, else straight away from the wedge
    // anchor. Used both to bias the strafe side and as the plain-walk fallback heading.
    Vec3 pref = hint;
    if (lengthSq(pref) < 1e-6f) pref = Vec3{selfPos.x - anchor.x, 0.0f, selfPos.z - anchor.z};
    const bool havePref = lengthSq(pref) > 1e-6f;
    if (havePref) pref = normalize(pref);

    if (ti >= 0) {
        // Aim + fire at the target (lead projectile weapons, mirroring decideCombat). KEEPING the guns on
        // is what kills a body-blocker and unwedges the bot.
        const Autoplay::BotTarget& t = v.targets[(u32)ti];
        const Vec3 eye = selfPos + Vec3{0, v.eyeHeight, 0};
        Vec3 aimPt = t.pos;
        if (v.weaponProjSpeed > 0.1f) {
            f32 tHit;
            if (LeadAssist::interceptTime(t.pos - eye, t.vel, v.weaponProjSpeed, tHit))
                aimPt = t.pos + t.vel * tHit;
        }
        Autoplay::dirToAim(aimPt - eye, out.aimYaw, out.aimPitch);
        out.fire = !v.stunned && !v.rolling;

        // Strafe perpendicular to the aim. MOVE_RIGHT world dir = {cos(yaw),0,-sin(yaw)} (player.cpp:84-89,
        // right = cross(flatForward, up)); MOVE_LEFT is its negation. Take the hazard-safe side, preferring
        // the one that best follows `pref` so the circling motion also drifts toward the exit.
        const f32 cy = cosf(out.aimYaw), sy = sinf(out.aimYaw);
        const Vec3 rightW{cy, 0.0f, -sy}, leftW{-cy, 0.0f, sy};
        const bool rOk = Autoplay::stepAllowed(grid, selfPos, feetY, rightW, lavaFloor);
        const bool lOk = Autoplay::stepAllowed(grid, selfPos, feetY, leftW, lavaFloor);
        const f32 rScore = havePref ? dot(rightW, pref) : 0.0f;
        const f32 lScore = havePref ? dot(leftW,  pref) : 0.0f;
        if      (rOk && (!lOk || rScore >= lScore)) out.moveRight = true;   // strafe the exit-ward safe side
        else if (lOk)                               out.moveLeft  = true;
        // Neither lateral safe: fire in place. Damage is still progress — the target dies and unwedges us.
        return out;
    }

    // Nothing to shoot: relocate along the preferred heading (the classic break-off / escape walk).
    if (havePref) { Autoplay::dirToAim(pref, out.aimYaw, out.aimPitch); out.aimPitch = 0.0f; out.moveFwd = true; }
    return out;
}

// True if `in` carries an actionable command (any move or fire) the anti-stall helper produced — used
// so a "nothing to do" result leaves the caller's existing intent untouched instead of forcing a no-op.
static bool intentActs(const Autoplay::BotIntent& in) {
    return in.moveFwd || in.moveBack || in.moveLeft || in.moveRight || in.fire;
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
    // While the H-handoff grace is live, human input is NOT allowed to take control back: the takeover
    // latch hands to the human on the first input it sees, so without this the bot handed straight back
    // the instant the player moved the mouse toward another window — the reported "handover doesn't
    // work". The grace holds the bot through the switch-away; once the player clicks off, the game is
    // unfocused and humanActivity is gated false anyway, so nothing more is needed.
    if (m_autoplayHandoffGrace > 0.0f) m_autoplayHandoffGrace -= dt;
    const bool humanAct = Input::humanActivityThisFrame() && m_autoplayHandoffGrace <= 0.0f;
    m_autoplayControl.tick(humanAct, uiOpen, dt);

    // H = instant HANDOFF, window STAYS VISIBLE. Hand control to the bot NOW (skip the 2 s resume
    // window), ARM the grace above so the bot keeps control while the player reaches for another window,
    // and FREE THE CURSOR that relative-mouse mode locks to the window centre so the player can click or
    // alt-tab away WITHOUT the window being minimised/hidden — they wanted to keep watching it play.
    // When they click off, the game loses OS focus for real and the "unfocused = no input, game keeps
    // running" gate keeps the bot driving. forceBot() runs AFTER the latch tick above on purpose: the
    // H keystroke registers as human activity, which the tick would read as a takeover, so forcing bot
    // control here wins over it. releaseCursorOnce() leaves the WANTED mouse mode intact, so the aim
    // still works if the player tabs back and reclaims control. isKeyPressed is edge-triggered (one tap).
    if (!uiOpen && Input::isKeyPressed(SDL_SCANCODE_H)) {
        m_autoplayControl.forceBot();
        m_autoplayHandoffGrace = 4.0f;   // ~4 s: long enough to move the mouse and click/alt-tab away
        Input::releaseCursorOnce();
    }

    // Human is driving (or resuming window still counting down): drop any synthetic held actions so
    // the real device is the only input, and get out of the way.
    if (!m_autoplayControl.botInControl()) { Input::clearBotHeld(); return; }
    // Hard-freeze UI up (pause / character inspect / options / menagerie): the world is frozen in SP
    // and the bot must not act. botMayAct() already excludes those but still allows the inventory.
    if (!botMayAct()) { Input::clearBotHeld(); return; }

    // Tick the bot-side DODGE LEASHES before building the view — the policy only ever sees
    // "allowed / not allowed", so the timers themselves stay entirely on this side.
    if (m_autoplayDodgeCd    > 0.0f) m_autoplayDodgeCd    -= dt;
    if (m_autoplayGapCloseCd > 0.0f) m_autoplayGapCloseCd -= dt;

    // TOWN: its own tiny policy, entirely on this side. The pure brain idles OFF a normal floor
    // (`onNormalFloor` covers town + arena + Source chamber) and the town's flow field points at the
    // plaza centre rather than the portal, so an armed run parked at the hub forever. Only the TOWN
    // is claimed here: the ARENA (a progression firewall — no XP, no loot, no saves) and the SOURCE
    // CHAMBER (the secret boss you opt into) still idle exactly as before, and the brain itself stays
    // flat-floor pure — no town concept ever reaches it.
    if (m_level.inTown) { autoplayTownStep(dt, uiOpen); return; }

    // --- BALANCE TELEMETRY (playtest rig). One `[TELEM]` line per floor completed + a 30 s heartbeat,
    // so a soak is a balance dataset: how long each floor took, deaths/kills on it, and the player's
    // power (HP / sustained weapon DPS / gear score) against the effective floor (raw + difficulty*50).
    {
        m_autoplayRunTime += dt; m_autoplayFloorTime += dt; m_autoplayHbTimer += dt;
        const char* cls = kClassDefs[static_cast<u32>(m_playerClass)].name;
        auto weaponDps = [&]() -> f32 {
            const WeaponDef w = Inventory::getEffectiveWeapon(m_inventories[0], m_itemDefs, m_weaponDefs[0]);
            const f32 cd = (w.cooldown > 0.01f) ? w.cooldown : 0.2f;
            return (w.clipSize > 0) ? WeaponDps::sustained(w.damage, cd, w.clipSize, w.reloadTime)
                                    : WeaponDps::sustained(w.damage, cd, 0.0f, 0.0f);
        };
        if (m_level.currentFloor != m_autoplayTelemFloor) {   // completed a floor: emit the one we left
            if (m_autoplayTelemFloor != 0)
                LOG_INFO("[TELEM] cls=%s fl=%u eff=%u secs=%.1f deaths=%u kills=%u hp=%.0f/%.0f wdps=%.0f gear=%.0f boss=%d",
                         cls, m_autoplayTelemFloor, m_autoplayTelemFloor + m_difficulty * 50u,
                         m_autoplayFloorTime, m_autoplayDeaths - m_autoplayFloorStartDeaths,
                         m_totalKills[0] - m_autoplayFloorStartKills, m_localPlayer.health,
                         m_localPlayer.maxHealth, weaponDps(),
                         BuildScore::gearScoreForCell(m_inventories[0], m_itemDefs, m_itemDefCount,
                                                      m_inventories[0].buildCell),
                         static_cast<int>(m_level.floorHasBoss));
            m_autoplayTelemFloor       = m_level.currentFloor;
            m_autoplayFloorTime        = 0.0f;
            m_autoplayFloorStartDeaths = m_autoplayDeaths;
            m_autoplayFloorStartKills  = m_totalKills[0];
        }
        if (m_autoplayHbTimer >= 30.0f) {   // heartbeat: progression rate + visibility into a long/stuck floor
            m_autoplayHbTimer = 0.0f;
            LOG_INFO("[TELEM-HB] cls=%s fl=%u eff=%u elapsed=%.0f secs_fl=%.0f deaths=%u kills=%u hp=%.0f/%.0f wdps=%.0f",
                     cls, m_level.currentFloor, m_level.currentFloor + m_difficulty * 50u, m_autoplayRunTime,
                     m_autoplayFloorTime, m_autoplayDeaths, m_totalKills[0], m_localPlayer.health,
                     m_localPlayer.maxHealth, weaponDps());
        }
    }

    Autoplay::BotView v = buildBotView();

    // TARGET LOS GRACE (aim steadiness). buildBotView has just resolved the sticky target's slot;
    // time how long it has been BLIND so the pure pickTarget can ride out a flicker instead of
    // releasing on it. A single raycast to a target's centre from a moving eye toggles constantly
    // (measured: 45-57 of every 60 ticks in a corridor fight) and each release dropped the brain out
    // of FIGHT into TRAVEL, swinging the desired aim ~55° some 25 times a second — the camera shake.
    if (v.currentTargetIdx >= 0 && !v.targets[(u32)v.currentTargetIdx].hasLOS)
        m_autoplayTargetBlind += dt;
    else
        m_autoplayTargetBlind = 0.0f;
    v.targetBlindGrace = m_autoplayTargetBlind <= Autoplay::TARGET_LOS_GRACE;

    // Floor-type facts shared by several blocks below. VHALL-UPPER-EXIT is the "protect the climb"
    // scope (the fall veto + the commit's edge release); the pad carve-outs mirror the travel veto
    // in buildBotView so a COMMITTED heading is re-judged by the same rules the fresh one passed.
    const bool vhallUpperExit = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL &&
                                m_level.floorDoorPos.y > 1.5f;
    const bool commitAvoidPads = m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY &&
                                 !Autoplay::onJumpPad(m_level.grid, m_localPlayer.position) &&
                                 !m_autoplayDescent.paddedOnly;

    // TRAVEL-HEADING COMMIT (aim steadiness, the other half). Hold whichever heading we committed to
    // rather than re-deciding the flow byte + detour fan every tick. Four release conditions, all of
    // them safety- rather than time-driven:
    //   * the committed step stopped being hazard-safe — re-vetoed every tick BY THE SAME RULES the
    //     fresh heading passed in buildBotView (jump pads count on a Descent floor), so the commit
    //     can never drive the bot somewhere the fresh heading would have refused,
    //   * on a VHALL upper-exit climb, the committed step WOULD FALL (Autoplay::wouldFall). The
    //     two-story field can never point off an edge, but the commit replays a heading up to
    //     0.4 s old: crest the ramp and the fresh heading turns ~90° along the balcony — under the
    //     120° release below — while the held mid-ramp heading runs straight across the 2-cell rim
    //     (2.4 m at walk speed). That stale replay was the residual "climbs then drops".
    //   * the fresh heading points more than ~120° away (a genuine route change, e.g. the exit is now
    //     behind us) — a 45/90° disagreement is exactly the boundary toggle we are damping, so that
    //     one deliberately does NOT release,
    //   * the window expired, or there is no heading at all (at the exit / off-field).
    {
        constexpr f32 kTravelCommitSec = 0.40f;   // ~2.4 m at walking speed: a cell or two
        constexpr f32 kRouteReversed   = -0.5f;   // dot < this = more than 120° apart
        if (m_autoplayTravelHold > 0.0f) m_autoplayTravelHold -= dt;
        const bool haveFresh = lengthSq(v.flowDir) > 1e-6f;
        const bool haveHeld  = lengthSq(m_autoplayTravelDir) > 1e-6f;
        if (!haveFresh) {                                     // at the exit / boxed in: drop the commit
            m_autoplayTravelDir = Vec3{0, 0, 0}; m_autoplayTravelHold = 0.0f;
        } else if (m_autoplayTravelHold > 0.0f && haveHeld &&
                   dot(m_autoplayTravelDir, v.flowDir) > kRouteReversed &&
                   Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, m_localPlayer.position.y,
                                         m_autoplayTravelDir, m_level.lavaFloor, commitAvoidPads) &&
                   !(vhallUpperExit &&
                     Autoplay::wouldFall(m_level.grid, m_localPlayer.position,
                                         m_localPlayer.position.y, m_autoplayTravelDir))) {
            v.flowDir = m_autoplayTravelDir;                  // keep walking the committed heading
        } else {
            m_autoplayTravelDir  = v.flowDir;                 // re-commit to this tick's choice
            m_autoplayTravelHold = kTravelCommitSec;
        }
    }

    Autoplay::BotIntent in = Autoplay::decide(v);
    // SURVIVE is sacred: the remedy chain below rewrites `in` wholesale (the exit bull, the escape
    // ladder, look-behind), which would drop a potion the brain wanted at low HP. Capture the desire
    // here and re-assert it after the remedies so a COMMITTED shove to the door (which now persists
    // through a swarm) can never march the bot to its death holding a potion it never drank.
    const bool decidedPotion = in.potion;

    // CLIMB-ASSIST JUMP (VERTICAL_HALL up-ramps). buildBotView flags an unfinished climb; the ramps
    // are narrow 2-wide graduated slabs and the eased-aim walk drifts the bot off the strip and
    // slides it back before it crests (measured: on some seeds it never passed ~1 m of a 3 m climb).
    // A steady hop carries it up over the risers. Only pulse while the brain is actually TRAVELLING
    // (a fight or a potion this tick owns the body — never fight the player's own re-gear), and let
    // applyBotIntent gate the jump on being grounded. The cadence is a deterministic tick window
    // (~1.2 s period, a 4-tick pulse) — no rand, so it stays replay-safe like the rest of the bot.
    // Only pulse in the LOWER part of the climb — the mount is where the risers defeat a plain walk;
    // higher up the bot has the slab under it and a jump there is as likely to bounce it off the
    // narrow strip as help. `m_localPlayer.position.y < 1.5` is roughly the lower half of a 3 m ramp.
    // The !in.fire gate was WRONG for the swarm case it most needs to handle: on a VHALL upper-exit
    // floor a balcony/ramp-foot swarm makes the bot FIRE every tick, which suppressed the climb hop
    // entirely — so a fighting bot could never crest the ramp and roamed the floor forever (measured: 5
    // of 6 deep-floor stalls in a 9-seed benchmark). Allow the hop while firing on VHALL upper-exit; the
    // ~1.2 s cadence keeps it from being a constant bounce, and applyBotIntent still gates it on grounded.
    // AND require the bot to be at the ramp (m_autoplayVhOnRamp): the pos.y gate alone is true across the
    // whole flat void (ground pos.y≈0 < 1.5), so without this the bot bunny-hops the entire approach.
    const bool vhClimbHop = m_autoplayVhClimbing && m_autoplayVhOnRamp && m_localPlayer.position.y < 1.5f &&
                            !in.potion && !in.descend &&
                            (vhallUpperExit || !in.fire);
    if (vhClimbHop) {
        const u32 phase = currentLocalTick() % 72;
        if (phase < 4) in.jump = true;
    }

    // TARGET STICKINESS bookkeeping. Re-run the (pure, cheap — a scan of <= 16 slots) pick with the
    // same view the brain just used, so the driver learns WHICH hostile was engaged and can carry
    // that identity into the next tick. Same target => the dwell accumulates and eventually unlocks
    // a switch; a different one (or none) => reset, so the next switch has to earn its dwell again.
    {
        const s32 chosen = Autoplay::pickTarget(v, Autoplay::doctrineFor(v.buildCell));
        const u32 chosenId = (chosen >= 0) ? v.targets[(u32)chosen].id : 0u;
        if (chosenId != m_autoplayTargetId) { m_autoplayTargetId = chosenId; m_autoplayTargetDwell = 0.0f; }
        else                                 m_autoplayTargetDwell += dt;
    }

    // CHARGE the leash on the REQUEST, not on the roll actually starting. The policy already
    // requires the engine's own dodge to be ready, so a request essentially always becomes a roll;
    // and on the rare tick it doesn't (mid-air, a state change later in the frame) charging anyway
    // is the conservative direction — it delays the next ask rather than letting it re-fire.
    if (in.dodge) {
        if (in.dodgeIsGapClose) m_autoplayGapCloseCd = Autoplay::GAP_CLOSE_COOLDOWN;
        else                    m_autoplayDodgeCd    = Autoplay::doctrineFor(v.buildCell).dodgeCooldownSec;
    }

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
    // is UNIFIED across travel AND combat: the bot is making progress this tick if it MOVED (>0.5 m
    // from the anchor) OR it dealt combat damage (a nearby hostile's HP fell / one died). Only when it
    // did NEITHER does the no-progress timer climb. The old code exempted any in-band fight outright —
    // which SUPPRESSED the timer forever whenever the bot fired at an in-band LOS target it could not
    // actually kill (cover/doorway/elevation blocks the shots though LOS-to-centre reads clear), the
    // ship-blocking combat livelock. Now such a standoff (fire in place, zero damage) lets the timer
    // climb like any wedge, so the break-off (3) and the escape ladder below can break it.
    bool inBandFight = false;
    bool combatProgress = false;
    bool killedThisTick = false;   // a hostile DIED this tick (targetCount fell) — real fight progress,
                                   // distinct from mere chip damage; releases the exit bull back to combat
    {
        const Vec3 p  = m_localPlayer.position;
        const f32  dx = p.x - m_autoplayLastPos.x, dz = p.z - m_autoplayLastPos.z;
        const f32  dy = p.y - m_autoplayLastPos.y;
        // 3D displacement, so CLIMBING counts as progress. The old XZ-only test read a bot walking up
        // a ramp (much of whose motion is vertical) as "stuck", tripped the escape ladder, and the
        // escape headings walked it off the ramp — climb, stall, get shoved off, repeat. Vertical
        // progress is real progress on a stacked floor.
        const bool progressed = (dx * dx + dz * dz + dy * dy) > 0.25f;   // > 0.5 m from the fast anchor

        // NET progress over a slow window (engine.h m_autoplaySlow*): the fast `progressed` above is
        // fooled by an in-place OSCILLATION — a bot sliding along a wall or orbiting a pin moves > 0.5 m
        // every tick, re-anchoring forever, so the stuck timer never climbs and the escape ladder never
        // fires. Every kSlowWin s we check real NET travel; a window with < kSlowMin of it means the bot
        // is livelocked in place however much it churns. XZ only (a stacked climb is handled by `dy`
        // above and rarely oscillates).
        constexpr f32 kSlowWin = 2.5f, kSlowMin = 2.5f;
        m_autoplaySlowAnchorT += dt;
        if (m_autoplaySlowAnchorT >= kSlowWin) {
            const f32 sdx = p.x - m_autoplaySlowAnchor.x, sdz = p.z - m_autoplaySlowAnchor.z;
            m_autoplaySlowNetStuck = (sdx * sdx + sdz * sdz) < kSlowMin * kSlowMin;
            m_autoplaySlowAnchor = p; m_autoplaySlowAnchorT = 0.0f;
        }
        const bool netStuck = m_autoplaySlowNetStuck;

        // In-band fight = an LOS target the bot is SHOOTING AT, so this must track decideCombat's fire
        // gate exactly: within engageMax x range, no engageMin term (the kite floor moves the bot, it
        // never holds fire). Keeping the old floor here would blind the standoff detector to precisely
        // the case the fire fix created — a swarm inside the kite floor being shot at point-blank.
        const Autoplay::Doctrine doc = Autoplay::doctrineFor(v.buildCell);
        f32 enemyHp = 0.0f;
        for (u32 i = 0; i < v.targetCount; i++) {
            const Autoplay::BotTarget& t = v.targets[i];
            enemyHp += t.hp;                                   // combat-progress signal: total nearby HP
            if (t.hasLOS && t.dist <= doc.engageMax * v.weaponRange) inBandFight = true;
        }
        // Combat progress = we dealt damage (summed HP fell past a small epsilon) OR scored a kill
        // (fewer hostiles gathered than last tick). Comparing against the previous tick's snapshot; a
        // RISE (a new enemy walked into range) is not progress, so we only test for a drop.
        killedThisTick = (v.targetCount < m_autoplayLastEnemyCount);
        combatProgress = killedThisTick || (enemyHp < m_autoplayLastEnemyHp - 0.5f);
        m_autoplayLastEnemyHp    = enemyHp;
        m_autoplayLastEnemyCount = v.targetCount;

        if (progressed && !netStuck) {
            // Real progress resumed (moved > 0.5 m from the wedge anchor AND actually getting somewhere
            // NET): re-anchor and DROP the whole escape ladder so the bot returns to plain flow-field
            // travel. The !netStuck gate is what stops an in-place slide/orbit from masquerading as
            // progress and starving the escape ladder — the wall-pinned-swarm livelock.
            m_autoplayLastPos = p; m_autoplayNoProgressTimer = 0.0f;
            m_autoplayNudgeTimer = 0.0f; m_autoplayEscapeTimer = 0.0f;
            m_autoplayLookBehindDone = false;   // new episode gets a fresh look-behind
        } else if (combatProgress) {
            // Dealing damage in place is progress too (a real fight, not a wedge): hold the timer + escape
            // ladder at zero WITHOUT moving the anchor (the bot hasn't travelled, it's killing things).
            m_autoplayNoProgressTimer = 0.0f;
            m_autoplayNudgeTimer = 0.0f; m_autoplayEscapeTimer = 0.0f;
            m_autoplayLookBehindDone = false;
        } else if (m_autoplayLootDwell <= 0.0f) {
            m_autoplayNoProgressTimer += dt;                  // no move, no damage, not dwelling: wedged
        }
    }
    const bool stuck = m_autoplayNoProgressTimer > 4.0f;

    // (2b) BREAK OFF a stalled fight — the fix for the combat livelock. When the bot has been firing in
    // place at an in-band target for ~3 s but dealt no damage (combatStalled), suppress FIGHT and force
    // a short TRAVEL leg toward the exit so it physically relocates and its firing angle changes: from
    // the new spot the target is either killable (clear line) or off the route (bot has moved on). We
    // commit the leg for ~1.5 s so it clears the standoff instead of resuming fire the instant it moves
    // 0.5 m and re-stalling in place. Gated off when parked at an eligible door (Remedy A descends
    // instead of walking away) and when there is no travel heading to follow. The forced move re-zeros
    // the no-progress timer each tick, so a PURE combat standoff never reaches the 4 s geometry ladder;
    // only a bot that is ALSO physically wedged (travel forced but walls block the step) climbs to 4 s
    // and escalates to Remedy B — exactly the intended split.
    if (m_autoplayBreakoffTimer > 0.0f) m_autoplayBreakoffTimer -= dt;
    const bool bossGate     = v.hasBoss && v.bossAlive;

    // (2c) EXIT-PROGRESS WATCHDOG. The stuck timer above keys off XZ displacement, so a bot that keeps
    // MOVING but never gets anywhere useful slips right past it: a kiting sorcerer swarmed inside its own
    // engage floor NEVER fires and just circles / spirals near the exit at a crawl, never closing the last
    // few metres and never descending. The watchdog asks a blunt question on a rolling window: over the
    // last N seconds did the bot get MEANINGFULLY closer to the exit OR deal combat damage? If NEITHER,
    // it is livelocked on this floor — bull to the exit (Remedy A) and leave. A RATE check (approach > 1 m
    // per window), not a best-distance one, so a slow inward spiral that never actually arrives still
    // trips it (a best-distance test kept resetting on the crawl and never fired).
    //
    // The window is DELIBERATELY LONG. 4 s of no-progress is not "can't get past" — it is "this enemy is
    // not a pushover": a tougher fight (an armored enemy, a kiting build repositioning, an add that takes a
    // while) legitimately spends stretches dealing no damage AND not closing on the exit, and a short
    // window bailed the bot straight out of exactly those fights instead of letting it WIN them. The bot
    // must fight its way through floors — the bull is a LAST RESORT for a genuine livelock (an unkillable
    // swarm it can neither hurt nor escape), which only shows itself over MANY seconds. So the window is
    // 16 s: any real fight resolves well inside it (the window resets on any damage dealt), and only a bot
    // that has done nothing useful for that long — no chip, no approach — is treated as stuck.
    constexpr f32 kDoorCheckWindow = 16.0f;  // "can't get past" ~ no damage AND no approach for this long
    constexpr f32 kDoorApproachMin = 1.0f;   // must close at least 1 m toward the door per window
    if (m_level.currentFloor != m_autoplayLastFloor) {   // new floor: re-anchor the window, drop the latch
        m_autoplayLastFloor      = m_level.currentFloor;
        m_autoplayDoorCheckDist  = v.distToDoor;
        m_autoplayExitStallTimer = 0.0f;
        m_autoplayExitBull       = false;
        m_autoplayFloorCheckDist  = v.distToDoor;        // and the long, kill-agnostic window below
        m_autoplayFloorStallTimer = 0.0f;
        m_autoplaySlowAnchor      = m_localPlayer.position;   // net-progress anchor: don't carry a stale
        m_autoplaySlowAnchorT     = 0.0f;                     // net-stuck flag across the descent teleport
        m_autoplaySlowNetStuck    = false;
        m_autoplayVhCommit        = false;                    // the climb is done once we've descended
        m_autoplayDescentCommit   = false;                    // ...and so is the Descent push
    }
    if (v.doorActive && !bossGate) {
        if (m_autoplayExitBull) {
            // ALREADY LATCHED — a COMMITTED shove to the door, held until the bot reaches it and the
            // descend fires (the floor-change reset above clears the latch). The bull is a LAST RESORT,
            // not a run-to-the-exit default: the bot must still FIGHT its way through floors. So it is
            // released the moment combat becomes VIABLE again — a KILL (targetCount fell) means the bot
            // can make real progress fighting, so hand control back to the FIGHT branch. What it is
            // deliberately NOT released by is mere CHIP damage: the swarm-kite livelock this exists to
            // catch always deals a little (an enemy's HP ticking down while it heals / more arrive), and
            // dropping the bull on that let the kiting bounce the bot straight back off the door it was
            // 4 m from, over and over. Kill = fight on; chip-without-kill = keep leaving. (A bot that CAN
            // kill never latched the bull in the first place — the pre-latch window resets on any damage —
            // so this only re-opens a fight the bot regained the ability to win.)
            if (killedThisTick) {
                m_autoplayExitBull = false;
                m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;
            }
        } else if (combatProgress) {   // PRE-LATCH ONLY: a fight that IS closing on the exit shouldn't arm it
            m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;
        } else {
            m_autoplayExitStallTimer += dt;
            if (m_autoplayExitStallTimer >= kDoorCheckWindow) {
                // Window elapsed with no exit-approach: did we close > 1 m toward the door in it? If not,
                // latch the bull (which now PERSISTS, per the branch above, until the bot descends). A
                // RATE check, not a best-distance one, so a slow inward spiral that never arrives still trips.
                m_autoplayExitBull      = (m_autoplayDoorCheckDist - v.distToDoor) < kDoorApproachMin;
                m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;   // next window
            }
        }
    } else {
        m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;
        m_autoplayExitBull      = false;   // no eligible exit (boss alive / town): idle the watchdog
    }

    // (2c-ii) FLOOR-STALL WATCHDOG — the same question over a much longer window, and DELIBERATELY
    // blind to combat. The window above hands a live fight the benefit of the doubt by restarting on
    // every point of damage dealt; that is right on a normal floor and useless on a dense stacked one,
    // where there is always something else to shoot and the bot can spend a whole run "winning" fights
    // in one corner of a maze. It never once latched across three measured Descent runs.
    //
    // distToDoor is a 3D distance, which is what makes one rule work on every story: descending a
    // story closes 3 m of it outright, and crossing the maze closes the rest — so any genuine
    // progress, vertical or horizontal, satisfies the window comfortably. Only a bot that is neither
    // descending nor travelling fails it.
    //
    // The remedy is the existing combat BREAK-OFF leg, not the exit bull: the bull A*-routes to the
    // door in XZ, which above L0 would march the bot to a spot three stories over the exit and park
    // it. The break-off just drops fire and walks the current travel heading — and that heading is
    // already the right one on every story (the descent field upstairs, the exit flow field on L0).
    // The window and the leg together set the travel duty cycle the bot is GUARANTEED on a floor it
    // would otherwise spend entirely in combat, and that is how they were chosen. At the first values
    // tried (10 s window, 2.5 s leg) the disengage fired — 12-14% of ticks — but only bought a 20%
    // duty cycle, and two of three measured seeds still shot for 56-63% of the run without finishing
    // floor 1. 6 s and 3 s puts a floor of ~33% travel under the bot, which is what a maze this size
    // needs, without making it walk away from a fight it is actually in danger of losing (SURVIVE
    // still outranks everything, and the leg is short enough that anything genuinely chasing is still
    // there at the end of it).
    // 20 s window (was 6 s): a STRONGER enemy — a champion, an elite — legitimately takes many seconds
    // to kill, and because this watchdog is kill-agnostic a 6 s window fired mid-fight and made the bot
    // "randomly disengage" from exactly those fights (the fight wasn't ALSO carrying it toward the exit).
    // 20 s is long enough that any real fight resolves first, so the watchdog only fires on a genuine
    // livelock (circling a floor, never approaching the exit), which is what it is for.
    constexpr f32 kFloorWindow      = 20.0f;  // how long the bot may go without getting closer to the way out
    constexpr f32 kFloorApproachMin = 2.0f;   // metres of closure required in that window
    constexpr f32 kFloorPushLeg     = 3.0f;   // disengage-and-travel leg when it fails (> the 1.5 s de-fixate)
    if (v.doorActive && !bossGate) {
        m_autoplayFloorStallTimer += dt;
        if (m_autoplayFloorStallTimer >= kFloorWindow) {
            if ((m_autoplayFloorCheckDist - v.distToDoor) < kFloorApproachMin) {
                // The break-off's 3 s leg is too short on a dense STACKED floor — it walks a bit then
                // FIGHT re-owns the feet in place, and the bot never descends/crosses. Latch a PERSISTENT
                // commit instead (holds until the bot leaves the floor): the VHALL climb commit upstairs,
                // the FOUR_STORY descend commit on a Descent maze (fight your way DOWN to the next hole).
                // Everywhere else (flat/lava), the short de-fixate leg is right.
                if (vhallUpperExit)                                            m_autoplayVhCommit      = true;
                else if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) m_autoplayDescentCommit = true;
                else                                                           m_autoplayBreakoffTimer = kFloorPushLeg;
            }
            m_autoplayFloorCheckDist  = v.distToDoor;
            m_autoplayFloorStallTimer = 0.0f;
        }
    } else {
        m_autoplayFloorCheckDist = v.distToDoor; m_autoplayFloorStallTimer = 0.0f;
    }
    // Suppress the combat break-off while bulling for the exit or standing on it — leaving the floor wins
    // over re-angling a fight we've already given up on.
    if (Autoplay::combatStalled(m_autoplayNoProgressTimer, inBandFight, combatProgress) &&
        !m_autoplayExitBull && !v.atExit && m_autoplayBreakoffTimer <= 0.0f)
        m_autoplayBreakoffTimer = 1.5f;   // arm a relocation leg (re-armed only after the timer expires)
        // NB: no flowDir requirement — the break-off STRAFES around the target (unstickCombatMove), which
        // needs no exit heading, so it works even when the bot is boxed and flowDir is vetoed to zero
        // (exactly the pocket the bot froze in: firing at an unhittable target with no flow to walk).

    // (2d) LOOK BEHIND — the dormant-ambusher trigger, and the FIRST thing tried when the bot stops
    // making progress (3 s, before the 4 s geometry ladder). A stone gargoyle is an unkillable solid
    // body that wakes ONLY while unobserved (autoplay_nav.h LOOK_BEHIND_* has the full rule), so a bot
    // that walks into one and — being an ordinary hostile in its target list — stares at it while
    // firing has built a wedge that can never clear itself. Turning around un-watches it. One-shot per
    // stuck episode; the latch is re-armed by the progress branches above.
    if (m_autoplayLookBehindTimer > 0.0f) m_autoplayLookBehindTimer -= dt;
    // Scoped to floors 1-10 (Aaron): the look-behind exists for the dormant STONE GARGOYLE standoff —
    // an unkillable statue the bot pins asleep by staring at it — and those appear on the early floors.
    // Off the early floors the spin-around reads as odd and the escape ladder handles other wedges, so
    // the one watchdog whose whole job is "shooting an untriggered gargoyle forever" is early-floor only.
    if (m_level.currentFloor <= 10 &&
        Autoplay::lookBehindDue(m_autoplayNoProgressTimer, m_autoplayLookBehindDone)) {
        m_autoplayLookBehindDone  = true;
        m_autoplayLookBehindTimer = Autoplay::LOOK_BEHIND_HOLD;
        m_autoplayLookBehindYaw   = Autoplay::lookBehindYaw(m_localPlayer.yaw);
    }

    // Remedy A (priority) — WEDGED right at the exit with the boss dead: an unreachable LOS straggler keeps
    // FIGHT active but the bot can't close, so stand still and force the descend (hold PICKUP, drop
    // fire/move) — the interact-hold completes over the next few ticks and we leave.
    //
    // The stand-still is gated on DESCEND_STOP_M, strictly INSIDE the 2 m radius updateFloorDoor
    // actually descends in. It used to engage at 2.5 m, which meant that between 2.0 and 2.5 m the bot
    // stood perfectly still holding a button that could never fire — and standing still IS "no
    // progress", so the remedy re-armed itself forever (measured live: 73 consecutive seconds frozen
    // beside an open exit). Outside the radius it now WALKS THE LAST METRE IN instead, still holding
    // the interact so the descend fires the instant it arrives. That walk-in is bounded by the
    // no-progress timer: if pressing at the door isn't working after 8 s the bot is wedged on real
    // geometry, and the escape ladder below — which the walk-in must never shadow — takes over.
    const bool atDoor = stuck && v.doorActive && !bossGate;
    if (m_autoplayVhCommit && vhallUpperExit) {
        // VHALL COMMIT (armed by the floor-stall watchdog; see engine.h). The bot climbed to the balcony
        // story but kept FIGHTING the swarm in place — kite/strafe, never walking to the door — and fell
        // back off the rim (measured: pos.y cycling 3<->0, d2d never closing).
        //
        // The FIX is to commit only the FEET to the exit, NOT to stop fighting. An earlier version
        // clobbered the whole intent to a bare walk+fire; the bot then "just [ran] for the exit without a
        // care" and died to the swarm (user: "when pushing for the exit fight back properly"). So KEEP the
        // brain's combat decisions this tick — aim, fire, dodge, block, class skills, potion — and only
        // OVERRIDE the locomotion: decompose the exit heading onto the CURRENT facing basis (faceAndGo's
        // exact convention) so the bot MOVES toward the door while still facing / shooting / dodging /
        // blocking the enemy it is aimed at. The feet can never kite away (they always resolve toward the
        // exit), so there is no fight-in-place, but every defensive reflex still fires — it fights its way
        // out. The per-component FALL VETO below keeps each step edge-safe. Force descend + the ramp hop;
        // stop the feet inside 1.5 m so the descend hold can land. The floor-change reset clears the latch.
        if (lengthSq(v.flowDir) > 1e-6f) {
            const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
            const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
            const Vec3 dir = normalize(Vec3{v.flowDir.x, 0.0f, v.flowDir.z});
            const f32  df = dot(dir, fwd), dr = dot(dir, right);
            constexpr f32 kAxis = 0.35f;                     // ~20°, matches faceAndGo
            const bool stop = v.distToDoor <= 1.5f;          // at the door: hold still for the descend
            in.moveFwd   = !stop && df >  kAxis;             // (aim / fire / dodge / block / skill / potion
            in.moveBack  = !stop && df < -kAxis;             //  all stay as the brain decided them — only
            in.moveRight = !stop && dr >  kAxis;             //  the WASD feet are overridden toward the exit)
            in.moveLeft  = !stop && dr < -kAxis;
            in.descend   = true;
            // FAST jump pulse while climbing (edge every ~8 ticks): the bot spends most of a failed ramp
            // mount AIRBORNE (bouncing off a riser), grounded for only a tick at a time, so a slow ~1.2 s
            // pulse almost never lands on a grounded frame. A fast pulse catches those frames — one jump
            // from the base clears the ~0.7 m riser. applyBotIntent gates it on grounded, so it can't
            // double-jump. Gated on m_autoplayVhOnRamp so the commit WALKS the flat approach (and settles
            // onto a void pad to be launched) instead of bunny-hopping across it.
            const bool climbing = m_localPlayer.position.y < m_level.floorDoorPos.y - 0.5f;
            if (climbing && m_autoplayVhOnRamp && (currentLocalTick() % 8u) < 4u) in.jump = true;
        }
    } else if (m_autoplayDescentCommit && m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) {
        // FOUR_STORY DESCEND COMMIT (armed by the floor-stall watchdog; see engine.h). The bot was
        // standing in the swarm firing instead of descending. Same shape as the VHALL commit: KEEP the
        // brain's combat this tick (aim / fire / dodge / block / class skills / potion) and only OVERRIDE
        // the WASD feet toward the descent field heading (v.flowDir routes to the next drop hole upstairs,
        // to the door on L0), decomposed onto the CURRENT facing basis — so the bot walks/strafes to the
        // hole while still fighting the swarm, and falls through it. At a hole the field returns {0,0,0}
        // and this leaves the feet alone, so the drop itself is never fought. The pad-avoidance veto below
        // still applies to these feet (it runs after), so the commit can't march the bot onto a return
        // lift. Held until the bot leaves the floor (the floor-change reset clears the latch).
        if (v.distToDoor < Autoplay::DESCEND_STOP_M) {
            // Reached the L0 exit door under the commit. This branch SHADOWS the normal atDoor descend
            // below, so it must fire the interact ITSELF — otherwise the bot arrives at the door (0.7 m,
            // field flow=0) and stands there fighting the swarm forever, never pressing descend (measured:
            // runs reaching the door and never taking it). Hold descend, feet still, like the atDoor case.
            in = Autoplay::BotIntent{};
            in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
            in.descend = true;
        } else if (lengthSq(v.flowDir) > 1e-6f) {
            const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
            const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
            const Vec3 dir = normalize(Vec3{v.flowDir.x, 0.0f, v.flowDir.z});
            const f32  df = dot(dir, fwd), dr = dot(dir, right);
            constexpr f32 kAxis = 0.35f;                     // ~20 degrees, matches faceAndGo
            in.moveFwd = df > kAxis; in.moveBack = df < -kAxis;
            in.moveRight = dr > kAxis; in.moveLeft = dr < -kAxis;
        }
    } else if (atDoor && v.distToDoor < Autoplay::DESCEND_STOP_M) {
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        in.descend = true;
    } else if (atDoor && v.distToDoor < 2.5f && m_autoplayNoProgressTimer < 8.0f) {
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        const Vec3 h{m_level.floorDoorPos.x - m_localPlayer.position.x, 0.0f,
                     m_level.floorDoorPos.z - m_localPlayer.position.z};
        if (lengthSq(h) > 1e-6f) {
            f32 y, p; Autoplay::dirToAim(h, y, p);
            in.aimYaw = y; in.aimPitch = 0.0f; in.moveFwd = true;   // close the last metre
        }
        in.descend = true;
    } else if (stuck || m_autoplayNudgeTimer > 0.0f || m_autoplayEscapeTimer > 0.0f) {
        // Remedy B — wedged on geometry: an ESCALATING escape so an AFK bot is NEVER found permanently
        // idle. The longer the bot makes no XZ progress (m_autoplayNoProgressTimer keeps climbing while
        // wedged), the more aggressive the escape:
        //   STAGE 1 (stuck, <6 s): a lateral ±90/180 nudge off the current heading (the original remedy).
        //   STAGE 2 (nudge found no safe step, or >6 s): a full 8-direction safe-step search that walks
        //           AWAY from the wedge anchor (autoplay_nav.h escapeHeading) — the flow field can be
        //           {0,0,0} here (off-field on a stacked floor, boxed in a lava corner) so we can't lean
        //           on it, but the geometry still has an opening unless the cell is fully walled.
        //   STAGE 3 (>8 s): a short A* leg toward the exit door — the escape hatch for when the flow
        //           field ITSELF gives no heading; falls back to STAGE 2 if the door is out of A*'s
        //           256-cell reach or its first step isn't safe.
        // The Stage 2/3 heading is committed for a ~0.5 s window (traverse a cell before re-deciding;
        // also throttles A* to once per window). While stuck the bot is NEVER left with a zero heading
        // unless the cell is fully walled — which the level geometry guarantees can't persist.
        const f32  feetY  = m_localPlayer.position.y;
        const Vec3 anchor = m_autoplayLastPos;   // last progress point = where the bot wedged
        Vec3 esc{0, 0, 0};

        // STAGE 1: lateral nudge. Arms at the 4 s stuck onset and only up to 6 s (past that, escalate).
        if (stuck && m_autoplayNudgeTimer <= 0.0f && m_autoplayEscapeTimer <= 0.0f &&
            m_autoplayNoProgressTimer < 6.0f)
            m_autoplayNudgeTimer = 0.5f;
        if (m_autoplayNudgeTimer > 0.0f) {
            m_autoplayNudgeTimer -= dt;
            // Base heading: the travel heading if we have one, else the bot's facing. Rotate to a
            // lateral/back direction and take the first whose one-cell step is hazard-safe.
            Vec3 base = v.flowDir;
            if (lengthSq(base) < 1e-6f)
                base = Vec3{-sinf(m_localPlayer.yaw), 0.0f, -cosf(m_localPlayer.yaw)};
            const f32 kAngles[3] = {1.5707963f, -1.5707963f, 3.14159265f};   // +90°, -90°, 180°
            for (u32 i = 0; i < 3; i++) {
                const Vec3 cand = rotateY_XZ(base, kAngles[i]);
                if (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, cand, m_level.lavaFloor)) {
                    esc = cand; break;
                }
            }
            if (lengthSq(esc) < 1e-6f) m_autoplayNudgeTimer = 0.0f;   // no lateral step: abandon, escalate now
        }

        // STAGE 2 / 3: committed 8-dir (or A*) escape, engaged whenever the lateral nudge isn't driving.
        if (lengthSq(esc) < 1e-6f) {
            if (m_autoplayEscapeTimer <= 0.0f) {
                Vec3 h{0, 0, 0};
                // STAGE 3 first (deepest escalation): a short A* leg toward the exit for when the flow
                // field itself yields no heading. bodyRadius ~ the player half-width; findPath returns
                // world-space waypoints (outPath[0] = the first corner toward the goal), 0 if the door is
                // unreachable within its 256-cell cap.
                if (m_autoplayNoProgressTimer > 8.0f && m_level.floorDoorActive) {
                    // On a STACKED floor the flat A* below is story-blind and routes to the door's XZ
                    // under a balcony / away from a hole — use the story-aware field heading instead.
                    const bool stackedExit = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL ||
                                             m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY;
                    if (stackedExit && lengthSq(v.flowDir) > 1e-6f) {
                        h = normalize(Vec3{v.flowDir.x, 0.0f, v.flowDir.z});
                    } else {
                        Vec3 wp[MAX_PATH_WAYPOINTS];
                        const u8 n = Pathfinder::findPath(m_level.grid, m_localPlayer.position,
                                                          m_level.floorDoorPos, wp, MAX_PATH_WAYPOINTS, 0.3f);
                        if (n > 0) {
                            const Vec3 to{wp[0].x - m_localPlayer.position.x, 0.0f,
                                          wp[0].z - m_localPlayer.position.z};
                            if (lengthSq(to) > 1e-6f) {
                                const Vec3 cand = normalize(to);
                                // Only trust the A* heading if its own first cell is hazard-safe (A* is
                                // 2D / story-blind, so re-veto its immediate step here).
                                if (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, cand,
                                                          m_level.lavaFloor))
                                    h = cand;
                            }
                        }
                    }
                }
                // STAGE 2 (and the STAGE-3 fallback when A* gave nothing usable): 8-dir search away from
                // the wedge. Returns a safe heading unless the cell is fully walled.
                if (lengthSq(h) < 1e-6f)
                    h = Autoplay::escapeHeading(m_level.grid, m_localPlayer.position, feetY, anchor,
                                                m_level.lavaFloor);
                m_autoplayEscapeDir   = h;
                m_autoplayEscapeTimer = 0.5f;   // commit for ~0.5 s (traverse a cell; throttle the A* leg)
            }
            m_autoplayEscapeTimer -= dt;
            // Re-validate the committed heading each tick (cheap insurance); drop the commit early if it
            // is no longer safe so the next tick recomputes rather than driving into a hazard.
            if (lengthSq(m_autoplayEscapeDir) > 1e-6f &&
                Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, m_autoplayEscapeDir,
                                      m_level.lavaFloor))
                esc = m_autoplayEscapeDir;
            else
                m_autoplayEscapeTimer = 0.0f;
        }

        // Apply the escape heading through unstickCombatMove: if a hostile is in reach it STRAFES around
        // it while FIRING (kills a body-blocker, changes the angle) biased toward `esc`; otherwise it just
        // walks `esc` (identical to the old forward step for a pure geometry wedge with nothing to shoot).
        // Only override when it produced an actionable move — a fully-boxed no-target result leaves the
        // bot's current intent alone. This is what stops the >4 s escape zone from silently holstering the
        // guns and freezing next to enemies it could have killed.
        {
            Autoplay::BotIntent u = unstickCombatMove(v, esc, m_level.grid, feetY, m_level.lavaFloor,
                                                      anchor, m_localPlayer.position, m_localPlayer.yaw);
            if (intentActs(u)) in = u;
            // JUMP as part of the escape. The ladder above only ever tried new HEADINGS, and a body
            // caught on a lip, a step edge or the inside of a corner does not need a new heading —
            // it needs to leave the ground, because move-and-slide will keep refusing the same
            // blocked axis at the same height forever. Pulsed on the kiting cadence rather than held
            // so the bot hops out rather than pogoing (a held JUMP re-fires every landing frame).
            in.jump = Autoplay::kitingJumpTick(v.tick);
        }
    } else if (m_autoplayExitBull && v.doorActive && !bossGate) {
        // Remedy B2 — EXIT BULL (the exit-progress watchdog latched): the bot is MOVING but getting
        // nowhere useful — orbiting/spiralling the floor, or kited off the exit by a swarm it refuses to
        // shoot — so stop playing and just leave. Ranked BELOW the geometry escape on purpose: when the
        // bot is physically wedged, walking at the door only presses it into the wall and the escape
        // ladder never gets to run (measured: 35 s frozen with the bull latched and moveFwd held). The
        // two are naturally exclusive — `stuck` means not moving, the bull means moving-but-not-arriving.
        const Vec3 pos = m_localPlayer.position;
        Vec3 heading{m_level.floorDoorPos.x - pos.x, 0.0f, m_level.floorDoorPos.z - pos.z};
        // On a STACKED floor the door is on ANOTHER STORY (a VHALL balcony, an FS drop below), so a
        // flat A* to its XZ walks the bot UNDER the balcony / away from the hole and wedges it there —
        // measured, the bull dragged the geared paladin to directly beneath the upstairs door and it
        // sat at ground level, never climbing. The STORY-AWARE field (v.flowDir, already the two-story
        // VHALL field / the Descent field this tick) is the only correct heading; the bull just walks
        // it with fire off. Flat A* stays for FLAT floors, where the door really is at that XZ.
        const bool stackedExit = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL ||
                                 m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY;
        if (stackedExit) {
            if (lengthSq(v.flowDir) > 1e-6f) heading = Vec3{v.flowDir.x, 0.0f, v.flowDir.z};
        } else if (v.distToDoor > 3.0f) {   // flat floor, far: a WALL-AWARE route, never the straight line
            bool routed = false;
            Vec3 wp[MAX_PATH_WAYPOINTS];
            const u8 n = Pathfinder::findPath(m_level.grid, pos, m_level.floorDoorPos, wp,
                                              MAX_PATH_WAYPOINTS, 0.3f);
            if (n > 0) {
                const Vec3 toWp{wp[0].x - pos.x, 0.0f, wp[0].z - pos.z};
                if (lengthSq(toWp) > 1e-6f) { heading = toWp; routed = true; }
            }
            // A* gives up after MAX_ASTAR_SEARCH (256) closed cells, and on a maze a door past that
            // returns NOTHING — the old code then drove the STRAIGHT LINE (heading's default) into a
            // wall and the persistent bull wedged there for good (measured: a swarmed sorcerer pinned at
            // a wall 14 m from the door, x stuck, sliding in z forever). The exit FLOW FIELD is a full,
            // UNCAPPED BFS to the same door, valid on every cell the bot can stand on, so fall back to it
            // rather than the wall-seeking bee-line. This is what makes the bull's "just leave" reliable
            // on a large flat maze, not only near the door.
            if (!routed && lengthSq(v.flowDir) > 1e-6f) heading = Vec3{v.flowDir.x, 0.0f, v.flowDir.z};
        }
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        if (lengthSq(heading) > 1e-6f) {
            f32 y, p; Autoplay::dirToAim(heading, y, p);
            in.aimYaw = y; in.aimPitch = 0.0f;
            // STOP once inside the descend radius. The exit is taken by HOLDING interact for
            // INTERACT_HOLD_SEC (0.35 s), so a bot that keeps walking blasts straight through the 2 m
            // window (measured: reached 0.1 m from the door at 6-16 m/s, repeatedly, and never descended
            // because it was never inside the radius long enough for one hold to complete). Standing still
            // is what lets the hold land.
            if (v.distToDoor > 1.5f) in.moveFwd = true;
            // PUNCH THROUGH. The committed walk gets a fragile build shoved in circles by a body-blocking
            // swarm — moving 15 m of churn but never CLOSING (measured Marksman, flat floor). So while the
            // bull is walking, DODGE toward the exit: the roll's i-frames + ~4 m lunge slide past the
            // bodies along the door heading (moveFwd is set, so the roll goes that way). Pulsed — the
            // engine's own ~1 s dodge cooldown paces the real rolls — overriding the balance leash (this
            // is escape, not a combat dodge). Dying mid-punch is fine; converging on the exit is the goal.
            // FLAT floors ONLY: a horizontal roll on a stacked floor could carry the bot off a balcony /
            // ramp edge (VHALL/FOUR_STORY route UP or via drops — a different fix), and moveFwd already
            // cuts out inside 1.5 m so the punch never overshoots the descend radius.
            const bool flatFloor = m_level.layoutStyle != LevelGen::LayoutStyle::VERTICAL_HALL &&
                                   m_level.layoutStyle != LevelGen::LayoutStyle::FOUR_STORY;
            if (flatFloor && in.moveFwd && (m_autoplayBullDodgeTick % 45u == 0u))
                in.dodge = true;
        }
        m_autoplayBullDodgeTick++;
        in.descend = true;   // held so it fires the moment the bot is inside the 2 m descend radius
        // FIRE through anything blocking the run to the exit. The shot travels along the door heading, so a
        // body ON the path is hit — this is what clears the swarm a squishy kiting build can't (its
        // doctrine kite-floor makes it REFUSE point-blank enemies, so a swarm on the exit chips it to death
        // and knocks it back forever; measured a sorcerer bouncing 5->13 m off the door at 17 HP). Bypasses
        // the band here because leaving the floor, not perfect target selection, is the goal.
        for (u32 i = 0; i < v.targetCount; i++) {
            if (v.targets[i].hasLOS && v.targets[i].dist <= v.weaponRange && !v.stunned && !v.rolling) {
                in.fire = true; break;
            }
        }
    } else if (m_autoplayBreakoffTimer > 0.0f) {
        // Remedy C — break off a stalled fight (armed in (2b)): firing at an in-band target the shots
        // can't kill (cover/angle), or an enemy body-blocking the bot. The response depends on whether an
        // exit heading exists:
        //   flowDir != 0  → WALK toward the exit with fire OFF. This DE-FIXATES from the unkillable cover
        //                   target and leapfrogs past it (move-and-slide slides around any body); moving
        //                   > 0.5 m resets the stuck timer, so the bot advances a little each cycle and
        //                   eventually reaches the exit. (Strafing-in-place here just oscillated forever
        //                   next to a cover enemy while the exit sat open — no forward progress.)
        //   flowDir == 0  → BOXED, no exit to walk to: STRAFE around the target while FIRING to kill
        //                   whatever pins us (the only way out). See unstickCombatMove — this is the fix
        //                   for the 60 s freeze where the bot refused to shoot two body-blocking enemies.
        const f32 feetY = m_localPlayer.position.y;
        if (lengthSq(v.flowDir) > 1e-6f) {
            f32 yaw, pitch; Autoplay::dirToAim(v.flowDir, yaw, pitch);
            in = Autoplay::BotIntent{};
            in.aimYaw = yaw; in.aimPitch = 0.0f; in.moveFwd = true;
        } else {
            Autoplay::BotIntent u = unstickCombatMove(v, Vec3{0, 0, 0}, m_level.grid, feetY, m_level.lavaFloor,
                                                      m_autoplayLastPos, m_localPlayer.position, m_localPlayer.yaw);
            if (intentActs(u)) in = u;
        }
    }

    // (2e) LOOK-BEHIND OVERRIDE. Applied AFTER the whole remedy chain because it has to beat every
    // one of them — and, crucially, the FIGHT branch: the gargoyle that wedged us is an ordinary
    // hostile in the target list, so decideCombat would aim straight back at it and pin it asleep
    // again. Movement and fire are dropped for the turn (a deliberate look-behind, not a fighting
    // retreat); `descend` is left alone so a door hold already in progress is not thrown away. The
    // aim smoother turns at its own rate, so this reads as a look over the shoulder, never a snap.
    if (m_autoplayLookBehindTimer > 0.0f) {
        in.aimYaw = m_autoplayLookBehindYaw; in.aimPitch = 0.0f;
        in.moveFwd = in.moveBack = in.moveLeft = in.moveRight = false;
        in.fire = false; in.jump = false;
    }

    // (2f) SHRINE USE — grab a shrine on the way. When a shrine is the current travel detour
    // (buildBotView steered onto it) and the bot has reached interact reach, and it is neither
    // fighting nor surviving nor wedged nor already holding for the exit, HOLD interact (PICKUP) to
    // activate it — the interact arbitration routes a hold to the shrine over the exit, and one hold
    // consumes it (grantShrineBuff + deactivate). Reuses the descend hold + its pulse (block 3): the
    // pulse spends the shrine on the first cycle exactly as it does a shrine sharing the exit. The bot
    // approached facing the shrine (the flowDir steer aims faceAndGo at it), so the interact aim cone
    // is satisfied; within the 1.2 m grab radius facing stops mattering, so it stops there to let the
    // hold land. Flat-floor detour only (m_autoplayShrineTarget is set only there).
    if (m_autoplayShrineTarget && !in.fire && !in.potion && !in.descend && !stuck &&
        !m_autoplayExitBull && !v.stunned && !v.rolling) {
        const Vec3 to{m_autoplayShrinePos.x - m_localPlayer.position.x, 0.0f,
                      m_autoplayShrinePos.z - m_localPlayer.position.z};
        const f32 d = length(to);
        if (d < GameConst::INTERACT_RANGE) {
            if (lengthSq(to) > 1e-6f) { f32 y, p; Autoplay::dirToAim(to, y, p); in.aimYaw = y; in.aimPitch = 0.0f; }
            in.descend = true;                                             // hold PICKUP -> shrine (pulsed below)
            if (d < 1.5f) in.moveFwd = in.moveBack = in.moveLeft = in.moveRight = false;   // stop in grab range
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


    // (4) HAZARD-VETO the lateral strafe, and gate the jump. The pure policy asks for a side-step
    // without knowing the geometry (that is the whole point of keeping it engine-free), so the one
    // authoritative check lives here — and here it is authoritative for EVERY producer of a strafe,
    // the combat policy and the unstick helper alike.
    //
    // MOVE_RIGHT's world direction is {cos(yaw), 0, -sin(yaw)} (player.cpp: right = cross(flatForward,
    // up)); MOVE_LEFT is its negation. The basis is the player's CURRENT yaw rather than the intent's
    // desired yaw, because that is the yaw the movement code will actually read this tick — the aim
    // is only EASED toward the desired one.
    if (in.moveLeft || in.moveRight) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 want = in.moveRight ? Vec3{cy, 0.0f, -sy} : Vec3{-cy, 0.0f, sy};
        const f32  feetY = m_localPlayer.position.y;
        if (!Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, want, m_level.lavaFloor)) {
            // Blocked that way: try the other side before giving up, so a bot strafing along a wall
            // simply reverses instead of standing still until the cadence flips it back.
            const Vec3 other = want * -1.0f;
            const bool otherOk = Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY,
                                                       other, m_level.lavaFloor);
            const bool wasRight = in.moveRight;
            in.moveLeft = otherOk && wasRight;
            in.moveRight = otherOk && !wasRight;
        }
    }
    // DESCENT FLOORS: no bot movement of ANY kind steps onto a jump pad. The travel heading is
    // already pad-vetoed upstream in buildBotView, but the FIGHT branch's kite/close movement is
    // deliberately unvetoed (short, reactive, enemy-derived) — and on a Descent floor that is the
    // hole in the fence. A pad launches the bot about two stories, so one kiting step onto one
    // throws away a descent it may have spent a minute on: measured, a run that had reached L0 and
    // closed to 21 m of the exit ended up spending 61% of its time back on L2. Combat is where the
    // bot spends most of a Descent floor (43-65% of ticks firing), so leaving this producer
    // unguarded left the floor unfinishable no matter how good the routing got.
    //
    // WALLS too, on the Descent maze. The FIGHT branch's kite/close/strafe movement was originally
    // left to press into walls ("walls remain the FIGHT branch's own business") — but on FOUR_STORY's
    // 3-wide braided corridors that IS the wall-hugging the player sees: with the geared paladin
    // (combat isolated) wall-scraping ran 0-6% while travelling and jumped to 24-45% exactly when the
    // bot was firing, i.e. the FIGHT movement grinding a corridor wall. stepAllowed covers walls +
    // off-map + the corner-cut rule + (avoidPads) pads in one call, so a kiting bot now SLIDES along
    // the open axis instead of pinning itself to a wall. Per-component (drop the blocked axis, keep
    // the others) so it never freezes when only one direction is walled.
    if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const Vec3 p     = m_localPlayer.position;
        const f32  feetY = p.y;
        // Pads are vetoed unless the bot is standing on one (a 3x3 pad node would box it in), the
        // storey's only ways down are lifts (paddedOnly), or the field's next routed step IS a pad
        // (a pad blocking the route — cross it) — same carve-outs as the travel veto.
        const bool avoidPads = !Autoplay::onJumpPad(m_level.grid, p) && !m_autoplayDescent.paddedOnly &&
                               !Autoplay::descentNextIsPad(m_autoplayDescent, m_level.grid, p);
        auto blocked = [&](Vec3 d) {
            return !Autoplay::stepAllowed(m_level.grid, p, feetY, d, /*lavaFloor=*/false, avoidPads);
        };
        if (in.moveFwd   && blocked(fwd))            in.moveFwd   = false;
        if (in.moveBack  && blocked(fwd   * -1.0f))  in.moveBack  = false;
        if (in.moveRight && blocked(right))          in.moveRight = false;
        if (in.moveLeft  && blocked(right * -1.0f))  in.moveLeft  = false;
    }

    // FALL VETO — VERTICAL_HALL with an UPPER exit ONLY, and for EVERY movement producer. On this
    // floor type no movement ever WANTS a fall: the two-story field only emits height-continuous
    // steps, FIGHT's kite/close/strafe is enemy-derived and blind to edges, and the escape ladder /
    // unstick strafes only want to MOVE, not to descend. So the veto covers them all. It was
    // originally gated on BotIntent::engaging (FIGHT movement only), which left two holes: the
    // escape ladder could walk a wedged bot clean off the balcony it had just climbed (escapeHeading
    // knows walls/lava, not edges), and a stale committed travel heading crossed the rim unchecked.
    // Scope stays tight on purpose:
    //   * VHALL only — FOUR_STORY descends BY falling through drop holes (the pad block above + the
    //     descent router) and must stay untouched.
    //   * EXIT UPPER only (floorDoorPos.y > 1.5 m) — when the exit is on the GROUND the bot spawned
    //     on a balcony and must get DOWN, and dropping off the rim is a valid way down; a fall veto
    //     there would only hinder the descent. The protection is for the CLIMB, where a fall undoes it.
    //   * GROUNDED only — the veto's job is to stop a bot from STEPPING off a ledge, which only a
    //     grounded bot can do. While AIRBORNE (a climb-assist hop, a void-pad launch), wouldFall reads
    //     feetY at the elevated apex, so EVERY neighbour cell's floor resolves far below it and all four
    //     directions veto at once — the bot freezes horizontally in mid-air. That turned the commit's
    //     fast jump pulse into a POGO: jump → airborne → forward vetoed → land → jump, never advancing
    //     (measured: committed bot stuck at d2d≈40 m, on=0, fwd=0, never reaching the exit ramp). An
    //     airborne bot is already on a ballistic arc — freezing its steering can't UNDO a fall, it only
    //     stops it steering toward a safe landing (the ramp riser it hopped for, the balcony it was
    //     flung at). So we lift the veto in the air and let it steer; an airborne drift off an edge is
    //     acceptable (dying is fine, freezing is not), while the grounded rim protection is untouched.
    // Applied per WASD component so the bot slides along the safe axes instead of freezing.
    if (vhallUpperExit && m_localPlayer.onGround) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const Vec3 p     = m_localPlayer.position;
        const f32  feetY = p.y;
        if (in.moveFwd   && Autoplay::wouldFall(m_level.grid, p, feetY, fwd))            in.moveFwd   = false;
        if (in.moveBack  && Autoplay::wouldFall(m_level.grid, p, feetY, fwd   * -1.0f))  in.moveBack  = false;
        if (in.moveRight && Autoplay::wouldFall(m_level.grid, p, feetY, right))          in.moveRight = false;
        if (in.moveLeft  && Autoplay::wouldFall(m_level.grid, p, feetY, right * -1.0f))  in.moveLeft  = false;
    }
    // JUMP only from the ground (the engine ignores it otherwise, but asking for what cannot happen
    // muddies the telemetry) and never while a roll owns the body.
    if (in.jump && (!m_localPlayer.onGround || m_localPlayer.dodgeState.rolling)) in.jump = false;
    in.potion = in.potion || decidedPotion;   // SURVIVE re-asserted (see decide() above): never skip a needed heal

    applyBotIntent(in, uiOpen, dt, v.weaponIsMelee);
    updateSidearm(v, dt);
}

// One tick of the TOWN policy: beeline to the to-dungeon portal and take it. Called instead of the
// whole view/brain/backstop chain while m_level.inTown — the hub has no hostiles, no exit flow and
// no floor door, so none of that machinery has anything to say here.
//
// The mid-run visitor and the cleared hero take the SAME portal; what differs is what
// Engine::updateTownPortal does on the other side (startGame(CONTINUE) straight back into the run,
// or the Free-Play select — see the auto-confirm in engine_menu.cpp for that half).
void Engine::autoplayTownStep(f32 dt, bool uiOpen) {
    Autoplay::BotIntent in{};
    in.aimYaw   = m_localPlayer.yaw;
    in.aimPitch = 0.0f;

    // The stuck/escape ladder never runs in town, so hold its state at "just made progress" instead
    // of leaving it frozen: a no-progress timer parked at 3.9 s from the last dungeon floor would
    // otherwise fire a spurious escape nudge on the first frame after the portal drops us back in.
    m_autoplayLastPos         = m_localPlayer.position;
    m_autoplayNoProgressTimer = 0.0f;
    m_autoplayNudgeTimer      = 0.0f;
    m_autoplayEscapeTimer     = 0.0f;
    m_autoplayLookBehindTimer = 0.0f;
    m_autoplayLookBehindDone  = false;

    const Autoplay::TownPortalPlan plan =
        Autoplay::planTownPortal(m_localPlayer.position, m_level.townPortalPos);

    if (lengthSq(plan.heading) > 1e-6f) {
        // Same hazard veto + widening detour fan the travel heading rides in buildBotView. The
        // beeline crosses an open plaza on the default approach, but the hub carries hut footprints
        // and a perimeter wall, and a bot pushed off-line (a knockback in, an odd arrival) must round
        // them rather than press into a plank wall. `lavaFloor=false`: the town is never molten.
        Vec3      heading = plan.heading;
        const f32 feetY   = m_localPlayer.position.y;
        if (!Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, heading, false)) {
            constexpr f32 kFan[4] = { 0.7853981634f, -0.7853981634f,     // ±45°: the gentle detour
                                      1.5707963268f, -1.5707963268f };   // ±90°: the square sidestep
            Vec3 pick{0, 0, 0};
            for (u32 i = 0; i < 4; i++) {
                const Vec3 cand = rotateY_XZ(heading, kFan[i]);
                if (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, cand, false)) {
                    pick = cand; break;
                }
            }
            heading = pick;   // {0,0,0} = fully boxed: face the portal anyway and just press
        }
        if (lengthSq(heading) > 1e-6f) {
            f32 yaw, pitch;
            Autoplay::dirToAim(heading, yaw, pitch);
            in.aimYaw   = yaw;
            in.aimPitch = 0.0f;
            in.moveFwd  = plan.walk;
        }
    }

    // Taking the portal rides the SAME pulsed interact the floor exit uses, for the same reason: the
    // portal is an EXIT-class HOLD target, and a continuously-held PICKUP makes Interact::poll fire
    // exactly ONCE — spent on whatever else is in reach (an item, the plaza's stash chest) and then
    // latched `consumed` forever. Pulsing releases the latch so the next hold reaches the portal.
    // m_townPortalRequested is set by that very same updatePlayerPickup arbitration, so the BUTTON is
    // the correct driver here — a direct flag write would be reset before updateTownPortal reads it.
    if (plan.take) {
        m_autoplayDescendPulse += dt;
        in.descend = Autoplay::descendPulseHeld(m_autoplayDescendPulse);
    } else {
        m_autoplayDescendPulse = 0.0f;
    }

    applyBotIntent(in, uiOpen, dt, /*melee=*/false);
}

// Fill the read-only decision snapshot from live engine state (lane 0 — the only Autoplay lane).
// One tick of the melee ranged-sidearm decision. Owns the WEAPON slot while active. Requirements:
// a MELEE build, on a VERTICAL_HALL floor whose exit is UPPER, with a hostile it WANTS to hit but
// cannot reach by walking — out of melee reach AND the step toward it would fall off the balcony.
// Then a ranged weapon from the bag is the only way to engage it without leaving the story, which is
// exactly what the player asked the bot NOT to do. When the trigger clears (target gone / now
// meleeable / no longer fighting), it switches back. Hysteresis (dwell + cooldown) stops it
// chattering when a target flickers in and out of the condition.
void Engine::updateSidearm(const Autoplay::BotView& v, f32 dt) {
    constexpr f32 kMinDwell = 3.0f;   // once switched, hold the sidearm at least this long
    constexpr f32 kCooldown = 5.0f;   // and wait this long between switches
    if (m_autoplaySidearmCooldown > 0.0f) m_autoplaySidearmCooldown -= dt;
    if (m_autoplaySidearmActive)          m_autoplaySidearmDwell    += dt;

    PlayerInventory& inv = m_inventories[0];

    // Is there a target a melee build could only reach by falling? Scan this tick's LOS targets.
    // "Wants it" = LOS + within the engagement ceiling (the same gate the brain fights on). "Can't
    // reach" = beyond melee swing AND the step straight toward it would fall off the balcony. Only on
    // a VHALL upper-exit climb — the one place the "don't fall to reach an enemy" rule bites.
    bool trigger = false;
    if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL && m_level.floorDoorPos.y > 1.5f) {
        // Judge the trigger with the MELEE build's numbers even while the sidearm is worn. The view
        // is the RANGED one then (buildBotView overrides weaponRange + buildCell for the brain's
        // sake), and judging "already in / near melee reach" with a pistol's reach skipped every
        // enemy on the floor — the trigger cleared the instant the sidearm was drawn, the "keep it
        // while the trigger holds" rule never held, and the state machine collapsed to a 3 s dwell
        // on / 5 s cooldown off duty cycle while the cross-gap enemy stood there the whole time.
        const f32 meleeReach = m_autoplaySidearmActive ? m_autoplaySidearmMeleeRange : v.weaponRange;
        const u8  meleeCell  = m_autoplaySidearmActive ? inv.buildCell : v.buildCell;
        const Autoplay::Doctrine doc = Autoplay::doctrineFor(meleeCell);
        // engageCeiling's own formula, on the melee numbers (v.weaponRange is the sidearm's).
        const f32 ceil = fmaxf(doc.engageMax * meleeReach, Autoplay::THREAT_RADIUS);
        for (u32 i = 0; i < v.targetCount; i++) {
            const Autoplay::BotTarget& t = v.targets[i];
            if (!t.hasLOS || t.dist > ceil) continue;
            if (t.dist <= meleeReach * 1.2f) continue;             // already in / near melee reach
            const Vec3 toT{t.pos.x - v.pos.x, 0.0f, t.pos.z - v.pos.z};
            if (lengthSq(toT) < 1e-6f) continue;
            if (Autoplay::wouldFall(m_level.grid, v.pos, v.pos.y, toT)) { trigger = true; break; }
        }
    }

    if (!m_autoplaySidearmActive) {
        // --- Consider switching TO the sidearm. ---
        if (trigger && m_autoplaySidearmCooldown <= 0.0f && v.weaponIsMelee) {
            const s32 idx = BuildScore::bestRangedBackpackIdx(inv, m_itemDefs, m_itemDefCount);
            if (idx >= 0) {
                m_autoplaySidearmMeleeUid   = inv.equipped[(u32)ItemSlot::WEAPON].uid;  // stash BEFORE equip
                m_autoplaySidearmMeleeRange = v.weaponRange;   // the melee reach the trigger keeps judging by
                Inventory::equip(inv, (u8)idx, m_itemDefs);   // ranged weapon -> WEAPON slot; melee -> bag
                m_autoplaySidearmActive = true;
                m_autoplaySidearmDwell  = 0.0f;
                addChatMessage("", "Autoplay: drew a ranged sidearm", Vec3{0.6f, 0.85f, 1.0f});
            }
        }
        return;
    }

    // --- Active: consider switching BACK to melee. ---
    // Keep it while the trigger holds OR the min dwell has not elapsed. Otherwise put the melee
    // weapon back by finding the stashed uid in the bag (its slot can move as loot comes and goes).
    if (trigger || m_autoplaySidearmDwell < kMinDwell) return;

    s32 meleeIdx = -1;
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++)
        if (inv.backpack[i].defId != 0xFFFF && inv.backpack[i].uid == m_autoplaySidearmMeleeUid) { meleeIdx = i; break; }
    if (meleeIdx >= 0) Inventory::equip(inv, (u8)meleeIdx, m_itemDefs);   // melee -> WEAPON slot
    // If the stashed weapon vanished (should not happen — nothing discards the equipped-then-bagged
    // melee weapon while the sidearm guard blocks auto-equip), fall through: clearing the flag lets
    // the next autoEquipBackpack re-gear the melee build normally.
    m_autoplaySidearmActive   = false;
    m_autoplaySidearmCooldown = kCooldown;
    addChatMessage("", "Autoplay: back to melee", Vec3{0.6f, 0.85f, 1.0f});
}

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
    // The bot's OWN leashes on top of the engine cooldown (see m_autoplayDodgeCd) — the policy asks
    // for a roll only when the matching one has expired.
    v.dodgeAllowed    = m_autoplayDodgeCd    <= 0.0f;
    v.gapCloseAllowed = m_autoplayGapCloseCd <= 0.0f;
    // blockTimer is only meaningful WHILE blocking — it is zeroed on the raise edge and simply left
    // stale on release (engine_update.cpp), so report 0 when the shield is down or the policy would
    // read a months-old hold and refuse to ever raise again.
    v.blockHeld = m_localPlayer.blocking ? m_localPlayer.blockTimer : 0.0f;

    // potionReady — replicate the tick-based gate the potion heal itself uses (engine_update.cpp) so
    // the bot only asks to drink when the press would actually fire.
    {
        const f32 cdr    = m_inventories[m_localPlayerIndex].bonusCooldownReduction * 0.1f;
        const u32 cdTk   = static_cast<u32>(GameConst::POTION_COOLDOWN * (1.0f - cdr) * 60.0f + 0.5f);
        v.potionReady    = GameConst::cooldownReady(currentLocalTick(), m_potionLastActivationTick, cdTk);
    }

    // --- class skills: per-slot "would this press actually cast?" ---
    // MIRRORS handleClassSkillActivation + SkillSystem::tryActivate gate for gate, in the same order:
    // the slot holds a real skill, the EFFECTIVE floor (difficulty adds 50/floor tier) has unlocked
    // it, a def exists, the shared energy pool covers its cost, and its tick cooldown has elapsed.
    // A bot pressing a skill that no-ops is worse than not pressing — it burns the slot selection and
    // makes the build look broken — so anything we can't verify here reads as NOT castable.
    {
        const ClassDef& cls  = kClassDefs[static_cast<u32>(m_playerClass)];
        const u32 effFloor   = m_level.currentFloor + m_difficulty * 50;
        const f32 cdr        = m_inventories[m_localPlayerIndex].bonusCooldownReduction;
        const f32 pool       = m_skillStates[m_localPlayerIndex].energy;
        for (u8 s = 0; s < 4; s++) {
            const SkillId id = cls.skills[s];
            if (id == SkillId::NONE) continue;
            if (effFloor < cls.skillUnlockFloor[s]) continue;          // still locked on this floor
            const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, id);
            if (!def) continue;
            // AoE nature is a property of the skill (independent of whether it is castable THIS tick):
            // it hits a cluster if it throws shards, bounces between targets, fires multiple
            // projectiles, or has a real blast radius. The 3 m radius floor keeps a small self-blast
            // (Fireball, r2.5) classed as single-target filler so the group branch reaches for a real
            // pack-clearer (Frozen Orb r-shards, Meteor r5, Chain bounces).
            v.skillIsAoe[s] = (def->shardCount > 0) || (def->bounces > 0) ||
                              (def->projectileCount > 1) || (def->radius >= 3.0f);
            // A teleport/gap-close skill authors a dash `distance` (Holy Smite 3 m, Shadow Step 15 m);
            // damage skills leave it 0. That is the exact set the bot should use to close on a target.
            v.skillIsGapClose[s] = def->distance > 0.0f;
            // BLOOD_NOVA pays HEALTH, not energy (tryActivate refuses to suicide); everything else
            // draws the shared pool. Mirroring the split keeps the bot off a skill it can't afford.
            if (id == SkillId::BLOOD_NOVA) {
                if (m_localPlayer.health <= m_localPlayer.health * def->healthCostPct + 1.0f) continue;
            } else if (pool < def->energyCost) {
                continue;
            }
            if (!GameConst::cooldownReady(currentLocalTick(), m_classSkillStates[s].lastActivationTick,
                                          SkillSystem::computeCooldownTicks(def->cooldown, cdr)))
                continue;                                              // still on cooldown
            v.castableSkill[s] = true;
        }
    }

    // --- equipment legendary skills (boots F / helmet G): "would the press actually cast?" ---
    // MIRRORS handleEquipmentSkillActivation (engine_update_skills.cpp) gate for gate: the slot is
    // BOUND to a skill (that binding is derived there from a LEGENDARY item in the boots/helmet
    // slot, so reading the bound state single-sources it rather than re-deriving the rarity rule),
    // the shared energy pool covers the cost, and the tick cooldown has elapsed. The helmet is
    // additionally stun-gated; the boots deliberately are NOT, because BOOT_SKILL is the Break Free
    // rail and escaping a stun is the whole point of it.
    //
    // The binding is written by that handler LATER in the same tick, so this reads last tick's
    // value — one tick of lag on the frame a legendary is equipped, which no player can perceive
    // and which can only ever make the bot cast one tick late, never wrongly.
    {
        const f32 cdr  = m_inventories[m_localPlayerIndex].bonusCooldownReduction;
        const f32 pool = m_skillStates[m_localPlayerIndex].energy;
        auto castable = [&](const SkillState& ss) {
            if (ss.activeSkill == SkillId::NONE) return false;
            const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, ss.activeSkill);
            if (!def) return false;
            if (pool < def->energyCost) return false;
            return GameConst::cooldownReady(currentLocalTick(), ss.lastActivationTick,
                                            SkillSystem::computeCooldownTicks(def->cooldown, cdr));
        };
        v.bootCastable   = castable(m_bootSkillStates[m_localPlayerIndex]);
        v.helmetCastable = !v.stunned && castable(m_helmetSkillStates[m_localPlayerIndex]);
        // Is the bound legendary a teleport/gap-close (Phase Dash on the Swift Boots)? Read its dash
        // distance the same way the class-skill loop does, so the policy can cast it to CLOSE rather
        // than blink it past a target it is already on top of.
        auto isGapClose = [&](const SkillState& ss) {
            if (ss.activeSkill == SkillId::NONE) return false;
            const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, ss.activeSkill);
            return def && def->distance > 0.0f;
        };
        v.bootIsGapClose   = isGapClose(m_bootSkillStates[m_localPlayerIndex]);
        v.helmetIsGapClose = isGapClose(m_helmetSkillStates[m_localPlayerIndex]);
    }

    // Deterministic cadence clock for the strafe flip + the kiting jump (never rand()).
    v.tick = currentLocalTick();

    // --- inbound HOSTILE projectile: seconds to impact, soonest first -------------------------
    // What makes a shield raise land in the PERFECT window against an archer. Only `!fromPlayer`
    // shots are considered — reacting to our own would have the bot blocking its own bow — and only
    // ones actually CLOSING: the time of closest approach along the shot's own velocity must be in
    // the future, and the miss distance at that moment must be inside the player's body. That miss
    // test is what keeps the bot from turtling every time a stray bolt crosses the room.
    {
        v.incomingProjectileEta = 1e9f;
        const Vec3 centre = m_localPlayer.position + Vec3{0.0f, PLAYER_HEIGHT * 0.5f, 0.0f};
        // Body half-width plus a little: a projectile has its own radius, and the aim is to raise
        // slightly too often rather than miss a hit that was going to land.
        constexpr f32 kHitR = PLAYER_HALF_WIDTH + 0.35f;
        for (u32 a = 0; a < m_projectiles.activeCount; a++) {
            const Projectile& p = m_projectiles.projectiles[m_projectiles.activeList[a]];
            if (!p.active || p.fromPlayer) continue;          // ours, or a reaped slot
            const f32 v2 = lengthSq(p.velocity);
            if (v2 < 1.0f) continue;                          // effectively parked: no impact clock
            const Vec3 rel = centre - p.position;
            const f32  tca = dot(rel, p.velocity) / v2;       // time of closest approach
            if (tca <= 0.0f) continue;                        // already past us / moving away
            const Vec3 miss = rel - p.velocity * tca;         // offset at closest approach
            if (lengthSq(miss) > kHitR * kHitR) continue;     // it misses: nothing to block
            if (tca < v.incomingProjectileEta) v.incomingProjectileEta = tca;
        }
    }

    // --- weapon (effective, incl. affixes) ---
    // Mirror getEffectiveWeapon; MELEE/HITSCAN carry no projectile lead (projSpeed 0), only PROJECTILE.
    const WeaponDef w = Inventory::getEffectiveWeapon(m_inventories[0], m_itemDefs, m_weaponDefs[0]);
    // NOT w.range directly: projectile weapons author no range at all (see botWeaponRange), and a
    // 0 there zeroes the doctrine's whole engagement band, so the bot would never fire a wand/bow.
    v.weaponRange     = Autoplay::botWeaponRange(w.range, w.projectileSpeed);
    v.weaponProjSpeed = (w.type == WeaponType::PROJECTILE) ? w.projectileSpeed : 0.0f;
    v.weaponIsMelee   = (w.type == WeaponType::MELEE);
    v.buildCell       = m_inventories[0].buildCell;
    // While the ranged SIDEARM is worn, present a RANGED doctrine cell to the brain (same risk row,
    // Ranged column) so it holds ground and shoots instead of trying to walk a gun into melee reach.
    // The equipped weapon is already ranged (getEffectiveWeapon above set weaponRange/isMelee), so
    // only the doctrine column needs the swap. The PERSISTED buildCell (m_inventories[0].buildCell)
    // is untouched — this is a per-tick view override.
    if (m_autoplaySidearmActive) v.buildCell = Autoplay::rangedCellFor(v.buildCell);

    // --- world gate: idle in town / arena / the Source, and only travel while an ordinary exit exists ---
    v.onNormalFloor = !(m_level.inTown || m_level.inArena || m_level.inSourceChamber) && m_level.floorDoorActive;
    // Stacked styles carry walk-on slab storys, so "3 m above me" means "another floor of the
    // building" rather than "up a step" — the policy's cross-story target gate keys off this.
    v.stackedFloor  = (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL) ||
                      (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY);

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
        // The story fields' staleness stamp must be the floor's IDENTITY, not its NUMBER. Floor
        // numbers repeat — a new run's floor 9 is a different maze, and the difficulty ladder walks
        // the same numbers again on the next tier — while everything else the ensure* early-outs
        // compare matches in exactly those cases (stacked grids are FORCED sizes, storys sit on
        // fixed 3 m pitches, a VHALL door is upper half the time). A bare floor number therefore
        // resurrected the PREVIOUS maze's field and routed the bot on geometry that no longer
        // exists. This is the same seed fold startGame builds the dungeon from (levelSeed + floor +
        // difficulty), so it changes exactly when the geometry does and never otherwise.
        const u32 floorStamp = m_level.levelSeed
                             + m_level.currentFloor * 7919u
                             + static_cast<u32>(m_difficulty) * 104729u;
        // Default the climb-assist flag OFF every tick; only the VERTICAL_HALL climb branch re-arms
        // it. Set inside that branch alone, it would otherwise stay stale on the next (flat) floor
        // and pulse spurious jumps.
        // A WORLD-only horizontal ray (eye height) to a goal's XZ: true when no wall blocks a STRAIGHT
        // bearing there. The pad-climb, shrine detour and boss-seek all gate on it — a straight bearing
        // at a target behind a wall just jams the bot into the wall (the hazard veto's ±45/±90 fan
        // cannot route around a wall), so steer straight ONLY with a clear line and route around else.
        auto clearLineTo = [&](Vec3 goal) -> bool {
            const Vec3 eye = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
            const Vec3 to{goal.x - eye.x, 0.0f, goal.z - eye.z};
            const f32  len = length(to);
            if (len < 1e-3f) return true;
            const RayHit hit = Raycast::cast(m_level.grid, eye, to * (1.0f / len), len);
            return !hit.hit || hit.distance >= len - 0.5f;
        };

        m_autoplayVhClimbing = false;
        m_autoplayVhOnRamp   = false;   // set true below only when within hop range of the exit ramp
        if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL) {
            // Cache the floor's JUMP-PAD cells once (VHALL doesn't record them in jumpPads[], so a
            // grid scan is the only way to see them). Cluster-deduped so a 3x3 pad node is ONE goal.
            if (m_autoplayPadFloor != m_level.currentFloor) {
                m_autoplayPadFloor = m_level.currentFloor;
                m_autoplayPadCount = 0;
                for (u32 z = 0; z < m_level.grid.depth && m_autoplayPadCount < 8; z++)
                    for (u32 x = 0; x < m_level.grid.width && m_autoplayPadCount < 8; x++) {
                        if (!(LevelGridSystem::getCell(m_level.grid, x, z).flags & CELL_JUMPPAD)) continue;
                        const Vec3 c{(x + 0.5f) * m_level.grid.cellSize, 0.0f, (z + 0.5f) * m_level.grid.cellSize};
                        bool dup = false;   // fold a pad cluster's cells into one goal
                        for (u8 k = 0; k < m_autoplayPadCount; k++) {
                            const f32 dx = c.x - m_autoplayPadCells[k].x, dz = c.z - m_autoplayPadCells[k].z;
                            if (dx * dx + dz * dz < 9.0f) { dup = true; break; }   // within ~3 m => same pad
                        }
                        if (!dup) m_autoplayPadCells[m_autoplayPadCount++] = c;
                    }
            }

            // BELOW an UPPER exit: USE A JUMP PAD to climb. The void pad flings the bot up a story
            // reliably — that is what it is FOR — instead of fighting up the narrow 2-wide ramp (the
            // hard, flaky part of a VHALL climb). Route to the nearest reachable pad with a clear line;
            // the collision launch does the vertical. Once up (feet near the exit height) this stops
            // triggering and the VHallField takes over to cross the upper story to the door. Falls back
            // to the ramp route when no pad is in reach.
            const bool belowExit = m_level.floorDoorPos.y > 1.5f && pos.y < m_level.floorDoorPos.y - 0.5f;

            // RAMP CLIMB — an anti-drift assist on top of the VHallField (the user's call: "pathfind to
            // the ramp, approach it the right way"). See the block below for why it is scoped to "already
            // on a slab" rather than used to route TO the ramp.
            bool climbingViaRamp = false;
            if (belowExit && dg.portalCount > 0) {
                // Let the story-aware VHallField route EVERYTHING — the ground approach, which ramp to
                // climb (it routes ground -> foot -> up the slab -> balcony -> door as one shortest
                // path), and the balcony cross. The ONE thing it can't do is keep the eased-aim walk from
                // drifting off the narrow 2-wide graduated slab and sliding back (the original "94%
                // airborne, never crests" stall). So we ADD the centreline steer only as an anti-drift
                // assist, and ONLY once the bot is confirmed ON a slab (elevated, pos.y > 0.5) — never as
                // a router. Every attempt to route TO a ramp with it (by segment distance to a chosen
                // ramp, or to the ramp FOOT via a 2-D RouteField) regressed: it either trapped the bot on
                // the ground UNDER the slab, or delivered it to the foot XZ where the 2-D field has no
                // "step up" and it never mounted (measured: max_pos.y stuck at 0). Keying purely on "am I
                // already on a slab" and centring on the NEAREST ramp is the only version that mounts on
                // EVERY run. (The residual — it can climb a non-exit ramp and then must cross a catwalk it
                // falls off — is the open upper-story-crossing problem, tracked in the concept doc.)
                if (pos.y > 0.5f) {
                    s32 nr = -1; f32 brs = 1e18f;
                    for (u8 k = 0; k < dg.portalCount; k++) {
                        const f32 rs = Autoplay::rampSegDistXZ(dg.portals[k].lowPos, dg.portals[k].highPos, pos);
                        if (rs < brs) { brs = rs; nr = k; }
                    }
                    if (nr >= 0 && brs < 5.0f) {
                        const Vec3 rd = Autoplay::rampApproachDir(dg.portals[nr].lowPos, dg.portals[nr].highPos, pos);
                        if (lengthSq(rd) > 1e-6f) { v.flowDir = rd; climbingViaRamp = true; m_autoplayVhOnRamp = true; }
                    }
                }
            }

            // JUMP PAD fallback (only when NOT already on the ramp approach). The void pad flings the bot
            // up a story in one launch — used when it is reachable and the ramp is not close.
            bool climbingViaPad = false;
            if (!climbingViaRamp && m_level.floorDoorPos.y > 1.5f && pos.y < m_level.floorDoorPos.y - 1.5f) {
                constexpr f32 kPadReach = 18.0f;
                f32 bestD2 = kPadReach * kPadReach; s32 best = -1;
                for (u8 k = 0; k < m_autoplayPadCount; k++) {
                    const f32 dx = m_autoplayPadCells[k].x - pos.x, dz = m_autoplayPadCells[k].z - pos.z;
                    const f32 d2 = dx * dx + dz * dz;
                    if (d2 < bestD2 && clearLineTo(m_autoplayPadCells[k])) { bestD2 = d2; best = k; }
                }
                if (best >= 0) {
                    const Vec3 to{m_autoplayPadCells[best].x - pos.x, 0.0f, m_autoplayPadCells[best].z - pos.z};
                    if (lengthSq(to) > 1e-6f) { v.flowDir = normalize(to); climbingViaPad = true; }
                }
            }

            // The exit is a balcony door on the OPPOSITE story. Routing is a BFS FLOW FIELD over
            // (cell, STORY) nodes seeded from the door (autoplay_vhall.h) — it routes the whole journey
            // ground -> ramp foot -> up the ramp -> across the balcony -> door, and can never steer the
            // bot off a balcony edge. It does the COARSE approach to the ramp foot (before the centreline
            // steer takes over) and the cross to the door up top — used whenever not on the ramp/pad.
            if (!climbingViaPad && !climbingViaRamp &&
                Autoplay::ensureVHallField(m_autoplayVHall, m_level.grid, m_level.floorDoorPos, floorStamp)) {
                const Vec3 vd = Autoplay::vhallDirection(m_autoplayVHall, m_level.grid, pos);
                if (lengthSq(vd) > 1e-6f) v.flowDir = vd;
            }
            // Climb-assist jump: the ramp is a narrow 2-wide graduated slab, and even with correct
            // steering the eased-aim walk can stall against the risers. Pulse a hop while the bot is
            // BELOW the exit height and the exit is UP — i.e. still climbing the RAMP (not while riding a
            // pad, which does its own launch). (m_autoplayVhClimbing was defaulted false above.)
            if (!climbingViaPad && m_level.floorDoorPos.y > 1.5f && pos.y < m_level.floorDoorPos.y - 0.5f)
                m_autoplayVhClimbing = true;
        } else if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) {
            // The Descent: the exit is always DOWN, so the travel goal is a hole in THIS story's
            // slab — and getting to one is a MAZE routing problem, not a bearing.
            //
            // The route is a BFS FLOW FIELD seeded from this story's drop holes (autoplay_descent.h),
            // rebuilt only when the story or floor changes. A field rather than a bearing or an A*
            // leg because the direction it returns is always derived from a route that exists: it
            // can never point into a wall, it is defined on every reachable cell (so there is no
            // "no plan" tick where the bot stands and stares at a corner), and it steers at the next
            // cell's CENTRE, which pulls the body off the corridor walls instead of tracking along
            // them. On L0 there are no holes, the field reports invalid, and the heading stays the
            // ordinary exit flow field — which is exactly the walk to the door.
            // The storey the field routes on is HELD with hysteresis, not read raw each tick: raw
            // botStoryY is a knife-edge at a drop-hole lip (a few cm of XZ drift, or a 0.2 m feet-Y
            // dip, flips it a full storey to the ground below), and the field reseeds per storey with
            // the two seedings pointing opposite ways — which froze the bot oscillating at the very
            // hole it should drop into. commitBotStory only moves the storey once the bot is solidly
            // standing on a new one, so a lip flicker is ignored and a real fall commits on landing.
            m_autoplayDescentStory = Autoplay::commitBotStory(m_level.grid, pos, m_autoplayDescentStory);
            if (Autoplay::ensureDescentField(m_autoplayDescent, m_level.grid, dg,
                                             m_autoplayDescentStory, floorStamp, m_level.floorDoorPos)) {
                const Vec3 dd = Autoplay::descentDirection(m_autoplayDescent, m_level.grid, pos);
                if (lengthSq(dd) > 1e-6f) v.flowDir = dd;
            }
        }
        // Lava floors get no vertical goal — the veto below (lava-aware) keeps the bot off the lakes
        // and rides the stone causeways the flat flow field already routes along.

        // SHRINE DETOUR — grab a shrine on the way. A shrine is a free timed buff (power / speed /
        // vitality / spell) sitting in the level; steer travel onto the nearest active one within a
        // small detour radius so the bot picks it up in passing (activation is in updateAutoplay).
        // Folded into flowDir like the globe detour, so it only bites in TRAVEL — an LOS enemy still
        // takes priority (FIGHT ignores flowDir). FLAT non-lava floors only: on stacked/lava floors the
        // story/causeway routing above owns the heading, and the boss-seek below still overrides this
        // (kill the boss first), the globe steady after it (survival first).
        m_autoplayShrineTarget = false;
        if (!v.stackedFloor && !m_level.lavaFloor) {
            constexpr f32 kShrineDetour = 8.0f;   // metres: a minor detour, not a cross-floor trek
            f32 bestD2 = kShrineDetour * kShrineDetour;
            for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
                const WorldItem& w = m_worldItems.items[i];
                if (!w.active || !isShrine(w.item)) continue;
                const f32 dx = w.position.x - pos.x, dz = w.position.z - pos.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; m_autoplayShrinePos = w.position; m_autoplayShrineTarget = true; }
            }
            if (m_autoplayShrineTarget) {
                const Vec3 to{m_autoplayShrinePos.x - pos.x, 0.0f, m_autoplayShrinePos.z - pos.z};
                // Steer to the shrine ONLY with a clear line; a shrine is optional, never worth
                // wall-hugging toward one behind a wall. The flag stays set so the activation still
                // fires if the ordinary exit route happens to bring the bot into range.
                if (lengthSq(to) > 1e-6f && clearLineTo(m_autoplayShrinePos)) v.flowDir = normalize(to);
            }
        }

        // BOSS SEEK. On a boss floor the exit is SEALED until the milestone boss dies, and the exit
        // portal sits at the boss room's CENTRE — where the boss itself spawns. So the exit flow field
        // (a wall-aware BFS) routes the bot all the way INTO the arena; the only gap it leaves is the
        // last stretch across the open arena to a boss offset from the exit cell. So the boss-seek only
        // does that last stretch: it steers STRAIGHT at the boss ONLY when there is a CLEAR LINE to it
        // (i.e. we're inside the open arena). Through a WALL the straight bearing just jams the bot into
        // it — the exit flow field routes AROUND walls and must be kept until the boss is actually in
        // sight. This is the "it wants to navigate to the boss even behind a wall" freeze (measured:
        // frozen 8 m from the boss with flow vetoed to zero against the wall between them).
        // (This scan runs only on boss floors, and breaks on the one boss, so it is nearly free.)
        if (m_level.floorHasBoss) {
            Vec3 bossPos{}; bool haveBoss = false;
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                const Entity& e = m_entities.entities[m_entities.activeList[a]];
                if (e.isBoss && !(e.flags & ENT_DEAD)) { bossPos = e.position; haveBoss = true; break; }
            }
            if (haveBoss) {
                const Vec3 toBoss{bossPos.x - pos.x, 0.0f, bossPos.z - pos.z};
                const f32  dBoss = length(toBoss);
                constexpr f32 kBossSeekRadius = 25.0f;   // covers a major boss's expanded arena
                const bool flowIdle = lengthSq(v.flowDir) < 1e-6f;
                const bool wantSeek = (dBoss < kBossSeekRadius || v.atExit || flowIdle) && dBoss > 1e-3f;
                if (wantSeek) {
                    if (clearLineTo(bossPos)) {
                        // Open arena, no wall between us: a straight bearing is the snappiest way to
                        // close on (and catch a kiting) boss.
                        v.flowDir = normalize(toBoss);
                    } else {
                        // A WALL is in the way. A straight bearing here jams into it (the reported
                        // "trying to get to the boss through a wall", every build). Route via a WALL-
                        // AWARE BFS flow field seeded from the boss (autoplay_route.h) — the same proven
                        // primitive the exit / Descent / VHall fields use: it routes the whole way around
                        // walls, is defined on every reachable cell (no straight-line dead ends), steers
                        // at cell centres (no wall-hug), has no A* cell cap (a boss across the floor still
                        // routes), and rebuilds only when the boss changes cell (a moving target). Falls
                        // back to the exit field only if the boss is genuinely unreachable.
                        if (Autoplay::ensureRouteField(m_autoplayBossRoute, m_level.grid, bossPos, floorStamp)) {
                            const Vec3 rd = Autoplay::routeDirection(m_autoplayBossRoute, m_level.grid, pos);
                            if (lengthSq(rd) > 1e-6f) v.flowDir = rd;
                        }
                    }
                }
            }
        }

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
    // a wall, off the map, or grounded into lava. Try the heading first, then a widening fan of
    // detours, else stop (the driver's stuck-override in updateAutoplay recovers a boxed-in bot).
    //
    // The fan goes out to ±90°, not just ±45°, BECAUSE of the veto's corner-cut rule: when a CARDINAL
    // heading is blocked by a wall dead ahead, both ±45° detours are diagonals whose orthogonal
    // component includes that very wall cell — so they are (correctly) refused too, and a ±45°-only
    // ladder would leave the bot with no heading at all and hand every wall-ahead to the 4-second
    // stuck-override. ±90° is the square sidestep: it rounds the corner along the grid instead of
    // scraping through it, which is the whole point of the corner rule.
    if (lengthSq(v.flowDir) > 1e-6f) {
        const f32 feetY = m_localPlayer.position.y;
        // JUMP PADS ARE A HAZARD ON A DESCENT FLOOR, and only there. The objective is to get DOWN;
        // a pad is the one piece of terrain that reverses that, and the maze is seeded with them
        // (every dead-end node, plus a return lift under ~1 in 3 drop holes). They fire the instant
        // the bot is grounded and lift about two stories, so walking over one throws away the whole
        // descent — measured, 25-27 unplanned climbs per 150 s run before this, with the bot pinned
        // at 63% of its time on a single story. The carve-out matters as much as the rule: the veto
        // tests the DESTINATION cell, so a bot that has landed inside a 3x3 pad node would find
        // every neighbour refused and box itself in, and the stuck ladder would be left to dig it
        // out of a trampoline. While it is standing on one, the veto stands down so it can leave.
        // ...and it stands down on a storey whose ONLY ways down are return lifts (paddedOnly), or
        // the veto would refuse the last step into the single hole available and leave the bot
        // circling something it is not allowed to enter. Hole density thins to 7% on the deepest
        // storey, so that state is rare but real.
        // ...and when the field's NEXT routed step IS a pad (descentNextIsPad): the tier-2 recovery
        // routes THROUGH a pad only when a pad severs the pocket / blocks the exit corridor, and there
        // the veto would freeze the bot at the pad it must cross. Standing down lets it take the bounce
        // and re-route, instead of standing still (the "can't find the exit when a pad is in the way").
        const bool avoidPads = (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) &&
                               !Autoplay::onJumpPad(m_level.grid, v.pos) &&
                               !m_autoplayDescent.paddedOnly &&
                               !Autoplay::descentNextIsPad(m_autoplayDescent, m_level.grid, v.pos);
        if (!Autoplay::stepAllowed(m_level.grid, v.pos, feetY, v.flowDir, m_level.lavaFloor, avoidPads)) {
            constexpr f32 kFan[4] = { 0.7853981634f, -0.7853981634f,     // ±45°: the gentle detour
                                      1.5707963268f, -1.5707963268f };   // ±90°: the square sidestep
            Vec3 pick{0, 0, 0};
            for (u32 i = 0; i < 4; i++) {
                const Vec3 cand = rotateY_XZ(v.flowDir, kFan[i]);
                if (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, cand, m_level.lavaFloor, avoidPads)) { pick = cand; break; }
            }
            if (lengthSq(pick) > 1e-6f) v.flowDir = pick;
            else                        v.flowDir = Vec3{0, 0, 0};   // boxed in: stop (stuck-override recovers)
        }
    }

    // --- descend gate context (consumed by the brain's mayDescend mirror) ---
    v.doorActive  = m_level.floorDoorActive;
    v.distToDoor  = length(m_level.floorDoorPos - m_localPlayer.position);
    v.hasBoss     = m_level.floorHasBoss;
    v.bossAlive   = floorBossAlive();

    // --- targets: nearest-first hostiles, then a WORLD-ONLY LOS test from the bot's eye ---
    // TWO PASSES on purpose. Pass 1 gathers the nearest kMaxTargets hostiles; pass 2 raycasts only
    // those survivors, so a floor holding 90 enemies (the Stacked Loop) pays 16 casts instead of 90 —
    // the LOS used to be computed for every candidate and then thrown away by the cap.
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
        // Stable identity across ticks (the array is re-sorted every tick, so the index is not one).
        // +1 on the index so a valid handle can never pack to 0 (= "unset").
        t.id     = (static_cast<u32>(e.generation) << 16) | (m_entities.activeList[a] + 1u);
        t.pos    = e.position;               // AABB centre (aim point)
        t.vel    = Vec3{e.velocity.x, 0.0f, e.velocity.z};   // XZ only, for projectile lead
        t.dist   = length(e.position - eye);
        t.hp     = e.health;
        t.isBoss = e.isBoss;
        // Threat timing. `attackRange > 5` is the enemy AI's OWN ranged test (enemy_ai_states.cpp),
        // reused verbatim so the bot's idea of "that one shoots at me" can't drift from the AI's.
        // attackTimer counts DOWN to the next swing, so it is handed over as-is.
        t.isRanged    = e.attackRange > 5.0f;
        t.attackRange = e.attackRange;
        t.attackTimer = e.attackTimer;
        // FEET, not the AABB centre: the story comparison below (and the policy's cross-story gate)
        // wants the surface the body is standing on, the same quantity snapEntityToFloor writes.
        t.feetY       = e.position.y - e.halfExtents.y;
        t.isFlying    = (e.flags & ENT_FLYING) != 0;   // hovers by design: exempt from the story gate
        t.isLootGoblin = (e.flags & ENT_LOOT_GOBLIN) != 0;   // flees with loot: rush it above all else
        // Currently DAMAGE-IMMUNE — mirror Combat::applyDamage's early returns so the bot never wastes
        // shots (or, for a gargoyle, keeps it asleep by staring). A dormant AMBUSH gargoyle, an entombed
        // boss (Malachar's channel), the Engine while its wave adds live. minionShield is NOT here: it
        // is only 75% reduction, not immunity, so a shielded boss is still worth shooting.
        t.invulnerable = (e.aiState == AIState::DORMANT && (e.enemyRole & EnemyRole::AMBUSH)) ||
                         (e.bossPhase == BossPhase::ENTOMBING) ||
                         (e.isEngine && Combat::engineShieldActive(m_entities, m_entities.activeList[a]));
        t.bossShielded = e.isBoss && e.minionShield;   // 75% DR while its brood lives: kill the adds first

        // Insert nearest-first into the fixed cap (simple insertion — the pool is small).
        u32 pos = n;
        if (n < kMaxTargets) n++;
        else if (t.dist >= s_targets[kMaxTargets - 1].dist) continue;   // full + farther: drop
        else pos = kMaxTargets - 1;
        while (pos > 0 && s_targets[pos - 1].dist > t.dist) { s_targets[pos] = s_targets[pos - 1]; pos--; }
        s_targets[pos] = t;
    }
    // LOS pass — WORLD GEOMETRY ONLY. This used to call CombatQuery::raycast (which sweeps the world
    // AND every entity AABB) and read "the nearest hit was not WORLD" as clear line. That is wrong the
    // moment ANOTHER ENEMY stands between the bot and an occluding wall: the nearest hit becomes an
    // ENTITY, the wall behind it stops counting as an occluder, and the bot "sees" — and shoots —
    // straight through the wall. Raycast::cast is the bare slab-aware grid DDA (the same primitive the
    // melee cone's LOS gate and the enemy AI's hasLOSToPoint use), so only real geometry can block and
    // a body in the way can never hide one. (Whether an intervening enemy should block the SHOT is a
    // separate question — the projectile hits it, which is fine; the bug was the vanishing wall.)
    // The 0.1 m slack mirrors hasLOSToPoint: a hit at/after the target's own centre is not an occluder.
    for (u32 i = 0; i < n; i++) {
        const Vec3 toT = s_targets[i].pos - eye;
        const f32  d   = length(toT);
        if (d < 1e-4f) { s_targets[i].hasLOS = true; continue; }   // on top of it
        const RayHit hit = Raycast::cast(m_level.grid, eye, toT * (1.0f / d), d);
        s_targets[i].hasLOS = (!hit.hit || hit.distance >= d - 0.1f);
    }
    // PROJECTILE-LEAD VELOCITY SMOOTHING. Replace each target's raw per-frame velocity with an
    // exponential average of it before the brain leads a shot with it. See engine.h for the measured
    // reason: an enemy FSM rewrites velocity every frame, and the lead multiplies that noise by
    // timeToHit (0.5-1.5 s at ranged distances), so the raw value shakes the crosshair by an order
    // of magnitude more than the target's actual bearing moves.
    //
    // This is NOT the low-pass on the desired aim that was deliberately rejected earlier (which
    // would add a second lag stage in series with the ease and push the steady-state tracking error
    // past FIRE_ALIGN_RAD, muting fire on crossing targets). The target's BEARING stays instantaneous
    // — only the velocity ESTIMATE is filtered, and a short average is a strictly better estimate of
    // sustained motion than a single frame's sample, so the lead gets more accurate, not laggier.
    {
        constexpr f32 kTau = 0.15f;   // s; ~0.1 s to track a genuine direction change, kills 60 Hz noise
        const f32 alpha = 1.0f - expf(-(f32)FIXED_DT / kTau);   // frame-rate correct, like the aim ease
        u32  freshId[AIM_VEL_SLOTS]  = {};
        Vec3 freshVel[AIM_VEL_SLOTS] = {};
        for (u32 i = 0; i < n && i < AIM_VEL_SLOTS; i++) {
            const Vec3 raw = s_targets[i].vel;
            Vec3 sm = raw;                                   // unseen target: seed with its raw value
            for (u32 j = 0; j < AIM_VEL_SLOTS; j++) {
                if (m_autoplayVelId[j] != s_targets[i].id || m_autoplayVelId[j] == 0) continue;
                sm = m_autoplayVelEma[j] + (raw - m_autoplayVelEma[j]) * alpha;
                break;
            }
            freshId[i] = s_targets[i].id; freshVel[i] = sm;
            s_targets[i].vel = sm;
        }
        // Rebuilt wholesale each tick, so a target that left the list simply drops its history —
        // which is what we want: re-acquiring it later should not lead on a stale velocity.
        for (u32 j = 0; j < AIM_VEL_SLOTS; j++) { m_autoplayVelId[j] = freshId[j]; m_autoplayVelEma[j] = freshVel[j]; }
    }

    v.targets     = s_targets;
    v.targetCount = n;

    // TARGET STICKINESS: resolve the remembered entity identity back to a slot in THIS tick's array
    // (it is re-sorted by distance every tick, so the index from last tick means nothing). Not found
    // = the enemy died, despawned, or fell out of the nearest-kMaxTargets cap — either way the memory
    // is stale and pickTarget falls back to plain nearest-LOS.
    v.currentTargetIdx = -1;
    if (m_autoplayTargetId != 0) {
        for (u32 i = 0; i < n; i++)
            if (s_targets[i].id == m_autoplayTargetId) { v.currentTargetIdx = (s32)i; break; }
    }
    v.targetSwitchAllowed = m_autoplayTargetDwell >= Autoplay::TARGET_MIN_DWELL;

    // (globes were collected above, before the nav steer that consumes them.)
    return v;
}

// Translate one BotIntent into a yaw/pitch write + synthetic held GameActions. Clears last tick's
// held set first (so a no-longer-wanted action releases), then arms exactly this tick's actions.
// When uiOpen, the movement/nav actions are SUPPRESSED (see below) but combat is kept.
void Engine::applyBotIntent(const Autoplay::BotIntent& in, bool uiOpen, f32 dt, bool melee) {
    Input::clearBotHeld();

    // --- AIM: EASED and rate-limited, not snapped -------------------------------------------------
    // The intent carries the DESIRED aim (lead-corrected, exactly as the pure policy computed it).
    // Writing it straight onto the player made the bot's head teleport onto every new target — the
    // aimbot tell. Instead we EASE toward it (speed proportional to the remaining error) under a
    // speed cap, with a sub-degree deterministic wobble laid on top so shots are human-imperfect
    // rather than mathematically centred.
    //
    // These four numbers are FEEL values, not physics. Aaron watched the first (rate-capped-only,
    // 7/14 rad/s) pass and asked for the aim to "move smoother and less rapidly" — it still read as
    // an aimbot: ~400 deg/s of fine tracking is faster than a person tracks, and the flick was
    // ~800 deg/s. Everything below is tuned toward "a person leading a target", then measured live
    // to confirm the bot still clears floors (see the tune commit message for the A/B numbers).
    //
    // GAIN drives the ease-out (stepAngle integrates it exactly, so it is tick-rate independent).
    // It is what governs the LAST stretch: at 10 deg of error it turns ~60 deg/s and needs ~0.4 s
    // to close to 1 deg — a visible settle instead of a stop-dead. It also sets the steady-state
    // TRACKING LAG on a moving target (lag = target's angular rate / gain), which is the real cost
    // of lowering it: at 6 /s a target crossing at 0.4 rad/s sits ~3.8 deg off centre, still inside
    // a body at normal engagement range.
    constexpr f32 kAimGain      = 6.0f;    // 1/s: error-proportional approach (tau ~ 0.17 s)
    // TURN RATE caps the far field, where gain alone would still be a teleport (a 180 deg error at
    // gain 6 starts at ~19 rad/s). Two-point, because one constant cannot serve both jobs: fine
    // tracking wants to be slow enough to SEE, while acquiring something that just walked in behind
    // you wants a flick — a human does both, and a single fine-rate turn of 180 deg would take
    // ~1.1 s, long enough for the bot to eat a free hit every time something spawns at its back.
    // Both are roughly HALF the first pass's caps.
    constexpr f32 kAimTurnFine  = 2.8f;    // rad/s (~160 deg/s): tracking something already in view
    constexpr f32 kAimTurnFlick = 5.6f;    // rad/s (~320 deg/s): full-speed acquisition
    constexpr f32 kFlickError   = 1.0f;    // rad (~57 deg): error at/above which the flick rate applies

    f32 wobbleYaw, wobblePitch;
    Autoplay::aimWobble(currentLocalTick(), wobbleYaw, wobblePitch);
    const f32 desiredYaw   = in.aimYaw   + wobbleYaw;
    const f32 desiredPitch = in.aimPitch + wobblePitch;

    const f32 err  = fabsf(Autoplay::angleDelta(m_localPlayer.yaw, desiredYaw));
    const f32 lerp = (err >= kFlickError) ? 1.0f : (err / kFlickError);
    const f32 rate = kAimTurnFine + (kAimTurnFlick - kAimTurnFine) * lerp;

    // DEADZONE FIRST. Inside AIM_DEADZONE_RAD the aim simply HOLDS — see the constant for why
    // reversals, not magnitude, are what read as "shaky". Yaw and pitch are gated independently so a
    // settled yaw doesn't freeze a pitch that still has real work to do (and vice versa).
    if (!Autoplay::aimWithinDeadzone(m_localPlayer.yaw, desiredYaw))
        m_localPlayer.yaw = Autoplay::stepAngle(m_localPlayer.yaw, desiredYaw, kAimGain, rate, dt);
    // Pitch rides the same ease + cap (no wrapping needed — stepAngle's fold is a no-op inside ±89°).
    f32 pitch = m_localPlayer.pitch;
    if (!Autoplay::aimWithinDeadzone(pitch, desiredPitch))
        pitch = Autoplay::stepAngle(pitch, desiredPitch, kAimGain, rate, dt);
    // Clamp to the same ±89° applyMovement enforces (a straight-down/up aim would gimbal look).
    constexpr f32 kMaxPitch = 89.0f * 3.14159265f / 180.0f;
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
    // FIRE is gated on the crosshair having ACTUALLY ARRIVED (Autoplay::aimOnTarget). The policy
    // decides `fire` from the DESIRED aim, but the ease above means the real crosshair is still
    // sweeping toward it — and everything it sweeps across is what the bot was shooting. Compared
    // against `in.aim*` (the true target direction) rather than the wobbled desired: the wobble is
    // deliberate imprecision we ACCEPT, not an error to converge on. Melee relaxes the tolerance and
    // drops the pitch term — its swing is a wide horizontal cone (see the constants).
    const bool onTarget = Autoplay::aimOnTarget(m_localPlayer.yaw, m_localPlayer.pitch,
                                                in.aimYaw, in.aimPitch, melee);
    Input::setBotHeld(GameAction::FIRE,   in.fire && onTarget);
    Input::setBotHeld(GameAction::BLOCK,  in.block);
    Input::setBotHeld(GameAction::DODGE,  in.dodge);
    Input::setBotHeld(GameAction::POTION, in.potion);
    Input::setBotHeld(GameAction::RELOAD, in.reload);
    // Class skill: select the slot (SKILL_n) AND press CLASS_SKILL — the selection loop runs before
    // the activation in handleClassSkillActivation, so both land in one frame.
    //
    // PULSED, not held. Activation is EDGE-triggered (isActionPressed), so a continuously-HELD button
    // casts exactly once and then does nothing until released. And the bot WOULD hold it forever:
    // some class skill (cheap, low-cooldown Fireball) is almost always castable, so classSkillSlot
    // stays >= 0 every engaging tick and the button never releases — which is why the Sorcerer cast
    // once per fight and read as "not aggressive enough". Pressing only on even ticks (clearBotHeld
    // releases it on the odd ones) makes every other tick a fresh press edge — 30 edges/s, far above
    // any skill's cooldown — so the engine's own per-skill cooldown becomes the true cast rate and a
    // caster fires Frozen Orb / its nukes as fast as they come up. (BOOT/HELMET skills need no pulse:
    // each is ONE slot that self-releases the instant it goes on cooldown.)
    if (in.classSkillSlot >= 0 && (currentLocalTick() & 1u) == 0u) {
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
    // The sidearm state machine only ticks inside the bot loop, so a run that ends while the
    // sidearm is drawn would otherwise leak it past the exit: the flag stayed set, the melee weapon
    // stayed in the bag, and — because the auto-equip suppression keyed on the flag alone — every
    // later NORMAL Auto-Loot game in the same process had lane-0 re-gearing silently dead. Put the
    // melee weapon back (same uid search as the stow path; slots move as loot comes and goes) and
    // clear the state with the rest of the disarm.
    if (m_autoplaySidearmActive) {
        PlayerInventory& inv = m_inventories[0];
        for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++)
            if (inv.backpack[i].defId != 0xFFFF && inv.backpack[i].uid == m_autoplaySidearmMeleeUid) {
                Inventory::equip(inv, i, m_itemDefs);
                break;
            }
        m_autoplaySidearmActive = false;
    }
    m_autoplayActive = false;
    Input::setBotOverlayActive(false);   // also clears any held synthetic actions (input.cpp)
    m_autoplayRespawnTimer = 0.0f;
}

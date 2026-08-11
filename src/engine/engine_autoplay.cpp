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
// DODGE LEASHES (ap().dodgeCd / ap().gapCloseCd — the engine's 1 s dodge cooldown is a balance
// number, and a bot that rolls whenever it is legal reads as panic) and the STICKY TARGET (the engaged
// enemy's identity + how long it has been engaged, so the crosshair stops flipping between similar-range
// hostiles). Both reach the policy as plain booleans/indices on BotView, keeping the brain engine-free.
//
// AIM STEADINESS is a driver concern for the same reason — it is all MEMORY. The bot's camera IS the
// player camera, so a desired aim that jumps is a screen that shakes, and the measurement said the jumps
// were never "jitter" in any single signal: they were the aim's SOURCE changing. Three pieces of state
// answer that, and all three live here: the TARGET LOS GRACE (ap().targetBlind — a target's LOS
// raycast flickers, and releasing on the flicker threw the brain between FIGHT and TRAVEL ~25 times a
// second), the TRAVEL-HEADING COMMIT (ap().travelDir/Hold — the flow byte and the detour fan both
// toggle across a cell boundary), and the aim DEADZONE in applyBotIntent. Measured on paired 2-minute
// live runs: mean |per-tick change of the desired yaw| 5.0 deg -> 1.9, applied-yaw direction reversals
// 4.5/s -> 1.2 (marksman); 2.8 -> 2.2 and 1.6/s -> 0.9 (warrior).
#include "engine/engine.h"
#include "game/zone_route.h"
#include "game/free_play.h"
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
#include <cstdlib>              // getenv/atof — AUTOPLAY_STALL_SEC, the stall-autopsy threshold override

#include "game/weapon_throw.h"    // WeaponThrow::botShouldThrow / botTap* — the bot's weapon-throw tap
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
// `commitWalk`: after the strafe has had its chance (deep no-progress), WALK the hint heading while
// still aiming and firing at the target — feet and guns are independent, exactly as kiting proves.
// The strafe-only default is right for the common body-blocker orbit; commitWalk is the escalation
// for the false-LOS standoff, where the strafe axis (perpendicular to the aim) can be exactly the
// walled axis of a pocket and the bot side-steps into walls forever while its escape heading — which
// points at the open route — is used for nothing but picking the strafe side (the 7102 s gauntlet
// livelock, couch_soak8).
static Autoplay::BotIntent unstickCombatMove(const Autoplay::BotView& v, Vec3 hint,
                                             const LevelGrid& grid, f32 feetY, bool lavaFloor,
                                             Vec3 anchor, Vec3 selfPos, f32 selfYaw,
                                             bool commitWalk) {
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
        if (commitWalk && havePref) {
            // Deep escalation: decompose the escape heading onto the aim basis (the vh-commit /
            // faceAndGo convention) so the feet WALK the route while the guns stay on the target.
            const Vec3 fwdW{-sy, 0.0f, -cy};
            const f32 df = dot(pref, fwdW), dr = dot(pref, rightW);
            constexpr f32 kAxis = 0.35f;               // ~20°, matches every other decomposition
            out.moveFwd   = df >  kAxis;
            out.moveBack  = df < -kAxis;
            out.moveRight = dr >  kAxis;
            out.moveLeft  = dr < -kAxis;
            return out;
        }
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
    if (ap().dodgeCd    > 0.0f) ap().dodgeCd    -= dt;
    if (ap().gapCloseCd > 0.0f) ap().gapCloseCd -= dt;

    // TOWN: its own tiny policy, entirely on this side. The pure brain idles OFF a normal floor
    // (`onNormalFloor` covers town + arena + Source chamber) and the town's flow field points at the
    // plaza centre rather than the portal, so an armed run parked at the hub forever. Only the TOWN
    // is claimed here: the ARENA (a progression firewall — no XP, no loot, no saves) and the SOURCE
    // CHAMBER (the secret boss you opt into) still idle exactly as before, and the brain itself stays
    // flat-floor pure — no town concept ever reaches it.
    if (m_level.inTown) { autoplayTownStep(dt, uiOpen); return; }

    // THE OVERWORLD IS PLAYED, NOT REFUSED. The bot used to end its run on entering a zone, because
    // an act has no descent objective and a bot standing in one would idle forever — the town and
    // credits strands in a new costume. It now has an objective: the quest chain (game/zone_route.h),
    // presented to the brain as an ordinary floor door (engine_autoplay_zone.cpp). The run ends only
    // when both acts are finished, or when the route is genuinely stranded — and says which.
    //
    // ORDERING IS LOAD-BEARING — this must stay BELOW the botInControl()/botMayAct() gates.
    // It used to be the FIRST statement in this function, above them, which meant it fired while
    // the HUMAN held control. "Armed" and "driving" are very different states: the takeover latch
    // hands control to the player on the first input it sees, so an armed run under human control
    // is the NORMAL way a person watches the bot and then takes the wheel. Reported from the
    // Switch: a player walked their own endgame ROGUE into Act 1 — whose quests that character had
    // already finished, so objectiveZone() is 0 and this returns true on the very first tick in the
    // zone — and the game rolled the hero into a fresh run: class rotated by one (Rogue 3 ->
    // Paladin 4), difficulty reset to Normal, floor 1, on a new save slot. The character survived
    // (saveAllCharacters runs first) but the player was thrown out of the world they had just
    // entered. A branch that mints a new character belongs behind the same gate as every other
    // thing the bot does.
    if (m_level.inZone && zoneAutoplayStep()) {
        // FINISHING THE ACTS MUST NOT PARK THE BOT. exitAutoplayRun only disarms the driver — it
        // does not leave the world — so on its own it left the hero standing in Hellgate: Localhost
        // with nobody driving, forever. Measured: a marksman completed both acts, logged "ACTS
        // COMPLETE", and then stood in the rift for the remaining 17 minutes of the soak, emitting
        // no [ZBOT] at all. That is the credits park and the death-screen strand for a third time,
        // and again it is the BEST outcome the mode can produce that stops it playing.
        //
        // So the acts end the way the standard ending does: mint the next run. Same three rules —
        // save the champion FIRST (autoplayNextRun moves the lane onto a fresh slot, so this is the
        // last chance to write the hero that just finished), and singleplayer only, because a host
        // silently re-rolling a dungeon would strand its guests.
        //
        // A STRANDED route rolls on too, deliberately: zoneAutoplayStep has already logged the WARN
        // that names the broken link, so nothing is hidden, and a bot that keeps playing is strictly
        // better for a soak than one parked in a corner of a zone it cannot leave.
        //
        // SECOND GUARD: a hero the PLAYER loaded is never rolled over. autoplayNextRun rotates the
        // class and starts a fresh Normal floor-1 run — right for a soak, which MINTED the character
        // it is replacing, and wrong for someone's saved hero, who would come back to a different
        // class. Soaks are unaffected because they launch --new/--endgame, so the lane is not
        // loaded-from-save. Such a run ENDS instead (the hero keeps standing where they are, with
        // the acts finished and saved) — parking a bot is the lesser evil when the alternative is
        // replacing a character the player owns.
        const bool playerOwnedHero = m_laneLoadedFromSave[m_localPlayerIndex];
        saveAllCharacters();   // the acts are finished either way — persist that before deciding
        if (m_netRole == NetRole::NONE && !playerOwnedHero) {
            autoplayNextRun();
        } else {
            if (playerOwnedHero)
                LOG_INFO("[AUTOPLAY] acts finished on a LOADED hero — ending the run rather than "
                         "rolling it over (the character belongs to the player)");
            exitAutoplayRun();
        }
        return;
    }


    // --- BALANCE TELEMETRY (playtest rig). One `[TELEM]` line per floor completed + a 30 s heartbeat,
    // so a soak is a balance dataset: how long each floor took, deaths/kills on it, and the player's
    // power (HP / sustained weapon DPS / gear score) against the effective floor (raw + difficulty*50).
    {
        // WALL-CLOCK ACCUMULATORS ARE GLOBAL, BUT updateAutoplay RUNS ONCE PER LOCAL LANE.
        // In couch co-op that made every duration in the telemetry advance at 2x real time: a 3 h
        // soak reported elapsed=21525 s, the "30 s" heartbeat fired every 15 s, per-floor dwell read
        // double, and the STALL autopsy's 5-minute gate tripped after 2.5 real minutes. The counters
        // are deliberately shared (one run, one floor, one clock — see the couch state split in
        // engine.h), so the fix is to advance them exactly once per FRAME rather than per lane.
        // Everything downstream (the [TELEM]/[TELEM-HB] lines, the stall gate) is then in real seconds.
        if (m_localPlayerIndex == 0) { m_autoplayRunTime += dt; m_autoplayFloorTime += dt; m_autoplayHbTimer += dt; }
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
            // wedges = escapes fired on THIS floor. A long secs_fl with a climbing wedge count is a
            // bot fighting geometry; a long one with zero is a bot fighting enemies. That single
            // distinction is what cost several 25-minute instrumented runs to establish, so it is
            // worth a field in the heartbeat rather than a rebuild with a tracer.
            LOG_INFO("[TELEM-HB] cls=%s fl=%u eff=%u elapsed=%.0f secs_fl=%.0f deaths=%u kills=%u hp=%.0f/%.0f wdps=%.0f wedges=%u",
                     cls, m_level.currentFloor, m_level.currentFloor + m_difficulty * 50u, m_autoplayRunTime,
                     m_autoplayFloorTime, m_autoplayDeaths, m_totalKills[0], m_localPlayer.health,
                     m_localPlayer.maxHealth, weaponDps(), ap().wedgeCount);
        }
    }

    Autoplay::BotView v = buildBotView();

    // TARGET LOS GRACE (aim steadiness). buildBotView has just resolved the sticky target's slot;
    // time how long it has been BLIND so the pure pickTarget can ride out a flicker instead of
    // releasing on it. A single raycast to a target's centre from a moving eye toggles constantly
    // (measured: 45-57 of every 60 ticks in a corridor fight) and each release dropped the brain out
    // of FIGHT into TRAVEL, swinging the desired aim ~55° some 25 times a second — the camera shake.
    if (v.currentTargetIdx >= 0 && !v.targets[(u32)v.currentTargetIdx].hasLOS)
        ap().targetBlind += dt;
    else
        ap().targetBlind = 0.0f;
    v.targetBlindGrace = ap().targetBlind <= Autoplay::TARGET_LOS_GRACE;

    // Floor-type facts shared by several blocks below. VHALL-UPPER-EXIT is the "protect the climb"
    // scope (the fall veto + the commit's edge release); the pad carve-outs mirror the travel veto
    // in buildBotView so a COMMITTED heading is re-judged by the same rules the fresh one passed.
    const bool vhallUpperExit = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL &&
                                m_level.floorDoorPos.y > 1.5f;
    const bool commitAvoidPads = m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY &&
                                 !Autoplay::onJumpPad(m_level.grid, m_localPlayer.position) &&
                                 !ap().descent.paddedOnly;

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
    // Bypassed wholesale while the VHALL follower owns travel: the NODE latch is the anti-toggle
    // (a stable point target, not a held direction), and a 0.4 s stale-heading replay across a rim
    // is this commit's own documented residual failure ("climbs then drops"). Everything else keeps
    // the commit unchanged.
    if (m_level.layoutStyle != LevelGen::LayoutStyle::VERTICAL_HALL)
    {
        constexpr f32 kTravelCommitSec = 0.40f;   // ~2.4 m at walking speed: a cell or two
        constexpr f32 kRouteReversed   = -0.5f;   // dot < this = more than 120° apart
        if (ap().travelHold > 0.0f) ap().travelHold -= dt;
        const bool haveFresh = lengthSq(v.flowDir) > 1e-6f;
        const bool haveHeld  = lengthSq(ap().travelDir) > 1e-6f;
        if (!haveFresh) {                                     // at the exit / boxed in: drop the commit
            ap().travelDir = Vec3{0, 0, 0}; ap().travelHold = 0.0f;
        } else if (ap().travelHold > 0.0f && haveHeld &&
                   dot(ap().travelDir, v.flowDir) > kRouteReversed &&
                   Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, m_localPlayer.position.y,
                                         ap().travelDir, m_level.lavaFloor, commitAvoidPads) &&
                   !(vhallUpperExit &&
                     Autoplay::wouldFall(m_level.grid, m_localPlayer.position,
                                         m_localPlayer.position.y, ap().travelDir))) {
            v.flowDir = ap().travelDir;                  // keep walking the committed heading
        } else {
            ap().travelDir  = v.flowDir;                 // re-commit to this tick's choice
            ap().travelHold = kTravelCommitSec;
        }
    }

    Autoplay::BotIntent in = Autoplay::decide(v);
    // SURVIVE is sacred: the remedy chain below rewrites `in` wholesale (the exit bull, the escape
    // ladder, look-behind), which would drop a potion the brain wanted at low HP. Capture the desire
    // here and re-assert it after the remedies so a COMMITTED shove to the door (which now persists
    // through a swarm) can never march the bot to its death holding a potion it never drank.
    const bool decidedPotion = in.potion;

    // TARGET STICKINESS bookkeeping. Re-run the (pure, cheap — a scan of <= 16 slots) pick with the
    // same view the brain just used, so the driver learns WHICH hostile was engaged and can carry
    // that identity into the next tick. Same target => the dwell accumulates and eventually unlocks
    // a switch; a different one (or none) => reset, so the next switch has to earn its dwell again.
    {
        const s32 chosen = Autoplay::pickTarget(v, Autoplay::doctrineFor(v.buildCell));
        const u32 chosenId = (chosen >= 0) ? v.targets[(u32)chosen].id : 0u;
        if (chosenId != ap().targetId) { ap().targetId = chosenId; ap().targetDwell = 0.0f; }
        else                                 ap().targetDwell += dt;
    }

    // CHARGE the leash on the REQUEST, not on the roll actually starting. The policy already
    // requires the engine's own dodge to be ready, so a request essentially always becomes a roll;
    // and on the rare tick it doesn't (mid-air, a state change later in the frame) charging anyway
    // is the conservative direction — it delays the next ask rather than letting it re-fire.
    if (in.dodge) {
        if (in.dodgeIsGapClose) ap().gapCloseCd = Autoplay::GAP_CLOSE_COOLDOWN;
        else                    ap().dodgeCd    = Autoplay::doctrineFor(v.buildCell).dodgeCooldownSec;
    }

    // --- 8b driver backstops applied on top of the pure decision -----------------------------------
    // (1) LOOT-SETTLE dwell. When a fight just ended (hostile count fell to zero), hold position for a
    // beat so the auto-loot vacuum can sweep the drops before the bot walks off them. We only gate the
    // forward move; the vacuum/equip/prune are existing systems. Armed on the >0->0 edge, capped ~3 s.
    if (v.targetCount == 0 && ap().lastTargetCount > 0)
        ap().lootDwell = fminf(ap().lootDwell + 1.5f, 3.0f);
    if (ap().lootDwell > 0.0f) ap().lootDwell -= dt;
    ap().lastTargetCount = v.targetCount;
    if (ap().lootDwell > 0.0f && v.targetCount == 0)
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
        const f32  dx = p.x - ap().lastPos.x, dz = p.z - ap().lastPos.z;
        const f32  dy = p.y - ap().lastPos.y;
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
        ap().slowAnchorT += dt;
        if (ap().slowAnchorT >= kSlowWin) {
            const f32 sdx = p.x - ap().slowAnchor.x, sdz = p.z - ap().slowAnchor.z;
            ap().slowNetStuck = (sdx * sdx + sdz * sdz) < kSlowMin * kSlowMin;
            ap().slowAnchor = p; ap().slowAnchorT = 0.0f;
        }
        const bool netStuck = ap().slowNetStuck;

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
        killedThisTick = (v.targetCount < ap().lastEnemyCount);
        combatProgress = killedThisTick || (enemyHp < ap().lastEnemyHp - 0.5f);
        ap().lastEnemyHp    = enemyHp;
        ap().lastEnemyCount = v.targetCount;

        if (progressed && !netStuck) {
            // Real progress resumed (moved > 0.5 m from the wedge anchor AND actually getting somewhere
            // NET): re-anchor and DROP the whole escape ladder so the bot returns to plain flow-field
            // travel. The !netStuck gate is what stops an in-place slide/orbit from masquerading as
            // progress and starving the escape ladder — the wall-pinned-swarm livelock.
            ap().lastPos = p; ap().noProgressTimer = 0.0f;
            ap().nudgeTimer = 0.0f; ap().escapeTimer = 0.0f;
            ap().lookBehindDone = false;   // new episode gets a fresh look-behind
        } else if (combatProgress) {
            // Dealing damage in place is progress too (a real fight, not a wedge): hold the timer + escape
            // ladder at zero WITHOUT moving the anchor (the bot hasn't travelled, it's killing things).
            ap().noProgressTimer = 0.0f;
            ap().nudgeTimer = 0.0f; ap().escapeTimer = 0.0f;
            ap().lookBehindDone = false;
        } else if (ap().lootDwell <= 0.0f) {
            ap().noProgressTimer += dt;                  // no move, no damage, not dwelling: wedged
        }
    }
    const bool stuck = ap().noProgressTimer > 4.0f;

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
    if (ap().breakoffTimer > 0.0f) ap().breakoffTimer -= dt;
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
        ap().doorCheckDist  = v.distToDoor;
        ap().exitStallTimer = 0.0f;
        ap().exitBull       = false;
        ap().floorCheckDist  = v.distToDoor;        // and the long, kill-agnostic window below
        ap().floorStallTimer = 0.0f;
        ap().slowAnchor      = m_localPlayer.position;   // net-progress anchor: don't carry a stale
        ap().slowAnchorT     = 0.0f;                     // net-stuck flag across the descent teleport
        ap().slowNetStuck    = false;
        ap().vhCommit        = false;                    // the climb is done once we've descended
        ap().descentCommit   = false;                    // ...and so is the Descent push
        ap().vhFollow        = Autoplay::VHallFollow{};  // node commitments don't survive the teleport
        ap().wedgeAnchor     = m_localPlayer.position;   // a floor change teleports the body: without
        ap().wedgeWinT       = 0.0f;                     // re-anchoring, the first window after it
        ap().wedgeCmdT       = 0.0f;                     // measures a huge phantom "travel" and the
        ap().wedgeEscT       = 0.0f;                     // one after that inherits a stale escalation
        ap().wedgeVetoT      = 0.0f;
        ap().wedgeTry        = 0;
        ap().wedgeCount      = 0;
        ap().bossCommit      = false;                    // the next boss floor opens its own window
        ap().bossCmtWinT     = 0.0f;
        ap().bossCmtStartDb  = -1.0f;
        ap().bossCmtBestDb   = -1.0f;
    }
    if (v.doorActive && !bossGate) {
        if (ap().exitBull) {
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
                ap().exitBull = false;
                ap().doorCheckDist = v.distToDoor; ap().exitStallTimer = 0.0f;
            }
        } else if (combatProgress) {   // PRE-LATCH ONLY: a fight that IS closing on the exit shouldn't arm it
            ap().doorCheckDist = v.distToDoor; ap().exitStallTimer = 0.0f;
        } else {
            ap().exitStallTimer += dt;
            if (ap().exitStallTimer >= kDoorCheckWindow) {
                // Window elapsed with no exit-approach: did we close > 1 m toward the door in it? If not,
                // latch the bull (which now PERSISTS, per the branch above, until the bot descends). A
                // RATE check, not a best-distance one, so a slow inward spiral that never arrives still trips.
                ap().exitBull      = (ap().doorCheckDist - v.distToDoor) < kDoorApproachMin;
                ap().doorCheckDist = v.distToDoor; ap().exitStallTimer = 0.0f;   // next window
            }
        }
    } else {
        ap().doorCheckDist = v.distToDoor; ap().exitStallTimer = 0.0f;
        ap().exitBull      = false;   // no eligible exit (boss alive / town): idle the watchdog
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
        ap().floorStallTimer += dt;
        if (ap().floorStallTimer >= kFloorWindow) {
            if ((ap().floorCheckDist - v.distToDoor) < kFloorApproachMin) {
                // The break-off's 3 s leg is too short on a dense STACKED floor — it walks a bit then
                // FIGHT re-owns the feet in place, and the bot never descends/crosses. Latch a PERSISTENT
                // commit instead (holds until the bot leaves the floor): the VHALL climb commit upstairs,
                // the FOUR_STORY descend commit on a Descent maze (fight your way DOWN to the next hole).
                // Everywhere else (flat/lava), the short de-fixate leg is right.
                if (vhallUpperExit)                                            ap().vhCommit      = true;
                else if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY) ap().descentCommit = true;
                else                                                           ap().breakoffTimer = kFloorPushLeg;
            }
            ap().floorCheckDist  = v.distToDoor;
            ap().floorStallTimer = 0.0f;
        }
    } else {
        ap().floorCheckDist = v.distToDoor; ap().floorStallTimer = 0.0f;
    }
    // Suppress the combat break-off while bulling for the exit or standing on it — leaving the floor wins
    // over re-angling a fight we've already given up on.
    if (Autoplay::combatStalled(ap().noProgressTimer, inBandFight, combatProgress) &&
        !ap().exitBull && !v.atExit && ap().breakoffTimer <= 0.0f)
        ap().breakoffTimer = 1.5f;   // arm a relocation leg (re-armed only after the timer expires)
        // NB: no flowDir requirement — the break-off STRAFES around the target (unstickCombatMove), which
        // needs no exit heading, so it works even when the bot is boxed and flowDir is vetoed to zero
        // (exactly the pocket the bot froze in: firing at an unhittable target with no flow to walk).

    // (2d) LOOK BEHIND — the dormant-ambusher trigger, and the FIRST thing tried when the bot stops
    // making progress (3 s, before the 4 s geometry ladder). A stone gargoyle is an unkillable solid
    // body that wakes ONLY while unobserved (autoplay_nav.h LOOK_BEHIND_* has the full rule), so a bot
    // that walks into one and — being an ordinary hostile in its target list — stares at it while
    // firing has built a wedge that can never clear itself. Turning around un-watches it. One-shot per
    // stuck episode; the latch is re-armed by the progress branches above.
    if (ap().lookBehindTimer > 0.0f) ap().lookBehindTimer -= dt;
    // Scoped to floors 1-10 (Aaron): the look-behind exists for the dormant STONE GARGOYLE standoff —
    // an unkillable statue the bot pins asleep by staring at it — and those appear on the early floors.
    // Off the early floors the spin-around reads as odd and the escape ladder handles other wedges, so
    // the one watchdog whose whole job is "shooting an untriggered gargoyle forever" is early-floor only.
    // The floor gate is the DUNGEON's early floors, where the stone gargoyles live — plus zones,
    // which field their own ambush body (Mind The Gap, Act 2). Written explicitly: a zone leaves
    // m_level.currentFloor at whatever dungeon floor preceded it (it is never re-assigned on zone
    // entry — the world's identity there is zoneFloor), so `currentFloor <= 10` happens to pass in
    // the overworld today by accident. Anyone who later makes currentFloor honest would silently
    // switch this remedy off in the acts; saying `|| inZone` means the intent survives that fix.
    if ((m_level.currentFloor <= 10 || m_level.inZone) &&
        Autoplay::lookBehindDue(ap().noProgressTimer, ap().lookBehindDone)) {
        ap().lookBehindDone  = true;
        ap().lookBehindTimer = Autoplay::LOOK_BEHIND_HOLD;
        ap().lookBehindYaw   = Autoplay::lookBehindYaw(m_localPlayer.yaw);
    }

    // ...AND A STANDOFF TRIGGER THAT DOES NOT CONSULT THE PROGRESS CLOCK.
    //
    // The rule above is gated on noProgressTimer, and `combatProgress` zeroes that timer on any
    // damage dealt — INCLUDING damage dealt by MINIONS while the player stands still. So for the two
    // summon classes the clock is pinned near zero forever and every remedy hanging off it is
    // permanently disarmed. Measured in the 9-class act soak: a Tinkerer stood at exactly
    // (14.9, 46.4) in Piccadilly Circus for THIRTY MINUTES with three hostiles 2.5 m away, `fire=0`
    // throughout, its drones chipping away — `npt` never once exceeded 1.3 s across the whole run
    // and the look-behind fired ZERO times. This is the same shape as the exit watchdog that
    // "latched 0% of the time" because chip damage kept restarting it.
    //
    // The standoff is a BEHAVIOURAL state, so detect it behaviourally: targets are visible, the bot
    // refuses to shoot any of them, and it is not moving. That cannot be confused with a real fight
    // (which fires) or with travel (which moves), and it is exactly the dormant-AMBUSH deadlock the
    // look-behind exists for — a body that wakes only while unobserved, which the bot pins asleep by
    // staring at it. Act 2 fields one (Mind The Gap, `ambush`, tier 5), and a CLEAR_ZONE quest
    // cannot complete while one is left standing.
    // Same 0.5 m test the progress branch uses, against the same anchor — ap().lastPos only moves
    // when the bot actually travels, so "still within 0.5 m of it" IS "has not gone anywhere".
    const Vec3 sd = m_localPlayer.position - ap().lastPos;
    const bool standoffStill = (sd.x * sd.x + sd.z * sd.z) <= 0.25f;
    const bool standoff = v.targetCount > 0 && !in.fire && standoffStill;
    ap().standoffT = standoff ? ap().standoffT + dt : 0.0f;
    if (ap().standoffT >= Autoplay::STANDOFF_AT) {
        ap().standoffT       = 0.0f;      // re-arm: one turn per standoff, not a continuous spin
        ap().lookBehindTimer = Autoplay::LOOK_BEHIND_HOLD;
        ap().lookBehindYaw   = Autoplay::lookBehindYaw(m_localPlayer.yaw);
        LOG_INFO("[AUTOPLAY] standoff: %u targets, none engaged, no movement for %.1f s — looking away",
                 v.targetCount, Autoplay::STANDOFF_AT);
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
    // The "can it act" guard, mirroring the FOUR_STORY twin below — a latched commit with nothing to
    // do MUST NOT shadow the escape ladder. This branch was missed when that fix went in, and the
    // measurement says it cost exactly what the comment there predicts: on a stalled VHALL floor the
    // no-progress timer climbed to 45 s (the ladder's threshold is 4 s) and the ladder never fired,
    // because the commit sits above it in this chain and swallowed every tick.
    // Which producer ends up owning the intent — reset each tick, stamped by whichever branch of the
    // remedy chain below wins. Reported by the STALL autopsy: a stalled bot looks identical whether
    // the brain, a latched commit or the escape ladder is driving, and knowing which one is the
    // difference between "the rescue never fired" and "the rescue fired and did not work".
    ap().remedy = "brain";
    if (ap().vhCommit && vhallUpperExit &&
        (lengthSq(v.flowDir) > 1e-6f || v.distToDoor <= 1.5f)) {
        ap().remedy = "vh-commit";
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
        }
    // A LATCHED COMMIT WITH NOTHING TO DO MUST NOT SHADOW THE ESCAPE LADDER. The commit sits above the
    // escape ladder in this else-if chain and is held for the rest of the floor, so once it latched it
    // used to swallow every tick — including the ticks where it had NO heading to walk and was not at
    // the door, i.e. exactly when the bot was wedged and needed the ladder. Measured: a 22 s dead stop
    // with flow=0.00, cmt=1, the no-progress timer climbing past 9 s and esc/nudge never firing, because
    // the ladder below was unreachable. So the branch is entered only when it can actually act — walk a
    // heading, or descend at the door — and otherwise the chain falls through to the ladder, which does
    // its own 8-direction search and digs the bot out.
    } else if (ap().descentCommit && m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY &&
               (lengthSq(v.flowDir) > 1e-6f || v.distToDoor < Autoplay::DESCEND_STOP_M)) {
        ap().remedy = "descent-cmt";
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
        ap().remedy = "descend";
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        in.descend = true;
    } else if (atDoor && v.distToDoor < 2.5f && ap().noProgressTimer < 8.0f) {
        ap().remedy = "door-walk";
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
        const Vec3 goalPos = autoplayGoalPos();
        const Vec3 h{goalPos.x - m_localPlayer.position.x, 0.0f,
                     goalPos.z - m_localPlayer.position.z};
        if (lengthSq(h) > 1e-6f) {
            f32 y, p; Autoplay::dirToAim(h, y, p);
            in.aimYaw = y; in.aimPitch = 0.0f; in.moveFwd = true;   // close the last metre
        }
        in.descend = true;
    } else if ((stuck || ap().nudgeTimer > 0.0f || ap().escapeTimer > 0.0f) &&
               !(ap().exitBull && v.doorActive && !bossGate)) {
        // ^ A LATCHED BULL PREEMPTS THE ESCAPE LADDER. The 7102 s gauntlet livelock (couch_soak8):
        // a false-LOS standoff kept `stuck` true on every tick, so this branch consumed the whole
        // chain and the bull below — the one remedy built for exactly that pocket (A*-route to the
        // door, fire through everything on the path) — never executed despite being latched. Same
        // failure class as "a latched commit must not shadow the escape ladder", inverted. Falling
        // through is safe against the reverse shadow: on a flat floor with an active door the bull
        // ALWAYS has a heading (A* first leg, uncapped exit-flow-field fallback), it releases on a
        // kill, and the floor-change reset clears the latch.
        ap().remedy = "escape";
        // Remedy B — wedged on geometry: an ESCALATING escape so an AFK bot is NEVER found permanently
        // idle. The longer the bot makes no XZ progress (ap().noProgressTimer keeps climbing while
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
        const Vec3 anchor = ap().lastPos;   // last progress point = where the bot wedged
        Vec3 esc{0, 0, 0};

        // STAGE 1: lateral nudge. Arms at the 4 s stuck onset and only up to 6 s (past that, escalate).
        if (stuck && ap().nudgeTimer <= 0.0f && ap().escapeTimer <= 0.0f &&
            ap().noProgressTimer < 6.0f)
            ap().nudgeTimer = 0.5f;
        if (ap().nudgeTimer > 0.0f) {
            ap().nudgeTimer -= dt;
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
            if (lengthSq(esc) < 1e-6f) ap().nudgeTimer = 0.0f;   // no lateral step: abandon, escalate now
        }

        // STAGE 2 / 3: committed 8-dir (or A*) escape, engaged whenever the lateral nudge isn't driving.
        if (lengthSq(esc) < 1e-6f) {
            if (ap().escapeTimer <= 0.0f) {
                Vec3 h{0, 0, 0};
                // STAGE 3 first (deepest escalation): a short A* leg toward the exit for when the flow
                // field itself yields no heading. bodyRadius ~ the player half-width; findPath returns
                // world-space waypoints (outPath[0] = the first corner toward the goal), 0 if the door is
                // unreachable within its 256-cell cap.
                if (ap().noProgressTimer > 8.0f && m_level.floorDoorActive) {
                    // On a STACKED floor the flat A* below is story-blind and routes to the door's XZ
                    // under a balcony / away from a hole — use the story-aware field heading instead.
                    const bool stackedExit = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL ||
                                             m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY;
                    if (stackedExit && lengthSq(v.flowDir) > 1e-6f) {
                        h = normalize(Vec3{v.flowDir.x, 0.0f, v.flowDir.z});
                    } else {
                        Vec3 wp[MAX_PATH_WAYPOINTS];
                        const u8 n = Pathfinder::findPath(m_level.grid, m_localPlayer.position,
                                                          autoplayGoalPos(), wp, MAX_PATH_WAYPOINTS, 0.3f);
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
                ap().escapeDir   = h;
                ap().escapeTimer = 0.5f;   // commit for ~0.5 s (traverse a cell; throttle the A* leg)
            }
            ap().escapeTimer -= dt;
            // Re-validate the committed heading each tick (cheap insurance); drop the commit early if it
            // is no longer safe so the next tick recomputes rather than driving into a hazard.
            if (lengthSq(ap().escapeDir) > 1e-6f &&
                Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, ap().escapeDir,
                                      m_level.lavaFloor))
                esc = ap().escapeDir;
            else
                ap().escapeTimer = 0.0f;
        }

        // Apply the escape heading through unstickCombatMove: if a hostile is in reach it STRAFES around
        // it while FIRING (kills a body-blocker, changes the angle) biased toward `esc`; otherwise it just
        // walks `esc` (identical to the old forward step for a pure geometry wedge with nothing to shoot).
        // Only override when it produced an actionable move — a fully-boxed no-target result leaves the
        // bot's current intent alone. This is what stops the >4 s escape zone from silently holstering the
        // guns and freezing next to enemies it could have killed.
        {
            // commitWalk past 10 s: Stages 1-2 gave the strafe its chance; from here the ladder's
            // heading is COMMANDED, not advisory (see unstickCombatMove).
            Autoplay::BotIntent u = unstickCombatMove(v, esc, m_level.grid, feetY, m_level.lavaFloor,
                                                      anchor, m_localPlayer.position, m_localPlayer.yaw,
                                                      /*commitWalk=*/ap().noProgressTimer > 10.0f);
            if (intentActs(u)) in = u;
            // JUMP as part of the escape. The ladder above only ever tried new HEADINGS, and a body
            // caught on a lip, a step edge or the inside of a corner does not need a new heading —
            // it needs to leave the ground, because move-and-slide will keep refusing the same
            // blocked axis at the same height forever. Pulsed on the kiting cadence rather than held
            // so the bot hops out rather than pogoing (a held JUMP re-fires every landing frame).
            in.jump = Autoplay::kitingJumpTick(v.tick);
        }
    } else if (ap().exitBull && v.doorActive && !bossGate) {
        ap().remedy = "bull";
        // Remedy B2 — EXIT BULL (the exit-progress watchdog latched): the bot is MOVING but getting
        // nowhere useful — orbiting/spiralling the floor, or kited off the exit by a swarm it refuses to
        // shoot — so stop playing and just leave. Ranked BELOW the geometry escape on purpose: when the
        // bot is physically wedged, walking at the door only presses it into the wall and the escape
        // ladder never gets to run (measured: 35 s frozen with the bull latched and moveFwd held). The
        // two are naturally exclusive — `stuck` means not moving, the bull means moving-but-not-arriving.
        const Vec3 pos = m_localPlayer.position;
        const Vec3 goalPos = autoplayGoalPos();
        Vec3 heading{goalPos.x - pos.x, 0.0f, goalPos.z - pos.z};
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
            const u8 n = Pathfinder::findPath(m_level.grid, pos, autoplayGoalPos(), wp,
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

        // ...AND THE HEADING MUST BE WALKABLE. Every other movement producer is vetoed against the
        // geometry; the bull was the one that aimed at the goal and held FORWARD whatever was in the
        // way, which is exactly how it wedges. Measured in the 9-class act soak: a warrior 6.1 m
        // from the Den's gate held moveFwd into a WALL for 2198 seconds — `net=0.00` with the routed
        // field (`fdir=+0.17,+0.99`) and the commanded movement (`mdir=-0.29,-0.96`) pointing
        // OPPOSITE ways. A* had answered with a first leg through the obstacle, and because a
        // returned path counts as "routed" the wall-aware field never got its turn.
        //
        // So: if the chosen heading walks into something, take the field instead; if that is blocked
        // too, drop the bull's claim on the intent entirely so the escape ladder — which is built
        // for a body wedged in geometry — gets to run. Preferring the field over A*'s first leg
        // outright would be the wrong fix: A* is what routes AROUND a large obstacle, and the field
        // is a greedy descent that can sit in a local pocket. Each covers the other's failure.
        {
            const f32 feetY = m_localPlayer.position.y;
            if (!Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, heading,
                                       m_level.lavaFloor)) {
                const Vec3 fieldDir{v.flowDir.x, 0.0f, v.flowDir.z};
                if (lengthSq(fieldDir) > 1e-6f &&
                    Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, fieldDir,
                                          m_level.lavaFloor)) {
                    heading = fieldDir;
                } else {
                    // Nothing walkable toward the exit from here: this is a WEDGE, not a
                    // "moving but not arriving". UNLATCH so the escape ladder can own the next tick.
                    //
                    // Clearing the latch is the whole fix, not merely dropping the heading. A
                    // latched bull deliberately PREEMPTS the escape branch (the gauntlet livelock:
                    // the ladder monopolised the intent for two hours emitting useless sidesteps),
                    // so a bull that keeps its latch while refusing to move starves the one rescue
                    // built for a body stuck in geometry — the bot would stand still instead of
                    // walking into the wall, which is not an improvement.
                    //
                    // Re-latching is automatic: the watchdog re-arms whenever the bot is moving and
                    // still not closing on the exit, so the two remedies alternate by their own
                    // definitions — the bull for "moving, not arriving", the ladder for "not moving".
                    ap().remedy   = "bull-blocked";
                    ap().exitBull = false;
                    heading = Vec3{0, 0, 0};
                }
            }
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
            if (flatFloor && in.moveFwd && (ap().bullDodgeTick % 45u == 0u))
                in.dodge = true;
        }
        ap().bullDodgeTick++;
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
    } else if (ap().breakoffTimer > 0.0f) {
        ap().remedy = "breakoff";
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
                                                      ap().lastPos, m_localPlayer.position, m_localPlayer.yaw,
                                                      /*commitWalk=*/false);   // break-off keeps the orbit
            if (intentActs(u)) in = u;
        }
    }

    // (2e) LOOK-BEHIND OVERRIDE. Applied AFTER the whole remedy chain because it has to beat every
    // one of them — and, crucially, the FIGHT branch: the gargoyle that wedged us is an ordinary
    // hostile in the target list, so decideCombat would aim straight back at it and pin it asleep
    // again. Movement and fire are dropped for the turn (a deliberate look-behind, not a fighting
    // retreat); `descend` is left alone so a door hold already in progress is not thrown away. The
    // aim smoother turns at its own rate, so this reads as a look over the shoulder, never a snap.
    if (ap().lookBehindTimer > 0.0f) {
        in.aimYaw = ap().lookBehindYaw; in.aimPitch = 0.0f;
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
    // hold land. Flat-floor detour only (ap().shrineTarget is set only there).
    if (ap().shrineTarget && !in.fire && !in.potion && !in.descend && !stuck &&
        !ap().exitBull && !v.stunned && !v.rolling) {
        const Vec3 to{ap().shrinePos.x - m_localPlayer.position.x, 0.0f,
                      ap().shrinePos.z - m_localPlayer.position.z};
        const f32 d = length(to);
        if (d < GameConst::INTERACT_RANGE) {
            if (lengthSq(to) > 1e-6f) { f32 y, p; Autoplay::dirToAim(to, y, p); in.aimYaw = y; in.aimPitch = 0.0f; }
            in.descend = true;                                             // hold PICKUP -> shrine (pulsed below)
            if (d < 1.5f) in.moveFwd = in.moveBack = in.moveLeft = in.moveRight = false;   // stop in grab range
        }
    }

    // (2g) DESCENT FLOORS: NEVER STAND STILL WHILE A ROUTE EXISTS.
    // On FOUR_STORY the bot used to root in place and fight instead of descending. Measured over six
    // instrumented runs: it held NO movement key on 57% of ticks, and in 267 of 267 of those ticks a
    // VALID descent heading existed — it always knew the way down and simply did not walk. The cause
    // is that a Descent floor carries four storeys of enemies, so the nearest-target scan is pinned at
    // its cap (16 in view, permanently) and the FIGHT branch owns the intent; that branch only emits
    // movement when it is kiting, closing or strafing, so an in-band target it is neither closing on
    // nor kiting from produces zero WASD. Both rescues are disarmed by the very same conditions: the
    // escape ladder needs ap().noProgressTimer > 4 s but the combat-progress branch pins it at 0
    // while the bot chips the swarm (measured max 0.5-0.6 s), and the 20 s floor-stall window that
    // arms ap().descentCommit resets whenever the bot happens to drift 2 m doorward mid-fight.
    //
    // So make the commit's behaviour the DEFAULT rather than a watchdog-armed emergency: whenever the
    // intent carries no movement at all and a descent heading exists, walk it. Combat is untouched —
    // aim / fire / dodge / block / skills are whatever the brain decided; only the FEET are filled in,
    // exactly as the commit body does (same faceAndGo decomposition, same kAxis). This is the fix the
    // measurement points at: with the commit latched the standstill rate more than halved (57% -> 24%),
    // so the mechanism was already right and merely under-triggered.
    //
    // Runs BEFORE the descend pulse below on purpose: that pulse drops in.descend on its release beat,
    // and reading the flag after it would let a release beat walk the bot off the door it is opening.
    // Skipped when standing ON the drop hole (atDescentGoal) — it is about to fall and steering it now
    // would walk it back off the hole — and while the deliberate stand-stills are live.
    // BOSS-FLOOR MOVEMENT FILL — the Descent fill's twin, for the residual boss stall soak11 made
    // self-describing: 273 boss-gated pins, dB median 40 m, mv=0 on 232 and flow=0 on 208 — the bot
    // fights in place while the heading toward the one enemy that opens the exit never reaches the
    // feet (FIGHT emits WASD only when kiting/closing/strafing, and every no-progress rescue is
    // disarmed by the chip damage it keeps dealing). When the intent carries no movement on a
    // boss-gated floor, walk toward the boss: prefer the (possibly veto-adjusted) travel heading,
    // else re-read the wall-aware boss RouteField DIRECTLY — the buildBotView copy can be zeroed by
    // the detour fan on cavern niches, but the field's own step is wall-aware by construction. Only
    // the FEET are filled; aim / fire / dodge / block / potion stay the brain's.
    // BOSS-FLOOR CLOSING COMMIT (soak13 "ranged never closes" — see autoplay_combat.h bossCommit*).
    // The movement fill below only fires on an EMPTY-WASD intent, and a ranged doctrine strafing its
    // add-band always has WASD — the ranger orbited one NM-25 boss floor for 78 min at dB 44-45 with
    // no boss LOS. So a 20 s window tracks whether the bot is CLOSING on the boss at all; when it
    // provably is not (and the boss isn't already fightable — the warrior standing ON Korvath must
    // never latch), the commit latches and the feet are OVERRIDDEN toward the boss route even over a
    // live FIGHT intent, exactly the descend-commit shape: combat stays the brain's, only the WASD
    // is rewritten. Released the moment the boss has LOS inside the release range — from there the
    // boss-floor target priority + the any-range boss exemption hold it in the fight normally.
    // Env kill-switch NO_BOSSCMT=1 for the A/B; remove once measured.
    static const bool sNoBossCmt = std::getenv("NO_BOSSCMT") != nullptr;   // A/B arm, read once
    if (bossGate && ap().bossDist >= 0.0f && !sNoBossCmt) {
        const f32 relR = Autoplay::bossCommitReleaseRange(v.weaponRange);
        if (ap().bossCommit) {
            if (Autoplay::bossCommitShouldRelease(v.bossAlive, ap().bossLOS != 0,
                                                  ap().bossDist, relR)) {
                ap().bossCommit     = false;
                ap().bossCmtWinT    = 0.0f;
                ap().bossCmtStartDb = -1.0f;   // fightable now: next window seeds fresh if it drifts
            }
        } else {
            if (ap().bossCmtStartDb < 0.0f) {   // window unseeded (fresh floor / just released)
                ap().bossCmtStartDb = ap().bossCmtBestDb = ap().bossDist;
                ap().bossCmtWinT    = 0.0f;
            }
            if (ap().bossDist < ap().bossCmtBestDb) ap().bossCmtBestDb = ap().bossDist;
            ap().bossCmtWinT += dt;
            if (ap().bossCmtWinT >= Autoplay::BOSS_COMMIT_WINDOW_SEC) {
                ap().bossCommit = Autoplay::bossCommitShouldLatch(
                    ap().bossCmtStartDb, ap().bossCmtBestDb,
                    ap().bossLOS != 0, ap().bossDist, relR);
                ap().bossCmtStartDb = -1.0f;   // either way the window restarts (latched: idle until release)
            }
        }
    } else if (!bossGate) {
        ap().bossCommit = false; ap().bossCmtStartDb = -1.0f;   // boss died / left the floor mid-window
    }

    if (m_level.floorHasBoss && v.hasBoss && v.bossAlive &&
        (ap().bossCommit ||
         (!in.moveFwd && !in.moveBack && !in.moveLeft && !in.moveRight)) &&
        !in.descend && !v.stunned && !v.rolling &&
        ap().lookBehindTimer <= 0.0f && ap().lootDwell <= 0.0f) {
        Vec3 h = v.flowDir;
        if (lengthSq(h) < 1e-6f && ap().bossRoute.valid)
            h = Autoplay::routeDirection(ap().bossRoute, m_level.grid, m_localPlayer.position);
        if (lengthSq(h) > 1e-6f) {
            const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
            const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
            const f32  df = h.x * fwd.x + h.z * fwd.z, dr = h.x * right.x + h.z * right.z;
            constexpr f32 kAxis = 0.35f;
            in.moveFwd = df > kAxis; in.moveBack = df < -kAxis;
            in.moveRight = dr > kAxis; in.moveLeft = dr < -kAxis;
            if (ap().bossCommit) ap().remedy = "boss-cmt";   // [STALL]: this branch owns the feet
        }
    }

    if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY &&
        !in.moveFwd && !in.moveBack && !in.moveLeft && !in.moveRight &&
        !in.descend && !v.stunned && !v.rolling &&
        ap().lookBehindTimer <= 0.0f && ap().lootDwell <= 0.0f &&
        lengthSq(v.flowDir) > 1e-6f &&
        !Autoplay::atDescentGoal(ap().descent, m_level.grid, m_localPlayer.position)) {
        // Decompose the heading onto the player's CURRENT yaw basis — the basis the movement code will
        // actually read this tick, since the aim is only EASED toward the intent's desired yaw.
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const f32  df = v.flowDir.x * fwd.x   + v.flowDir.z * fwd.z;
        const f32  dr = v.flowDir.x * right.x + v.flowDir.z * right.z;
        constexpr f32 kAxis = 0.35f;   // same threshold as faceAndGo / the two commit bodies
        in.moveFwd   = df >  kAxis;
        in.moveBack  = df < -kAxis;
        in.moveRight = dr >  kAxis;
        in.moveLeft  = dr < -kAxis;
    }

    // (2h) COMMIT THE DROP — walk it in, and ROLL if walking isn't doing it.
    // Measured over four instrumented Descent runs: ranged builds spend 79-86% of their ticks within a
    // METRE of a drop hole and 82% of them airborne, and still do not go down — they hover at the lip,
    // where the body's own half-width keeps catching the slab edge, while the field (which codes a hole
    // 0xFE) reports "at the goal" and stops steering. Walking is evidently not enough to fall in. Melee
    // builds have the opposite problem: only 2% of their ticks are within a metre of a hole at all.
    // So within a short radius of the nearest way down: aim the FEET at the hole centre (this also
    // pulls the melee builds the last few metres in), and once genuinely close, spend a DODGE ROLL at
    // it — a committed ~4 m lunge that carries the body clear of the lip. The roll direction is derived
    // from the WASD held THIS tick (PlayerController::computeRollDirection), so the movement keys must
    // be set on the same tick, exactly as the gap-closer roll does. Combat (aim/fire/block/skills) is
    // left alone. Grounded only — an airborne bot is already committed and a roll would just burn the CD.
    if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY && m_localPlayer.onGround &&
        !in.descend && !v.stunned && !v.rolling && ap().lootDwell <= 0.0f &&
        ap().lookBehindTimer <= 0.0f) {
        constexpr f32 kHoleSteer = 6.0f;   // m: steer the feet at the hole from here in
        constexpr f32 kHoleRoll  = 2.5f;   // m: close enough that a roll lands in it
        Vec3 hole{};
        const f32 hd = Autoplay::nearestDropHole(ap().descent, m_level.grid,
                                                 m_localPlayer.position, hole);
        if (hd <= kHoleSteer) {
            const Vec3 to{hole.x - m_localPlayer.position.x, 0.0f, hole.z - m_localPlayer.position.z};
            if (lengthSq(to) > 1e-6f) {
                const Vec3 dir = normalize(to);
                const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
                const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
                const f32  df = dot(dir, fwd), dr = dot(dir, right);
                constexpr f32 kAxis = 0.35f;
                in.moveFwd = df > kAxis; in.moveBack = df < -kAxis;
                in.moveRight = dr > kAxis; in.moveLeft = dr < -kAxis;
                // The lunge itself. Rate-limited by the engine's own ~1 s dodge cooldown rather than
                // the doctrine leash — this is a navigation move, not a defensive one, and the leash
                // exists to stop the bot LOOKING twitchy in a fight, not to ration a descent.
                if (hd <= kHoleRoll) in.dodge = true;
            }
        }
    }

    // (3) DESCEND PICKUP PULSE. The exit is a HOLD target, but a HOLD reaches a SHRINE sharing the
    // exit's interact range FIRST; the bot holds PICKUP continuously, so Interact::poll fires once
    // (spending the shrine), latches `consumed`, and never re-fires to reach the exit — a permanent
    // wedge. So we release + re-hold in a pulse (autoplay_nav.h descendPulseHeld): one cycle spends
    // the shrine, the next descends. Only bites the descend intent; combat/movement are untouched.
    if (in.descend) {
        ap().descendPulse += dt;
        if (!Autoplay::descendPulseHeld(ap().descendPulse)) in.descend = false;   // release beat
    } else {
        ap().descendPulse = 0.0f;
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
        // PADS ARE NOT VETOED HERE. Fighting is reactive and short-range, and refusing every step
        // whose footprint clips a pad made the bot skirt widely around them mid-combat — Aaron:
        // "make the bot properly use jump pads instead of being afraid of them". A pad taken during a
        // fight costs a re-route, which the descent field, padEscapeDirection and the wedge escape
        // all now handle; being unable to step where the fight is has no such recovery. Walls,
        // off-map and the corner-cut rule still apply.
        auto blocked = [&](Vec3 d) {
            return !Autoplay::stepAllowed(m_level.grid, p, feetY, d, /*lavaFloor=*/false,
                                          /*avoidPads=*/false);
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
    // FOLLOWER TAKEOFF INJECTION. The follower asked for the committed jump this tick
    // (ap().vhFollowJump, stashed by buildBotView); the press fires only when the FEET agree — the
    // tick's WASD resultant, rebuilt on the CURRENT yaw basis, must be within ~40 degrees of the
    // committed link axis. That is the FIGHT-interleave guard: combat owns the intent on ~half of
    // all ticks, and a kiting tick whose feet point off-axis must not take off sideways into the
    // void. While the press is live, the dodge and block reflexes are suppressed for the takeoff
    // tick only — a roll is a ~4 m lunge and a block is a 0.4x move-speed launch, and either one
    // sabotages the arc; eating one hit costs less than a 3 m fall and a re-climb.
    bool vhTakeoff = false;
    if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL && ap().vhFollowJump) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        Vec3 res{0, 0, 0};
        if (in.moveFwd)   res = res + fwd;
        if (in.moveBack)  res = res - fwd;
        if (in.moveRight) res = res + right;
        if (in.moveLeft)  res = res - right;
        const f32 rl = sqrtf(lengthSq(res));
        if (rl > 1e-3f) {
            const f32 along = (res.x * ap().vhFollowJumpDir.x + res.z * ap().vhFollowJumpDir.z) / rl;
            if (along > 0.766f) {                      // cos ~40 degrees
                vhTakeoff = true;
                in.jump  = true;
                in.dodge = false;
                in.block = false;
            }
        }
    }

    // Applied per WASD component so the bot slides along the safe axes instead of freezing.
    // ...EXCEPT on a follower takeoff tick: the whole point of that press is to cross the rim the
    // veto exists to protect, and the press is already triple-gated (standing on a lip node, feet
    // aligned with the link axis, StoryNav::planVault confirming a landing) — stricter gates than
    // the veto's own 1-cell lookahead. Combat feet at the lip on every OTHER tick stay vetoed;
    // only the aligned, geometry-approved takeoff crosses.
    if (vhallUpperExit && m_localPlayer.onGround && !vhTakeoff) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const Vec3 p     = m_localPlayer.position;
        const f32  feetY = p.y;
        if (in.moveFwd   && Autoplay::wouldFall(m_level.grid, p, feetY, fwd))            in.moveFwd   = false;
        if (in.moveBack  && Autoplay::wouldFall(m_level.grid, p, feetY, fwd   * -1.0f))  in.moveBack  = false;
        if (in.moveRight && Autoplay::wouldFall(m_level.grid, p, feetY, right))          in.moveRight = false;
        if (in.moveLeft  && Autoplay::wouldFall(m_level.grid, p, feetY, right * -1.0f))  in.moveLeft  = false;
        // ...AND THE RESULTANT. Per-component alone is not enough: at a balcony CORNER each axis
        // lands on the slab while the DIAGONAL between them goes over the edge, so both components
        // pass and the bot walks off anyway. This is the same corner case stepAllowed already guards
        // (a diagonal step needs the destination AND both orthogonals); the fall veto never did.
        //
        // Measured: on VHALL upper-exit floors the bot reaches the top in ~60 s then spends only
        // 4-11% of the next ten minutes there, and attributing every drop showed ~64% were plain
        // WALK-OFFS with no jump or roll anywhere near them (one sample: no jump for 14 s) — i.e.
        // exactly the case a per-axis veto cannot see.
        //
        // The LATERAL component is dropped first: it is usually the incidental one (a strafe or a
        // kite side-step) while the fore/aft motion is what the brain actually intended. If the
        // resultant still falls, the fore/aft goes too and the bot holds its ground rather than
        // stepping off.
        for (u8 pass = 0; pass < 2; pass++) {
            Vec3 res{0.0f, 0.0f, 0.0f};
            if (in.moveFwd)   res = res + fwd;
            if (in.moveBack)  res = res - fwd;
            if (in.moveRight) res = res + right;
            if (in.moveLeft)  res = res - right;
            if (lengthSq(res) < 1e-6f) break;                       // nothing left to check
            if (!Autoplay::wouldFall(m_level.grid, p, feetY, res)) break;   // the combined step is safe
            if (pass == 0 && (in.moveRight || in.moveLeft)) { in.moveRight = in.moveLeft = false; }
            else { in.moveFwd = in.moveBack = in.moveRight = in.moveLeft = false; break; }
        }

        // THE VETO MUST NEVER FREEZE THE BOT OUTRIGHT.
        //
        // Measured (couch soak, floor 36, both lanes): standing ON the exit storey (y = 3.00 =
        // exitY), grounded, a VALID two-story heading every tick (flow = 1.00), 12 visible targets —
        // and mv = 0 with d2d frozen at 17-18 m for the whole floor. Every direction reads as a fall
        // from a balcony whose route to the door crosses a catwalk (one of the pair has a deliberate
        // 2-cell jump gap), so the resultant branch above cleared all four components and the bot
        // stood there permanently. "They don't even move."
        //
        // The VHallField cannot route off an edge — its adjacency only links nodes whose surfaces are
        // within one step — so when the FIELD has a heading and this 1-cell lookahead disagrees, the
        // lookahead is the thing that is wrong: it cannot see the catwalk the field is routing over.
        // Trust the field and restore its step. A veto that can zero every direction is not a safety
        // rail, it is a trap; falling costs a re-climb, standing still costs the whole run.
        if (!in.moveFwd && !in.moveBack && !in.moveLeft && !in.moveRight &&
            lengthSq(v.flowDir) > 1e-6f) {
            const f32 df = v.flowDir.x * fwd.x   + v.flowDir.z * fwd.z;
            const f32 dr = v.flowDir.x * right.x + v.flowDir.z * right.z;
            constexpr f32 kAxis = 0.35f;
            in.moveFwd   = df >  kAxis;
            in.moveBack  = df < -kAxis;
            in.moveRight = dr >  kAxis;
            in.moveLeft  = dr < -kAxis;
        }
    }
    // UNDER-SLAB TRAP. Two rules, and they must be in this order.
    //
    // The hazard veto in buildBotView already refuses a TRAVEL step under the low end of a ramp, but
    // it only guards producers that go through stepAllowed — and the FIGHT branch's close/kite
    // movement is deliberately unvetoed (short, reactive, enemy-derived). So a melee build chasing a
    // hostile walks itself under the stairs anyway, and once pinned there the axis-separated
    // moveAndSlide has nowhere to push it out to. Reported twice live on a Paladin.
    //
    // Unlike lava or a ledge this is not a tactical hazard to weigh — it is geometry that swallows
    // the body — so it is applied to the FINAL intent, whichever producer wrote it.
    {
        const Vec3 p     = m_localPlayer.position;
        const f32  feetY = p.y;
        u32 px, pz;
        const bool pinnedNow = LevelGridSystem::worldToGrid(m_level.grid, p, px, pz) &&
                               LevelGridSystem::bodyPinnedUnderSlab(m_level.grid, px, pz, feetY);
        if (pinnedNow) {
            // ALREADY UNDER: every direction reads as refused from in here, so vetoing would pin the
            // bot for good. Override the feet toward the nearest cell where the body fits and let it
            // walk out; combat (aim / fire / block / skills) is left alone.
            const Vec3 out = Autoplay::unpinDirection(m_level.grid, p, feetY);
            if (lengthSq(out) > 1e-6f) {
                const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
                const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
                const f32  df = out.x * fwd.x + out.z * fwd.z, dr = out.x * right.x + out.z * right.z;
                constexpr f32 kAxis = 0.35f;
                in.moveFwd = df > kAxis; in.moveBack = df < -kAxis;
                in.moveRight = dr > kAxis; in.moveLeft = dr < -kAxis;
            }
        } else {
            // NOT under yet: refuse the individual components that would take us in, so the bot
            // slides along the open axis instead of walking into the pinch.
            const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
            const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
            auto pins = [&](Vec3 d) {
                u32 gx, gz;
                const Vec3 to = p + d * m_level.grid.cellSize;
                return LevelGridSystem::worldToGrid(m_level.grid, to, gx, gz) &&
                       LevelGridSystem::bodyPinnedUnderSlab(m_level.grid, gx, gz, feetY);
            };
            if (in.moveFwd   && pins(fwd))            in.moveFwd   = false;
            if (in.moveBack  && pins(fwd   * -1.0f))  in.moveBack  = false;
            if (in.moveRight && pins(right))          in.moveRight = false;
            if (in.moveLeft  && pins(right * -1.0f))  in.moveLeft  = false;
        }
    }

    // JUMP only from the ground (the engine ignores it otherwise, but asking for what cannot happen
    // muddies the telemetry) and never while a roll owns the body.
    if (in.jump && (!m_localPlayer.onGround || m_localPlayer.dodgeState.rolling)) in.jump = false;
    in.potion = in.potion || decidedPotion;   // SURVIVE re-asserted (see decide() above): never skip a needed heal

    // PERFECT-BLOCK PACING (state + constants: engine.h / autoplay_combat.h). decideCombat wants to
    // perfect-block every incoming swing; left alone the bot never mistimes, which reads as a machine.
    // So: land a STREAK of perfect blocks, then take an "unreliable" LAPSE where most swings mistime,
    // then be sharp again. One decision PER SWING — a block want spans ~9 ticks, so we act on the
    // rising edge and latch the outcome for the swing. Deterministic (tick-hashed), replay-safe.
    if (ap().blockUnreliableT > 0.0f) ap().blockUnreliableT -= dt;
    {
        const bool wantNow = in.block;                        // what decideCombat asked for, pre-pacing
        if (wantNow && !ap().blockWantPrev) {            // fresh swing to react to
            if (ap().blockUnreliableT > 0.0f) {
                // LAPSE: the perfect tap lands only sometimes; the rest are mistimed (eaten).
                const u32 h = currentLocalTick() * 2654435761u;
                ap().blockSuppress = ((h >> 25) % 100u) >= Autoplay::BLOCK_UNRELIABLE_PCT;
            } else {
                // SHARP: land the perfect block and build the streak; at the cap, fall into a lapse.
                ap().blockSuppress = false;
                if (++ap().blockStreak >= ap().blockStreakCap) {
                    ap().blockUnreliableT = Autoplay::BLOCK_UNRELIABLE_SEC;
                    ap().blockStreak      = 0;
                    const u32 h = currentLocalTick() * 40503u;   // next run: 2 or 3 in a row
                    ap().blockStreakCap   = Autoplay::BLOCK_STREAK_MIN +
                        (u8)((h >> 20) % (u32)(Autoplay::BLOCK_STREAK_MAX - Autoplay::BLOCK_STREAK_MIN + 1u));
                }
            }
        }
        if (!wantNow) ap().blockSuppress = false;        // swing over: clear the per-swing latch
        if (ap().blockSuppress) in.block = false;        // a mistimed swing: no perfect block
        ap().blockWantPrev = wantNow;
    }

    // (6) WEAPON THROWS — the two "hurl the weapon" flourishes, both driven through the REAL buttons.
    //
    // (a) MELEE THROW at a RANGED enemy. The short-click throw is decided in handleWeaponFire off the
    //     raw Fire button, so the bot cannot request it directly — it must emit a synthetic short TAP,
    //     and because an auto-firing bot has held Fire far past TAP_SEC the tap has to RELEASE first to
    //     zero the hold accumulator (botTapFireHeld sequences release -> brief press -> release).
    //     Aimed at archers beyond swing reach: the throw crosses the gap the bot would otherwise walk
    //     under fire. Leashed so it stays an occasional flourish rather than a thrown-weapon machine gun.
    // (b) THROWAWAY RELOAD THROW at clip-1. The legendary hurls the gun whenever a reload TRIGGERS, and
    //     its damage scales with the rounds left (wpn.damage * currentClip * 0.5) — while a manual
    //     reload is only legal below a full clip. So clipSize-1, i.e. immediately after the first shot,
    //     is the highest-damage throw the rules allow. One-tick RELOAD pulse (the engine reads a press
    //     EDGE), on its own leash.
    // Both are suppressed while a UI is open or the bot is stunned/rolling, and lane-0 only (v1).
    {
        const u8 tl = (m_localPlayerIndex < MAX_LOCAL_PLAYERS) ? m_localPlayerIndex : 0;
        if (ap().throwLeash  > 0.0f) ap().throwLeash  -= dt;
        if (ap().reloadThrow > 0.0f) ap().reloadThrow -= dt;
        ap().reloadPulse = false;
        const bool mayThrow = !uiOpen && !v.stunned && !v.rolling;

        // Nearest hostile that is RANGED and actually visible — the throw wants a line, not a guess.
        const Autoplay::BotTarget* rangedT = nullptr;
        for (u32 i = 0; i < v.targetCount && !rangedT; i++)
            if (v.targets[i].isRanged && v.targets[i].hasLOS && !v.targets[i].invulnerable)
                rangedT = &v.targets[i];

        if (ap().throwSeq >= 0.0f) {                 // a tap is already in flight: run it out
            ap().throwSeq += dt;
            if (WeaponThrow::botTapDone(ap().throwSeq)) ap().throwSeq = -1.0f;
        } else if (mayThrow && rangedT && ap().throwLeash <= 0.0f &&
                   WeaponThrow::botShouldThrow(v.weaponIsMelee, m_weaponThrowCd[tl] <= 0.0f,
                                               rangedT->isRanged, rangedT->hasLOS,
                                               rangedT->dist, v.weaponRange)) {
            ap().throwSeq   = 0.0f;                              // start the synthetic tap
            ap().throwLeash = WeaponThrow::BOT_THROW_LEASH;
        }

        // (b) the THROWAWAY gun toss. Same "occasional", same visible-target requirement.
        if (mayThrow && ap().reloadThrow <= 0.0f && rangedT) {
            const ItemInstance& eq =
                m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
            if (!isItemEmpty(eq) && isLegendaryOrBetter(eq.rarity) &&
                m_itemDefs[eq.defId].legendarySkillId == SkillId::THROWAWAY) {
                const WeaponState& ws = m_players[activeNetSlot()].weaponState;
                const WeaponDef wd = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex],
                                                                  m_itemDefs, eq);
                // clip-1 exactly: the fullest clip a manual reload is allowed to interrupt.
                if (wd.clipSize > 0 && !ws.reloading && ws.currentClip == wd.clipSize - 1) {
                    ap().reloadPulse = true;
                    ap().reloadThrow = 8.0f;   // s between gun tosses
                }
            }
        }
    }

    // (5) WEDGE ESCAPE — the last thing before the intent is pressed, because it must be able to
    // override EVERY producer above it (the descent commit, the movement fill, the hole commit, the
    // FIGHT branch's kite/close).
    //
    // This exists because none of the other rescues can see this failure. The escape ladder needs
    // ap().noProgressTimer > 4 s, and the `combatProgress` branch pins that timer at 0 on any
    // chip damage; the floor-stall watchdog resets on any doorward drift and only arms the descent
    // commit — which is already latched here and is itself the thing being ignored. Measured on the
    // residual FOUR_STORY park: valid heading on 100% of ticks, veto never fired, not on a pad, Y
    // pinned to the centimetre, and 0.0-0.1 m of travel per 5 s window with the commit latched. The
    // bot was telling the body to walk and the body would not go.
    //
    // Two remedies, together: a JUMP (a body balanced on a slab lip is refused on the same axis at the
    // same height forever — it has to leave the ground, and on a Descent floor falling is the
    // objective anyway, so a jump can never make things worse) and a SIDEWAYS heading, escalating on
    // each consecutive failure.
    //
    // FOUR_STORY ONLY, and that scope is a MEASURED result, not caution. A VERTICAL_HALL stall has an
    // identical-looking signature — bot pinned at y=2.50 on a ramp topping out at 3.0 m, commanding
    // movement on 100% of ticks, 0.4 m of travel per 5 s, 75% of ticks airborne inside a 0.07 m band
    // — so extending the escape there was the obvious move. It does NOT work: paired A/B over 10
    // class-pairs (4 x 25 min, then 6 x 30 min) came out 131 floors with it vs 127 without, i.e. a
    // wash, with the second round actually favouring OFF (74 vs 81). The mechanism is visible in the
    // trace: one paladin floor fired 282 escapes and still took 799 s. SIDEWAYS-FIRST IS THE WRONG
    // REMEDY ON A RAMP. It works on the Descent because that wedge is a flat slab LIP with open floor
    // to either side; a VHALL ramp is a narrow 2-wide graduated slab, so a sidestep walks the bot off
    // the strip — precisely the ramp-drift the follower's point servo corrects. A VHALL riser wedge needs a
    // ramp-aware remedy (back off DOWN the strip and re-approach centred, or a centreline hop), not
    // this one. Do not re-extend without measuring that separately.
    // ...and OVERWORLD ZONES, which are flat by construction (a zone's terrain maps to WILDERNESS /
    // GAUNTLET / HUB — never a stacked style), so this is the Descent's own shape: open floor either
    // side of the wedge, which is exactly the condition that makes sideways-first the right remedy
    // and the VHALL ramp the wrong place for it. Gated on `inZone` rather than on the style because
    // `m_level.layoutStyle` is NOT set on zone entry — it still reads whatever dungeon floor came
    // before (measured: `style=rooms` while standing in zone 58), so a style test here would be
    // silently wrong in both directions.
    //
    // Measured need: the 9-class act soak left a warrior in zone 52 with `flow=0.00 fdir=+0.00,+0.00
    // mv=0 tgts=0` — the veto had refused the routed heading and all four fan detours, so the brain
    // had nothing to command and simply idled, 25 m from a border it never crossed. The no-progress
    // timer could not rescue it either: the bot drifts ~0.4 m per window, which re-anchors the timer
    // (`npt=2.8`) and starves the escape ladder. Being boxed in is POSITIONAL, so it needs the
    // positional detector, not a progress clock.
    if ((m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY || m_level.inZone) && !in.descend &&
        !v.stunned && !v.rolling && ap().lootDwell <= 0.0f) {
        const bool cmdMove = in.moveFwd || in.moveBack || in.moveLeft || in.moveRight;

        // Close the observation window. TWO ways in, because the failure has two symptoms: the bot is
        // commanding movement and not moving (a lip/corner wedge), or the veto refused every direction
        // so it has no heading to command at all (boxed in). Both mean "wants to travel, cannot".
        ap().wedgeWinT += dt;
        if (cmdMove) ap().wedgeCmdT += dt;
        if (ap().flowVetoed) ap().wedgeVetoT += dt;
        if (ap().wedgeWinT >= Autoplay::WEDGE_WIN_SEC) {
            const Vec3 d = m_localPlayer.position - ap().wedgeAnchor;
            const f32  net = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);   // 3D: a fall/pad ride is progress
            if (Autoplay::wedgeDetected(ap().wedgeWinT, ap().wedgeCmdT, net) ||
                Autoplay::boxedDetected(ap().wedgeWinT, ap().wedgeVetoT)) {
                ap().wedgeEscT = Autoplay::WEDGE_BURST_SEC;
                if (ap().wedgeTry < 255u) ap().wedgeTry++;   // next one tries a wider angle
                ap().wedgeCount++;
            } else {
                ap().wedgeTry = 0;                                // freed: back to the first angle
            }
            ap().wedgeWinT = 0.0f; ap().wedgeCmdT = 0.0f; ap().wedgeVetoT = 0.0f;
            ap().wedgeAnchor = m_localPlayer.position;
        }

        if (ap().wedgeEscT > 0.0f) {
            ap().wedgeEscT -= dt;
            ap().remedy = "wedge";   // overrides every producer above — reported by the STALL autopsy
            // Rotate whatever heading we HAVE. flowDir is the routed one; with none (boxed in) fall
            // back to the facing, so the burst still produces a real direction to shove at.
            Vec3 base = v.flowDir;
            if (lengthSq(base) < 1e-6f) {
                const f32 cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
                base = Vec3{-sy, 0.0f, -cy};
            }
            const Vec3 esc = rotateY_XZ(base, Autoplay::wedgeEscapeAngle(ap().wedgeTry));
            const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
            const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
            const f32  df = esc.x * fwd.x + esc.z * fwd.z, dr = esc.x * right.x + esc.z * right.z;
            constexpr f32 kAxis = 0.35f;
            in.moveFwd = df > kAxis; in.moveBack = df < -kAxis;
            in.moveRight = dr > kAxis; in.moveLeft = dr < -kAxis;
            // The jump is the half that actually clears a lip. Mirrors the gate applied to in.jump
            // further up (grounded, not mid-roll) — that gate has already run by the time we get here.
            if (m_localPlayer.onGround && !m_localPlayer.dodgeState.rolling) in.jump = true;

        }
    }

    // STALL AUTOPSY. Fires only once a single floor has run over 5 minutes — rare enough to cost
    // nothing, and by then the run is in trouble and the next question is always the same: does the
    // bot have a route, is it commanding movement, and is it moving? Shipped ON (no env gate)
    // because the last several stalls were diagnosed by rebuilding with a temporary tracer and
    // re-running for half an hour; a stuck floor should explain itself the first time.
    // The 5-minute gate is right for a shipped build (a stall is rare and the log must stay quiet),
    // but it makes the autopsy useless for REPRODUCING one: a repro run needs the geometry dump within
    // seconds, not after the bot has already been stuck for five minutes. AUTOPLAY_STALL_SEC overrides
    // the threshold for a diagnostic run without a rebuild — which is the whole point of shipping the
    // autopsy in the first place, since every stall before it cost a rebuild-with-a-tracer.
    static const f32 kStallGate = []() -> f32 {
        const char* s = std::getenv("AUTOPLAY_STALL_SEC");
        return (s && *s) ? (f32)atof(s) : 300.0f;
    }();
    if (m_autoplayFloorTime > kStallGate) {
        // 15 s is right for a shipped stall (quiet log, and a stall lasts minutes). A REPRO run has
        // lowered the gate deliberately and needs to see an OSCILLATION, which 15 s samples alias away.
        const f32 dumpEvery = (kStallGate >= 300.0f) ? 15.0f : 3.0f;
        ap().stallDumpT += dt;
        if (ap().stallDumpT >= dumpEvery) {
            ap().stallDumpT = 0.0f;
            const bool mv = in.moveFwd || in.moveBack || in.moveLeft || in.moveRight;
            const Vec3 p  = m_localPlayer.position;

            // NET TRAVEL between dumps. The old line reported only "is a key held", which cannot
            // separate a bot that is WEDGED from one that is walking a 1 m loop — and both look
            // identical in a frozen distance-to-door. This is the number that tells them apart.
            const Vec3 d  = p - ap().stallAnchor;
            const f32  netXZ = sqrtf(d.x * d.x + d.z * d.z);
            ap().stallAnchor = p;

            // THE GEOMETRY UNDER AND AHEAD OF THE BODY. Every stacked-floor stall so far has been
            // diagnosed by rebuilding with a tracer to answer one question — "can the body actually
            // make the step the route is asking for?" — so the autopsy answers it directly: the
            // surface underfoot, the surface of the cell one step along the commanded heading, and
            // the rise between them against STEP_UP_HEIGHT (the walk/jump threshold).
            f32 surf = p.y, aheadY = p.y, rise = 0.0f; s32 gx = -1, gz = -1; bool aheadSolid = false;
            Vec3 dbgMv{0, 0, 0};
            {
                u32 cx, cz;
                if (LevelGridSystem::worldToGrid(m_level.grid, p, cx, cz)) {
                    gx = (s32)cx; gz = (s32)cz;
                    surf = LevelGridSystem::effectiveFloorHeight(m_level.grid, cx, cz, p.y);
                }
                // Rebuild the world-space heading the WASD flags actually encode (same basis as
                // faceAndGo) so "ahead" is where the body is being pushed, not where it is aiming.
                const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
                const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
                Vec3 mvDir{0, 0, 0};
                if (in.moveFwd)   mvDir = mvDir + fwd;
                if (in.moveBack)  mvDir = mvDir - fwd;
                if (in.moveRight) mvDir = mvDir + right;
                if (in.moveLeft)  mvDir = mvDir - right;
                if (lengthSq(mvDir) > 1e-6f) dbgMv = normalize(mvDir);
                if (lengthSq(mvDir) > 1e-6f) {
                    const Vec3 step = p + normalize(mvDir) * m_level.grid.cellSize;
                    u32 ax, az;
                    if (LevelGridSystem::worldToGrid(m_level.grid, step, ax, az)) {
                        aheadSolid = LevelGridSystem::isSolid(m_level.grid, ax, az);
                        aheadY = LevelGridSystem::effectiveFloorHeight(m_level.grid, ax, az, p.y);
                        rise   = aheadY - surf;
                    }
                }
            }
            // HOW MANY BODIES ARE TOUCHING US. On flat ground with no rise ahead, the only thing
            // left that can stop a moving body is another body — and the swarm is exactly what pins
            // the no-progress timer near zero (chip damage) so every rescue stays disarmed. Without
            // this count a body-block and a geometry wedge are indistinguishable in the log, which
            // is what made the last VHALL fix aim at the wrong one.
            u32 near2 = 0, near1 = 0;
            for (u32 ti = 0; ti < v.targetCount; ti++) {
                if (v.targets[ti].dist < 2.0f) near2++;
                if (v.targets[ti].dist < 1.2f) near1++;
            }
            // `door` / `bossG` separate the two ways a floor can be unexitable, which look identical
            // from outside: the exit is SEALED behind a live boss (bossG=1 — go fight it), or there is
            // no ordinary exit on this floor at all (door=0 — the brain returns an EMPTY intent via
            // onNormalFloor and the bot idles, the same failure the Source chamber had). A measured
            // 85-minute stall standing 1.0 m from the exit could not be told apart without this.
            // The ROUTED heading vs the heading the FEET actually encode. A stall where these two
            // disagree — or where the routed one reverses tick to tick — is a producer conflict, not
            // geometry, and nothing in the old line could tell those apart.
            LOG_WARN("[STALL] %s fl=%u t=%.0f | flow=%.2f mv=%d jmp=%d grnd=%d net=%.2f | "
                     "cell=%d,%d surf=%.2f ahead=%.2f rise=%+.2f%s%s | y=%.2f exitY=%.1f d2d=%.1f | "
                     "fdir=%+.2f,%+.2f mdir=%+.2f,%+.2f | "
                     "tgts=%u near2=%u near1=%u fire=%d npt=%.1f | door=%d bossG=%d | "
                     "cmt=%d vd=%u fm=%u dB=%.0f bL=%u rem=%s | style=%s",
                     kClassDefs[static_cast<u32>(m_playerClass)].name, m_level.currentFloor,
                     m_autoplayFloorTime, sqrtf(lengthSq(v.flowDir)), (int)mv, (int)in.jump,
                     (int)m_localPlayer.onGround, netXZ,
                     gx, gz, surf, aheadY, rise,
                     (rise > STEP_UP_HEIGHT ? " NEEDS-JUMP" : ""), (aheadSolid ? " WALL" : ""),
                     p.y, m_level.floorDoorPos.y, v.distToDoor,
                     v.flowDir.x, v.flowDir.z, dbgMv.x, dbgMv.z,
                     v.targetCount, near2, near1, (int)in.fire,
                     ap().noProgressTimer, (int)v.doorActive, (int)(v.hasBoss && v.bossAlive),
                     (int)ap().vhCommit, (unsigned)ap().vhFollowDist, (unsigned)ap().vhFollow.mode,
                     ap().bossDist, (unsigned)ap().bossLOS, ap().remedy, LevelGen::styleName(m_level.layoutStyle));
        }
    }

    // AN ACT'S "DOOR" MUST BE LOOKED AT, not merely stood next to.
    //
    // A dungeon floor door is pure proximity (updateFloorDoor), so the brain's descend branch never
    // had to turn — it just holds interact. A zone gate is a WORLD ITEM, and resolveInteractTargets
    // only selects one that is inside the AIM CONE beyond the grab radius. So a bot that arrived at
    // a portal kept whatever heading the last fight left it with and pressed interact at empty air.
    // Measured in the first act soak: two classes stood 1.9 m from the Den's way out for fifteen
    // minutes, holding the button, with a valid route and full health.
    if (m_level.inZone && in.descend) {
        const Vec3 g = autoplayGoalPos();
        const Vec3 d{g.x - m_localPlayer.position.x, 0.0f, g.z - m_localPlayer.position.z};
        if (lengthSq(d) > 0.01f) {
            f32 y, pt;
            Autoplay::dirToAim(d, y, pt);
            in.aimYaw = y;          // pitch is left alone: the cone is judged horizontally
        }
    }
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
    ap().lastPos         = m_localPlayer.position;
    ap().noProgressTimer = 0.0f;
    ap().nudgeTimer      = 0.0f;
    ap().escapeTimer     = 0.0f;
    ap().lookBehindTimer = 0.0f;
    ap().lookBehindDone  = false;

    // THE ACTS OUTRANK THE PORTAL. A bot that wanders out of the Blood Buffer's SOUTH edge lands in
    // the town — correct game behaviour, that edge IS the way home — and the town step would then
    // take the dungeon portal and start a fresh RUN, abandoning an act it was halfway through. The
    // soak caught two classes doing exactly that: their [STALL] lines read `fl=32` and `fl=8`, i.e.
    // deep dungeon floors, after twenty minutes with zero act progress.
    //
    // So while any quest is outstanding, the town's goal is its NORTH GATE, not its portal. Nothing
    // else changes: walking into the gate band hands off to zone 52 through the ordinary
    // host-authoritative transition, which re-checks the Inferno unlock for itself.
    Vec3 townGoal = m_level.townPortalPos;
    bool townPortalIsGoal = true;
    //
    // ...AND SO DOES A FINISHED HERO'S FREE ROAM. The quest test alone covers only a run that is
    // MIDWAY through the acts; a hero who has already finished them has objectiveZone() == 0, so the
    // guard fell open and the town step took the portal — reported as "why did my autoplay bot enter
    // the dungeon after playing the overworld". zoneBotGoal's roam already refuses to AIM at the
    // town link, but that is not enough on its own: combat drift, the escape ladder and an ordinary
    // edge crossing can all put the bot in the town without ever choosing it, and once there the
    // portal was the only thing the town step knew how to want.
    //
    // Keyed on m_laneLoadedFromSave — the PLAYER's own hero, the same predicate that decides roam vs
    // roll-on in zoneBotGoal, so the two halves of "whose character is this" cannot disagree. A
    // mode-MINTED hero is untouched: it has no overworld session to protect and its ending mints the
    // next run anyway, which is what keeps the act soak byte-identical.
    const bool roamingOwnHero = m_laneLoadedFromSave[m_localPlayerIndex];
    if (FreePlay::overworldUnlocked(m_level.savedFloor, m_difficulty) &&
        (ZoneRoute::objectiveZone(m_questMask[m_localPlayerIndex]) != 0 || roamingOwnHero)) {
        // Aimed AT the border line, not at the crossing depth. planTownPortal deliberately stops
        // 1.5 m short of its target — a portal is a HOLD target you stand next to — so aiming at
        // the crossing depth parks the bot just OUTSIDE the trigger band, walking nowhere. Targeting
        // the line puts the stopping point inside it. The step veto still refuses the wall itself,
        // so this cannot walk the bot into geometry.
        townGoal = Vec3{ static_cast<f32>(m_level.grid.width) * 0.5f, 0.0f, 0.0f };
        townPortalIsGoal = false;
    }
    const Autoplay::TownPortalPlan plan =
        Autoplay::planTownPortal(m_localPlayer.position, townGoal);

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

    // Heading for the north gate instead? Then there is nothing to press — walk in and the border
    // hands off, exactly as an overworld edge does.
    if (!townPortalIsGoal) { applyBotIntent(in, uiOpen, dt, /*melee=*/false); return; }

    // Taking the portal rides the SAME pulsed interact the floor exit uses, for the same reason: the
    // portal is an EXIT-class HOLD target, and a continuously-held PICKUP makes Interact::poll fire
    // exactly ONCE — spent on whatever else is in reach (an item, the plaza's stash chest) and then
    // latched `consumed` forever. Pulsing releases the latch so the next hold reaches the portal.
    // m_townPortalRequested is set by that very same updatePlayerPickup arbitration, so the BUTTON is
    // the correct driver here — a direct flag write would be reset before updateTownPortal reads it.
    if (plan.take) {
        ap().descendPulse += dt;
        in.descend = Autoplay::descendPulseHeld(ap().descendPulse);
    } else {
        ap().descendPulse = 0.0f;
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
    if (ap().sidearmCooldown > 0.0f) ap().sidearmCooldown -= dt;
    if (ap().sidearmActive)          ap().sidearmDwell    += dt;

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
        const f32 meleeReach = ap().sidearmActive ? ap().sidearmMeleeRange : v.weaponRange;
        const u8  meleeCell  = ap().sidearmActive ? inv.buildCell : v.buildCell;
        const Autoplay::Doctrine doc = Autoplay::doctrineFor(meleeCell);
        // engageCeiling's own formula, on the melee numbers (v.weaponRange is the sidearm's).
        const f32 ceil = fmaxf(doc.engageMax * meleeReach, Autoplay::THREAT_RADIUS);
        for (u32 i = 0; i < v.targetCount; i++) {
            const Autoplay::BotTarget& t = v.targets[i];
            if (!t.hasLOS || t.dist > ceil) continue;
            if (t.dist <= meleeReach * 1.2f) continue;             // already in / near melee reach
            // The "can't walk there" fact is computed ONCE in buildBotView and shared, so the sidearm
            // and the gap-close suppression can never disagree about which targets are across a gap.
            if (t.onlyReachableByFall) { trigger = true; break; }
        }
    }

    if (!ap().sidearmActive) {
        // --- Consider switching TO the sidearm. ---
        if (trigger && ap().sidearmCooldown <= 0.0f && v.weaponIsMelee) {
            const s32 idx = BuildScore::bestRangedBackpackIdx(inv, m_itemDefs, m_itemDefCount);
            // The draw/stow is logged (not just chatted) because "the sidearm stopped working" is
            // otherwise invisible in a soak: addChatMessage only reaches the on-screen chat, so a
            // regression here can only be seen by watching the game. The idx < 0 line separates the
            // two failures that look identical from outside — the trigger never fires, versus it
            // fires and the BAG has no ranged weapon to draw (an auto-loot/keeper regression).
            if (idx < 0) {
                if (m_autoplaySidearmNoWeaponLog < 3u) {   // a few per run: enough to diagnose, not spam
                    m_autoplaySidearmNoWeaponLog++;
                    LOG_INFO("Autoplay: sidearm WANTED on floor %u but the bag holds no ranged weapon",
                             m_level.currentFloor);
                }
            } else {
                ap().sidearmMeleeUid   = inv.equipped[(u32)ItemSlot::WEAPON].uid;  // stash BEFORE equip
                ap().sidearmMeleeRange = v.weaponRange;   // the melee reach the trigger keeps judging by
                Inventory::equip(inv, (u8)idx, m_itemDefs);   // ranged weapon -> WEAPON slot; melee -> bag
                ap().sidearmActive = true;
                ap().sidearmDwell  = 0.0f;
                addChatMessage("", "Autoplay: drew a ranged sidearm", Vec3{0.6f, 0.85f, 1.0f});
                LOG_INFO("Autoplay: SIDEARM drawn on floor %u (melee reach %.1f m)",
                         m_level.currentFloor, ap().sidearmMeleeRange);
            }
        }
        return;
    }

    // --- Active: consider switching BACK to melee. ---
    // Keep it while the trigger holds OR the min dwell has not elapsed. Otherwise put the melee
    // weapon back by finding the stashed uid in the bag (its slot can move as loot comes and goes).
    if (trigger || ap().sidearmDwell < kMinDwell) return;

    s32 meleeIdx = -1;
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++)
        if (inv.backpack[i].defId != 0xFFFF && inv.backpack[i].uid == ap().sidearmMeleeUid) { meleeIdx = i; break; }
    if (meleeIdx >= 0) Inventory::equip(inv, (u8)meleeIdx, m_itemDefs);   // melee -> WEAPON slot
    // If the stashed weapon vanished (should not happen — nothing discards the equipped-then-bagged
    // melee weapon while the sidearm guard blocks auto-equip), fall through: clearing the flag lets
    // the next autoEquipBackpack re-gear the melee build normally.
    ap().sidearmActive   = false;
    ap().sidearmCooldown = kCooldown;
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
    // The bot's OWN leashes on top of the engine cooldown (see ap().dodgeCd) — the policy asks
    // for a roll only when the matching one has expired.
    v.dodgeAllowed    = ap().dodgeCd    <= 0.0f;
    v.gapCloseAllowed = ap().gapCloseCd <= 0.0f;
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
            v.skillGapDist[s]    = def->distance;
            // SUMMON / DEPLOY skills. SkillDef carries no flag for this, so classify by id — these are
            // the four that leave a persistent ALLY behind (Tinkerer drones/queen, Combat Engineer
            // turret/coil). Their value is independent of the current target, so the policy fires them
            // whenever they are off cooldown rather than saving them for a good moment.
            //
            // TESLA_COIL IS NOT ONE, and listing it here was the summon classes' whole energy
            // problem. It leaves nothing behind — `fireTeslaCoil` is a 360-degree query that damages
            // and staggers, a pure burst. Classed as a summon it inherited the "cast whenever off
            // cooldown" priority ABOVE everything else, so the Combat Engineer spent 25 energy every
            // 5 s on it and could never afford the 40-energy Deploy Turret when that came up. It is
            // an AoE (radius 4), which skillIsAoe already reports, so the group branch and the dump
            // still use it — at the right priority.
            v.skillIsSummon[s] = (id == SkillId::SWARM_DEPLOY)  || (id == SkillId::SWARM_QUEEN) ||
                                 (id == SkillId::DEPLOY_TURRET);
            // Reactive parry (Wanderer Deflect): cast on the block-tap triggers, never on cooldown.
            v.skillIsCounter[s] = (id == SkillId::DEFLECT);
            // Spends the swarm to deal damage and kills it. Last resort only — see autoplay_combat.h.
            v.skillIsMinionSacrifice[s] = (id == SkillId::DETONATE_SWARM);
            v.skillIsMinionBuff[s]      = (id == SkillId::OVERCLOCK);
            // Recorded for EVERY unlocked slot, castable or not: the reserve rule below has to know
            // what a summon costs precisely when the pool cannot yet afford it.
            v.skillCost[s] = def->energyCost;
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
    if (ap().sidearmActive) v.buildCell = Autoplay::rangedCellFor(v.buildCell);

    // --- world gate: idle in town / arena, and only travel while an ordinary exit exists ---
    //
    // THE SOURCE CHAMBER IS A FIGHT, NOT A WORLD THE BRAIN CANNOT EXPRESS. It used to be lumped in
    // with town/arena as "idle here", and the cost of that was the single largest waste measured in
    // the 3 h couch soak: three of nine sessions collected all ten Source shards across a full Hell
    // run, opened the portal on floor 50, walked in — and then stood still for the remaining ~2 hours
    // (they were the ONLY silent sessions, and the only ones that entered). The brain returns an
    // empty intent when this is false, so the bot did not fight, drink or move while the Engine and
    // its summoned waves worked on it. Earning the secret boss and then refusing to play it is the
    // worst of both outcomes.
    //
    // Nothing else needs to change to support it: the chamber has no exit (floorDoorActive is false
    // by construction, so DESCEND stays disarmed), its flow field is seeded at the centre where the
    // Engine stands (so TRAVEL walks toward the fight), and pickTarget already skips an invulnerable
    // target — so while the Engine is shielded the bot fights the adds, which is the intended answer.
    // A ZONE now IS a world the brain can express: engine_autoplay_zone.cpp gives it a goal (the
    // quest chain) and presents that goal as the floor door, so every travel/fight/stall mechanism
    // works unchanged. The town and the arena are still worlds the bot does not play.
    v.onNormalFloor = !(m_level.inTown || m_level.inArena) &&
                      (m_level.floorDoorActive || m_level.inSourceChamber || m_level.inZone);
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

        if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL) {
            // Per-tick takeoff stash: cleared unconditionally so a stale press can never fire later.
            ap().vhFollowJump    = false;
            ap().vhFollowJumpDir = Vec3{0, 0, 0};
            ap().vhFollowDist    = 0xFFFF;
            // THE FIELD IS THE SINGLE AUTHORITY, THE FOLLOWER ITS ONLY EXECUTOR. The field (with
            // the broken-catwalk JUMP LINKS as cost-3 edges, which reconnect the isolated W balcony
            // and delete the legitimate-but-fatal down-the-ramp routes) picks the next node; the
            // follower latches it and steers at its CENTRE (a point servo — lateral drift
            // self-corrects); a committed jump is executed with an aligned, planVault-gated press.
            // Nothing else writes v.flowDir in this branch.
            //
            // This REPLACED a stack of four assists (ramp anti-drift, ground centreline blend,
            // jump-pad beeline, hop-pulse machinery) layered ABOVE a link-free field readout — each
            // a compensation for the previous one's failure, and the pile the reason a bot could
            // circle an upper-exit floor for 6000+ s unseen. A/B (one binary + env switch, 6
            // classes x both arms, 25 min): floors 71 vs 49, kills 2223 vs 1535, upper-exit pins
            // 132.5/h vs 223/h, worst dwell 874 vs 1425 s; the confirming couch soak cut VHALL from
            // 78% of all pinned stall samples to 6%, with 8/9 sessions reaching floor 50.
            // dt is the fixed sim step: updateAutoplay runs once per 1/60 s tick by construction.
            if (Autoplay::ensureVHallField(ap().vHall, m_level.grid, dg, m_level.floorDoorPos,
                                           floorStamp, /*useJumpLinks=*/true)) {
                const Autoplay::VHallFollowOut fo = Autoplay::vhallFollowTick(
                    ap().vhFollow, ap().vHall, m_level.grid, dg, pos, pos.y,
                    m_localPlayer.yaw, m_localPlayer.onGround, 1.0f / 60.0f);
                if (lengthSq(fo.dir) > 1e-6f) v.flowDir = fo.dir;
                ap().vhFollowJump    = fo.wantJump;
                ap().vhFollowJumpDir = fo.jumpDir;
                ap().vhFollowDist    = fo.distHere;
            }
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
            ap().descentStory = Autoplay::commitBotStory(m_level.grid, pos, ap().descentStory);
            if (Autoplay::ensureDescentField(ap().descent, m_level.grid, dg,
                                             ap().descentStory, floorStamp, m_level.floorDoorPos)) {
                const Vec3 dd = Autoplay::descentDirection(ap().descent, m_level.grid, pos);
                if (lengthSq(dd) > 1e-6f) {
                    v.flowDir = dd;
                } else if (Autoplay::onJumpPad(m_level.grid, pos)) {
                    // STANDING ON A RETURN LIFT with no route. Pads are excluded from the field on
                    // purpose, so descentDirection says nothing here — and a bot with nothing to say
                    // simply stands on the pad, is relaunched, falls, lands on it again. That loop IS
                    // the Descent floor PARK (traced: 12 minutes with distance-to-exit oscillating
                    // between two fixed values, 47% of ticks airborne, 32% with no heading). Steer it
                    // off the pad to the nearest routable non-pad cell and the descent resumes.
                    const Vec3 esc = Autoplay::padEscapeDirection(ap().descent, m_level.grid, pos);
                    if (lengthSq(esc) > 1e-6f) v.flowDir = esc;
                }
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
        ap().shrineTarget = false;
        if (!v.stackedFloor && !m_level.lavaFloor) {
            constexpr f32 kShrineDetour = 8.0f;   // metres: a minor detour, not a cross-floor trek
            f32 bestD2 = kShrineDetour * kShrineDetour;
            for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
                const WorldItem& w = m_worldItems.items[i];
                if (!w.active || !isShrine(w.item)) continue;
                const f32 dx = w.position.x - pos.x, dz = w.position.z - pos.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; ap().shrinePos = w.position; ap().shrineTarget = true; }
            }
            if (ap().shrineTarget) {
                const Vec3 to{ap().shrinePos.x - pos.x, 0.0f, ap().shrinePos.z - pos.z};
                // Steer to the shrine ONLY with a clear line; a shrine is optional, never worth
                // wall-hugging toward one behind a wall. The flag stays set so the activation still
                // fires if the ordinary exit route happens to bring the bot into range.
                if (lengthSq(to) > 1e-6f && clearLineTo(ap().shrinePos)) v.flowDir = normalize(to);
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
                // GOAL SUBSTITUTION, not a seek assist. Two prior shapes both failed, differently:
                // the 25 m radius left the at-door park (4256 s at eff65: the bot ON the sealed door,
                // d2d median 1 m, boss alive, never closing), and simply DROPPING the radius A/B'd
                // WORSE (boss floors slower, 57->80 s median dwell) because an always-on straight
                // seek fought the exit field and the local fight. The conceptual defect was that the
                // EXIT field stayed the travel authority on a floor whose exit cannot open: at the
                // door it reads "at goal" and emits nothing, and the seek was an assist competing
                // with it. So while a milestone boss lives, the boss-seeded wall-aware RouteField IS
                // the travel field (the exit field takes over the tick the boss dies); the straight
                // bearing survives only as the last-metres fallback when the route field is invalid
                // AND the line is clear. dBoss/bLOS are stashed for the boss-floor [STALL] autopsy.
                ap().bossDist = dBoss;
                ap().bossLOS  = clearLineTo(bossPos) ? 1 : 0;
                const bool substitute = dBoss > 1e-3f;
                if (substitute) {
                    // The wall-aware BFS RouteField seeded from the boss (autoplay_route.h): routes
                    // around walls, defined on every reachable cell, steers at cell centres, no A*
                    // cap, rebuilds only when the boss changes cell. THE authority here.
                    bool routed = false;
                    if (Autoplay::ensureRouteField(ap().bossRoute, m_level.grid, bossPos, floorStamp)) {
                        const Vec3 rd = Autoplay::routeDirection(ap().bossRoute, m_level.grid, pos);
                        if (lengthSq(rd) > 1e-6f) { v.flowDir = rd; routed = true; }
                    }
                    // Route field invalid or at-goal with the boss offset from its cell: the straight
                    // bearing closes the last open stretch, but ONLY with a clear line (through a wall
                    // it jams — the measured "navigate to the boss even behind a wall" freeze).
                    if (!routed && ap().bossLOS) v.flowDir = normalize(toBoss);
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
    // Cleared every tick and raised only in the all-detours-refused branch below, so it means exactly
    // "the router wanted to go somewhere and the veto left it with nowhere" — the boxed-in signal the
    // wedge escape watches (a bot merely standing still has no heading here to veto).
    ap().flowVetoed = false;
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
        // PAD PREFERENCE, NOT PAD FEAR. The descent FIELD already prefers a pad-free descent (its
        // tier-1 flood excludes pads and only tier-2 routes through them), which is the right place
        // to express "a return lift undoes the descent" — it costs the bot nothing when a clean route
        // exists. The hard VETO on top of that was the part that read as fear: it refused any step
        // whose footprint so much as clipped a pad, so the bot skirted them, boxed itself in corners
        // beside them, and stalled next to lifts it could simply have crossed. Removed on Aaron's
        // call. The consequence — an occasional unplanned launch — is now recoverable in a way it was
        // not when the veto was written: padEscapeDirection gets the bot off a lift, the wedge escape
        // digs it out of a corner, and the field re-routes from wherever it lands.
        const bool avoidPads = false;
        if (!Autoplay::stepAllowed(m_level.grid, v.pos, feetY, v.flowDir, m_level.lavaFloor, avoidPads)) {
            constexpr f32 kFan[4] = { 0.7853981634f, -0.7853981634f,     // ±45°: the gentle detour
                                      1.5707963268f, -1.5707963268f };   // ±90°: the square sidestep
            Vec3 pick{0, 0, 0};
            for (u32 i = 0; i < 4; i++) {
                const Vec3 cand = rotateY_XZ(v.flowDir, kFan[i]);
                if (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, cand, m_level.lavaFloor, avoidPads)) { pick = cand; break; }
            }
            if (lengthSq(pick) > 1e-6f) v.flowDir = pick;
            else { v.flowDir = Vec3{0, 0, 0}; ap().flowVetoed = true; }   // boxed: nowhere left to step
        }
    }

    // --- descend gate context (consumed by the brain's mayDescend mirror) ---
    v.doorActive  = m_level.floorDoorActive;
    v.distToDoor  = length(m_level.floorDoorPos - m_localPlayer.position);
    v.hasBoss     = m_level.floorHasBoss;
    v.bossAlive   = floorBossAlive();

    // An ACT overrides all four of those: its "door" is the next hop on the quest road, and its
    // "boss" is whatever a SLAY quest is asking for. Applied AFTER the dungeon values so there is
    // exactly one place the two worlds meet, rather than four conditionals threaded above.
    if (m_level.inZone) zoneFillBotView(v);

    // The one layout where "don't fall to reach an enemy" is a rule (see BotTarget::onlyReachableByFall).
    const bool vhUpperExitFloor = m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL &&
                                  m_level.floorDoorPos.y > 1.5f;
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
        // Boss-floor sustain priority (see BotTarget::isHealer): HEALER shamans + SUMMONER
        // necromancers — the roles that heal the pack back up / resurrect it. Role is a bitmask, so
        // a boss that IS a necromancer reads as both, and the healer-first rule simply targets it.
        t.isHealer = (e.enemyRole & (EnemyRole::HEALER | EnemyRole::SUMMONER)) != 0;
        // Currently DAMAGE-IMMUNE — mirror Combat::applyDamage's early returns so the bot never wastes
        // shots (or, for a gargoyle, keeps it asleep by staring). A dormant AMBUSH gargoyle, an entombed
        // boss (Malachar's channel), the Engine while its wave adds live. minionShield is NOT here: it
        // is only 75% reduction, not immunity, so a shielded boss is still worth shooting.
        t.invulnerable = (e.aiState == AIState::DORMANT && (e.enemyRole & EnemyRole::AMBUSH)) ||
                         (e.bossPhase == BossPhase::ENTOMBING) ||
                         (e.isEngine && Combat::engineShieldActive(m_entities, m_entities.activeList[a]));
        t.bossShielded = e.isBoss && e.minionShield;   // 75% DR while its brood lives: kill the adds first
        // UNREACHABLE WITHOUT FALLING (VHALL upper-exit climbs only — see BotTarget). Computed here,
        // once, so the melee sidearm and the gap-close suppression cannot disagree about which
        // targets are across a gap: the sidearm draws a gun FOR these, and the blink must not fire AT
        // them. Cheap: one grid lookup per candidate, and only on the one layout where it applies.
        if (vhUpperExitFloor) {
            const Vec3 toT{t.pos.x - v.pos.x, 0.0f, t.pos.z - v.pos.z};
            t.onlyReachableByFall = lengthSq(toT) > 1e-6f &&
                                    Autoplay::wouldFall(m_level.grid, v.pos, v.pos.y, toT);
        }

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
                if (ap().velId[j] != s_targets[i].id || ap().velId[j] == 0) continue;
                sm = ap().velEma[j] + (raw - ap().velEma[j]) * alpha;
                break;
            }
            freshId[i] = s_targets[i].id; freshVel[i] = sm;
            s_targets[i].vel = sm;
        }
        // Rebuilt wholesale each tick, so a target that left the list simply drops its history —
        // which is what we want: re-acquiring it later should not lead on a stale velocity.
        for (u32 j = 0; j < AIM_VEL_SLOTS; j++) { ap().velId[j] = freshId[j]; ap().velEma[j] = freshVel[j]; }
    }

    v.targets     = s_targets;
    v.targetCount = n;

    // Minions a buff would actually reach. The predicate MIRRORS SkillSystem::fireOverclock exactly
    // (friendly, alive, npcClass NONE — class NPCs like the cleric are skipped there, so counting
    // them here would fire the buff on an empty swarm and waste the pool the reserve just protected).
    // Counted in the driver rather than passed as a flag because the pure policy must stay engine-free.
    v.minionCount = 0;
    for (u32 a2 = 0; a2 < m_entities.activeCount; a2++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a2]];
        if (!(e.flags & ENT_FRIENDLY) || (e.flags & ENT_DEAD)) continue;
        if (e.npcClass != NpcClass::NONE) continue;
        v.minionCount++;
    }

    // TARGET STICKINESS: resolve the remembered entity identity back to a slot in THIS tick's array
    // (it is re-sorted by distance every tick, so the index from last tick means nothing). Not found
    // = the enemy died, despawned, or fell out of the nearest-kMaxTargets cap — either way the memory
    // is stale and pickTarget falls back to plain nearest-LOS.
    v.currentTargetIdx = -1;
    if (ap().targetId != 0) {
        for (u32 i = 0; i < n; i++)
            if (s_targets[i].id == ap().targetId) { v.currentTargetIdx = (s32)i; break; }
    }
    v.targetSwitchAllowed = ap().targetDwell >= Autoplay::TARGET_MIN_DWELL;

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
    // WEAPON-THROW TAP OVERRIDE. While the synthetic tap is in flight it OWNS the Fire button — the
    // sequence is release -> brief press -> release, and letting the ordinary auto-fire hold leak
    // through would keep `held` above TAP_SEC and turn the intended throw back into a plain swing.
    const bool throwTap    = (ap().throwSeq >= 0.0f);
    const bool throwTapDown = throwTap && WeaponThrow::botTapFireHeld(ap().throwSeq);
    Input::setBotHeld(GameAction::FIRE,   throwTap ? (throwTapDown && !uiOpen)
                                                   : (in.fire && onTarget));
    Input::setBotHeld(GameAction::BLOCK,  in.block);
    Input::setBotHeld(GameAction::DODGE,  in.dodge);
    Input::setBotHeld(GameAction::POTION, in.potion);
    // RELOAD also carries the THROWAWAY gun toss (a one-tick press edge set by updateAutoplay).
    Input::setBotHeld(GameAction::RELOAD, in.reload || ap().reloadPulse);
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
    if (ap().sidearmActive) {
        PlayerInventory& inv = m_inventories[0];
        for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++)
            if (inv.backpack[i].defId != 0xFFFF && inv.backpack[i].uid == ap().sidearmMeleeUid) {
                Inventory::equip(inv, i, m_itemDefs);
                break;
            }
        ap().sidearmActive = false;
    }
    m_autoplayActive = false;
    Input::setBotOverlayActive(false);   // also clears any held synthetic actions (input.cpp)
    m_autoplayRespawnTimer = 0.0f;
    m_autoplayBotDeath     = false;      // leaving the run must not leave a revive latched
}

// THE STANDARD ENDING'S CONTINUATION (2026-08-01). Beating the game used to END an autoplay run:
// the VICTORY screen returned to the MENU and disarmed the bot, so the single best outcome the mode
// can produce was also the one that stopped it playing — the same class of hole as the credits park
// and the death-screen strand, one screen further on. (The Engine-slayer's ending already rolls into
// the TOWN and continues from there; this is the OTHER ending.) A finished run now mints the next
// one directly.
//
// Three rules make that safe rather than merely automatic:
//   * NEVER overwrite the champion. A menu-started run owns a real save slot, so the next run takes
//     the first FREE one; if the save list is full — or the run never had a slot at all (a CLI/dev
//     run saves nowhere: slot 0 is `saveCharacter`'s own "don't save" sentinel) — the new run plays
//     UNSAVED. Losing a run's progress is recoverable; clobbering a character is not.
//   * ROTATE the class per lane. An endless loop replaying one class is a worse demo and a much
//     worse soak than one that walks the roster — soak13's 100x wdps spread across classes is
//     exactly the kind of thing a single-class loop hides.
//   * Fresh hero, floor 1, back down to Normal. The difficulty ladder is a per-RUN progression
//     (floor 50 -> next difficulty) and this hero just finished the whole of it.
void Engine::autoplayNextRun() {
    const u8 lanes = (m_splitPlayerCount > 0) ? m_splitPlayerCount : 1;

    for (u8 L = 0; L < lanes && L < MAX_LOCAL_PLAYERS; L++) {
        const u8 next = static_cast<u8>((static_cast<u8>(m_playerClasses[L]) + 1) %
                                        static_cast<u8>(PlayerClass::CLASS_COUNT));
        m_playerClasses[L] = static_cast<PlayerClass>(next);
    }

    // Slot reassignment. RE-SCAN FIRST — this is load-bearing, not hygiene: `m_saveSlots` is only
    // ever refreshed by the menu, so on this path it can be stale by an entire run. A hero that
    // started as a fresh New Game in slot 12 wrote save_12 during play, and against a scan taken
    // before that the slot still reads FREE — so the "never overwrite the champion" rule would hand
    // the new run the champion's own slot. (A CLI launch never scans at all, which would make every
    // slot read free and put the new run on slot 1, on top of whatever lives there.)
    scanSaveSlots();
    // ...then per lane, COLLISION-CHECKED: nothing has been written for the NEW run yet, so
    // firstFreeSaveSlot() would hand both couch lanes the same empty slot and the second save would
    // silently eat the first.
    u8 taken = 0;
    for (u8 L = 0; L < MAX_LOCAL_PLAYERS; L++) {
        if (m_playerSaveSlot[L] == 0) continue;          // was already an unsaved (dev/CLI) lane
        u8 slot = firstFreeSaveSlot();
        if (slot != 0 && slot == taken) {                // lane 0 just claimed it — take the next
            slot = 0;
            for (u32 i = taken; i < MAX_SAVE_SLOTS; i++)
                if (!m_saveSlots[i].exists) { slot = static_cast<u8>(i + 1); break; }
        }
        m_playerSaveSlot[L] = slot;                      // 0 = the list is full: play on, don't save
        if (slot != 0) taken = slot;
    }

    m_difficulty               = 0;   // Normal — the ladder restarts with the hero
    m_level.currentFloor       = 1;
    m_level.inSourceChamber    = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;

    applyClassToLane0(m_playerClasses[0]);
    if (lanes > 1) {
        // Couch: prepare BOTH lanes explicitly and start `lanesPrepared`, exactly as the
        // --autoplay-couch dev door does (the NEW_GAME wipe only equips lane 0's hero).
        for (u8 L = 0; L < lanes && L < MAX_LOCAL_PLAYERS; L++) equipFreshLane(L);
        startGame(GameStart::NEW_GAME, /*lanesPrepared=*/true);
    } else {
        startGame(GameStart::NEW_GAME);
    }
    enterAutoplayRun(/*freshCharacter=*/true);   // re-arms the bot + seeds each lane's build cell

    LOG_INFO("[AUTOPLAY] run finished -> next run: %s%s%s, slot %u",
             kClassDefs[static_cast<u32>(m_playerClasses[0])].name,
             lanes > 1 ? " + " : "",
             lanes > 1 ? kClassDefs[static_cast<u32>(m_playerClasses[1])].name : "",
             (unsigned)m_playerSaveSlot[0]);
}

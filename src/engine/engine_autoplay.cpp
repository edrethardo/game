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
//   A  wedged AT the exit          -> stand still and force the descend hold.
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
#include "engine/engine.h"
#include "platform/input.h"
#include "world/raycast.h"        // Raycast::cast — the WORLD-ONLY (slab-aware) DDA behind the target LOS test
#include "world/level_grid.h"
#include "world/story_nav.h"      // StoryNav::onUpperStory / nearestPortalGoal — per-style vertical routing
#include "world/pathfinder.h"     // Pathfinder::findPath — Stage-3 escape's short A* leg toward the exit
#include "game/autoplay_nav.h"    // Autoplay::stepAllowed / escapeHeading — the travel hazard veto + 8-dir escape
#include "game/autoplay_combat.h" // Autoplay::dirToAim / doctrineFor — nudge heading + in-band fight test
#include "game/item.h"            // GLOBE_HEALTH_ID / m_worldItems — low-hp globe detours
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
    m_autoplayControl.tick(Input::humanActivityThisFrame(), uiOpen, dt);

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

    Autoplay::BotView v = buildBotView();
    Autoplay::BotIntent in = Autoplay::decide(v);

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
    {
        const Vec3 p  = m_localPlayer.position;
        const f32  dx = p.x - m_autoplayLastPos.x, dz = p.z - m_autoplayLastPos.z;
        const bool progressed = (dx * dx + dz * dz) > 0.25f;   // > 0.5 m from the anchor

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
        combatProgress = (v.targetCount < m_autoplayLastEnemyCount) ||
                         (enemyHp < m_autoplayLastEnemyHp - 0.5f);
        m_autoplayLastEnemyHp    = enemyHp;
        m_autoplayLastEnemyCount = v.targetCount;

        if (progressed) {
            // Real XZ progress resumed (moved > 0.5 m from the wedge anchor): re-anchor and DROP the
            // whole escape ladder so the bot returns to plain flow-field travel (reset on progress).
            m_autoplayLastPos = p; m_autoplayNoProgressTimer = 0.0f;
            m_autoplayNudgeTimer = 0.0f; m_autoplayEscapeTimer = 0.0f;
        } else if (combatProgress) {
            // Dealing damage in place is progress too (a real fight, not a wedge): hold the timer + escape
            // ladder at zero WITHOUT moving the anchor (the bot hasn't travelled, it's killing things).
            m_autoplayNoProgressTimer = 0.0f;
            m_autoplayNudgeTimer = 0.0f; m_autoplayEscapeTimer = 0.0f;
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
    // last few seconds did the bot get MEANINGFULLY closer to the exit OR deal combat damage? If NEITHER,
    // it is livelocked on this floor — bull to the exit (Remedy A) and leave. A RATE check (approach > 1 m
    // per window), not a best-distance one, so a slow inward spiral that never actually arrives still
    // trips it (a best-distance test kept resetting on the crawl and never fired).
    constexpr f32 kDoorCheckWindow = 4.0f;   // evaluate exit progress every 4 s
    constexpr f32 kDoorApproachMin = 1.0f;   // must close at least 1 m toward the door per window
    if (m_level.currentFloor != m_autoplayLastFloor) {   // new floor: re-anchor the window, drop the latch
        m_autoplayLastFloor      = m_level.currentFloor;
        m_autoplayDoorCheckDist  = v.distToDoor;
        m_autoplayExitStallTimer = 0.0f;
        m_autoplayExitBull       = false;
    }
    if (v.doorActive && !bossGate) {
        if (combatProgress) {   // a kill/damage this tick = a real fight worth finishing: restart the window
            m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;
            m_autoplayExitBull      = false;
        } else {
            m_autoplayExitStallTimer += dt;
            if (m_autoplayExitStallTimer >= kDoorCheckWindow) {
                // Window elapsed with no kills: did we close > 1 m toward the exit in it? The latch LATCHES
                // (rather than firing for a tick) so the bull runs continuously until the next window shows
                // real progress — a one-shot commit left duty-cycle gaps the kite immediately undid.
                m_autoplayExitBull      = (m_autoplayDoorCheckDist - v.distToDoor) < kDoorApproachMin;
                m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;   // next window
            }
        }
    } else {
        m_autoplayDoorCheckDist = v.distToDoor; m_autoplayExitStallTimer = 0.0f;
        m_autoplayExitBull      = false;   // no eligible exit (boss alive / town): idle the watchdog
    }
    // Suppress the combat break-off while bulling for the exit or standing on it — leaving the floor wins
    // over re-angling a fight we've already given up on.
    if (Autoplay::combatStalled(m_autoplayNoProgressTimer, inBandFight, combatProgress) &&
        !m_autoplayExitBull && !v.atExit && m_autoplayBreakoffTimer <= 0.0f)
        m_autoplayBreakoffTimer = 1.5f;   // arm a relocation leg (re-armed only after the timer expires)
        // NB: no flowDir requirement — the break-off STRAFES around the target (unstickCombatMove), which
        // needs no exit heading, so it works even when the bot is boxed and flowDir is vetoed to zero
        // (exactly the pocket the bot froze in: firing at an unhittable target with no flow to walk).

    // Remedy A (priority) — WEDGED right at the exit with the boss dead: an unreachable LOS straggler keeps
    // FIGHT active but the bot can't close, so stand still and force the descend (hold PICKUP, drop
    // fire/move) — the interact-hold completes over the next few ticks and we leave.
    if (stuck && v.doorActive && v.distToDoor < 2.5f && !bossGate) {
        in = Autoplay::BotIntent{};
        in.aimYaw = m_localPlayer.yaw; in.aimPitch = m_localPlayer.pitch;
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
        if (v.distToDoor > 3.0f) {   // far: prefer a wall-avoiding A* first leg over the straight line
            Vec3 wp[MAX_PATH_WAYPOINTS];
            const u8 n = Pathfinder::findPath(m_level.grid, pos, m_level.floorDoorPos, wp,
                                              MAX_PATH_WAYPOINTS, 0.3f);
            if (n > 0) {
                const Vec3 toWp{wp[0].x - pos.x, 0.0f, wp[0].z - pos.z};
                if (lengthSq(toWp) > 1e-6f) heading = toWp;
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
        }
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
    // JUMP only from the ground (the engine ignores it otherwise, but asking for what cannot happen
    // muddies the telemetry) and never while a roll owns the body.
    if (in.jump && (!m_localPlayer.onGround || m_localPlayer.dodgeState.rolling)) in.jump = false;

    applyBotIntent(in, uiOpen, dt, v.weaponIsMelee);
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
    }

    // Deterministic cadence clock for the strafe flip + the kiting jump (never rand()).
    v.tick = currentLocalTick();

    // --- weapon (effective, incl. affixes) ---
    // Mirror getEffectiveWeapon; MELEE/HITSCAN carry no projectile lead (projSpeed 0), only PROJECTILE.
    const WeaponDef w = Inventory::getEffectiveWeapon(m_inventories[0], m_itemDefs, m_weaponDefs[0]);
    // NOT w.range directly: projectile weapons author no range at all (see botWeaponRange), and a
    // 0 there zeroes the doctrine's whole engagement band, so the bot would never fire a wand/bow.
    v.weaponRange     = Autoplay::botWeaponRange(w.range, w.projectileSpeed);
    v.weaponProjSpeed = (w.type == WeaponType::PROJECTILE) ? w.projectileSpeed : 0.0f;
    v.weaponIsMelee   = (w.type == WeaponType::MELEE);
    v.buildCell       = m_inventories[0].buildCell;

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
            // The Descent: the exit is always DOWN. Steer to the nearest same-story drop hole that
            // is NOT a return lift — Autoplay::pickDropHole owns that rule (a pad one story under a
            // hole flings the bot straight back up through it, which is the loop that kept the bot
            // on floor 1 forever). Stepping onto it drops a story; gravity does the rest. No
            // same-story hole → keep the flat heading, which on L0 is exactly the walk to the door.
            const s32 hi = Autoplay::pickDropHole(m_level.grid, dg, pos);
            if (hi >= 0) {
                const Vec3 g = dg.dropHoles[(u8)hi].pos;
                const Vec3 to{g.x - pos.x, 0.0f, g.z - pos.z};
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
        if (!Autoplay::stepAllowed(m_level.grid, v.pos, feetY, v.flowDir, m_level.lavaFloor)) {
            constexpr f32 kFan[4] = { 0.7853981634f, -0.7853981634f,     // ±45°: the gentle detour
                                      1.5707963268f, -1.5707963268f };   // ±90°: the square sidestep
            Vec3 pick{0, 0, 0};
            for (u32 i = 0; i < 4; i++) {
                const Vec3 cand = rotateY_XZ(v.flowDir, kFan[i]);
                if (Autoplay::stepAllowed(m_level.grid, v.pos, feetY, cand, m_level.lavaFloor)) { pick = cand; break; }
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

    m_localPlayer.yaw = Autoplay::stepAngle(m_localPlayer.yaw, desiredYaw, kAimGain, rate, dt);
    // Pitch rides the same ease + cap (no wrapping needed — stepAngle's fold is a no-op inside ±89°).
    f32 pitch = Autoplay::stepAngle(m_localPlayer.pitch, desiredPitch, kAimGain, rate, dt);
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

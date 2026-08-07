// engine_autoplay_zone.cpp — the Autoplay driver's overworld half: how a bot plays an ACT.
//
// WHY THIS IS SEPARATE FROM THE DUNGEON DRIVER. In the dungeon the objective is always the same
// sentence — "get to the door" — and the whole brain is built around it: a flow field toward the
// exit, a descend gate, a boss that seals it. An ACT is a different sentence. There is no descent;
// there is a quest chain, a road between fifteen worlds, and a reason to walk backward through a
// portal you already used. That is a routing problem (game/zone_route.h) plus a small amount of
// engine glue, and mixing it into engine_autoplay.cpp would bury both.
//
// THE TRICK THAT MAKES IT CHEAP. Rather than teach the brain a new objective, the zone objective is
// presented to it AS THE FLOOR DOOR:
//
//   * the level's flow field is rebuilt toward whatever the act currently wants (a quest boss, the
//     nearest straggler, the next hop's gate) — so TRAVEL, the hazard veto, the heading commit, the
//     escape ladder and the stall watchdogs all work unchanged;
//   * `doorActive` + `distToDoor` are pointed at the next HOP, so the brain's DESCEND branch walks
//     the last metres and pulses interact — which is exactly what a portal needs, and harmless at a
//     border gate, where crossing happens on proximity anyway;
//   * `hasBoss`/`bossAlive` are set for a SLAY quest, which reuses the dungeon's whole boss stack
//     (goal substitution toward the boss, healer-first targeting, the closing commit) and seals the
//     "door" until the quest is done — the same rule, for the same reason.
//
// So the bot gains an act without the brain gaining a branch. What lives here is only: what does the
// act want next, where is that in the world, and when has this run finished.

#include "engine/engine.h"
#include "game/zone_route.h"
#include "core/log.h"

namespace {

// A zone's middle, in world metres — where buildZoneLevel seeds its own flow field and stands the
// named boss. Derived from the grid edge rather than read back off the level, so it is answerable
// before anything has spawned.
inline Vec3 zoneCentre(const Zone::ZoneDef& def) {
    const f32 half = static_cast<f32>(def.gridSize) * 0.5f;
    return Vec3{half, 0.0f, half};
}

// How far the goal must move before the flow field is rebuilt. A BFS over a 52x52 grid is cheap but
// not free, and a goal that tracks a walking enemy would otherwise rebuild it every single tick.
// 3 m is under the bot's own engagement band, so the route never goes meaningfully stale.
constexpr f32 GOAL_REBUILD_DIST = 3.0f;

} // namespace


// Where the way onward is, whichever world this is.
//
// The driver's rescue machinery — the exit-progress watchdog, the committed bull, the A* first leg —
// was all written against `m_level.floorDoorPos`, which in an act is not set at all (a zone has no
// floor door). Left alone, every one of those remedies would aim at a stale dungeon coordinate, so a
// bot pinned by the acts' tier-5 density would never be shoved out of the fight — which is exactly
// what the first live run did: position frozen for twenty seconds while its health swung between a
// fifth and full.
//
// One accessor rather than a conditional at each of the five call sites, because the failure mode of
// missing one is silent: the remedy still fires, it just walks somewhere meaningless.
Vec3 Engine::autoplayGoalPos() const {
    if (m_level.inZone && ap().zoneGoalValid) return ap().zoneGoal;
    return m_level.floorDoorPos;
}

// Where the act wants the bot to go, right now.
//
// Returns false when there is nothing to head for — the acts are finished, or this is a world the
// route does not describe — which is what ends the run.
bool Engine::zoneBotGoal(Vec3& outGoal, bool& outNeedsInteract) {
    const Zone::ZoneDef* def = Zone::find(m_level.zoneFloor);
    if (!def) return false;

    const u64 mask = m_questMask[m_localPlayerIndex];
    outNeedsInteract = false;

    // 1. Is there unfinished business HERE? A quest zone is not left until its quest is done — which
    //    is also what the gate would enforce, but a bot that walks to a door it cannot open reads as
    //    stuck and burns the stall watchdogs for nothing.
    switch (ZoneRoute::taskFor(m_level.zoneFloor, mask)) {
        case ZoneRoute::Task::SLAY: {
            // Head for the named boss. It is spawned at the zone centre and does not roam far, but
            // track the live entity when we can find it — a boss that has walked out of its pad is
            // still the objective.
            const Entity* boss = nullptr;
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                const Entity& e = m_entities.entities[m_entities.activeList[a]];
                if ((e.flags & ENT_DEAD) || (e.flags & ENT_FRIENDLY)) continue;
                if (e.isBoss) { boss = &e; break; }
            }
            outGoal = boss ? boss->position : zoneCentre(*def);
            return true;
        }
        case ZoneRoute::Task::CLEAR_ZONE: {
            // Hunt. The FIGHT branch handles anything in reach; this is for the stragglers that a
            // "clear the zone" objective otherwise leaves scattered in corners, which is the whole
            // difference between finishing the quest and circling forever.
            // Same skip set the target scan and CombatQuery use, so "is this a hostile" cannot
            // drift between the thing that fights them and the thing that counts them.
            const Entity* nearest = nullptr;
            f32 bestD2 = 1e18f;
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                const Entity& e = m_entities.entities[m_entities.activeList[a]];
                if (e.flags & ENT_DEAD)     continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (e.enemyType == EnemyType::PROP) continue;
                const f32 d2 = lengthSq(e.position - m_localPlayer.position);
                if (d2 < bestD2) { bestD2 = d2; nearest = &e; }
            }
            if (nearest) { outGoal = nearest->position; return true; }
            // Nothing left alive: the CLEAR poll completes the quest within a tick, so just hold at
            // the centre rather than reporting "no goal" and ending the run a frame early.
            outGoal = zoneCentre(*def);
            return true;
        }
        default: break;   // TRAVEL — fall through to the route
    }

    // 2. Nothing to do here: take the next hop toward the act's objective.
    const u8 goalZone = ZoneRoute::objectiveZone(mask);
    if (goalZone == 0) return false;                       // both acts done — the run is over
    const ZoneRoute::Hop hop = ZoneRoute::nextHop(m_level.zoneFloor, goalZone, mask);
    if (hop.kind == ZoneRoute::HopKind::NONE) return false;

    // AN EDGE IS WALKED INTO; ONLY A PORTAL IS PRESSED. This distinction is the whole difference
    // between a bot that travels and one that stands at a border holding a button — which is what
    // the first act soak measured: five classes frozen 1.9 m from an OPEN gate for ten minutes,
    // `rem=descend mv=0`, because the goal was flagged interactable so the brain stopped at the
    // descend radius and pressed. A border has nothing to press, and the crossing fires on
    // proximity, so the bot must keep walking until it does.
    if (hop.kind == ZoneRoute::HopKind::EDGE) {
        outGoal = zoneEdgeCrossPos(hop.dir);   // INSIDE the trigger band, not the arrival inset
        return true;
    }
    outNeedsInteract = true;

    // A PORTAL hop: aim at the fixture itself. Its destination rides in the item's itemLevel byte,
    // which is what makes one object serve both directions — so match on that rather than on
    // position, or an interior with its way home nearby would be ambiguous.
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = m_worldItems.items[i];
        if (!wi.active || !isZoneGate(wi.item)) continue;
        if (static_cast<u8>(wi.item.itemLevel) != hop.dest) continue;
        outGoal = wi.position;
        return true;
    }
    // The fixture has not spawned (or was lost): fall back to the zone's own return point, which is
    // where an interior's gate stands. Better to walk somewhere plausible than to stall.
    outGoal = zoneReturnPos(*def);
    return true;
}

// Fill the parts of the BotView that only an act can answer. Called from buildBotView.
//
// Everything else in the view — self, weapon, targets, globes — is world-agnostic and already
// correct, which is the reason this is a handful of lines rather than a second buildBotView.
void Engine::zoneFillBotView(Autoplay::BotView& v) {
    v.onNormalFloor = true;                 // an act IS a world the brain can express, now

    Vec3 goal{}; bool needsInteract = false;
    const bool haveGoal = zoneBotGoal(goal, needsInteract);
    if (!haveGoal) { v.onNormalFloor = false; return; }   // acts finished; the driver ends the run

    // Rebuild the field only when the goal has actually moved. buildFlowField is a full BFS.
    if (!ap().zoneGoalValid || lengthSq(goal - ap().zoneGoal) > GOAL_REBUILD_DIST * GOAL_REBUILD_DIST) {
        LevelGridSystem::buildFlowField(m_level.grid, goal);
        ap().zoneGoal      = goal;
        ap().zoneGoalValid = true;
    }

    // 1 Hz act telemetry. A soak of the acts needs to answer "what is it trying to do and is it
    // getting there" without a rebuild — the same reason [TELEM-HB] and [STALL] exist for the
    // dungeon. Cheap: one line a second, only while a bot is in a zone.
    ap().zoneTelemT += 1.0f / 60.0f;
    if (ap().zoneTelemT >= 1.0f) {
        ap().zoneTelemT = 0.0f;
        const u64 mask = m_questMask[m_localPlayerIndex];
        LOG_INFO("[ZBOT] zone=%u task=%u obj=%u goal=(%.1f,%.1f) d=%.1f hop=%d p=(%.1f,%.1f) hp=%.0f",
                 static_cast<u32>(m_level.zoneFloor),
                 static_cast<u32>(ZoneRoute::taskFor(m_level.zoneFloor, mask)),
                 static_cast<u32>(ZoneRoute::objectiveZone(mask)),
                 static_cast<f64>(goal.x), static_cast<f64>(goal.z),
                 static_cast<f64>(length(goal - m_localPlayer.position)),
                 static_cast<int>(needsInteract),
                 static_cast<f64>(m_localPlayer.position.x),
                 static_cast<f64>(m_localPlayer.position.z),
                 static_cast<f64>(m_localPlayer.health));
    }

    // The "door" is the next hop. During a quest there is no door at all, which is what keeps the bot
    // in the zone doing the work instead of drifting toward the exit it is not allowed through yet.
    v.doorActive  = needsInteract;
    v.distToDoor  = length(goal - m_localPlayer.position);

    // A SLAY quest is a boss floor in every way that matters to the bot: the way onward is sealed
    // until it dies, so reusing the dungeon's boss handling gives goal substitution toward the boss,
    // healer-first targeting and the closing commit for free.
    const bool slaying = ZoneRoute::taskFor(m_level.zoneFloor, m_questMask[m_localPlayerIndex])
                       == ZoneRoute::Task::SLAY;
    v.hasBoss   = slaying;
    v.bossAlive = slaying;   // taskFor stops saying SLAY the moment the quest completes
}

// The driver's per-tick zone step. Returns true when the run has ENDED (the acts are complete).
//
// The only thing the dungeon driver does that an act does not want is the descend pulse's assumption
// that interact means "use the floor door"; here the same pulse reaches a zone gate fixture through
// the identical arbitration (updatePlayerPickup), so nothing special is needed. What IS needed is an
// ending: a bot that finishes Act 2 and keeps walking is the credits-park failure in a new costume.
bool Engine::zoneAutoplayStep() {
    // EVALUATED, never read off a flag. The first version consulted a lane flag that buildBotView
    // writes LATER in the same tick, so on the very first tick in a zone it read the default `false`
    // and ended every run the instant it arrived. A cheap recompute is worth more than a cached bool
    // whose correctness depends on call order inside a function that is itself ordered for other
    // reasons.
    Vec3 goal{}; bool needsInteract = false;
    if (zoneBotGoal(goal, needsInteract)) return false;

    if (ZoneRoute::objectiveZone(m_questMask[m_localPlayerIndex]) == 0) {
        LOG_INFO("[AUTOPLAY] ACTS COMPLETE — both acts finished in zone %u; ending the run",
                 static_cast<u32>(m_level.zoneFloor));
    } else {
        // No goal but quests outstanding means the route could not find a way on — a gate closed
        // against us, or a table edit that broke a link. Say which, loudly: this is precisely the
        // "unfinishable act" the route tests exist to prevent, and in a soak it must not look like an
        // ordinary quiet ending.
        LOG_WARN("[AUTOPLAY] STRANDED in zone %u — objective %u is unreachable; ending the run",
                 static_cast<u32>(m_level.zoneFloor),
                 static_cast<u32>(ZoneRoute::objectiveZone(m_questMask[m_localPlayerIndex])));
    }
    return true;
}

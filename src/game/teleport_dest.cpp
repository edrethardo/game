// teleport_dest.cpp — shared landing-spot resolver for player teleports (see header).
// The march direction is the DESIGN: walking back from the ideal point toward the caster
// degrades gracefully (a blocked backstab spot becomes a melee-range landing on the caster's
// side of the target) and can only ever shorten the movement — never redirect it sideways
// into geometry the caster couldn't see.

#include "game/teleport_dest.h"
#include "game/entity.h"
#include "world/level_grid.h"
#include "world/raycast.h"
#include "world/collision.h"   // PLAYER_HALF_WIDTH

namespace {

// True if the player's XZ footprint (feet at `pos`) fits in open cells: center + 4 corners.
// A single cell-center test — what Shadow Step used — passes with half the body inside the
// neighboring wall whenever the point sits near a cell edge.
bool footprintClear(const LevelGrid& grid, Vec3 pos) {
    static constexpr f32 HW = PLAYER_HALF_WIDTH;
    const f32 offs[5][2] = {{0, 0}, {HW, HW}, {HW, -HW}, {-HW, HW}, {-HW, -HW}};
    for (const auto& o : offs) {
        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(grid, {pos.x + o[0], pos.y, pos.z + o[1]}, gx, gz))
            return false;                                  // off the map is never a landing spot
        if (LevelGridSystem::isSolid(grid, gx, gz)) return false;
    }
    return true;
}

// True if `pos` is inside any living entity's body (+ a finger of air). UNTARGETABLE
// entities (swarm drones, effects, cosmetic pets) are never solid to anyone.
bool overlapsAnyBody(EntityPool& entities, Vec3 pos) {
    for (u32 a = 0; a < entities.activeCount; a++) {
        const Entity& e = entities.entities[entities.activeList[a]];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_UNTARGETABLE) continue;
        if (e.flags & ENT_BURROWED) continue;   // underground — a valid landing spot above it
        const f32 r = (e.halfExtents.x > e.halfExtents.z ? e.halfExtents.x : e.halfExtents.z)
                    + PLAYER_HALF_WIDTH + 0.05f;
        const Vec3 d = pos - e.position;
        if (d.x * d.x + d.z * d.z < r * r) return true;
    }
    return false;
}

} // namespace

Vec3 Teleport::resolveDest(const LevelGrid& grid, EntityPool& entities, Vec3 start, Vec3 desired)
{
    Vec3 delta = desired - start;
    delta.y = 0.0f;
    const f32  dist = length(delta);
    const Vec3 dir  = (dist > 0.001f) ? delta * (1.0f / dist) : Vec3{0, 0, 0};

    for (f32 back = 0.0f; back <= dist + 0.001f; back += 0.25f) {
        Vec3 p = desired - dir * back;
        if (!footprintClear(grid, p)) continue;
        if (overlapsAnyBody(entities, p)) continue;

        // Sealed-pocket guard: the landing spot must be VISIBLE from the caster. Without
        // this, a candidate past a thin wall (rejected samples in the wall, then open floor
        // beyond it) would teleport the player through geometry into a room the blink was
        // never meant to reach. Skipped when p has collapsed back to ~start.
        Vec3 toP = p - start;
        toP.y = 0.0f;
        const f32 dp = length(toP);
        if (dp > 0.25f) {
            const Vec3 eye = start + Vec3{0, 0.5f, 0};
            const RayHit hit = Raycast::cast(grid, eye, toP * (1.0f / dp), dp);
            if (hit.hit && hit.distance < dp - 0.1f) continue;
        }

        // Land on the destination cell's floor so the player neither floats nor clips in.
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(grid, p, gx, gz))
            p.y = LevelGridSystem::getFloorHeight(grid, gx, gz);
        return p;
    }
    return start;   // the entire line is blocked — refuse the movement rather than guess
}

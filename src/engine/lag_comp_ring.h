#pragma once
// lag_comp_ring.h — Standalone ring buffer for entity pose history used by
// lag compensation. Extracted from engine_combat.cpp so the math can be unit-
// tested independently of the full Engine object graph. engine_combat.cpp
// continues to own its own static ring array; this module is the tested
// reference implementation that future callers (AOE in M9, etc.) can use.
//
// Ownership: engine_combat.cpp drives push() once per server tick per live
// entity. beginLagComp / endLagComp are higher-level callers of findByTickStamp
// / atTicksAgo.

#include "core/types.h"
#include "core/math.h"

struct LagCompPose {
    Vec3 position    = {0,0,0};
    f32  yaw         = 0.0f;
    Vec3 halfExtents = {0,0,0};
    u32  tickStamp   = 0;   // 0 = empty
};

static constexpr u32 LAG_COMP_HISTORY_TICKS_MAX = 16;

// Single-entity ring of recent poses. Owners (engine_combat.cpp) maintain one per
// entity slot. tickStamp == 0 means the slot is empty.
struct LagCompRing {
    LagCompPose poses[LAG_COMP_HISTORY_TICKS_MAX] = {};
    u8          head = 0;
};

namespace LagCompRingOps {
    void reset(LagCompRing& r);
    void push(LagCompRing& r, const LagCompPose& pose);
    // Returns pointer to the entry whose tickStamp matches (or null). For approximate
    // rewind, callers can scan with their own logic — keep this exact-match for now.
    const LagCompPose* findByTickStamp(const LagCompRing& r, u32 tickStamp);
    // Returns the entry `ticksAgo` positions before head (1 = newest, etc.), or null
    // if not filled.
    const LagCompPose* atTicksAgo(const LagCompRing& r, u32 ticksAgo);
}

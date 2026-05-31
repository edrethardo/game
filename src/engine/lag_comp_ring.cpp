// lag_comp_ring.cpp — Implementation of the standalone entity pose history ring.
// See lag_comp_ring.h for ownership notes and usage context.

#include "engine/lag_comp_ring.h"

void LagCompRingOps::reset(LagCompRing& r) {
    for (u32 i = 0; i < LAG_COMP_HISTORY_TICKS_MAX; i++) r.poses[i] = {};
    r.head = 0;
}

void LagCompRingOps::push(LagCompRing& r, const LagCompPose& pose) {
    // Overwrites the oldest slot — head always points to the next write position,
    // so after push the most-recent entry is at (head - 1) mod CAPACITY.
    r.poses[r.head] = pose;
    r.head = (r.head + 1) % LAG_COMP_HISTORY_TICKS_MAX;
}

const LagCompPose* LagCompRingOps::findByTickStamp(const LagCompRing& r, u32 tickStamp) {
    // tickStamp 0 is the sentinel for "empty" — never match it.
    if (tickStamp == 0) return nullptr;
    for (u32 i = 0; i < LAG_COMP_HISTORY_TICKS_MAX; i++) {
        if (r.poses[i].tickStamp == tickStamp) return &r.poses[i];
    }
    return nullptr;
}

const LagCompPose* LagCompRingOps::atTicksAgo(const LagCompRing& r, u32 ticksAgo) {
    // ticksAgo=1 is the most-recently pushed entry; 0 is invalid (undefined),
    // and > CAPACITY means the data has been evicted.
    if (ticksAgo == 0 || ticksAgo > LAG_COMP_HISTORY_TICKS_MAX) return nullptr;
    // head points to the *next* write slot, so (head - 1) mod N is newest.
    u32 idx = (r.head + LAG_COMP_HISTORY_TICKS_MAX - ticksAgo) % LAG_COMP_HISTORY_TICKS_MAX;
    if (r.poses[idx].tickStamp == 0) return nullptr;
    return &r.poses[idx];
}

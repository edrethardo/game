// src/net/prediction_ring.cpp
//
// Implementation of PredictionRingOps — the four operations (reset, push, find,
// collectInputsAfter) on the client-side prediction ring buffer.  See
// prediction_ring.h for design rationale.

#include "net/prediction_ring.h"

// Zero all entries and reset cursors.
void PredictionRingOps::reset(PredictionRing& r) {
    for (u32 i = 0; i < PREDICTION_RING_CAPACITY; i++) r.entries[i] = {};
    r.head  = 0;
    r.count = 0;
}

// Write entry at head, advance head mod capacity.  When full (count==capacity) the
// head slot already holds the oldest entry — we overwrite it, naturally evicting it.
void PredictionRingOps::push(PredictionRing& r, u32 clientTick, const NetInput& in, const PredictedState& s) {
    PredictionEntry& slot = r.entries[r.head];
    slot.occupied   = true;
    slot.clientTick = clientTick;
    slot.input      = in;
    slot.state      = s;
    r.head = (r.head + 1) % PREDICTION_RING_CAPACITY;
    if (r.count < PREDICTION_RING_CAPACITY) r.count++;
}

// Linear scan over the live entries.  Entries are stored contiguously from index 0
// up to count (the ring hasn't wrapped yet) or scattered once full — but we track
// count, so the first `count` slots in entries[] are always valid regardless of where
// head points after wrap.  This is correct because push always overwrites from index 0
// onwards and only starts overwriting old entries once the ring is full.
const PredictionEntry* PredictionRingOps::find(const PredictionRing& r, u32 clientTick) {
    for (u32 i = 0; i < r.count; i++) {
        const PredictionEntry& e = r.entries[i];
        if (e.occupied && e.clientTick == clientTick) return &e;
    }
    return nullptr;
}

// Two-pass: collect matching indices, insertion-sort by clientTick, copy inputs.
// n is bounded by PREDICTION_RING_CAPACITY (256) so insertion sort is fine.
u32 PredictionRingOps::collectInputsAfter(const PredictionRing& r, u32 afterTick, NetInput* out, u32 outCap) {
    u32 idxs[PREDICTION_RING_CAPACITY];
    u32 n = 0;
    for (u32 i = 0; i < r.count && n < PREDICTION_RING_CAPACITY; i++) {
        const PredictionEntry& e = r.entries[i];
        if (e.occupied && e.clientTick > afterTick) idxs[n++] = i;
    }
    // Insertion sort ascending by clientTick so replay happens oldest→newest.
    for (u32 i = 1; i < n; i++) {
        u32 key = idxs[i];
        u32 j   = i;
        while (j > 0 && r.entries[idxs[j-1]].clientTick > r.entries[key].clientTick) {
            idxs[j] = idxs[j-1];
            j--;
        }
        idxs[j] = key;
    }
    u32 written = 0;
    for (u32 i = 0; i < n && written < outCap; i++) {
        out[written++] = r.entries[idxs[i]].input;
    }
    return written;
}

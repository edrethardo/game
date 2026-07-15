#pragma once
// src/net/prediction_ring.h
//
// PredictionRing — client-side circular buffer for client-side prediction (M3).
//
// Each tick the client captures its input + resulting player state and pushes
// them into this ring keyed by clientTick. On snapshot arrival the reconciliation
// path (engine_net.cpp clientNetPost) looks up the ring entry at
// snap.lastProcessedInputTick[mySlot], compares against the server's authoritative
// state, and if they diverged replays all inputs newer than the ACK to re-derive the
// current tick's state. Capacity 256 covers ~4 s of divergence at 60 Hz.

#include "core/types.h"
#include "core/math.h"
#include "net/net_player.h"

static constexpr u32 PREDICTION_RING_CAPACITY = 256; // ~4 s at 60 Hz

// The subset of player state captured per tick for prediction comparison.
// Mirrors the fields the server encodes into SnapPlayer that we care about.
struct PredictedState {
    Vec3 position    = {0,0,0};
    Vec3 velocity    = {0,0,0};
    f32  yaw         = 0.0f;
    f32  pitch       = 0.0f;
    f32  health      = 100.0f;
    f32  invulnTimer = 0.0f;
    bool onGround    = false;
};

// One slot in the ring: the input that drove this tick and the resulting state.
struct PredictionEntry {
    bool           occupied   = false;
    u32            clientTick = 0;
    NetInput       input      = {};
    PredictedState state      = {};
};

// Circular buffer: head is the next write slot (oldest entry is at head when full).
struct PredictionRing {
    PredictionEntry entries[PREDICTION_RING_CAPACITY] = {};
    u32             head  = 0;  // index of next write slot
    u32             count = 0;  // number of valid entries (≤ PREDICTION_RING_CAPACITY)
};

namespace PredictionRingOps {
    // Zero all entries and reset head/count. Call on connect/floor-transition.
    void reset(PredictionRing& r);

    // Write a new (clientTick, input, state) entry, evicting the oldest if full.
    void push(PredictionRing& r, u32 clientTick, const NetInput& in, const PredictedState& s);

    // Return a pointer to the entry with matching clientTick, or nullptr if not found.
    // Search is linear over count entries — fine for PREDICTION_RING_CAPACITY=256.
    const PredictionEntry* find(const PredictionRing& r, u32 clientTick);

    // Mutable twin of find(). The replay path uses it to overwrite each replayed tick's
    // predicted state with the corrected one, so the NEXT ack compares the server against
    // the corrected history — without this, one real mispredict would re-fire a "divergence"
    // on every subsequent ack until the bad entries aged out of the ring.
    PredictionEntry* findMut(PredictionRing& r, u32 clientTick);

    // Collect inputs whose clientTick > afterTick into out[], sorted ascending by tick.
    // Returns number written (capped at outCap). Used by the replay path to re-apply
    // all inputs newer than the last server-acknowledged tick.
    u32 collectInputsAfter(const PredictionRing& r, u32 afterTick, NetInput* out, u32 outCap);
}

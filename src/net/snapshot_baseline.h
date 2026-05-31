#pragma once
#include "core/types.h"

// Per-client baseline tracker. The server keeps one per connected client; it stores
// the serverTick of the last snapshot the client has confirmed receipt of. When the
// server next encodes a snapshot for this client, it compares the current state to
// this baseline and emits per-slot "unchanged since baseline" hints.
//
// For v1 the baseline is just a tick number (not a full snapshot copy). The encoder
// queries the server's current state and the *predecessor* snapshot the server held
// for the matching tick. Future versions can store a full WorldSnapshot copy per
// client for richer delta encoding.
struct BaselineTracker {
    u32 baselineTick = 0;   // 0 = no baseline yet
};

namespace BaselineTrackerOps {
    void reset(BaselineTracker& t);
    void store(BaselineTracker& t, u32 serverTick);
    // Returns true if the client's reported ackedTick doesn't match our baseline,
    // meaning we should send a full snapshot (not a delta) on the next tick.
    bool shouldSendFullSnapshot(const BaselineTracker& t, u32 clientAckedTick);
}

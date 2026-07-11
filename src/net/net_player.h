#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/weapon.h"
#include "game/item.h"
#include "net/net.h"

// Input as received from a client (or captured locally for listen server host).
//
// Aim is sent as ABSOLUTE quantized values rather than deltas. Deltas were lossy
// under UDP loss: a dropped packet permanently dropped its mouse delta and the
// server's yaw drifted behind the client's live camera. Absolutes are idempotent —
// a dropped packet only delays the next sync; nothing is lost. Same byte count.
//
// Position is server-authoritative (M2+) — the server runs PlayerController on each
// remote and computes np.position from moveFlags + yaw. M3 will add client-side
// prediction + replay reconciliation on top.
struct NetInput {
    u32 clientTick;       // monotonic client-local sim tick (M1) — server uses for input
                          // ring buffer ordering and lastProcessedInputTick echo (M2).
    u16 ackedSnapshotTick;// low 16 bits of the latest snapshot.serverTick the client has
                          // applied. Server uses this in M11 (delta compression) to choose
                          // a baseline. Unused until M11 — write zero meanwhile.
    u8  moveFlags;        // bit0=W, bit1=S, bit2=A, bit3=D, bit4=jump, bit5=fire, bit6=lockHold
    u8  weaponId;         // currently selected weapon
    u16 yawQ;             // absolute yaw,   packed via Quantize::packAngle over [-π, π]
    u16 pitchQ;           // absolute pitch, packed via Quantize::packAngle over [-π, π]
    u8  extFlags;         // extended input flags (potion, reload, skill, etc.)
    u8  skillSlot;        // which class skill slot (0-3) to activate
    u8  interpDelayMs;    // the interpolation delay the client ACTUALLY used when it captured
                          // this input (33–150 ms, adaptive with jitter). The server rewinds
                          // enemies by exactly this much when it replays the input, so both
                          // sides collide against the same world. 0 = not stamped -> server
                          // falls back to the baseline. See net/lag_comp.h for why guessing
                          // this value server-side could never work.
};

// Move flag bits
static constexpr u8 INPUT_FORWARD  = 1 << 0;
static constexpr u8 INPUT_BACKWARD = 1 << 1;
static constexpr u8 INPUT_RIGHT    = 1 << 2;
static constexpr u8 INPUT_LEFT     = 1 << 3;
static constexpr u8 INPUT_JUMP     = 1 << 4;
static constexpr u8 INPUT_FIRE     = 1 << 5;
static constexpr u8 INPUT_LOCK     = 1 << 6;

// Extended input (sent in a second byte)
static constexpr u8 INPUT_EX_POTION     = 1 << 0;
static constexpr u8 INPUT_EX_RELOAD     = 1 << 1;
static constexpr u8 INPUT_EX_SKILL      = 1 << 2;  // right-click class skill
static constexpr u8 INPUT_EX_BOOT_SKILL = 1 << 3;
static constexpr u8 INPUT_EX_HELM_SKILL = 1 << 4;
static constexpr u8 INPUT_EX_INVENTORY  = 1 << 5;  // Tab toggle
// bit 6 free — respawn now uses the reliable CL_RESPAWN packet, not an input flag
static constexpr u8 INPUT_EX_DODGE     = 1 << 7;  // Wanderer dodge roll

// Networked player state — the authoritative state the server maintains.
struct NetPlayer {
    bool active         = false;
    u8   slotIndex      = 0xFF;

    // Transform (server-authoritative)
    Vec3 position       = {0,0,0};
    Vec3 velocity       = {0,0,0};
    f32  yaw            = 0.0f;
    f32  pitch          = 0.0f;
    bool onGround       = false;
    bool noclip         = false;

    // Combat
    f32  health         = 100.0f;
    f32  maxHealth      = 100.0f;
    f32  damageFlashTimer = 0.0f;

    // Weapon
    WeaponState weaponState;

    // Target lock — currently inert (lockActive never set true). lockActive and lockIndex
    // are NO LONGER on the wire (CV-4 removed them from SnapPlayer) but the in-memory fields
    // are kept on NetPlayer/Player so consumers compile until lock-on is brought back.
    u16  lockIndex      = 0xFFFF;
    u16  lockGeneration = 0;
    bool lockActive     = false;

    // Spawn
    Vec3 spawnPosition  = {0,0,0};

    // Network
    u32  lastProcessedInputTick = 0;

    // Status effects (server-authoritative)
    f32  invulnTimer       = 0.0f;
    f32  damageReduction   = 0.0f;
    f32  slowTimer         = 0.0f;
    f32  poisonTimer       = 0.0f;
    f32  poisonDps         = 0.0f;
    f32  burnTimer         = 0.0f;
    f32  burnDps           = 0.0f;
    f32  freezeTimer       = 0.0f;
    // Server-side Wanderer dodge-roll movement state (mirrors the client's DodgeState roll
    // fields). Previously the server only granted i-frames and let the client predict the
    // 4 m roll alone, so a guest's dodge diverged ~4 m from the server and rubber-banded.
    // The server now replays the same velocity burst from these. Not serialized as fields —
    // SnapPlayer.dodgeFlags bit0 carries `rollTimer > 0` so remotes animate the roll.
    Vec3 rollDirection     = {0,0,0}; // normalized XZ roll direction, captured at dodge-start
    f32  rollTimer         = 0.0f;    // countdown during the 0.5 s roll
    f32  rollCooldownTimer = 0.0f;    // 1.0 s lockout after a roll (mirrors client cooldown)
    bool blocking          = false;
    f32  blockTimer        = 0.0f;
    bool isDead            = false;
    f32  potionCooldown    = 0.0f;
    // R17: tick at which this player last activated a potion. Authoritative for the
    // gate on server-side remote-player processing. 0 = never (gate passes). On
    // server: set to input->clientTick on successful gate. potionCooldown above is
    // HUD-derived.
    u32  potionLastActivationTick = 0;
    // Near-death lifesaver i-frame (TA-1): server-side mirror of Player.lifesaverArmed so the
    // one-shot below-20%-HP invuln stays one-shot across frames for remotes (consumed on use,
    // re-earned only at >=40% HP). NOT serialized — server-only state, never on the wire.
    bool lifesaverArmed    = true;
    // Server-only mirror of Player.graceInvuln: tags this slot's invulnTimer as near-death-grace-
    // sourced so the "clear grace once healthy (>85% HP)" rule in serverNetPost can cancel it
    // without touching dodge i-frames (which also set invulnTimer=0.3). NOT on the wire.
    bool graceInvuln       = false;
    // Ring-passive state (M8/M9 follow-up batch): server-only mirror of the Player fields so a
    // remote with Soul Harvest / Second Wind / Phase Strike actually accumulates stacks, gets
    // the emergency heal, and runs stealth — instead of all of it landing on the host. Not on
    // the wire; the host's gameUpdate path keeps using the Player swap aliases.
    f32  soulHarvestTimer  = 0.0f;
    u8   soulHarvestStacks = 0;
    f32  secondWindCooldown = 0.0f;
    // Blood Nova ARMOR aura (Demonhide Cuirass) for a REMOTE. `lastDamageTaken` is written back
    // from the throwaway Player view by writeBackRemoteView — it is the ONLY way the server learns
    // a guest got hit (SV_DAMAGE_TO_ME only covers projectile hits, and it flows the other way).
    // Consumed and cleared each tick in serverNetPost. Not on the wire.
    f32  lastDamageTaken    = 0.0f;
    f32  bloodNovaCooldown  = 0.0f;
    // Frenzy glove passive (mirrors Player::{frenzyStacks,frenzyTimer}; not on the wire) —
    // server-side so a remote's on-hit attack-speed stacks speed up the REMOTE's fire path.
    u8   frenzyStacks      = 0;
    f32  frenzyTimer       = 0.0f;
    f32  smokeTimer        = 0.0f;  // Phase Strike post-kill stealth (mirrors Player::smokeTimer)
    // Wanderer mark-prey state (death-preamble follow-up batch): server-only mirror so a remote
    // Wanderer's Shadow Dance / mark-spread / speed stacks credit the *remote*, not the host
    // (m_localPlayer swap alias). Mirrors Player::{shadowDanceTimer,markTimer,markSpeedStacks,
    // markSpeedTimers}; not on the wire.
    f32  shadowDanceTimer  = 0.0f;
    f32  markTimer         = 0.0f;
    u8   markSpeedStacks   = 0;
    f32  markSpeedTimers[20] = {};
    // Per-player equipment passives (read from inventory each tick)
    SkillId weaponProc     = SkillId::NONE;
    SkillId armorAura      = SkillId::NONE;
    SkillId ringPassive    = SkillId::NONE;
    PlayerClass playerClass = PlayerClass::WARRIOR;

    // Eye height (metres above feet)
    f32  eyeHeight      = 1.7f;
    f32  moveSpeed      = 6.0f;
    f32  sensitivity    = 0.002f;

    Vec3 eyePos() const { return position + Vec3{0, eyeHeight, 0}; }
};

// Number of consecutive inputs packed into each CL_INPUT packet (oldest→newest).
// Redundancy benefit: one dropped UDP packet no longer drops an input — the next
// packet still carries it. At 8, seven consecutive losses (~120 ms at 60 Hz) are
// needed to actually lose an input, covering bursty Wi-Fi/cellular loss. Cost is
// 8×14+4 = 116 B per CL_INPUT (~7 KB/s up) — negligible. The server's per-input drain
// (collectUnprocessedInputs) processes each new clientTick once, so the extra
// redundancy never double-applies. Was 4 (~50 ms of cover).
static constexpr u32 INPUT_WINDOW_SIZE = 8;

// Serialize up to `count` NetInputs (≤ INPUT_WINDOW_SIZE) into `outBuf`. Wire layout:
//   u8  windowCount
//   u8  targetSlot  (online couch co-op: absolute net slot this input is for; 0 for a single client,
//                    which the server overrides with the peer's own slot)
//   u16 reserved (=0)
//   N × (u32 clientTick + u16 ackedSnapshotTick + u8 moveFlags + u8 weaponId
//        + u16 yawQ + u16 pitchQ + u8 extFlags + u8 skillSlot)  // 14 B per input
// Returns total bytes written (0 on overflow).
u32 serializeInputWindow(u8* outBuf, u32 outCap, const NetInput* inputs, u32 count, u8 targetSlot = 0);

// Inverse of serializeInputWindow. Writes up to `maxCount` inputs into `outInputs`.
// Returns the number actually decoded (0 if the buffer is truncated or malformed).
u32 deserializeInputWindow(const u8* buf, u32 size, NetInput* outInputs, u32 maxCount);

// Ring buffer for storing recent inputs (server-side per player, client-side for prediction)
static constexpr u32 INPUT_BUFFER_SIZE = 64;

struct InputRingBuffer {
    NetInput inputs[INPUT_BUFFER_SIZE];
    u32      head  = 0;
    u32      count = 0;

    void push(const NetInput& input) {
        // Client input rides an UNSEQUENCED/unreliable channel (see Server::receiveInput /
        // net.cpp), so a late or duplicate packet can arrive carrying an OLDER tick than one
        // already buffered. getLatest() returns the most-recently-PUSHED entry, so accepting
        // a stale tick here would make the server re-apply old movement / re-fire for that
        // remote player. Keep the buffer monotonic: ignore any input whose tick isn't strictly
        // newer than the newest already buffered. (Input-repeat on packet loss is preserved —
        // getLatest() simply keeps returning the last good input.) The tick counter resets to 0
        // alongside this buffer on every floor transition (Server::init in startGame), so an
        // empty buffer always accepts the next tick regardless of prior values; u32 tick wrap is
        // a non-concern at 60 Hz (matches the serverTick assumption in client.cpp).
        if (count > 0) {
            const NetInput& newest = inputs[(head + INPUT_BUFFER_SIZE - 1) % INPUT_BUFFER_SIZE];
            if (input.clientTick <= newest.clientTick) return;
        }
        inputs[head] = input;
        head = (head + 1) % INPUT_BUFFER_SIZE;
        if (count < INPUT_BUFFER_SIZE) count++;
    }

    const NetInput* getForTick(u32 tick) const {
        for (u32 i = 0; i < count; i++) {
            u32 idx = (head + INPUT_BUFFER_SIZE - 1 - i) % INPUT_BUFFER_SIZE;
            if (inputs[idx].clientTick == tick) return &inputs[idx];
        }
        return nullptr;
    }

    const NetInput* getLatest() const {
        if (count == 0) return nullptr;
        return &inputs[(head + INPUT_BUFFER_SIZE - 1) % INPUT_BUFFER_SIZE];
    }
};

// Select every UNPROCESSED input (clientTick > cursor), oldest→newest, into out[],
// advancing `cursor` to the newest one visited. Returns how many were written
// (clamped to outCap). This is the canonical "drain all new inputs once each"
// selection the server's serverNetPre loop relies on for BOTH movement and
// activation edges: an edge bit (potion/skill) is set on exactly one client tick,
// so reading only getLatest() drops it whenever a newer input arrived first (the
// common jitter case). Walking every new tick instead guarantees each press is
// seen exactly once. push() keeps the ring monotonic by clientTick, so the resend
// window collapses to one entry per tick and the cursor never revisits a tick →
// no double-fire. Pure + header-only so it can be unit-tested without the Engine.
inline u32 collectUnprocessedInputs(const InputRingBuffer& buf, u32& cursor,
                                    const NetInput** out, u32 outCap) {
    u32 n = 0;
    for (u32 k = 0; k < buf.count && n < outCap; k++) {
        // Same index math as serverNetPre's drain loop: head points one past the
        // newest write, so the oldest live entry is `count` slots back from head.
        u32 idx = (buf.head + INPUT_BUFFER_SIZE - buf.count + k) % INPUT_BUFFER_SIZE;
        const NetInput& in = buf.inputs[idx];
        if (in.clientTick <= cursor) continue; // already applied on a prior server tick
        cursor = in.clientTick;
        out[n++] = &buf.inputs[idx];
    }
    return n;
}

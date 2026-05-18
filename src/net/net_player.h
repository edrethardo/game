#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/weapon.h"
#include "game/item.h"
#include "net/net.h"

// Input as received from a client (or captured locally for listen server host)
struct NetInput {
    u32 tick;           // which server tick this input is for
    u8  moveFlags;      // bit0=W, bit1=S, bit2=A, bit3=D, bit4=jump, bit5=fire, bit6=lockHold
    u8  weaponId;       // currently selected weapon
    s16 mouseDeltaX;    // quantized mouse delta
    s16 mouseDeltaY;
    u8  extFlags;       // extended input flags (potion, reload, skill, etc.)
    u8  skillSlot;      // which class skill slot (0-3) to activate
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
static constexpr u8 INPUT_EX_RESPAWN    = 1 << 6;  // Respawn after death
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

    // Target lock (server validates)
    u16  lockIndex      = 0xFFFF;
    u16  lockGeneration = 0;
    bool lockActive     = false;
    f32  lockLosTimer   = 0.0f;

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
    bool blocking          = false;
    f32  blockTimer        = 0.0f;
    bool isDead            = false;
    f32  potionCooldown    = 0.0f;
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

// Ring buffer for storing recent inputs (server-side per player, client-side for prediction)
static constexpr u32 INPUT_BUFFER_SIZE = 64;

struct InputRingBuffer {
    NetInput inputs[INPUT_BUFFER_SIZE];
    u32      head  = 0;
    u32      count = 0;

    void push(const NetInput& input) {
        inputs[head] = input;
        head = (head + 1) % INPUT_BUFFER_SIZE;
        if (count < INPUT_BUFFER_SIZE) count++;
    }

    const NetInput* getForTick(u32 tick) const {
        for (u32 i = 0; i < count; i++) {
            u32 idx = (head + INPUT_BUFFER_SIZE - 1 - i) % INPUT_BUFFER_SIZE;
            if (inputs[idx].tick == tick) return &inputs[idx];
        }
        return nullptr;
    }

    const NetInput* getLatest() const {
        if (count == 0) return nullptr;
        return &inputs[(head + INPUT_BUFFER_SIZE - 1) % INPUT_BUFFER_SIZE];
    }
};

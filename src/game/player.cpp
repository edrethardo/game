#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game/player.h"
#include "game/game_constants.h"
#include "net/net_player.h"
#include "net/packet.h"  // Quantize::packAngle / packPos for absolute-aim NetInput
#include "platform/input.h"
#include <cmath>

static constexpr f32 MAX_PITCH = 89.0f * 3.14159265f / 180.0f;

// Shared roll-direction math (see player.h). Pure so client + server agree exactly.
Vec3 PlayerController::computeRollDirection(bool w, bool s, bool a, bool d, f32 yaw) {
    f32 cosY = cosf(yaw);
    f32 sinY = sinf(yaw);
    Vec3 flatFwd   = normalize(Vec3{-sinY, 0.0f, -cosY});
    Vec3 flatRight = normalize(cross(flatFwd, {0.0f, 1.0f, 0.0f}));
    Vec3 dir = {0, 0, 0};
    if (w) dir += flatFwd;
    if (s) dir -= flatFwd;
    if (d) dir += flatRight;
    if (a) dir -= flatRight;
    if (lengthSq(dir) < 0.001f) dir = flatFwd; // no WASD held → roll straight forward
    return normalize(dir);
}

// ---------------------------------------------------------------------------
// Shared movement logic (operates on raw values, shared by Player & NetPlayer)
// ---------------------------------------------------------------------------
static Vec3 s_lastForward = {0, 0, -1};

static void applyMovement(Vec3& position, Vec3& velocity, f32& yaw, f32& pitch,
                           bool& onGround, bool noclip,
                           f32 moveSpeed, f32 sensitivity,
                           f32 lookDX, f32 lookDY,
                           bool w, bool s, bool a, bool d, bool jump,
                           f32 dt)
{
    // Mouse/stick/gyro look — float precision, no integer quantization
    yaw   -= lookDX * sensitivity;
    pitch -= lookDY * sensitivity;
    if (pitch >  MAX_PITCH) pitch =  MAX_PITCH;
    if (pitch < -MAX_PITCH) pitch = -MAX_PITCH;

    f32 cosP = cosf(pitch);
    f32 sinP = sinf(pitch);
    f32 cosY = cosf(yaw);
    f32 sinY = sinf(yaw);

    Vec3 forward = normalize(Vec3{-sinY * cosP, sinP, -cosY * cosP});
    s_lastForward = forward; // cache for retrieval

    Vec3 flatForward = normalize(Vec3{-sinY, 0.0f, -cosY});
    Vec3 right       = normalize(cross(flatForward, {0.0f, 1.0f, 0.0f}));

    Vec3 move = {0, 0, 0};
    if (w) move += flatForward;
    if (s) move -= flatForward;
    if (d) move += right;
    if (a) move -= right;

    if (noclip) {
        if (w) move += forward;
        if (s) move -= forward;
        move = (lengthSq(move) > 0.0001f) ? normalize(move) : Vec3{0,0,0};
        position += move * (moveSpeed * dt);
        velocity = {0, 0, 0};
        return;
    }

    Vec3 horzMove = (lengthSq(move) > 0.0001f) ? normalize(move) * moveSpeed : Vec3{0,0,0};
    velocity.x = horzMove.x;
    velocity.z = horzMove.z;

    if (jump && onGround) {
        velocity.y = 8.0f;
        onGround   = false;
    }

    if (!onGround) {
        velocity.y -= 20.0f * dt;
    }
}

// ---------------------------------------------------------------------------
// Original: reads Input:: directly
// ---------------------------------------------------------------------------
void PlayerController::update(Player& player, f32 dt) {
    // Accumulate look deltas as float to preserve sub-pixel gyro/stick precision.
    // Keyboard+mouse belong to player 0 only (P2+ are controller-only in split-screen).
    // getMouseDelta() returns the global, un-consumed SDL delta, so without this gate the
    // same mouse motion would be applied to every local player in the per-player update
    // loop — spinning P2's camera whenever P1 moves the mouse.
    s32 rawMx = 0, rawMy = 0;
    if (Input::getActivePlayer() == 0) Input::getMouseDelta(rawMx, rawMy);
    // User mouse-sensitivity multiplier — applied to the MOUSE delta only (stick/gyro added
    // below carry their own sensitivity sliders). captureLocalInput scales identically so the
    // netplay prediction (yawQ/pitchQ) matches this look update exactly.
    f32 mouseSens = Input::getMouseSensitivity();
    f32 mx = static_cast<f32>(rawMx) * mouseSens;
    f32 my = static_cast<f32>(rawMy) * mouseSens;

    // Add right stick look (controller)
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += rsX * stickScale;
        my += rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f);
    }

    // Add gyro aiming — angular velocity mapped to look delta
    f32 gyroDx, gyroDy;
    Input::getGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += gyroDx * Input::getGyroSensitivity();
        my += gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f);
    }

    // Apply slow debuff (e.g., from boss cleaver hit)
    f32 effectiveSpeed = player.moveSpeed * GameConst::SPEED_MULT;
    // Soul Harvest speed bonus: +5% per stack
    if (player.soulHarvestStacks > 0 && player.soulHarvestTimer > 0.0f) {
        effectiveSpeed *= (1.0f + player.soulHarvestStacks * 0.05f);
    }
    // Wanderer adrenaline move speed bonus: +5% per stack (requires adrenaline upgrade at floor 30)
    if (player.adrenalineUpgraded && player.dodgeState.counterStacks > 0) {
        effectiveSpeed *= (1.0f + player.dodgeState.counterStacks * 0.05f);
    }
    // Wanderer Deflect burst speed buff: +8% for 3s after releasing absorbed damage
    if (player.deflectSpeedTimer > 0.0f) {
        effectiveSpeed *= 1.08f;
    }
    // Wanderer Exploit Weakness speed buff: +5% per stack from marked enemy interactions
    if (player.markSpeedStacks > 0) {
        effectiveSpeed *= (1.0f + player.markSpeedStacks * 0.05f);
    }
    if (player.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow
        player.slowTimer -= dt;
    }
    // Freeze stops all movement — predicted locally to match the server
    // (updateNetPlayerFromInput zeroes speed when frozen). Without this the client kept
    // walking at full speed while the server held position → frozen players slid forward
    // then snapped back. freezeTimer decay stays server-authoritative + snapshot-driven
    // (we apply the effect but don't tick the timer here), same as slow's ownership split.
    if (player.freezeTimer > 0.0f) {
        effectiveSpeed = 0.0f;
    }

    // Merge keyboard + left stick for movement
    bool w = Input::isActionDown(GameAction::MOVE_FORWARD)  || Input::getStickY(false) < -0.3f;
    bool s = Input::isActionDown(GameAction::MOVE_BACKWARD) || Input::getStickY(false) > 0.3f;
    bool a = Input::isActionDown(GameAction::MOVE_LEFT)     || Input::getStickX(false) < -0.3f;
    bool d = Input::isActionDown(GameAction::MOVE_RIGHT)    || Input::getStickX(false) > 0.3f;

    // --- Wanderer dodge roll activation ---
    // Triggered on Shift press when not already rolling and cooldown has expired.
    if (Input::isActionPressed(GameAction::DODGE) && !player.dodgeState.rolling
        && player.dodgeState.cooldownTimer <= 0.0f) {
        DodgeState& ds = player.dodgeState;
        ds.rolling = true;
        ds.rollTimer = 0.5f;
        ds.rollAngle = 0.0f;
        ds.pitchAngle = 0.0f;

        // Direction from WASD, or forward if no directional input held (shared with
        // the server's dodge replication so both compute the identical roll vector).
        f32 cosY = cosf(player.yaw);
        f32 sinY = sinf(player.yaw);
        Vec3 flatFwd   = normalize(Vec3{-sinY, 0.0f, -cosY});
        Vec3 flatRight = normalize(cross(flatFwd, {0.0f, 1.0f, 0.0f}));
        ds.rollDirection = computeRollDirection(w, s, a, d, player.yaw);

        // Camera rotation axis depends on dodge direction relative to facing:
        // Forward  = front flip (pitch), Backward = back flip (pitch),
        // Left/Right = barrel roll, Diagonal = blend of both.
        f32 fwdDot   = dot(ds.rollDirection, flatFwd);   // +1 forward, -1 back
        f32 rightDot = dot(ds.rollDirection, flatRight);  // +1 right, -1 left
        ds.pitchWeight = fabsf(fwdDot);   // how much front/back flip
        ds.rollWeight  = fabsf(rightDot);  // how much barrel roll
        // Normalize so they sum to 1 (pure direction = 1.0, diagonal ≈ 0.707 each)
        f32 totalW = ds.pitchWeight + ds.rollWeight;
        if (totalW > 0.001f) { ds.pitchWeight /= totalW; ds.rollWeight /= totalW; }
        else { ds.pitchWeight = 1.0f; ds.rollWeight = 0.0f; } // fallback: front flip
        ds.pitchSign = (fwdDot >= 0.0f) ? -1 : 1;  // forward = pitch down first, back = pitch up first
        ds.rollSign  = (rightDot >= 0.0f) ? -1 : 1; // right = CW roll, left = CCW roll

        // I-frames: invulnerability for the first 0.3s of the roll (60% of duration)
        player.invulnTimer = 0.3f;
    }

    // During roll, zero out look deltas so camera stays fixed (no mouse look)
    if (player.dodgeState.rolling) {
        mx = 0.0f;
        my = 0.0f;
    }

    applyMovement(player.position, player.velocity, player.yaw, player.pitch,
                  player.onGround, player.noclip,
                  effectiveSpeed, player.sensitivity,
                  mx, my,
                  w, s, a, d,
                  Input::isActionPressed(GameAction::JUMP),
                  dt);

    // --- Wanderer dodge roll tick ---
    // Override velocity during roll for a consistent 4m distance (8m/s × 0.5s).
    {
        DodgeState& ds = player.dodgeState;
        if (ds.rolling) {
            constexpr f32 ROLL_SPEED = 8.0f; // 4m over 0.5s
            player.velocity.x = ds.rollDirection.x * ROLL_SPEED;
            player.velocity.z = ds.rollDirection.z * ROLL_SPEED;

            ds.rollTimer -= dt;

            // Smooth 360° rotation using cubic ease-in-out (3t²-2t³).
            // Blends between barrel roll (sideways) and pitch flip (forward/back)
            // based on dodge direction.
            constexpr f32 ROLL_DURATION = 0.5f;
            constexpr f32 TWO_PI = 2.0f * 3.14159265f;
            f32 t = 1.0f - (ds.rollTimer / ROLL_DURATION); // 0→1 over duration
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            f32 smooth = t * t * (3.0f - 2.0f * t);
            ds.rollAngle  = smooth * TWO_PI * static_cast<f32>(ds.rollSign)  * ds.rollWeight;
            ds.pitchAngle = smooth * TWO_PI * static_cast<f32>(ds.pitchSign) * ds.pitchWeight;
            // 0->1->0 arc peaking at mid-roll — drives the camera head-dip + viewmodel lean
            // so the roll reads as a forward tumble (head dips toward the floor and back).
            ds.rollProg = sinf(t * 3.14159265f);

            if (ds.rollTimer <= 0.0f) {
                // Roll finished — reset angle and start cooldown
                ds.rolling = false;
                ds.rollTimer = 0.0f;
                ds.rollAngle = 0.0f;
                ds.pitchAngle = 0.0f;
                ds.rollProg = 0.0f;
                ds.cooldownTimer = 1.0f;
            }
        } else if (ds.cooldownTimer > 0.0f) {
            ds.cooldownTimer -= dt;
            if (ds.cooldownTimer < 0.0f) ds.cooldownTimer = 0.0f;
        }
    }

    player.forward = s_lastForward;
}

// ---------------------------------------------------------------------------
// Network-aware: applies a NetInput struct to a NetPlayer
// ---------------------------------------------------------------------------
void PlayerController::updateNetPlayerFromInput(NetPlayer& np, const NetInput& input, f32 dt,
                                                bool movementOnly) {
    // Apply slow debuff (same as singleplayer PlayerController::update). During a reconcile
    // REPLAY (movementOnly=true) these inputs were already applied once during live
    // prediction, so re-integrate position with the slow SPEED still in effect but do NOT
    // re-decay the timer — owned by the authoritative snapshot, and replaying it would make
    // slow wear off too fast. The dodge re-arm below is intentionally idempotent (only sets
    // invulnTimer when it's currently <=0, then a single input replay re-asserts the same
    // 0.3 s); the old comment incorrectly claimed it was skipped on replay.
    f32 effectiveSpeed = np.moveSpeed * GameConst::SPEED_MULT;
    if (np.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow (speed effect only; timer decay is owned by
        // PlayerController::update / serverNetPost to avoid the previous double-decay where
        // the client decayed slowTimer twice per frame — once here and once in update()).
    }
    // Freeze stops all movement
    if (np.freezeTimer > 0.0f) {
        effectiveSpeed = 0.0f;
    }
    // (M-3) Soul Harvest ring: +5% speed per stack while the buff window is active. Mirrors
    // PlayerController::update's host bonus at player.cpp:104-106 so a remote with stacks
    // actually moves faster — without this M8 stacks accumulated but had no kinetic effect.
    if (np.soulHarvestStacks > 0 && np.soulHarvestTimer > 0.0f) {
        effectiveSpeed *= (1.0f + np.soulHarvestStacks * 0.05f);
    }
    // Shadow Dance: +20% move speed while active (mirrors host at engine_update.cpp:614).
    if (np.shadowDanceTimer > 0.0f) {
        effectiveSpeed *= 1.2f;
    }
    // Wanderer Exploit Weakness speed stacks: +5% per stack (mirrors host at player.cpp:116).
    if (np.markSpeedStacks > 0) {
        effectiveSpeed *= (1.0f + np.markSpeedStacks * 0.05f);
    }
    // Absolute aim: SET yaw/pitch from the input's quantized fields, then run movement
    // with zero look-delta (applyMovement uses np.yaw to compute forward direction, so
    // the assignment must happen first or movement direction is one tick stale).
    // This is the key fix for "shoot where I'm not aiming" — server's NetPlayer.yaw
    // is now exactly what the client packed, immune to UDP loss of any single input.
    np.yaw   = Quantize::unpackAngle(input.yawQ);
    np.pitch = Quantize::unpackAngle(input.pitchQ);
    applyMovement(np.position, np.velocity, np.yaw, np.pitch,
                  np.onGround, np.noclip,
                  effectiveSpeed, np.sensitivity,
                  /*lookDX=*/0.0f, /*lookDY=*/0.0f,
                  (input.moveFlags & INPUT_FORWARD)  != 0,
                  (input.moveFlags & INPUT_BACKWARD) != 0,
                  (input.moveFlags & INPUT_LEFT)     != 0,
                  (input.moveFlags & INPUT_RIGHT)    != 0,
                  (input.moveFlags & INPUT_JUMP)     != 0,
                  dt);

    // Server-side Wanderer dodge replication. The server now replays the SAME 4 m roll the
    // client predicts (previously it only granted i-frames, so a guest's dodge diverged ~4 m
    // and rubber-banded). rollDirection is recomputed from THIS input's WASD + yaw — identical
    // to what the client computed at dodge-start — so no wire field is needed. Timers only
    // advance on live server processing (!movementOnly); during a reconcile replay the
    // snapshot owns timer decay, but the velocity override still runs so position re-integrates.
    constexpr f32 ROLL_SPEED = 8.0f; // 4 m over 0.5 s — matches client ROLL_SPEED
    if (!movementOnly) {
        // Start a roll on the dodge edge when not already rolling and off cooldown (mirrors
        // PlayerController::update's gate). invulnTimer = 0.3 matches the client (set
        // unconditionally there) so server and client agree on the i-frame window.
        if ((input.extFlags & INPUT_EX_DODGE) && np.rollTimer <= 0.0f && np.rollCooldownTimer <= 0.0f) {
            const bool w = (input.moveFlags & INPUT_FORWARD)  != 0;
            const bool s = (input.moveFlags & INPUT_BACKWARD) != 0;
            const bool a = (input.moveFlags & INPUT_LEFT)     != 0;
            const bool d = (input.moveFlags & INPUT_RIGHT)    != 0;
            np.rollDirection = PlayerController::computeRollDirection(w, s, a, d, np.yaw);
            np.rollTimer     = 0.5f; // matches client ROLL_DURATION
            np.invulnTimer   = 0.3f;
        }
    }
    // Override horizontal velocity for every rolling frame (BEFORE the timer decrements, as the
    // client does) so serverNetPre's per-input moveAndSlide carries the 8 m/s burst.
    if (np.rollTimer > 0.0f) {
        np.velocity.x = np.rollDirection.x * ROLL_SPEED;
        np.velocity.z = np.rollDirection.z * ROLL_SPEED;
    }
    if (!movementOnly) {
        if (np.rollTimer > 0.0f) {
            np.rollTimer -= dt;
            if (np.rollTimer <= 0.0f) { np.rollTimer = 0.0f; np.rollCooldownTimer = 1.0f; }
        } else if (np.rollCooldownTimer > 0.0f) {
            np.rollCooldownTimer -= dt;
            if (np.rollCooldownTimer < 0.0f) np.rollCooldownTimer = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Capture current Input:: into a NetInput
//
// Packs ABSOLUTE yaw/pitch/position rather than mouse deltas. The look fields are
// computed as "where the player will be after PlayerController::update applies this
// same frame's mouse delta later in the loop" — that calculation must mirror
// applyMovement's `yaw -= dx*sens; pitch -= dy*sens; pitch clamp` exactly, or the
// server-side NetPlayer drifts away from the client's local camera every tick.
//
// Position is the player's CURRENT (not predicted) position. PlayerController::update
// hasn't run yet this frame, so this is end-of-last-frame position. Acceptable: the
// server treats this as authoritative and snaps NetPlayer to it; the resulting 1-tick
// lag is invisible compared to the prior reconcile-snap rubberband.
// ---------------------------------------------------------------------------
NetInput PlayerController::captureLocalInput(const Player& player, u32 tick, u8 weaponId) {
    NetInput input = {};
    input.clientTick = tick;
    input.weaponId = weaponId;

    // Merge keyboard + left stick for movement flags
    u8 flags = 0;
    if (Input::isActionDown(GameAction::MOVE_FORWARD)  || Input::getStickY(false) < -0.3f) flags |= INPUT_FORWARD;
    if (Input::isActionDown(GameAction::MOVE_BACKWARD) || Input::getStickY(false) > 0.3f)  flags |= INPUT_BACKWARD;
    if (Input::isActionDown(GameAction::MOVE_RIGHT)    || Input::getStickX(false) > 0.3f)  flags |= INPUT_RIGHT;
    if (Input::isActionDown(GameAction::MOVE_LEFT)     || Input::getStickX(false) < -0.3f) flags |= INPUT_LEFT;
    if (Input::isActionPressed(GameAction::JUMP))    flags |= INPUT_JUMP;
    if (Input::isActionDown(GameAction::FIRE))       flags |= INPUT_FIRE;
    // INPUT_LOCK is deliberately never set: it was fed by the cut lock-on feature and is read by
    // nobody on the server. The bit stays reserved in the flags byte so the wire layout is
    // unchanged (it simply always reads 0) — see net_player.h.
    input.moveFlags = flags;

    // Peek at this frame's mouse/stick/gyro delta WITHOUT consuming it (Input::getMouseDelta
    // doesn't mutate; PlayerController::update will read the same s_mouseDX/Y later in
    // the frame and produce the identical look update).
    s32 rawMx, rawMy;
    Input::getMouseDelta(rawMx, rawMy);
    // Same mouse-sensitivity multiplier as PlayerController::update (mouse delta only) so the
    // predicted yaw/pitch packed below matches what the look update will actually produce.
    f32 mouseSens = Input::getMouseSensitivity();
    f32 mx = static_cast<f32>(rawMx) * mouseSens;
    f32 my = static_cast<f32>(rawMy) * mouseSens;
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += rsX * stickScale;
        my += rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f);
    }
    // R14: peek — captureLocalInput is the wire packer, not the gameplay
    // consumer. The matching consuming read happens in PlayerController::update
    // (player.cpp:96, called from gameUpdate) and is what actually rotates
    // m_localPlayer.yaw. Pre-R14 this site called Input::getGyro (consume)
    // and starved the gameplay consumer on CLIENT/HOST in MP — wire packet
    // got the gyro contribution but the local screen never rotated.
    f32 gyroDx, gyroDy;
    Input::peekGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += gyroDx * Input::getGyroSensitivity();
        my += gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f);
    }

    // Compute absolute yaw/pitch that PlayerController::update WILL produce this frame.
    // Formula must match applyMovement at player.cpp:25-28 exactly.
    f32 newYaw   = player.yaw   - mx * player.sensitivity;
    f32 newPitch = player.pitch - my * player.sensitivity;
    if (newPitch >  MAX_PITCH) newPitch =  MAX_PITCH;
    if (newPitch < -MAX_PITCH) newPitch = -MAX_PITCH;
    input.yawQ   = Quantize::packAngle(newYaw);
    input.pitchQ = Quantize::packAngle(newPitch);

    // Position is server-authoritative (M2+) — posXQ/Y/Z removed. The server runs
    // PlayerController::updateNetPlayerFromInput on the remote slot using moveFlags + yaw.

    // Extended input flags — unified keyboard + gamepad
    u8 ext = 0;
    if (Input::isActionPressed(GameAction::POTION))       ext |= INPUT_EX_POTION;
    if (Input::isActionPressed(GameAction::RELOAD))        ext |= INPUT_EX_RELOAD;
    if (Input::isActionPressed(GameAction::CLASS_SKILL))   ext |= INPUT_EX_SKILL;
    if (Input::isActionPressed(GameAction::BOOT_SKILL))    ext |= INPUT_EX_BOOT_SKILL;
    if (Input::isActionPressed(GameAction::HELMET_SKILL))  ext |= INPUT_EX_HELM_SKILL;
    if (Input::isActionPressed(GameAction::INVENTORY))     ext |= INPUT_EX_INVENTORY;
    if (Input::isActionPressed(GameAction::DODGE))         ext |= INPUT_EX_DODGE;
    input.extFlags = ext;
    input.skillSlot = 0; // set by engine before sending

    return input;
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
void PlayerController::applyToCamera(const Player& player, Camera& cam) {
    cam.position = player.position + Vec3{0.0f, player.eyeHeight, 0.0f};
    // Dodge head-dip: tuck the eye toward the floor at mid-roll and back up, so the roll reads
    // as a real forward tumble on top of the existing forward slide + pitch flip. Visual only —
    // player.position (collision) and aim (player.yaw/pitch) are untouched.
    constexpr f32 HEAD_DIP = 0.35f;
    cam.position.y -= player.dodgeState.rollProg * HEAD_DIP;
    cam.yaw      = player.yaw;
    cam.pitch    = player.pitch;
    cam.roll     = player.dodgeState.rollAngle;  // barrel roll component (sideways dodge)
    // Pitch flip component (forward/back dodge) — added to cam.pitch, not clamped
    cam.pitch   += player.dodgeState.pitchAngle;
    cam.forward  = normalize(Vec3{
        -sinf(player.yaw) * cosf(player.pitch),
         sinf(player.pitch),
        -cosf(player.yaw) * cosf(player.pitch)
    });
    cam.right = normalize(cross(cam.forward, {0.0f, 1.0f, 0.0f}));
}

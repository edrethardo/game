#pragma once
// imu_filter.h — Madgwick IMU orientation filter (gyro + accelerometer, no magnetometer)
// plus a still-detector gyro-bias calibrator. Used to remove gyro zero-rate bias ("aim
// drift") while preserving the existing rate-based gyro aim:
//
//   feed raw gyro (rad/s) + accel (any units) + dt  ->  read corrG{x,y,z} (bias-corrected
//   gyro, rad/s) and map it to look exactly as before.
//
// Why both pieces: an accelerometer can only correct the TILT axes (pitch/roll) via gravity;
// it cannot observe yaw, which is the usual horizontal aim creep. So the actual drift fix is
// the bias calibrator (works on all axes, gated by a gravity-steady "at rest" test), while the
// Madgwick orientation quaternion provides the gravity-stabilized reference (and is kept ready
// for a future 1:1 / player-space motion-aim upgrade). Header-only; no heap, no globals.

#include "core/types.h"
#include <cmath>

struct ImuFilter {
    // Orientation quaternion (w, x, y, z) — gravity-stabilized.
    f32 q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
    // Learned gyro zero-rate bias (rad/s), estimated while the device is at rest.
    f32 biasX = 0.0f, biasY = 0.0f, biasZ = 0.0f;
    // Bias-corrected gyro for the current frame (rad/s) — what the aim mapping consumes.
    f32 corrGx = 0.0f, corrGy = 0.0f, corrGz = 0.0f;
    // Stillness accumulator + running accel-magnitude reference for the "steady" test.
    f32  stillTime    = 0.0f;
    f32  accelMagEma  = 0.0f;
    bool accelMagInit = false;

    // gyro in rad/s, accel in any consistent units (normalised internally), dt in seconds.
    void update(f32 gx, f32 gy, f32 gz, f32 ax, f32 ay, f32 az, f32 dt) {
        // --- tuning knobs (single place to adjust feel) ---
        // BASELINE auto-calibration: only learns the resting bias when the gyro itself reads near
        // zero (< STILL_GYRO) and the gravity field is steady. On PC controllers (small bias) this
        // reliably zeroes the tiny drift. On Switch Joy-Cons the resting bias exceeds STILL_GYRO so
        // this gate never fires → no bias is learned → raw gyro (the original mild handheld drift).
        // That deadlock is intentional here: handheld can't be held still enough to sample a CLEAN
        // bias, and a contaminated sample makes the drift WORSE, so we'd rather not calibrate than
        // mis-calibrate. (A deliberate "hold still + press to recalibrate" is the real Switch fix
        // if we revisit it — it gives a clean sample without ever touching live aim.)
        constexpr f32 BETA       = 0.05f;  // Madgwick accel-correction gain
        constexpr f32 STILL_GYRO = 0.05f;  // rad/s (~3 deg/s): below = "not deliberately turning"
        constexpr f32 STILL_HOLD = 0.3f;   // s of stillness before trusting the bias estimate
        constexpr f32 BIAS_LERP  = 0.02f;  // how fast the resting bias is learned
        constexpr f32 ACC_STEADY = 0.06f;  // accel mag may deviate this fraction from its EMA

        const f32 gyroMag = sqrtf(gx*gx + gy*gy + gz*gz);
        const f32 accMag  = sqrtf(ax*ax + ay*ay + az*az);
        const bool accelValid = accMag > 1e-6f;  // false if the controller has no accelerometer

        // --- 1. stillness detection: low rotation AND (if available) a steady gravity field.
        //        With no accel, fall back to the gyro-only test. ---
        f32 accDev = 0.0f;
        if (accelValid) {
            if (!accelMagInit) { accelMagEma = accMag; accelMagInit = true; }
            accDev = (accelMagEma > 1e-6f) ? fabsf(accMag - accelMagEma) / accelMagEma : 1.0f;
            accelMagEma += (accMag - accelMagEma) * 0.05f;   // slow EMA of |accel|
        }
        const bool steady = (gyroMag < STILL_GYRO) && (!accelValid || accDev < ACC_STEADY);
        stillTime = steady ? (stillTime + dt) : 0.0f;

        // --- 2. learn the resting bias once the device has been still long enough ---
        if (stillTime > STILL_HOLD) {
            biasX += (gx - biasX) * BIAS_LERP;
            biasY += (gy - biasY) * BIAS_LERP;
            biasZ += (gz - biasZ) * BIAS_LERP;
        }

        // --- 3. bias-corrected gyro (the aim output) ---
        corrGx = gx - biasX;
        corrGy = gy - biasY;
        corrGz = gz - biasZ;

        // --- 4. Madgwick IMU orientation update (drift-corrected via gravity) ---
        const f32 cgx = corrGx, cgy = corrGy, cgz = corrGz;
        // Quaternion rate from the (corrected) gyro.
        f32 qDot0 = 0.5f * (-q1*cgx - q2*cgy - q3*cgz);
        f32 qDot1 = 0.5f * ( q0*cgx + q2*cgz - q3*cgy);
        f32 qDot2 = 0.5f * ( q0*cgy - q1*cgz + q3*cgx);
        f32 qDot3 = 0.5f * ( q0*cgz + q1*cgy - q2*cgx);

        // Accel gradient-descent correction (standard Madgwick IMU) — only with valid gravity.
        if (accMag > 1e-6f) {
            const f32 inv = 1.0f / accMag;
            const f32 nax = ax*inv, nay = ay*inv, naz = az*inv;
            const f32 _2q0 = 2.0f*q0, _2q1 = 2.0f*q1, _2q2 = 2.0f*q2, _2q3 = 2.0f*q3;
            const f32 _4q0 = 4.0f*q0, _4q1 = 4.0f*q1, _4q2 = 4.0f*q2;
            const f32 _8q1 = 8.0f*q1, _8q2 = 8.0f*q2;
            const f32 q0q0 = q0*q0, q1q1 = q1*q1, q2q2 = q2*q2, q3q3 = q3*q3;
            f32 s0 = _4q0*q2q2 + _2q2*nax + _4q0*q1q1 - _2q1*nay;
            f32 s1 = _4q1*q3q3 - _2q3*nax + 4.0f*q0q0*q1 - _2q0*nay - _4q1 + _8q1*q1q1 + _8q1*q2q2 + _4q1*naz;
            f32 s2 = 4.0f*q0q0*q2 + _2q0*nax + _4q2*q3q3 - _2q3*nay - _4q2 + _8q2*q1q1 + _8q2*q2q2 + _4q2*naz;
            f32 s3 = 4.0f*q1q1*q3 - _2q1*nax + 4.0f*q2q2*q3 - _2q2*nay;
            const f32 snorm = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
            if (snorm > 1e-9f) {
                const f32 sinv = 1.0f / snorm;
                qDot0 -= BETA * s0 * sinv;
                qDot1 -= BETA * s1 * sinv;
                qDot2 -= BETA * s2 * sinv;
                qDot3 -= BETA * s3 * sinv;
            }
        }

        q0 += qDot0 * dt; q1 += qDot1 * dt; q2 += qDot2 * dt; q3 += qDot3 * dt;
        const f32 qnorm = sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
        if (qnorm > 1e-9f) {
            const f32 qinv = 1.0f / qnorm;
            q0 *= qinv; q1 *= qinv; q2 *= qinv; q3 *= qinv;
        }
    }
};

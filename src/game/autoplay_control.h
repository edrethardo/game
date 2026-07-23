// autoplay_control.h — who is driving: the bot or the human.
//
// Pure latch. Real gameplay-device activity (Input::humanActivityThisFrame) hands control to the
// human instantly; after RESUME_SECONDS with no activity the bot resumes. Activity while a blocking
// UI screen is open is IGNORED — opening the inventory to change the build must not count as taking
// over (that is the whole point of "browse the build while it keeps fighting"). Only the caller
// knows whether a takeover-exempt screen is open; it passes uiOpen in.
#pragma once
#include "core/types.h"

struct AutoplayControl {
    static constexpr f32 RESUME_SECONDS = 2.0f;   // idle time before the bot resumes

    bool botInControl() const { return m_botControl; }
    f32  resumeCountdown() const { return m_resumeTimer; }   // for the HUD "MANUAL · Ns" readout

    void tick(bool humanActive, bool uiOpen, f32 dt) {
        if (uiOpen) return;                       // UI navigation is never a gameplay takeover
        if (humanActive) { m_botControl = false; m_resumeTimer = RESUME_SECONDS; return; }
        if (!m_botControl) {
            m_resumeTimer -= dt;
            if (m_resumeTimer <= 0.0f) { m_resumeTimer = 0.0f; m_botControl = true; }
        }
    }
    void forceBot() { m_botControl = true; m_resumeTimer = 0.0f; }   // mode entry / floor start

private:
    bool m_botControl = true;
    f32  m_resumeTimer = 0.0f;
};

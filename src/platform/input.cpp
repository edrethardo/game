#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "platform/input.h"
#include "platform/user_paths.h"
#include "core/log.h"
#include "core/imu_filter.h"

#include <cstring>
#include <cstdio>

static constexpr s32 NUM_KEYS = SDL_NUM_SCANCODES;
static constexpr s32 NUM_MOUSE_BUTTONS = 5;
static constexpr s32 NUM_PAD_BUTTONS = SDL_CONTROLLER_BUTTON_MAX;

static u8 s_currentKeys[NUM_KEYS];
static u8 s_previousKeys[NUM_KEYS];

static u8 s_currentMouseButtons[NUM_MOUSE_BUTTONS];
static u8 s_previousMouseButtons[NUM_MOUSE_BUTTONS];
static s32 s_mouseDX = 0;
static s32 s_mouseDY = 0;
static s32 s_mouseX = 0;
static s32 s_mouseY = 0;
static s32 s_mouseWheelY = 0;

static SDL_GameController* s_controllers[Input::MAX_GAMEPADS] = {};

// Sensitivity settings (adjustable from options menu)
static f32  s_stickSensitivity = 1.0f;
static f32  s_gyroSensitivity  = 5.0f;
static bool s_stickInvertY     = false;
static bool s_gyroInvertY      = true;
static f32  s_mouseSensitivity = Input::MOUSE_SENS_DEFAULT; // multiplier on base mouse rad/px

// Active-input-device tracking (see input.h). Declared up here so init() (below) can set the
// launch default. Thresholds are deliberately above the movement deadzones so a resting/drifting
// stick or tiny mouse jitter never flips the on-screen glyphs.
static Input::InputDevice s_activeDevice = Input::InputDevice::KeyboardMouse; // fixed up in init()
static constexpr s32 DEVICE_MOUSE_MOVE_PX     = 6;    // Manhattan px/frame to count as mouse use
static constexpr f32 DEVICE_STICK_DEADZONE    = 0.5f; // normalized stick magnitude for "pad in use"
static constexpr f32 DEVICE_TRIGGER_THRESHOLD = 0.5f;

// Trailing "settings" rows appended to controls.json — sentinel indices ABOVE GameAction::COUNT
// so older readers (which guard `idx < COUNT`) skip them, and so a controls.json written before
// this feature simply leaves the defaults in place. Lets us persist look sensitivity + invert-Y
// in the same flat file without changing its per-line format. (Before this, none of these
// settings persisted across launches — they reset to the defaults above every run.)
static constexpr u32 CFG_STICK_SENS   = 1000;
static constexpr u32 CFG_GYRO_SENS    = 1001;
static constexpr u32 CFG_MOUSE_SENS   = 1002;
static constexpr u32 CFG_STICK_INVERT = 1003;
static constexpr u32 CFG_GYRO_INVERT  = 1004;

// Split-screen: which player's controller to read (0=player1, 1=player2)
static u8 s_activePlayer = 0;

// Cached gyro reading — updated once per frame in Input::update(),
// returned by getGyro(). Per-player to avoid P2 sensor bleeding into P1.
static f32 s_gyroDx[2] = {};
static f32 s_gyroDy[2] = {};

// Per-player IMU sensor-fusion filter (gyro + accel). Removes gyro zero-rate bias so the aim
// no longer drifts; its bias-corrected gyro feeds the SAME look mapping the raw gyro used.
static ImuFilter s_imu[2];

// Per-frame dt for the IMU filter, derived from the wall clock — self-contained (no coupling to
// the game Clock). Clamped so a frame hitch can't blow up the integrator.
static u32 s_gyroPrevTicks = 0;
static f32 gyroFrameDt() {
    u32 now = SDL_GetTicks();
    f32 dt = (s_gyroPrevTicks == 0) ? (1.0f / 60.0f) : (now - s_gyroPrevTicks) * 0.001f;
    s_gyroPrevTicks = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.05f)  dt = 0.05f;
    return dt;
}

static void updateGyroCache(); // forward decl — platform-specific definition below
#ifdef __SWITCH__
static void initGyro();        // forward declaration — enables sensors (defined below)
static void initVibration();   // forward declaration — opens rumble devices (defined below)
static void switchRumble(u8 player, f32 strength, u32 durationMs); // libnx rumble
static void tickRumbleStop();  // stop a pulse once its duration elapses
// Rumble pulse end-times (SDL_GetTicks ms) per player — plain types so they live up here;
// the libnx HidVibrationDeviceHandle table needs <switch.h> and is defined further below.
static u32  s_rumbleEndMs[2]  = {};
static bool s_rumbleActive[2] = {};
#endif

#ifdef __SWITCH__
#include <switch.h>
// libnx per-player pad state — used for all gamepad input on Switch
static PadState s_pads[2];
static bool s_padsInitialized = false;
static u64 s_padPrevButtons[2] = {};

static void initPads() {
    if (s_padsInitialized) return;
    s_padsInitialized = true;
    // Accept Pro Controller, handheld, and dual joycons (two joycons held
    // sideways or attached to the grip as a single controller)
    padConfigureInput(2, HidNpadStyleSet_NpadFullCtrl | HidNpadStyleTag_NpadJoyDual);
    padInitializeDefault(&s_pads[0]);   // P1: auto-detects handheld, pro, or dual joycon
    padInitialize(&s_pads[1], HidNpadIdType_No2);
    // Seed prev state so buttons held at init don't trigger false presses
    for (s32 p = 0; p < 2; p++) {
        padUpdate(&s_pads[p]);
        s_padPrevButtons[p] = padGetButtons(&s_pads[p]);
    }
    LOG_INFO("Pads: initialized 2-player input");
}

#endif

// Per-controller per-frame button state for pressed detection
static u8 s_currentPadButtons[Input::MAX_GAMEPADS][NUM_PAD_BUTTONS];
static u8 s_previousPadButtons[Input::MAX_GAMEPADS][NUM_PAD_BUTTONS];
static bool s_splitActive = false;

// Axis edge detection — raw axis values snapshotted each frame so "pressed"
// only fires on threshold crossing, not every tick while held.
static constexpr s32 MAX_AXES = 8;
static f32 s_currentAxisValues[Input::MAX_GAMEPADS][MAX_AXES];
static f32 s_previousAxisValues[Input::MAX_GAMEPADS][MAX_AXES];

// Left-stick MENU navigation edge state (per pad, per StickNav direction). `pressed` is the
// this-frame edge (set in update(), cleared by consumePressedState() like a button press);
// `latched` stays true while the stick is held past the deflect threshold so a held stick fires
// exactly once until it returns toward center (hysteresis re-arm). See isMenuStickPressed().
static bool s_menuStickPressed[Input::MAX_GAMEPADS][4];
static bool s_menuStickLatched[Input::MAX_GAMEPADS][4];

// ---------------------------------------------------------------------------
// Default bindings table
// ---------------------------------------------------------------------------
// key, mouseButton, button, modifier, axis, axisThreshold
static InputBinding s_bindings[static_cast<u32>(GameAction::COUNT)];

// Fill `out` with the default binding table. Split out from setDefaults() so the per-category
// resets (resetKeyboard/ControllerBindingsToDefaults) can restore just half of each binding.
static void buildDefaults(InputBinding out[static_cast<u32>(GameAction::COUNT)]) {
    auto& b = out;
    auto set = [&](GameAction a, s32 key, u8 mouse, s32 btn, s32 mod = -1, s32 ax = -1, f32 axT = 0.0f) {
        u32 i = static_cast<u32>(a);
        b[i] = {key, mouse, btn, mod, ax, axT};
    };

    // Movement
    set(GameAction::MOVE_FORWARD,  SDL_SCANCODE_W,     0, -1);
    set(GameAction::MOVE_BACKWARD, SDL_SCANCODE_S,     0, -1);
    set(GameAction::MOVE_LEFT,     SDL_SCANCODE_A,     0, -1);
    set(GameAction::MOVE_RIGHT,    SDL_SCANCODE_D,     0, -1);

    // Core actions
    set(GameAction::JUMP,          SDL_SCANCODE_SPACE,  0, SDL_CONTROLLER_BUTTON_A);
    set(GameAction::FIRE,          -1, MOUSE_LEFT,       -1, -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0.5f);
    set(GameAction::BLOCK,         SDL_SCANCODE_LCTRL,  0, -1, -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0.5f);
    set(GameAction::DODGE,         SDL_SCANCODE_LSHIFT, 0, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    set(GameAction::CLASS_SKILL,   -1, MOUSE_RIGHT,      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    set(GameAction::TARGET_LOCK,   -1, MOUSE_MIDDLE,     -1); // L is modifier-only on gamepad, not target lock

    // Items / utility
    set(GameAction::POTION,        SDL_SCANCODE_Q,      0, SDL_CONTROLLER_BUTTON_B);
    set(GameAction::PICKUP,        SDL_SCANCODE_E,      0, SDL_CONTROLLER_BUTTON_X);
    set(GameAction::RELOAD,        SDL_SCANCODE_R,      0, SDL_CONTROLLER_BUTTON_Y);

    // Skill selection (D-pad)
    set(GameAction::SKILL_1,       SDL_SCANCODE_1,      0, SDL_CONTROLLER_BUTTON_DPAD_UP);
    set(GameAction::SKILL_2,       SDL_SCANCODE_2,      0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    set(GameAction::SKILL_3,       SDL_SCANCODE_3,      0, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    set(GameAction::SKILL_4,       SDL_SCANCODE_4,      0, SDL_CONTROLLER_BUTTON_DPAD_LEFT);

    // Equipment skills (L + face button)
    set(GameAction::BOOT_SKILL,    SDL_SCANCODE_F,      0, SDL_CONTROLLER_BUTTON_A,    SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(GameAction::HELMET_SKILL,  SDL_SCANCODE_G,      0, SDL_CONTROLLER_BUTTON_B,    SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    // UI
    set(GameAction::INVENTORY,       SDL_SCANCODE_TAB,    0, SDL_CONTROLLER_BUTTON_START);
    set(GameAction::PAUSE,           SDL_SCANCODE_ESCAPE, 0, SDL_CONTROLLER_BUTTON_BACK);
    // Character inspect screen. Keyboard: C. Gamepad: L + "+" chord (LEFTSHOULDER + START).
    // INVENTORY is bare "+" (START); the shared-button collision is resolved in checkActionRaw,
    // where a bare binding yields to a chord that claims the same button while L is held.
    set(GameAction::CHARACTER_SCREEN, SDL_SCANCODE_C,      0, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(GameAction::QUICKBAR_PREV, -1,                  0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(GameAction::QUICKBAR_NEXT, -1,                  0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    // Menu navigation
    set(GameAction::MENU_UP,       SDL_SCANCODE_UP,     0, SDL_CONTROLLER_BUTTON_DPAD_UP);
    set(GameAction::MENU_DOWN,     SDL_SCANCODE_DOWN,   0, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    set(GameAction::MENU_CONFIRM,  SDL_SCANCODE_RETURN, 0, SDL_CONTROLLER_BUTTON_A);
    set(GameAction::MENU_BACK,     SDL_SCANCODE_ESCAPE, 0, SDL_CONTROLLER_BUTTON_B);
}

static void setDefaults() { buildDefaults(s_bindings); }

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
void Input::init() {
    memset(s_currentKeys, 0, sizeof(s_currentKeys));
    memset(s_previousKeys, 0, sizeof(s_previousKeys));
    memset(s_currentMouseButtons, 0, sizeof(s_currentMouseButtons));
    memset(s_previousMouseButtons, 0, sizeof(s_previousMouseButtons));
    memset(s_currentPadButtons, 0, sizeof(s_currentPadButtons));
    memset(s_previousPadButtons, 0, sizeof(s_previousPadButtons));
    s_splitActive = false;
    s_mouseDX = 0;
    s_mouseDY = 0;

#ifdef __SWITCH__
    initPads();
    initGyro();       // start sensors early so they warm up during menus
    initVibration();  // open rumble devices up front
#endif

    setDefaults();

    // Open any controllers already connected
    for (s32 i = 0; i < SDL_NumJoysticks() && i < MAX_GAMEPADS; ++i) {
        if (SDL_IsGameController(i)) {
            s_controllers[i] = SDL_GameControllerOpen(i);
            if (s_controllers[i]) {
                LOG_INFO("Gamepad %d connected: %s", i,
                         SDL_GameControllerName(s_controllers[i]));
#ifndef __SWITCH__
                // Enable gyro + accel sensors if available (PC: SDL2 sensor API). The accel
                // feeds the IMU filter's gravity reference for drift correction.
                if (SDL_GameControllerHasSensor(s_controllers[i], SDL_SENSOR_GYRO)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[i], SDL_SENSOR_GYRO, SDL_TRUE);
                    LOG_INFO("Gamepad %d: gyro enabled", i);
                }
                if (SDL_GameControllerHasSensor(s_controllers[i], SDL_SENSOR_ACCEL)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[i], SDL_SENSOR_ACCEL, SDL_TRUE);
                }
#endif
            }
        }
    }

    // Default active device: if a pad is already connected at launch, start showing controller
    // glyphs (preserves Steam Deck "controller by default" until the player touches keyboard/mouse).
    // Runs AFTER the controller-open loop so s_controllers[] is populated.
#ifdef __SWITCH__
    s_activeDevice = InputDevice::Gamepad;
#else
    s_activeDevice = InputDevice::KeyboardMouse;
    for (s32 i = 0; i < MAX_GAMEPADS; ++i) if (s_controllers[i]) { s_activeDevice = InputDevice::Gamepad; break; }
#endif

    // Try to load saved bindings. Desktop reads from the per-user data dir (Steam-Cloud-synced);
    // Switch keeps the read-only romfs default via ASSET_PATH.
#ifdef __SWITCH__
    loadBindings(ASSET_PATH("assets/config/controls.json"));
#else
    char ctrlPath[512];
    loadBindings(Platform::userDataPath("controls.json", ctrlPath, sizeof(ctrlPath)));
#endif

    LOG_INFO("Input system initialized");
}

void Input::shutdown() {
    for (s32 i = 0; i < MAX_GAMEPADS; ++i) {
        if (s_controllers[i]) {
            SDL_GameControllerClose(s_controllers[i]);
            s_controllers[i] = nullptr;
        }
    }
    LOG_INFO("Input system shut down");
}

Input::InputDevice Input::activeDevice()      { return s_activeDevice; }
bool Input::activeDeviceIsGamepad()           { return s_activeDevice == InputDevice::Gamepad; }

void Input::update() {
    // Copy current state to previous
    memcpy(s_previousKeys, s_currentKeys, sizeof(s_currentKeys));
    memcpy(s_previousMouseButtons, s_currentMouseButtons, sizeof(s_currentMouseButtons));
    memcpy(s_previousPadButtons, s_currentPadButtons, sizeof(s_currentPadButtons));

    s_mouseWheelY = 0;

    // Snapshot keyboard
    int numKeys = 0;
    const u8* state = SDL_GetKeyboardState(&numKeys);
    s32 copyCount = (numKeys < NUM_KEYS) ? numKeys : NUM_KEYS;
    memcpy(s_currentKeys, state, copyCount);

    // Mouse
    u32 mouseState = SDL_GetRelativeMouseState(&s_mouseDX, &s_mouseDY);
    SDL_GetMouseState(&s_mouseX, &s_mouseY);
    for (s32 i = 0; i < NUM_MOUSE_BUTTONS; ++i) {
        s_currentMouseButtons[i] = (mouseState & SDL_BUTTON(i + 1)) ? 1 : 0;
    }

    // Gamepad button state snapshot — used on PC for frame-edge press detection
    memset(s_currentPadButtons, 0, sizeof(s_currentPadButtons));
    for (s32 c = 0; c < MAX_GAMEPADS; c++) {
        if (!s_controllers[c]) continue;
        for (s32 i = 0; i < NUM_PAD_BUTTONS; i++) {
            s_currentPadButtons[c][i] = SDL_GameControllerGetButton(
                s_controllers[c], static_cast<SDL_GameControllerButton>(i));
        }
    }

    // Axis value snapshot — edge detection for trigger-based "pressed" queries
    memcpy(s_previousAxisValues, s_currentAxisValues, sizeof(s_currentAxisValues));
    memset(s_currentAxisValues, 0, sizeof(s_currentAxisValues));
    for (s32 c = 0; c < MAX_GAMEPADS; c++) {
        if (!s_controllers[c]) continue;
        for (s32 a = 0; a < MAX_AXES; a++) {
            s16 raw = SDL_GameControllerGetAxis(
                s_controllers[c], static_cast<SDL_GameControllerAxis>(a));
            s_currentAxisValues[c][a] = static_cast<f32>(raw) / 32767.0f;
        }
    }

#ifdef __SWITCH__
    // Update libnx pads every frame (used for all gamepad input on Switch)
    for (s32 p = 0; p < 2; p++) {
        s_padPrevButtons[p] = padGetButtons(&s_pads[p]);
        padUpdate(&s_pads[p]);
    }
    tickRumbleStop();  // end any rumble pulse whose duration has elapsed (Switch libnx rumble)
#endif

    // Menu-stick navigation: turn each pad's left-stick deflection into a debounced edge "press"
    // so the analog stick drives menus like a D-pad tap. Uses getAxis + the same deadzone remap as
    // getStickX/Y (so platform behavior matches in-game movement), with hysteresis: fire once at
    // DEFLECT, re-arm only below RELEASE. Computed here (once per frame); consumePressedState()
    // clears `pressed` so it can't multi-fire across the fixed-timestep substeps.
    {
        constexpr f32 NAV_DEFLECT = 0.6f;   // post-deadzone magnitude to register a nav "press"
        constexpr f32 NAV_RELEASE = 0.35f;  // must fall back below this before the next press
        memset(s_menuStickPressed, 0, sizeof(s_menuStickPressed));
        auto axisMag = [](s32 pad, s32 axis) -> f32 {
            f32 v = getAxis(pad, axis);
            if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
            f32 sign = (v > 0) ? 1.0f : -1.0f;
            return sign * (fabsf(v) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
        };
        for (s32 c = 0; c < Input::MAX_GAMEPADS; c++) {
            f32 lx = axisMag(c, SDL_CONTROLLER_AXIS_LEFTX);
            f32 ly = axisMag(c, SDL_CONTROLLER_AXIS_LEFTY);  // SDL convention: up is negative
            // Per-direction signed magnitude (positive = deflected that way).
            const f32 dirMag[4] = {
                -ly,  // StickNav::Up
                 ly,  // StickNav::Down
                -lx,  // StickNav::Left
                 lx,  // StickNav::Right
            };
            for (s32 d = 0; d < 4; d++) {
                if (dirMag[d] >= NAV_DEFLECT) {
                    if (!s_menuStickLatched[c][d]) {
                        s_menuStickPressed[c][d] = true;
                        s_menuStickLatched[c][d] = true;
                    }
                } else if (dirMag[d] < NAV_RELEASE) {
                    s_menuStickLatched[c][d] = false;
                }
            }
        }
    }

    // Gyro cache: run the IMU drift filter and refresh the per-player look delta once per frame
    // (both platforms — the PC and Switch definitions of updateGyroCache live above).
    updateGyroCache();

    // --- Active input device (KBM vs Gamepad) --------------------------------------------------
    // Decide which device the player is actively using so prompts/glyphs show the matching symbols
    // even when a controller is also connected (Steam Deck). LEVEL detection: a held key or pushed
    // stick counts as "in use" (resting devices read 0 thanks to the thresholds). Last-device-wins:
    // switch only when exactly one device is active this frame; hold on both/neither.
#ifdef __SWITCH__
    s_activeDevice = InputDevice::Gamepad;   // console: no KBM, and the pad path is libnx (above)
#else
    bool kbmActive = false;
    for (s32 i = 0; i < NUM_KEYS && !kbmActive; ++i) if (s_currentKeys[i]) kbmActive = true;
    if (!kbmActive) {
        s32 md = (s_mouseDX < 0 ? -s_mouseDX : s_mouseDX) + (s_mouseDY < 0 ? -s_mouseDY : s_mouseDY);
        kbmActive = (md >= DEVICE_MOUSE_MOVE_PX);
        for (s32 i = 0; i < NUM_MOUSE_BUTTONS && !kbmActive; ++i) if (s_currentMouseButtons[i]) kbmActive = true;
    }

    bool padActive = false;
    for (s32 c = 0; c < MAX_GAMEPADS && !padActive; ++c) {
        if (!s_controllers[c]) continue;
        for (s32 b = 0; b < NUM_PAD_BUTTONS && !padActive; ++b) if (s_currentPadButtons[c][b]) padActive = true;
        // Sticks rest near 0 and swing both ways → magnitude test past a generous deadzone.
        if (!padActive && (fabsf(s_currentAxisValues[c][SDL_CONTROLLER_AXIS_LEFTX])  >= DEVICE_STICK_DEADZONE ||
                           fabsf(s_currentAxisValues[c][SDL_CONTROLLER_AXIS_LEFTY])  >= DEVICE_STICK_DEADZONE ||
                           fabsf(s_currentAxisValues[c][SDL_CONTROLLER_AXIS_RIGHTX]) >= DEVICE_STICK_DEADZONE ||
                           fabsf(s_currentAxisValues[c][SDL_CONTROLLER_AXIS_RIGHTY]) >= DEVICE_STICK_DEADZONE))
            padActive = true;
        // Triggers rest near 0 → one-sided threshold.
        if (!padActive && (s_currentAxisValues[c][SDL_CONTROLLER_AXIS_TRIGGERLEFT]  >= DEVICE_TRIGGER_THRESHOLD ||
                           s_currentAxisValues[c][SDL_CONTROLLER_AXIS_TRIGGERRIGHT] >= DEVICE_TRIGGER_THRESHOLD))
            padActive = true;
    }

    if (kbmActive != padActive)   // exactly one device active → adopt it (else keep the last one)
        s_activeDevice = kbmActive ? InputDevice::KeyboardMouse : InputDevice::Gamepad;
#endif
}

// Consume all "just pressed" edges so subsequent accumulator ticks see them as held,
// not freshly pressed. Call after the first tick in the fixed-timestep loop.
void Input::consumePressedState() {
    memcpy(s_previousKeys, s_currentKeys, NUM_KEYS);
    memcpy(s_previousMouseButtons, s_currentMouseButtons, NUM_MOUSE_BUTTONS);
    memcpy(s_previousPadButtons, s_currentPadButtons, sizeof(s_currentPadButtons));
    memcpy(s_previousAxisValues, s_currentAxisValues, sizeof(s_currentAxisValues));
    // Menu-stick edges are one-shot per frame too (the latch in update() handles re-arming).
    memset(s_menuStickPressed, 0, sizeof(s_menuStickPressed));
#ifdef __SWITCH__
    // Also consume libnx pad edges — isButtonPressed on Switch reads
    // s_padPrevButtons (libnx), not the SDL s_previousPadButtons arrays.
    for (s32 p = 0; p < 2; p++) {
        s_padPrevButtons[p] = padGetButtons(&s_pads[p]);
    }
#endif
}

// ---------------------------------------------------------------------------
// Raw keyboard
// ---------------------------------------------------------------------------
bool Input::isKeyDown(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return s_currentKeys[scancode] != 0;
}
bool Input::isKeyPressed(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return s_currentKeys[scancode] && !s_previousKeys[scancode];
}
bool Input::isKeyReleased(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return !s_currentKeys[scancode] && s_previousKeys[scancode];
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void Input::getMouseDelta(s32& dx, s32& dy) { dx = s_mouseDX; dy = s_mouseDY; }
void Input::getMousePosition(s32& x, s32& y) { x = s_mouseX; y = s_mouseY; }

bool Input::isMouseButtonDown(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return s_currentMouseButtons[button - 1] != 0;
}
bool Input::isMouseButtonPressed(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return s_currentMouseButtons[button - 1] && !s_previousMouseButtons[button - 1];
}
bool Input::isMouseButtonReleased(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return !s_currentMouseButtons[button - 1] && s_previousMouseButtons[button - 1];
}
// Relative-mouse mode is the gameplay aim mode (cursor hidden + warped). Menus/pause/death turn it
// OFF. We track the state so we can (a) latch the ON→OFF edge — "gameplay just handed off to a
// cursor screen" — for the menu input-mode gate, and (b) re-show the cursor as a baseline on every
// transition so a menu that hid it can't strand it hidden on the next screen.
static bool s_relativeMode     = false;
static bool s_relativeReleased = false;  // latched ON->OFF edge, consumed by consumeRelativeReleased()
static bool s_cursorVisible    = true;    // our SDL_ShowCursor state (edge-tracked)

void Input::setRelativeMouseMode(bool enabled) {
    if (s_relativeMode && !enabled) s_relativeReleased = true;  // gameplay -> cursor screen edge
    s_relativeMode = enabled;
    SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
    // Baseline: any mode transition restores a visible cursor (relative mode hides the real cursor
    // while active anyway). The menu gate is the only thing that hides it, via setCursorVisible.
    if (!s_cursorVisible) { SDL_ShowCursor(SDL_ENABLE); s_cursorVisible = true; }
}

void Input::setCursorVisible(bool visible) {
    if (visible == s_cursorVisible) return;   // only hit SDL on a transition
    s_cursorVisible = visible;
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

bool Input::consumeRelativeReleased() {
    bool v = s_relativeReleased;
    s_relativeReleased = false;
    return v;
}
s32 Input::getMouseWheelDelta() { return s_mouseWheelY; }
void Input::handleMouseWheel(s32 y) { s_mouseWheelY += y; }

// ---------------------------------------------------------------------------
// Gamepad raw
// ---------------------------------------------------------------------------
f32 Input::getAxis(s32 gamepadIndex, s32 axis) {
#ifdef __SWITCH__
    if (s_splitActive && s_padsInitialized && gamepadIndex >= 0 && gamepadIndex < 2) {
        // Read analog sticks from libnx per-player pads
        HidAnalogStickState ls = padGetStickPos(&s_pads[gamepadIndex], 0); // left stick
        HidAnalogStickState rs = padGetStickPos(&s_pads[gamepadIndex], 1); // right stick
        switch (axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:  return static_cast<f32>(ls.x) / 32767.0f;
            case SDL_CONTROLLER_AXIS_LEFTY:  return static_cast<f32>(-ls.y) / 32767.0f; // SDL inverts Y
            case SDL_CONTROLLER_AXIS_RIGHTX: return static_cast<f32>(rs.x) / 32767.0f;
            case SDL_CONTROLLER_AXIS_RIGHTY: return static_cast<f32>(-rs.y) / 32767.0f;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                return (padGetButtons(&s_pads[gamepadIndex]) & HidNpadButton_ZL) ? 1.0f : 0.0f;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                return (padGetButtons(&s_pads[gamepadIndex]) & HidNpadButton_ZR) ? 1.0f : 0.0f;
            default: return 0.0f;
        }
    }
#endif
    if (s_splitActive) {
        if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return 0.0f;
        if (!s_controllers[gamepadIndex]) return 0.0f;
        s16 raw = SDL_GameControllerGetAxis(s_controllers[gamepadIndex],
                                             static_cast<SDL_GameControllerAxis>(axis));
        return static_cast<f32>(raw) / 32767.0f;
    }
    f32 best = 0.0f;
    for (s32 c = 0; c < MAX_GAMEPADS; c++) {
        if (!s_controllers[c]) continue;
        s16 raw = SDL_GameControllerGetAxis(s_controllers[c],
                                             static_cast<SDL_GameControllerAxis>(axis));
        f32 v = static_cast<f32>(raw) / 32767.0f;
        if (fabsf(v) > fabsf(best)) best = v;
    }
    return best;
}

#ifdef __SWITCH__
// Map SDL button constants to libnx HidNpadButton.
// libnx uses Nintendo naming natively (A=right, B=bottom), so this is a direct 1:1 mapping.
static u64 sdlButtonToHid(s32 sdlButton) {
    switch (sdlButton) {
        case SDL_CONTROLLER_BUTTON_A:             return HidNpadButton_A;  // Nintendo A (right)
        case SDL_CONTROLLER_BUTTON_B:             return HidNpadButton_B;  // Nintendo B (bottom)
        case SDL_CONTROLLER_BUTTON_X:             return HidNpadButton_X;  // Nintendo X (top)
        case SDL_CONTROLLER_BUTTON_Y:             return HidNpadButton_Y;  // Nintendo Y (left)
        case SDL_CONTROLLER_BUTTON_BACK:          return HidNpadButton_Minus;
        case SDL_CONTROLLER_BUTTON_START:         return HidNpadButton_Plus;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return HidNpadButton_StickL;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return HidNpadButton_StickR;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return HidNpadButton_L;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return HidNpadButton_R;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return HidNpadButton_Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return HidNpadButton_Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return HidNpadButton_Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return HidNpadButton_Right;
        default: return 0;
    }
}
#endif

bool Input::isButtonDown(s32 gamepadIndex, s32 button) {
#ifdef __SWITCH__
    // Always use libnx pads on Switch — Nintendo naming, no SDL remap needed
    if (gamepadIndex < 0 || gamepadIndex > 1) return false;
    u64 hid = sdlButtonToHid(button);
    return (padGetButtons(&s_pads[gamepadIndex]) & hid) != 0;
#else
    if (s_splitActive) {
        if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
        if (!s_controllers[gamepadIndex]) return false;
        return SDL_GameControllerGetButton(s_controllers[gamepadIndex],
                                            static_cast<SDL_GameControllerButton>(button)) != 0;
    }
    // Single-player: merge all controllers
    for (s32 c = 0; c < MAX_GAMEPADS; c++) {
        if (!s_controllers[c]) continue;
        if (SDL_GameControllerGetButton(s_controllers[c],
                static_cast<SDL_GameControllerButton>(button)))
            return true;
    }
    return false;
#endif
}

bool Input::isButtonPressed(s32 gamepadIndex, s32 button) {
    if (button < 0 || button >= NUM_PAD_BUTTONS) return false;
#ifdef __SWITCH__
    // Always use libnx pads on Switch — Nintendo naming, no SDL remap needed
    if (gamepadIndex < 0 || gamepadIndex > 1) return false;
    u64 hid = sdlButtonToHid(button);
    u64 cur = padGetButtons(&s_pads[gamepadIndex]);
    u64 prev = s_padPrevButtons[gamepadIndex];
    return (cur & hid) && !(prev & hid);
#else
    if (s_splitActive) {
        if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
        return s_currentPadButtons[gamepadIndex][button] && !s_previousPadButtons[gamepadIndex][button];
    }
    for (s32 c = 0; c < MAX_GAMEPADS; c++) {
        if (s_currentPadButtons[c][button] && !s_previousPadButtons[c][button])
            return true;
    }
    return false;
#endif
}

bool Input::isGamepadConnected(s32 gamepadIndex) {
#ifdef __SWITCH__
    // Switch always has controllers (Joy-Cons or Pro Controller via libnx)
    return gamepadIndex >= 0 && gamepadIndex <= 1;
#else
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    return s_controllers[gamepadIndex] != nullptr;
#endif
}

void Input::handleControllerEvent(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        s32 index = event.cdevice.which;
        if (index >= 0 && index < MAX_GAMEPADS && !s_controllers[index]) {
            s_controllers[index] = SDL_GameControllerOpen(index);
            if (s_controllers[index]) {
                LOG_INFO("Gamepad %d connected: %s", index,
                         SDL_GameControllerName(s_controllers[index]));
                // Enable gyro + accel sensors if available (accel feeds the IMU drift filter)
                if (SDL_GameControllerHasSensor(s_controllers[index], SDL_SENSOR_GYRO)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[index], SDL_SENSOR_GYRO, SDL_TRUE);
                    LOG_INFO("Gamepad %d: gyro enabled", index);
                }
                if (SDL_GameControllerHasSensor(s_controllers[index], SDL_SENSOR_ACCEL)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[index], SDL_SENSOR_ACCEL, SDL_TRUE);
                }
            }
        }
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_JoystickID id = event.cdevice.which;
        for (s32 i = 0; i < MAX_GAMEPADS; ++i) {
            if (s_controllers[i]) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(s_controllers[i]);
                if (SDL_JoystickInstanceID(joy) == id) {
                    LOG_INFO("Gamepad %d disconnected", i);
                    SDL_GameControllerClose(s_controllers[i]);
                    s_controllers[i] = nullptr;
                    break;
                }
            }
        }
        // If that was the last pad, revert glyphs to keyboard/mouse (the only device left) so we're
        // never stranded on controller prompts with no controller present.
        bool anyPad = false;
        for (s32 k = 0; k < MAX_GAMEPADS; ++k) if (s_controllers[k]) { anyPad = true; break; }
        if (!anyPad) s_activeDevice = InputDevice::KeyboardMouse;
    }
}

// ---------------------------------------------------------------------------
// Gamepad rumble
// ---------------------------------------------------------------------------
void Input::rumble(u8 slot, f32 strength, u32 durationMs) {
    if (slot >= static_cast<u8>(MAX_GAMEPADS)) return;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
#ifdef __SWITCH__
    // SDL_GameControllerRumble is a no-op on the libnx SDL port — drive the HID
    // vibration devices directly. slot maps 1:1 to the libnx pad index (0/1).
    if (slot < 2) switchRumble(slot, strength, durationMs);
#else
    SDL_GameController* gc = s_controllers[slot];
    if (!gc) return;
    Uint16 mag = static_cast<Uint16>(strength * 65535.0f);
    SDL_GameControllerRumble(gc, mag, mag, durationMs);
#endif
}

// ---------------------------------------------------------------------------
// Stick with deadzone
// ---------------------------------------------------------------------------
f32 Input::getStickX(bool rightStick, s32 gamepadIndex) {
    if (gamepadIndex == 0) gamepadIndex = s_activePlayer; // route to active player in split-screen
    s32 ax = rightStick ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX;
    f32 v = getAxis(gamepadIndex, ax);
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
    // Remap from deadzone..1.0 to 0..1.0 for smooth response
    f32 sign = (v > 0) ? 1.0f : -1.0f;
    f32 mag = (fabsf(v) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    return sign * mag;
}

f32 Input::getStickY(bool rightStick, s32 gamepadIndex) {
    if (gamepadIndex == 0) gamepadIndex = s_activePlayer;
    s32 ax = rightStick ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY;
    f32 v = getAxis(gamepadIndex, ax);
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
    f32 sign = (v > 0) ? 1.0f : -1.0f;
    f32 mag = (fabsf(v) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    return sign * mag;
}

bool Input::isMenuStickPressed(StickNav dir, s32 gamepadIndex) {
    if (gamepadIndex == 0) gamepadIndex = s_activePlayer; // route to active player like getStick*
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    return s_menuStickPressed[gamepadIndex][static_cast<u32>(dir)];
}

bool Input::isModifierHeld(s32 gamepadIndex) {
    return isButtonDown(gamepadIndex, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
}

// ---------------------------------------------------------------------------
// Gyro (motion sensor)
// ---------------------------------------------------------------------------

#ifdef __SWITCH__
#include <switch.h>
static bool s_gyroInitialized = false;

// Each gyro handle carries the npad style it was acquired for. We read only the handle
// whose style matches the pad's CURRENT active style (padGetStyleSet) — otherwise a
// started-but-unconnected sensor (e.g. the Pro handle in handheld mode) emits zero-valued
// samples and the old break-on-first-count>0 loop would starve the actually-active sensor.
struct GyroEntry {
    HidSixAxisSensorHandle handle;
    u64                    style; // HidNpadStyleTag_* bit
};
static GyroEntry s_gyroEntries[4];   // P1: Pro, Handheld, dual-JoyCon
static u32       s_gyroEntryCount = 0;
static GyroEntry s_gyroEntriesP2[4]; // P2: Pro, dual-JoyCon (only one player can be Handheld)
static u32       s_gyroEntryCountP2 = 0;

static void initGyro() {
    if (s_gyroInitialized) return;
    s_gyroInitialized = true;

    s_gyroEntryCount = 0;

    // P1: Pro Controller
    hidGetSixAxisSensorHandles(&s_gyroEntries[s_gyroEntryCount].handle, 1,
                               HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    s_gyroEntries[s_gyroEntryCount].style = HidNpadStyleTag_NpadFullKey;
    hidStartSixAxisSensor(s_gyroEntries[s_gyroEntryCount].handle);
    s_gyroEntryCount++;

    // P1: Handheld (attached joycons)
    hidGetSixAxisSensorHandles(&s_gyroEntries[s_gyroEntryCount].handle, 1,
                               HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    s_gyroEntries[s_gyroEntryCount].style = HidNpadStyleTag_NpadHandheld;
    hidStartSixAxisSensor(s_gyroEntries[s_gyroEntryCount].handle);
    s_gyroEntryCount++;

    // P1: Dual joycons (detached, used as one controller — right joycon has gyro)
    hidGetSixAxisSensorHandles(&s_gyroEntries[s_gyroEntryCount].handle, 1,
                               HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    s_gyroEntries[s_gyroEntryCount].style = HidNpadStyleTag_NpadJoyDual;
    hidStartSixAxisSensor(s_gyroEntries[s_gyroEntryCount].handle);
    s_gyroEntryCount++;

    // P2: Pro Controller
    s_gyroEntryCountP2 = 0;
    hidGetSixAxisSensorHandles(&s_gyroEntriesP2[s_gyroEntryCountP2].handle, 1,
                               HidNpadIdType_No2, HidNpadStyleTag_NpadFullKey);
    s_gyroEntriesP2[s_gyroEntryCountP2].style = HidNpadStyleTag_NpadFullKey;
    hidStartSixAxisSensor(s_gyroEntriesP2[s_gyroEntryCountP2].handle);
    s_gyroEntryCountP2++;

    // P2: Dual joycons
    hidGetSixAxisSensorHandles(&s_gyroEntriesP2[s_gyroEntryCountP2].handle, 1,
                               HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual);
    s_gyroEntriesP2[s_gyroEntryCountP2].style = HidNpadStyleTag_NpadJoyDual;
    hidStartSixAxisSensor(s_gyroEntriesP2[s_gyroEntryCountP2].handle);
    s_gyroEntryCountP2++;

    LOG_INFO("Gyro: initialized P1=%u P2=%u sensor handles", s_gyroEntryCount, s_gyroEntryCountP2);
}

// Read gyro sensor for one player into the per-player cache slot. Only the sensor whose
// style is currently active for `pad` is read — see GyroEntry comment for why.
static void readGyroForPlayer(u8 playerIdx, const GyroEntry* entries, u32 entryCount, PadState& pad, f32 dt) {
    s_gyroDx[playerIdx] = 0.0f;
    s_gyroDy[playerIdx] = 0.0f;
    const u64 activeStyle = padGetStyleSet(&pad);
    for (u32 i = 0; i < entryCount; i++) {
        if (!(activeStyle & entries[i].style)) continue; // skip handles whose npad style isn't active
        HidSixAxisSensorState states[16];
        s32 count = hidGetSixAxisSensorStates(entries[i].handle, states, 16);
        if (count <= 0) continue;
        // Average the buffered gyro samples and use them RAW. The Switch system already
        // bias-corrects the gyro — it reads EXACTLY 0.0 at rest (confirmed via nxlink capture),
        // so it has no zero-rate drift to remove. Running our own bias calibration here was
        // actively harmful: the still-detector fired on the motion-settling tail when you stop
        // turning, learned a SPURIOUS bias, then subtracted it from the already-zero rest gyro
        // → injecting drift that worsened every time you stopped aiming. (PC's SDL gyro is
        // genuinely raw/uncalibrated, so the PC path keeps the ImuFilter bias correction.)
        (void)dt; // no per-frame filter integration on Switch — raw gyro is already clean
        f32 gxv = 0, gzv = 0;
        for (s32 s = 0; s < count; s++) {
            gxv += states[s].angular_velocity.x;   // pitch axis (verified by nxlink capture)
            gzv += states[s].angular_velocity.z;   // yaw axis
        }
        const f32 inv = 1.0f / static_cast<f32>(count);
        // Axis mapping confirmed empirically (nxlink capture): a left/right TURN drives Z, an
        // up/down TILT drives X; Y is roll. The old `yaw = -(Y+Z)` folded roll into yaw, so any
        // incidental wrist-roll while moving turned the view ("working against me") — use Z alone.
        // SW_GYRO_SCALE tunes Switch gyro sensitivity. Started at the PC path's 0.2 but that felt
        // sluggish on Switch (libnx reports smaller per-motion values than SDL), so it's raised.
        // Single knob — increase for snappier aim, decrease if it feels twitchy.
        constexpr f32 SW_GYRO_SCALE = 1.0f;
        s_gyroDx[playerIdx] = -(gzv * inv) * (180.0f / 3.14159f) * SW_GYRO_SCALE; // yaw  = Z
        s_gyroDy[playerIdx] =  (gxv * inv) * (180.0f / 3.14159f) * SW_GYRO_SCALE; // pitch = X
        return; // active-style handle read; nothing else to consider this frame
    }
}

// Cache gyro reading once per frame — called from Input::update().
// Reads both players' sensors so each gets their own gyro data.
static void updateGyroCache() {
    initGyro();
    const f32 dt = gyroFrameDt();
    readGyroForPlayer(0, s_gyroEntries,   s_gyroEntryCount,   s_pads[0], dt);
    if (s_splitActive) {
        readGyroForPlayer(1, s_gyroEntriesP2, s_gyroEntryCountP2, s_pads[1], dt);
    }
}

// --- Switch rumble (libnx vibration) -----------------------------------------
// SDL_GameControllerRumble is a no-op on the libnx SDL port, so we drive the HID
// vibration devices directly — mirroring the gyro handle setup: initialise the two
// vibration devices for every npad style we accept, then each rumble sends to the
// handle pair whose style is currently active (handheld / pro / dual joycon).
// libnx vibration is continuous until stopped, so tickRumbleStop() sends zero
// amplitude once the requested pulse duration elapses.
struct VibEntry { HidVibrationDeviceHandle handles[2]; u64 style; };
static VibEntry s_vibEntries[4];     // P1
static u32      s_vibEntryCount   = 0;
static VibEntry s_vibEntriesP2[4];   // P2
static u32      s_vibEntryCountP2  = 0;
static bool     s_vibInitialized   = false;

static void initVibOne(VibEntry* arr, u32& count, HidNpadIdType id, u64 style) {
    hidInitializeVibrationDevices(arr[count].handles, 2, id, static_cast<HidNpadStyleTag>(style));
    arr[count].style = style;
    count++;
}

static void initVibration() {
    if (s_vibInitialized) return;
    s_vibInitialized = true;
    s_vibEntryCount = 0;
    initVibOne(s_vibEntries, s_vibEntryCount, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
    initVibOne(s_vibEntries, s_vibEntryCount, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    initVibOne(s_vibEntries, s_vibEntryCount, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
    s_vibEntryCountP2 = 0;
    initVibOne(s_vibEntriesP2, s_vibEntryCountP2, HidNpadIdType_No2,  HidNpadStyleTag_NpadFullKey);
    initVibOne(s_vibEntriesP2, s_vibEntryCountP2, HidNpadIdType_No2,  HidNpadStyleTag_NpadJoyDual);
    LOG_INFO("Rumble: initialized P1=%u P2=%u vibration devices", s_vibEntryCount, s_vibEntryCountP2);
}

// Send the same vibration to both devices of whichever style is active for `player`.
static void sendVib(u8 player, f32 amp) {
    initVibration();
    VibEntry* entries = (player == 0) ? s_vibEntries   : s_vibEntriesP2;
    u32       count   = (player == 0) ? s_vibEntryCount : s_vibEntryCountP2;
    const u64 activeStyle = padGetStyleSet(&s_pads[player]);
    HidVibrationValue val;
    val.amp_low   = amp;
    val.freq_low  = 160.0f;   // low-band motor frequency (Hz)
    val.amp_high  = amp;
    val.freq_high = 320.0f;   // high-band motor frequency (Hz)
    HidVibrationValue vals[2] = { val, val };
    for (u32 i = 0; i < count; i++) {
        if (!(activeStyle & entries[i].style)) continue; // only the active controller style
        hidSendVibrationValues(entries[i].handles, vals, 2);
    }
}

static void switchRumble(u8 player, f32 strength, u32 durationMs) {
    if (player > 1) return;
    sendVib(player, strength);
    s_rumbleEndMs[player]  = SDL_GetTicks() + durationMs;
    s_rumbleActive[player] = true;
}

// Called once per frame — stop any pulse whose duration has elapsed.
static void tickRumbleStop() {
    if (!s_vibInitialized) return;
    u32 now = SDL_GetTicks();
    for (u8 p = 0; p < 2; p++) {
        if (s_rumbleActive[p] && now >= s_rumbleEndMs[p]) {
            sendVib(p, 0.0f);            // zero amplitude = stop
            s_rumbleActive[p] = false;
        }
    }
}
#else // ---- PC ----
// Read SDL gyro + accel for one player's controller, run the IMU filter, and cache the
// bias-corrected look delta (deg/s) — same per-player cache the Switch fills. Without the filter
// the PC gyro was a stateless per-substep query; the filter needs persistent dt-stepped state,
// so it moves here (once per frame).
static void readGyroForPlayerPC(u8 player, s32 gamepadIndex, f32 dt) {
    s_gyroDx[player] = 0.0f;
    s_gyroDy[player] = 0.0f;
    if (gamepadIndex < 0 || gamepadIndex >= Input::MAX_GAMEPADS) return;
    SDL_GameController* gc = s_controllers[gamepadIndex];
    if (!gc) return;
    if (!SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO)) return;
    f32 g[3] = {}, a[3] = {};
    if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, g, 3) != 0) return;
    // Accel is optional — if the pad has none, a[] stays {0,0,0} and the filter degrades to a
    // gyro-only integrator + bias-cal (still removes drift via the gyro-magnitude still test).
    SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, a, 3);

    ImuFilter& f = s_imu[player];
    f.update(g[0], g[1], g[2], a[0], a[1], a[2], dt);
    // SDL gyro axes: [0]=x(pitch), [1]=y(yaw). Match the prior PC mapping — negate yaw and apply
    // PC_GYRO_SCALE so the shared gyroSensitivity lands in the Switch's range (SDL reports rad/s).
    constexpr f32 PC_GYRO_SCALE = 0.2f;
    s_gyroDx[player] = -f.corrGy * (180.0f / 3.14159f) * PC_GYRO_SCALE;  // yaw (un-inverted)
    s_gyroDy[player] =  f.corrGx * (180.0f / 3.14159f) * PC_GYRO_SCALE;  // pitch
}

static void updateGyroCache() {
    const f32 dt = gyroFrameDt();
    readGyroForPlayerPC(0, 0, dt);
    if (s_splitActive) readGyroForPlayerPC(1, 1, dt);
}
#endif

// Shared gyro read body. Both platforms now fill a per-player look-delta cache (deg/s) once per
// frame in updateGyroCache() — the IMU filter needs persistent, dt-stepped state, so the read is
// no longer a stateless query. `consume` zeroes the cache after the first read so the delta is
// applied once per render frame (getGyro=true); NetInput packing peeks without consuming.
static void readGyroInto(f32& dx, f32& dy, s32 gamepadIndex, bool consume) {
    (void)gamepadIndex; // cache is per-player; select by the active (swapped-in) player
    u8 pi = s_activePlayer;
    if (pi > 1) pi = 0;
    dx = s_gyroDx[pi];
    dy = s_gyroDy[pi];
    if (consume) {
        s_gyroDx[pi] = 0.0f;
        s_gyroDy[pi] = 0.0f;
    }
}

void Input::getGyro (f32& dx, f32& dy, s32 gamepadIndex) { readGyroInto(dx, dy, gamepadIndex, /*consume=*/true);  }
void Input::peekGyro(f32& dx, f32& dy, s32 gamepadIndex) { readGyroInto(dx, dy, gamepadIndex, /*consume=*/false); }

bool Input::isGyroAvailable(s32 gamepadIndex) {
#ifdef __SWITCH__
    (void)gamepadIndex;
    return true; // Switch always has gyro (Joy-Cons or Pro Controller)
#else
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    if (!s_controllers[gamepadIndex]) return false;
    return SDL_GameControllerHasSensor(s_controllers[gamepadIndex], SDL_SENSOR_GYRO);
#endif
}

// Switch software keyboard. Used for typing the multiplayer host IP, which is otherwise
// impossible on a console with no physical keyboard. Blocks until the user accepts/cancels.
bool Input::openVirtualKeyboard(const char* headerText, const char* initial,
                                char* outBuf, size_t outBufSize) {
    if (!outBuf || outBufSize == 0) return false;
    outBuf[0] = '\0';
#ifdef __SWITCH__
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    if (headerText && *headerText) swkbdConfigSetHeaderText(&kbd, headerText);
    if (initial && *initial)       swkbdConfigSetInitialText(&kbd, initial);
    // Cap the length so the result always fits the caller's buffer.
    swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(outBufSize - 1));
    Result rc = swkbdShow(&kbd, outBuf, outBufSize);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && outBuf[0] != '\0';
#else
    // PC has a physical keyboard; the lobby's SDL scancode path handles text entry.
    (void)headerText; (void)initial;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Sensitivity getters/setters
// ---------------------------------------------------------------------------
f32  Input::getStickSensitivity()        { return s_stickSensitivity; }
void Input::setStickSensitivity(f32 v)   { s_stickSensitivity = v; }
f32  Input::getGyroSensitivity()         { return s_gyroSensitivity; }
void Input::setGyroSensitivity(f32 v)    { s_gyroSensitivity = v; }
bool Input::getStickInvertY()            { return s_stickInvertY; }
void Input::setStickInvertY(bool v)      { s_stickInvertY = v; }
bool Input::getGyroInvertY()             { return s_gyroInvertY; }
void Input::setGyroInvertY(bool v)       { s_gyroInvertY = v; }
f32  Input::getMouseSensitivity()        { return s_mouseSensitivity; }
void Input::setMouseSensitivity(f32 v)   { s_mouseSensitivity = v; }
void Input::setActivePlayer(u8 index)    { s_activePlayer = index; }
u8   Input::getActivePlayer()            { return s_activePlayer; }
void Input::setSplitScreen(bool active)  { s_splitActive = active; }

// ---------------------------------------------------------------------------
// Action-based input — merges keyboard + mouse + gamepad
// ---------------------------------------------------------------------------

// A bare gamepad-button binding (no modifier) must yield to a chord binding that
// shares the same physical button when that chord's modifier is currently held.
// Example: "+" (START) alone opens the Inventory, but L+"+" opens the Character
// screen — without this, pressing L+"+" would fire BOTH on the same frame. L is
// reserved as a modifier-only button by design, so this only ever suppresses the
// bare action while a real chord on that button is active.
static bool chordClaimsButton(s32 padIdx, s32 button) {
    for (u32 i = 0; i < static_cast<u32>(GameAction::COUNT); ++i) {
        const InputBinding& o = s_bindings[i];
        if (o.button == button && o.modifier >= 0 && Input::isButtonDown(padIdx, o.modifier))
            return true;
    }
    return false;
}

static bool checkActionRaw(GameAction action, bool pressed) {
    u32 idx = static_cast<u32>(action);
    if (idx >= static_cast<u32>(GameAction::COUNT)) return false;
    const InputBinding& b = s_bindings[idx];
    s32 padIdx = static_cast<s32>(s_activePlayer); // which controller to read

    // Left-stick menu navigation: MENU_UP/DOWN also fire on a debounced left-stick edge so the
    // analog stick drives every menu that goes through these actions. Pressed-only (edge), and
    // safe because MENU_* actions are read exclusively in menus, never in-game.
    if (pressed) {
        if (action == GameAction::MENU_UP   && Input::isMenuStickPressed(Input::StickNav::Up))   return true;
        if (action == GameAction::MENU_DOWN && Input::isMenuStickPressed(Input::StickNav::Down)) return true;
    }

    // Keyboard + mouse only for player 0 (player 2 is controller-only in split-screen)
    if (s_activePlayer == 0) {
        if (b.key >= 0) {
            if (pressed ? Input::isKeyPressed(b.key) : Input::isKeyDown(b.key))
                return true;
        }
        if (b.mouseButton > 0) {
            if (pressed ? Input::isMouseButtonPressed(b.mouseButton)
                        : Input::isMouseButtonDown(b.mouseButton))
                return true;
        }
    }

    // Gamepad button check — routes to active player's controller
    if (b.button >= 0) {
        bool modOk = (b.modifier < 0) || Input::isButtonDown(padIdx, b.modifier);
        if (b.modifier >= 0 && !Input::isButtonDown(padIdx, b.modifier)) modOk = false;
        // Bare binding yields to an active chord on the same button (see chordClaimsButton).
        if (modOk && b.modifier < 0 && chordClaimsButton(padIdx, b.button)) modOk = false;
        if (modOk) {
            if (pressed ? Input::isButtonPressed(padIdx, b.button) : Input::isButtonDown(padIdx, b.button))
                return true;
        }
    }

    // Axis check (triggers) — routes to active player's controller
    if (b.axis >= 0 && b.axis < MAX_AXES) {
        f32 v = Input::getAxis(padIdx, b.axis);
        if (v >= b.axisThreshold) {
            if (!pressed) return true;
            // Edge detection: only fire on threshold crossing
            if (padIdx >= 0 && padIdx < Input::MAX_GAMEPADS &&
                s_previousAxisValues[padIdx][b.axis] < b.axisThreshold)
                return true;
        }
    }

    return false;
}

bool Input::isActionDown(GameAction action) {
    return checkActionRaw(action, false);
}

bool Input::isActionPressed(GameAction action) {
    return checkActionRaw(action, true);
}

// ---------------------------------------------------------------------------
// Binding management
// ---------------------------------------------------------------------------
const InputBinding& Input::getBinding(GameAction action) {
    return s_bindings[static_cast<u32>(action)];
}

void Input::setKeyBinding(GameAction action, s32 scancode) {
    s_bindings[static_cast<u32>(action)].key = scancode;
}

void Input::setButtonBinding(GameAction action, s32 button, s32 modifier) {
    s_bindings[static_cast<u32>(action)].button = button;
    s_bindings[static_cast<u32>(action)].modifier = modifier;
}

void Input::resetBindingsToDefaults() {
    setDefaults();
}

// Restore only the keyboard/mouse half of the rebindable actions (0..INVENTORY) — used by the
// "Keyboard & Mouse" options submenu's reset so it leaves controller bindings untouched.
void Input::resetKeyboardBindingsToDefaults() {
    InputBinding d[static_cast<u32>(GameAction::COUNT)];
    buildDefaults(d);
    for (u32 i = 0; i <= static_cast<u32>(GameAction::INVENTORY); i++) {
        s_bindings[i].key         = d[i].key;
        s_bindings[i].mouseButton = d[i].mouseButton;
    }
}

// Restore only the controller half of the rebindable actions (0..INVENTORY) — used by the
// "Controller" options submenu's reset so it leaves keyboard bindings untouched.
void Input::resetControllerBindingsToDefaults() {
    InputBinding d[static_cast<u32>(GameAction::COUNT)];
    buildDefaults(d);
    for (u32 i = 0; i <= static_cast<u32>(GameAction::INVENTORY); i++) {
        s_bindings[i].button        = d[i].button;
        s_bindings[i].modifier      = d[i].modifier;
        s_bindings[i].axis          = d[i].axis;
        s_bindings[i].axisThreshold = d[i].axisThreshold;
    }
}

// Save/load use a simple text format (one line per action)
void Input::saveBindings(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (u32 i = 0; i < static_cast<u32>(GameAction::COUNT); i++) {
        const InputBinding& b = s_bindings[i];
        fprintf(f, "%u %d %u %d %d %d %.2f\n",
                i, b.key, (u32)b.mouseButton, b.button, b.modifier, b.axis, b.axisThreshold);
    }
    // Sensitivity / invert-Y settings as trailing sentinel rows (see CFG_* above). The value
    // rides in the final float field; the other fields are unused padding so the line still has
    // the 7 tokens the binding reader expects (older builds read + harmlessly skip these).
    fprintf(f, "%u 0 0 -1 -1 -1 %.3f\n", CFG_STICK_SENS,   s_stickSensitivity);
    fprintf(f, "%u 0 0 -1 -1 -1 %.3f\n", CFG_GYRO_SENS,    s_gyroSensitivity);
    fprintf(f, "%u 0 0 -1 -1 -1 %.3f\n", CFG_MOUSE_SENS,   s_mouseSensitivity);
    fprintf(f, "%u 0 0 -1 -1 -1 %.3f\n", CFG_STICK_INVERT, s_stickInvertY ? 1.0f : 0.0f);
    fprintf(f, "%u 0 0 -1 -1 -1 %.3f\n", CFG_GYRO_INVERT,  s_gyroInvertY  ? 1.0f : 0.0f);
    fclose(f);
    LOG_INFO("Input: saved bindings to %s", path);
}

void Input::loadBindings(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return; // no saved bindings, use defaults
    u32 idx;
    s32 key, btn, mod, ax;
    u32 mouse;
    f32 axT;
    while (fscanf(f, "%u %d %u %d %d %d %f", &idx, &key, &mouse, &btn, &mod, &ax, &axT) == 7) {
        if (idx < static_cast<u32>(GameAction::COUNT)) {
            s_bindings[idx].key = key;
            s_bindings[idx].mouseButton = static_cast<u8>(mouse);
            s_bindings[idx].button = btn;
            s_bindings[idx].modifier = mod;
            s_bindings[idx].axis = ax;
            s_bindings[idx].axisThreshold = axT;
        } else if (idx == CFG_STICK_SENS)   { s_stickSensitivity = axT;
        } else if (idx == CFG_GYRO_SENS)    { s_gyroSensitivity  = axT;
        } else if (idx == CFG_MOUSE_SENS)   { s_mouseSensitivity = axT;
        } else if (idx == CFG_STICK_INVERT) { s_stickInvertY = (axT != 0.0f);
        } else if (idx == CFG_GYRO_INVERT)  { s_gyroInvertY  = (axT != 0.0f);
        }
        // Any other idx >= COUNT: unknown/future setting — ignore (forward-compatible).
    }
    fclose(f);
    LOG_INFO("Input: loaded bindings from %s", path);
}

// ---------------------------------------------------------------------------
// Display names
// ---------------------------------------------------------------------------
const char* Input::actionName(GameAction action) {
    switch (action) {
        case GameAction::MOVE_FORWARD:  return "Move Forward";
        case GameAction::MOVE_BACKWARD: return "Move Backward";
        case GameAction::MOVE_LEFT:     return "Move Left";
        case GameAction::MOVE_RIGHT:    return "Move Right";
        case GameAction::JUMP:          return "Jump";
        case GameAction::FIRE:          return "Attack / Fire";
        case GameAction::BLOCK:         return "Block / Shield";
        case GameAction::CLASS_SKILL:   return "Class Skill";
        case GameAction::TARGET_LOCK:   return "Target Lock";
        case GameAction::POTION:        return "Potion";
        case GameAction::PICKUP:        return "Pickup / Interact";
        case GameAction::RELOAD:        return "Reload";
        case GameAction::SKILL_1:       return "Skill 1";
        case GameAction::SKILL_2:       return "Skill 2";
        case GameAction::SKILL_3:       return "Skill 3";
        case GameAction::SKILL_4:       return "Skill 4";
        case GameAction::BOOT_SKILL:    return "Boot Skill (F)";
        case GameAction::HELMET_SKILL:  return "Helmet Skill (G)";
        case GameAction::INVENTORY:     return "Inventory";
        case GameAction::PAUSE:         return "Pause / Menu";
        case GameAction::QUICKBAR_PREV: return "Quickbar Prev";
        case GameAction::QUICKBAR_NEXT: return "Quickbar Next";
        case GameAction::MENU_UP:       return "Menu Up";
        case GameAction::MENU_DOWN:     return "Menu Down";
        case GameAction::MENU_CONFIRM:  return "Confirm";
        case GameAction::MENU_BACK:          return "Back";
        case GameAction::CHARACTER_SCREEN:   return "Character Screen";
        default: return "Unknown";
    }
}

const char* Input::buttonName(s32 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:             return "A";
        case SDL_CONTROLLER_BUTTON_B:             return "B";
        case SDL_CONTROLLER_BUTTON_X:             return "X";
        case SDL_CONTROLLER_BUTTON_Y:             return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:          return "-";
        case SDL_CONTROLLER_BUTTON_START:         return "+";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return "R3";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "L";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "D-Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "D-Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "D-Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "D-Right";
        default: return "?";
    }
}

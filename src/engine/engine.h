#pragma once

#include "core/types.h"
#include "engine/asset_manifest.h"   // mesh manifest + MESH_DEF_CAPACITY (sizes m_meshDefs)
#include "renderer/camera.h"
#include "renderer/particles.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "renderer/obj_loader.h"  // BodyRegions (armor auto-fit)
#include "world/collision.h"     // CollisionObstacle — used by buildLagCompPlayerObstacles
#include "world/level_grid.h"
#include "world/level_mesh.h"
#include "world/raycast.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/entity.h"
#include "world/spatial_grid.h"
#include "game/limb_system.h"
#include "game/weapon.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/stash.h"
#include "game/arena.h"   // PvP deathmatch rules (Arena mode, floor 97)
#include "game/combat.h"  // Combat::PvpHit/PvpHitOutcome — the arena's atomic hit apply
#include "game/interact.h"   // tap/hold interact rule (pure)
#include "game/inventory_ui.h"   // SkillBarRects — shared skill-bar geometry (HUD + inventory screen)
#include "renderer/hud.h"        // HUD::EquipSkillSlot — built by buildEquipSkillSlots
#include "game/boss_def.h"
#include "game/enemy_def.h"
#include "game/floor_event.h"
#include "game/shrine.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"    // WorldSnapshot — needed for m_baselineSnap / m_lastAppliedSnap (D7.2)
#include "net/clock_sync.h"
#include "net/lobby_code.h"  // LobbyCode::BUF_SIZE — the host's shareable lobby code buffer
#include "platform/steam.h"  // Steam::isAvailable() — gates the Public/Private lobby rows
#include "net/prediction_ring.h"
#include "net/render_offset.h"
#include "net/pending_hit_ring.h"
#include "net/pending_damage_ring.h"
#include "net/pending_pickup_ring.h"
#include "net/pending_skill_ring.h"
#include "game/squad.h"
#include "world/level_gen.h"

static constexpr u32 MAX_LEVEL_SECTIONS = 64;

// Pause-menu row geometry — ONE definition shared by the renderer (engine_hud.cpp pause overlay)
// and the mouse hit-test (engine_update.cpp pauseMenuHit), which previously each hardcoded the
// same raw-pixel numbers — the exact draw-vs-hit-test drift class the quickbar already suffered
// (see hud.h drawQuickbar). Everything scales with viewport height, the same factor FontSystem
// bakes into the labels, so the boxes grow with the text they hold instead of overflowing at
// resolutions above 720p.
struct PauseMenuLayout {
    f32 rowW, rowH;        // option box size
    f32 rowStep;           // vertical distance between row bottoms
    f32 firstRowOffset;    // row 0's bottom, relative to screen-center Y
};
inline PauseMenuLayout pauseMenuLayout(u32 sh) {
    const f32 s = static_cast<f32>(sh) / 720.0f;
    return { 250.0f * s, 28.0f * s, 35.0f * s, 10.0f * s };
}

enum struct GameState : u8 {
    MENU,
    LOBBY_HOST,
    LOBBY_JOIN,
    CONNECTING,
    IN_GAME,
    GAME_OVER,          // player died, show death screen
    FLOOR_TRANSITION,   // between-floor title card (2s hold)
    VICTORY,            // player completed floor 50 — victory screen before menu return
    CREDITS,            // scrolling credits (entered via the post-Engine exit portal or the
                        // Hell-complete ending; broadcast to clients, leads into VICTORY)
};

// How a call to Engine::startGame() should treat player progression state.
// Makes the intent explicit instead of inferring it from floor/difficulty/inventory:
//   NEW_GAME — fresh run: wipe inventory, grant the class starting loadout, reset to class HP.
//   CONTINUE — loaded from a save (loadGame already restored inventory/skills/HP): leave them alone.
//   DESCEND  — next floor or difficulty loop within a run: keep inventory & HP; floor already set.
enum struct GameStart : u8 {
    NEW_GAME,
    CONTINUE,
    DESCEND,
};

// Developer CLI launch options (launch_options.h) — forward-declared so engine.h need not pull
// in the parser header; the definition is included where applyLaunchOptions is implemented/called.
struct LaunchOptions;

// Steam lobby-browser layout. SHARED deliberately: the renderer (engine_render_menus.cpp, subState
// 20) and the mouse hit-test (engine_menu.cpp, menuMouseForState case 20) must agree exactly, and
// every mouse-driven menu in this codebase has the same standing hazard — a layout tweak in one file
// silently desyncs clicks in the other. Single-sourcing them here removes that failure mode.
static constexpr u32 STEAM_BROWSER_VISIBLE  = 8;      // rows shown at once
static constexpr f32 STEAM_BROWSER_ROW_H    = 32.0f;  // row pitch, × uiScale
static constexpr f32 STEAM_BROWSER_LIST_TOP = 0.60f;  // y of row 0's box, × screen height
static constexpr f32 STEAM_BROWSER_PANEL_W  = 0.66f;  // panel width, × screen width

class Engine {
public:
    void init();
    void shutdown();
    void run();
    // Developer launch flags (engine_launch.cpp): called once between init() and run() to boot
    // straight into a state (host/join/single + load/new), skipping the menu. Sequences the same
    // start primitives the menu uses. No/invalid options leave the game at the normal menu.
    void applyLaunchOptions(const LaunchOptions& opt);

private:
    static constexpr f64 FIXED_DT            = 1.0 / 60.0;
    static constexpr s32 MAX_STEPS_PER_FRAME = 4;

    bool m_running     = false;
    f64  m_accumulator = 0.0;
    f64  m_statsTimer  = 0.0;
    bool m_firstTick   = true; // true only on first accumulator iteration per frame
    u32  m_updateCount = 0;
    u32  m_frameCount  = 0;
    u32  m_displayFps  = 0;  // last completed second's frame count (for HUD)

    // Game state
    GameState m_gameState = GameState::MENU;

    // Difficulty — 0=Normal, 1=Nightmare, 2=Hell. Each tier adds +50 "effective floors"
    // to every enemy (incl. bosses). Enemy HP compounds off the effective floor while
    // damage stays linear + a flat per-tier bump (x1.5 NM / x2 Hell). See GameConst::
    // floorHealthMult / floorDamageMult / difficultyDamageBump.
    u8 m_difficulty        = 0;
    // Highest difficulty unlocked globally (persisted in difficulty_unlock.dat)
    u8 m_highestUnlocked   = 0;

    // Menu/UI state — grouped for isolation
    struct MenuState {
        u8   selection = 0;
        u8   subState = 0;       // 0=main, 1=singleplayer, 2=P1 class, 3=options category list,
                                 // 4=couch lobby, 5=P2 class, 6=P1 slot, 8=overwrite confirm,
                                 // 10=host mode, 11=P2 New/Continue chooser, 12=P2 slot select,
                                 // 14=free-play level select (post-clear),
                                 // 15=options:audio, 16=options:keyboard&mouse, 17=options:controller,
                                 // 18=options:display, 22=Arena Mode chooser (Host Arena / Local Versus)
        u8   subSelection = 0;
        // Free-Play (post-clear level select, sub-state 14): a cleared character's chosen difficulty
        // (0-2) and floor (1-50) for a non-destructive farming session. subSelection picks the active
        // row (0 = difficulty, 1 = floor).
        u8   freePlayDifficulty = 2;   // default Hell
        u8   freePlayFloor      = 1;   // default floor 1
        bool freePlayFromTown   = false; // select opened from the town portal: BACK returns to town
        f32  msgTimer = 0.0f;    // countdown for transient menu messages
        const char* msg = nullptr;
        // Couch co-op: each lane's New-vs-Continue intent, captured during slot selection so the
        // shared start (subState 4/5/12) calls startGame with the right world mode (CONTINUE keeps
        // P1's saved floor/seed; NEW_GAME makes a fresh world) and prepares each lane correctly.
        bool p1Continue = false;
        bool p2Continue = false;
        // Arena mode (PvP): set by the Arena chooser (subState 22), consumed by every start site
        // (class-select host start, couch-lobby solo start, startCouchGame) to route into
        // enterArena() instead of the dungeon/town. Reset by the main-menu confirm.
        bool arena = false;
        u8   overwriteLane = 0;        // which local lane (0=P1, 1=P2) the subState-8 overwrite is for
        bool couchHost = false;        // true when the host-mode chooser (10) was reached from the
                                       // couch lobby to host an ONLINE couch game (both locals + remotes)
        bool couchJoin = false;        // true when the IP-entry screen (9) was reached from the couch
                                       // lobby to JOIN an online game with two local players
        bool bindCapture = false;   // true when waiting for key/button press to rebind
        bool bindKeyboard = true;   // true=capturing keyboard, false=capturing controller
        bool confirmQuit = false;      // "are you sure?" overlay when pressing ESC in-game
        // Options was opened from the PAUSE menu, not the main menu. The options screens live in
        // GameState::MENU, so opening them mid-game means leaving IN_GAME — this flag is what makes
        // BACK come home to the paused game instead of dumping the player at the main menu with
        // their run still loaded behind it.
        bool optionsFromPause = false;
        u8   overwriteSlot = 0;        // save slot pending overwrite (subState 8)
        char connectAddress[64] = "127.0.0.1";
        // True while the IP-entry screen (subState 9) hasn't been edited yet — the first
        // digit/dot keystroke wipes the default 127.0.0.1 so the user can type fresh
        // without having to backspace nine characters first. Reset each time Join is
        // chosen from the main menu.
        bool connectAddressClearOnType = true;
        // Cursor index into the on-screen keyboard grid (MenuOsk) on the desktop Host-IP
        // screen (subState 9), for controller-only text entry. Transient UI state.
        u8   oskCursor = 0;
        // Host-mode selector (subState 10). true → ask the router for a UPnP IGD port
        // mapping at host time so friends on other networks can join; false → strictly
        // LAN-only, skip the SSDP discovery and never touch the router. Defaults to the
        // historical behavior (always-attempt UPnP) so launches that bypass subState 10
        // don't regress.
        bool hostOnline = true;
        // Steam lobby visibility chosen on the host-mode screen (subState 10). PRIVATE creates an
        // unlisted lobby: it never appears in the public browser and can only be entered by invite,
        // by a friend, or by someone typing the lobby CODE the host shares. Public is the default.
        bool hostPrivate = false;
        // Lobby-code entry (subState 21). Its own screen and its own on-screen keyboard (CodeOsk)
        // rather than sharing the Host-IP screen — the address screen only wants digits + hex + IP
        // punctuation, and stuffing 26 letters into it to serve a different screen would make typing
        // an IP worse for everyone.
        char lobbyCodeInput[LobbyCode::BUF_SIZE] = {};
        u32  codeOskCursor = 0;        // cursor into CodeOsk::KEYS (controller entry)
        bool codeNotFound = false;     // set when a lookup came back empty — shown under the field
        f32  creditsScroll = 0.0f;  // Y offset for scrolling credits text
    };
    MenuState m_menu;

    // Floor transition screen state
    struct TransitionState {
        f32  timer = 0.0f;
        u32  floorKillCount = 0;    // kills on current floor (reset in startGame)
        f32  floorTime = 0.0f;      // seconds spent on current floor
        f32  totalPlayTime = 0.0f;  // cumulative play time across all floors (saved)
        u32  snapshotKills = 0;     // snapshot for transition display
        f32  snapshotTime = 0.0f;   // snapshot for transition display
    };
    TransitionState m_transition;
    f32 m_lowHpRumbleTimer = 0.0f;  // countdown between low-HP "heartbeat" rumble nags

    // --- Split-screen state ---
    static constexpr u32 MAX_LOCAL_PLAYERS = 2;
    // (L5) Several split-screen sites assume EXACTLY two local players: the 2-way viewport
    // split (engine_render.cpp), the "the other player" partner logic (otherP = 1 - idx in
    // engine_render_world.cpp and the player-push code), and the couch co-op menu flow.
    // Raising this cap requires revisiting all of them — fail loudly at compile time.
    static_assert(MAX_LOCAL_PLAYERS == 2, "split-screen assumes exactly 2 local players (see L5)");
    u8   m_splitPlayerCount = 1;   // 1=single, 2=split-screen
    // (M2) The "currently-updated player" index is m_localPlayerIndex, set by swapInPlayer.
    // The old separate m_activePlayerIndex duplicated it and was only updated in the update
    // loop (never in render), so the two could silently disagree — collapsed into one.
    u8   m_splitMode = 0;          // 0=horizontal (top/bottom), 1=vertical (left/right)
    bool m_playerDead[MAX_LOCAL_PLAYERS] = {}; // per-player death state for split-screen
    // Online couch co-op: true when split-screen (m_splitPlayerCount==2) is INTENTIONALLY combined
    // with a net role (host-couch / client-couch). Normally split-screen and networking are mutually
    // exclusive and the dispatch guard forces count to 1 under a net role; this flag opts a genuine
    // couch-online session out of that guard so two local players can share one connection.
    bool m_netCouch = false;

    // Per-local-player state (swapped into m_localPlayer/m_camera before gameUpdate)
    Player         m_localPlayers[MAX_LOCAL_PLAYERS];
    Camera         m_cameras[MAX_LOCAL_PLAYERS];
    ViewmodelState m_viewmodelStates[MAX_LOCAL_PLAYERS];
    PlayerClass    m_playerClasses[MAX_LOCAL_PLAYERS] = {PlayerClass::WARRIOR, PlayerClass::WARRIOR};
    u8             m_activeClassSkills[MAX_LOCAL_PLAYERS] = {};
    SkillState     m_classSkillStatesPerPlayer[MAX_LOCAL_PLAYERS][4];
    SkillId        m_armorAuras[MAX_LOCAL_PLAYERS] = {};
    SkillId        m_weaponProcs[MAX_LOCAL_PLAYERS] = {};
    SkillId        m_ringPassives[MAX_LOCAL_PLAYERS] = {};
    SkillId        m_glovesPassives[MAX_LOCAL_PLAYERS] = {};
    bool           m_inventoryOpenArr[MAX_LOCAL_PLAYERS] = {};
    bool           m_characterScreenOpenArr[MAX_LOCAL_PLAYERS] = {};
    // Pre-seeded to 0.6 rad so the model isn't front-facing on first open.
    f32            m_inspectYawArr[MAX_LOCAL_PLAYERS] = {0.6f, 0.6f};
    f32            m_hitMarkerTimers[MAX_LOCAL_PLAYERS] = {};
    f32            m_potionCooldowns[MAX_LOCAL_PLAYERS] = {};
    // R17: tick at which each local player last activated a potion. Authoritative for
    // gate. 0 = never (gate passes). m_potionCooldowns above is HUD-derived from this.
    u32            m_potionLastActivationTicks[MAX_LOCAL_PLAYERS] = {};

    // Active-player aliases (gameUpdate reads/writes these, swapped per player)
    PlayerClass m_playerClass = PlayerClass::WARRIOR;
    u8          m_activeClassSkill = 0;
    SkillState  m_classSkillStates[4];
    SkillId     m_armorAura = SkillId::NONE;
    SkillId     m_weaponProc = SkillId::NONE;
    SkillId     m_ringPassive = SkillId::NONE;
    SkillId     m_glovesPassive = SkillId::NONE;  // FRENZY while legendary gloves equipped
    bool        m_inventoryOpen = false;
    // Character inspect overlay: toggled by CHARACTER_SCREEN action (C / LB+Plus).
    // Frees the mouse (like inventory) and freezes gameplay input. m_inspectYaw
    // accumulates per-tick from mouse-X drag, right-stick X, and an idle auto-spin
    // so the renderer can rotate the paper-doll model around the Y axis.
    bool        m_characterScreenOpen = false;
    f32         m_inspectYaw = 0.6f;   // inspect-model rotation in radians (initial angle)
    ViewmodelState  m_viewmodelState;

    // Death-screen mouse control. m_deathHover is the option the mouse is over on the SP
    // GAME_OVER screen (0=Respawn, 1=Reload, 2=Quit; -1 = none) so the renderer can highlight
    // it. m_deathCursorFree tracks whether we've freed the cursor for the networked-MP dead
    // overlay (edge flag — the dead-branch runs every frame, so toggle the OS cursor once).
    s8          m_deathHover = -1;
    bool        m_deathCursorFree = false;

    // Menu "last input device" gate. While keyboard/controller drives a menu-like screen (main
    // menu, options, class-select, save slots, free-play, the SP death screen, the pause menu) the
    // mouse pointer neither hovers nor clicks and the OS cursor is hidden; it re-activates only when
    // the mouse is OBVIOUSLY used (moved >= MENU_MOUSE_MOVE_PX, or a click). updateMenuMouseActive()
    // maintains m_menuMouseActive once per render frame. m_menuMouseDeltaPrimed discards the one
    // bogus mouse delta SDL emits when relative-mouse mode is switched off on entry from gameplay.
    static constexpr s32 MENU_MOUSE_MOVE_PX = 4;   // Manhattan px to count as intentional motion
    bool        m_menuMouseActive     = false;
    bool        m_menuMouseDeltaPrimed = false;

    // Gameplay input (movement/aim/fire/skills/block/dodge) is frozen while a blocking UI is
    // open — the inventory, the in-game pause menu (m_menu.confirmQuit), or one of the pause
    // menu's sub-pages (options-from-pause / menagerie, which CLEAR confirmQuit while open and
    // so need their own terms). In SP all of these early-return and freeze the whole world; in
    // MP the world keeps running for everyone, so these gates are what hold the menuing
    // player's own character still — without them a host clicking audio sliders would also be
    // walking and firing. Potion is intentionally NOT gated (matches inventory — you can
    // drink while a menu is open).
    bool gameplayInputFrozen() const {
        return m_inventoryOpen || m_characterScreenOpen || m_menu.confirmQuit
            || m_menu.optionsFromPause || m_menagerieOpen;
    }

    // Networking
    NetRole    m_netRole = NetRole::NONE;
    u8         m_localPlayerIndex = 0;
    // Net slot of each local lane as assigned by the server (SV_JOIN_ACCEPT). On a host or in
    // singleplayer/split a lane's net slot equals its lane index, so these stay {0,…} and
    // activeNetSlot() == m_localPlayerIndex. On a CLIENT the server-assigned slots (≥1) live here,
    // per lane — online couch co-op carries TWO local players over one connection, so lane 1 has its
    // own slot. activeNetSlot() indexes by m_localPlayerIndex (the swapped-in lane). Separating slot
    // from lane is required because swapInPlayer() overwrites m_localPlayerIndex every frame.
    u8         m_clientNetSlot[MAX_LOCAL_PLAYERS] = {0, 0};
    u32        m_serverTick = 0;
    // Client-local monotonic sim tick. Drives NetInput.clientTick (M1). Independent of
    // m_serverTick. On CLIENT role, read m_clockSync.currentServerTickEst for any
    // "what does the server think the time is" question.
    u32        m_clientTick = 0;
    f32        m_connectingElapsed = 0.0f; // seconds spent in CONNECTING; bails out on timeout (M10)

    // Clock-sync subsystem (CLIENT role) — see src/net/clock_sync.h. The host
    // (SERVER role) has direct access to its m_serverTick so it does not consult
    // m_clockSync.
    ClockSync m_clockSync;
    f64       m_lastPingSentSec = 0.0;
    u32       m_pingsSent       = 0;

    // M14: net diagnostics — debug knobs and counters.
    // m_netFakeLatencyMs / m_netFakeLossPct are "cvar-style" settings that net.cpp
    // reads via s_engineForNet to inject artificial loss at send time (v1: loss only).
    // m_divergenceCount accumulates every reconcile mismatch in clientNetPost and is
    // reported + reset by the 1 Hz [NET-GRAPH] log emitted in update().
    u32  m_netFakeLatencyMs  = 0;    // D5: ms of one-way simulated latency on all sends
    u32  m_netFakeJitterMs   = 0;    // --net-jitter: pushed into Net:: each frame (serverNetPre/clientNetPre)
    u8   m_netFakeLossPct    = 0;    // 0–100: percentage of packets to drop (both directions)
    u32  m_divergenceCount   = 0;    // count of reconcile mismatches since last log interval
    // Shaky-client-FOV diagnostic (accumulated per-correction in clientNetPost, reported +
    // reset by the 1 Hz [NET-GRAPH] log). Together these show whether the camera shake is
    // driven by frequent, enemy-correlated prediction divergence: divSum/divCount = mean
    // correction magnitude, divMax = worst this window, divNearEnemy = how many corrections
    // fired while the local player was brushing a moving enemy (the suspected root cause —
    // client/server obstacle-time mismatch, see buildLagCompPlayerObstacles).
    f32  m_divergenceSumM        = 0.0f; // Σ correction magnitude (m) since last log interval
    f32  m_divergenceMaxM        = 0.0f; // max single correction magnitude (m) this interval
    u32  m_divergenceNearEnemyCount = 0; // corrections that fired within brush-range of an enemy
    f64  m_lastDebugLogSec   = 0.0;  // wall-clock time of last [NET-GRAPH] emission
    // D6: toggles an on-screen net-stats overlay (F9). Visible only on CLIENT role.
    bool m_netGraphVisible   = false;

    // Screenshot / cinematic capture (F8 / F10). m_hideHud suppresses the IN_GAME HUD draws so
    // captures are clean key art; m_screenshotPending is a one-shot set by F8 and consumed just
    // before the IN_GAME swapBuffers in renderGame(). Used for Steam/marketing hero shots.
    bool m_hideHud           = false;
    bool m_screenshotPending = false;
    u32  m_screenshotSeq     = 0;    // monotonic counter for screenshot_NNNN.png filenames
    // Auto-screenshot (CLI --screenshot-interval): capture every m_shotInterval seconds while
    // IN_GAME (0 = off). m_shotTimer accumulates in-game frame time toward the next shot.
    f64  m_shotInterval      = 0.0;
    f64  m_shotTimer         = 0.0;

    // Prediction ring (CLIENT role, M3) — stores (input, predicted-state) per clientTick
    // so that clientNetPost can compare the server's authoritative pose against what we
    // predicted and snap/correct if divergence > 10 cm. Reset on every CLIENT connect.
    // PER-LANE for online couch co-op: each local player predicts/reconciles against its own
    // net slot, indexed by m_localPlayerIndex (the swapped-in lane). Single client uses [0].
    PredictionRing m_predictionRing[MAX_LOCAL_PLAYERS];
    u32            m_lastReconciledTick[MAX_LOCAL_PLAYERS] = {0, 0};

    // Smooth correction offset (CLIENT role, M4) — accumulates the visible delta when the
    // server corrects our predicted position. Decays to zero each frame so the rendered
    // camera position smoothly slides toward the corrected sim position (~150 ms window)
    // instead of teleporting. Reset on every CLIENT connect (see engine_startgame.cpp).
    // Per-lane (see m_predictionRing) — each local player's camera smooths independently.
    RenderOffset m_renderOffset[MAX_LOCAL_PLAYERS];

    // Pending predicted hits (CLIENT role, M6) — records one entry per predicted melee
    // or hitscan hit (clientTick, entitySlot). M10's SV_DAMAGE_DONE handler will ack
    // confirmed hits and trigger soft FX cleanup on mismatches. Until M10 lands, entries
    // accumulate; expireOlderThan bounds growth once M10 drives the pruning cadence.
    // Reset on every CLIENT connect (below).
    PendingHitRing m_pendingHits;

    // Pending predicted incoming damage (CLIENT role, M7) — records one entry per predicted
    // enemy-projectile hit on the local player. Visual feedback (damageFlashTimer) fires
    // immediately; HP is NOT touched locally — it follows the next snapshot's authoritative
    // value to avoid flicker on mispredicts. M10's SV_DAMAGE_TO_ME will ack confirmed events.
    // Reset on every CLIENT connect.
    PendingDamageRing m_pendingDamage;

    // Pending predicted world-item pickups (CLIENT role, M8) — records one entry per
    // CL_PICKUP_ITEM sent (clientTick, itemUid). The item is immediately hidden locally so
    // it disappears on send instead of waiting ~RTT/2 for the snapshot mirror. If the server
    // rejects the pickup, mirrorWorldItems re-activates the item on the next snapshot tick.
    // expireOlderThan() trims entries older than ~2 s (120 ticks) in clientNetPost.
    // Reset on every CLIENT connect.
    PendingPickupRing m_pendingPickups;

    // Pending predicted skill activations (CLIENT role, M9) — records one entry per successful
    // SkillSystem::tryActivate on the local player (clientTick, skillSlot). M10's SV_SKILL_RESULT
    // handler will ack confirmed activations and detect server rejections (insufficient energy /
    // cooldown not ready). Until M10 lands, entries accumulate; expireOlderThan bounds growth.
    // skillSlot: 0–3 = class skill index, 0xFE = boot skill, 0xFF = helmet skill (sentinels from
    // pending_skill_ring.h). Reset on every CLIENT connect.
    PendingSkillRing m_pendingSkills;

    // The m_players[]/snapshot slotIndex/m_renderInterp index of the ACTIVE LOCAL player.
    // Use this (not m_localPlayerIndex) for net-array access of the LOCAL player: on a client
    // m_localPlayerIndex is the lane (0) but the player lives at the server-assigned slot.
    // Per-lane split-screen arrays (m_inventories/m_skillStates/m_localPlayers/... sized by
    // lane) must keep using m_localPlayerIndex.
    u8 activeNetSlot() const { return (m_netRole == NetRole::CLIENT) ? m_clientNetSlot[m_localPlayerIndex] : m_localPlayerIndex; }
    // Returns the server's current simulation tick — used by the server-side CL_TIME_PING
    // handler so net.cpp can stamp SV_TIME_PONG without directly accessing m_serverTick.
    u32 serverTickNow() const { return m_serverTick; }

    // Last ADOPTED authoritative HP per local lane (client role only) — one leg of the
    // unpredicted-damage detector in clientNetPost (the other leg is the pre-adopt value; the
    // detector takes min() of both — see the comment there for why either alone false-fires).
    // Zero-init is safe: min(pre, 0) can never register a drop on the first snapshot.
    f32 m_lastAdoptedHp[MAX_LOCAL_PLAYERS] = {};

    // Per-client delta-compression baselines (server role only).
    // Tracks the serverTick of the last snapshot sent to each remote client so that
    // shouldSendFullSnapshot can decide whether a delta is safe for the next snapshot.
    // Index matches the player slot (slot 0 = host; that entry is unused but kept so
    // slot arithmetic stays simple). Reset on connect and on floor descent.

    // The full-tick ACK each remote client reported in its most recent NetInput.
    // ackedSnapshotTick on the wire is u16 (low bits only); we reconstruct the full
    // u32 here using the high bits of m_serverTick at the time the input is received.
    u32 m_clientAckedSnap[MAX_PLAYERS] = {};

    // Input dry-out coasting (server role): consecutive synthetic last-input repeats applied to
    // this slot while its input buffer is dry. Each coast CLAIMS its tick (advances the slot's
    // lastProcessedInputTick) so snapshots stay time-consistent and late real inputs are dropped
    // by the ordinary monotonic check. Zeroed when real input resumes, on slot deactivation, and
    // at Server::init (floor transition). Cap lives at the use site (STARVE_REPEAT_CAP).
    u8 m_starvedRepeats[MAX_PLAYERS] = {};

    // Activation-edge watermark (server role): the newest clientTick whose activation edges
    // (potion / class / boot / helm presses) have fired for this slot. Separate from
    // lastProcessedInputTick because dry-out coasting advances THAT watermark past ticks whose
    // real inputs are still in flight — when those arrive their movement is (correctly) dropped,
    // but a press riding one must still fire, exactly once. Reset with Server::init.
    u32 m_lastActivationTick[MAX_PLAYERS] = {};

    // D7.3v2 — Global ring of the last SNAP_HISTORY_DEPTH built snapshots (server role only).
    // Snapshot payloads are recipient-independent (world state + the full per-slot ack array),
    // so ONE history serves every client: when a client's input acks tick T, the server deltas
    // against history[T] if it is still in range. Replaces the per-slot m_baselineSnap copies + the
    // exact-match BaselineTracker, which required ack == last-sent-tick — permanently false at
    // any real RTT, so deltas never engaged.
    //
    // Depth 64 = 1.07 s of ack latitude before a client falls back to full snapshots. At 300+ ms
    // RTT the freshest client ack is ~18-28 ticks old (RTT + jitter + input cadence), and a measured
    // DE<->NZ soak showed baseline ages of 21-38 ticks — off the edge of the old 32-ring, forcing
    // 12-25% of snapshots into full-snapshot fallback (~2x bandwidth). 64 restores comfortable margin;
    // cost is memory only (~33 KB per slot on PC — the ~8.2 KB stack struct plus its heap projectile
    // array; ~20 KB on Switch with the smaller pool). MUST deepen in lockstep with the client's
    // SNAP_BUFFER_SIZE: a baseline the client has already evicted is an undecodable delta, so the
    // two rings staying equal is what lets any baseline the server names still be decoded.
    static constexpr u32 SNAP_HISTORY_DEPTH = 64;
    WorldSnapshot m_snapHistory[SNAP_HISTORY_DEPTH];
    u32           m_snapHistoryHead = 0;   // next write slot
    u32           m_snapHistoryCount = 0;  // valid entries

    // D7.2 — Last successfully applied snapshot baseline (client role only).
    // After Client::receiveSnapshot succeeds, the engine copies the deserialized
    // WorldSnapshot here.  D7.3 reads it to reconstruct unchanged slots from a
    // delta packet (i.e. slots whose bit is not set in changedBits are copied from
    // this baseline rather than left as stale zeros).
    WorldSnapshot m_lastAppliedSnap;

    // Players (networked)
    NetPlayer  m_players[MAX_PLAYERS];
    WeaponDef  m_weaponDefs[MAX_WEAPON_DEFS];
    u32        m_weaponDefCount = 0;

    // Boss definitions (loaded from bosses.json)
    BossDefTable m_bossDefs;

    // Enemy definitions (loaded from enemies.json — replaces kTier* inline arrays)
    EnemyDefTable m_enemyDefs;

    // Item/loot system
    ItemDef    m_itemDefs[MAX_ITEM_DEFS];
    u32        m_itemDefCount = 0;
    AffixDef   m_affixDefs[MAX_AFFIX_DEFS];
    u32        m_affixDefCount = 0;
    SkillDef   m_skillDefs[MAX_SKILL_DEFS];
    u32        m_skillDefCount = 0;
    PlayerInventory m_inventories[MAX_PLAYERS];
    SkillState      m_skillStates[MAX_PLAYERS];
    SkillState      m_bootSkillStates[MAX_PLAYERS];
    SkillState      m_helmetSkillStates[MAX_PLAYERS];
    // Per-tick coalesced energy grants for REMOTE guests (SV_ENERGY_GAIN). grantEnergy()
    // accumulates here; serverNetPost flushes one reliable packet per guest then zeroes it.
    // Transient scratch — never serialized.
    f32             m_pendingEnergyGain[MAX_PLAYERS] = {};
    // R9: per-net-slot class-skill state, server-side only. Used to give remote
    // clients persistent cooldown timers so SkillSystem::tryActivate gates spam
    // requests; the host's own slot is unused here (host's class skills live in
    // m_classSkillStates / m_classSkillStatesPerPlayer above and are ticked in
    // tickSkillCooldowns/gameUpdate). Server ticks remote slots in serverNetPre.
    SkillState      m_classSkillStatesNet[MAX_PLAYERS][4];
    WorldItemPool   m_worldItems;
    QuickbarState   m_quickbars[MAX_PLAYERS];
    Mesh            m_handMesh;

    // Active-player aliases (set before gameUpdate, read back after)
    Player     m_localPlayer;
    Camera     m_camera;
    f32        m_hitMarkerTimer = 0.0f;
    f32        m_potionCooldown = 0.0f;
    // R17: tick at which the active local player last activated a potion. Source-of-
    // truth gate; m_potionCooldown is HUD-derived. Swap-victim alongside m_potionCooldown.
    u32        m_potionLastActivationTick = 0;
    Shader  m_basicShader;
    Shader  m_unlitShader;
    Shader  m_vignetteShader;  // fullscreen radial red damage vignette (BioShock-style)
    Shader  m_dimShader;       // flat fullscreen scrim (in-game options overlay); see dim.frag
    Shader  m_particleShader;  // batched per-vertex-color shader for billboard particles
    Mesh    m_cubeMesh;
    Mesh    m_quadMesh;   // flat quad for billboard sprites

    // Mesh registry for entities (MeshDef struct defined in limb_system.h). The mesh table lives in
    // engine/asset_manifest.h, which static_asserts that it fits here — an overflow would silently
    // drop the TAIL of the table (the player meshes), so the capacity and the table must not drift.
    static constexpr u32 MAX_MESH_DEFS = MESH_DEF_CAPACITY;
    MeshDef  m_meshDefs[MAX_MESH_DEFS] = {};
    u32      m_meshDefCount = 0;
    // Per-body-mesh region boxes (head/torso/feet/hands) for armor auto-fit, keyed by mesh id.
    // Only player body meshes have .valid=true; filled at load in engine_init_assets.
    BodyRegions m_bodyRegions[MAX_MESH_DEFS] = {};

    // Level, dungeon, and world state
    struct LevelState {
        LevelGrid    grid;
        LevelSection sections[MAX_LEVEL_SECTIONS];
        u32          sectionCount = 0;
        u32          levelSeed    = 42;
        u32          currentFloor = 1;
        u32          savedFloor   = 1;
        u32          savedSeed    = 0;
        DungeonResult dungeon;
        // Which structural generator carved this floor (seed-derived in startGame). Gameplay may
        // compensate for a style's geometry — e.g. CAVERN floors scale enemy detectionRange up,
        // because a 14 m aggro bubble that corridor walls used to hide reads as "passive" when
        // the player can watch the enemy from 35 m across an open cave.
        LevelGen::LayoutStyle layoutStyle = LevelGen::LayoutStyle::BSP_ROOMS;
        SquadPool     squads;
        Vec3          floorDoorPos;
        bool          floorDoorActive = false;
        bool          floorHasBoss    = false; // this floor spawned a milestone boss (gates the exit lock)

        // The Source — the secret superboss chamber. None of these are serialized (LevelState is
        // rebuilt from seed on load), so they cost no save-format change. inSourceChamber routes the
        // game out of the normal floor flow: no descent, no floor boss, Engine-death → victory.
        bool          inSourceChamber    = false; // we are inside The Source fighting the Engine
        // The post-Engine TOWN hub (sentinel floor 98). inTown flips the sky/lighting, gates the
        // NPCs' stay-home AI, and routes saves to keep the CLEARED header floor (never 98).
        bool          inTown             = false;
        // The PvP ARENA (Arena mode, sentinel floor 97, engine_arena.cpp). inArena shares the
        // town's daylight rendering, gates ALL PvP damage (Combat::pvpActive), and firewalls
        // progression: no XP, no loot, no drops, no saves. Like inTown, never serialized.
        bool          inArena            = false;
        bool          townPortalActive   = false; // the town's to-dungeon portal (opens Free-Play select)
        Vec3          townPortalPos;
        bool          sourcePortalActive = false; // the second hidden portal is live in the floor-50 arena
        Vec3          sourcePortalPos;            // its world position (valid while sourcePortalActive)
        // The way OUT — spawned when the Dungeon Engine superboss dies. Unlike the (hidden,
        // host-only) Source portal above, this one is replicated to clients via
        // SV_EVENT::EXIT_PORTAL so everyone can see it and walk in; entering rolls the credits.
        bool          exitPortalActive = false;
        Vec3          exitPortalPos;              // valid while exitPortalActive
    };
    LevelState m_level;

    // --- Arena mode (PvP deathmatch, engine_arena.cpp) -----------------------------------
    // Authoritative on the host/SP; clients mirror score + match-end via ARENA_* events.
    Arena::Score m_arenaScore;                      // kills per net slot
    f32          m_arenaRespawn[MAX_PLAYERS] = {};  // >0 = that slot is dead, counting down to auto-respawn
    f32          m_arenaOverTimer = 0.0f;           // >0 = match decided, winner banner running
    u8           m_arenaWinner    = 0xFF;           // valid while m_arenaOverTimer > 0
    Player       m_pvpViews[MAX_PLAYERS];           // seeded remote views held open for the PvP window
    // Non-null ONLY during the shared AI/projectile pass in tickSharedSystems: slot -> the throwaway
    // remote view (remoteViews[]) that applyRemotePlayerViews will write back once at pass end. While
    // set, pvpApplyHit composes a remote PvP hit onto THIS shared view instead of writing the
    // NetPlayer directly — otherwise the pre-pass-seeded view's write-back silently erases the PvP
    // damage (chakrams/projectiles/AoE "unreliable in PvP"). Set right after buildRemotePlayerViews,
    // cleared right after applyRemotePlayerViews. See pvpApplyHit / tickSharedSystems.
    Player*      m_sharedRemoteView[MAX_PLAYERS] = {};
    // Kill feed (HUD): newest first, ttl-faded. Fed by arenaHandleDeath (authority) and the
    // ARENA_KILL event (clients).
    static constexpr u32 ARENA_FEED_LINES = 4;
    struct ArenaFeedEntry { u8 killer = 0xFF; u8 victim = 0xFF; f32 ttl = 0.0f; };
    ArenaFeedEntry m_arenaFeed[ARENA_FEED_LINES];

    // Entities + projectiles (authoritative on server/singleplayer)
    EntityPool     m_entities;
    ProjectilePool m_projectiles;
    SpatialGrid    m_spatialGrid;  // rebuilt each tick for fast entity proximity queries

    // Per-room point lights (placed at level gen, nearest 4 sent to shader per frame)
    static constexpr u32 MAX_POINT_LIGHTS = 64;
    struct PointLight { Vec3 position; Vec3 color; };
    PointLight m_pointLights[MAX_POINT_LIGHTS];
    u32 m_pointLightCount = 0;

    // Dynamic lights — brief weapon flash effects (muzzle flashes, melee swings)
    static constexpr u32 MAX_DYNAMIC_LIGHTS = 4;
    struct DynamicLight { Vec3 position; Vec3 color; f32 timer; };
    DynamicLight m_dynamicLights[MAX_DYNAMIC_LIGHTS] = {};
    void spawnDynamicLight(Vec3 pos, Vec3 color, f32 duration);

    // NPC equipment pool — persistent across floor transitions for surviving NPCs
    NpcEquipment   m_npcEquip[MAX_NPC_EQUIP] = {};

    // Spawn a friendly NPC with class-appropriate equipment, returns entity handle
    EntityHandle spawnFriendlyNpc(Vec3 pos, NpcClass npcClass, u8 floor);
    // Roll class-appropriate starting equipment for an NPC at the given floor level
    void rollNpcEquipment(NpcEquipment& equip, NpcClass npcClass, u8 floor);
    // Recalculate NPC stats from equipment (mirrors Inventory::recalculateStats)
    void applyNpcEquipmentStats(Entity& e, const NpcEquipment& equip);
    // Upgrade equipment for NPCs that survived the floor
    void upgradeNpcEquipment(u8 newFloor);

    // Client interpolation state — read-only from gameplay, written by net code
    struct RenderInterp {
        EntityPool     entities;
        ProjectilePool projectiles;
        Vec3           playerPositions[MAX_PLAYERS];
        f32            playerYaws[MAX_PLAYERS];
        bool           playerActive[MAX_PLAYERS];
        f32            playerHealth[MAX_PLAYERS];
        f32            playerMaxHealth[MAX_PLAYERS];
        u8             playerAnimFlags[MAX_PLAYERS]; // bit0=attacking, bit1=reloading, bit2=dead
        u8             playerWeaponMeshId[MAX_PLAYERS]; // equipped weapon mesh (wire; clients lack remote inventories)
        // Equipped armor tier-mesh ids per player slot — [slot][k] where k: 0=helmet,
        // 1=chest, 2=boots, 3=gloves. 0 = empty. Populated from SnapPlayer.armorMeshId
        // each snapshot so clients render remote players' armor without holding inventories.
        u8             playerArmorMeshId[MAX_PLAYERS][4];
        // PlayerClass (cast to u8) of each remote player — populated from SnapPlayer.playerClass
        // each snapshot. Without this the renderer can't pick the per-class mesh for a remote
        // (the client's NetPlayer.playerClass for non-local slots is never set).
        u8             playerClass[MAX_PLAYERS];
        u8             playerDodgeFlags[MAX_PLAYERS]; // bit0=rolling (SnapPlayer.dodgeFlags) — drives the remote roll-tumble render
        // Remote-roll TUMBLE DIRECTION + air-dodge gate, forwarded from the (already-wired) SnapPlayer
        // fields — no new wire data. During a roll velocity == ROLL_SPEED*rollDirection, so the XZ
        // wire velocity recovers the dodge direction the client lacks (rollDirection isn't on the
        // wire). onGround (flags bit1) gates the takeoff dust so an air-dodge kicks up nothing.
        Vec3           playerVelXZ[MAX_PLAYERS];    // wire velocity XZ (y unused) — normalized to the roll axis
        bool           playerOnGround[MAX_PLAYERS]; // SnapPlayer.flags bit1 — grounded gate for dust
    };
    RenderInterp m_renderInterp;

    // Dodge-roll body-tumble render state (see engine_render_world.cpp). Local-only visual — the
    // roll itself replicates via SnapPlayer.dodgeFlags. m_rollAnimTimer drives progress for CLIENT
    // remotes (which receive only the rolling BIT, not the 0.5 s countdown); host/split-screen
    // derive progress from the real rollTimer. m_wasRolling gates the one-shot takeoff dust puff.
    f32  m_rollAnimTimer[MAX_PLAYERS] = {};   // 0..0.5 s, advances while a client-remote roll bit is held
    bool m_wasRolling[MAX_PLAYERS]    = {};   // previous-frame rolling state, indexed by NET SLOT (network-remote loop)
    // Separate rising-edge tracker for the split-screen partner, indexed by LOCAL lane (0/1). Kept
    // distinct from m_wasRolling[] because the two draw paths index different spaces: the network
    // loop uses NET SLOT, the split block uses local lane. In a client-couch (a couch pair joins a
    // host) the host-remote lands on net slot 0 AND the local partner is local lane 0 — sharing one
    // array would make them fight over the same cell and misfire the dust edge. Two arrays, no clash.
    bool m_wasRollingLocal[MAX_LOCAL_PLAYERS] = {}; // previous-frame rolling state of the split-screen partner

    // Combat feedback
    CombatHit   m_lastCombatHit;

    // Inventory UI state
    InventoryDragState m_dragState;        // active alias (swapped per player)
    DoubleClickState   m_dblClickState;    // active alias (swapped per player)
    // (L2) Per-player backing store so each split player has independent drag/double-click
    // state (only P0 uses the mouse today, but this stops P1's UI from sharing P0's drag).
    InventoryDragState m_dragStates[MAX_LOCAL_PLAYERS] = {};
    DoubleClickState   m_dblClickStates[MAX_LOCAL_PLAYERS] = {};
    u8  m_invCursorPanel = 0;  // active alias (swapped per player)
    u8  m_invCursorIndex = 0;  // active alias
    u8  m_invCursorPanels[MAX_LOCAL_PLAYERS] = {};
    u8  m_invCursorIndices[MAX_LOCAL_PLAYERS] = {};
    // Inventory input mode for the keyboard+mouse lane (player 0). The cursor (WASD/E and the D-pad)
    // and the physical mouse are BOTH live at once — last-input-wins picks which drives the
    // highlight + tooltip: true after a WASD/E/D-pad nav, false after the mouse moves or clicks. A
    // gamepad-only split lane (player > 0) has no mouse, so inventoryUsesCursor() forces cursor there.
    // PER-LANE (swapped) — it used to be a single shared bool, so in couch co-op P2's D-pad nav
    // (which sets it true) flipped P1 into cursor mode and skipped P1's mouse path, killing P1's
    // double-click-to-equip whenever the other player touched a controller.
    bool m_invCursorActive = false;                         // active alias (swapped per player)
    bool m_invCursorActiveArr[MAX_LOCAL_PLAYERS] = {};
    s32  m_invLastMouseX = -1;   // previous-frame absolute mouse pos; a change flips back to mouse mode
    s32  m_invLastMouseY = -1;
    f32 m_fullBackpackNotifyTimer = 0.0f;
    f32 m_bossLockNotifyTimer = 0.0f;  // "defeat the boss" prompt when descend is blocked
    bool m_firstPickupTooltipShown = false;  // item picked up → show "Open Inventory"
    bool m_inventoryOpenedOnce    = false;  // dismiss "Open Inventory" once opened
    bool m_equipTooltipShown = false;       // inventory opened → show "equip" hint
    bool m_itemEquippedOnce  = false;       // dismiss equip hint once an item is equipped
    bool m_shieldBlockedOnce = false;       // dismiss shield tutorial once player blocks
    bool m_dodgeRolledOnce   = false;       // dismiss dodge tutorial once player dodges
    f32  m_controlsTooltipTimer = 0.0f;     // LMB/RMB controls shown on floor 1 entry
    f32  m_tutorialPulseTimer   = 0.0f;     // shared pulse timer for tutorial tooltips
    f32  m_spawnCalmTimer       = 0.0f;     // >0 = floor-start calm window: no enemy auto-aggro, NPCs hold

    // Chat log — displays NPC speech and game events on the left side of the screen
    static constexpr u32 MAX_CHAT_LINES = 8;
    static constexpr u32 CHAT_LINE_LEN = 48;
    struct ChatLine { char text[CHAT_LINE_LEN]; Vec3 color; f32 timer; };
    ChatLine m_chatLog[MAX_CHAT_LINES] = {};
    void addChatMessage(const char* speaker, const char* msg, Vec3 color);

    // Visual effects pools — grouped for isolation
    static constexpr u32 MAX_IMPACT_FX = 8;
    struct ImpactFX { Vec3 pos; Vec3 normal; f32 timer; bool active; bool isEntity; };
    static constexpr u32 MAX_FIRE_FX = 8;
    struct FireFX { Vec3 pos; f32 radius; f32 timer; bool active; };
    static constexpr u32 MAX_NOVA_FX = 4;
    struct NovaFX { Vec3 pos; f32 maxRadius; f32 timer; bool active; Vec3 color; };
    static constexpr u32 MAX_DASH_FX = 4;
    struct DashFX { Vec3 start; Vec3 end; f32 timer; bool active; };
    static constexpr u32 MAX_BEAM_FX = 8;
    struct BeamFX { Vec3 start; Vec3 end; Vec3 color; f32 timer; bool active; };
    // Melee swing slash arc — a diagonal slash drawn in CAMERA space (anchored a fixed depth
    // in front of the camera, centred on the crosshair) so it always occupies the same
    // impactful spot regardless of aim, like a viewmodel element. scale sizes it by weapon
    // arc; ownerLane restricts it to the swinging player's own split-screen viewport.
    static constexpr u32 MAX_SWING_FX = 4;
    // style = WeaponSubtype of the swing, so the renderer can shape the arc to match the
    // weapon's viewmodel motion (sword diagonal, claymore horizontal, axe overhead, dagger stab).
    struct SwingFX { Vec3 color; f32 scale; u8 ownerLane; u8 style; f32 timer; bool active; };
    static constexpr u32 MAX_CHAIN_FX = 4;
    static constexpr u32 MAX_CHAIN_POINTS = 24;
    struct ChainFX { Vec3 points[MAX_CHAIN_POINTS]; u8 pointCount; f32 timer; bool active; };
    static constexpr u32 MAX_SCORCH = 4;
    // slowPct > 0 makes the zone ALSO a slow field (Ranger Barrage): each tick it slows enemies
    // (and, in PvP, rival players — ownerSlot excluded) inside the radius. dps 0 + slowPct set = a
    // pure slow zone. ownerSlot (0xFF = none) is the PvP caster, excluded from the slow. FX-only
    // struct (never serialized), so the two extra fields cost nothing on the wire/save.
    struct ScorchZone { Vec3 pos; f32 radius; f32 timer; f32 dps; f32 slowPct; u8 ownerSlot; bool active; };
    static constexpr u32 MAX_DAMAGE_NUMBERS = 16;
    struct DamageNumber { Vec3 position; f32 amount; f32 timer; bool active; bool isHeal; bool isCrit; };

    struct EffectsState {
        ImpactFX      impactFX[MAX_IMPACT_FX] = {};
        FireFX        fireFX[MAX_FIRE_FX] = {};
        NovaFX        novaFX[MAX_NOVA_FX] = {};
        DashFX        dashFX[MAX_DASH_FX] = {};
        BeamFX        beamFX[MAX_BEAM_FX] = {};
        SwingFX       swingFX[MAX_SWING_FX] = {};
        ChainFX       chainFX[MAX_CHAIN_FX] = {};
        ScorchZone    scorchZones[MAX_SCORCH] = {};
        DamageNumber  damageNumbers[MAX_DAMAGE_NUMBERS] = {};
    };
    EffectsState m_fx;

    // Particle system — pooled per-frame visual FX (blood, sparks, magic, smoke)
    ParticlePool m_particles;
    u8 m_particleBlobMatId  = 0;   // material ID for billboard smoke/magic blobs
    u8 m_particleSparkMatId = 0;   // material ID for geometric spark cubes

    void spawnDamageNumber(Vec3 pos, f32 amount, bool isHeal = false, bool isCrit = false);
    // Grant energy ("mana") to a player slot from a host-authoritative source (projectile
    // manasteal / mana-on-kill). Local lanes apply immediately; a remote guest's gain is
    // coalesced into m_pendingEnergyGain and shipped as SV_ENERGY_GAIN in serverNetPost.
    void grantEnergy(u8 slot, f32 amount);
    void renderDamageNumbers(u32 sw, u32 sh);
    // Spawn the projectile AoE splash VFX (floor-snapped fire FX + explosion particles +
    // camera shake) at an impact point. Shared by the host's splash callback and the CLIENT's
    // PROJECTILE_SPLASH SV_EVENT handler so both render identical splashes (the client's
    // ProjectileSystem::update is gated off, so it can't fire the callback itself).
    // Rows on the host-mode chooser (subState 10). With Steam, Online splits into Public / Private
    // (unlisted, code-only); without Steam there's no lobby at all (ENet + UPnP), so the split would
    // be meaningless. Single-sourced: the render, the input bounds and the mouse hit-test must agree.
    u32 hostModeOptionCount() const { return Steam::isAvailable() ? 3u : 2u; }

    void spawnSplashFX(Vec3 position, f32 radius);

    // --- Weapon on-hit PROC meteors: each player predicts their OWN, then tells the server ---
    // The proc roll rides a local std::rand() the other side can't reproduce, so the FIRING player
    // owns the roll. See CL_METEOR / SV_EVENT::METEOR in net.h for the full model.
    //
    // predictProcMeteor: called at the local player's proc site (melee/hitscan hit, or a predicted
    // projectile impact). CLIENT → spawn a predicted visual meteor NOW (instant telegraph) + send
    // CL_METEOR so the server spawns the one authoritative damaging meteor. SERVER (host) → spawn
    // the real one + relay to all clients. Singleplayer → just spawn it.
    void predictProcMeteor(Vec3 position, f32 damage, f32 radius, f32 delay);
    // Client → server: "I predicted a proc meteor here." Reliable. No-op unless CLIENT.
    void sendMeteorRequest(Vec3 position, f32 radius, f32 delay, f32 damage);
    // Server: relay a meteor to clients (SV_EVENT::METEOR). exceptSlot = the predicting caster to
    // skip (0xFF = send to everyone). No-op unless SERVER.
    void broadcastMeteorEvent(Vec3 position, f32 radius, f32 delay, u8 exceptSlot = 0xFF);

    // Blood Nova ARMOR aura (Demonhide Cuirass): 20% of CURRENT health per retaliation — the same
    // fraction skills.json charges the active cast (healthCostPct), so wearing the nova costs what
    // casting it costs. A fraction of CURRENT (not max) health, so the sacrifice decays
    // asymptotically and, with the floor guard in detonateBloodNova, can never itself be lethal.
    // Paired with the ~0.1 s re-arm below, this is the item's real limiter: it bleeds you hard and
    // fast while you are being hit, and that is the intended trade.
    static constexpr f32 BLOOD_NOVA_ARMOR_COST_PCT = 0.20f;
    // Retaliation cooldown — deliberately NOT SkillDef.cooldown (5 s, which still gates the active
    // cast). At 0.1 s this is barely a cooldown at all: it exists only to stop a single frame's
    // multi-hit (a swarm landing several blows at once, or a multi-hit AoE) from stacking several
    // novas in one instant. In practice the wearer erupts on essentially every hit taken, and the
    // 5%-of-current-health cost per eruption is what actually limits it.
    static constexpr f32 BLOOD_NOVA_ARMOR_COOLDOWN_SEC = 0.1f;

    // Spawn a nova ring locally; on the SERVER also replicate it (SV_EVENT / NOVA_FX) so the ring
    // is visible to the guest whose armor fired it — it cannot predict a nova triggered by a melee
    // hit it never observed. Safe to call from the CLIENT event handler (broadcast is SERVER-gated).
    void emitNovaFX(Vec3 position, f32 radius, Vec3 color);

    // Blood Nova fired from EQUIPMENT: Demonhide Cuirass (struck) and Aegis of Blood (perfect
    // block). The CALLER owns the trigger condition; this owns the health cost, cooldown, damage
    // and ring. health/cooldown are by reference because Player and NetPlayer share no base type.
    // SERVER/SP only (a CLIENT's entity pool is the N4 ghost sim). True = it detonated.
    bool detonateBloodNova(Vec3 origin, u8 ownerSlot, f32& health, f32& cooldown);
    // Legendary armor/shield procs (2026-07-16 batch; both server/SP-only — N5):
    // full chain lightning at whoever hit the wearer (Capacitor Mail discharge; Thunderwall's
    // perfect-block riposte reuses it), and the Hemophage Shroud's 4m life-drain aura tick.
    void staticDischarge(Vec3 pos, u8 wearerSlot, u16 attackerIdx);
    // Stamp the caster's gear spell damage (SPELL_DAMAGE_FLAT/PCT affixes) into the SkillSystem
    // scaling statics for the cast that follows. baseMult = the pre-gear multiplier (class/floor
    // scale, or 1.0 for item/proc skills); the gear % folds INTO it so every damage site — the
    // frozen orb's stored multiplier included — scales with zero per-skill edits.
    void applySpellScaling(const PlayerInventory& inv, f32 baseMult, f32 shrinePct = 0.0f);
    void hemophageAuraTick(Vec3 pos, u8 wearerSlot, f32& tickTimer, f32& health, f32 maxHealth, f32 dt);
    // Server-side CL_METEOR handler: validate + spawn the authoritative meteor for `slot`, then
    // relay it to the other clients.
    static void onMeteor(u8 playerSlot, const u8* data, u32 size);
    void handleMeteorRequest(u8 playerSlot, const u8* data, u32 size);

    // Pre-cached mesh IDs (resolved once in init, avoids strcmp in startGame)
    enum MeshId : u8 {
        MESH_SKELETON, MESH_BAT, MESH_SPIDER, MESH_CHEST, MESH_HUMAN,
        MESH_SWORD, MESH_DAGGER, MESH_AXE, MESH_MACE, MESH_BOW, MESH_STAFF,
        MESH_THROWING_KNIFE, MESH_CLERIC, MESH_ARCHER, MESH_MAGE, MESH_ROGUE,
        MESH_PALADIN, MESH_BUTCHER, MESH_CLEAVER, MESH_IRON_MAIDEN,
        MESH_ID_COUNT
    };
    u8 m_meshIds[MESH_ID_COUNT] = {};

    // Convenience accessors (keep existing code compiling with minimal changes)
    u8& m_meshIdSkeleton      = m_meshIds[MESH_SKELETON];
    u8& m_meshIdBat           = m_meshIds[MESH_BAT];
    u8& m_meshIdSpider        = m_meshIds[MESH_SPIDER];
    u8& m_meshIdChest         = m_meshIds[MESH_CHEST];
    u8& m_meshIdHuman         = m_meshIds[MESH_HUMAN];
    u8& m_meshIdSword         = m_meshIds[MESH_SWORD];
    u8& m_meshIdDagger        = m_meshIds[MESH_DAGGER];
    u8& m_meshIdAxe           = m_meshIds[MESH_AXE];
    u8& m_meshIdMace          = m_meshIds[MESH_MACE];
    u8& m_meshIdBow           = m_meshIds[MESH_BOW];
    u8& m_meshIdStaff         = m_meshIds[MESH_STAFF];
    u8& m_meshIdThrowingKnife = m_meshIds[MESH_THROWING_KNIFE];
    u8& m_meshIdCleric        = m_meshIds[MESH_CLERIC];
    u8& m_meshIdArcher        = m_meshIds[MESH_ARCHER];
    u8& m_meshIdMage          = m_meshIds[MESH_MAGE];
    u8& m_meshIdRogue         = m_meshIds[MESH_ROGUE];
    u8& m_meshIdPaladin       = m_meshIds[MESH_PALADIN];
    u8& m_meshIdButcher       = m_meshIds[MESH_BUTCHER];
    u8& m_meshIdCleaver       = m_meshIds[MESH_CLEAVER];
    u8& m_meshIdIronMaiden    = m_meshIds[MESH_IRON_MAIDEN];

    // Cached IDs for render-loop lookups (avoid per-frame string searches)
    u8 m_meshIdArrow    = 0;
    u8 m_meshIdBolt     = 0;
    u8 m_meshIdShard    = 0;  // source-shard world-item pickup (secret superboss key)
    u8 m_matIdBatWing   = 0;

    // Internal render-scale: the 3D scene renders to an offscreen FBO at this fraction of the window
    // resolution, then is upscaled (linear blit) to fill the screen — cuts fragment fill/overdraw on
    // the fill-bound Switch GPU while still filling the display. 1.0 = native (FBO bypassed). Cycled
    // by F6 (desktop) / L+R3 (Switch). FBO objects below are created lazily in ensureFbo().
#ifdef __SWITCH__
    f32 m_renderScale = 0.65f; // confirmed: holds a steady 60 fps in handheld with a crisp native HUD
#else
    f32 m_renderScale = 1.0f;
#endif
    u32 m_sceneFbo = 0, m_sceneColorTex = 0, m_sceneDepthRbo = 0, m_sceneFboW = 0, m_sceneFboH = 0;
    // Character-inspect offscreen render target: the player model + equipment is drawn 3D into this
    // FBO each frame the screen is open, then composited as a 2D panel by renderCharacterInspect.
    // All five FBO objects + the two quad GL objects below are created lazily in ensureFbo() /
    // drawTexturedQuad() and freed explicitly in Engine::shutdown().
    u32 m_inspectFbo = 0, m_inspectColorTex = 0, m_inspectDepthRbo = 0, m_inspectFboW = 0, m_inspectFboH = 0;
    u32 m_inspectQuadVao = 0, m_inspectQuadVbo = 0; // unit quad for compositing the FBO into screen-space

    static constexpr f32 SWITCH_FAR_PLANE     = 60.0f;
    static constexpr u32 SWITCH_MAX_ENTITIES  = 64;
    static constexpr u32 SWITCH_RES_W         = 1280;
    static constexpr u32 SWITCH_RES_H         = 720;

    // Split-screen player swap helpers
    void swapInPlayer(u8 idx);   // copy per-player arrays → active aliases
    void swapOutPlayer(u8 idx);  // copy active aliases → per-player arrays
    void positionLocalPlayersAtSpawn(); // (M5) place the couch-co-op pair at the current spawn
                                        // (P0 from the active alias, P1 beside it) — one helper
                                        // for the floor-transition / continue-load / new-run paths
    // Begin a 2-player couch game once both lanes are prepared by the menu (P2 loaded or freshly
    // equipped). Preps a fresh P1 lane if needed, sets split count = 2, then starts the world on
    // Player 1's floor (CONTINUE if P1 continued, else NEW_GAME) with lanes already prepared.
    void startCouchGame();
    // Online couch co-op JOIN: prep a fresh Player-1 lane if needed, advertise both classes +
    // localCount=2, and connect to m_menu.connectAddress (→ CONNECTING). Both lanes already have
    // characters from the couch lobby. Called from the IP-entry confirm (desktop + Switch swkbd).
    void beginCouchJoin();

    // Core update paths
    void update(f32 dt);
    void gameUpdate(f32 dt);        // unified gameplay — all roles call this
    void tickSharedSystems(f32 dt); // ONCE/frame after the per-player loop: AI, projectiles,
                                    // entity timers, world items, shared FX, meteors, particles (M3)
    void serverNetPre(f32 dt);      // server: process remote inputs before gameplay
    void serverNetPost(f32 dt);     // server: status ticks + snapshot broadcast
    void clientNetPre(f32 dt);      // client: predict + reconcile before gameplay
    void clientNetPost(f32 dt);     // client: interpolate remote state after gameplay

    // R17 — local-player tick reference for tick-based gates (e.g. SkillSystem::
    // tryActivate, potion gate). Returns m_clientTick on CLIENT, m_serverTick on
    // HOST/SP. For server-side processing of remote-client INPUT_EX_X, callers pass
    // input->clientTick directly (that's the remote client's frame, matching what
    // the client used locally for its own gate — divergence-free by construction).
    u32 currentLocalTick() const { return (m_netRole == NetRole::CLIENT) ? m_clientTick : m_serverTick; }

    // R17 — populate per-slot lastActivationTick fields in the snapshot built by
    // Server::buildSnapshotOnly. Pulls from Engine's skill-state arrays (which
    // Server can't see directly). Called from serverNetPost between buildSnapshotOnly
    // and the per-slot send loop. Belt-and-suspenders: the tick gate already
    // produces by-construction agreement; shipping these values lets reconcile
    // catch any edge case (rejected activation, lost packet) where client's
    // local state drifted from server's view.
    void populateSnapshotCooldowns();

    // R17 — CLIENT side of the belt-and-suspenders sync. Reads the latest snapshot
    // and adopts MAX(local, snapshot) for every lastActivationTick. MAX (not
    // direct overwrite) preserves any client over-prediction: if server rejected
    // a press for non-cooldown reasons, server's tick is older → local stays
    // (client over-gates briefly, never under-gates). Called from clientNetPre
    // after Client::reconcile so the cooldown adoption sees the same snapshot
    // reconcile used for HP/clip/etc.
    void adoptSnapshotCooldowns();

    // Shared logic
    void handleWeaponFireForPlayer(NetPlayer& np, f32 dt);
    // Throwing-knife aim assist: bends the throw (≤12°) toward the intercept point of a
    // target already near the crosshair (7° cone). Shared by the local and remote fire
    // paths; knives only — see game/lead_assist.h for the full rationale.
    void applyKnifeLeadAssist(const Vec3& spawnPos, Vec3& dir, f32 projSpeed);
    // Apply a remote player's one-shot activation EDGES (potion + class/boot/helm skills)
    // carried in `in.extFlags` for a single processed input. Called PER-INPUT from the
    // serverNetPre drain loop so every press is seen exactly once (the old getLatest()
    // read dropped edges that weren't on the newest buffered input — the unreliable-
    // activation bug). `slot` is the remote's net slot; gated on !isDead by the caller.
    void processRemoteActivation(u8 slot, const NetInput& in, f32 dt);
    void handleWeaponFire(f32 dt); // singleplayer legacy
    // Cap a player's airborne Infinity Chakrams: if `ownerSlot` already has the max
    // PROJ_INFINITE_BOUNCE projectiles aloft, retire the oldest (largest count-up lifetime),
    // skipping `keepIdx` (the just-spawned one). Keeps them from filling the shared pool.
    void capInfinityChakrams(u8 ownerSlot, u16 keepIdx);
    // Equip the item in quickbar slot `slot` (the bar's only "use" verb — it holds weapons, not
    // consumables). Shared by both input routes: middle-click on the wheel-selected slot, and a
    // gamepad's L + D-pad, which selects and equips in one press. Pushes sendInventorySync() so a
    // client's swap reaches the authoritative server.
    void useQuickbarSlot(u8 slot);
    // Bind the currently-equipped WEAPON to the ACTIVE quickbar slot (the inventory quickbar-loadout
    // UX — see engine_inventory.cpp). Called after an inventory WEAPON equip. Quickbar is local state.
    void bindWeaponToActiveQuickbar();
    // Lock-on is inert (lockActive never set true); this now only handles the
    // quickbar-use action. Name kept to avoid churning call sites (R7-6).
    void updateTargetLock(f32 dt); // singleplayer legacy

    void render(f32 alpha);
    void renderViewmodel();  // draws first-person hand + equipped weapon
    void renderMenu();
    void renderLobby();

    // Update sub-functions (called from singleplayerUpdate())
    void updateInventoryInteraction(f32 dt);
    // --- Interact (E) targeting + tap/hold arbitration -------------------------------------
    // Resolved ONCE per tick per local player by updatePlayerPickup, then consumed by
    // updateFloorDoor and renderInteractionPrompts. Before this, four places each ran their own
    // "what am I looking at" scan (host pickup, client pickup, the prompt renderer, the door) and
    // they had already drifted: the CLIENT's scan rejected any defId past the real item range,
    // which silently made every shrine un-activatable in co-op, and the prompt used a pure
    // proximity test so it could advertise a shrine that pressing E would not activate.
    struct InteractState {
        s32  itemIdx   = -1;         // best aimed world item (never a shrine)
        s32  shrineIdx = -1;         // best aimed shrine
        // Best aimed dormant mimic "chest" (entity pool index, -1 = none). Item-class for the
        // tap/hold rule: opening the chest is a tap, but real loot in reach always wins it.
        s32  mimicIdx  = -1;
        // Best aimed REAL chest (world-item index of a CHEST_ID sentinel, -1 = none). Same
        // item-class tap as the mimic — from the player's side the two must be identical.
        s32  chestIdx  = -1;
        bool nearExit   = false;     // standing in the floor-exit trigger
        bool nearPortal = false;     // standing in The Source portal trigger (floor 50 secret)
        bool nearExitPortal = false; // standing in the post-Engine exit portal (rolls credits)
        s32  stashIdx  = -1;         // world-item index of the town's stash chest in reach (-1 none)
        bool nearTownPortal = false; // standing in the town's to-dungeon portal
        Interact::HoldState hold;    // tap/hold machine state (see game/interact.h)
    };
    InteractState m_interact[MAX_LOCAL_PLAYERS];
    // Set by updatePlayerPickup, consumed by updateFloorDoor later in the SAME tick (the door
    // no longer reads the button itself — the tap/hold rule has to arbitrate it against loot).
    bool m_descendRequested = false;
    // Same arbitration for The Source portal (updateSourcePortal). It is EXIT-class: entering the
    // superboss chamber is irreversible, so a tap meant for the loot at your feet must never do it.
    bool m_portalRequested = false;
    bool m_creditsRequested = false;   // arbitrated exit-portal enter (see m_portalRequested)
    bool m_townPortalRequested = false; // arbitrated town-portal enter (opens Free-Play select)
    f32  m_creditsScroll = 0.0f;       // CREDITS state: scroll position in rows-of-text pixels

    void resolveInteractTargets(InteractState& st);

    // Open a real chest (CHEST_ID world-item sentinel): remove it and spawn its loot, rolled
    // HERE at open time from the chest's stored itemLevel. Authoritative-sim only (SP/host
    // direct; a guest's request arrives via handlePickupRequest's chest branch).
    void openChest(u32 worldIdx);

    void updatePlayerPickup(f32 dt);
    // Auto-pickup of a source shard (secret superboss key): set the session bit for its floor and
    // fire its one-line lore whisper (idempotent — re-collecting an already-held floor does nothing).
    // Called from updatePlayerPickup (host-local lanes) and serverNetPost (remote lanes).
    void collectSourceShard(const ItemInstance& shard);
    // Returns true if the player descended (caller should return immediately)
    bool updateFloorDoor();
    // Run the floor-descent flow (bump currentFloor, grow all players, refresh cooldowns,
    // save, schedule FLOOR_TRANSITION, broadcast SV_LEVEL_SEED). Shared between the local
    // host-pressed-E path (updateFloorDoor) and the remote client request path
    // (onDescendRequest). Returns true on success — caller skips remainder of tick.
    bool triggerFloorDescent();
    bool floorBossAlive() const; // true while a milestone boss on this floor is not yet dead

    // --- The Source (secret superboss) flow. See ~/.claude/plans (the-dungeon-engine). ---
    // Per-tick on the host: once the floor-50 boss is dead AND the full shard set is held, spawn the
    // hidden second portal; if the player stands in it and presses pickup, enter The Source.
    // Returns true if the player entered (caller skips the rest of the tick, like updateFloorDoor).
    bool updateSourcePortal();
    // Carve the fixed closed arena into the grid and rebuild the mesh; returns the arena centre.
    Vec3 buildSourceChamber();
    // Host: transition into The Source — build the chamber, move players in, spawn the Engine,
    // broadcast the sentinel-floor seed so clients follow. Does NOT bump currentFloor or save.
    void enterSourceChamber();
    // Client: mirror of enterSourceChamber driven by the sentinel-floor SV_LEVEL_SEED.
    void enterSourceChamberClient();
    // Town hub (engine_town.cpp): the outdoor post-Engine home base — deterministic build
    // (buildSourceChamber pattern), NPC + stash + portal population shared by host and client.
    Vec3 buildTownLevel();
    void spawnTownContents(Vec3 center);
    void enterTown();
    void enterTownClient();
    // PvP arena (engine_arena.cpp): deterministic colosseum on sentinel floor 97 — same rails
    // as the town (host broadcasts the sentinel seed, clients build the identical level).
    Vec3 buildArenaLevel();
    void spawnArenaContents(Vec3 center);
    void enterArenaCommon();          // shared host/client body (build + reset + local placement)
    void enterArena();                // host/SP entry (seats NetPlayers, broadcasts the seed)
    void enterArenaClient();          // client mirror (position/score adopted from the server)
    Vec3 arenaPad(u8 slot) const;     // a slot's spawn/respawn pad (world coords)
    // The authoritative PvP window: registers every living combatant with Combat::setPvpTargets
    // for exactly the span of the tick where PvP damage can resolve (serverNetPre's input drain
    // + remote activations, the per-lane gameUpdate fire paths, and tickSharedSystems'
    // projectiles/meteors). Registry views are GEOMETRY SNAPSHOTS only — each landed hit is
    // applied atomically by pvpApplyHit (fresh seed → damage → writeback), so the other
    // seed/writeback cycles running inside the window (remote activations, the shared AI view
    // pass) can never be clobbered. No-ops outside the arena and on CLIENTs.
    void arenaBeginPvpWindow();
    void arenaEndPvpWindow();
    Combat::PvpHitOutcome pvpApplyHit(u8 slot, const Combat::PvpHit& hit);
    // Deathmatch loop (engine_arena.cpp): authority-side kill credit + auto-respawn + match
    // end; clients mirror via the ARENA_* events (engine.cpp onEvent).
    void arenaHandleDeath(u8 victimSlot, u8 killerSlot);
    void arenaRespawnSlot(u8 slot);
    void arenaTick(f32 dt);            // respawn clocks (authority) + over-banner (everyone)
    void beginArenaOver(u8 winner);    // broadcast-first, then the local flip (CREDITS rule)
    void arenaSendScores(u8 toSlot);   // ARENA_SCORES refresh (0xFF = broadcast)
    void arenaLeaveToMenu();           // teardown, never saves
    void arenaPushFeed(u8 killerSlot, u8 victimSlot);
    // SERVER net-callback wiring + Server::init, extracted from startGame so hosts that build
    // their world WITHOUT startGame (arena Continue / --arena, town cleared-Continue) still
    // seat joiners. Idempotent.
    void wireServerNet();
    // CLIENT twin: callback wiring + prediction/ring resets, so sentinel-floor joins
    // (enterArenaClient/enterTownClient/enterSourceChamberClient) aren't connected-but-deaf.
    void wireClientNet();
    // Post-Engine victory flow: portal spawn (host: local + broadcast; client: via SV_EVENT),
    // the arbitrated enter (updateExitPortal), and the shared credits sequence. startCredits is
    // the LOCAL state flip every machine runs; beginCreditsSequence is the server/SP entry that
    // broadcasts first — this is what un-hangs co-op clients on the run's ending.
    void spawnExitPortal(Vec3 pos);
    bool updateExitPortal();
    bool updateTownPortal();
    void beginCreditsSequence(bool engineSlain);
    void startCredits(bool engineSlain);

    // gameUpdate helpers — extracted from the god function for readability.
    // Each is called exactly once, in the same order they appear in gameUpdate.
    void tickWandererTimers(f32 dt);             // adrenaline stacks, deflect burst, mark, Death's Dance, floor unlock
    // Steam achievement polling (once per second from run()'s stats block): currently just the
    // fully-equipped check — event-driven achievements (first item, Butchered) unlock inline at
    // their trigger sites. The guard bool keeps the once-unlocked state from re-querying Steam.
    void checkAchievements();
    bool m_achFullyEquipped = false;
    // Deflect window payoff (nova + projectile spread), shared by the local tick above and the
    // server's remote-lane expiry in serverNetPost — one implementation, no host/guest drift.
    void fireDeflectBurst(const Vec3& feetPos, const Vec3& forward, f32 eyeHeight,
                          f32 absorbed, u8 hits, u8 ownerSlot, bool localVisuals);
    void tickPlayerStatusEffects(f32 dt);        // poison / burn / freeze DoT ticks
    void handleDebugKeys();                      // F1-F6 debug toggles and stress spawners
    void tickSharedFX(f32 dt);                   // ONCE/frame: impact/fire/nova/dash/beam/chain/light timers + scorch AoE DoT
    void tickPlayerFX(f32 dt);                   // per local player: overcharge tick + herald aura on m_localPlayer
    void tickSkillCooldowns(f32 dt);             // SkillSystem::update + class/boot/helmet cooldown ticks
    void tickPassiveEquipment();                 // bind weapon-proc / armor-aura / ring-passive SkillIds each tick
    void handleClassSkillActivation(f32 dt, Vec3 eyePos);     // keys 1-4 selection + right-click cast
    void handleEquipmentSkillActivation(f32 dt, Vec3 eyePos); // boots F / helmet G casts
    void tickArmorRingPassives(f32 dt);          // ring timers, Second Wind, Divine Judgment, per-entity armor/ring pass
    void tickVisualFeedback(f32 dt);             // damage flash + hurt vignette + low-HP rumble
    void snapCameraToPlayer();                   // snap camera onto player (no interp smear) after teleport/respawn
    void tickMiscTimers(f32 dt);                 // smoke/overdrive/shadowDance/curse/hitMarker/tutorial + camera + view bob + hit shake

    // Player-entity push collision (shared between singleplayer and server paths)
    void pushPlayerFromEntities();
    void resetEnemiesToRooms(); // walk enemies back to spawn on player death

    // Mesh ID lookup helper — linear scan over m_meshDefs by name
    u8 findMeshByName(const char* name) const;

    // Render sub-functions (called from render())
    void renderEntities(u32 sw, u32 sh);
    void renderProjectilesAndEffects(u32 sw, u32 sh);
    void renderWorldItems(u32 sw, u32 sh);
    // Draw the equipped weapon + armor overlays onto a 3rd-person body at (pos, yaw, scale).
    // `anim` = weapon anim flag bits (bit0 attacking, bit1 reloading). Used by the split-screen
    // partner, remote players, and the character-inspect screen.
    void submitPlayerEquipment(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                               u8 bodyMeshId, const PlayerInventory& inv);
    // Mesh-id variant for clients that have wire mesh ids but no remote inventory.
    // Material resolves to default texture/tint (material id 0) — adequate for remote view.
    void submitPlayerEquipmentIds(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                                  u8 bodyMeshId, u8 weaponMeshId, const u8 armorMeshId[4]);
    // Armor auto-fit helpers (engine_render_world.cpp): bodyRegionsFor returns the measured
    // head/torso/feet/hand boxes for a body mesh (synthesizing coarse ones from the mesh AABB if
    // unmeasured); armorFitMatrix builds the world transform that scales an armor mesh to enclose
    // a region (+margin) on a body drawn at (pos,yaw,bodyScale). Replaces hand-tuned offsets.
    BodyRegions bodyRegionsFor(u8 bodyMeshId) const;
    // Single armor-fit transform: scale an armor mesh's local AABB to a target box (center + per-axis
    // half-extents in body-local space) and seat it, then apply the body's pos/yaw/scale. Driven by
    // the per-slot specs in submitPlayerEquipment — replaces the old per-piece margin/skullcap magic.
    Mat4 fitMeshToBox(u8 armorMesh, const Vec3& center, const Vec3& half,
                      const Vec3& pos, f32 yaw, f32 bodyScale) const;
    // The slot-spec table: compute the body-local target box (center + half-extents) for an armor
    // slot (0=helmet,1=chest,2=boots,3=gloves) from measured landmarks. Returns false when the
    // slot's landmark is invalid (e.g. a robed body with no hands) so the piece is skipped. All the
    // meaningful "how big / where" tuning lives here in one place.
    bool armorSlotBox(int slot, const BodyRegions& reg, f32 bodyH,
                      Vec3& outCenter, Vec3& outHalf) const;
    void renderSpeechBubbles(u32 sw, u32 sh);
    // Screen-space interaction prompts (floor-descend + item-pickup button hints).
    // Drawn in the NATIVE HUD pass (not the scaled 3D pass) so the button-glyph
    // background projects through the correct HUD ortho and stays crisp.
    void renderInteractionPrompts(u32 sw, u32 sh);
    void renderHUD(u32 sw, u32 sh);
    void logStats();

    // renderHUD helpers — extracted contiguous blocks, called in original order
    void renderInventoryHUD(u32 sw, u32 sh);          // inventory screen branch
    // True while the inventory's item comparison is on screen (cursor on a non-empty backpack
    // cell): the skill bars + quickbar hide for that frame — the two tooltips land on top of them.
    bool inventoryComparisonActive(u32 sw, u32 sh) const;
    // Character-inspect screen (C key / LB+Plus): a live rotatable armored model rendered into an
    // offscreen FBO (renderInspectModelToFbo) composited beside a grouped stats sheet
    // (renderCharacterInspect). Implemented in engine_render_character.cpp.
    void renderInspectModelToFbo();
    void renderCharacterInspect(u32 sw, u32 sh);
    // Lazily (re)create an offscreen FBO (RGB color tex + depth RBO) at w x h. Body moved from the
    // former file-static ensureSceneFbo so both the render-scale path and the inspect screen share it.
    void ensureFbo(u32& fbo, u32& colorTex, u32& depthRbo, u32& curW, u32& curH, u32 w, u32 h);
    // Thunderclap's floor upgrade scales the stun on the SHARED SkillDef, so it must be applied
    // around a cast and restored right after. Call as a PAIR, and from BOTH the local cast path and
    // the server's remote-cast path — a guest's Warrior must get the same upgrade the host's does.
    SkillDef* beginThunderclapUpgrade(SkillId skill, u8 upgradeFloor, u32 effectiveFloor,
                                      f32& outOrigDuration);
    static void endThunderclapUpgrade(SkillDef* def, f32 origDuration);

    void renderSkillsHUD(u32 sw, u32 sh);             // class skill bar + equip bar + active skill display

    // Max legendary equipment skills that can be on the equip bar at once: boots, helmet, armor
    // aura, weapon proc, ring passive, gloves passive.
    static constexpr u32 MAX_EQUIP_SKILL_SLOTS = 6;
    // Fill `out` (capacity MAX_EQUIP_SKILL_SLOTS) with the equipped legendary skills in a fixed
    // order; returns the count. Shared by renderSkillsHUD and the inventory screen so both draw the
    // identical bar and a slot index means the same skill in both.
    // `outSlots` (optional, same capacity) records which ItemSlot each entry came from — the skill
    // tooltip needs it, because a skill can read differently per slot (Blood Nova on a weapon vs on
    // armor), and resolving without it would make the skill-bar tooltip disagree with the item's.
    u32 buildEquipSkillSlots(HUD::EquipSkillSlot* out, ItemSlot* outSlots = nullptr) const;

    // Inventory cursor panels (m_invCursorPanel). Was a 2-value bool-ish flip; the skill bars added
    // two more, so it is now a proper cycle. INV_PANEL_EQUIP_SKILL is skipped when nothing equipped
    // grants an equipment skill.
    static constexpr u8 INV_PANEL_BACKPACK    = 0;
    static constexpr u8 INV_PANEL_EQUIPMENT   = 1;
    static constexpr u8 INV_PANEL_CLASS_SKILL = 2;
    static constexpr u8 INV_PANEL_EQUIP_SKILL = 3;
    static constexpr u8 INV_PANEL_COUNT       = 4;

    // Park the synthetic cursor on the D-pad-selected slot, so the gamepad drives the SAME hover
    // path the mouse does (items and skills alike) instead of needing its own.
    void inventoryCursorToMouse(u32 sw, u32 sh, s32& mx, s32& my) const;
    // True when the inventory highlight + tooltip should follow the cursor (WASD/E or D-pad) rather
    // than the physical mouse. Split-screen P2 (gamepad-only) is always cursor; player 0 follows the
    // last input used (m_invCursorActive). Read by both the interaction handler and the HUD render so
    // the two never disagree about which one is driving.
    bool inventoryUsesCursor() const {
        return m_localPlayerIndex != 0 ? true : m_invCursorActive;
    }

    // Class + equipment skill bars ON the inventory screen, plus the hover/selection tooltip.
    void renderInventorySkillBars(u32 sw, u32 sh,
                                  const InventoryUI::SkillBarRects& sb,
                                  const HUD::EquipSkillSlot* equipSlots, u32 equipCount,
                                  const ItemSlot* equipSources,
                                  s32 mx, s32 my);
    void renderMinimapAndFloor(u32 sw, u32 sh);       // minimap + door blip + floor text + potion cooldown
    void renderTutorials(u32 sw, u32 sh);             // tutorial tooltip blocks

    // render() helpers — extracted contiguous blocks, called in original order
    // Returns true if a non-IN_GAME screen was drawn (caller should early-out)
    bool renderTransitionScreens(u32 sw, u32 sh);
    void selectPointLights();                          // gathers candidates, calls Renderer::setPointLights
    void renderAuraDiscs(const EntityPool& entPool);  // herald/buffed-enemy ground-disc pass
    void renderPostOverlays(u32 sw, u32 sh);          // radial red damage vignette
    // Draws a viewport-filling quad with the given RGBA + ortho projection using the
    // supplied shader.
    void drawScreenQuad(u32 sw, u32 sh, Vec4 rgba, const Shader& shader);

    // Menu/lobby
    void updateMenu(f32 dt);
    // Maintains the menu "last input device" gate (see m_menuMouseActive) once per render frame and
    // returns whether the mouse pointer should drive menus this frame. Also hides/shows the OS
    // cursor to match. Call at the top of every cursor-free screen's update (menu, death, pause).
    bool updateMenuMouseActive();
    // Rebind capture for the options submenus. keyboardMode=true binds only keys (Keyboard & Mouse
    // submenu); false binds only controller buttons/axes (Controller submenu). Sets m_menu.bindCapture
    // false once bound or cancelled (B/ESC).
    void captureRebind(bool keyboardMode);
    void updateLobby(f32 dt);
    // lanesPrepared=true: the menu already populated every local lane (loaded heroes and/or
    // freshly-equipped new lanes via equipFreshLane), so startGame only builds the world (per
    // `mode`) and skips the NEW_GAME inventory wipe/grant + HP reset. Used by the couch-co-op
    // start where lanes can mix New and Continue. Default false = legacy behavior.
    void startGame(GameStart mode, bool lanesPrepared = false);
    // Shared class-stat setup for lane 0 (HP/move/energy, class skills, mirror arrays). Called by
    // the menu's class-select confirm and by applyLaunchOptions(Save::NEW) — one source of truth.
    void applyClassToLane0(PlayerClass cls);
    // Equip the class starting weapon for one local player (centralizes what used
    // to be copy-pasted across the menu start paths). Called only on NEW_GAME.
    void equipStartingLoadout(u8 playerIdx);
    // Fully initialize one local lane as a brand-new character: wipe inventory/quickbar/skills,
    // grant the class starting loadout, and set class base HP/move/energy. m_playerClasses[lane]
    // must already be set. Shared by startGame's NEW_GAME path and the couch menu's new lanes.
    void equipFreshLane(u8 lane);

    // Floor-population helpers called by startGame in engine_spawn.cpp.
    // All receive dungeon by reference because spawnFloorBoss mutates room geometry.
    void spawnFloorEnemies(DungeonResult& dungeon, u8 tier);
    // Promote a just-spawned enemy to a CHAMPION pack leader and spawn its minions around it.
    // Returns true if it did. Called from BOTH spawn paths in spawnFloorEnemies (JSON + fallback)
    // so the two can't drift. Minions are cloned from the leader's PRE-buff stats, which is why
    // this takes the entity rather than a def — EnemyDef and EnemyTemplate are different types and
    // duplicating the logic per branch is exactly how these two paths rot apart.
    // Server/SP only: champions roll with std::rand() on the host and reach clients via the
    // replicated champAffixes byte, never by re-rolling locally.
    bool tryMakeChampion(Entity& leader, u16 leaderIdx, const DungeonRoom& room, u32 effFloor);
    // Champion packs already placed on this floor — the MAX_PACKS_PER_FLOOR gate. Reset per floor.
    u8   m_championPacksThisFloor = 0;

    // --- Floor events (game/floor_event.h) ---
    FloorEventTable m_floorEvents;              // loaded once from assets/config/events.json
    // Roll this floor's event (0 or 1) and spawn it. Called from startGame AFTER spawnFloorBoss and
    // buildClearanceField: the boss call MUTATES the boss room's geometry and rebuilds the level
    // mesh, so anything placed earlier can be swallowed by the arena expansion — and the goblin
    // needs the clearance field to path.
    void spawnFloorEvents(DungeonResult& dungeon);
    // Walk-up shrines (game/shrine.h). WorldItem sentinels, so they inherit replication + the
    // server-validated pickup path rather than needing a parallel interactable system.
    void spawnFloorShrines(const DungeonResult& dungeon);
    // ONE grant path, overloaded for the host's local Player and a remote's authoritative NetPlayer
    // — if these two drifted, a shrine would mean different things depending on who touched it.
    void grantShrineBuff(Player& p, u8 buff);
    void grantShrineBuff(NetPlayer& p, u8 buff);
    // Ambient monster-cry clock (tickSharedSystems): one living hostile calls out when it hits 0,
    // then it re-arms to a jittered ~12 s. Starts small so a fresh floor speaks up early.
    f32  m_monsterCryTimer = 5.0f;

    // Loot-goblin chase breadcrumbs (tickSharedSystems): every sharp turn of the FLEE serpentine
    // rattles the coin sack (positional GOBLIN_JINGLE) and, rate-limited, drops a chat line with
    // the goblin's direction relative to the nearest player. Pure per-machine detection from the
    // goblin's replicated velocity — nothing on the wire. One tracked goblin (one spawns per
    // floor); heading is XZ atan2, valid only while it is actually moving.
    f32  m_goblinTrackHeading  = 0.0f;
    bool m_goblinHeadingValid  = false;
    f32  m_goblinJingleCd      = 0.0f;   // debounce so one jink = one jingle
    f32  m_goblinChatCd        = 0.0f;   // chat is rarer than the jingle or it floods the log
    // Hoard chatter: while the goblin SITS (aiState IDLE, pre-provocation) it mutters to itself
    // and its sack clinks now and then — an audible "there is a goblin nearby" tell that fires
    // only within earshot, so it never spoils the surprise from across the floor.
    f32  m_goblinMutterTimer   = 4.0f;

    // Guest-side speech-bubble storage. SV_EVENT::SPEECH delivers NPC lines as transient wire
    // bytes, but Entity.speechText is a bare const char* — so a received line is parked in this
    // small ring and the interp entity points into it. Eight slots comfortably outlive any
    // 2.4 s bubble; reuse under absurd speech rates only retextures the oldest bubble.
    // CLIENT-side storage for replicated speech lines (SV_EVENT::SPEECH): one 64-byte buffer PER
    // ENTITY POOL SLOT, indexed by the server pool index the event names. This used to be an
    // 8-entry round-robin ring — under a speech burst (goblin mutters + ally banter + boss aggro)
    // the ring wrapped within a bubble's 2.4 s lifetime and rewrote a line out from under a LIVE
    // bubble, so an entity's bubble silently changed into another entity's sentence. Keyed by pool
    // slot, a new line can only ever overwrite ITS OWN entity's previous line — which is the
    // correct semantics (a speaker starting a new sentence replaces its old one). 128×64 = 8 KB.
    char m_guestSpeech[MAX_ENTITIES][64] = {};

    // The loot goblin: flees, bleeds loot while chased, and expires (paying nothing) if it escapes.
    void spawnLootGoblin(const DungeonResult& dungeon);
    // Drips the goblin's loot while it is alive and fleeing. Authoritative sim only.
    void tickLootGoblins(f32 dt);
    u8   m_goblinMeshId = 0;

    // Pet companions (the goblin's 1% consumable drop + every enemy's 1-in-10000 "Mini <Enemy>").
    // togglePetCompanion is the server/SP-authoritative summon-or-dismiss for the pet belonging to
    // `petDefId` (an items.json petSummon def); tryUsePetItem is the client-side use intercept
    // every equip entry point calls first (returns true = it was a consumable, handled). A CLIENT
    // lane's use is sent as CL_USE_PET (reliable, defId payload — the old INPUT_EX_PET bit could
    // not say WHICH pet once there was more than one) and lands in onUsePet on the server.
    void togglePetCompanion(u8 ownerSlot, u16 petDefId);
    bool tryUsePetItem(u8 backpackIndex);
    // --- Pet menagerie: profile-wide "which minipets have I ever summoned" collection ---
    // Persisted in menagerie.dat beside difficulty_unlock.dat — deliberately NOT part of the
    // (frozen, versioned) save format, and profile-wide like difficulty unlocks: the collection
    // belongs to the player, not to one character. Bit i of the mask = enemy def index i's
    // "Mini <Enemy>"; the goblin (no enemy def) has its own flag. recordPetSummon fires from
    // tryUsePetItem — the one local entry every use path crosses on every role.
    u64  m_menagerieEnemyMask = 0;
    bool m_menagerieGoblin    = false;
    bool m_menagerieOpen      = false;   // pause-menu subpage overlay (view-only)
    void loadMenagerie();
    // Shared account stash (5 pages x 48 slots, one stash.dat for all characters). Loaded once
    // at init; saved (atomically) whenever dirty — on stash-UI close, Save&Quit, and shutdown.
    Stash::State m_stash;
    void loadStash();
    void saveStash();
    // Stash UI rides the inventory screen: opening sets both flags; every existing inventory
    // close path (ESC/B/Tab/respawn) also closes the stash via the per-frame reconciler in
    // gameUpdate, which flushes stash.dat when it does.
    bool m_stashOpen = false;
    void openStashUI();
    // Account-wide town unlock (town_unlock.dat, the difficulty_unlock pattern): set the
    // moment ANY character's Engine kill writes the cleared marker — and retroactively by
    // loading any cleared save (covers pre-town slayers + tools/mark_cleared.py). New
    // characters start at the town gate once set; the shared stash finally twinks them.
    bool m_townUnlocked = false;
    void unlockTown();
    // Per-character lifetime stats — persisted in the stats_NN.dat SIDECAR next to the save
    // slot (save_NN.dat's frozen format is untouched; same pattern as menagerie.dat, keyed per
    // slot). Currently one counter: total kills, shown as "Enemies deleted" on the floor
    // transition. Incremented at both kill-credit sites (authoritative death callback for
    // local lanes; SV_KILL for a guest's own kills).
    u32  m_totalKills[MAX_LOCAL_PLAYERS] = {};
    u32  loadCharStats(u8 slot);                    // sidecar read (0 if absent/foreign)
    void saveCharStats(u8 slot, u32 totalKills);    // sidecar write (rides every character save)
    void saveMenagerie();
    void recordPetSummon(u16 petDefId);
    // The pause row only exists once something has been summoned — an empty museum is a
    // spoiler ("there are 39 pets?!"), a started one is a collection.
    bool menagerieUnlocked() const { return m_menagerieGoblin || m_menagerieEnemyMask != 0; }
    void renderMenagerie(u32 sw, u32 sh);
    void sendUsePetPacket(u16 petDefId);
    static void onUsePet(u8 playerSlot, u16 itemDefId);   // Net::OnUsePetFn (defId parsed in net.cpp)
    // Mimic chest E-interact (CL_INTERACT_ENTITY, v17). Entities have no world-item uid; the
    // shared name across machines is the SERVER pool index, which the client's entity mirror
    // is keyed by. onEntityInteract re-validates everything (active, MIMIC, DORMANT, in reach
    // of the authoritative NetPlayer) — a stale or hostile index can at worst do nothing.
    void sendMimicInteract(s32 poolIdx);                          // CLIENT lane "opened" a chest
    static void onEntityInteract(u8 playerSlot, u8 poolIdx);      // Net::OnEntityInteractFn
    // enemy def index → its pet item def (0xFFFF = none). Filled at init from petEnemy names;
    // consumed by the 1-in-10000 jackpot roll in handleNormalLootDrop.
    u16 m_petItemForEnemy[MAX_ENEMY_DEFS] = {};

    // --- Target health bar (Diablo 2 style, top of screen) ---
    // Preference: the enemy you are AIMING at; if none, the last one you hit, held for a moment so
    // the bar doesn't flicker out the instant your crosshair drifts. Resolved once per frame in
    // renderTargetBar (render-only state — it never feeds the simulation).
    EntityHandle m_targetEnt;          // currently displayed target
    f32          m_targetLinger = 0.0f; // seconds the target stays up after you stop aiming at it
    static constexpr f32 TARGET_LINGER_SEC = 4.0f;
    static constexpr f32 TARGET_FADE_SEC   = 0.6f;   // tail of the linger spent fading out
    void renderTargetBar(u32 sw, u32 sh);
    // True if the local player can actually SEE this point (no wall between eye and target).
    // Used to keep the target bar from becoming a wallhack.
    bool hasLineOfSightTo(Vec3 target) const;
    u8   m_shrineMeshId = 0;
    // The champion affixes that fire on a CYCLE (Molten eruptions, Thundering novas, Teleport
    // blinks) rather than on a hit (applyDamage) or a death (handleDeathPreamble). Authoritative
    // sim only — called from tickSharedSystems inside its NetRole::CLIENT gate.
    void tickChampions(f32 dt);
    // Returns the index of the boss room used for exit-portal placement,
    // or 0xFFFFFFFF if no boss was spawned this floor.
    u32  spawnFloorBoss(DungeonResult& dungeon);
    // Initialise an already-spawned entity as a boss from its def + effective floor (visuals,
    // role/phase/on-hit fields). Factored out of spawnFloorBoss so spawnSourceBoss and the Engine's
    // wave-summons build identical boss entities. Does NOT touch HP (set at spawn) or leash (the
    // caller owns its arena). `def` must be an element of m_bossDefs.defs (bossDefIdx is derived
    // from its address).
    void initBossEntity(Entity& e, const BossDef& def, u32 effFloor);
    // Spawn the Dungeon Engine superboss at the centre of The Source. Returns its pool index.
    u16  spawnSourceBoss(Vec3 center);
    void spawnFloorChests(const DungeonResult& dungeon);
    void spawnFloorDecorations(const DungeonResult& dungeon);
    void spawnFloorNpcs(const DungeonResult& dungeon);

    // Per-character persistence. A save file holds exactly ONE character (playerCount=1); a
    // split-screen session writes each local lane to its own slot (m_playerSaveSlot[lane]).
    // saveGame(slot) is the legacy single-character entry (== saveCharacter(0, slot)).
    void saveGame(u8 slot);
    void saveCharacter(u8 lane, u8 slot);  // write one local lane as a 1-player file to `slot`
    void saveAllCharacters();              // write each active lane to its own m_playerSaveSlot[lane]
    bool loadGame(u8 slot);                // world-adopting P1 load into lane 0 (+legacy 2-player files)
    // Load a file's first character into `lane` WITHOUT touching the world (floor/seed/difficulty
    // stay P1's). Used to drop a Continue'd hero into the Player-2 seat. Returns false on failure.
    bool loadCharacterIntoLane(u8 slot, u8 lane);
    u8   firstFreeSaveSlot();              // lowest 1-based empty slot, or 0 if all 20 are taken

    // One deserialized character block (the per-player portion of a save file). Shared scratch
    // for loadGame / loadCharacterIntoLane so the deserialize + apply logic lives in one place.
    struct SavedChar {
        f32 hp = 0, maxHp = 0;
        PlayerInventory inv{};
        QuickbarState   qb{};
        SkillState      skill{};
        u8 cls = 0, activeSkill = 0;
        SkillState      classSkills[4]{};
    };
    // Apply a deserialized character to a local lane: affix migration, stat recompute, class base
    // stats, energy/skill rewire. Does NOT touch world state. (Defined in engine_persist.cpp.)
    void applySavedCharToLane(u8 lane, const SavedChar& ps);

    void initAssets();      // load meshes/materials/JSON content + resolve visuals (called by init)
    void initCallbacks();   // wire Combat/SkillSystem/ProjectileSystem/Inventory event callbacks (called by init)

    // Death-event handlers (called from the Combat death callback).
    // handleDeathPreamble runs unconditionally for every death (squad, speech, class passives, bomber).
    // The remaining helpers handle loot and on-kill ring effects in order; each bool-returning
    // helper returns true when it fully handled the drop path and no further drop logic should run.
    void handleDeathPreamble(EntityPool& pool, u16 idx, Vec3 pos);
    bool handleFirstKillDrop(EntityPool& pool, u16 idx, Vec3 pos);
    bool handleBossLootDrop(EntityPool& pool, u16 idx, Vec3 pos);
    // Champion pack LEADERS drop a guaranteed item whose rarity floor scales with affix count.
    // Returns true if it handled the drop (caller then skips the normal roll). Minions deliberately
    // fall through to the normal path — see the pool-saturation note in the implementation.
    bool handleChampionLootDrop(EntityPool& pool, u16 idx, Vec3 pos);
    // The loot goblin's death payout. An ESCAPED goblin never reaches this: it expires via
    // Entity::lifeTimer, which frees the slot without firing the death callback.
    bool handleGoblinLootDrop(EntityPool& pool, u16 idx, Vec3 pos);
    void handleNormalLootDrop(EntityPool& pool, u16 idx, Vec3 pos);
    void handleOnKillRingPassives(EntityPool& pool, u16 idx, Vec3 pos);

    // Save slot management
    static constexpr u32 MAX_SAVE_SLOTS = 20;
    u8 m_activeSaveSlot = 0;  // 0 = no active slot, 1-20 = slot number. == m_playerSaveSlot[0] (P1).
    // Per-local-lane save destination (1-20; 0 = this lane isn't persisted this session). Lane 0
    // tracks m_activeSaveSlot (P1); lane 1 is the Player-2 character's own slot, chosen in the couch
    // lobby. saveAllCharacters() writes each active lane to its own slot, so characters never share
    // a file. Reset (lane 1 -> 0) whenever a fresh game-setup begins so a solo Continue stays solo.
    u8 m_playerSaveSlot[MAX_LOCAL_PLAYERS] = {0, 0};

    // Per-lane origin (runtime only, not persisted): true if this lane's character
    // was loaded from a save (Continue / network join), false if it's a fresh New
    // Game character. Gates the no-downgrade save guard: a loaded high-floor hero in
    // a lower world keeps its on-disk progress, but a New Game intentionally
    // overwrites its slot at the real (low) floor. Set false in equipFreshLane,
    // true in loadGame / loadCharacterIntoLane.
    bool m_laneLoadedFromSave[MAX_LOCAL_PLAYERS] = {false, false};

    // Set by the client-side menu when the join flow picked "Continue" with a
    // valid save slot — the save is loaded locally before connectToServer, and this
    // flag tells (a) startGame to skip the inventory wipe + starting-kit grant and
    // (b) updateLobby (post-SV_JOIN_ACCEPT) to push the loaded inventory to the
    // server via CL_INVENTORY_SYNC. Cleared after the sync is sent.
    bool m_clientLoadedFromSave = false;

    // Steam relay join target (P2): the host's SteamID from an accepted lobby invite / browse. Non-zero
    // routes the menu's connect step through Net::connectToSteamHost instead of connectToServer.
    // Cleared on connect / return to menu.
    u64 m_steamJoinHost = 0;
    // P3 lobby browser/quickmatch: pending request mode (0=none,1=quickmatch,2=browse) + browser cursor.
    u8  m_steamMenuMode = 0;
    u8  m_steamBrowserSel = 0;
    // The shareable code for the lobby WE host ("K3M7-Q2XA-9BTC-F"), filled in when the lobby is
    // created. Shown in the pause menu so the host can read it out / copy it (C) — this is the only
    // way into a PRIVATE lobby for someone who isn't a Steam friend. Empty when we host no lobby.
    char m_lobbyCode[LobbyCode::BUF_SIZE] = {};
    // True from the moment the browser fires a RequestLobbyList until onSteamLobbyList answers.
    // Lets the browser show "Searching…" instead of flashing "No public games found" during the
    // round-trip (the old screen was indistinguishable from a genuinely empty result).
    bool m_steamBrowserSearching = false;
public:
    // Start a network host from the menu: Steam relay + a lobby when Online is chosen and Steam is
    // available, else ENet + UPnP. Returns true on success.
    bool netHostGame(u8 localPlayerCount = 1);
    // Enter the join flow for a Steam-relay host (from an accepted invite): stash the host SteamID and
    // route the menu to New/Continue -> class. Guards internally to only act from the MENU state.
    // Called from the Steam lobby-entered callback (a free function).
    void beginSteamJoin(u64 hostSteamId);
    // P3: kick off a quickmatch (join best public lobby, else host) / a public browser request.
    void steamQuickJoin();
    void steamBrowse();
    // Join by the host's 4-glyph share code — the only route into a PRIVATE lobby for a non-friend.
    // `code` must be normalized (LobbyCode::normalize) so it matches the host's published value.
    void steamJoinByCode(const char* code);
    // Steam lobby-list result handler (from the Steam callback): drives quickmatch / opens the browser.
    void onSteamLobbyList(int count);
    // Push the current roster into the Steam lobby (P2): publishes "players"/"slots_free" (the browser
    // shows the "players" count) and flips the lobby joinable flag off when full / back on when a slot
    // frees. No-op unless we're the Steam-relay host that owns a lobby. Called from onPlayerJoin/
    // onPlayerLeft (roster changes) and steamOnLobbyCreated (initial publish) — the latter is a free
    // function, so this must be public. Stops friends-list/browser "Join" from offering a full game.
    void updateSteamLobbyRoster();
    // Host: publish the lobby's identity, visibility and 4-glyph share code (called from the Steam
    // onLobbyCreated callback). Kept as an Engine method because it needs private engine state.
    void publishLobbyIdentity();
private:


    struct SaveSlotInfo {
        bool exists;
        u8   floor;
        u8   playerCount;       // 1 or 2
        u8   playerClasses[2];  // PlayerClass cast to u8; index 1 is 0xFF if single-player
        u32  timestamp;         // seconds since epoch
        f32  totalPlayTime;
        u8   difficulty;        // R13: 0=Normal, 1=Nightmare, 2=Hell — paired with `floor`
                                //      for the effective-floor comparison saveGame uses to
                                //      avoid downgrading a joined CLIENT's on-disk progress.
    };
    SaveSlotInfo m_saveSlots[MAX_SAVE_SLOTS];

    // Populate m_saveSlots by reading just the header of each slot file
    void scanSaveSlots();
    // Write the platform-appropriate slot path into buf (e.g. "save_01.dat")
    static const char* getSaveSlotPath(u8 slot, char* buf, u32 bufSize);

    // Net player helpers
    void syncLocalPlayerToNetPlayer();
    void syncNetPlayerToLocalPlayer();

    // R7-3: bridge active REMOTE NetPlayers into the server's AI/projectile target set.
    // The AI + projectile systems operate on `Player&`/`Player**`, but remote players are
    // NetPlayers — so on the SERVER we build throwaway Player "views" of each active,
    // non-dead remote NetPlayer (copying the fields the targeting/damage paths read), pass
    // them as extras to BOTH EnemyAI::update and ProjectileSystem::update, then copy the
    // mutated fields (health + status timers) back into the NetPlayers. `buildRemotePlayerViews`
    // fills the view array + pointer array (skipping the local host slot) and returns the count;
    // `applyRemotePlayerViews` writes the mutated state back. NOT used in SP/split-screen.
    u32  buildRemotePlayerViews(Player* views, Player** ptrs, u8* slots);
    void applyRemotePlayerViews(const Player* views, const u8* slots, u32 count);
    // TA-3: single-slot form of the same bridge, for the remote skill-cast paths. A guest's
    // skill must run against ITS OWN Player view (not the host's m_localPlayer), so build a
    // view of one remote NetPlayer, pass it to SkillSystem::tryActivate, then write back.
    void buildRemotePlayerView(u8 slot, Player& out);
    void applyRemotePlayerView(const Player& v, u8 slot);

    // Client: pick the aimed world item and request its pickup from the server
    // (CL_PICKUP_ITEM, server-authoritative pickups — N5).
    void sendPickupRequest(s32 worldIdx);
    void sendPickupPacket(u32 uid);
    // Server: validate and apply a client's CL_PICKUP_ITEM request (proximity + ownership),
    // moving the item into that player's inventory and freeing the world slot.
    void handlePickupRequest(u8 playerSlot, u32 uid);

    // R11 — Client: report an inventory drop to the server (CL_DROP_ITEM). Pass slotKind
    // = 0 for backpack, 1 for an equipped slot; the slotIndex is interpreted accordingly.
    // The full ItemInstance + drop position ride the packet so the server can spawn the
    // world item with the rolled stats intact.
    void sendDropRequest(u8 slotKind, u8 slotIndex, const ItemInstance& it, Vec3 dropPos);
    // R11 — Server: zero the named inventory slot and spawn a world item at dropPos. No
    // anti-cheat validation today (co-op trust model, same as onInventorySync).
    void handleDropRequest(u8 playerSlot, u8 slotKind, u8 slotIndex,
                           const ItemInstance& it, Vec3 dropPos);

    // Client: request respawn after death (reliable CL_RESPAWN). Server-authoritative.
    void sendRespawnRequest(u8 targetSlot = 0); // targetSlot = the dying lane's net slot (couch co-op)
    // Server: respawn a dead client's NetPlayer (idempotent; revival propagates via snapshot).
    void handleRespawnRequest(u8 playerSlot);

    // Client: request the host trigger a floor descent at the portal (reliable
    // CL_REQUEST_DESCEND). Server re-validates proximity + boss-dead before triggering.
    void sendDescendRequest();
    // Server: validate a remote client's descent request and run the shared descent flow.
    void handleDescendRequest(u8 playerSlot);

    // Client: send a weapon-fire request (reliable CL_FIRE_WEAPON) carrying the local
    // eye position + aim at the moment of trigger. The server fires authoritatively
    // from THESE values — no drain-derived np.yaw — so a slow input queue can't
    // produce a "fires along aim from seconds ago" stale shot. M10.1: moved from
    // unreliable+manual-retransmit to ENet reliable; resendPendingFire() is deleted.
    void sendFireWeapon(Vec3 origin, f32 yaw, f32 pitch);
    // Server: clear the CL_FIRE_WEAPON dedup ring for a single slot — called from
    // onPlayerJoin so a recycled slot doesn't carry the prior occupant's tick history.
    void resetFireDedup(u8 slot);
    // Server: clear ALL CL_FIRE_WEAPON dedup rings — called after a floor descent so
    // post-reset client ticks (which restart near 0) aren't squashed as duplicates of
    // ticks the ring saw on the prior floor. Also drops any in-flight client-side
    // retransmit window so a client doesn't keep re-sending a prior-floor fire.
    void resetAllFireDedup();
    // Server: handle a remote client's fire request. Loose-validates (cooldown gate,
    // origin clamped to within ~1 m of authoritative np.position) and queues a pending
    // fire that the per-tick handleWeaponFireForPlayer consumes.
    void handleFireWeaponRequest(u8 playerSlot, u32 clientTick,
                                 Vec3 claimedOrigin, f32 claimedYaw, f32 claimedPitch);

    // Phase 3 — Server-side lag compensation. Snapshot every active entity's transform
    // each tick; when a remote client's hitscan / melee is processed, rewind candidate
    // entity poses to where they were on the firer's screen, run the hit query, then
    // restore present-time poses before any side-effects observable outside.
    void pushEntityHistory();        // call from serverNetPost after snapshot built
    void resetEntityHistory();       // call from startGame (floor descent / new level)
    u32  computeLagCompTicks(u8 slot) const;
    void beginLagComp(u32 ticksAgo);
    void endLagComp();

    // R6: build the player-collision obstacle list at a lag-comp-rewound tick so the
    // server's per-input moveAndSlide sees the same entity positions the client used
    // when capturing the input. `out` must have capacity for MAX_ENTITIES; `outCount`
    // is set on return. `targetSnapTickF` is the FRACTIONAL server-tick the client
    // interpolated against — build it with LagComp::targetTick(ackedSnap, in.interpDelayMs)
    // so the rewind matches the delay that client actually applied (it is adaptive; see
    // net/lag_comp.h). Poses are lerped between the bracketing history entries. Falls back to
    // live entity positions for any entity whose history ring is empty (first ticks after a
    // join, after a level reset, etc.).
    void buildLagCompPlayerObstacles(f32 targetSnapTickF,
                                     CollisionObstacle* out,
                                     u32& outCount) const;

    // Net callbacks (static, forwarded to engine instance)
    static void onSnapshot(const u8* data, u32 size);
    static void onInput(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_PICKUP_ITEM handler (forwarded from the net layer). Validates the
    // request against authoritative world-item + player state and applies the pickup.
    static void onPickup(u8 playerSlot, const u8* data, u32 size);
    // R11 — Server-side CL_DROP_ITEM handler. Removes the named slot from the requester's
    // inventory and spawns a world item carrying the full rolled stats. Without this,
    // the client's local-only drop is silently overwritten by the next inventory sync /
    // mirrorWorldItems pass — the item vanishes and the bag drifts from the server.
    static void onDropItem(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_RESPAWN handler (forwarded from the net layer).
    static void onRespawn(u8 playerSlot);
    // Server-side CL_REQUEST_DESCEND handler (forwarded from the net layer). Re-validates
    // proximity to the door + boss-dead gate, then runs the shared descent flow.
    static void onDescendRequest(u8 playerSlot);
    // Server-side CL_FIRE_WEAPON handler (forwarded from the net layer). Unpacks the
    // payload (clientTick + claimed origin + yaw/pitch) and queues the fire request.
    static void onFireWeapon(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_INVENTORY_SYNC handler: deserialize the joining client's saved
    // inventory + class state into m_inventories[slot] / m_quickbars[slot] / m_players[slot]
    // overriding whatever starting kit onPlayerJoin granted moments earlier.
    static void onInventorySync(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_TIME_PING handler (M1.4): read clientTimeMs from the payload, stamp
    // serverTick + serverTimeMs, and send SV_TIME_PONG back on the unreliable channel.
    static void onTimePing(u8 playerSlot, const u8* data, u32 size);
    // Client-side SV_TIME_PONG handler (M1.5): strip the 4-byte header, pass the 12-byte
    // body to Client::handleTimePong, which feeds ClockSyncOps::onPongReceived using
    // Clock::getElapsedSeconds() as the pong-arrival wall time.
    static void onTimePong(const u8* data, u32 size);
    // Client-side helper: serialize m_inventories[localSlot] + class/skill state and ship
    // it via CL_INVENTORY_SYNC. Called once shortly after SV_JOIN_ACCEPT when the joiner
    // came from the menu's "Continue" path with a loaded save.
    // Push one local lane's inventory to its server slot (online couch co-op: per lane). NO
    // default args on purpose: the old `= 0` defaults let every in-game equip path push LANE 0
    // to the primary slot, so a couch client's P2 fought with join-time gear forever. In-game
    // callers pass (m_localPlayerIndex, activeNetSlot()) from inside the per-lane swap.
    void sendInventorySync(u8 lane, u8 targetSlot);
    static void onEvent(const u8* data, u32 size);
    static void onPlayerJoin(u8 playerSlot, u8 classId);
    static void onPlayerLeft(u8 playerSlot);
    // Client-side SV_DAMAGE_DONE handler (M10.2). Acks the matching PendingHitRing entry
    // so the predicted hit-marker state is cleaned up when the server confirms the hit.
    static void onDamageDone(u32 clientTick, u16 targetEntityIdx);
    // Client-side SV_DAMAGE_TO_ME handler (M10.3). Acks the matching PendingDamageRing
    // entry so predicted incoming-damage visual state is cleaned up on server confirmation.
    static void onDamageToMe(u32 projectileSrcKey, f32 damage);
    // Client-side SV_KILL handler (D1.1). v1 logs the kill for diagnostics.
    static void onKill(u8 killerSlot, u8 victimType, u16 victimIdx,
                       u8 weaponMeshId, u8 isCrit);
    // Client-side SV_PICKUP_RESULT handler (D1.2). Acks the pending-pickup ring
    // entry regardless of accept/reject (server snapshot handles the item state).
    static void onPickupResult(u8 accept, u32 itemUid);
    // Client-side SV_LOOT_SPAWN handler (D1.3). v1 logs the loot event.
    static void onLootSpawn(u32 uid, f32 posX, f32 posY, f32 posZ, u16 itemDefId);
    // Client-side SV_ENERGY_GAIN handler. Adds server-granted energy to the local player's pool.
    static void onEnergyGain(f32 amount);
    // Server-pushed mid-run floor descent (SV_LEVEL_SEED). Client-only: adopt the
    // host's new floor/difficulty/seed and follow into the same FLOOR_TRANSITION path.
    static void onLevelSeed(u8 floor, u8 difficulty, u32 seed);
};

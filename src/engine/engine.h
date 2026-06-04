#pragma once

#include "core/types.h"
#include "renderer/camera.h"
#include "renderer/particles.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
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
#include "game/boss_def.h"
#include "game/enemy_def.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"    // WorldSnapshot — needed for m_baselineSnap / m_lastAppliedSnap (D7.2)
#include "net/clock_sync.h"
#include "net/prediction_ring.h"
#include "net/render_offset.h"
#include "net/pending_hit_ring.h"
#include "net/pending_damage_ring.h"
#include "net/pending_pickup_ring.h"
#include "net/pending_skill_ring.h"
#include "net/snapshot_baseline.h"
#include "game/squad.h"
#include "world/level_gen.h"

static constexpr u32 MAX_LEVEL_SECTIONS = 64;

enum struct GameState : u8 {
    MENU,
    LOBBY_HOST,
    LOBBY_JOIN,
    CONNECTING,
    IN_GAME,
    GAME_OVER,          // player died, show death screen
    FLOOR_TRANSITION,   // between-floor title card (2s hold)
    VICTORY,            // player completed floor 50 — victory screen before menu return
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

class Engine {
public:
    void init();
    void shutdown();
    void run();

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

    // Difficulty — 0=Normal, 1=Nightmare (2x HP/1.5x dmg), 2=Hell (3x HP/2x dmg)
    u8 m_difficulty        = 0;
    // Highest difficulty unlocked globally (persisted in difficulty_unlock.dat)
    u8 m_highestUnlocked   = 0;

    // Menu/UI state — grouped for isolation
    struct MenuState {
        u8   selection = 0;
        u8   subState = 0;       // 0=main, 1=singleplayer, 2=P1 class, 3=options, 4=couch lobby,
                                 // 5=P2 class, 6=P1 slot, 8=overwrite confirm, 10=host mode,
                                 // 11=P2 New/Continue chooser, 12=P2 slot select
        u8   subSelection = 0;
        f32  msgTimer = 0.0f;    // countdown for transient menu messages
        const char* msg = nullptr;
        // Couch co-op: each lane's New-vs-Continue intent, captured during slot selection so the
        // shared start (subState 4/5/12) calls startGame with the right world mode (CONTINUE keeps
        // P1's saved floor/seed; NEW_GAME makes a fresh world) and prepares each lane correctly.
        bool p1Continue = false;
        bool p2Continue = false;
        u8   overwriteLane = 0;        // which local lane (0=P1, 1=P2) the subState-8 overwrite is for
        bool couchHost = false;        // true when the host-mode chooser (10) was reached from the
                                       // couch lobby to host an ONLINE couch game (both locals + remotes)
        bool couchJoin = false;        // true when the IP-entry screen (9) was reached from the couch
                                       // lobby to JOIN an online game with two local players
        bool bindCapture = false;   // true when waiting for key/button press to rebind
        bool bindKeyboard = true;   // true=capturing keyboard, false=capturing controller
        bool confirmQuit = false;      // "are you sure?" overlay when pressing ESC in-game
        u8   overwriteSlot = 0;        // save slot pending overwrite (subState 8)
        char connectAddress[64] = "127.0.0.1";
        // True while the IP-entry screen (subState 9) hasn't been edited yet — the first
        // digit/dot keystroke wipes the default 127.0.0.1 so the user can type fresh
        // without having to backspace nine characters first. Reset each time Join is
        // chosen from the main menu.
        bool connectAddressClearOnType = true;
        // Host-mode selector (subState 10). true → ask the router for a UPnP IGD port
        // mapping at host time so friends on other networks can join; false → strictly
        // LAN-only, skip the SSDP discovery and never touch the router. Defaults to the
        // historical behavior (always-attempt UPnP) so launches that bypass subState 10
        // don't regress.
        bool hostOnline = true;
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
    bool           m_inventoryOpenArr[MAX_LOCAL_PLAYERS] = {};
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
    bool        m_inventoryOpen = false;
    ViewmodelState  m_viewmodelState;

    // Death-screen mouse control. m_deathHover is the option the mouse is over on the SP
    // GAME_OVER screen (0=Respawn, 1=Reload, 2=Quit; -1 = none) so the renderer can highlight
    // it. m_deathCursorFree tracks whether we've freed the cursor for the networked-MP dead
    // overlay (edge flag — the dead-branch runs every frame, so toggle the OS cursor once).
    s8          m_deathHover = -1;
    bool        m_deathCursorFree = false;

    // Gameplay input (movement/aim/fire/skills/block/dodge) is frozen while a blocking UI is
    // open — the inventory OR the in-game pause menu (m_menu.confirmQuit). In SP the pause
    // early-returns and freezes the whole world; in MP the world keeps running for everyone
    // else, so these gates are what hold the paused player's own character still. Potion is
    // intentionally NOT gated (matches inventory — you can drink while a menu is open).
    bool gameplayInputFrozen() const { return m_inventoryOpen || m_menu.confirmQuit; }

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
    u8   m_netFakeLossPct    = 0;    // 0–100: percentage of packets to drop (both directions)
    u32  m_divergenceCount   = 0;    // count of reconcile mismatches since last log interval
    f64  m_lastDebugLogSec   = 0.0;  // wall-clock time of last [NET-GRAPH] emission
    // D6: toggles an on-screen net-stats overlay (F9). Visible only on CLIENT role.
    bool m_netGraphVisible   = false;

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

    // Per-client delta-compression baselines (server role only).
    // Tracks the serverTick of the last snapshot sent to each remote client so that
    // shouldSendFullSnapshot can decide whether a delta is safe for the next snapshot.
    // Index matches the player slot (slot 0 = host; that entry is unused but kept so
    // slot arithmetic stays simple). Reset on connect and on floor descent.
    BaselineTracker m_baselines[MAX_PLAYERS];
    // The full-tick ACK each remote client reported in its most recent NetInput.
    // ackedSnapshotTick on the wire is u16 (low bits only); we reconstruct the full
    // u32 here using the high bits of m_serverTick at the time the input is received.
    u32 m_clientAckedSnap[MAX_PLAYERS] = {};

    // D7.2 — Per-client full snapshot baseline (server role only).
    // After each snapshot broadcast, the server copies the sent WorldSnapshot here
    // indexed by player slot. D7.3 computes changedBits by comparing the current
    // snapshot against this baseline to decide which slots need to be on the wire.
    // Memory cost: MAX_PLAYERS (4) × ~50 KB ≈ 200 KB — all on BSS, not the stack.
    // Slot 0 (host) is stored but unused; kept so index arithmetic is uniform.
    WorldSnapshot m_baselineSnap[MAX_PLAYERS];

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
    Shader  m_particleShader;  // batched per-vertex-color shader for billboard particles
    Mesh    m_cubeMesh;
    Mesh    m_quadMesh;   // flat quad for billboard sprites

    // Mesh registry for entities (MeshDef struct defined in limb_system.h)
    static constexpr u32 MAX_MESH_DEFS = 96;
    MeshDef  m_meshDefs[MAX_MESH_DEFS] = {};
    u32      m_meshDefCount = 0;

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
        SquadPool     squads;
        Vec3          floorDoorPos;
        bool          floorDoorActive = false;
        bool          floorHasBoss    = false; // this floor spawned a milestone boss (gates the exit lock)
    };
    LevelState m_level;

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
        // PlayerClass (cast to u8) of each remote player — populated from SnapPlayer.playerClass
        // each snapshot. Without this the renderer can't pick the per-class mesh for a remote
        // (the client's NetPlayer.playerClass for non-local slots is never set).
        u8             playerClass[MAX_PLAYERS];
    };
    RenderInterp m_renderInterp;

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
    static constexpr u32 MAX_CHAIN_FX = 4;
    static constexpr u32 MAX_CHAIN_POINTS = 24;
    struct ChainFX { Vec3 points[MAX_CHAIN_POINTS]; u8 pointCount; f32 timer; bool active; };
    static constexpr u32 MAX_SCORCH = 4;
    struct ScorchZone { Vec3 pos; f32 radius; f32 timer; f32 dps; bool active; };
    static constexpr u32 MAX_DAMAGE_NUMBERS = 16;
    struct DamageNumber { Vec3 position; f32 amount; f32 timer; bool active; bool isHeal; bool isCrit; };

    struct EffectsState {
        ImpactFX      impactFX[MAX_IMPACT_FX] = {};
        FireFX        fireFX[MAX_FIRE_FX] = {};
        NovaFX        novaFX[MAX_NOVA_FX] = {};
        DashFX        dashFX[MAX_DASH_FX] = {};
        BeamFX        beamFX[MAX_BEAM_FX] = {};
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
    void renderDamageNumbers(u32 sw, u32 sh);
    // Spawn the projectile AoE splash VFX (floor-snapped fire FX + explosion particles +
    // camera shake) at an impact point. Shared by the host's splash callback and the CLIENT's
    // PROJECTILE_SPLASH SV_EVENT handler so both render identical splashes (the client's
    // ProjectileSystem::update is gated off, so it can't fire the callback itself).
    void spawnSplashFX(Vec3 position, f32 radius);

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
    u8 m_matIdBatWing   = 0;

    // Internal render-scale: the 3D scene renders to an offscreen FBO at this fraction of the window
    // resolution, then is upscaled (linear blit) to fill the screen — cuts fragment fill/overdraw on
    // the fill-bound Switch GPU while still filling the display. 1.0 = native (FBO bypassed). Cycled
    // by F6 (desktop) / L+R3 (Switch). FBO objects below are created lazily in ensureSceneFbo().
#ifdef __SWITCH__
    f32 m_renderScale = 0.65f; // confirmed: holds a steady 60 fps in handheld with a crisp native HUD
#else
    f32 m_renderScale = 1.0f;
#endif
    u32 m_sceneFbo = 0, m_sceneColorTex = 0, m_sceneDepthRbo = 0, m_sceneFboW = 0, m_sceneFboH = 0;

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
    // Apply a remote player's one-shot activation EDGES (potion + class/boot/helm skills)
    // carried in `in.extFlags` for a single processed input. Called PER-INPUT from the
    // serverNetPre drain loop so every press is seen exactly once (the old getLatest()
    // read dropped edges that weren't on the newest buffered input — the unreliable-
    // activation bug). `slot` is the remote's net slot; gated on !isDead by the caller.
    void processRemoteActivation(u8 slot, const NetInput& in, f32 dt);
    void handleWeaponFire(f32 dt); // singleplayer legacy
    // Lock-on is inert (lockActive never set true); this now only handles the
    // quickbar-use action. Name kept to avoid churning call sites (R7-6).
    void updateTargetLock(f32 dt); // singleplayer legacy

    void render(f32 alpha);
    void renderViewmodel();  // draws first-person hand + equipped weapon
    void renderMenu();
    void renderLobby();

    // Update sub-functions (called from singleplayerUpdate())
    void updateInventoryInteraction(f32 dt);
    void updatePlayerPickup();
    // Returns true if the player descended (caller should return immediately)
    bool updateFloorDoor();
    // Run the floor-descent flow (bump currentFloor, grow all players, refresh cooldowns,
    // save, schedule FLOOR_TRANSITION, broadcast SV_LEVEL_SEED). Shared between the local
    // host-pressed-E path (updateFloorDoor) and the remote client request path
    // (onDescendRequest). Returns true on success — caller skips remainder of tick.
    bool triggerFloorDescent();
    bool floorBossAlive() const; // true while a milestone boss on this floor is not yet dead

    // gameUpdate helpers — extracted from the god function for readability.
    // Each is called exactly once, in the same order they appear in gameUpdate.
    void tickWandererTimers(f32 dt);             // adrenaline stacks, deflect burst, mark, Death's Dance, floor unlock
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
    void renderSpeechBubbles(u32 sw, u32 sh);
    // Screen-space interaction prompts (floor-descend + item-pickup button hints).
    // Drawn in the NATIVE HUD pass (not the scaled 3D pass) so the button-glyph
    // background projects through the correct HUD ortho and stays crisp.
    void renderInteractionPrompts(u32 sw, u32 sh);
    void renderHUD(u32 sw, u32 sh);
    void logStats();

    // renderHUD helpers — extracted contiguous blocks, called in original order
    void renderInventoryHUD(u32 sw, u32 sh);          // inventory screen branch
    void renderSkillsHUD(u32 sw, u32 sh);             // class skill bar + equip bar + active skill display
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
    void updateLobby(f32 dt);
    // lanesPrepared=true: the menu already populated every local lane (loaded heroes and/or
    // freshly-equipped new lanes via equipFreshLane), so startGame only builds the world (per
    // `mode`) and skips the NEW_GAME inventory wipe/grant + HP reset. Used by the couch-co-op
    // start where lanes can mix New and Continue. Default false = legacy behavior.
    void startGame(GameStart mode, bool lanesPrepared = false);
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
    // Returns the index of the boss room used for exit-portal placement,
    // or 0xFFFFFFFF if no boss was spawned this floor.
    u32  spawnFloorBoss(DungeonResult& dungeon);
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
    void sendPickupRequest();
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
    // is set on return. `targetSnapTick` is the server-tick the client interpolated
    // against (= the client's ackedSnapshotTick minus the interp-delay offset, computed
    // by the caller). Falls back to live entity positions for any entity whose history
    // ring is empty (first ticks after a join, after a level reset, etc.).
    void buildLagCompPlayerObstacles(u32 targetSnapTick,
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
    void sendInventorySync(u8 lane = 0, u8 targetSlot = 0); // push one local lane's inventory to its
                                                            // server slot (online couch co-op: per lane)
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
    // Server-pushed mid-run floor descent (SV_LEVEL_SEED). Client-only: adopt the
    // host's new floor/difficulty/seed and follow into the same FLOOR_TRANSITION path.
    static void onLevelSeed(u8 floor, u8 difficulty, u32 seed);
};

#pragma once

#include "core/types.h"
#include "renderer/camera.h"
#include "renderer/particles.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
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
#include "net/clock_sync.h"
#include "net/prediction_ring.h"
#include "net/render_offset.h"
#include "net/pending_hit_ring.h"
#include "net/pending_damage_ring.h"
#include "net/pending_pickup_ring.h"
#include "net/pending_skill_ring.h"
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
        u8   subState = 0;       // 0=main, 1=singleplayer, 2=class, 3=options, 8=overwrite confirm
        u8   subSelection = 0;
        f32  msgTimer = 0.0f;    // countdown for transient menu messages
        const char* msg = nullptr;
        bool bindCapture = false;   // true when waiting for key/button press to rebind
        bool bindKeyboard = true;   // true=capturing keyboard, false=capturing controller
        bool confirmQuit = false;      // "are you sure?" overlay when pressing ESC in-game
        u8   overwriteSlot = 0;        // save slot pending overwrite (subState 8)
        char connectAddress[64] = "127.0.0.1";
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

    // Active-player aliases (gameUpdate reads/writes these, swapped per player)
    PlayerClass m_playerClass = PlayerClass::WARRIOR;
    u8          m_activeClassSkill = 0;
    SkillState  m_classSkillStates[4];
    SkillId     m_armorAura = SkillId::NONE;
    SkillId     m_weaponProc = SkillId::NONE;
    SkillId     m_ringPassive = SkillId::NONE;
    bool        m_inventoryOpen = false;
    ViewmodelState  m_viewmodelState;

    // Networking
    NetRole    m_netRole = NetRole::NONE;
    u8         m_localPlayerIndex = 0;
    // Net slot of the local player as assigned by the server (SV_JOIN_ACCEPT). On a host or in
    // singleplayer/split the local player's net slot equals its lane, so this stays 0 and
    // activeNetSlot() == m_localPlayerIndex. On a CLIENT, networking forces m_splitPlayerCount=1
    // so m_localPlayerIndex (the split-screen lane) is always 0, while the real net slot (>=1)
    // lives here. Separating the two is required because swapInPlayer() overwrites
    // m_localPlayerIndex with the lane every frame — see activeNetSlot().
    u8         m_clientNetSlot = 0;
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

    // Prediction ring (CLIENT role, M3) — stores (input, predicted-state) per clientTick
    // so that clientNetPost can compare the server's authoritative pose against what we
    // predicted and snap/correct if divergence > 10 cm. Reset on every CLIENT connect.
    PredictionRing m_predictionRing;
    u32            m_lastReconciledTick = 0;

    // Smooth correction offset (CLIENT role, M4) — accumulates the visible delta when the
    // server corrects our predicted position. Decays to zero each frame so the rendered
    // camera position smoothly slides toward the corrected sim position (~150 ms window)
    // instead of teleporting. Reset on every CLIENT connect (see engine_startgame.cpp).
    RenderOffset m_renderOffset;

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
    u8 activeNetSlot() const { return (m_netRole == NetRole::CLIENT) ? m_clientNetSlot : m_localPlayerIndex; }
    // Returns the server's current simulation tick — used by the server-side CL_TIME_PING
    // handler so net.cpp can stamp SV_TIME_PONG without directly accessing m_serverTick.
    u32 serverTickNow() const { return m_serverTick; }

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
    WorldItemPool   m_worldItems;
    QuickbarState   m_quickbars[MAX_PLAYERS];
    Mesh            m_handMesh;

    // Active-player aliases (set before gameUpdate, read back after)
    Player     m_localPlayer;
    Camera     m_camera;
    f32        m_hitMarkerTimer = 0.0f;
    f32        m_potionCooldown = 0.0f;
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

    // Switch constraint mode
    bool m_switchMode = false;
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

    // Core update paths
    void update(f32 dt);
    void gameUpdate(f32 dt);        // unified gameplay — all roles call this
    void tickSharedSystems(f32 dt); // ONCE/frame after the per-player loop: AI, projectiles,
                                    // entity timers, world items, shared FX, meteors, particles (M3)
    void serverNetPre(f32 dt);      // server: process remote inputs before gameplay
    void serverNetPost(f32 dt);     // server: status ticks + snapshot broadcast
    void clientNetPre(f32 dt);      // client: predict + reconcile before gameplay
    void clientNetPost(f32 dt);     // client: interpolate remote state after gameplay

    // Shared logic
    void handleWeaponFireForPlayer(NetPlayer& np, f32 dt);
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
    void startGame(GameStart mode);
    // Equip the class starting weapon for one local player (centralizes what used
    // to be copy-pasted across the menu start paths). Called only on NEW_GAME.
    void equipStartingLoadout(u8 playerIdx);

    // Floor-population helpers called by startGame in engine_spawn.cpp.
    // All receive dungeon by reference because spawnFloorBoss mutates room geometry.
    void spawnFloorEnemies(DungeonResult& dungeon, u8 tier);
    // Returns the index of the boss room used for exit-portal placement,
    // or 0xFFFFFFFF if no boss was spawned this floor.
    u32  spawnFloorBoss(DungeonResult& dungeon);
    void spawnFloorChests(const DungeonResult& dungeon);
    void spawnFloorDecorations(const DungeonResult& dungeon);
    void spawnFloorNpcs(const DungeonResult& dungeon);

    void saveGame(u8 slot);
    bool loadGame(u8 slot);

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
    u8 m_activeSaveSlot = 0;  // 0 = no active slot, 1-20 = slot number

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

    // Client: request respawn after death (reliable CL_RESPAWN). Server-authoritative.
    void sendRespawnRequest();
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

    // Net callbacks (static, forwarded to engine instance)
    static void onSnapshot(const u8* data, u32 size);
    static void onInput(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_PICKUP_ITEM handler (forwarded from the net layer). Validates the
    // request against authoritative world-item + player state and applies the pickup.
    static void onPickup(u8 playerSlot, const u8* data, u32 size);
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
    void sendInventorySync();
    static void onEvent(const u8* data, u32 size);
    static void onPlayerJoin(u8 playerSlot, u8 classId);
    static void onPlayerLeft(u8 playerSlot);
    // Client-side SV_DAMAGE_DONE handler (M10.2). Acks the matching PendingHitRing entry
    // so the predicted hit-marker state is cleaned up when the server confirms the hit.
    static void onDamageDone(u32 clientTick, u16 targetEntityIdx);
    // Client-side SV_DAMAGE_TO_ME handler (M10.3). Acks the matching PendingDamageRing
    // entry so predicted incoming-damage visual state is cleaned up on server confirmation.
    static void onDamageToMe(u32 projectileSrcKey, f32 damage);
    // Server-pushed mid-run floor descent (SV_LEVEL_SEED). Client-only: adopt the
    // host's new floor/difficulty/seed and follow into the same FLOOR_TRANSITION path.
    static void onLevelSeed(u8 floor, u8 difficulty, u32 seed);
};

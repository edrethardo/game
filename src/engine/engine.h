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
#include "game/limb_system.h"
#include "game/weapon.h"
#include "game/projectile.h"
#include "game/item.h"
#include "net/net.h"
#include "net/net_player.h"
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

    // Game state
    GameState m_gameState = GameState::MENU;

    // Difficulty — 0=Normal, 1=Nightmare (2x HP/1.5x dmg), 2=Hell (3x HP/2x dmg)
    u8 m_difficulty        = 0;
    // Highest difficulty unlocked globally (persisted in difficulty_unlock.dat)
    u8 m_highestUnlocked   = 0;

    // Menu/UI state — grouped for isolation
    struct MenuState {
        u8   selection = 0;
        u8   subState = 0;       // 0=main, 1=singleplayer, 2=class selection, 3=options
        u8   subSelection = 0;
        f32  msgTimer = 0.0f;    // countdown for transient menu messages
        const char* msg = nullptr;
        bool bindCapture = false;   // true when waiting for key/button press to rebind
        bool bindKeyboard = true;   // true=capturing keyboard, false=capturing controller
        bool confirmQuit = false;   // "are you sure?" overlay when pressing ESC in-game
        char connectAddress[64] = "127.0.0.1";
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

    // --- Split-screen state ---
    static constexpr u32 MAX_LOCAL_PLAYERS = 2;
    u8   m_splitPlayerCount = 1;   // 1=single, 2=split-screen
    u8   m_activePlayerIndex = 0;  // which player is currently being updated
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
    u32        m_serverTick = 0;

    // Players (networked)
    NetPlayer  m_players[MAX_PLAYERS];
    WeaponDef  m_weaponDefs[MAX_WEAPON_DEFS];
    u32        m_weaponDefCount = 0;

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
    Mesh    m_cubeMesh;
    Mesh    m_quadMesh;   // flat quad for billboard sprites

    // Mesh registry for entities (MeshDef struct defined in limb_system.h)
    static constexpr u32 MAX_MESH_DEFS = 64;
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
    };
    LevelState m_level;

    // Entities + projectiles (authoritative on server/singleplayer)
    EntityPool     m_entities;
    ProjectilePool m_projectiles;

    // Per-room point lights (placed at level gen, nearest 4 sent to shader per frame)
    static constexpr u32 MAX_POINT_LIGHTS = 64;
    struct PointLight { Vec3 position; Vec3 color; };
    PointLight m_pointLights[MAX_POINT_LIGHTS];
    u32 m_pointLightCount = 0;

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
        f32            playerPitches[MAX_PLAYERS];
        bool           playerActive[MAX_PLAYERS];
        f32            playerHealth[MAX_PLAYERS];
        f32            playerMaxHealth[MAX_PLAYERS];
        u8             playerAnimFlags[MAX_PLAYERS]; // bit0=attacking, bit1=reloading, bit2=dead
    };
    RenderInterp m_renderInterp;

    // Combat feedback
    CombatHit   m_lastCombatHit;

    // Inventory UI state
    InventoryDragState m_dragState;
    DoubleClickState   m_dblClickState;
    u8  m_invCursorPanel = 0;  // active alias (swapped per player)
    u8  m_invCursorIndex = 0;  // active alias
    u8  m_invCursorPanels[MAX_LOCAL_PLAYERS] = {};
    u8  m_invCursorIndices[MAX_LOCAL_PLAYERS] = {};
    f32 m_fullBackpackNotifyTimer = 0.0f;
    bool m_firstPickupTooltipShown = false;
    f32  m_firstPickupTooltipTimer = 0.0f;
    bool m_equipTooltipShown = false;  // "double-click to equip" shown once
    f32  m_equipTooltipTimer = 0.0f;
    f32  m_controlsTooltipTimer = 0.0f;  // LMB/RMB controls shown on floor 1 entry

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

    // Core update paths
    void update(f32 dt);
    void gameUpdate(f32 dt);        // unified gameplay — all roles call this
    void serverNetPre(f32 dt);      // server: process remote inputs before gameplay
    void serverNetPost(f32 dt);     // server: status ticks + snapshot broadcast
    void clientNetPre(f32 dt);      // client: predict + reconcile before gameplay
    void clientNetPost(f32 dt);     // client: interpolate remote state after gameplay

    // Shared logic
    void handleWeaponFireForPlayer(NetPlayer& np, f32 dt);
    void updateTargetLockForPlayer(NetPlayer& np, f32 dt);
    void handleWeaponFire(f32 dt); // singleplayer legacy
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

    // Menu/lobby
    void updateMenu(f32 dt);
    void updateLobby(f32 dt);
    void startGame();
    void saveGame(u8 slot);
    bool loadGame(u8 slot);

    // Save slot management
    static constexpr u32 MAX_SAVE_SLOTS = 20;
    u8 m_activeSaveSlot = 0;  // 0 = no active slot, 1-20 = slot number

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

    // Net callbacks (static, forwarded to engine instance)
    static void onSnapshot(const u8* data, u32 size);
    static void onInput(u8 playerSlot, const u8* data, u32 size);
    static void onEvent(const u8* data, u32 size);
    static void onPlayerJoin(u8 playerSlot);
    static void onPlayerLeft(u8 playerSlot);
};

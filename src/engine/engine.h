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
    f32 m_fadeFromBlack = 0.0f;  // overlay alpha timer — hides stale frames after level load
    f32 m_lowHpRumbleTimer = 0.0f;  // countdown between low-HP "heartbeat" rumble nags

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
    bool m_firstPickupTooltipShown = false;  // item picked up → show "Open Inventory"
    bool m_inventoryOpenedOnce    = false;  // dismiss "Open Inventory" once opened
    bool m_equipTooltipShown = false;       // inventory opened → show "equip" hint
    bool m_itemEquippedOnce  = false;       // dismiss equip hint once an item is equipped
    bool m_shieldBlockedOnce = false;       // dismiss shield tutorial once player blocks
    bool m_dodgeRolledOnce   = false;       // dismiss dodge tutorial once player dodges
    f32  m_controlsTooltipTimer = 0.0f;     // LMB/RMB controls shown on floor 1 entry
    f32  m_tutorialPulseTimer   = 0.0f;     // shared pulse timer for tutorial tooltips

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

    // gameUpdate helpers — extracted from the god function for readability.
    // Each is called exactly once, in the same order they appear in gameUpdate.
    void tickWandererTimers(f32 dt);             // adrenaline stacks, deflect burst, mark, Death's Dance, floor unlock
    void tickPlayerStatusEffects(f32 dt);        // poison / burn / freeze DoT ticks
    void handleDebugKeys();                      // F1-F6 debug toggles and stress spawners
    void tickFXDecay(f32 dt);                    // impact/fire/nova/dash/beam/chain/light timers + scorch AoE + herald aura
    void tickSkillCooldowns(f32 dt);             // SkillSystem::update + class/boot/helmet cooldown ticks
    void tickPassiveEquipment();                 // bind weapon-proc / armor-aura / ring-passive SkillIds each tick
    void handleClassSkillActivation(f32 dt, Vec3 eyePos);     // keys 1-4 selection + right-click cast
    void handleEquipmentSkillActivation(f32 dt, Vec3 eyePos); // boots F / helmet G casts
    void tickArmorRingPassives(f32 dt);          // ring timers, Second Wind, Divine Judgment, per-entity armor/ring pass
    void tickVisualFeedback(f32 dt);             // damage flash + hurt vignette + low-HP rumble
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
    void renderPostOverlays(u32 sw, u32 sh);          // fade-from-black + hurt vignette fullscreen quads
    // Draws a fullscreen quad with the given RGBA using the unlit shader + ortho projection
    void drawFullscreenQuad(u32 sw, u32 sh, Vec4 rgba);

    // Menu/lobby
    void updateMenu(f32 dt);
    void updateLobby(f32 dt);
    void startGame(GameStart mode);
    // Equip the class starting weapon for one local player (centralizes what used
    // to be copy-pasted across the menu start paths). Called only on NEW_GAME.
    void equipStartingLoadout(u8 playerIdx);
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

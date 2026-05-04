#pragma once

#include "core/types.h"
#include "renderer/camera.h"
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

static constexpr u32 MAX_LEVEL_SECTIONS = 64;

enum struct GameState : u8 {
    MENU,
    LOBBY_HOST,
    LOBBY_JOIN,
    CONNECTING,
    IN_GAME,
    GAME_OVER,  // player died, show death screen
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
    u32  m_updateCount = 0;
    u32  m_frameCount  = 0;

    // Game state
    GameState m_gameState = GameState::MENU;
    u8        m_menuSelection = 0;
    u8        m_menuSubState = 0;  // 0=main, 1=singleplayer, 2=class selection
    u8        m_menuSubSelection = 0;
    bool      m_confirmQuit = false;  // "are you sure?" overlay when pressing ESC in-game
    char      m_connectAddress[64] = "127.0.0.1";

    // Player class system
    PlayerClass m_playerClass = PlayerClass::WARRIOR;
    u8          m_activeClassSkill = 0;  // which of 4 class skills is selected (0-3)
    SkillState  m_classSkillStates[4];   // per-slot cooldown tracking for class skills

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
    SkillState      m_skillStates[MAX_PLAYERS];   // ring (right-click)
    SkillState      m_bootSkillStates[MAX_PLAYERS];  // boots (F key)
    SkillState      m_helmetSkillStates[MAX_PLAYERS]; // helmet (G key)
    SkillId         m_armorAura = SkillId::NONE;      // passive armor legendary
    SkillId         m_weaponProc = SkillId::NONE;     // weapon on-hit proc
    WorldItemPool   m_worldItems;
    bool       m_inventoryOpen = false;
    QuickbarState   m_quickbars[MAX_PLAYERS];
    ViewmodelState  m_viewmodelState;  // first-person hand/weapon animation
    Mesh            m_handMesh;        // procedural hand geometry

    // Legacy single-player compat
    Player     m_localPlayer;  // used for singleplayer mode camera/movement

    // Rendering
    Camera  m_camera;
    Shader  m_basicShader;
    Shader  m_unlitShader;
    Mesh    m_cubeMesh;

    // Mesh registry for entities (MeshDef struct defined in limb_system.h)
    static constexpr u32 MAX_MESH_DEFS = 64;
    MeshDef  m_meshDefs[MAX_MESH_DEFS] = {};
    u32      m_meshDefCount = 0;

    // Level
    LevelGrid    m_grid;
    LevelSection m_sections[MAX_LEVEL_SECTIONS];
    u32          m_sectionCount = 0;
    u32          m_levelSeed    = 42;
    u32          m_currentFloor = 1;  // current dungeon floor (increases each descent)
    u32          m_savedFloor   = 1;  // last saved floor for death respawn
    u32          m_savedSeed    = 0;  // saved RNG seed for that floor

    // Entities + projectiles (authoritative on server/singleplayer)
    EntityPool     m_entities;
    ProjectilePool m_projectiles;

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

    // Render copies for client interpolation
    EntityPool     m_renderEntities;
    ProjectilePool m_renderProjectiles;
    Vec3           m_renderPlayerPositions[MAX_PLAYERS];
    f32            m_renderPlayerYaws[MAX_PLAYERS];
    f32            m_renderPlayerPitches[MAX_PLAYERS];
    bool           m_renderPlayerActive[MAX_PLAYERS];
    f32            m_renderPlayerHealth[MAX_PLAYERS];
    f32            m_renderPlayerMaxHealth[MAX_PLAYERS];

    // Combat feedback
    CombatHit   m_lastCombatHit;
    f32         m_hitMarkerTimer = 0.0f;
    f32         m_potionCooldown = 0.0f;  // healing potion cooldown (15s)

    // Inventory UI state
    InventoryDragState m_dragState;
    DoubleClickState   m_dblClickState;
    f32 m_fullBackpackNotifyTimer = 0.0f;

    // Floor door — portal to next dungeon level
    Vec3 m_floorDoorPos    = {0, 0, 0};
    bool m_floorDoorActive = false;

    // Chat log — displays NPC speech and game events on the left side of the screen
    static constexpr u32 MAX_CHAT_LINES = 8;
    static constexpr u32 CHAT_LINE_LEN = 48;
    struct ChatLine { char text[CHAT_LINE_LEN]; Vec3 color; f32 timer; };
    ChatLine m_chatLog[MAX_CHAT_LINES] = {};
    void addChatMessage(const char* speaker, const char* msg, Vec3 color);

    // AoE fire effect (cheap visual for molotov splash)
    static constexpr u32 MAX_FIRE_FX = 8;
    struct FireFX { Vec3 pos; f32 radius; f32 timer; bool active; };

    // Blood Nova / skill ring burst effect
    static constexpr u32 MAX_NOVA_FX = 4;
    struct NovaFX { Vec3 pos; f32 maxRadius; f32 timer; bool active; Vec3 color; };
    NovaFX m_novaFX[MAX_NOVA_FX] = {};

    // Phase Dash trail effect
    static constexpr u32 MAX_DASH_FX = 4;
    struct DashFX { Vec3 start; Vec3 end; f32 timer; bool active; };
    DashFX m_dashFX[MAX_DASH_FX] = {};

    // Meteor scorch zones — persistent ground fire that deals AoE DoT
    static constexpr u32 MAX_SCORCH = 4;
    struct ScorchZone { Vec3 pos; f32 radius; f32 timer; f32 dps; bool active; };
    ScorchZone m_scorchZones[MAX_SCORCH] = {};
    FireFX m_fireFX[MAX_FIRE_FX] = {};

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

    // Switch constraint mode
    bool m_switchMode = false;
    static constexpr f32 SWITCH_FAR_PLANE     = 60.0f;
    static constexpr u32 SWITCH_MAX_ENTITIES  = 64;
    static constexpr u32 SWITCH_RES_W         = 960;
    static constexpr u32 SWITCH_RES_H         = 540;

    // Core update paths
    void update(f32 dt);
    void serverUpdate(f32 dt);
    void clientUpdate(f32 dt);
    void singleplayerUpdate(f32 dt);

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
    void saveGame();
    bool loadGame();

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

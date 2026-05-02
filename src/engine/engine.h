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
    char      m_connectAddress[64] = "127.0.0.1";

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

    // Mesh registry for entities
    static constexpr u32 MAX_MESH_DEFS = 32;
    struct MeshDef { Mesh mesh; AABB bounds; char name[32]; };
    MeshDef  m_meshDefs[MAX_MESH_DEFS] = {};
    u32      m_meshDefCount = 0;

    // Level
    LevelGrid    m_grid;
    LevelSection m_sections[MAX_LEVEL_SECTIONS];
    u32          m_sectionCount = 0;
    u32          m_levelSeed    = 42;

    // Entities + projectiles (authoritative on server/singleplayer)
    EntityPool     m_entities;
    ProjectilePool m_projectiles;

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
    void logStats();

    // Menu/lobby
    void updateMenu(f32 dt);
    void updateLobby(f32 dt);
    void startGame();

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
